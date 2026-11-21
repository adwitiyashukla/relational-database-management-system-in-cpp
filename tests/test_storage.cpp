#include <algorithm>
#include <cmath>
#include <map>

#include "buffer.h"
#include "config.h"
#include "page.h"
#include "test.h"
#include "value.h"

using namespace rdbms;

TEST(crc32c_matches_known_vector) {
  CHECK_EQ(Crc32c("123456789", 9), 0xE3069283u);
  CHECK_EQ(Crc32c("", 0), 0u);
  std::string big(1000, 'x');
  uint32_t whole = Crc32c(big.data(), big.size());
  uint32_t split = Crc32c(big.data() + 300, 700, Crc32c(big.data(), 300));
  CHECK_EQ(whole, split);
}

TEST(page_ops_match_a_map) {
  char buf[kPageSize];
  Page p(buf);
  p.Format(7, PageType::Leaf, 0);
  std::map<std::string, std::string> model;
  Rng rng(42);
  for (int step = 0; step < 20000; step++) {
    std::string key = "k" + std::to_string(rng.Uniform(300));
    bool exact = false;
    uint16_t idx = p.LowerBound(key, &exact);
    uint64_t op = rng.Uniform(3);
    if (op == 2 && exact) {
      p.Erase(idx);
      model.erase(key);
      continue;
    }
    std::string rec =
        MakeLeaf(key, step, 0, std::string(rng.Uniform(120), static_cast<char>('a' + step % 26)));
    size_t have = p.FreeSpace() + (exact ? p.Rec(idx).size() + kSlotSize : 0);
    if (rec.size() + kSlotSize > have) continue;
    if (exact) {
      p.Replace(idx, rec);
    } else {
      p.Insert(idx, rec);
    }
    model[key] = rec;
  }
  CHECK_EQ(static_cast<size_t>(p.count()), model.size());
  size_t i = 0;
  size_t live = 0;
  for (auto& [k, rec] : model) {
    CHECK(p.KeyAt(static_cast<uint16_t>(i)) == k);
    CHECK(p.Rec(static_cast<uint16_t>(i)) == rec);
    live += rec.size();
    i++;
  }
  CHECK_EQ(p.LiveBytes(), live);
  CHECK_EQ(p.FreeSpace(), kPageSize - kHeaderSize - model.size() * kSlotSize - live);
  p.Compact();
  i = 0;
  for (auto& [k, rec] : model) CHECK(p.Rec(static_cast<uint16_t>(i++)) == rec);
  char copy[kPageSize];
  Page::Restore(copy, p.Image());
  CHECK(Page(copy).Image() == p.Image());
}

TEST(page_checksum_catches_a_flipped_bit) {
  char buf[kPageSize];
  Page p(buf);
  p.Format(3, PageType::Leaf, 0);
  p.Insert(0, MakeLeaf("abc", 1, 0, "value"));
  p.Seal();
  CHECK(p.Verify(3));
  CHECK(!p.Verify(4));
  buf[4000] ^= 0x10;
  CHECK(!p.Verify(3));
}

TEST(key_encoding_preserves_order) {
  Rng rng(7);
  std::vector<std::vector<Value>> rows;
  for (int i = 0; i < 3000; i++) {
    std::vector<Value> r;
    r.push_back(Value::Int(static_cast<int64_t>(rng.Next()) >> rng.Uniform(60)));
    double m = rng.Real() - 0.5;
    double d = m * std::pow(10.0, static_cast<double>(rng.Uniform(12)));
    r.push_back(Value::Float(d));
    std::string s;
    size_t n = rng.Uniform(6);
    for (size_t j = 0; j < n; j++) s.push_back(static_cast<char>(rng.Uniform(4)));
    r.push_back(Value::Text(s));
    rows.push_back(r);
  }
  auto by_value = [](const std::vector<Value>& a, const std::vector<Value>& b) {
    for (size_t i = 0; i < a.size(); i++) {
      int c = Compare(a[i], b[i]);
      if (c != 0) return c < 0;
    }
    return false;
  };
  for (size_t col = 0; col < 3; col++) {
    std::vector<std::vector<Value>> sorted = rows;
    std::vector<std::vector<Value>> single;
    for (auto& r : rows) {
      std::vector<Value> s(r.begin() + static_cast<long>(col), r.end());
      single.push_back(s);
    }
    std::sort(single.begin(), single.end(), by_value);
    for (size_t i = 1; i < single.size(); i++) {
      int c = CompareKeys(EncodeKey(single[i - 1]), EncodeKey(single[i]));
      CHECK(c <= 0);
      if (by_value(single[i - 1], single[i])) CHECK(c < 0);
    }
  }
  CHECK(CompareKeys(EncodeKey({Value::Float(-0.0)}), EncodeKey({Value::Float(0.0)})) == 0);
  CHECK(CompareKeys(EncodeKey({Value::Text("ab")}), EncodeKey({Value::Text("ab"), Value::Int(1)})) <
        0);
  CHECK(CompareKeys(KeySuccessor(EncodeKey({Value::Text("ab")})),
                    EncodeKey({Value::Text("ab"), Value::Int(9)})) > 0);
  CHECK_EQ(DecodeKeyInt(EncodeKey({Value::Int(-42)})), -42);
}

