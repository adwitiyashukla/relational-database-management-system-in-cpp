#include "fs.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace rdbms {

namespace stdfs = std::filesystem;

namespace {

#ifdef _WIN32

std::string WinError(const std::string& what) {
  return what + ": windows error " + std::to_string(GetLastError());
}

class DiskFile : public File {
 public:
  DiskFile(HANDLE h, std::string path) : h_(h), path_(std::move(path)) {}
  ~DiskFile() override { CloseHandle(h_); }

  size_t ReadAt(uint64_t off, char* buf, size_t n) override {
    size_t done = 0;
    while (done < n) {
      OVERLAPPED ov{};
      uint64_t at = off + done;
      ov.Offset = static_cast<DWORD>(at);
      ov.OffsetHigh = static_cast<DWORD>(at >> 32);
      DWORD got = 0;
      if (!ReadFile(h_, buf + done, static_cast<DWORD>(n - done), &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) break;
        Fail(state::kIo, WinError("read " + path_));
      }
      if (got == 0) break;
      done += got;
    }
    return done;
  }

  void WriteAt(uint64_t off, const char* buf, size_t n) override {
    size_t done = 0;
    while (done < n) {
      OVERLAPPED ov{};
      uint64_t at = off + done;
      ov.Offset = static_cast<DWORD>(at);
      ov.OffsetHigh = static_cast<DWORD>(at >> 32);
      DWORD put = 0;
      if (!WriteFile(h_, buf + done, static_cast<DWORD>(n - done), &put, &ov))
        Fail(state::kIo, WinError("write " + path_));
      done += put;
    }
  }

  void Sync() override {
    if (!FlushFileBuffers(h_)) Fail(state::kIo, WinError("flush " + path_));
  }

  uint64_t Size() override {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h_, &s)) Fail(state::kIo, WinError("size " + path_));
    return static_cast<uint64_t>(s.QuadPart);
  }

  void Truncate(uint64_t size) override {
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &info, sizeof(info)))
      Fail(state::kIo, WinError("truncate " + path_));
  }

 private:
  HANDLE h_;
  std::string path_;
};

#else

std::string PosixError(const std::string& what) { return what + ": " + std::strerror(errno); }

class DiskFile : public File {
 public:
  DiskFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~DiskFile() override { ::close(fd_); }

  size_t ReadAt(uint64_t off, char* buf, size_t n) override {
    size_t done = 0;
    while (done < n) {
      ssize_t r = ::pread(fd_, buf + done, n - done, static_cast<off_t>(off + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        Fail(state::kIo, PosixError("read " + path_));
      }
      if (r == 0) break;
      done += static_cast<size_t>(r);
    }
    return done;
  }

  void WriteAt(uint64_t off, const char* buf, size_t n) override {
    size_t done = 0;
    while (done < n) {
      ssize_t r = ::pwrite(fd_, buf + done, n - done, static_cast<off_t>(off + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        Fail(state::kIo, PosixError("write " + path_));
      }
      done += static_cast<size_t>(r);
    }
  }

  void Sync() override {
#if defined(__APPLE__)
    if (::fcntl(fd_, F_FULLFSYNC) == 0) return;
    if (::fsync(fd_) != 0) Fail(state::kIo, PosixError("fsync " + path_));
#elif defined(__linux__)
    if (::fdatasync(fd_) != 0) Fail(state::kIo, PosixError("fdatasync " + path_));
#else
    if (::fsync(fd_) != 0) Fail(state::kIo, PosixError("fsync " + path_));
#endif
  }

  uint64_t Size() override {
    struct stat st;
    if (::fstat(fd_, &st) != 0) Fail(state::kIo, PosixError("stat " + path_));
    return static_cast<uint64_t>(st.st_size);
  }

  void Truncate(uint64_t size) override {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0)
      Fail(state::kIo, PosixError("truncate " + path_));
  }

 private:
  int fd_;
  std::string path_;
};

#endif

class DiskFs : public FileSystem {
 public:
  std::unique_ptr<File> Open(const std::string& path, bool create) override {
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) Fail(state::kIo, WinError("open " + path));
    return std::make_unique<DiskFile>(h, path);
#else
    int fd = ::open(path.c_str(), O_RDWR | (create ? O_CREAT : 0), 0644);
    if (fd < 0) Fail(state::kIo, PosixError("open " + path));
    return std::make_unique<DiskFile>(fd, path);
#endif
  }

  bool Exists(const std::string& path) override {
    std::error_code ec;
    return stdfs::exists(path, ec);
  }

  void Remove(const std::string& path) override {
    std::error_code ec;
    stdfs::remove(path, ec);
    if (ec) Fail(state::kIo, "remove " + path + ": " + ec.message());
  }

  std::vector<std::string> List(const std::string& dir) override {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = stdfs::directory_iterator(dir, ec); !ec && it != stdfs::directory_iterator();
         it.increment(ec)) {
      out.push_back(it->path().filename().string());
    }
    return out;
  }

  void MakeDirs(const std::string& dir) override {
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    if (ec) Fail(state::kIo, "mkdir " + dir + ": " + ec.message());
  }

  void SyncDir(const std::string& dir) override {
#ifdef _WIN32
    (void)dir;
#else
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) Fail(state::kIo, PosixError("open dir " + dir));
    int r = ::fsync(fd);
    ::close(fd);
    if (r != 0 && errno != EINVAL) Fail(state::kIo, PosixError("fsync dir " + dir));
#endif
  }
};

