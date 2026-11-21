#include <thread>

#include "test.h"
#include "wal.h"

using namespace rdbms;

namespace {

LogRecord RowRecord(Xid xid, const std::string& key, size_t size) {
  LogRecord r;
  r.type = LogType::Row;
  r.xid = xid;
  r.tree = 9;
  r.page = 33;
  r.key = key;
  r.has_before = true;
  r.before.xid = 1;
  r.before.value = "old";
  r.has_after = true;
  r.after.xid = xid;
  r.after.value = std::string(size, 'v');
  return r;
}

}

TEST(records_round_trip_through_bytes) {
  LogRecord row = RowRecord(5, "k1", 10);
  LogRecord clr;
  clr.type = LogType::Clr;
  clr.xid = 5;
  clr.key = "k1";
  clr.undo_next = 77;
  LogRecord img;
  img.type = LogType::Images;
  img.images.emplace_back(4, std::string(100, 'i'));
  img.images.emplace_back(8, std::string(50, 'j'));
  LogRecord ck;
  ck.type = LogType::Checkpoint;
  ck.next_xid = 99;
  ck.att.push_back({5, TxnStatus::Aborting, 100, 200});
  ck.dpt.emplace_back(4, 150);
  for (LogRecord* r : {&row, &clr, &img, &ck}) {
    std::string b = r->Encode();
    PutU32(b.data(), Crc32c(b.data() + 4, b.size() - 4));
    LogRecord back;
    CHECK(LogRecord::Decode(b, &back));
    CHECK(back.type == r->type);
    CHECK(back.Encode() == r->Encode());
    b[b.size() / 2] ^= 1;
    CHECK(!LogRecord::Decode(b, &back));
  }
}

TEST(records_span_segments_and_read_back) {
  SimFs fs(3);
  WalOptions opt;
  opt.segment_size = 4096;
  LogManager log(fs, "db", opt);
  log.Create();
  std::vector<Lsn> lsns;
  for (int i = 0; i < 200; i++)
    lsns.push_back(log.Append(RowRecord(10 + i, "key" + std::to_string(i), 37 * (i % 9))));
  log.Flush(lsns.back());
  CHECK(log.SegmentCount() > 5);
  LogManager again(fs, "db", opt);
  Lsn pos = LogManager::kStart;
  LogRecord r;
  Lsn next = 0;
  int n = 0;
  while (again.Read(pos, &r, &next)) {
    CHECK_EQ(r.lsn, lsns[static_cast<size_t>(n)]);
    CHECK_EQ(r.key, "key" + std::to_string(n));
    pos = next;
    n++;
  }
  CHECK_EQ(n, 200);
}

TEST(torn_tail_ends_the_log_and_reopen_zeroes_it) {
  SimFs fs(4);
  WalOptions opt;
  opt.segment_size = 1 << 16;
  LogManager log(fs, "db", opt);
  log.Create();
  Lsn a = log.Append(RowRecord(1, "a", 100));
  Lsn b = log.Append(RowRecord(2, "b", 100));
  log.Flush(b);
  auto seg = fs.Open("db/wal/0000000000000000.wal", false);
  char junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  seg->WriteAt(b + 40, junk, sizeof(junk));
  seg->Sync();
  LogManager again(fs, "db", opt);
  LogRecord r;
  Lsn end = 0;
  CHECK(again.Read(a, &r, &end));
  CHECK_EQ(end, b);
  CHECK(!again.Read(b, &r, &end));
  again.Reopen(b);
  Lsn c = again.Append(RowRecord(3, "c", 10));
  CHECK_EQ(c, b);
  again.Flush(c);
  LogManager third(fs, "db", opt);
  Lsn after = 0;
  CHECK(third.Read(c, &r, &after));
  CHECK_EQ(r.key, std::string("c"));
  CHECK(!third.Read(after, &r, nullptr));
}

TEST(group_commit_shares_flushes) {
  SimFs fs(5);
  LogManager log(fs, "db", WalOptions{});
  log.Create();
  std::vector<std::thread> threads;
  std::atomic<int> early{0};
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 200; i++) {
        LogRecord c;
        c.type = LogType::Commit;
        c.xid = static_cast<Xid>(t * 1000 + i + 1);
        Lsn l = log.Append(c);
        log.Flush(l);
        if (log.flushed_lsn() <= l) early++;
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK_EQ(early.load(), 0);
  CHECK(log.flush_count() < 1600u);
  CHECK(log.flush_count() > 0u);
}

TEST(truncation_drops_whole_old_segments) {
  SimFs fs(6);
  WalOptions opt;
  opt.segment_size = 4096;
  LogManager log(fs, "db", opt);
  log.Create();
  Lsn last = 0;
  for (int i = 0; i < 100; i++) last = log.Append(RowRecord(1, "k", 200));
  log.Flush(last);
  size_t before = log.SegmentCount();
  log.TruncateBefore(last);
  CHECK(log.SegmentCount() < before);
  LogRecord r;
  CHECK(log.Read(last, &r, nullptr));
}

TEST(control_file_survives_a_torn_slot) {
  SimFs fs(7);
  fs.MakeDirs("db");
  LogManager log(fs, "db", WalOptions{});
  log.WriteControl(100);
  log.WriteControl(200);
  Lsn ck = 0;
  LogManager reader(fs, "db", WalOptions{});
  CHECK(reader.ReadControl(&ck));
  CHECK_EQ(ck, 200u);
  auto f = fs.Open("db/control", false);
  char junk[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  f->WriteAt(8, junk, 8);
  f->Sync();
  LogManager again(fs, "db", WalOptions{});
  CHECK(again.ReadControl(&ck));
  CHECK_EQ(ck, 100u);
}
