#include <atomic>
#include <thread>

#include "engine.h"
#include "test.h"

using namespace rdbms;

namespace {

constexpr Isolation RC = Isolation::ReadCommitted;
constexpr Isolation RR = Isolation::RepeatableRead;
constexpr Isolation SER = Isolation::Serializable;

struct Db {
  SimFs fs{21};
  std::unique_ptr<Engine> e;
  std::shared_ptr<Table> t;

  explicit Db(uint32_t lock_ms = 3000) {
    EngineOptions o;
    o.dir = "db";
    o.pool_pages = 256;
    o.background = false;
    o.lock_timeout_ms = lock_ms;
    e = std::make_unique<Engine>(fs, o);
    e->Open();
    Schema s;
    s.name = "kv";
    s.cols = {{"k", ColType::BigInt, true, -1}, {"v", ColType::BigInt, false, -1}};
    s.pk = {0};
    e->CreateTable(s, false);
    t = e->FindTable("kv");
  }

  Txn* Begin(Isolation iso) {
    Txn* x = e->Begin(iso);
    e->StartStatement(*x);
    return x;
  }

  std::optional<int64_t> Get(Txn* x, int64_t k) {
    std::string v;
    if (!e->Read(*x, *t->tree, EncodeKey({Value::Int(k)}), &v)) return std::nullopt;
    return DecodeRow(v)[1].i;
  }

  void Insert(Txn* x, int64_t k, int64_t v) {
    std::string row = EncodeRow({Value::Int(k), Value::Int(v)});
    e->Write(*x, *t->tree, EncodeKey({Value::Int(k)}), Engine::Op::Insert,
             [&](const std::string*) { return std::optional<std::string>(row); });
  }

  bool Set(Txn* x, int64_t k, int64_t v) {
    std::string row = EncodeRow({Value::Int(k), Value::Int(v)});
    return e->Write(*x, *t->tree, EncodeKey({Value::Int(k)}), Engine::Op::Update,
                    [&](const std::string*) { return std::optional<std::string>(row); });
  }

  bool Add(Txn* x, int64_t k, int64_t d) {
    return e->Write(*x, *t->tree, EncodeKey({Value::Int(k)}), Engine::Op::Update,
                    [&](const std::string* latest) {
                      std::vector<Value> r = DecodeRow(*latest);
                      r[1].i += d;
                      return std::optional<std::string>(EncodeRow(r));
                    });
  }

  bool Del(Txn* x, int64_t k) {
    return e->Write(*x, *t->tree, EncodeKey({Value::Int(k)}), Engine::Op::Delete,
                    [](const std::string*) { return std::optional<std::string>(std::string()); });
  }

  bool Lock(Txn* x, int64_t k) {
    return e->Write(*x, *t->tree, EncodeKey({Value::Int(k)}), Engine::Op::Lock,
                    [](const std::string* l) { return std::optional<std::string>(*l); });
  }

  int64_t Count(Txn* x, int64_t lo, int64_t hi) {
    int64_t n = 0;
    std::string h = EncodeKey({Value::Int(hi)});
    e->Scan(*x, *t->tree, EncodeKey({Value::Int(lo)}), &h, true,
            [&](std::string_view, std::string_view) {
              n++;
              return true;
            });
    return n;
  }

  void Load(std::initializer_list<std::pair<int64_t, int64_t>> rows) {
    Txn* x = Begin(RC);
    for (auto& [k, v] : rows) Insert(x, k, v);
    e->Commit(x);
  }
};

}

TEST(read_committed_sees_new_commits_repeatable_read_does_not) {
  Db db;
  db.Load({{1, 10}});
  Txn* rr = db.Begin(RR);
  Txn* rc = db.Begin(RC);
  CHECK_EQ(*db.Get(rr, 1), 10);
  CHECK_EQ(*db.Get(rc, 1), 10);
  Txn* w = db.Begin(RC);
  db.Set(w, 1, 20);
  CHECK_EQ(*db.Get(rr, 1), 10);
  CHECK_EQ(*db.Get(w, 1), 20);
  db.e->Commit(w);
  CHECK_EQ(*db.Get(rr, 1), 10);
  db.e->StartStatement(*rc);
  CHECK_EQ(*db.Get(rc, 1), 20);
  db.e->Commit(rr);
  db.e->Commit(rc);
}

