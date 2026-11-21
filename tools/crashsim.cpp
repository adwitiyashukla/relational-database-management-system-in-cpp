#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "engine.h"

using namespace rdbms;

namespace {

constexpr int kSlots = 3;
constexpr int kAccounts = 30;
constexpr int64_t kStart = 1000;

struct Effects {
  std::map<int64_t, std::array<int64_t, 3>> ledger;
  std::map<int64_t, std::optional<std::string>> scratch;
};

struct Model {
  std::map<int64_t, std::array<int64_t, 3>> ledger;
  std::map<int64_t, std::string> scratch;

  void Apply(const Effects& e) {
    for (auto& [k, v] : e.ledger) ledger[k] = v;
    for (auto& [k, v] : e.scratch) {
      if (v) {
        scratch[k] = *v;
      } else {
        scratch.erase(k);
      }
    }
  }
  bool operator==(const Model& o) const { return ledger == o.ledger && scratch == o.scratch; }
};

struct Open {
  Txn* t = nullptr;
  Effects fx;
  std::map<int64_t, std::optional<std::string>> view;
};

struct Totals {
  uint64_t seeds = 0, commits = 0, crashes = 0, recovery_crashes = 0, losers = 0, redone = 0;
  uint64_t torn = 0, dropped = 0, kept = 0, lost_files = 0, failures = 0, torn_pages = 0;
};

EngineOptions Options(Rng& rng) {
  EngineOptions o;
  o.dir = "db";
  o.pool_pages = 20 + rng.Uniform(60);
  o.background = false;
  o.wal.segment_size = uint64_t{1} << (13 + rng.Uniform(4));
  o.lock_timeout_ms = 200;
  return o;
}

Schema Bank() {
  Schema s;
  s.name = "bank";
  s.cols = {{"id", ColType::BigInt, true, -1}, {"bal", ColType::BigInt, false, -1}};
  s.pk = {0};
  return s;
}

Schema Ledger() {
  Schema s;
  s.name = "ledger";
  s.cols = {{"id", ColType::BigInt, true, -1},
            {"src", ColType::BigInt, false, -1},
            {"dst", ColType::BigInt, false, -1},
            {"amt", ColType::BigInt, false, -1}};
  s.pk = {0};
  return s;
}

Schema Scratch() {
  Schema s;
  s.name = "scratch";
  s.cols = {{"id", ColType::BigInt, true, -1}, {"v", ColType::Text, false, -1}};
  s.pk = {0};
  return s;
}

std::string K(int64_t k) { return EncodeKey({Value::Int(k)}); }

int64_t ReadBal(Engine& e, Txn& t, int64_t id) {
  std::string v;
  if (!e.Read(t, *e.FindTable("bank")->tree, K(id), &v)) Fail(state::kInternal, "account missing");
  return DecodeRow(v)[1].i;
}

void Move(Engine& e, Txn& t, int64_t id, int64_t delta) {
  e.Write(t, *e.FindTable("bank")->tree, K(id), Engine::Op::Update, [&](const std::string* latest) {
    std::vector<Value> r = DecodeRow(*latest);
    r[1].i += delta;
    return std::optional<std::string>(EncodeRow(r));
  });
}

struct State {
  Model model;
  std::map<int64_t, int64_t> bal;
  std::string error;
};

State Observe(Engine& e) {
  State s;
  Txn* t = e.Begin(Isolation::RepeatableRead);
  e.StartStatement(*t);
  e.Scan(*t, *e.FindTable("bank")->tree, std::string(), nullptr, false,
         [&](std::string_view, std::string_view v) {
           auto r = DecodeRow(v);
           s.bal[r[0].i] = r[1].i;
           return true;
         });
  e.Scan(*t, *e.FindTable("ledger")->tree, std::string(), nullptr, false,
         [&](std::string_view, std::string_view v) {
           auto r = DecodeRow(v);
           s.model.ledger[r[0].i] = {r[1].i, r[2].i, r[3].i};
           return true;
         });
  e.Scan(*t, *e.FindTable("scratch")->tree, std::string(), nullptr, false,
         [&](std::string_view, std::string_view v) {
           auto r = DecodeRow(v);
           s.model.scratch[r[0].i] = r[1].s;
           return true;
         });
  e.Commit(t);
  for (auto& tbl : e.Tables()) tbl->tree->Validate();
  return s;
}

std::string Check(const State& s, const Model& committed, const Effects* doubt) {
  Model with = committed;
  if (doubt) with.Apply(*doubt);
  if (!(s.model == committed) && !(doubt && s.model == with)) {
    for (auto& [k, v] : committed.ledger) {
      if (!s.model.ledger.count(k))
        return "acknowledged transfer " + std::to_string(k) + " is missing";
    }
    for (auto& [k, v] : s.model.ledger) {
      if (!with.ledger.count(k))
        return "transfer " + std::to_string(k) + " was never committed but is present";
    }
    return "scratch table does not match any allowed state";
  }
  std::map<int64_t, int64_t> want;
  for (int a = 0; a < kAccounts; a++) want[a] = kStart;
  for (auto& [k, v] : s.model.ledger) {
    want[v[0]] -= v[2];
    want[v[1]] += v[2];
  }
  if (want != s.bal) return "balances do not match the ledger";
  return std::string();
}

bool RunSeed(uint64_t seed, Totals& tot, bool verbose) {
  Rng rng(seed * 7919 + 13);
  SimFs fs(seed);
  EngineOptions opt = Options(rng);
  Model committed;
  std::optional<Effects> doubt;
  int64_t next_ledger = 1;
  uint64_t crash_op = 0;
  {
    Engine e(fs, opt);
    e.Open();
    e.CreateTable(Bank(), false);
    e.CreateTable(Ledger(), false);
    e.CreateTable(Scratch(), false);
    Txn* t = e.Begin(Isolation::ReadCommitted);
    e.StartStatement(*t);
    for (int64_t a = 0; a < kAccounts; a++) {
      std::string row = EncodeRow({Value::Int(a), Value::Int(kStart)});
      e.Write(*t, *e.FindTable("bank")->tree, K(a), Engine::Op::Insert,
              [&](const std::string*) { return std::optional<std::string>(row); });
    }
    e.Commit(t);
    e.Checkpoint();
    fs.ArmCrash(fs.ops() + 1 + rng.Uniform(1500));
    std::array<Open, kSlots> open;
    try {
      for (int step = 0; step < 100000; step++) {
        uint64_t pick = rng.Uniform(100);
        if (pick < 3) {
          e.Checkpoint();
          continue;
        }
        if (pick < 8) {
          e.FlushPages(1 + rng.Uniform(8));
          continue;
        }
        if (pick < 11) {
          e.RunGc();
          continue;
        }
        int s = static_cast<int>(rng.Uniform(kSlots));
        Open& o = open[static_cast<size_t>(s)];
        if (!o.t) {
          Isolation iso = static_cast<Isolation>(rng.Uniform(3));
          o.t = e.Begin(iso);
          o.fx = Effects();
          o.view.clear();
          continue;
        }
        uint64_t act = rng.Uniform(10);
        if (act < 5) {
          e.StartStatement(*o.t);
          int64_t base = s * (kAccounts / kSlots);
          int64_t a = base + static_cast<int64_t>(rng.Uniform(kAccounts / kSlots));
          int64_t b = base + static_cast<int64_t>(rng.Uniform(kAccounts / kSlots));
          if (a == b) continue;
          int64_t amt = 1 + static_cast<int64_t>(rng.Uniform(50));
          ReadBal(e, *o.t, a);
          Move(e, *o.t, a, -amt);
          Move(e, *o.t, b, amt);
          int64_t id = next_ledger++;
          std::string row =
              EncodeRow({Value::Int(id), Value::Int(a), Value::Int(b), Value::Int(amt)});
          e.Write(*o.t, *e.FindTable("ledger")->tree, K(id), Engine::Op::Insert,
                  [&](const std::string*) { return std::optional<std::string>(row); });
          o.fx.ledger[id] = {a, b, amt};
        } else if (act < 7) {
          e.StartStatement(*o.t);
          int64_t k = s * 100000 + static_cast<int64_t>(rng.Uniform(300));
          std::optional<std::string> cur;
          auto vit = o.view.find(k);
          if (vit != o.view.end()) {
            cur = vit->second;
          } else {
            auto cit = committed.scratch.find(k);
            if (cit != committed.scratch.end()) cur = cit->second;
          }
          auto tree = e.FindTable("scratch")->tree.get();
          if (cur && rng.Chance(0.3)) {
            e.Write(*o.t, *tree, K(k), Engine::Op::Delete,
                    [](const std::string*) { return std::optional<std::string>(std::string()); });
            o.view[k] = std::nullopt;
            o.fx.scratch[k] = std::nullopt;
          } else {
            char c = static_cast<char>('a' + rng.Uniform(26));
            std::string v(1 + rng.Uniform(rng.Chance(0.1) ? 1500 : 120), c);
            std::string row = EncodeRow({Value::Int(k), Value::Text(v)});
            e.Write(*o.t, *tree, K(k), cur ? Engine::Op::Update : Engine::Op::Insert,
                    [&](const std::string*) { return std::optional<std::string>(row); });
            o.view[k] = v;
            o.fx.scratch[k] = v;
          }
        } else if (act < 9) {
          Txn* t2 = o.t;
          o.t = nullptr;
          try {
            e.Commit(t2);
          } catch (...) {
            doubt = o.fx;
            throw;
          }
          committed.Apply(o.fx);
          tot.commits++;
        } else {
          Txn* t2 = o.t;
          o.t = nullptr;
          e.Abort(t2);
        }
      }
      fs.CrashNow();
    } catch (const SimCrash&) {
      crash_op = fs.ops();
    } catch (const DbError& err) {
      if (!fs.crashed()) {
        std::printf("seed %llu: unexpected error before the crash: %s\n",
                    static_cast<unsigned long long>(seed), err.what());
        return false;
      }
      crash_op = fs.ops();
    }
  }
  tot.crashes++;
  fs.Restart();
  std::string problem;
  if (rng.Chance(0.4)) {
    fs.ArmCrash(1 + rng.Uniform(12));
    try {
      Engine e(fs, opt);
      e.Open();
      fs.CrashNow();
    } catch (const SimCrash&) {
      tot.recovery_crashes++;
    } catch (const std::exception& err) {
      problem = std::string("recovery failed: ") + err.what();
    }
    fs.Restart();
  }
  State seen;
  RecoveryStats rec;
  if (problem.empty()) {
    try {
      Engine e(fs, opt);
      e.Open();
      rec = e.recovery();
      tot.losers += e.recovery().losers;
      tot.redone += e.recovery().redone;
      tot.torn_pages += static_cast<uint64_t>(e.recovery().torn_pages);
      seen = Observe(e);
      problem = Check(seen, committed, doubt ? &*doubt : nullptr);
      if (problem.empty()) {
        Txn* t = e.Begin(Isolation::Serializable);
        e.StartStatement(*t);
        Move(e, *t, 0, -1);
        Move(e, *t, 1, 1);
        int64_t id = 1000000000;
        std::string row = EncodeRow({Value::Int(id), Value::Int(0), Value::Int(1), Value::Int(1)});
        e.Write(*t, *e.FindTable("ledger")->tree, K(id), Engine::Op::Insert,
                [&](const std::string*) { return std::optional<std::string>(row); });
        e.Commit(t);
      }
    } catch (const std::exception& err) {
      problem = std::string("recovery failed: ") + err.what();
    }
  }
  if (problem.empty()) {
    try {
      Engine e(fs, opt);
      e.Open();
      State again = Observe(e);
      if (again.model.ledger.size() != seen.model.ledger.size() + 1)
        problem = "clean restart lost the post-recovery transaction";
    } catch (const std::exception& err) {
      problem = std::string("clean restart failed: ") + err.what();
    }
  }
  SimFs::Stats st = fs.stats();
  tot.torn += st.torn;
  tot.dropped += st.dropped;
  tot.kept += st.kept;
  tot.lost_files += st.lost_files;
  tot.seeds++;
  if (!problem.empty()) {
    tot.failures++;
    std::printf("seed %llu FAILED: %s\n", static_cast<unsigned long long>(seed), problem.c_str());
    std::string ids;
    if (doubt) {
      for (auto& [k, v] : doubt->ledger) ids += " " + std::to_string(k);
    }
    std::printf(
        "  crash at op %llu, in doubt: %s%s, committed transfers %zu, recovered transfers %zu\n",
        static_cast<unsigned long long>(crash_op), doubt ? "yes" : "no", ids.c_str(),
        committed.ledger.size(), seen.model.ledger.size());
    std::printf(
        "  final recovery: checkpoint %llu, end %llu, records %llu, redone %llu, losers %llu, "
        "undone %llu\n",
        static_cast<unsigned long long>(rec.checkpoint), static_cast<unsigned long long>(rec.end),
        static_cast<unsigned long long>(rec.records), static_cast<unsigned long long>(rec.redone),
        static_cast<unsigned long long>(rec.losers), static_cast<unsigned long long>(rec.undone));
    return false;
  }
  if (verbose)
    std::printf("seed %llu ok: %zu ledger rows, %zu scratch rows, in doubt %s\n",
                static_cast<unsigned long long>(seed), seen.model.ledger.size(),
                seen.model.scratch.size(), doubt ? "yes" : "no");
  return true;
}

}

