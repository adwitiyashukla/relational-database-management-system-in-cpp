#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "common.h"
#include "wal.h"

namespace rdbms {

enum class Isolation : uint8_t { ReadCommitted = 0, RepeatableRead = 1, Serializable = 2 };

const char* IsolationName(Isolation iso);
bool ParseIsolation(std::string_view text, Isolation* out);

constexpr Xid kPurgeXid = ~0ull - 1;

struct UndoEntry {
  uint32_t tree = 0;
  std::string key;
  Lsn lsn = 0;
  bool has_before = false;
  RowImage before;
  bool pushed = false;
};

struct Txn {
  Xid xid = 0;
  Isolation iso = Isolation::ReadCommitted;
  std::atomic<Ts> snapshot{0};
  Ts commit_ts = 0;
  bool ssi_registered = false;
  Lsn first_lsn = 0;
  Lsn last_lsn = 0;
  TxnStatus wal_status = TxnStatus::Active;
  bool ended = false;
  std::vector<UndoEntry> undo;
  std::vector<std::string> locks;
  std::vector<std::pair<uint32_t, std::string>> deletes;
  std::atomic<bool> doomed{false};
  std::atomic<bool> cancel{false};
  std::atomic<bool> published{false};

  bool ser() const { return iso == Isolation::Serializable; }
  bool wrote() const { return first_lsn != 0; }
};

class TxnManager {
 public:
  explicit TxnManager(Xid next_xid);
  void Register(Txn* t, Isolation iso);
  void TakeSnapshot(Txn& t);
  bool Visible(Xid writer, const Txn& reader);
  bool VisibleToAll(Xid writer);
  void RegisterCommit(Txn* t, Lsn lsn);
  void Publish(Lsn flushed);
  void Finish(Txn& t);
  void Gc();
  Ts clock();
  Xid next_xid() const { return next_xid_.load(); }
  size_t active_count();
  std::vector<TxnEntry> ActiveForCheckpoint();

 private:
  SharedMutex mu_;
  std::unordered_map<Xid, Txn*> active_;
  std::unordered_map<Xid, Ts> committed_;
  std::atomic<Xid> next_xid_;
  std::atomic<Xid> visible_below_;
  Ts clock_ = 1;
  Ts oldest_ = 1;
  std::mutex pub_mu_;
  std::map<Lsn, Txn*> pending_;
};

class LockManager {
 public:
  explicit LockManager(uint32_t timeout_ms) : timeout_ms_(timeout_ms) {}
  void Lock(Txn& t, const std::string& key);
  bool TryLock(Xid owner, const std::string& key);
  void Unlock(Xid owner, const std::string& key);
  void ReleaseAll(Txn& t);
  uint64_t waits() const { return waits_.load(); }
  uint64_t deadlocks() const { return deadlocks_.load(); }

 private:
  struct Entry {
    Xid owner = 0;
    std::deque<Xid> waiters;
  };
  struct Shard {
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, Entry> map;
  };
  static constexpr size_t kShards = 64;
  Shard& ShardFor(const std::string& key);
  void Grant(Shard& s, std::unordered_map<std::string, Entry>::iterator it);

  std::array<Shard, kShards> shards_;
  std::mutex graph_mu_;
  std::unordered_map<Xid, Xid> waits_for_;
  uint32_t timeout_ms_;
  std::atomic<uint64_t> waits_{0};
  std::atomic<uint64_t> deadlocks_{0};
};

struct Version {
  Xid xid = 0;
  Xid succ = 0;
  uint8_t flags = 0;
  std::string value;
};

class VersionStore {
 public:
  void Push(uint32_t tree, std::string_view key, Version v);
  void Pop(uint32_t tree, std::string_view key);
  void Erase(uint32_t tree, std::string_view key);
  void Find(uint32_t tree, std::string_view key, const std::function<bool(const Version&)>& fn);
  size_t Gc(const std::function<bool(Xid)>& visible_to_all);
  size_t size() const { return count_.load(); }

 private:
  struct Shard {
    std::mutex mu;
    std::unordered_map<std::string, std::vector<Version>> map;
  };
  static std::string Key(uint32_t tree, std::string_view key);
  Shard& ShardFor(const std::string& k);

  std::array<Shard, 64> shards_;
  std::atomic<size_t> count_{0};
};

class SsiManager {
 public:
  void Begin(Txn& t, const std::function<void()>& take_snapshot);
  void OnRead(Txn& t, uint32_t tree, std::string_view key);
  void OnScan(Txn& t, uint32_t tree, const std::string& lo, const std::string* hi, bool hi_incl);
  void OnSkip(Txn& reader, Xid writer);
  void OnWrite(Txn& writer, uint32_t tree, std::string_view key);
  void PreCommit(Txn& t, const std::function<void()>& then);
  void Committed(Txn& t, bool read_only, Ts point);
  void Aborted(Txn& t);
  void Gc();
  size_t nodes();
  uint64_t failures() const { return failures_.load(); }

 private:
  struct Node {
    Xid xid = 0;
    Txn* txn = nullptr;
    Ts snapshot = 0;
    bool prepared = false;
    uint64_t seq = ~0ull;
    bool published = false;
    bool read_only = false;
    Ts point = 0;
    uint64_t gone_out = ~0ull;
    std::unordered_set<Xid> in;
    std::unordered_set<Xid> out;
    std::vector<std::pair<uint32_t, std::string>> points;
    std::unordered_map<uint32_t, size_t> per_tree;
    std::unordered_set<uint32_t> escalated;
  };
  struct RangeLock {
    Xid xid;
    std::string lo;
    std::string hi;
    bool has_hi;
    bool hi_incl;
  };
  struct TreeLocks {
    std::unordered_map<std::string, std::vector<Xid>> points;
    std::vector<RangeLock> ranges;
  };

  bool Before(const Node& n, Ts snapshot) const;
  uint64_t Seq(const Node& n) const { return n.prepared ? n.seq : ~0ull; }
  void AddEdge(Node& r, Node& w);
  void Remove(Xid xid);
  [[noreturn]] void Abort(const char* why);

  std::mutex mu_;
  std::unordered_map<Xid, Node> nodes_;
  std::unordered_map<uint32_t, TreeLocks> locks_;
  uint64_t seq_ = 0;
  std::atomic<uint64_t> failures_{0};
};

}