TEST(lost_update_is_refused_at_repeatable_read) {
  Db db;
  db.Load({{1, 100}});
  Txn* a = db.Begin(RR);
  Txn* b = db.Begin(RR);
  CHECK_EQ(*db.Get(a, 1), 100);
  CHECK_EQ(*db.Get(b, 1), 100);
  db.Add(b, 1, 5);
  db.e->Commit(b);
  CHECK_CODE(db.Add(a, 1, 7), state::kSerialization);
  db.e->Abort(a);
  Txn* c = db.Begin(RC);
  CHECK_EQ(*db.Get(c, 1), 105);
  db.e->Commit(c);
}

TEST(read_committed_update_waits_then_uses_the_latest_row) {
  Db db;
  db.Load({{1, 100}});
  Txn* a = db.Begin(RC);
  db.Add(a, 1, 10);
  std::atomic<bool> done{false};
  std::thread other([&] {
    Txn* b = db.Begin(RC);
    db.Add(b, 1, 1);
    db.e->Commit(b);
    done = true;
  });
  SleepMicros(50000);
  CHECK(!done.load());
  db.e->Commit(a);
  other.join();
  Txn* c = db.Begin(RC);
  CHECK_EQ(*db.Get(c, 1), 111);
  db.e->Commit(c);
}

TEST(write_skew_passes_snapshot_isolation_but_not_serializable) {
  for (Isolation iso : {RR, SER}) {
    Db db;
    db.Load({{1, 1}, {2, 1}});
    Txn* a = db.Begin(iso);
    Txn* b = db.Begin(iso);
    CHECK_EQ(*db.Get(a, 1) + *db.Get(a, 2), 2);
    CHECK_EQ(*db.Get(b, 1) + *db.Get(b, 2), 2);
    db.Set(a, 1, 0);
    int failures = 0;
    try {
      db.Set(b, 2, 0);
    } catch (const DbError& e) {
      CHECK_EQ(e.code(), std::string(state::kSerialization));
      failures++;
      db.e->Abort(b);
      b = nullptr;
    }
    db.e->Commit(a);
    if (b) {
      try {
        db.e->Commit(b);
      } catch (const DbError& e) {
        CHECK_EQ(e.code(), std::string(state::kSerialization));
        failures++;
      }
    }
    Txn* c = db.Begin(RC);
    int64_t on_call = *db.Get(c, 1) + *db.Get(c, 2);
    db.e->Commit(c);
    if (iso == RR) {
      CHECK_EQ(failures, 0);
      CHECK_EQ(on_call, 0);
    } else {
      CHECK_EQ(failures, 1);
      CHECK_EQ(on_call, 1);
    }
  }
}

TEST(read_only_anomaly_is_caught_at_serializable) {
  Db db;
  db.Load({{1, 0}, {2, 0}});
  Txn* t2 = db.Begin(SER);
  int64_t x = *db.Get(t2, 1), y = *db.Get(t2, 2);
  Txn* t1 = db.Begin(SER);
  db.Add(t1, 2, 20);
  db.e->Commit(t1);
  Txn* t3 = db.Begin(SER);
  CHECK_EQ(*db.Get(t3, 1), 0);
  CHECK_EQ(*db.Get(t3, 2), 20);
  db.e->Commit(t3);
  int64_t penalty = x + y < 10 ? 11 : 10;
  bool failed = false;
  bool in_commit = false;
  try {
    db.Add(t2, 1, -penalty);
    in_commit = true;
    db.e->Commit(t2);
  } catch (const DbError& e) {
    CHECK_EQ(e.code(), std::string(state::kSerialization));
    failed = true;
    if (!in_commit) db.e->Abort(t2);
  }
  CHECK(failed);
}

