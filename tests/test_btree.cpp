#include <atomic>
#include <map>
#include <thread>

#include "btree.h"
#include "test.h"

using namespace rdbms;

namespace {

struct Env {
  SimFs fs{11};
  DiskManager disk{fs, "db"};
  LogManager log{fs, "db", WalOptions{}};
  std::unique_ptr<BufferPool> pool;
  std::unique_ptr<Store> store;

  explicit Env(size_t frames) {
    fs.MakeDirs("db");
    char meta[kPageSize], cat[kPageSize];
    Page(meta).FormatMeta();
    Page(cat).Format(kCatalogRoot, PageType::Leaf, 0);
    disk.Create(meta, cat);
    log.Create();
    pool = std::make_unique<BufferPool>(disk, log, frames);
    store = std::make_unique<Store>(Store{*pool, log});
  }

  bool Put(BTree& t, const std::string& key, const std::string& value) {
    return t.Modify(
        key,
        [&](const LeafRec*) -> std::optional<LeafChange> {
          LeafChange c;
          c.xid = 1;
          c.value = value;
          return c;
        },
        [&](PageRef& leaf, const LeafRec*, const LeafChange& ch) { Log(leaf, key, &ch); });
  }

  bool Remove(BTree& t, const std::string& key) {
    return t.Modify(
        key,
        [&](const LeafRec* cur) -> std::optional<LeafChange> {
          if (!cur) return std::nullopt;
          LeafChange c;
          c.remove = true;
          return c;
        },
        [&](PageRef& leaf, const LeafRec*, const LeafChange& ch) { Log(leaf, key, &ch); });
  }

  void Log(PageRef& leaf, const std::string& key, const LeafChange* ch) {
    LogRecord r;
    r.type = ch->remove ? LogType::Purge : LogType::Row;
    r.page = leaf.id();
    r.key = key;
    r.has_after = !ch->remove;
    r.after.value = ch->value;
    log.Append(r, [&](Lsn l) {
      leaf.page().set_lsn(l);
      pool->MarkDirty(leaf.frame(), l);
    });
  }

  std::map<std::string, std::string> Dump(BTree& t) {
    std::map<std::string, std::string> out;
    std::string prev;
    bool first = true;
    t.Scan(std::string(), nullptr, false, [&](const LeafRec& r) {
      std::string k(r.key);
      if (!first && CompareKeys(prev, k) >= 0) throw testing::Failure("scan out of order");
      first = false;
      prev = k;
      out[k] = std::string(r.value);
      return true;
    });
    return out;
  }
};

std::string Key(Rng& rng) {
  size_t n = 1 + rng.Uniform(40);
  std::string k;
  for (size_t i = 0; i < n; i++) k.push_back(static_cast<char>('a' + rng.Uniform(6)));
  return k;
}

}

TEST(random_operations_match_a_map) {
  Env env(64);
  BTree t(*env.store, 0, BTree::Create(*env.store));
  std::map<std::string, std::string> model;
  Rng rng(99);
  for (int i = 0; i < 30000; i++) {
    std::string k = Key(rng);
    if (rng.Chance(0.25)) {
      bool had = model.erase(k) > 0;
      CHECK_EQ(env.Remove(t, k), had);
    } else {
      std::string v(rng.Uniform(rng.Chance(0.05) ? 1800 : 90), static_cast<char>('A' + i % 26));
      env.Put(t, k, v);
      model[k] = v;
    }
    if (i % 5000 == 0) t.Validate();
  }
  BTree::Shape shape = t.Validate();
  CHECK_EQ(shape.records, model.size());
  CHECK(shape.height >= 3);
  CHECK(env.Dump(t) == model);
  for (auto& [k, v] : model) {
    bool found = t.Get(k, [&](const LeafRec* r) { CHECK(r && std::string(r->value) == v); });
    CHECK(found);
  }
}

TEST(range_scans_respect_bounds) {
  Env env(64);
  BTree t(*env.store, 0, BTree::Create(*env.store));
  for (int i = 0; i < 5000; i++) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06d", i);
    env.Put(t, buf, "v");
  }
  std::string hi = "002000";
  int n = 0;
  t.Scan("001000", &hi, true, [&](const LeafRec&) {
    n++;
    return true;
  });
  CHECK_EQ(n, 1001);
  n = 0;
  t.Scan("001000", &hi, false, [&](const LeafRec&) {
    n++;
    return true;
  });
  CHECK_EQ(n, 1000);
  n = 0;
  t.Scan("004990", nullptr, false, [&](const LeafRec&) {
    n++;
    return n < 5;
  });
  CHECK_EQ(n, 5);
  CHECK_EQ(t.LastKey(), std::string("004999"));
}

TEST(concurrent_writers_and_scanners_keep_the_tree_valid) {
  Env env(256);
  BTree t(*env.store, 0, BTree::Create(*env.store));
  std::atomic<int> errors{0};
  std::atomic<bool> done{false};
  std::vector<std::thread> threads;
  for (int w = 0; w < 4; w++) {
    threads.emplace_back([&, w] {
      Rng rng(static_cast<uint64_t>(w) + 100);
      for (int i = 0; i < 4000; i++) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d-%08d", w, i);
        try {
          env.Put(t, buf, std::string(20 + rng.Uniform(200), 'x'));
        } catch (...) {
          errors++;
        }
      }
    });
  }
  for (int r = 0; r < 2; r++) {
    threads.emplace_back([&] {
      while (!done.load()) {
        std::string prev;
        bool first = true;
        try {
          t.Scan(std::string(), nullptr, false, [&](const LeafRec& rec) {
            std::string k(rec.key);
            if (!first && CompareKeys(prev, k) >= 0) errors++;
            first = false;
            prev = k;
            return true;
          });
        } catch (...) {
          errors++;
        }
      }
    });
  }
  for (int i = 0; i < 4; i++) threads[static_cast<size_t>(i)].join();
  done = true;
  for (size_t i = 4; i < threads.size(); i++) threads[i].join();
  CHECK_EQ(errors.load(), 0);
  CHECK_EQ(t.Validate().records, 16000u);
}

TEST(large_records_still_split_cleanly) {
  Env env(64);
  BTree t(*env.store, 0, BTree::Create(*env.store));
  for (int i = 0; i < 300; i++) {
    std::string k(200, static_cast<char>('a' + i % 26));
    k += std::to_string(i);
    env.Put(t, k, std::string(kMaxRecordSize - k.size() - 20, 'q'));
  }
  CHECK_EQ(t.Validate().records, 300u);
  CHECK_CODE(env.Put(t, "big", std::string(kMaxRecordSize, 'q')), state::kLimit);
  CHECK_CODE(env.Put(t, std::string(300, 'k'), "v"), state::kLimit);
}