std::string ParentDir(const std::string& path) {
  auto pos = path.rfind('/');
  return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

class SimFile : public File {
 public:
  SimFile(SimFs* fs, std::shared_ptr<SimFs::Node> node) : fs_(fs), node_(std::move(node)) {}

  size_t ReadAt(uint64_t off, char* buf, size_t n) override {
    std::lock_guard<std::mutex> g(fs_->mu());
    const std::string& c = node_->current;
    if (off >= c.size()) return 0;
    size_t k = std::min<size_t>(n, c.size() - static_cast<size_t>(off));
    std::memcpy(buf, c.data() + off, k);
    return k;
  }

  void WriteAt(uint64_t off, const char* buf, size_t n) override {
    std::lock_guard<std::mutex> g(fs_->mu());
    if (!fs_->BeginOpLocked()) return;
    std::string& c = node_->current;
    if (c.size() < off + n) c.resize(static_cast<size_t>(off + n), '\0');
    std::memcpy(c.data() + off, buf, n);
    node_->pending.push_back({false, off, std::string(buf, n)});
  }

  void Sync() override {
    std::lock_guard<std::mutex> g(fs_->mu());
    if (!fs_->BeginOpLocked()) return;
    node_->durable = node_->current;
    node_->pending.clear();
  }

  uint64_t Size() override {
    std::lock_guard<std::mutex> g(fs_->mu());
    return node_->current.size();
  }

  void Truncate(uint64_t size) override {
    std::lock_guard<std::mutex> g(fs_->mu());
    if (!fs_->BeginOpLocked()) return;
    node_->current.resize(static_cast<size_t>(size), '\0');
    node_->pending.push_back({true, size, std::string()});
  }

 private:
  SimFs* fs_;
  std::shared_ptr<SimFs::Node> node_;
};

}

std::unique_ptr<FileSystem> NewDiskFs() { return std::make_unique<DiskFs>(); }

SimFs::SimFs(uint64_t seed) : rng_(seed) {}

std::unique_ptr<File> SimFs::Open(const std::string& path, bool create) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = files_.find(path);
  if (it == files_.end()) {
    if (!create) Fail(state::kIo, "open " + path + ": no such file");
    if (crashed_) return std::make_unique<SimFile>(this, std::make_shared<Node>());
    it = files_.emplace(path, std::make_shared<Node>()).first;
  }
  return std::make_unique<SimFile>(this, it->second);
}

bool SimFs::Exists(const std::string& path) {
  std::lock_guard<std::mutex> g(mu_);
  return files_.count(path) > 0 || dirs_.count(path) > 0;
}

void SimFs::Remove(const std::string& path) {
  std::lock_guard<std::mutex> g(mu_);
  if (crashed_) return;
  files_.erase(path);
}

std::vector<std::string> SimFs::List(const std::string& dir) {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<std::string> out;
  for (auto& [p, n] : files_) {
    if (ParentDir(p) == dir) out.push_back(p.substr(dir.size() + 1));
  }
  return out;
}

void SimFs::MakeDirs(const std::string& dir) {
  std::lock_guard<std::mutex> g(mu_);
  dirs_.insert(dir);
}

void SimFs::SyncDir(const std::string& dir) {
  std::lock_guard<std::mutex> g(mu_);
  if (!BeginOpLocked()) return;
  for (auto& [p, n] : files_) {
    if (ParentDir(p) == dir) n->created_durable = true;
  }
}

bool SimFs::BeginOpLocked() {
  if (crashed_) return false;
  ops_++;
  if (armed_ && ops_ >= crash_at_) {
    CrashLocked();
    throw SimCrash();
  }
  return true;
}

void SimFs::ArmCrash(uint64_t at_op) {
  std::lock_guard<std::mutex> g(mu_);
  armed_ = true;
  crash_at_ = at_op;
}

void SimFs::CrashNow() {
  std::lock_guard<std::mutex> g(mu_);
  if (!crashed_) CrashLocked();
}

void SimFs::CrashLocked() {
  crashed_ = true;
  armed_ = false;
  for (auto it = files_.begin(); it != files_.end();) {
    Node& n = *it->second;
    if (!n.created_durable) {
      stats_.lost_files++;
      it = files_.erase(it);
      continue;
    }
    std::string r = n.durable;
    for (auto& op : n.pending) {
      if (op.trunc) {
        if (rng_.Chance(0.5)) r.resize(static_cast<size_t>(op.off), '\0');
        continue;
      }
      uint64_t pick = rng_.Uniform(10);
      if (pick < 4) {
        stats_.kept++;
        if (r.size() < op.off + op.data.size()) r.resize(op.off + op.data.size(), '\0');
        std::memcpy(r.data() + op.off, op.data.data(), op.data.size());
      } else if (pick < 7) {
        stats_.dropped++;
      } else {
        stats_.torn++;
        uint64_t end = op.off + op.data.size();
        uint64_t s = op.off;
        while (s < end) {
          uint64_t e = std::min<uint64_t>((s / 512 + 1) * 512, end);
          if (rng_.Chance(0.5)) {
            if (r.size() < e) r.resize(static_cast<size_t>(e), '\0');
            std::memcpy(r.data() + s, op.data.data() + (s - op.off), static_cast<size_t>(e - s));
          }
          s = e;
        }
      }
    }
    n.durable = r;
    n.current = r;
    n.pending.clear();
    ++it;
  }
}

void SimFs::Restart() {
  std::lock_guard<std::mutex> g(mu_);
  crashed_ = false;
  armed_ = false;
  ops_ = 0;
}

bool SimFs::crashed() {
  std::lock_guard<std::mutex> g(mu_);
  return crashed_;
}

uint64_t SimFs::ops() {
  std::lock_guard<std::mutex> g(mu_);
  return ops_;
}

SimFs::Stats SimFs::stats() {
  std::lock_guard<std::mutex> g(mu_);
  return stats_;
}

}
