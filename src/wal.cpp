#include "wal.h"

#include <algorithm>
#include <cstdio>

namespace rdbms {

namespace {

constexpr uint32_t kMaxRecord = 64u << 20;
constexpr uint64_t kControlMagic = 0x4C5443534D424452ull;

void PutImage(Writer& w, const RowImage& r) {
  w.U64(r.xid);
  w.U8(r.flags);
  w.Str(r.value);
}

RowImage GetImage(Reader& r) {
  RowImage img;
  img.xid = r.U64();
  img.flags = r.U8();
  img.value = r.Str();
  return img;
}

}

std::string LogRecord::Encode() const {
  std::string out(LogManager::kHeader, '\0');
  Writer w(out);
  switch (type) {
    case LogType::Row:
      w.U32(tree);
      w.U32(page);
      w.Str(key);
      w.U8(has_before ? 1 : 0);
      if (has_before) PutImage(w, before);
      w.U8(has_after ? 1 : 0);
      if (has_after) PutImage(w, after);
      break;
    case LogType::Clr:
      w.U32(tree);
      w.U32(page);
      w.Str(key);
      w.U8(has_after ? 1 : 0);
      if (has_after) PutImage(w, after);
      w.U64(undo_next);
      break;
    case LogType::Images:
      w.U32(static_cast<uint32_t>(images.size()));
      for (auto& [pid, img] : images) {
        w.U32(pid);
        w.Str(img);
      }
      break;
    case LogType::Purge:
      w.U32(tree);
      w.U32(page);
      w.Str(key);
      break;
    case LogType::Checkpoint:
      w.U64(next_xid);
      w.U32(static_cast<uint32_t>(att.size()));
      for (auto& t : att) {
        w.U64(t.xid);
        w.U8(static_cast<uint8_t>(t.status));
        w.U64(t.first);
        w.U64(t.last);
      }
      w.U32(static_cast<uint32_t>(dpt.size()));
      for (auto& [pid, rec] : dpt) {
        w.U32(pid);
        w.U64(rec);
      }
      break;
    default:
      break;
  }
  PutU32(out.data() + 4, static_cast<uint32_t>(out.size()));
  PutU64(out.data() + 8, lsn);
  PutU64(out.data() + 16, prev);
  PutU64(out.data() + 24, xid);
  out[32] = static_cast<char>(type);
  return out;
}

bool LogRecord::Decode(std::string_view b, LogRecord* out) {
  if (b.size() < LogManager::kHeader) return false;
  if (GetU32(b.data()) != Crc32c(b.data() + 4, b.size() - 4)) return false;
  LogRecord r;
  r.lsn = GetU64(b.data() + 8);
  r.prev = GetU64(b.data() + 16);
  r.xid = GetU64(b.data() + 24);
  r.type = static_cast<LogType>(static_cast<uint8_t>(b[32]));
  Reader rd(b.data() + LogManager::kHeader, b.size() - LogManager::kHeader);
  try {
    switch (r.type) {
      case LogType::Row:
        r.tree = rd.U32();
        r.page = rd.U32();
        r.key = rd.Str();
        r.has_before = rd.U8() != 0;
        if (r.has_before) r.before = GetImage(rd);
        r.has_after = rd.U8() != 0;
        if (r.has_after) r.after = GetImage(rd);
        break;
      case LogType::Clr:
        r.tree = rd.U32();
        r.page = rd.U32();
        r.key = rd.Str();
        r.has_after = rd.U8() != 0;
        if (r.has_after) r.after = GetImage(rd);
        r.undo_next = rd.U64();
        break;
      case LogType::Images: {
        uint32_t n = rd.U32();
        for (uint32_t i = 0; i < n; i++) {
          PageId pid = rd.U32();
          r.images.emplace_back(pid, rd.Str());
        }
        break;
      }
      case LogType::Purge:
        r.tree = rd.U32();
        r.page = rd.U32();
        r.key = rd.Str();
        break;
      case LogType::Checkpoint: {
        r.next_xid = rd.U64();
        uint32_t n = rd.U32();
        for (uint32_t i = 0; i < n; i++) {
          TxnEntry t;
          t.xid = rd.U64();
          t.status = static_cast<TxnStatus>(rd.U8());
          t.first = rd.U64();
          t.last = rd.U64();
          r.att.push_back(t);
        }
        uint32_t m = rd.U32();
        for (uint32_t i = 0; i < m; i++) {
          PageId pid = rd.U32();
          r.dpt.emplace_back(pid, rd.U64());
        }
        break;
      }
      case LogType::Commit:
      case LogType::Abort:
      case LogType::End:
        break;
      default:
        return false;
    }
  } catch (const DbError&) {
    return false;
  }
  if (!rd.done()) return false;
  *out = std::move(r);
  return true;
}

std::vector<PageId> LogRecord::Pages() const {
  switch (type) {
    case LogType::Row:
    case LogType::Clr:
    case LogType::Purge:
      return {page};
    case LogType::Images: {
      std::vector<PageId> v;
      for (auto& im : images) v.push_back(im.first);
      return v;
    }
    default:
      return {};
  }
}

LogManager::LogManager(FileSystem& fs, std::string data_dir, WalOptions opt)
    : fs_(fs), data_dir_(std::move(data_dir)), dir_(data_dir_ + "/wal"), opt_(opt) {}

LogManager::~LogManager() = default;

std::string LogManager::SegmentPath(uint64_t idx) const {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.wal", static_cast<unsigned long long>(idx));
  return dir_ + "/" + name;
}

File* LogManager::Segment(uint64_t idx, bool create, bool* created) {
  auto it = segs_.find(idx);
  if (it != segs_.end()) return it->second.get();
  std::string path = SegmentPath(idx);
  bool exists = fs_.Exists(path);
  if (!exists && !create) return nullptr;
  auto f = fs_.Open(path, true);
  if (!exists && created) *created = true;
  File* raw = f.get();
  segs_[idx] = std::move(f);
  return raw;
}

void LogManager::Create() {
  fs_.MakeDirs(dir_);
  std::lock_guard<std::mutex> g(seg_mu_);
  bool created = false;
  File* f = Segment(kStart / opt_.segment_size, true, &created);
  f->WriteAt(0, std::string(kStart, '\0').data(), kStart);
  f->Sync();
  fs_.SyncDir(dir_);
  std::lock_guard<std::mutex> l(mu_);
  buf_.clear();
  buf_start_ = next_ = flushed_ = kStart;
}

Lsn LogManager::Append(const LogRecord& rec, const std::function<void(Lsn)>& locked) {
  std::string bytes = rec.Encode();
  std::lock_guard<std::mutex> g(mu_);
  if (panic_) Fail(state::kIo, "write-ahead log is unusable after an earlier I/O error");
  Lsn lsn = next_;
  PutU64(bytes.data() + 8, lsn);
  PutU32(bytes.data(), Crc32c(bytes.data() + 4, bytes.size() - 4));
  buf_.append(bytes);
  next_ += bytes.size();
  if (locked) locked(lsn);
  return lsn;
}

Lsn LogManager::AppendCheckpoint(const std::function<void(LogRecord&)>& fill) {
  std::lock_guard<std::mutex> g(mu_);
  if (panic_) Fail(state::kIo, "write-ahead log is unusable after an earlier I/O error");
  LogRecord rec;
  rec.type = LogType::Checkpoint;
  fill(rec);
  rec.lsn = next_;
  std::string bytes = rec.Encode();
  PutU32(bytes.data(), Crc32c(bytes.data() + 4, bytes.size() - 4));
  buf_.append(bytes);
  next_ += bytes.size();
  return rec.lsn;
}

void LogManager::Flush(Lsn lsn) {
  std::unique_lock<std::mutex> l(mu_);
  while (flushed_ <= lsn) {
    if (panic_) Fail(state::kIo, "write-ahead log is unusable after an earlier I/O error");
    if (flushing_) {
      cv_.wait(l);
      continue;
    }
    if (next_ == flushed_) return;
    flushing_ = true;
    if (opt_.group_delay_us > 0) {
      l.unlock();
      SleepMicros(opt_.group_delay_us);
      l.lock();
    }
    std::string data;
    data.swap(buf_);
    Lsn start = buf_start_;
    Lsn end = next_;
    buf_start_ = end;
    l.unlock();
    try {
      WriteBytes(start, data);
    } catch (...) {
      l.lock();
      panic_ = true;
      flushing_ = false;
      cv_.notify_all();
      throw;
    }
    l.lock();
    flushed_ = end;
    flushing_ = false;
    bytes_written_ += data.size();
    flushes_++;
    cv_.notify_all();
  }
}

void LogManager::WriteBytes(Lsn pos, const std::string& data) {
  std::lock_guard<std::mutex> g(seg_mu_);
  cache_.clear();
  bool created = false;
  std::vector<File*> touched;
  size_t done = 0;
  while (done < data.size()) {
    Lsn at = pos + done;
    uint64_t idx = at / opt_.segment_size;
    uint64_t off = at % opt_.segment_size;
    size_t n = static_cast<size_t>(std::min<uint64_t>(opt_.segment_size - off, data.size() - done));
    File* f = Segment(idx, true, &created);
    f->WriteAt(off, data.data() + done, n);
    if (std::find(touched.begin(), touched.end(), f) == touched.end()) touched.push_back(f);
    done += n;
  }
  if (opt_.fsync) {
    for (File* f : touched) f->Sync();
    if (created) fs_.SyncDir(dir_);
  } else if (created) {
    fs_.SyncDir(dir_);
  }
}

Lsn LogManager::next_lsn() {
  std::lock_guard<std::mutex> g(mu_);
  return next_;
}

Lsn LogManager::flushed_lsn() {
  std::lock_guard<std::mutex> g(mu_);
  return flushed_;
}

size_t LogManager::ReadRaw(Lsn pos, char* buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    Lsn at = pos + done;
    uint64_t idx = at / opt_.segment_size;
    uint64_t off = at % opt_.segment_size;
    size_t want = static_cast<size_t>(std::min<uint64_t>(opt_.segment_size - off, n - done));
    File* f = Segment(idx, false, nullptr);
    if (!f) break;
    size_t got = f->ReadAt(off, buf + done, want);
    done += got;
    if (got < want) break;
  }
  return done;
}

