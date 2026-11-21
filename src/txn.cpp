#include "txn.h"

#include <algorithm>
#include <chrono>

namespace rdbms {

const char* IsolationName(Isolation iso) {
  switch (iso) {
    case Isolation::ReadCommitted:
      return "read committed";
    case Isolation::RepeatableRead:
      return "repeatable read";
    case Isolation::Serializable:
      return "serializable";
  }
  return "read committed";
}

bool ParseIsolation(std::string_view text, Isolation* out) {
  std::string s = Lower(Trim(text));
  if (s == "read committed" || s == "read uncommitted") {
    *out = Isolation::ReadCommitted;
  } else if (s == "repeatable read" || s == "snapshot") {
    *out = Isolation::RepeatableRead;
  } else if (s == "serializable") {
    *out = Isolation::Serializable;
  } else {
    return false;
  }
  return true;
}

TxnManager::TxnManager(Xid next_xid) : next_xid_(next_xid), visible_below_(next_xid) {}

void TxnManager::Register(Txn* t, Isolation iso) {
  std::unique_lock<SharedMutex> l(mu_);
  t->xid = next_xid_.fetch_add(1);
  t->iso = iso;
  active_[t->xid] = t;
}

void TxnManager::TakeSnapshot(Txn& t) {
  std::shared_lock<SharedMutex> l(mu_);
  t.snapshot.store(clock_);
}

bool TxnManager::Visible(Xid w, const Txn& r) {
  if (w == r.xid) return true;
  if (w < visible_below_.load()) return true;
  std::shared_lock<SharedMutex> l(mu_);
  if (active_.count(w)) return false;
  auto it = committed_.find(w);
  if (it != committed_.end()) return it->second <= r.snapshot.load();
  return true;
}

bool TxnManager::VisibleToAll(Xid w) {
  if (w < visible_below_.load()) return true;
  std::shared_lock<SharedMutex> l(mu_);
  if (active_.count(w)) return false;
  auto it = committed_.find(w);
  if (it != committed_.end()) return it->second <= oldest_;
  return true;
}

void TxnManager::RegisterCommit(Txn* t, Lsn lsn) {
  std::lock_guard<std::mutex> g(pub_mu_);
  pending_[lsn] = t;
}

void TxnManager::Publish(Lsn flushed) {
  std::lock_guard<std::mutex> g(pub_mu_);
  while (!pending_.empty() && pending_.begin()->first < flushed) {
    Txn* t = pending_.begin()->second;
    pending_.erase(pending_.begin());
    {
      std::unique_lock<SharedMutex> l(mu_);
      t->commit_ts = ++clock_;
      active_.erase(t->xid);
      committed_[t->xid] = t->commit_ts;
    }
    t->published.store(true);
  }
}

void TxnManager::Finish(Txn& t) {
  std::unique_lock<SharedMutex> l(mu_);
  active_.erase(t.xid);
}

void TxnManager::Gc() {
  std::unique_lock<SharedMutex> l(mu_);
  Ts oldest = clock_;
  Xid low = next_xid_.load();
  for (auto& [x, t] : active_) {
    Ts s = t->snapshot.load();
    if (s != 0 && s < oldest) oldest = s;
    low = std::min(low, x);
  }
  oldest_ = oldest;
  for (auto it = committed_.begin(); it != committed_.end();) {
    if (it->second <= oldest) {
      it = committed_.erase(it);
    } else {
      low = std::min(low, it->first);
      ++it;
    }
  }
  visible_below_.store(low);
}

Ts TxnManager::clock() {
  std::shared_lock<SharedMutex> l(mu_);
  return clock_;
}

size_t TxnManager::active_count() {
  std::shared_lock<SharedMutex> l(mu_);
  return active_.size();
}

std::vector<TxnEntry> TxnManager::ActiveForCheckpoint() {
  std::shared_lock<SharedMutex> l(mu_);
  std::vector<TxnEntry> v;
  for (auto& [x, t] : active_) {
    if (t->first_lsn != 0 && !t->ended) v.push_back({x, t->wal_status, t->first_lsn, t->last_lsn});
  }
  return v;
}

LockManager::Shard& LockManager::ShardFor(const std::string& key) {
  return shards_[std::hash<std::string>{}(key) % kShards];
}

void LockManager::Grant(Shard& s, std::unordered_map<std::string, Entry>::iterator it) {
  Entry& e = it->second;
  if (e.waiters.empty()) {
    s.map.erase(it);
    return;
  }
  Xid next = e.waiters.front();
  e.waiters.pop_front();
  e.owner = next;
  {
    std::lock_guard<std::mutex> g(graph_mu_);
    waits_for_.erase(next);
    for (Xid w : e.waiters) waits_for_[w] = next;
  }
  s.cv.notify_all();
}

void LockManager::Lock(Txn& t, const std::string& key) {
  Shard& s = ShardFor(key);
  std::unique_lock<std::mutex> l(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) {
    s.map.emplace(key, Entry{t.xid, {}});
    t.locks.push_back(key);
    return;
  }
  if (it->second.owner == t.xid) return;
  it->second.waiters.push_back(t.xid);
  waits_++;
  auto give_up = [&](const char* code, const std::string& msg) {
    auto e = s.map.find(key);
    if (e != s.map.end()) {
      if (e->second.owner == t.xid) {
        Grant(s, e);
      } else {
        auto& w = e->second.waiters;
        w.erase(std::remove(w.begin(), w.end(), t.xid), w.end());
      }
    }
    {
      std::lock_guard<std::mutex> g(graph_mu_);
      waits_for_.erase(t.xid);
    }
    Fail(code, msg);
  };
  bool cycle = false;
  {
    std::lock_guard<std::mutex> g(graph_mu_);
    waits_for_[t.xid] = it->second.owner;
    Xid cur = it->second.owner;
    for (size_t hops = 0; hops <= waits_for_.size(); hops++) {
      auto w = waits_for_.find(cur);
      if (w == waits_for_.end()) break;
      cur = w->second;
      if (cur == t.xid) {
        cycle = true;
        break;
      }
    }
  }
  if (cycle) {
    deadlocks_++;
    give_up(state::kDeadlock, "deadlock detected");
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
  for (;;) {
    auto e = s.map.find(key);
    if (e != s.map.end() && e->second.owner == t.xid) {
      {
        std::lock_guard<std::mutex> g(graph_mu_);
        waits_for_.erase(t.xid);
      }
      t.locks.push_back(key);
      return;
    }
    if (t.cancel.load()) give_up(state::kCanceled, "canceling statement due to user request");
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) give_up(state::kLockTimeout, "canceling statement due to lock timeout");
    s.cv.wait_until(l, std::min(deadline, now + std::chrono::milliseconds(50)));
  }
}

bool LockManager::TryLock(Xid owner, const std::string& key) {
  Shard& s = ShardFor(key);
  std::lock_guard<std::mutex> l(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) {
    s.map.emplace(key, Entry{owner, {}});
    return true;
  }
  return it->second.owner == owner;
}

void LockManager::Unlock(Xid owner, const std::string& key) {
  Shard& s = ShardFor(key);
  std::lock_guard<std::mutex> l(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end() || it->second.owner != owner) return;
  Grant(s, it);
}

void LockManager::ReleaseAll(Txn& t) {
  for (auto& k : t.locks) Unlock(t.xid, k);
  t.locks.clear();
}

std::string VersionStore::Key(uint32_t tree, std::string_view key) {
  std::string k(4, '\0');
  PutU32(k.data(), tree);
  k.append(key.data(), key.size());
  return k;
}

VersionStore::Shard& VersionStore::ShardFor(const std::string& k) {
  return shards_[std::hash<std::string>{}(k) % shards_.size()];
}

void VersionStore::Push(uint32_t tree, std::string_view key, Version v) {
  std::string k = Key(tree, key);
  Shard& s = ShardFor(k);
  std::lock_guard<std::mutex> g(s.mu);
  s.map[k].push_back(std::move(v));
  count_++;
}

void VersionStore::Pop(uint32_t tree, std::string_view key) {
  std::string k = Key(tree, key);
  Shard& s = ShardFor(k);
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(k);
  if (it == s.map.end() || it->second.empty()) return;
  it->second.pop_back();
  count_--;
  if (it->second.empty()) s.map.erase(it);
}

void VersionStore::Erase(uint32_t tree, std::string_view key) {
  std::string k = Key(tree, key);
  Shard& s = ShardFor(k);
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(k);
  if (it == s.map.end()) return;
  count_ -= it->second.size();
  s.map.erase(it);
}

void VersionStore::Find(uint32_t tree, std::string_view key,
                        const std::function<bool(const Version&)>& fn) {
  std::string k = Key(tree, key);
  Shard& s = ShardFor(k);
  std::lock_guard<std::mutex> g(s.mu);
  auto it = s.map.find(k);
  if (it == s.map.end()) return;
  auto& v = it->second;
  for (size_t i = v.size(); i-- > 0;) {
    if (fn(v[i])) return;
  }
}

size_t VersionStore::Gc(const std::function<bool(Xid)>& visible_to_all) {
  size_t removed = 0;
  for (auto& s : shards_) {
    std::lock_guard<std::mutex> g(s.mu);
    for (auto it = s.map.begin(); it != s.map.end();) {
      auto& v = it->second;
      size_t drop = 0;
      while (drop < v.size() && visible_to_all(v[drop].succ)) drop++;
      if (drop > 0) {
        v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(drop));
        removed += drop;
        count_ -= drop;
      }
      if (v.empty()) {
        it = s.map.erase(it);
      } else {
        ++it;
      }
    }
  }
  return removed;
}