TEST(row_encoding_round_trips) {
  std::vector<Value> row = {Value::Int(-5), Value::Float(2.5), Value::Text("hello"),
                            Value::Null(),  Value::Bool(true), Value::Time(1700000000000000LL)};
  std::vector<Value> back = DecodeRow(EncodeRow(row));
  CHECK_EQ(back.size(), row.size());
  for (size_t i = 0; i < row.size(); i++) {
    CHECK(back[i].type == row[i].type);
    if (!row[i].null()) CHECK_EQ(Compare(back[i], row[i]), 0);
  }
}

TEST(values_format_like_postgres) {
  CHECK_EQ(ToText(Value::Float(0.1)), std::string("0.1"));
  CHECK_EQ(ToText(Value::Float(1e20)), std::string("1e+20"));
  CHECK_EQ(ToText(Value::Bool(false)), std::string("f"));
  int64_t t = 0;
  CHECK(ParseTimestamp("2026-10-04 12:34:56.5", &t));
  CHECK_EQ(FormatTimestamp(t), std::string("2026-10-04 12:34:56.5"));
  CHECK(ParseTimestamp("1999-12-31", &t));
  CHECK_EQ(FormatTimestamp(t), std::string("1999-12-31 00:00:00"));
  CHECK(!ParseTimestamp("2026-13-01", &t));
  int64_t v = 0;
  CHECK(ParseInt("-9223372036854775808", &v));
  CHECK(!ParseInt("9223372036854775808", &v));
  CHECK_CODE(Cast(Value::Int(70000), ColType::SmallInt), state::kOutOfRange);
  CHECK_CODE(CastTo(Value::Text("abc"), Type::Int), state::kBadText);
  CHECK_EQ(Compare(Value::Int(3), Value::Float(3.0)), 0);
  CHECK_EQ(Compare(Value::Text("10"), Value::Int(9)), 1);
}

TEST(config_parses_and_validates) {
  Config c = ParseConfig(
      "[server]\nport = 6000\nhost = \"0.0.0.0\"\n[wal]\nfsync = false\n"
      "segment_mb = 4\n[txn]\ndefault_isolation = \"Serializable\"\n");
  CHECK_EQ(c.port, 6000);
  CHECK_EQ(c.host, std::string("0.0.0.0"));
  CHECK(!c.fsync);
  CHECK_EQ(c.wal_segment_mb, 4);
  CHECK_EQ(c.default_isolation, std::string("serializable"));
  CHECK_CODE(ParseConfig("[server]\nprot = 1\n"), state::kInternal);
  CHECK_CODE(ParseConfig("[server]\nport = 70000\n"), state::kInternal);
  CHECK_CODE(ParseConfig("[wal]\nfsync = 1\n"), state::kInternal);
  CHECK_CODE(ParseConfig("[txn]\ndefault_isolation = \"chaos\"\n"), state::kInternal);
}

TEST(doublewrite_repairs_a_torn_page) {
  SimFs fs(1);
  fs.MakeDirs("db");
  DiskManager disk(fs, "db");
  char meta[kPageSize], cat[kPageSize];
  Page(meta).FormatMeta();
  Page(cat).Format(kCatalogRoot, PageType::Leaf, 0);
  disk.Create(meta, cat);
  char page[kPageSize];
  Page p(page);
  p.Format(5, PageType::Leaf, 0);
  p.Insert(0, MakeLeaf("key", 9, 0, std::string(3000, 'z')));
  std::vector<std::pair<PageId, char*>> batch = {{5, page}};
  disk.WritePages(batch);
  auto raw = fs.Open("db/data.db", false);
  std::string garbage(kPageSize / 2, '\x5A');
  raw->WriteAt(5 * kPageSize + 512, garbage.data(), garbage.size());
  raw->Sync();
  char check[kPageSize];
  CHECK_CODE(disk.ReadPage(5, check), state::kCorrupt);
  CHECK_EQ(disk.RepairTornPages(), 1);
  disk.ReadPage(5, check);
  CHECK(Page(check).Rec(0) == Page(page).Rec(0));
}

TEST(buffer_pool_flushes_the_log_before_a_page) {
  SimFs fs(2);
  fs.MakeDirs("db");
  DiskManager disk(fs, "db");
  char meta[kPageSize], cat[kPageSize];
  Page(meta).FormatMeta();
  Page(cat).Format(kCatalogRoot, PageType::Leaf, 0);
  disk.Create(meta, cat);
  LogManager log(fs, "db", WalOptions{});
  log.Create();
  BufferPool pool(disk, log, 16);
  std::vector<Lsn> lsns;
  for (PageId id = 10; id < 60; id++) {
    PageRef r = pool.Create(id);
    r.LockX();
    r.page().Format(id, PageType::Leaf, 0);
    LogRecord rec;
    rec.type = LogType::Images;
    rec.images.emplace_back(id, r.page().Image());
    Lsn l = log.Append(rec, [&](Lsn x) {
      r.page().set_lsn(x);
      pool.MarkDirty(r.frame(), x);
    });
    lsns.push_back(l);
  }
  CHECK(log.flushed_lsn() > lsns[lsns.size() - 20]);
  pool.FlushAll();
  CHECK_EQ(pool.dirty_count(), 0u);
  for (PageId id = 10; id < 60; id++) {
    PageRef r = pool.Fetch(id);
    r.LockS();
    CHECK_EQ(r.page().id(), id);
    CHECK(r.page().lsn() == lsns[id - 10]);
  }
}
