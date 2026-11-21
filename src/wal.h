#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "common.h"
#include "fs.h"

namespace rdbms {

enum class LogType : uint8_t {
  Row = 1,
  Clr = 2,
  Images = 3,
  Purge = 4,
  Commit = 5,
  Abort = 6,
  End = 7,
  Checkpoint = 8
};

struct RowImage {
  Xid xid = 0;
  uint8_t flags = 0;
  std::string value;
};

enum class TxnStatus : uint8_t { Active = 1, Committed = 2, Aborting = 3 };

struct TxnEntry {
  Xid xid = 0;
  TxnStatus status = TxnStatus::Active;
  Lsn first = 0;
  Lsn last = 0;
};

struct LogRecord {
  Lsn lsn = 0;
  Lsn prev = 0;
  Xid xid = 0;
  LogType type = LogType::Commit;
  uint32_t tree = 0;
  PageId page = kNoPage;
  std::string key;
  bool has_before = false;
  RowImage before;
  bool has_after = false;
  RowImage after;
  Lsn undo_next = 0;
  std::vector<std::pair<PageId, std::string>> images;
  Xid next_xid = 0;
  std::vector<TxnEntry> att;
  std::vector<std::pair<PageId, Lsn>> dpt;

  std::string Encode() const;
  static bool Decode(std::string_view bytes, LogRecord* out);
  std::vector<PageId> Pages() const;
};

struct WalOptions {
  uint64_t segment_size = 16ull << 20;
  bool fsync = true;
  uint32_t group_delay_us = 0;
};

class LogManager {
 public:
  static constexpr Lsn kStart = 64;
  static constexpr size_t kHeader = 33;

  LogManager(FileSystem& fs, std::string data_dir, WalOptions opt);
  ~LogManager();

  void Create();
  Lsn Append(const LogRecord& rec, const std::function<void(Lsn)>& locked = nullptr);
  Lsn AppendCheckpoint(const std::function<void(LogRecord&)>& fill);
  void Flush(Lsn lsn);
  Lsn next_lsn();
  Lsn flushed_lsn();
  uint64_t bytes_written() const { return bytes_written_.load(); }
  uint64_t flush_count() const { return flushes_.load(); }

  bool Read(Lsn lsn, LogRecord* out, Lsn* end);
  void Reopen(Lsn end);
  void TruncateBefore(Lsn lsn);
  size_t SegmentCount();

  void WriteControl(Lsn checkpoint);
  bool ReadControl(Lsn* checkpoint);

  const WalOptions& options() const { return opt_; }

 private:
  size_t ReadBytes(Lsn pos, char* buf, size_t n);
  size_t ReadRaw(Lsn pos, char* buf, size_t n);
  void WriteBytes(Lsn pos, const std::string& data);
  File* Segment(uint64_t idx, bool create, bool* created);
  std::string SegmentPath(uint64_t idx) const;

  FileSystem& fs_;
  std::string data_dir_;
  std::string dir_;
  WalOptions opt_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::string buf_;
  Lsn buf_start_ = kStart;
  Lsn next_ = kStart;
  Lsn flushed_ = kStart;
  bool flushing_ = false;
  bool panic_ = false;

  std::mutex seg_mu_;
  std::map<uint64_t, std::unique_ptr<File>> segs_;
  std::string cache_;
  Lsn cache_pos_ = 0;

  uint64_t control_seq_ = 0;
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> flushes_{0};
};

}