size_t LogManager::ReadBytes(Lsn pos, char* buf, size_t n) {
  std::lock_guard<std::mutex> g(seg_mu_);
  if (pos < cache_pos_ || pos + n > cache_pos_ + cache_.size()) {
    cache_.resize(std::max<size_t>(n, 1u << 20));
    cache_.resize(ReadRaw(pos, cache_.data(), cache_.size()));
    cache_pos_ = pos;
  }
  size_t k = std::min<size_t>(n, static_cast<size_t>(cache_pos_ + cache_.size() - pos));
  std::memcpy(buf, cache_.data() + (pos - cache_pos_), k);
  return k;
}

bool LogManager::Read(Lsn lsn, LogRecord* out, Lsn* end) {
  char hdr[kHeader];
  if (ReadBytes(lsn, hdr, kHeader) < kHeader) return false;
  uint32_t len = GetU32(hdr + 4);
  if (len < kHeader || len > kMaxRecord) return false;
  if (GetU64(hdr + 8) != lsn) return false;
  std::string bytes(len, '\0');
  if (ReadBytes(lsn, bytes.data(), len) < len) return false;
  if (!LogRecord::Decode(bytes, out)) return false;
  if (end) *end = lsn + len;
  return true;
}

void LogManager::Reopen(Lsn end) {
  {
    std::lock_guard<std::mutex> g(seg_mu_);
    cache_.clear();
    uint64_t last = end / opt_.segment_size;
    for (auto& name : fs_.List(dir_)) {
      if (name.size() != 20 || name.substr(16) != ".wal") continue;
      uint64_t idx = std::stoull(name.substr(0, 16), nullptr, 16);
      if (idx > last) {
        segs_.erase(idx);
        fs_.Remove(dir_ + "/" + name);
      }
    }
    bool created = false;
    File* f = Segment(last, true, &created);
    uint64_t off = end % opt_.segment_size;
    uint64_t size = f->Size();
    if (size > off) {
      std::string zeros(static_cast<size_t>(size - off), '\0');
      f->WriteAt(off, zeros.data(), zeros.size());
    }
    f->Sync();
    fs_.SyncDir(dir_);
  }
  std::lock_guard<std::mutex> g(mu_);
  buf_.clear();
  buf_start_ = next_ = flushed_ = end;
}