namespace {

constexpr size_t kEscalate = 10000;
constexpr const char* kSsiMessage =
    "could not serialize access due to read/write dependencies among transactions";

bool Covers(const std::string& lo, const std::string& hi, bool has_hi, bool incl,
            std::string_view key) {
  if (CompareKeys(key, lo) < 0) return false;
  if (!has_hi) return true;
  int c = CompareKeys(key, hi);
  return c < 0 || (c == 0 && incl);
}

}

void SsiManager::Abort(const char* why) {
  failures_++;
  Fail(state::kSerialization, why);
}

bool SsiManager::Before(const Node& n, Ts snapshot) const {
  if (!n.published) return false;
  return n.read_only ? n.point < snapshot : n.point <= snapshot;
}

void SsiManager::Begin(Txn& t, const std::function<void()>& take_snapshot) {
  std::lock_guard<std::mutex> g(mu_);
  take_snapshot();
  Node& n = nodes_[t.xid];
  n.xid = t.xid;
  n.txn = &t;
  n.snapshot = t.snapshot.load();
  t.ssi_registered = true;
}

void SsiManager::AddEdge(Node& r, Node& w) {
  if (r.xid == w.xid) return;
  if (!r.out.insert(w.xid).second) return;
  w.in.insert(r.xid);
  if (w.gone_out < Seq(w) && w.gone_out < Seq(r)) Abort(kSsiMessage);
  for (Xid x3 : w.out) {
    auto it = nodes_.find(x3);
    if (it == nodes_.end() || !it->second.prepared) continue;
    const Node& t3 = it->second;
    if (Seq(t3) < Seq(w) && (x3 == r.xid || Seq(t3) < Seq(r))) Abort(kSsiMessage);
  }
  if (w.prepared && Seq(w) < Seq(r)) {
    for (Xid x1 : r.in) {
      auto it = nodes_.find(x1);
      if (it == nodes_.end()) continue;
      if (x1 == w.xid || Seq(w) < Seq(it->second)) Abort(kSsiMessage);
    }
  }
}

