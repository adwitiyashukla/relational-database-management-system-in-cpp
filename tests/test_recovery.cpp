#include "engine.h"
#include "test.h"

using namespace rdbms;

namespace {

EngineOptions Opts(size_t pool = 32) {
  EngineOptions o;
  o.dir = "db";
  o.pool_pages = pool;
  o.background = false;
  o.wal.segment_size = 1 << 15;
  return o;
}

Schema KvSchema() {
  Schema s;
  s.name = "kv";
  s.cols = {{"k", ColType::BigInt, true, -1}, {"v", ColType::Text, false, -1}};
  s.pk = {0};
  return s;
}

std::string K(int64_t k) { return EncodeKey({Value::Int(k)}); }

void Put(Engine& e, Txn* t, int64_t k, const std::string& v, Engine::Op op) {
  std::string row = EncodeRow({Value::Int(k), Value::Text(v)});
  e.Write(*t, *e.FindTable("kv")->tree, K(k), op,
          [&](const std::string*) { return std::optional<std::string>(row); });
}

std::map<int64_t, std::string> Dump(Engine& e) {
  std::map<int64_t, std::string> out;
  Txn* t = e.Begin(Isolation::RepeatableRead);
  e.StartStatement(*t);
  e.Scan(*t, *e.FindTable("kv")->tree, std::string(), nullptr, false,
         [&](std::string_view, std::string_view v) {
           auto row = DecodeRow(v);
           out[row[0].i] = row[1].s;
           return true;
         });
  e.Commit(t);
  return out;
}

std::string Val(int64_t k, int round) {
  return std::string(40 + k % 70, static_cast<char>('a' + (k + round) % 26));
}

class HookFs : public FileSystem {
 public:
  explicit HookFs(SimFs& base) : base_(base) {}
  std::function<void()> before_data_write;

  std::unique_ptr<File> Open(const std::string& path, bool create) override {
    auto f = base_.Open(path, create);
    if (path.size() < 7 || path.compare(path.size() - 7, 7, "data.db") != 0) return f;
    return std::make_unique<HookFile>(std::move(f), this);
  }
  bool Exists(const std::string& path) override { return base_.Exists(path); }
  void Remove(const std::string& path) override { base_.Remove(path); }
  std::vector<std::string> List(const std::string& dir) override { return base_.List(dir); }
  void MakeDirs(const std::string& dir) override { base_.MakeDirs(dir); }
  void SyncDir(const std::string& dir) override { base_.SyncDir(dir); }

 private:
  struct HookFile : File {
    HookFile(std::unique_ptr<File> f, HookFs* fs) : f_(std::move(f)), fs_(fs) {}
    size_t ReadAt(uint64_t off, char* buf, size_t n) override { return f_->ReadAt(off, buf, n); }
    void WriteAt(uint64_t off, const char* buf, size_t n) override {
      if (fs_->before_data_write) {
        auto hook = std::move(fs_->before_data_write);
        fs_->before_data_write = nullptr;
        hook();
      }
      f_->WriteAt(off, buf, n);
    }
    void Sync() override { f_->Sync(); }
    uint64_t Size() override { return f_->Size(); }
    void Truncate(uint64_t size) override { f_->Truncate(size); }
    std::unique_ptr<File> f_;
    HookFs* fs_;
  };
  SimFs& base_;
};

}

TEST(rows_changed_during_a_flush_are_redone) {
  SimFs sim(41);
  HookFs fs(sim);
  {
    Engine e(fs, Opts());
    e.Open();
    e.CreateTable(KvSchema(), false);
    Txn* t = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*t);
    Put(e, t, 1, "first", Engine::Op::Insert);
    e.Commit(t);
    fs.before_data_write = [&] {
      Txn* u = e.Begin(Isolation::ReadCommitted);
      e.StartStatement(*u);
      Put(e, u, 1, "second", Engine::Op::Update);
      e.Commit(u);
    };
    e.FlushPages(1000);
    CHECK(!fs.before_data_write);
    e.Checkpoint();
    sim.CrashNow();
  }
  sim.Restart();
  Engine e(sim, Opts());
  e.Open();
  CHECK_EQ(Dump(e)[1], std::string("second"));
}