void LogManager::TruncateBefore(Lsn lsn) {
  std::lock_guard<std::mutex> g(seg_mu_);
  cache_.clear();
  for (auto& name : fs_.List(dir_)) {
    if (name.size() != 20 || name.substr(16) != ".wal") continue;
    uint64_t idx = std::stoull(name.substr(0, 16), nullptr, 16);
    if ((idx + 1) * opt_.segment_size <= lsn) {
      segs_.erase(idx);
      fs_.Remove(dir_ + "/" + name);
    }
  }
}

size_t LogManager::SegmentCount() {
  size_t n = 0;
  for (auto& name : fs_.List(dir_)) {
    if (name.size() == 20 && name.substr(16) == ".wal") n++;
  }
  return n;
}

void LogManager::WriteControl(Lsn checkpoint) {
  control_seq_++;
  char rec[32] = {};
  PutU64(rec, kControlMagic);
  PutU64(rec + 8, control_seq_);
  PutU64(rec + 16, checkpoint);
  PutU32(rec + 24, Crc32c(rec, 24));
  std::string path = data_dir_ + "/control";
  bool exists = fs_.Exists(path);
  auto f = fs_.Open(path, true);
  f->WriteAt((control_seq_ % 2) * 512, rec, sizeof(rec));
  f->Sync();
  if (!exists) fs_.SyncDir(data_dir_);
}

bool LogManager::ReadControl(Lsn* checkpoint) {
  std::string path = data_dir_ + "/control";
  if (!fs_.Exists(path)) return false;
  auto f = fs_.Open(path, false);
  bool found = false;
  uint64_t best = 0;
  for (int slot = 0; slot < 2; slot++) {
    char rec[32] = {};
    if (f->ReadAt(static_cast<uint64_t>(slot) * 512, rec, sizeof(rec)) < sizeof(rec)) continue;
    if (GetU64(rec) != kControlMagic || GetU32(rec + 24) != Crc32c(rec, 24)) continue;
    uint64_t seq = GetU64(rec + 8);
    if (!found || seq > best) {
      found = true;
      best = seq;
      *checkpoint = GetU64(rec + 16);
    }
  }
  if (found) control_seq_ = best;
  return found;
}

}