void SsiManager::OnRead(Txn& t, uint32_t tree, std::string_view key) {
  std::lock_guard<std::mutex> g(mu_);
  auto ni = nodes_.find(t.xid);
  if (ni == nodes_.end()) return;
  Node& n = ni->second;
  if (n.escalated.count(tree)) return;
  auto& vec = locks_[tree].points[std::string(key)];
  if (std::find(vec.begin(), vec.end(), t.xid) != vec.end()) return;
  vec.push_back(t.xid);
  n.points.emplace_back(tree, std::string(key));
  if (++n.per_tree[tree] > kEscalate) {
    locks_[tree].ranges.push_back({t.xid, std::string(), std::string(), false, false});
    n.escalated.insert(tree);
  }
}

void SsiManager::OnScan(Txn& t, uint32_t tree, const std::string& lo, const std::string* hi,
                        bool hi_incl) {
  std::lock_guard<std::mutex> g(mu_);
  auto ni = nodes_.find(t.xid);
  if (ni == nodes_.end()) return;
  Node& n = ni->second;
  if (n.escalated.count(tree)) return;
  locks_[tree].ranges.push_back({t.xid, lo, hi ? *hi : std::string(), hi != nullptr, hi_incl});
  if (++n.per_tree[tree] > kEscalate) {
    locks_[tree].ranges.push_back({t.xid, std::string(), std::string(), false, false});
    n.escalated.insert(tree);
  }
}

void SsiManager::OnSkip(Txn& reader, Xid writer) {
  std::lock_guard<std::mutex> g(mu_);
  auto wi = nodes_.find(writer);
  if (wi == nodes_.end()) return;
  auto ri = nodes_.find(reader.xid);
  if (ri == nodes_.end()) return;
  AddEdge(ri->second, wi->second);
}