TEST(phantoms_are_caught_at_serializable) {
  for (Isolation iso : {RR, SER}) {
    Db db;
    db.Load({{100, 1}});
    Txn* a = db.Begin(iso);
    Txn* b = db.Begin(iso);
    CHECK_EQ(db.Count(a, 1, 10), 0);
    CHECK_EQ(db.Count(b, 11, 20), 0);
    db.Insert(a, 15, 1);
    int failures = 0;
    bool in_commit = false;
    try {
      db.Insert(b, 5, 1);
      in_commit = true;
      db.e->Commit(b);
    } catch (const DbError& e) {
      CHECK_EQ(e.code(), std::string(state::kSerialization));
      failures++;
      if (!in_commit) db.e->Abort(b);
    }
    try {
      db.e->Commit(a);
    } catch (const DbError& e) {
      CHECK_EQ(e.code(), std::string(state::kSerialization));
      failures++;
    }
    CHECK_EQ(failures, iso == SER ? 1 : 0);
  }
}

TEST(pivot_is_doomed_when_the_third_transaction_commits_first) {
  Db db;
  db.Load({{1, 0}, {2, 0}});
  Txn* t1 = db.Begin(SER);
  Txn* t2 = db.Begin(SER);
  Txn* t3 = db.Begin(SER);
  db.Get(t1, 1);
  db.Set(t2, 1, 5);
  db.Get(t2, 2);
  db.Set(t3, 2, 7);
  db.e->Commit(t3);
  CHECK(t2->doomed.load());
  CHECK_CODE(db.e->Commit(t2), state::kSerialization);
  db.e->Commit(t1);
}

TEST(committed_pivot_keeps_its_conflict_after_gc) {
  Db db;
  db.Load({{1, 0}, {2, 0}});
  Txn* t2 = db.Begin(SER);
  CHECK_EQ(*db.Get(t2, 2), 0);
  Txn* t3 = db.Begin(SER);
  db.Set(t3, 2, 5);
  db.e->Commit(t3);
  Txn* t1 = db.Begin(SER);
  CHECK_EQ(*db.Get(t1, 2), 5);
  db.Set(t2, 1, 7);
  db.e->Commit(t2);
  db.e->RunGc();
  CHECK_CODE(db.Get(t1, 1), state::kSerialization);
  db.e->Abort(t1);
}

TEST(deadlocks_are_detected) {
  Db db;
  db.Load({{1, 0}, {2, 0}});
  Txn* a = db.Begin(RC);
  Txn* b = db.Begin(RC);
  db.Lock(a, 1);
  db.Lock(b, 2);
  std::atomic<int> outcome{0};
  std::thread ta([&] {
    try {
      db.Lock(a, 2);
      outcome += 1;
    } catch (const DbError& e) {
      outcome += e.code() == state::kDeadlock ? 10 : 100;
    }
  });
  SleepMicros(50000);
  bool b_dead = false;
  try {
    db.Lock(b, 1);
  } catch (const DbError& e) {
    CHECK_EQ(e.code(), std::string(state::kDeadlock));
    b_dead = true;
    db.e->Abort(b);
  }
  ta.join();
  CHECK(b_dead);
  CHECK_EQ(outcome.load(), 1);
  db.e->Commit(a);
  CHECK_EQ(db.e->stats().deadlocks, 1u);
}

TEST(lock_waits_time_out) {
  Db db(150);
  db.Load({{1, 0}});
  Txn* a = db.Begin(RC);
  db.Lock(a, 1);
  Txn* b = db.Begin(RC);
  uint64_t t0 = NowMicros();
  CHECK_CODE(db.Lock(b, 1), state::kLockTimeout);
  CHECK(NowMicros() - t0 >= 140000);
  db.e->Abort(b);
  db.e->Commit(a);
}

TEST(rollback_restores_rows_and_drops_versions) {
  Db db;
  db.Load({{1, 1}, {2, 2}});
  Txn* reader = db.Begin(RR);
  CHECK_EQ(*db.Get(reader, 1), 1);
  Txn* w = db.Begin(RC);
  db.Set(w, 1, 50);
  db.Del(w, 2);
  db.Insert(w, 3, 3);
  CHECK(db.e->versions().size() == 2);
  CHECK_EQ(*db.Get(reader, 1), 1);
  CHECK(db.Get(reader, 2).has_value());
  CHECK(!db.Get(reader, 3).has_value());
  db.e->Abort(w);
  CHECK_EQ(db.e->versions().size(), 0u);
  Txn* c = db.Begin(RC);
  CHECK_EQ(*db.Get(c, 1), 1);
  CHECK_EQ(*db.Get(c, 2), 2);
  CHECK(!db.Get(c, 3).has_value());
  db.e->Commit(c);
  db.e->Commit(reader);
}

