#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rdbms {

using PageId = uint32_t;
using Lsn = uint64_t;
using Xid = uint64_t;
using Ts = uint64_t;

constexpr size_t kPageSize = 8192;
constexpr PageId kNoPage = 0xFFFFFFFFu;
constexpr PageId kMetaPage = 0;
constexpr PageId kCatalogRoot = 1;
constexpr uint32_t kCatalogTree = 1;

namespace state {
inline constexpr const char* kSerialization = "40001";
inline constexpr const char* kDeadlock = "40P01";
inline constexpr const char* kLockTimeout = "55P03";
inline constexpr const char* kUnique = "23505";
inline constexpr const char* kNotNull = "23502";
inline constexpr const char* kSyntax = "42601";
inline constexpr const char* kNoTable = "42P01";
inline constexpr const char* kNoColumn = "42703";
inline constexpr const char* kDupTable = "42P07";
inline constexpr const char* kDupColumn = "42701";
inline constexpr const char* kAmbiguous = "42702";
inline constexpr const char* kGrouping = "42803";
inline constexpr const char* kTypeMismatch = "42804";
inline constexpr const char* kUndefinedFunc = "42883";
inline constexpr const char* kNoParam = "42P02";
inline constexpr const char* kOutOfRange = "22003";
inline constexpr const char* kDivZero = "22012";
inline constexpr const char* kBadText = "22P02";
inline constexpr const char* kTooLong = "22001";
inline constexpr const char* kFailedTxn = "25P02";
inline constexpr const char* kActiveTxn = "25001";
inline constexpr const char* kReadOnly = "25006";
inline constexpr const char* kNotSupported = "0A000";
inline constexpr const char* kTooMany = "53300";
inline constexpr const char* kCanceled = "57014";
inline constexpr const char* kShutdown = "57P01";
inline constexpr const char* kProtocol = "08P01";
inline constexpr const char* kLimit = "54000";
inline constexpr const char* kIo = "58030";
inline constexpr const char* kCorrupt = "XX001";
inline constexpr const char* kInternal = "XX000";
}

class DbError : public std::runtime_error {
 public:
  DbError(std::string code, const std::string& msg)
      : std::runtime_error(msg), code_(std::move(code)) {}
  const std::string& code() const { return code_; }

 private:
  std::string code_;
};

[[noreturn]] void Fail(const char* code, const std::string& msg);

#ifdef _WIN32
class SharedMutex {
 public:
  void lock();
  void unlock();
  bool try_lock();
  void lock_shared();
  void unlock_shared();
  bool try_lock_shared();

 private:
  void* srw_ = nullptr;
};
#else
using SharedMutex = std::shared_mutex;
#endif

inline void PutU16(char* p, uint16_t v) {
  p[0] = static_cast<char>(v);
  p[1] = static_cast<char>(v >> 8);
}
inline void PutU32(char* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<char>(v >> (8 * i));
}
inline void PutU64(char* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = static_cast<char>(v >> (8 * i));
}
inline uint16_t GetU16(const char* p) {
  return static_cast<uint16_t>(static_cast<uint8_t>(p[0]) |
                               (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8));
}
inline uint32_t GetU32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}
inline uint64_t GetU64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

class Writer {
 public:
  explicit Writer(std::string& out) : out_(out) {}
  void U8(uint8_t v) { out_.push_back(static_cast<char>(v)); }
  void U16(uint16_t v) {
    char b[2];
    PutU16(b, v);
    out_.append(b, 2);
  }
  void U32(uint32_t v) {
    char b[4];
    PutU32(b, v);
    out_.append(b, 4);
  }
  void U64(uint64_t v) {
    char b[8];
    PutU64(b, v);
    out_.append(b, 8);
  }
  void Str(std::string_view s) {
    U32(static_cast<uint32_t>(s.size()));
    out_.append(s.data(), s.size());
  }
  void Raw(std::string_view s) { out_.append(s.data(), s.size()); }

 private:
  std::string& out_;
};

class Reader {
 public:
  Reader(const char* p, size_t n) : p_(p), end_(p + n) {}
  explicit Reader(std::string_view s) : p_(s.data()), end_(s.data() + s.size()) {}
  uint8_t U8() {
    Need(1);
    return static_cast<uint8_t>(*p_++);
  }
  uint16_t U16() {
    Need(2);
    uint16_t v = GetU16(p_);
    p_ += 2;
    return v;
  }
  uint32_t U32() {
    Need(4);
    uint32_t v = GetU32(p_);
    p_ += 4;
    return v;
  }
  uint64_t U64() {
    Need(8);
    uint64_t v = GetU64(p_);
    p_ += 8;
    return v;
  }
  std::string Str() {
    uint32_t n = U32();
    Need(n);
    std::string s(p_, n);
    p_ += n;
    return s;
  }
  std::string_view View(size_t n) {
    Need(n);
    std::string_view s(p_, n);
    p_ += n;
    return s;
  }
  size_t left() const { return static_cast<size_t>(end_ - p_); }
  bool done() const { return p_ == end_; }

 private:
  void Need(size_t n) const {
    if (static_cast<size_t>(end_ - p_) < n) Fail(state::kCorrupt, "truncated record");
  }
  const char* p_;
  const char* end_;
};

uint32_t Crc32c(const void* data, size_t n, uint32_t crc = 0);

class Rng {
 public:
  explicit Rng(uint64_t seed);
  uint64_t Next();
  uint64_t Uniform(uint64_t n) { return n == 0 ? 0 : Next() % n; }
  double Real() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
  bool Chance(double p) { return Real() < p; }

 private:
  uint64_t s_[4];
};

uint64_t NowMicros();
int64_t WallMicros();
void SleepMicros(uint64_t us);

void SetLogQuiet(bool quiet);
void LogInfo(const char* fmt, ...);

int CompareKeys(std::string_view a, std::string_view b);

std::string Lower(std::string_view s);
std::string Trim(std::string_view s);

}