TEST(committed_rows_survive_and_uncommitted_rows_vanish) {
  SimFs fs(31);
  std::map<int64_t, std::string> expect;
  {
    Engine e(fs, Opts());
    e.Open();
    e.CreateTable(KvSchema(), false);
    Txn* t = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*t);
    for (int64_t k = 0; k < 3000; k++) {
      Put(e, t, k, Val(k, 0), Engine::Op::Insert);
      expect[k] = Val(k, 0);
    }
    e.Commit(t);
    Txn* loser = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*loser);
    for (int64_t k = 0; k < 3000; k += 3) Put(e, loser, k, "LOSER", Engine::Op::Update);
    for (int64_t k = 3000; k < 3500; k++) Put(e, loser, k, "LOSER", Engine::Op::Insert);
    e.pool().FlushAll();
    fs.CrashNow();
  }
  fs.Restart();
  Engine e(fs, Opts());
  e.Open();
  CHECK(e.recovery().losers == 1);
  CHECK(e.recovery().undone > 1000);
  CHECK(Dump(e) == expect);
  e.FindTable("kv")->tree->Validate();
}

TEST(a_crash_during_rollback_finishes_the_rollback) {
  for (uint64_t extra : {5ull, 40ull, 120ull, 400ull}) {
    SimFs fs(32 + extra);
    std::map<int64_t, std::string> expect;
    {
      Engine e(fs, Opts());
      e.Open();
      e.CreateTable(KvSchema(), false);
      Txn* t = e.Begin(Isolation::ReadCommitted);
      e.StartStatement(*t);
      for (int64_t k = 0; k < 2000; k++) {
        Put(e, t, k, Val(k, 1), Engine::Op::Insert);
        expect[k] = Val(k, 1);
      }
      e.Commit(t);
      Txn* loser = e.Begin(Isolation::ReadCommitted);
      e.StartStatement(*loser);
      for (int64_t k = 0; k < 2000; k += 2)
        Put(e, loser, k, std::string(300, 'L'), Engine::Op::Update);
      e.log().Flush(e.log().next_lsn());
      fs.ArmCrash(fs.ops() + extra);
      try {
        e.Abort(loser);
        e.pool().FlushAll();
        e.Checkpoint();
        fs.CrashNow();
      } catch (const SimCrash&) {
      }
    }
    fs.Restart();
    Engine e(fs, Opts());
    e.Open();
    CHECK(Dump(e) == expect);
    e.FindTable("kv")->tree->Validate();
  }
}

TEST(a_crash_during_recovery_is_survivable) {
  SimFs fs(40);
  std::map<int64_t, std::string> expect;
  {
    Engine e(fs, Opts());
    e.Open();
    e.CreateTable(KvSchema(), false);
    for (int round = 0; round < 3; round++) {
      Txn* t = e.Begin(Isolation::ReadCommitted);
      e.StartStatement(*t);
      for (int64_t k = 0; k < 1500; k++) {
        Put(e, t, k, Val(k, round), round == 0 ? Engine::Op::Insert : Engine::Op::Update);
        expect[k] = Val(k, round);
      }
      e.Commit(t);
      if (round == 1) e.Checkpoint();
    }
    Txn* loser = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*loser);
    for (int64_t k = 0; k < 1500; k += 5) Put(e, loser, k, "LOSER", Engine::Op::Update);
    e.pool().FlushAll();
    fs.CrashNow();
  }
  for (uint64_t at = 1; at < 400; at += 13) {
    fs.Restart();
    fs.ArmCrash(at);
    try {
      Engine e(fs, Opts());
      e.Open();
      fs.CrashNow();
    } catch (const SimCrash&) {
    }
  }
  fs.Restart();
  Engine e(fs, Opts());
  e.Open();
  CHECK(Dump(e) == expect);
  e.FindTable("kv")->tree->Validate();
}

