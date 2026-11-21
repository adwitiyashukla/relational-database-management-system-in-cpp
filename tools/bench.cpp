#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client.h"
#include "engine.h"

using namespace rdbms;

namespace {

struct Args {
  std::string cmd;
  std::string host = "127.0.0.1";
  int port = 5433;
  int clients = 4;
  int seconds = 10;
  int scale = 1;
  int keys = 10;
  std::string isolation = "read committed";
  std::string dir = "bench-data";
  int txns = 20000;
  int checkpoint_mb = 0;
};

[[noreturn]] void Usage() {
  std::fprintf(
      stderr,
      "usage: rdbms-bench <command> [options]\n"
      "  load      create and fill pgbench style tables (--scale)\n"
      "  tpcb      pgbench style TPC-B transactions (--clients --seconds --isolation)\n"
      "  hot       transfers between a few hot accounts (--keys --clients --seconds --isolation)\n"
      "  commit    one-row autocommit inserts to measure group commit (--clients --seconds)\n"
      "  recovery  in-process crash and restart timing (--txns --checkpoint-mb --dir)\n"
      "common options: --host 127.0.0.1 --port 5433\n");
  std::exit(2);
}

Args Parse(int argc, char** argv) {
  if (argc < 2) Usage();
  Args a;
  a.cmd = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string k = argv[i];
    if (i + 1 >= argc) Usage();
    if (k == "--host") {
      a.host = argv[++i];
    } else if (k == "--port") {
      a.port = std::stoi(argv[++i]);
    } else if (k == "--clients") {
      a.clients = std::stoi(argv[++i]);
    } else if (k == "--seconds") {
      a.seconds = std::stoi(argv[++i]);
    } else if (k == "--scale") {
      a.scale = std::stoi(argv[++i]);
    } else if (k == "--keys") {
      a.keys = std::stoi(argv[++i]);
    } else if (k == "--isolation") {
      a.isolation = argv[++i];
    } else if (k == "--dir") {
      a.dir = argv[++i];
    } else if (k == "--txns") {
      a.txns = std::stoi(argv[++i]);
    } else if (k == "--checkpoint-mb") {
      a.checkpoint_mb = std::stoi(argv[++i]);
    } else {
      Usage();
    }
  }
  return a;
}

void Must(const PgResult& r, const std::string& what) {
  if (!r.ok()) Fail(r.code.c_str(), what + ": " + r.message);
}

struct Stats {
  std::mutex mu;
  std::vector<double> lat;
  uint64_t commits = 0;
  uint64_t retries = 0;
  uint64_t errors = 0;

  void Merge(std::vector<double>& l, uint64_t c, uint64_t r, uint64_t e) {
    std::lock_guard<std::mutex> g(mu);
    lat.insert(lat.end(), l.begin(), l.end());
    commits += c;
    retries += r;
    errors += e;
  }

  void Print(const char* name, const Args& a, double secs) {
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) {
      if (lat.empty()) return 0.0;
      return lat[std::min(lat.size() - 1,
                          static_cast<size_t>(p * static_cast<double>(lat.size())))];
    };
    double attempts = static_cast<double>(commits + retries);
    std::printf(
        "%-7s clients=%-3d isolation=%-15s tps=%9.1f  p50=%6.2fms p95=%6.2fms p99=%6.2fms  "
        "retry_rate=%5.1f%%  commits=%llu\n",
        name, a.clients, a.isolation.c_str(), static_cast<double>(commits) / secs, pct(0.50),
        pct(0.95), pct(0.99), attempts > 0 ? 100.0 * static_cast<double>(retries) / attempts : 0.0,
        static_cast<unsigned long long>(commits));
    if (errors)
      std::printf("        unexpected errors: %llu\n", static_cast<unsigned long long>(errors));
  }
};

bool Retryable(const std::string& code) {
  return code == state::kSerialization || code == state::kDeadlock;
}

void Load(const Args& a) {
  PgClient c;
  c.Connect(a.host, a.port);
  for (const char* t :
       {"pgbench_history", "pgbench_tellers", "pgbench_accounts", "pgbench_branches"})
    Must(c.Exec(std::string("drop table if exists ") + t), "drop");
  Must(c.Exec("create table pgbench_branches (bid int primary key, bbalance int, filler char(88))"),
       "create");
  Must(c.Exec("create table pgbench_tellers (tid int primary key, bid int, tbalance int, filler "
              "char(84))"),
       "create");
  Must(c.Exec("create table pgbench_accounts (aid int primary key, bid int, abalance int, filler "
              "char(84))"),
       "create");
  Must(c.Exec("create table pgbench_history (tid int, bid int, aid int, delta int, mtime "
              "timestamp, filler char(22))"),
       "create");
  uint64_t t0 = NowMicros();
  for (int b = 1; b <= a.scale; b++)
    Must(c.Exec("insert into pgbench_branches values (" + std::to_string(b) + ", 0, null)"),
         "branches");
  for (int t = 1; t <= 10 * a.scale; t++)
    Must(c.Exec("insert into pgbench_tellers values (" + std::to_string(t) + ", " +
                std::to_string((t - 1) / 10 + 1) + ", 0, null)"),
         "tellers");
  int64_t total = 100000LL * a.scale;
  for (int64_t start = 1; start <= total; start += 1000) {
    std::string sql = "insert into pgbench_accounts values ";
    for (int64_t aid = start; aid < start + 1000 && aid <= total; aid++) {
      if (aid != start) sql += ", ";
      sql +=
          "(" + std::to_string(aid) + ", " + std::to_string((aid - 1) / 100000 + 1) + ", 0, null)";
    }
    Must(c.Exec(sql), "accounts");
  }
  Must(c.Exec("checkpoint"), "checkpoint");
  std::printf("loaded scale %d (%lld accounts) in %.1f s\n", a.scale, static_cast<long long>(total),
              static_cast<double>(NowMicros() - t0) / 1e6);
}