void SsiManager::OnWrite(Txn& writer, uint32_t tree, std::string_view key) {
  std::lock_guard<std::mutex> g(mu_);
  auto wi = nodes_.find(writer.xid);
  if (wi == nodes_.end()) return;
  auto ti = locks_.find(tree);
  if (ti == locks_.end()) return;
  TreeLocks& tl = ti->second;
  std::vector<Xid> readers;
  auto pi = tl.points.find(std::string(key));
  if (pi != tl.points.end()) readers = pi->second;
  for (auto& r : tl.ranges) {
    if (Covers(r.lo, r.hi, r.has_hi, r.hi_incl, key)) readers.push_back(r.xid);
  }
  std::sort(readers.begin(), readers.end());
  readers.erase(std::unique(readers.begin(), readers.end()), readers.end());
  for (Xid x : readers) {
    if (x == writer.xid) continue;
    auto ri = nodes_.find(x);
    if (ri == nodes_.end()) continue;
    if (Before(ri->second, wi->second.snapshot)) continue;
    AddEdge(ri->second, wi->second);
  }
}

void SsiManager::PreCommit(Txn& t, const std::function<void()>& then) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = nodes_.find(t.xid);
  if (it == nodes_.end()) {
    then();
    return;
  }
  Node& n = it->second;
  if (t.doomed.load()) Abort(kSsiMessage);
  for (Xid x3 : n.out) {
    auto c = nodes_.find(x3);
    if (c == nodes_.end() || !c->second.prepared) continue;
    for (Xid x1 : n.in) {
      auto a = nodes_.find(x1);
      if (a == nodes_.end()) continue;
      if (x1 == x3 || Seq(c->second) < Seq(a->second)) Abort(kSsiMessage);
    }
  }
  for (Xid x2 : n.in) {
    auto p = nodes_.find(x2);
    if (p == nodes_.end() || p->second.prepared) continue;
    for (Xid x1 : p->second.in) {
      auto a = nodes_.find(x1);
      if (a == nodes_.end()) continue;
      if (x1 == t.xid || !a->second.prepared) {
        if (p->second.txn) p->second.txn->doomed.store(true);
        break;
      }
    }
  }
  n.prepared = true;
  n.seq = ++seq_;
  then();
}

void SsiManager::Committed(Txn& t, bool read_only, Ts point) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = nodes_.find(t.xid);
  if (it == nodes_.end()) return;
  it->second.published = true;
  it->second.read_only = read_only;
  it->second.point = point;
  it->second.txn = nullptr;
}

void SsiManager::Aborted(Txn& t) {
  std::lock_guard<std::mutex> g(mu_);
  Remove(t.xid);
}

void SsiManager::Remove(Xid xid) {
  auto it = nodes_.find(xid);
  if (it == nodes_.end()) return;
  Node& n = it->second;
  for (Xid x : n.in) {
    auto o = nodes_.find(x);
    if (o == nodes_.end()) continue;
    o->second.out.erase(xid);
    if (n.published) o->second.gone_out = std::min(o->second.gone_out, n.seq);
  }
  for (Xid x : n.out) {
    auto o = nodes_.find(x);
    if (o != nodes_.end()) o->second.in.erase(xid);
  }
  for (auto& [tree, key] : n.points) {
    auto tl = locks_.find(tree);
    if (tl == locks_.end()) continue;
    auto p = tl->second.points.find(key);
    if (p == tl->second.points.end()) continue;
    auto& v = p->second;
    v.erase(std::remove(v.begin(), v.end(), xid), v.end());
    if (v.empty()) tl->second.points.erase(p);
  }
  for (auto& [tree, tl] : locks_) {
    auto& r = tl.ranges;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const RangeLock& x) { return x.xid == xid; }),
            r.end());
  }
  nodes_.erase(it);
}

void SsiManager::Gc() {
  std::lock_guard<std::mutex> g(mu_);
  bool any = false;
  Ts min_snap = ~0ull;
  for (auto& [x, n] : nodes_) {
    if (!n.published) {
      any = true;
      min_snap = std::min(min_snap, n.snapshot);
    }
  }
  std::vector<Xid> victims;
  for (auto& [x, n] : nodes_) {
    if (n.published && (!any || Before(n, min_snap))) victims.push_back(x);
  }
  for (Xid x : victims) Remove(x);
}

size_t SsiManager::nodes() {
  std::lock_guard<std::mutex> g(mu_);
  return nodes_.size();
}

}
