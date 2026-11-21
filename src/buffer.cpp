#include "buffer.h"

#include <algorithm>

namespace rdbms {

namespace {

constexpr uint64_t kDwMagic = 0x425744534D424452ull;

thread_local std::vector<Frame*> t_latched;

void ForgetLatch(Frame* f) {
  for (size_t i = t_latched.size(); i-- > 0;) {
    if (t_latched[i] == f) {
      t_latched.erase(t_latched.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

bool LatchedHere(Frame* f) {
  return std::find(t_latched.begin(), t_latched.end(), f) != t_latched.end();
}

}

DiskManager::DiskManager(FileSystem& fs, std::string data_dir)
    : fs_(fs), dir_(std::move(data_dir)) {}

void DiskManager::Create(const char* meta, const char* catalog_root) {
  data_ = fs_.Open(dir_ + "/data.db", true);
  dw_ = fs_.Open(dir_ + "/doublewrite", true);
  char buf[kPageSize];
  std::memcpy(buf, meta, kPageSize);
  Page(buf).Seal();
  data_->WriteAt(0, buf, kPageSize);
  std::memcpy(buf, catalog_root, kPageSize);
  Page(buf).Seal();
  data_->WriteAt(kPageSize, buf, kPageSize);
  data_->Sync();
  dw_->Sync();
  fs_.SyncDir(dir_);
}

void DiskManager::Open() {
  data_ = fs_.Open(dir_ + "/data.db", false);
  dw_ = fs_.Open(dir_ + "/doublewrite", true);
}

void DiskManager::ReadPage(PageId id, char* buf) {
  size_t got = data_->ReadAt(static_cast<uint64_t>(id) * kPageSize, buf, kPageSize);
  if (got < kPageSize) std::memset(buf + got, 0, kPageSize - got);
  Page p(buf);
  if (p.IsZero()) return;
  if (!p.Verify(id)) Fail(state::kCorrupt, "page " + std::to_string(id) + " failed its checksum");
}

void DiskManager::WritePages(std::vector<std::pair<PageId, char*>>& pages) {
  std::vector<std::pair<PageId, char*>> sorted = pages;
  std::sort(sorted.begin(), sorted.end());
  std::lock_guard<std::mutex> g(mu_);
  for (size_t at = 0; at < sorted.size(); at += kBatch) {
    size_t k = std::min(kBatch, sorted.size() - at);
    std::string dw(kPageSize * (k + 1), '\0');
    PutU64(dw.data(), kDwMagic);
    PutU32(dw.data() + 8, static_cast<uint32_t>(k));
    for (size_t i = 0; i < k; i++) {
      PutU32(dw.data() + 16 + 4 * i, sorted[at + i].first);
      Page(sorted[at + i].second).Seal();
      std::memcpy(dw.data() + kPageSize * (i + 1), sorted[at + i].second, kPageSize);
    }
    PutU32(dw.data() + 16 + 4 * k, Crc32c(dw.data(), 16 + 4 * k));
    dw_->WriteAt(0, dw.data(), dw.size());
    dw_->Sync();
    for (size_t i = 0; i < k; i++) {
      data_->WriteAt(static_cast<uint64_t>(sorted[at + i].first) * kPageSize, sorted[at + i].second,
                     kPageSize);
    }
    data_->Sync();
    written_ += k;
  }
}

int DiskManager::RepairTornPages() {
  if (dw_->Size() < kPageSize) return 0;
  std::string hdr(kPageSize, '\0');
  dw_->ReadAt(0, hdr.data(), kPageSize);
  if (GetU64(hdr.data()) != kDwMagic) return 0;
  uint32_t k = GetU32(hdr.data() + 8);
  if (k == 0 || k > kBatch) return 0;
  if (GetU32(hdr.data() + 16 + 4 * k) != Crc32c(hdr.data(), 16 + 4 * k)) return 0;
  int repaired = 0;
  char copy[kPageSize];
  char here[kPageSize];
  for (uint32_t i = 0; i < k; i++) {
    PageId pid = GetU32(hdr.data() + 16 + 4 * i);
    if (dw_->ReadAt(kPageSize * (i + 1), copy, kPageSize) < kPageSize) continue;
    if (!Page(copy).Verify(pid)) continue;
    size_t got = data_->ReadAt(static_cast<uint64_t>(pid) * kPageSize, here, kPageSize);
    if (got < kPageSize) std::memset(here + got, 0, kPageSize - got);
    if (Page(here).Verify(pid)) continue;
    data_->WriteAt(static_cast<uint64_t>(pid) * kPageSize, copy, kPageSize);
    repaired++;
  }
  if (repaired > 0) data_->Sync();
  return repaired;
}

PageRef& PageRef::operator=(PageRef&& o) noexcept {
  if (this != &o) {
    Release();
    pool_ = o.pool_;
    f_ = o.f_;
    mode_ = o.mode_;
    o.f_ = nullptr;
    o.mode_ = 0;
  }
  return *this;
}

void PageRef::LockS() {
  f_->latch.lock_shared();
  mode_ = 1;
  t_latched.push_back(f_);
}

void PageRef::LockX() {
  f_->latch.lock();
  mode_ = 2;
  t_latched.push_back(f_);
}

void PageRef::Unlock() {
  if (mode_ == 1) {
    f_->latch.unlock_shared();
  } else if (mode_ == 2) {
    f_->latch.unlock();
  } else {
    return;
  }
  mode_ = 0;
  ForgetLatch(f_);
}

void PageRef::Release() {
  if (!f_) return;
  Unlock();
  pool_->Unpin(f_);
  f_ = nullptr;
}

BufferPool::BufferPool(DiskManager& disk, LogManager& log, size_t frames)
    : disk_(disk), log_(log), n_(std::max<size_t>(frames, 16)) {
  mem_ = std::make_unique<char[]>(n_ * kPageSize);
  frames_ = std::make_unique<Frame[]>(n_);
  for (size_t i = 0; i < n_; i++) frames_[i].data = mem_.get() + i * kPageSize;
  table_.reserve(n_ * 2);
}

BufferPool::~BufferPool() = default;

int BufferPool::FindVictim() {
  for (size_t tries = 0; tries < 2 * n_; tries++) {
    hand_ = (hand_ + 1) % n_;
    Frame& f = frames_[hand_];
    if (f.pid == kNoPage) return static_cast<int>(hand_);
    if (f.pin.load() != 0 || f.dirty.load()) continue;
    if (f.ref.exchange(false)) continue;
    return static_cast<int>(hand_);
  }
  return -1;
}

PageRef BufferPool::Load(PageId id, bool read) {
  if (read) {
    std::shared_lock<SharedMutex> l(table_mu_);
    auto it = table_.find(id);
    if (it != table_.end()) {
      Frame* f = &frames_[it->second];
      f->pin.fetch_add(1);
      f->ref.store(true);
      hits_++;
      return PageRef(this, f);
    }
  }
  for (int attempt = 0;; attempt++) {
    std::unique_lock<SharedMutex> l(table_mu_);
    auto it = table_.find(id);
    if (it != table_.end()) {
      if (!read) Fail(state::kInternal, "page " + std::to_string(id) + " already cached");
      Frame* f = &frames_[it->second];
      f->pin.fetch_add(1);
      f->ref.store(true);
      hits_++;
      return PageRef(this, f);
    }
    int v = FindVictim();
    if (v >= 0) {
      Frame* f = &frames_[v];
      if (f->pid != kNoPage) table_.erase(f->pid);
      f->pid = id;
      f->pin.store(1);
      f->ref.store(true);
      table_[id] = static_cast<size_t>(v);
      try {
        if (read) {
          disk_.ReadPage(id, f->data);
        } else {
          std::memset(f->data, 0, kPageSize);
        }
      } catch (...) {
        table_.erase(id);
        f->pid = kNoPage;
        f->pin.store(0);
        throw;
      }
      misses_++;
      return PageRef(this, f);
    }
    l.unlock();
    if (attempt > 20000) Fail(state::kInternal, "buffer pool exhausted: every frame is pinned");
    if (FlushSome(32) == 0) SleepMicros(200);
  }
}

PageRef BufferPool::Fetch(PageId id) { return Load(id, true); }

PageRef BufferPool::Create(PageId id) { return Load(id, false); }

void BufferPool::MarkDirty(Frame* f, Lsn lsn) {
  std::lock_guard<std::mutex> g(state_mu_);
  if (!f->dirty.load()) {
    f->dirty.store(true);
    f->rec_lsn = lsn;
    dirty_count_++;
  }
  f->last_lsn = lsn;
}

size_t BufferPool::FlushSome(size_t max_pages, Lsn below) {
  struct Item {
    Frame* f;
    PageId pid;
    Lsn rec;
  };
  std::vector<Item> items;
  {
    std::lock_guard<std::mutex> g(state_mu_);
    for (size_t i = 0; i < n_; i++) {
      Frame& f = frames_[i];
      if (f.dirty.load() && !f.flushing && f.rec_lsn < below && !LatchedHere(&f))
        items.push_back({&f, f.pid, f.rec_lsn});
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.rec < b.rec; });
    if (items.size() > max_pages) items.resize(max_pages);
    for (auto& it : items) {
      it.f->pin.fetch_add(1);
      it.f->flushing = true;
    }
  }
  auto finish = [&] {
    for (auto& it : items) {
      it.f->flushing = false;
      it.f->pin.fetch_sub(1);
    }
  };
  std::string buf(items.size() * kPageSize, '\0');
  std::vector<std::pair<PageId, char*>> pages;
  std::vector<Lsn> lsns;
  std::vector<Frame*> copied;
  Lsn max_lsn = 0;
  for (auto& it : items) {
    if (!it.f->latch.try_lock_shared()) continue;
    char* dst = buf.data() + pages.size() * kPageSize;
    std::memcpy(dst, it.f->data, kPageSize);
    it.f->latch.unlock_shared();
    Lsn l = Page(dst).lsn();
    pages.emplace_back(it.pid, dst);
    lsns.push_back(l);
    copied.push_back(it.f);
    max_lsn = std::max(max_lsn, l);
  }
  try {
    if (!pages.empty()) {
      log_.Flush(max_lsn);
      disk_.WritePages(pages);
    }
  } catch (...) {
    std::lock_guard<std::mutex> g(state_mu_);
    finish();
    throw;
  }
  {
    std::lock_guard<std::mutex> g(state_mu_);
    for (size_t j = 0; j < copied.size(); j++) {
      Frame* f = copied[j];
      if (f->last_lsn == lsns[j]) {
        if (f->dirty.load()) {
          f->dirty.store(false);
          f->rec_lsn = 0;
          dirty_count_--;
        }
      } else {
        f->rec_lsn = lsns[j];
      }
    }
    finish();
  }
  flushed_ += pages.size();
  return pages.size();
}

void BufferPool::FlushAll() {
  for (int round = 0; dirty_count_.load() > 0 && round < 10000; round++) {
    if (FlushSome(n_) == 0) SleepMicros(500);
  }
}

void BufferPool::DirtyPages(std::vector<std::pair<PageId, Lsn>>* out) {
  std::lock_guard<std::mutex> g(state_mu_);
  for (size_t i = 0; i < n_; i++) {
    Frame& f = frames_[i];
    if (f.dirty.load()) out->emplace_back(f.pid, f.rec_lsn);
  }
}

BufferPool::Stats BufferPool::stats() const {
  Stats s;
  s.hits = hits_.load();
  s.misses = misses_.load();
  s.flushed = flushed_.load();
  return s;
}

}
