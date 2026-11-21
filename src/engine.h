#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <thread>

#include "btree.h"
#include "buffer.h"
#include "fs.h"
#include "txn.h"
#include "value.h"
#include "wal.h"

namespace rdbms {

struct EngineOptions {
  std::string dir = "data";
  size_t pool_pages = 8192;
  WalOptions wal;
  uint32_t lock_timeout_ms = 10000;
  bool background = true;
  uint32_t checkpoint_interval_ms = 30000;
  uint64_t checkpoint_wal_bytes = 64ull << 20;
  uint32_t gc_interval_ms = 20;
};

struct RecoveryStats {
  bool fresh = false;
  int torn_pages = 0;
  uint64_t records = 0;
  uint64_t redone = 0;
  uint64_t losers = 0;
  uint64_t undone = 0;
  Lsn checkpoint = 0;
  Lsn redo_start = 0;
  Lsn end = 0;
  double millis = 0;
};

struct Table {
  Schema schema;
  std::unique_ptr<BTree> tree;
  std::atomic<int64_t> next_rowid{1};
};

class Engine {
 public:
  enum class Op { Insert, Update, Delete, Lock };
  using Compute = std::function<std::optional<std::string>(const std::string* latest)>;
  using RowFn = std::function<bool(std::string_view key, std::string_view value)>;

  struct Stats {
    uint64_t commits = 0;
    uint64_t aborts = 0;
    uint64_t serialization_failures = 0;
    uint64_t deadlocks = 0;
    uint64_t lock_waits = 0;
    uint64_t versions = 0;
    uint64_t purged = 0;
    uint64_t checkpoints = 0;
    uint64_t wal_bytes = 0;
    uint64_t wal_flushes = 0;
    uint64_t pages_written = 0;
    size_t dirty_pages = 0;
    size_t active_txns = 0;
    size_t ssi_nodes = 0;
  };

  Engine(FileSystem& fs, EngineOptions opt);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void Open();
  void Close();
  void Halt();

  Txn* Begin(Isolation iso);
  void StartStatement(Txn& t);
  void Commit(Txn* t);
  void Abort(Txn* t);

  bool Read(Txn& t, BTree& tree, std::string_view key, std::string* value);
  void Scan(Txn& t, BTree& tree, const std::string& lo, const std::string* hi, bool hi_incl,
            const RowFn& fn);
  bool Write(Txn& t, BTree& tree, std::string_view key, Op op, const Compute& compute);

  std::shared_ptr<Table> FindTable(const std::string& name);
  std::vector<std::shared_ptr<Table>> Tables();
  void CreateTable(Schema schema, bool if_not_exists);
  bool DropTable(const std::string& name, bool if_exists);
  int64_t NextRowId(Table& t) { return t.next_rowid.fetch_add(1); }

  void Checkpoint();
  void FuzzyCheckpoint();
  void RunGc();
  size_t Vacuum();
  void FlushPages(size_t n);

  const RecoveryStats& recovery() const { return recovery_; }
  Stats stats();
  BufferPool& pool() { return *pool_; }
  LogManager& log() { return *log_; }
  TxnManager& txns() { return *txns_; }
  VersionStore& versions() { return versions_; }
  Store& store() { return *store_; }
  bool closed() const { return closed_; }

 private:
  void CreateFresh();
  void Recover();
  void Redo(const LogRecord& r, const std::map<PageId, Lsn>& dpt);
  void UndoLosers(std::map<Xid, TxnEntry>& att);
  void LoadCatalog();
  bool Resolve(Txn& t, uint32_t tree, const LeafRec& cur, std::string* value);
  void RollbackEntry(Txn& t, const UndoEntry& e, Lsn undo_next);
  void Release(Txn* t);
  void Purge();
  void WriterLoop();
  void GcLoop();
  std::string LockKey(uint32_t tree, std::string_view key);

  FileSystem& fs_;
  EngineOptions opt_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<LogManager> log_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<Store> store_;
  std::unique_ptr<TxnManager> txns_;
  std::unique_ptr<LockManager> locks_;
  VersionStore versions_;
  SsiManager ssi_;
  RecoveryStats recovery_;

  SharedMutex catalog_mu_;
  std::map<std::string, std::shared_ptr<Table>> tables_;
  std::mutex ddl_mu_;

  std::mutex live_mu_;
  std::set<Txn*> live_;

  std::mutex purge_mu_;
  struct PurgeItem {
    uint32_t tree;
    std::string key;
    Xid xid;
  };
  std::vector<PurgeItem> purge_;

  std::mutex ckpt_mu_;
  std::atomic<Lsn> last_ckpt_{0};
  std::atomic<uint64_t> last_ckpt_time_{0};

  std::mutex bg_mu_;
  std::condition_variable bg_cv_;
  bool stop_ = false;
  bool kick_ = false;
  std::atomic<bool> kicked_{false};
  std::thread writer_;
  std::thread gc_;
  bool opened_ = false;
  bool closed_ = false;

  std::atomic<uint64_t> commits_{0};
  std::atomic<uint64_t> aborts_{0};
  std::atomic<uint64_t> purged_{0};
  std::atomic<uint64_t> checkpoints_{0};
};

}