TEST(garbage_collection_waits_for_old_snapshots) {
  Db db;
  db.Load({{1, 0}});
  Txn* old = db.Begin(RR);
  CHECK_EQ(*db.Get(old, 1), 0);
  for (int i = 1; i <= 20; i++) {
    Txn* w = db.Begin(RC);
    db.Set(w, 1, i);
    db.e->Commit(w);
  }
  db.e->RunGc();
  CHECK_EQ(db.e->versions().size(), 20u);
  CHECK_EQ(*db.Get(old, 1), 0);
  db.e->Commit(old);
  db.e->RunGc();
  CHECK_EQ(db.e->versions().size(), 0u);
}

TEST(purge_removes_deleted_rows_once_nobody_can_see_them) {
  Db db;
  db.Load({{1, 1}, {2, 2}, {3, 3}});
  Txn* old = db.Begin(RR);
  CHECK_EQ(db.Count(old, 0, 10), 3);
  Txn* w = db.Begin(RC);
  db.Del(w, 2);
  db.e->Commit(w);
  db.e->RunGc();
  CHECK_EQ(db.Count(old, 0, 10), 3);
  int raw = 0;
  db.t->tree->Scan(std::string(), nullptr, false, [&](const LeafRec&) {
    raw++;
    return true;
  });
  CHECK_EQ(raw, 3);
  db.e->Commit(old);
  db.e->RunGc();
  raw = 0;
  db.t->tree->Scan(std::string(), nullptr, false, [&](const LeafRec&) {
    raw++;
    return true;
  });
  CHECK_EQ(raw, 2);
  CHECK(db.e->stats().purged >= 1u);
}

TEST(inserts_respect_keys_and_snapshots) {
  Db db;
  db.Load({{1, 1}});
  Txn* a = db.Begin(RC);
  CHECK_CODE(db.Insert(a, 1, 9), state::kUnique);
  db.e->Abort(a);
  Txn* rr = db.Begin(RR);
  CHECK_EQ(*db.Get(rr, 1), 1);
  Txn* d = db.Begin(RC);
  db.Del(d, 1);
  db.e->Commit(d);
  CHECK_CODE(db.Insert(rr, 1, 5), state::kSerialization);
  db.e->Abort(rr);
  Txn* rc = db.Begin(RC);
  db.Insert(rc, 1, 5);
  db.e->Commit(rc);
  Txn* c = db.Begin(RC);
  CHECK_EQ(*db.Get(c, 1), 5);
  db.e->Commit(c);
}

TEST(many_threads_moving_money_keep_the_total) {
  Db db(5000);
  {
    Txn* x = db.Begin(RC);
    for (int i = 0; i < 20; i++) db.Insert(x, i, 1000);
    db.e->Commit(x);
  }
  std::atomic<int> commits{0}, retries{0}, bad{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 6; w++) {
    threads.emplace_back([&, w] {
      Rng rng(static_cast<uint64_t>(w) * 31 + 7);
      Isolation iso = w % 3 == 0 ? RC : (w % 3 == 1 ? RR : SER);
      for (int i = 0; i < 300; i++) {
        int64_t from = static_cast<int64_t>(rng.Uniform(20));
        int64_t to = static_cast<int64_t>(rng.Uniform(20));
        if (from == to) continue;
        Txn* x = db.e->Begin(iso);
        bool in_commit = false;
        try {
          db.e->StartStatement(*x);
          db.Add(x, from, -7);
          db.e->StartStatement(*x);
          db.Add(x, to, 7);
          in_commit = true;
          db.e->Commit(x);
          commits++;
        } catch (const DbError& e) {
          if (e.code() != state::kSerialization && e.code() != state::kDeadlock) bad++;
          if (!in_commit) db.e->Abort(x);
          retries++;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK_EQ(bad.load(), 0);
  Txn* c = db.Begin(RR);
  int64_t total = 0;
  for (int i = 0; i < 20; i++) total += *db.Get(c, i);
  db.e->Commit(c);
  CHECK_EQ(total, 20000);
  CHECK(commits.load() > 1000);
}