template <class Fn>
void RunClients(const Args& a, Stats& stats, Fn body) {
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < a.clients; i++) {
    threads.emplace_back([&, i] {
      PgClient c;
      c.Connect(a.host, a.port);
      Rng rng(NowMicros() + static_cast<uint64_t>(i) * 7777);
      std::vector<double> lat;
      uint64_t commits = 0, retries = 0, errors = 0;
      while (!stop.load()) {
        uint64_t t0 = NowMicros();
        for (;;) {
          std::string code = body(c, rng, i);
          if (code.empty()) {
            commits++;
            break;
          }
          if (c.status() != 'I') c.Exec("rollback");
          if (!Retryable(code)) {
            errors++;
            break;
          }
          retries++;
          if (stop.load()) break;
        }
        lat.push_back(static_cast<double>(NowMicros() - t0) / 1000.0);
      }
      stats.Merge(lat, commits, retries, errors);
    });
  }
  SleepMicros(static_cast<uint64_t>(a.seconds) * 1000000);
  stop = true;
  for (auto& t : threads) t.join();
}

std::string Step(PgClient& c, const std::string& sql) {
  PgResult r = c.Exec(sql);
  return r.ok() ? std::string() : r.code;
}

void Tpcb(const Args& a) {
  PgClient probe;
  probe.Connect(a.host, a.port);
  PgResult n = probe.Exec("select count(*) from pgbench_branches");
  Must(n, "count branches");
  int scale = std::max(1, std::stoi(*n.rows[0][0]));
  probe.Close();
  Stats stats;
  std::string begin = "begin isolation level " + a.isolation;
  RunClients(a, stats, [&](PgClient& c, Rng& rng, int) -> std::string {
    std::string aid = std::to_string(1 + rng.Uniform(100000ull * static_cast<uint64_t>(scale)));
    std::string bid = std::to_string(1 + rng.Uniform(static_cast<uint64_t>(scale)));
    std::string tid = std::to_string(1 + rng.Uniform(10ull * static_cast<uint64_t>(scale)));
    std::string delta = std::to_string(static_cast<int64_t>(rng.Uniform(10001)) - 5000);
    for (const std::string& sql :
         {begin,
          "update pgbench_accounts set abalance = abalance + " + delta + " where aid = " + aid,
          "select abalance from pgbench_accounts where aid = " + aid,
          "update pgbench_tellers set tbalance = tbalance + " + delta + " where tid = " + tid,
          "update pgbench_branches set bbalance = bbalance + " + delta + " where bid = " + bid,
          "insert into pgbench_history (tid, bid, aid, delta, mtime) values (" + tid + ", " + bid +
              ", " + aid + ", " + delta + ", current_timestamp)",
          std::string("end")}) {
      std::string code = Step(c, sql);
      if (!code.empty()) return code;
    }
    return std::string();
  });
  stats.Print("tpcb", a, a.seconds);
}

void Hot(const Args& a) {
  {
    PgClient c;
    c.Connect(a.host, a.port);
    Must(c.Exec("drop table if exists hot"), "drop");
    Must(c.Exec("create table hot (id int primary key, bal bigint)"), "create");
    std::string sql = "insert into hot values ";
    for (int i = 0; i < a.keys; i++)
      sql += (i ? ", " : "") + std::string("(") + std::to_string(i) + ", 1000000)";
    Must(c.Exec(sql), "insert");
  }
  Stats stats;
  std::string begin = "begin isolation level " + a.isolation;
  RunClients(a, stats, [&](PgClient& c, Rng& rng, int) -> std::string {
    uint64_t x = rng.Uniform(static_cast<uint64_t>(a.keys));
    uint64_t y = rng.Uniform(static_cast<uint64_t>(a.keys) - 1);
    if (y >= x) y++;
    std::string from = std::to_string(x), to = std::to_string(y);
    for (const std::string& sql :
         {begin, "select bal from hot where id = " + from,
          "update hot set bal = bal - 1 where id = " + from,
          "update hot set bal = bal + 1 where id = " + to, std::string("commit")}) {
      std::string code = Step(c, sql);
      if (!code.empty()) return code;
    }
    return std::string();
  });
  stats.Print("hot", a, a.seconds);
  PgClient c;
  c.Connect(a.host, a.port);
  PgResult r = c.Exec("select sum(bal) from hot");
  Must(r, "sum");
  if (std::stoll(*r.rows[0][0]) != 1000000LL * a.keys) {
    std::printf("        money was created or destroyed\n");
    std::exit(1);
  }
}