TEST(checkpoints_shorten_recovery_and_trim_the_log) {
  SimFs fs(41);
  {
    Engine e(fs, Opts(512));
    e.Open();
    e.CreateTable(KvSchema(), false);
    for (int round = 0; round < 4; round++) {
      Txn* t = e.Begin(Isolation::ReadCommitted);
      e.StartStatement(*t);
      for (int64_t k = 0; k < 1000; k++)
        Put(e, t, k, Val(k, round), round == 0 ? Engine::Op::Insert : Engine::Op::Update);
      e.Commit(t);
    }
    size_t before = e.log().SegmentCount();
    e.pool().FlushAll();
    e.Checkpoint();
    CHECK(e.log().SegmentCount() < before);
    Txn* t = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*t);
    Put(e, t, 1, "after checkpoint", Engine::Op::Update);
    e.Commit(t);
    fs.CrashNow();
  }
  fs.Restart();
  Engine e(fs, Opts(512));
  e.Open();
  CHECK(e.recovery().records < 10);
  CHECK(Dump(e)[1] == "after checkpoint");
}

TEST(tables_and_hidden_row_ids_survive_restarts) {
  SimFs fs(42);
  {
    Engine e(fs, Opts());
    e.Open();
    Schema s;
    s.name = "events";
    s.cols = {{"msg", ColType::Text, false, -1}};
    s.hidden_rowid = true;
    e.CreateTable(s, false);
    e.CreateTable(KvSchema(), false);
    e.DropTable("kv", false);
    auto tbl = e.FindTable("events");
    Txn* t = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*t);
    for (int i = 0; i < 50; i++) {
      std::string row = EncodeRow({Value::Text("m" + std::to_string(i))});
      e.Write(*t, *tbl->tree, EncodeKey({Value::Int(e.NextRowId(*tbl))}), Engine::Op::Insert,
              [&](const std::string*) { return std::optional<std::string>(row); });
    }
    e.Commit(t);
    fs.CrashNow();
  }
  fs.Restart();
  Engine e(fs, Opts());
  e.Open();
  CHECK(e.FindTable("kv") == nullptr);
  auto tbl = e.FindTable("events");
  CHECK(tbl != nullptr);
  CHECK_EQ(tbl->next_rowid.load(), 51);
  Txn* t = e.Begin(Isolation::ReadCommitted);
  e.StartStatement(*t);
  std::string row = EncodeRow({Value::Text("after")});
  e.Write(*t, *tbl->tree, EncodeKey({Value::Int(e.NextRowId(*tbl))}), Engine::Op::Insert,
          [&](const std::string*) { return std::optional<std::string>(row); });
  e.Commit(t);
  CHECK_EQ(tbl->tree->Validate().records, 51u);
}

TEST(recovery_twice_in_a_row_gives_the_same_answer) {
  SimFs fs(43);
  {
    Engine e(fs, Opts());
    e.Open();
    e.CreateTable(KvSchema(), false);
    Txn* t = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*t);
    for (int64_t k = 0; k < 800; k++) Put(e, t, k, Val(k, 3), Engine::Op::Insert);
    e.Commit(t);
    Txn* loser = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*loser);
    for (int64_t k = 0; k < 800; k += 7) Put(e, loser, k, "LOSER", Engine::Op::Update);
    e.pool().FlushAll();
    fs.CrashNow();
  }
  std::map<int64_t, std::string> first;
  for (int i = 0; i < 2; i++) {
    fs.Restart();
    Engine e(fs, Opts());
    e.Open();
    auto now = Dump(e);
    CHECK_EQ(now.size(), 800u);
    if (i == 0) {
      first = now;
    } else {
      CHECK(now == first);
    }
    fs.CrashNow();
  }
}
