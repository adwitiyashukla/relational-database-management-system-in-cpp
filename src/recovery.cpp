#include <algorithm>
#include <queue>

#include "engine.h"

namespace rdbms {

void Engine::Recover() {
  uint64_t t0 = NowMicros();
  recovery_.torn_pages = disk_->RepairTornPages();
  Lsn ck = 0;
  log_->ReadControl(&ck);
  LogRecord cr;
  Lsn pos = 0;
  if (!log_->Read(ck, &cr, &pos) || cr.type != LogType::Checkpoint)
    Fail(state::kCorrupt, "checkpoint record at " + std::to_string(ck) + " is missing or damaged");
  recovery_.checkpoint = ck;
  std::map<Xid, TxnEntry> att;
  for (auto& e : cr.att) att[e.xid] = e;
  std::map<PageId, Lsn> dpt;
  for (auto& [p, l] : cr.dpt) dpt.emplace(p, l);
  Xid next_xid = std::max<Xid>(cr.next_xid, 1);
  LogRecord r;
  Lsn next = 0;
  while (log_->Read(pos, &r, &next)) {
    recovery_.records++;
    if (r.xid != 0) {
      next_xid = std::max(next_xid, r.xid + 1);
      if (r.type == LogType::End) {
        att.erase(r.xid);
      } else {
        TxnEntry& e = att[r.xid];
        e.xid = r.xid;
        if (e.first == 0) e.first = r.lsn;
        e.last = r.lsn;
        if (r.type == LogType::Commit) e.status = TxnStatus::Committed;
        if (r.type == LogType::Abort) e.status = TxnStatus::Aborting;
      }
    }
    for (PageId p : r.Pages()) dpt.emplace(p, r.lsn);
    pos = next;
  }
  Lsn end = pos;
  recovery_.end = end;
  Lsn start = ck;
  for (auto& [p, l] : dpt) start = std::min(start, l);
  recovery_.redo_start = start;
  pos = start;
  while (pos < end) {
    if (!log_->Read(pos, &r, &next))
      Fail(state::kCorrupt, "redo could not read the log record at " + std::to_string(pos));
    Redo(r, dpt);
    pos = next;
  }
  log_->Reopen(end);
  txns_ = std::make_unique<TxnManager>(next_xid);
  UndoLosers(att);
  pool_->FlushAll();
  Checkpoint();
  recovery_.millis = static_cast<double>(NowMicros() - t0) / 1000.0;
}

void Engine::Redo(const LogRecord& r, const std::map<PageId, Lsn>& dpt) {
  auto wanted = [&](PageId p) {
    auto it = dpt.find(p);
    return it != dpt.end() && r.lsn >= it->second;
  };
  switch (r.type) {
    case LogType::Row:
    case LogType::Clr:
    case LogType::Purge: {
      if (!wanted(r.page)) return;
      PageRef ref = pool_->Fetch(r.page);
      ref.LockX();
      Page p = ref.page();
      if (p.lsn() >= r.lsn) return;
      if (p.type() != PageType::Leaf)
        Fail(state::kCorrupt, "redo found page " + std::to_string(r.page) + " is not a leaf");
      if (r.type != LogType::Purge && r.has_after) {
        std::string rec = MakeLeaf(r.key, r.after.xid, r.after.flags, r.after.value);
        ApplyLeaf(p, r.key, &rec);
      } else {
        ApplyLeaf(p, r.key, nullptr);
      }
      p.set_lsn(r.lsn);
      pool_->MarkDirty(ref.frame(), r.lsn);
      recovery_.redone++;
      break;
    }
    case LogType::Images:
      for (auto& [pid, img] : r.images) {
        if (!wanted(pid)) continue;
        PageRef ref = pool_->Fetch(pid);
        ref.LockX();
        if (ref.page().lsn() >= r.lsn) continue;
        Page::Restore(ref.data(), img);
        ref.page().set_lsn(r.lsn);
        pool_->MarkDirty(ref.frame(), r.lsn);
        recovery_.redone++;
      }
      break;
    default:
      break;
  }
}

void Engine::UndoLosers(std::map<Xid, TxnEntry>& att) {
  std::map<Xid, Lsn> last;
  std::priority_queue<std::pair<Lsn, Xid>> work;
  for (auto& [x, e] : att) {
    if (e.status == TxnStatus::Committed) {
      LogRecord end;
      end.type = LogType::End;
      end.xid = x;
      end.prev = e.last;
      log_->Append(end);
      continue;
    }
    recovery_.losers++;
    Lsn tail = e.last;
    if (e.status == TxnStatus::Active) {
      LogRecord ab;
      ab.type = LogType::Abort;
      ab.xid = x;
      ab.prev = tail;
      tail = log_->Append(ab);
    }
    last[x] = tail;
    work.push({e.last, x});
  }
  while (!work.empty()) {
    auto [lsn, x] = work.top();
    work.pop();
    LogRecord r;
    if (!log_->Read(lsn, &r, nullptr))
      Fail(state::kCorrupt, "undo could not read log record " + std::to_string(lsn));
    Lsn next = r.prev;
    if (r.type == LogType::Clr) {
      next = r.undo_next;
    } else if (r.type == LogType::Row) {
      BTree tree(*store_, r.tree, r.tree);
      tree.Modify(
          r.key,
          [&](const LeafRec* cur) -> std::optional<LeafChange> {
            if (!cur) return std::nullopt;
            LeafChange ch;
            if (r.has_before) {
              ch.xid = r.before.xid;
              ch.flags = r.before.flags;
              ch.value = r.before.value;
            } else {
              ch.remove = true;
            }
            return ch;
          },
          [&](PageRef& leaf, const LeafRec*, const LeafChange& ch) {
            LogRecord clr;
            clr.type = LogType::Clr;
            clr.xid = x;
            clr.prev = last[x];
            clr.tree = r.tree;
            clr.page = leaf.id();
            clr.key = r.key;
            clr.has_after = !ch.remove;
            if (!ch.remove) clr.after = r.before;
            clr.undo_next = r.prev;
            last[x] = log_->Append(clr, [&](Lsn l) {
              leaf.page().set_lsn(l);
              pool_->MarkDirty(leaf.frame(), l);
            });
          });
      recovery_.undone++;
    }
    if (next == 0) {
      LogRecord end;
      end.type = LogType::End;
      end.xid = x;
      end.prev = last[x];
      log_->Append(end);
    } else {
      work.push({next, x});
    }
  }
}

}