int main(int argc, char** argv) {
  SetLogQuiet(true);
  uint64_t first = 1, seeds = 200;
  bool verbose = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--seeds" && i + 1 < argc) {
      seeds = std::stoull(argv[++i]);
    } else if (a == "--first" && i + 1 < argc) {
      first = std::stoull(argv[++i]);
    } else if (a == "--verbose") {
      verbose = true;
    } else {
      std::fprintf(stderr, "usage: rdbms-crashsim [--seeds N] [--first S] [--verbose]\n");
      return 2;
    }
  }
  Totals tot;
  uint64_t t0 = NowMicros();
  for (uint64_t s = first; s < first + seeds; s++) {
    RunSeed(s, tot, verbose);
    if (!verbose && (s - first + 1) % 100 == 0)
      std::printf("%llu seeds done, %llu failures\n",
                  static_cast<unsigned long long>(s - first + 1),
                  static_cast<unsigned long long>(tot.failures));
  }
  std::printf(
      "seeds %llu, crashes %llu (+%llu during recovery), commits %llu, losers rolled back %llu\n"
      "unsynced writes at crash: %llu kept, %llu dropped, %llu torn; %llu new files lost; "
      "%llu torn pages repaired\nfailures %llu (%.1f s)\n",
      static_cast<unsigned long long>(tot.seeds), static_cast<unsigned long long>(tot.crashes),
      static_cast<unsigned long long>(tot.recovery_crashes),
      static_cast<unsigned long long>(tot.commits), static_cast<unsigned long long>(tot.losers),
      static_cast<unsigned long long>(tot.kept), static_cast<unsigned long long>(tot.dropped),
      static_cast<unsigned long long>(tot.torn), static_cast<unsigned long long>(tot.lost_files),
      static_cast<unsigned long long>(tot.torn_pages),
      static_cast<unsigned long long>(tot.failures), static_cast<double>(NowMicros() - t0) / 1e6);
  return tot.failures == 0 ? 0 : 1;
}
