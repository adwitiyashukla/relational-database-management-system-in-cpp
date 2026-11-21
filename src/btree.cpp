#include "btree.h"

#include <algorithm>
#include <deque>

namespace rdbms {

namespace {

PageId Allocate(PageRef& meta) {
  Page m = meta.page();
  PageId id = m.meta_next_page();
  if (id >= kNoPage - 1) Fail(state::kLimit, "database file is full");
  m.set_meta_next_page(id + 1);
  return id;
}

std::vector<std::string> Records(Page p) {
  std::vector<std::string> v;
  v.reserve(p.count());
  for (uint16_t i = 0; i < p.count(); i++) v.emplace_back(p.Rec(i));
  return v;
}

void Rebuild(Page p, const std::vector<std::string>& recs, size_t from, size_t to) {
  PageId id = p.id();
  PageType t = p.type();
  uint8_t lvl = p.level();
  PageId nx = p.next();
  PageId lm = p.leftmost();
  Lsn lsn = p.lsn();
  p.Format(id, t, lvl);
  p.set_next(nx);
  p.set_leftmost(lm);
  p.set_lsn(lsn);
  for (size_t i = from; i < to; i++) p.Insert(static_cast<uint16_t>(i - from), recs[i]);
}

size_t SplitPoint(const std::vector<std::string>& recs, size_t lo, size_t hi) {
  size_t total = 0;
  for (auto& r : recs) total += r.size() + kSlotSize;
  size_t acc = 0;
  size_t m = recs.size() / 2;
  for (size_t i = 0; i < recs.size(); i++) {
    acc += recs[i].size() + kSlotSize;
    if (acc * 2 >= total) {
      m = i + 1;
      break;
    }
  }
  return std::clamp(m, lo, hi);
}

void CheckNode(Page p) {
  if (p.type() != PageType::Leaf && p.type() != PageType::Internal)
    Fail(state::kCorrupt, "page " + std::to_string(p.id()) + " is not a tree node");
}

}

void ApplyLeaf(Page p, std::string_view key, const std::string* rec) {
  bool exact = false;
  uint16_t idx = p.LowerBound(key, &exact);
  if (!rec) {
    if (exact) p.Erase(idx);
    return;
  }
  if (exact) {
    p.Replace(idx, *rec);
  } else {
    p.Insert(idx, *rec);
  }
}

PageId BTree::Create(Store& store) {
  PageRef meta = store.pool.Fetch(kMetaPage);
  meta.LockX();
  Page m = meta.page();
  PageId root = Allocate(meta);
  PageRef r = store.pool.Create(root);
  r.page().Format(root, PageType::Leaf, 0);
  LogRecord rec;
  rec.type = LogType::Images;
  rec.images.emplace_back(kMetaPage, m.Image());
  rec.images.emplace_back(root, r.page().Image());
  store.log.Append(rec, [&](Lsn l) {
    m.set_lsn(l);
    store.pool.MarkDirty(meta.frame(), l);
    r.page().set_lsn(l);
    store.pool.MarkDirty(r.frame(), l);
  });
  return root;
}

PageRef BTree::DescendRead(std::string_view key) {
  PageRef cur = s_.pool.Fetch(root_);
  cur.LockS();
  for (;;) {
    Page p = cur.page();
    CheckNode(p);
    if (p.leaf()) return cur;
    PageRef nxt = s_.pool.Fetch(p.ChildFor(key));
    nxt.LockS();
    cur = std::move(nxt);
  }
}

PageRef BTree::DescendWrite(std::string_view key) {
  PageRef cur = s_.pool.Fetch(root_);
  cur.LockS();
  CheckNode(cur.page());
  if (cur.page().leaf()) {
    cur.Unlock();
    cur.LockX();
    if (cur.page().leaf()) return cur;
    cur.Unlock();
    cur.LockS();
  }
  for (;;) {
    Page p = cur.page();
    CheckNode(p);
    bool child_leaf = p.level() == 1;
    PageRef nxt = s_.pool.Fetch(p.ChildFor(key));
    if (child_leaf) {
      nxt.LockX();
    } else {
      nxt.LockS();
    }
    cur = std::move(nxt);
    if (child_leaf) {
      CheckNode(cur.page());
      return cur;
    }
  }
}

bool BTree::Get(std::string_view key, const std::function<void(const LeafRec*)>& fn) {
  PageRef leaf = DescendRead(key);
  Page p = leaf.page();
  bool exact = false;
  uint16_t idx = p.LowerBound(key, &exact);
  if (exact) {
    LeafRec r = ParseLeaf(p.Rec(idx));
    fn(&r);
  } else {
    fn(nullptr);
  }
  return exact;
}

void BTree::Scan(std::string_view lo, const std::string* hi, bool hi_inclusive, const Visit& fn) {
  PageRef leaf = DescendRead(lo);
  uint16_t idx = leaf.page().LowerBound(lo, nullptr);
  for (;;) {
    Page p = leaf.page();
    for (; idx < p.count(); idx++) {
      std::string_view k = p.KeyAt(idx);
      if (hi) {
        int c = CompareKeys(k, *hi);
        if (c > 0 || (c == 0 && !hi_inclusive)) return;
      }
      if (!fn(ParseLeaf(p.Rec(idx)))) return;
    }
    PageId nx = p.next();
    if (nx == kNoPage) return;
    PageRef nref = s_.pool.Fetch(nx);
    nref.LockS();
    leaf = std::move(nref);
    idx = 0;
  }
}

bool BTree::Modify(std::string_view key, const Decide& decide, const Logger& log) {
  if (key.size() > kMaxKeySize) Fail(state::kLimit, "key is too long");
  for (int attempt = 0;; attempt++) {
    if (attempt > 64) Fail(state::kInternal, "could not make room for a row after repeated splits");
    PageRef leaf = DescendWrite(key);
    Page p = leaf.page();
    bool exact = false;
    uint16_t idx = p.LowerBound(key, &exact);
    std::string before_bytes;
    LeafRec before;
    if (exact) {
      before_bytes.assign(p.Rec(idx));
      before = ParseLeaf(before_bytes);
    }
    std::optional<LeafChange> ch = decide(exact ? &before : nullptr);
    if (!ch) return false;
    if (ch->remove) {
      if (!exact) return false;
      ApplyLeaf(p, key, nullptr);
      log(leaf, &before, *ch);
      return true;
    }
    std::string rec = MakeLeaf(key, ch->xid, ch->flags, ch->value);
    if (rec.size() > kMaxRecordSize) Fail(state::kLimit, "row is too large for a page");
    size_t have = p.FreeSpace() + (exact ? p.Rec(idx).size() + kSlotSize : 0);
    if (rec.size() + kSlotSize <= have) {
      ApplyLeaf(p, key, &rec);
      log(leaf, exact ? &before : nullptr, *ch);
      return true;
    }
    leaf.Release();
    Split(key, rec.size());
  }
}

void BTree::LogImages(const std::vector<PageRef*>& pages) {
  LogRecord rec;
  rec.type = LogType::Images;
  for (PageRef* r : pages) rec.images.emplace_back(r->id(), r->page().Image());
  s_.log.Append(rec, [&](Lsn l) {
    for (PageRef* r : pages) {
      r->page().set_lsn(l);
      s_.pool.MarkDirty(r->frame(), l);
    }
  });
}

void BTree::Split(std::string_view key, size_t need) {
  std::vector<PageRef> path;
  {
    PageRef r = s_.pool.Fetch(root_);
    r.LockX();
    path.push_back(std::move(r));
  }
  while (!path.back().page().leaf()) {
    CheckNode(path.back().page());
    PageRef c = s_.pool.Fetch(path.back().page().ChildFor(key));
    c.LockX();
    CheckNode(c.page());
    if (!c.page().leaf() && c.page().FreeSpace() >= kMaxBranchEntry) path.clear();
    path.push_back(std::move(c));
  }
  {
    Page leaf = path.back().page();
    bool exact = false;
    uint16_t idx = leaf.LowerBound(key, &exact);
    size_t have = leaf.FreeSpace() + (exact ? leaf.Rec(idx).size() + kSlotSize : 0);
    if (need + kSlotSize <= have) return;
  }
  PageRef meta = s_.pool.Fetch(kMetaPage);
  meta.LockX();
  std::deque<PageRef> fresh;
  std::vector<PageRef*> touched;
  auto touch = [&](PageRef* r) {
    if (std::find(touched.begin(), touched.end(), r) == touched.end()) touched.push_back(r);
  };
  touch(&meta);

  auto split_node = [&](PageRef& node, std::string* up_key,
                        PageId* up_child) -> std::pair<PageRef*, PageRef*> {
    PageRef* target = &node;
    if (node.id() == root_) {
      PageId lid = Allocate(meta);
      fresh.push_back(s_.pool.Create(lid));
      PageRef& left = fresh.back();
      std::memcpy(left.data(), node.data(), kPageSize);
      left.page().set_id(lid);
      uint8_t lvl = node.page().level();
      node.page().Format(root_, PageType::Internal, static_cast<uint8_t>(lvl + 1));
      node.page().set_leftmost(lid);
      touch(&node);
      target = &left;
    }
    Page t = target->page();
    std::vector<std::string> recs = Records(t);
    if (recs.size() < 3)
      Fail(state::kInternal, "cannot split a node with fewer than three entries");
    PageId rid = Allocate(meta);
    fresh.push_back(s_.pool.Create(rid));
    PageRef& right = fresh.back();
    Page rp = right.page();
    if (t.leaf()) {
      size_t m = SplitPoint(recs, 1, recs.size() - 1);
      rp.Format(rid, PageType::Leaf, 0);
      for (size_t i = m; i < recs.size(); i++) rp.Insert(static_cast<uint16_t>(i - m), recs[i]);
      rp.set_next(t.next());
      Rebuild(t, recs, 0, m);
      t.set_next(rid);
      *up_key = std::string(rp.KeyAt(0));
    } else {
      size_t m = SplitPoint(recs, 1, recs.size() - 2);
      rp.Format(rid, PageType::Internal, t.level());
      rp.set_leftmost(BranchChild(recs[m]));
      for (size_t i = m + 1; i < recs.size(); i++)
        rp.Insert(static_cast<uint16_t>(i - m - 1), recs[i]);
      *up_key = std::string(recs[m].data() + 2, GetU16(recs[m].data()));
      Rebuild(t, recs, 0, m);
    }
    *up_child = rid;
    touch(target);
    touch(&right);
    return {target, &right};
  };

  size_t i = path.size() - 1;
  std::string up_key;
  PageId up_child = kNoPage;
  split_node(path[i], &up_key, &up_child);
  for (;;) {
    PageRef* parent;
    if (path[i].id() == root_) {
      parent = &path[i];
    } else {
      if (i == 0) Fail(state::kInternal, "split ran out of ancestors");
      parent = &path[--i];
    }
    std::string entry = MakeBranch(up_key, up_child);
    Page pp = parent->page();
    if (pp.FreeSpace() >= entry.size() + kSlotSize) {
      pp.Insert(pp.UpperBound(up_key), entry);
      touch(parent);
      break;
    }
    std::string k2;
    PageId c2 = kNoPage;
    auto [l2, r2] = split_node(*parent, &k2, &c2);
    PageRef* dst = CompareKeys(up_key, k2) < 0 ? l2 : r2;
    Page d = dst->page();
    d.Insert(d.UpperBound(up_key), entry);
    up_key = std::move(k2);
    up_child = c2;
  }
  LogImages(touched);
}

std::string BTree::LastKey() {
  PageRef cur = s_.pool.Fetch(root_);
  cur.LockS();
  for (;;) {
    Page p = cur.page();
    CheckNode(p);
    if (p.leaf()) return p.count() == 0 ? std::string() : std::string(p.KeyAt(p.count() - 1));
    PageId child =
        p.count() == 0 ? p.leftmost() : BranchChild(p.Rec(static_cast<uint16_t>(p.count() - 1)));
    PageRef nxt = s_.pool.Fetch(child);
    nxt.LockS();
    cur = std::move(nxt);
  }
}

BTree::Shape BTree::Validate() {
  Shape s;
  int leaf_depth = -1;
  std::vector<PageId> leaves;
  std::function<void(PageId, const std::string*, const std::string*, int, int)> walk =
      [&](PageId id, const std::string* lo, const std::string* hi, int depth, int level) {
        std::vector<std::string> keys;
        std::vector<PageId> kids;
        bool is_leaf;
        {
          PageRef r = s_.pool.Fetch(id);
          r.LockS();
          Page p = r.page();
          CheckNode(p);
          is_leaf = p.leaf();
          if (level >= 0 && p.level() != level)
            Fail(state::kCorrupt, "page " + std::to_string(id) + " has the wrong level");
          for (uint16_t i = 0; i < p.count(); i++) keys.emplace_back(p.KeyAt(i));
          if (!is_leaf) {
            kids.push_back(p.leftmost());
            for (uint16_t i = 0; i < p.count(); i++) kids.push_back(BranchChild(p.Rec(i)));
          }
          if (is_leaf) {
            s.records += p.count();
          } else {
            level = p.level();
          }
        }
        for (size_t i = 0; i < keys.size(); i++) {
          if (i > 0 && CompareKeys(keys[i - 1], keys[i]) >= 0)
            Fail(state::kCorrupt, "keys out of order in page " + std::to_string(id));
          if (lo && CompareKeys(keys[i], *lo) < 0)
            Fail(state::kCorrupt, "key below its separator in page " + std::to_string(id));
          if (hi && CompareKeys(keys[i], *hi) >= 0)
            Fail(state::kCorrupt, "key above its separator in page " + std::to_string(id));
        }
        if (is_leaf) {
          s.leaves++;
          if (leaf_depth < 0) leaf_depth = depth;
          if (leaf_depth != depth) Fail(state::kCorrupt, "leaves at different depths");
          leaves.push_back(id);
          return;
        }
        s.internals++;
        for (size_t c = 0; c < kids.size(); c++) {
          const std::string* clo = c == 0 ? lo : &keys[c - 1];
          const std::string* chi = c < keys.size() ? &keys[c] : hi;
          walk(kids[c], clo, chi, depth + 1, level - 1);
        }
      };
  walk(root_, nullptr, nullptr, 0, -1);
  PageId at = leaves.front();
  for (size_t i = 0; i < leaves.size(); i++) {
    if (at != leaves[i]) Fail(state::kCorrupt, "leaf chain does not match tree order");
    PageRef r = s_.pool.Fetch(at);
    r.LockS();
    at = r.page().next();
  }
  if (at != kNoPage) Fail(state::kCorrupt, "leaf chain runs past the last leaf");
  s.height = leaf_depth + 1;
  return s;
}

}
