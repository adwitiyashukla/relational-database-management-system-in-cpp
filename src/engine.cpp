#include "engine.h"

#include <algorithm>
#include <chrono>

namespace rdbms {

namespace {

constexpr const char* kUpdateConflict = "could not serialize access due to concurrent update";

}

Engine::Engine(FileSystem& fs, EngineOptions opt) : fs_(fs), opt_(std::move(opt)) {}

Engine::~Engine() {
  try {
    Close();
  } catch (...) {
  }
  std::lock_guard<std::mutex> g(live_mu_);
  for (Txn* t : live_) delete t;
  live_.clear();
}

void Engine::Open() {
  fs_.MakeDirs(opt_.dir);
  disk_ = std::make_unique<DiskManager>(fs_, opt_.dir);
  log_ = std::make_unique<LogManager>(fs_, opt_.dir, opt_.wal);
  pool_ = std::make_unique<BufferPool>(*disk_, *log_, opt_.pool_pages);
  store_ = std::make_unique<Store>(Store{*pool_, *log_});
  locks_ = std::make_unique<LockManager>(opt_.lock_timeout_ms);
  Lsn ck = 0;
  if (!log_->ReadControl(&ck)) {
    CreateFresh();
  } else {
    if (!fs_.Exists(opt_.dir + "/data.db"))
      Fail(state::kCorrupt, "control file exists but data.db is missing");
    disk_->Open();
    Recover();
  }
  LoadCatalog();
  opened_ = true;
  last_ckpt_time_ = NowMicros();
  if (opt_.background) {
    writer_ = std::thread(&Engine::WriterLoop, this);
    gc_ = std::thread(&Engine::GcLoop, this);
  }
}

void Engine::CreateFresh() {
  for (const char* name : {"data.db", "doublewrite", "control"}) {
    std::string p = opt_.dir + "/" + name;
    if (fs_.Exists(p)) fs_.Remove(p);
  }
  for (auto& name : fs_.List(opt_.dir + "/wal")) fs_.Remove(opt_.dir + "/wal/" + name);
  char meta[kPageSize];
  Page(meta).FormatMeta();
  char cat[kPageSize];
  Page(cat).Format(kCatalogRoot, PageType::Leaf, 0);
  disk_->Create(meta, cat);
  log_->Create();
  txns_ = std::make_unique<TxnManager>(1);
  Lsn ck = log_->AppendCheckpoint([](LogRecord& r) { r.next_xid = 1; });
  log_->Flush(ck);
  log_->WriteControl(ck);
  last_ckpt_ = ck;
  recovery_.fresh = true;
}

void Engine::Close() {
  if (!opened_ || closed_) return;
  closed_ = true;
  {
    std::lock_guard<std::mutex> g(bg_mu_);
    stop_ = true;
  }
  bg_cv_.notify_all();
  if (writer_.joinable()) writer_.join();
  if (gc_.joinable()) gc_.join();
  try {
    RunGc();
    pool_->FlushAll();
    Checkpoint();
  } catch (const std::exception& e) {
    LogInfo("shutdown checkpoint failed: %s", e.what());
  }
}

void Engine::Halt() {
  if (!opened_ || closed_) return;
  closed_ = true;
  {
    std::lock_guard<std::mutex> g(bg_mu_);
    stop_ = true;
  }
  bg_cv_.notify_all();
  if (writer_.joinable()) writer_.join();
  if (gc_.joinable()) gc_.join();
}

Txn* Engine::Begin(Isolation iso) {
  Txn* t = new Txn();
  txns_->Register(t, iso);
  std::lock_guard<std::mutex> g(live_mu_);
  live_.insert(t);
  return t;
}

void Engine::StartStatement(Txn& t) {
  if (t.doomed.load())
    Fail(state::kSerialization,
         "could not serialize access due to read/write dependencies among transactions");
  if (t.iso == Isolation::ReadCommitted || t.snapshot.load() == 0) {
    if (t.ser() && !t.ssi_registered) {
      ssi_.Begin(t, [&] { txns_->TakeSnapshot(t); });
    } else {
      txns_->TakeSnapshot(t);
    }
  }
}

void Engine::Release(Txn* t) {
  std::lock_guard<std::mutex> g(live_mu_);
  live_.erase(t);
  delete t;
}

void Engine::Commit(Txn* t) {
  try {
    if (t->wrote()) {
      LogRecord rec;
      rec.type = LogType::Commit;
      rec.xid = t->xid;
      rec.prev = t->last_lsn;
      Lsn lsn = 0;
      auto append = [&] {
        lsn = log_->Append(rec, [&](Lsn l) {
          t->last_lsn = l;
          t->wal_status = TxnStatus::Committed;
          txns_->RegisterCommit(t, l);
        });
      };
      if (t->ser()) {
        ssi_.PreCommit(*t, append);
      } else {
        append();
      }
      log_->Flush(lsn);
      txns_->Publish(log_->flushed_lsn());
      if (opt_.background && lsn - last_ckpt_.load() >= opt_.checkpoint_wal_bytes &&
          !kicked_.exchange(true)) {
        std::lock_guard<std::mutex> g(bg_mu_);
        kick_ = true;
        bg_cv_.notify_all();
      }
      if (t->ssi_registered) ssi_.Committed(*t, false, t->commit_ts);
    } else {
      if (t->ssi_registered) ssi_.PreCommit(*t, [] {});
      Ts point = txns_->clock();
      txns_->Finish(*t);
      if (t->ssi_registered) ssi_.Committed(*t, true, point);
    }
  } catch (const DbError&) {
    if (t->wal_status != TxnStatus::Committed) Abort(t);
    throw;
  }
  locks_->ReleaseAll(*t);
  if (t->wrote()) {
    LogRecord end;
    end.type = LogType::End;
    end.xid = t->xid;
    end.prev = t->last_lsn;
    log_->Append(end, [&](Lsn l) {
      t->last_lsn = l;
      t->ended = true;
    });
  }
  if (!t->deletes.empty()) {
    std::lock_guard<std::mutex> g(purge_mu_);
    for (auto& [tree, key] : t->deletes) purge_.push_back({tree, key, t->xid});
  }
  commits_++;
  Release(t);
}

void Engine::RollbackEntry(Txn& t, const UndoEntry& e, Lsn undo_next) {
  BTree tree(*store_, e.tree, e.tree);
  tree.Modify(
      e.key,
      [&](const LeafRec* cur) -> std::optional<LeafChange> {
        if (!cur) Fail(state::kInternal, "rollback could not find a row it wrote");
        LeafChange ch;
        if (e.has_before) {
          ch.xid = e.before.xid;
          ch.flags = e.before.flags;
          ch.value = e.before.value;
        } else {
          ch.remove = true;
        }
        return ch;
      },
      [&](PageRef& leaf, const LeafRec*, const LeafChange& ch) {
        LogRecord clr;
        clr.type = LogType::Clr;
        clr.xid = t.xid;
        clr.prev = t.last_lsn;
        clr.tree = e.tree;
        clr.page = leaf.id();
        clr.key = e.key;
        clr.has_after = !ch.remove;
        if (!ch.remove) clr.after = e.before;
        clr.undo_next = undo_next;
        log_->Append(clr, [&](Lsn l) {
          leaf.page().set_lsn(l);
          pool_->MarkDirty(leaf.frame(), l);
          t.last_lsn = l;
        });
        if (e.pushed) versions_.Pop(e.tree, e.key);
      });
}

void Engine::Abort(Txn* t) {
  if (t->wrote() && !t->ended) {
    LogRecord ab;
    ab.type = LogType::Abort;
    ab.xid = t->xid;
    ab.prev = t->last_lsn;
    log_->Append(ab, [&](Lsn l) {
      t->last_lsn = l;
      t->wal_status = TxnStatus::Aborting;
    });
    for (size_t i = t->undo.size(); i-- > 0;)
      RollbackEntry(*t, t->undo[i], i > 0 ? t->undo[i - 1].lsn : 0);
    LogRecord end;
    end.type = LogType::End;
    end.xid = t->xid;
    end.prev = t->last_lsn;
    log_->Append(end, [&](Lsn l) {
      t->last_lsn = l;
      t->ended = true;
    });
  }
  txns_->Finish(*t);
  if (t->ssi_registered) ssi_.Aborted(*t);
  locks_->ReleaseAll(*t);
  aborts_++;
  Release(t);
}

bool Engine::Resolve(Txn& t, uint32_t tree, const LeafRec& cur, std::string* value) {
  if (txns_->Visible(cur.xid, t)) {
    if (cur.deleted()) return false;
    value->assign(cur.value.data(), cur.value.size());
    return true;
  }
  if (t.ser()) ssi_.OnSkip(t, cur.xid);
  bool found = false;
  versions_.Find(tree, cur.key, [&](const Version& v) {
    if (txns_->Visible(v.xid, t)) {
      found = (v.flags & kDeleted) == 0;
      if (found) *value = v.value;
      return true;
    }
    if (t.ser()) ssi_.OnSkip(t, v.xid);
    return false;
  });
  return found;
}

bool Engine::Read(Txn& t, BTree& tree, std::string_view key, std::string* value) {
  if (t.ser()) ssi_.OnRead(t, tree.id(), key);
  bool found = false;
  tree.Get(key, [&](const LeafRec* cur) {
    if (cur) found = Resolve(t, tree.id(), *cur, value);
  });
  return found;
}

void Engine::Scan(Txn& t, BTree& tree, const std::string& lo, const std::string* hi, bool hi_incl,
                  const RowFn& fn) {
  if (t.ser()) ssi_.OnScan(t, tree.id(), lo, hi, hi_incl);
  std::string value;
  tree.Scan(lo, hi, hi_incl, [&](const LeafRec& r) {
    if (!Resolve(t, tree.id(), r, &value)) return true;
    return fn(r.key, value);
  });
}

std::string Engine::LockKey(uint32_t tree, std::string_view key) {
  std::string k(4, '\0');
  PutU32(k.data(), tree);
  k.append(key.data(), key.size());
  return k;
}

bool Engine::Write(Txn& t, BTree& tree, std::string_view key, Op op, const Compute& compute) {
  if (t.doomed.load())
    Fail(state::kSerialization,
         "could not serialize access due to read/write dependencies among transactions");
  locks_->Lock(t, LockKey(tree.id(), key));
  bool found = false;
  bool applied = false;
  tree.Modify(
      key,
      [&](const LeafRec* cur) -> std::optional<LeafChange> {
        bool live = cur && !cur->deleted();
        if (cur && cur->xid != t.xid && t.iso != Isolation::ReadCommitted &&
            !txns_->Visible(cur->xid, t))
          Fail(state::kSerialization, kUpdateConflict);
        std::optional<std::string> nv;
        if (op == Op::Insert) {
          if (live) Fail(state::kUnique, "duplicate key value violates unique constraint");
          nv = compute(nullptr);
        } else {
          if (!live) return std::nullopt;
          found = true;
          std::string latest(cur->value);
          nv = compute(&latest);
          if (op == Op::Lock) return std::nullopt;
        }
        if (!nv) return std::nullopt;
        LeafChange ch;
        ch.xid = t.xid;
        ch.flags = op == Op::Delete ? kDeleted : 0;
        if (op != Op::Delete) ch.value = std::move(*nv);
        return ch;
      },
      [&](PageRef& leaf, const LeafRec* before, const LeafChange& ch) {
        LogRecord rec;
        rec.type = LogType::Row;
        rec.xid = t.xid;
        rec.prev = t.last_lsn;
        rec.tree = tree.id();
        rec.page = leaf.id();
        rec.key.assign(key.data(), key.size());
        if (before) {
          rec.has_before = true;
          rec.before.xid = before->xid;
          rec.before.flags = before->flags;
          rec.before.value.assign(before->value.data(), before->value.size());
        }
        rec.has_after = true;
        rec.after.xid = ch.xid;
        rec.after.flags = ch.flags;
        rec.after.value = ch.value;
        Lsn lsn = log_->Append(rec, [&](Lsn l) {
          leaf.page().set_lsn(l);
          pool_->MarkDirty(leaf.frame(), l);
          if (t.first_lsn == 0) t.first_lsn = l;
          t.last_lsn = l;
        });
        bool pushed = false;
        if (before && before->xid != t.xid) {
          Version v;
          v.xid = before->xid;
          v.succ = t.xid;
          v.flags = before->flags;
          v.value = rec.before.value;
          versions_.Push(tree.id(), key, std::move(v));
          pushed = true;
        }
        UndoEntry u;
        u.tree = tree.id();
        u.key = rec.key;
        u.lsn = lsn;
        u.has_before = rec.has_before;
        u.before = std::move(rec.before);
        u.pushed = pushed;
        t.undo.push_back(std::move(u));
        applied = true;
      });
  if (applied) {
    if (t.ser()) ssi_.OnWrite(t, tree.id(), key);
    if (op == Op::Delete) t.deletes.emplace_back(tree.id(), std::string(key));
  }
  return op == Op::Insert ? applied : found;
}

std::shared_ptr<Table> Engine::FindTable(const std::string& name) {
  std::shared_lock<SharedMutex> l(catalog_mu_);
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Table>> Engine::Tables() {
  std::shared_lock<SharedMutex> l(catalog_mu_);
  std::vector<std::shared_ptr<Table>> v;
  for (auto& [n, t] : tables_) v.push_back(t);
  return v;
}

void Engine::CreateTable(Schema schema, bool if_not_exists) {
  std::lock_guard<std::mutex> ddl(ddl_mu_);
  if (FindTable(schema.name)) {
    if (if_not_exists) return;
    Fail(state::kDupTable, "relation \"" + schema.name + "\" already exists");
  }
  Txn* t = Begin(Isolation::ReadCommitted);
  try {
    StartStatement(*t);
    schema.root = BTree::Create(*store_);
    BTree cat(*store_, kCatalogTree, kCatalogRoot);
    std::string key = EncodeKey({Value::Text(schema.name)});
    std::string enc = EncodeSchema(schema);
    Write(*t, cat, key, Op::Insert,
          [&](const std::string*) { return std::optional<std::string>(enc); });
  } catch (...) {
    Abort(t);
    throw;
  }
  Commit(t);
  auto tbl = std::make_shared<Table>();
  tbl->schema = schema;
  tbl->tree = std::make_unique<BTree>(*store_, schema.root, schema.root);
  std::unique_lock<SharedMutex> l(catalog_mu_);
  tables_[schema.name] = tbl;
}

bool Engine::DropTable(const std::string& name, bool if_exists) {
  std::lock_guard<std::mutex> ddl(ddl_mu_);
  if (!FindTable(name)) {
    if (if_exists) return false;
    Fail(state::kNoTable, "table \"" + name + "\" does not exist");
  }
  Txn* t = Begin(Isolation::ReadCommitted);
  try {
    StartStatement(*t);
    BTree cat(*store_, kCatalogTree, kCatalogRoot);
    Write(*t, cat, EncodeKey({Value::Text(name)}), Op::Delete,
          [](const std::string*) { return std::optional<std::string>(std::string()); });
  } catch (...) {
    Abort(t);
    throw;
  }
  Commit(t);
  std::unique_lock<SharedMutex> l(catalog_mu_);
  tables_.erase(name);
  return true;
}

void Engine::LoadCatalog() {
  std::vector<Schema> schemas;
  Txn* t = Begin(Isolation::ReadCommitted);
  try {
    StartStatement(*t);
    BTree cat(*store_, kCatalogTree, kCatalogRoot);
    Scan(*t, cat, std::string(), nullptr, false, [&](std::string_view, std::string_view v) {
      schemas.push_back(DecodeSchema(v));
      return true;
    });
  } catch (...) {
    Abort(t);
    throw;
  }
  Commit(t);
  std::unique_lock<SharedMutex> l(catalog_mu_);
  tables_.clear();
  for (auto& s : schemas) {
    auto tbl = std::make_shared<Table>();
    tbl->schema = s;
    tbl->tree = std::make_unique<BTree>(*store_, s.root, s.root);
    if (s.hidden_rowid) {
      std::string last = tbl->tree->LastKey();
      if (last.empty()) {
        tbl->tree->Scan(std::string(), nullptr, false, [&](const LeafRec& r) {
          last.assign(r.key.data(), r.key.size());
          return true;
        });
      }
      if (last.size() >= 8) tbl->next_rowid.store(DecodeKeyInt(last) + 1);
    }
    tables_[s.name] = tbl;
  }
}

void Engine::Checkpoint() {
  std::lock_guard<std::mutex> g(ckpt_mu_);
  Lsn keep = ~0ull;
  Lsn ck = log_->AppendCheckpoint([&](LogRecord& r) {
    r.next_xid = txns_->next_xid();
    r.att = txns_->ActiveForCheckpoint();
    pool_->DirtyPages(&r.dpt);
    for (auto& [p, l] : r.dpt) keep = std::min(keep, l);
    for (auto& e : r.att) keep = std::min(keep, e.first);
  });
  log_->Flush(ck);
  log_->WriteControl(ck);
  log_->TruncateBefore(std::min(keep, ck));
  last_ckpt_ = ck;
  last_ckpt_time_ = NowMicros();
  checkpoints_++;
}

void Engine::FuzzyCheckpoint() {
  pool_->FlushSome(pool_->capacity(), last_ckpt_.load());
  Checkpoint();
}

void Engine::FlushPages(size_t n) { pool_->FlushSome(n); }

void Engine::RunGc() {
  txns_->Gc();
  versions_.Gc([&](Xid x) { return txns_->VisibleToAll(x); });
  ssi_.Gc();
  Purge();
}

void Engine::Purge() {
  std::vector<PurgeItem> items;
  {
    std::lock_guard<std::mutex> g(purge_mu_);
    items.swap(purge_);
  }
  if (items.empty()) return;
  std::vector<PurgeItem> later;
  for (size_t i = 0; i < items.size(); i++) {
    PurgeItem& it = items[i];
    if (!txns_->VisibleToAll(it.xid)) {
      later.push_back(std::move(it));
      continue;
    }
    std::string lk = LockKey(it.tree, it.key);
    if (!locks_->TryLock(kPurgeXid, lk)) {
      later.push_back(std::move(it));
      continue;
    }
    try {
      BTree tree(*store_, it.tree, it.tree);
      tree.Modify(
          it.key,
          [&](const LeafRec* cur) -> std::optional<LeafChange> {
            if (cur && cur->deleted() && cur->xid == it.xid) {
              LeafChange c;
              c.remove = true;
              return c;
            }
            return std::nullopt;
          },
          [&](PageRef& leaf, const LeafRec*, const LeafChange&) {
            LogRecord r;
            r.type = LogType::Purge;
            r.tree = it.tree;
            r.page = leaf.id();
            r.key = it.key;
            log_->Append(r, [&](Lsn l) {
              leaf.page().set_lsn(l);
              pool_->MarkDirty(leaf.frame(), l);
            });
            versions_.Erase(it.tree, it.key);
            purged_++;
          });
    } catch (...) {
      locks_->Unlock(kPurgeXid, lk);
      std::lock_guard<std::mutex> g(purge_mu_);
      for (size_t j = i + 1; j < items.size(); j++) purge_.push_back(std::move(items[j]));
      for (auto& l : later) purge_.push_back(std::move(l));
      throw;
    }
    locks_->Unlock(kPurgeXid, lk);
  }
  if (!later.empty()) {
    std::lock_guard<std::mutex> g(purge_mu_);
    for (auto& l : later) purge_.push_back(std::move(l));
  }
}

size_t Engine::Vacuum() {
  txns_->Gc();
  std::vector<PurgeItem> found;
  std::vector<uint32_t> trees = {kCatalogTree};
  for (auto& t : Tables()) trees.push_back(t->tree->id());
  for (uint32_t id : trees) {
    BTree tree(*store_, id, id);
    tree.Scan(std::string(), nullptr, false, [&](const LeafRec& r) {
      if (r.deleted() && txns_->VisibleToAll(r.xid))
        found.push_back({id, std::string(r.key), r.xid});
      return true;
    });
  }
  {
    std::lock_guard<std::mutex> g(purge_mu_);
    for (auto& f : found) purge_.push_back(f);
  }
  uint64_t before = purged_.load();
  RunGc();
  return static_cast<size_t>(purged_.load() - before);
}

void Engine::WriterLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> l(bg_mu_);
      bg_cv_.wait_for(l, std::chrono::milliseconds(100), [&] { return stop_ || kick_; });
      if (stop_) return;
      kick_ = false;
    }
    try {
      if (pool_->dirty_count() > pool_->capacity() / 4) pool_->FlushSome(64);
      uint64_t since = log_->next_lsn() - last_ckpt_;
      bool due = NowMicros() - last_ckpt_time_ >= opt_.checkpoint_interval_ms * 1000ull;
      if (due || since >= opt_.checkpoint_wal_bytes) {
        FuzzyCheckpoint();
        kicked_.store(false);
      }
    } catch (const std::exception& e) {
      LogInfo("background writer: %s", e.what());
    }
  }
}

void Engine::GcLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> l(bg_mu_);
      bg_cv_.wait_for(l, std::chrono::milliseconds(opt_.gc_interval_ms), [&] { return stop_; });
      if (stop_) return;
    }
    try {
      RunGc();
    } catch (const std::exception& e) {
      LogInfo("garbage collector: %s", e.what());
    }
  }
}

Engine::Stats Engine::stats() {
  Stats s;
  s.commits = commits_.load();
  s.aborts = aborts_.load();
  s.serialization_failures = ssi_.failures();
  s.deadlocks = locks_->deadlocks();
  s.lock_waits = locks_->waits();
  s.versions = versions_.size();
  s.purged = purged_.load();
  s.checkpoints = checkpoints_.load();
  s.wal_bytes = log_->bytes_written();
  s.wal_flushes = log_->flush_count();
  s.pages_written = disk_->pages_written();
  s.dirty_pages = pool_->dirty_count();
  s.active_txns = txns_->active_count();
  s.ssi_nodes = ssi_.nodes();
  return s;
}

}