void Commit(const Args& a) {
  {
    PgClient c;
    c.Connect(a.host, a.port);
    Must(c.Exec("drop table if exists commits"), "drop");
    Must(c.Exec("create table commits (id bigint primary key, payload text)"), "create");
  }
  Stats stats;
  std::vector<uint64_t> seq(static_cast<size_t>(a.clients), 0);
  RunClients(a, stats, [&](PgClient& c, Rng&, int i) -> std::string {
    uint64_t id = static_cast<uint64_t>(i) * 1000000000ull + seq[static_cast<size_t>(i)]++;
    return Step(c, "insert into commits values (" + std::to_string(id) + ", 'x')");
  });
  stats.Print("commit", a, a.seconds);
}

void Recovery(const Args& a) {
  auto fs = NewDiskFs();
  for (auto& name : fs->List(a.dir + "/wal")) fs->Remove(a.dir + "/wal/" + name);
  for (const char* f : {"data.db", "doublewrite", "control"}) {
    if (fs->Exists(a.dir + "/" + f)) fs->Remove(a.dir + "/" + f);
  }
  EngineOptions opt;
  opt.dir = a.dir;
  opt.pool_pages = 16384;
  opt.wal.fsync = false;
  opt.background = false;
  uint64_t every = a.checkpoint_mb > 0 ? static_cast<uint64_t>(a.checkpoint_mb) << 20 : ~0ull;
  uint64_t written = 0;
  {
    Engine e(*fs, opt);
    e.Open();
    Schema s;
    s.name = "acct";
    s.cols = {{"id", ColType::BigInt, true, -1},
              {"bal", ColType::BigInt, false, -1},
              {"note", ColType::Text, false, -1}};
    s.pk = {0};
    e.CreateTable(s, false);
    auto tbl = e.FindTable("acct");
    Txn* t = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*t);
    for (int64_t k = 0; k < 100000; k++) {
      std::string row =
          EncodeRow({Value::Int(k), Value::Int(0), Value::Text(std::string(60, 'n'))});
      e.Write(*t, *tbl->tree, EncodeKey({Value::Int(k)}), Engine::Op::Insert,
              [&](const std::string*) { return std::optional<std::string>(row); });
    }
    e.Commit(t);
    e.pool().FlushAll();
    e.Checkpoint();
    Rng rng(5);
    uint64_t t0 = NowMicros();
    Lsn last = e.log().next_lsn();
    for (int i = 0; i < a.txns; i++) {
      if (e.log().next_lsn() - last >= every) {
        e.FuzzyCheckpoint();
        last = e.log().next_lsn();
      }
      Txn* x = e.Begin(Isolation::ReadCommitted);
      e.StartStatement(*x);
      for (int j = 0; j < 4; j++) {
        int64_t k = static_cast<int64_t>(rng.Uniform(100000));
        e.Write(*x, *tbl->tree, EncodeKey({Value::Int(k)}), Engine::Op::Update,
                [&](const std::string* latest) {
                  std::vector<Value> r = DecodeRow(*latest);
                  r[1].i += 1;
                  return std::optional<std::string>(EncodeRow(r));
                });
      }
      e.Commit(x);
    }
    written = e.stats().wal_bytes;
    std::printf("workload: %d transactions in %.1f s, %.1f MB of log, %llu checkpoints\n", a.txns,
                static_cast<double>(NowMicros() - t0) / 1e6,
                static_cast<double>(written) / 1048576.0,
                static_cast<unsigned long long>(e.stats().checkpoints));
    e.log().Flush(e.log().next_lsn());
    e.Halt();
  }
  Engine e(*fs, opt);
  uint64_t t0 = NowMicros();
  e.Open();
  double ms = static_cast<double>(NowMicros() - t0) / 1000.0;
  const RecoveryStats& rs = e.recovery();
  std::printf(
      "recovery checkpoint_mb=%d: %.1f ms, %.1f MB of log replayed, %llu records after the "
      "checkpoint, %llu changes redone\n",
      a.checkpoint_mb, ms, static_cast<double>(rs.end - rs.redo_start) / 1048576.0,
      static_cast<unsigned long long>(rs.records), static_cast<unsigned long long>(rs.redone));
}

}

int main(int argc, char** argv) {
  SetLogQuiet(true);
  Args a = Parse(argc, argv);
  try {
    if (a.cmd == "load") {
      Load(a);
    } else if (a.cmd == "tpcb") {
      Tpcb(a);
    } else if (a.cmd == "hot") {
      Hot(a);
    } else if (a.cmd == "commit") {
      Commit(a);
    } else if (a.cmd == "recovery") {
      Recovery(a);
    } else {
      Usage();
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
