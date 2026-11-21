#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "fs.h"
#include "page.h"
#include "wal.h"

namespace rdbms {

class DiskManager {
 public:
  static constexpr size_t kBatch = 64;

  DiskManager(FileSystem& fs, std::string data_dir);
  void Create(const char* meta, const char* catalog_root);
  void Open();
  void ReadPage(PageId id, char* buf);
  void WritePages(std::vector<std::pair<PageId, char*>>& pages);
  int RepairTornPages();
  uint64_t pages_written() const { return written_.load(); }

 private:
  FileSystem& fs_;
  std::string dir_;
  std::unique_ptr<File> data_;
  std::unique_ptr<File> dw_;
  std::mutex mu_;
  std::atomic<uint64_t> written_{0};
};

struct Frame {
  char* data = nullptr;
  PageId pid = kNoPage;
  std::atomic<int> pin{0};
  std::atomic<bool> ref{false};
  std::atomic<bool> dirty{false};
  bool flushing = false;
  Lsn rec_lsn = 0;
  Lsn last_lsn = 0;
  SharedMutex latch;
};

class BufferPool;

class PageRef {
 public:
  PageRef() = default;
  PageRef(BufferPool* pool, Frame* f) : pool_(pool), f_(f) {}
  PageRef(PageRef&& o) noexcept { *this = std::move(o); }
  PageRef& operator=(PageRef&& o) noexcept;
  PageRef(const PageRef&) = delete;
  PageRef& operator=(const PageRef&) = delete;
  ~PageRef() { Release(); }

  void LockS();
  void LockX();
  void Unlock();
  void Release();
  bool valid() const { return f_ != nullptr; }
  char* data() const { return f_->data; }
  Page page() const { return Page(f_->data); }
  PageId id() const { return f_->pid; }
  Frame* frame() const { return f_; }

 private:
  BufferPool* pool_ = nullptr;
  Frame* f_ = nullptr;
  int mode_ = 0;
};

class BufferPool {
 public:
  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t flushed = 0;
  };

  BufferPool(DiskManager& disk, LogManager& log, size_t frames);
  ~BufferPool();

  PageRef Fetch(PageId id);
  PageRef Create(PageId id);
  void Unpin(Frame* f) { f->pin.fetch_sub(1); }
  void MarkDirty(Frame* f, Lsn lsn);
  size_t FlushSome(size_t max_pages, Lsn below = ~0ull);
  void FlushAll();
  void DirtyPages(std::vector<std::pair<PageId, Lsn>>* out);
  size_t dirty_count() const { return dirty_count_.load(); }
  size_t capacity() const { return n_; }
  Stats stats() const;

 private:
  int FindVictim();
  PageRef Load(PageId id, bool read);

  DiskManager& disk_;
  LogManager& log_;
  size_t n_;
  std::unique_ptr<char[]> mem_;
  std::unique_ptr<Frame[]> frames_;
  SharedMutex table_mu_;
  std::unordered_map<PageId, size_t> table_;
  size_t hand_ = 0;
  std::mutex state_mu_;
  std::atomic<size_t> dirty_count_{0};
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
  std::atomic<uint64_t> flushed_{0};
};

}
