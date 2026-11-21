#include "common.h"

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rdbms {

#ifdef _WIN32
static_assert(sizeof(SRWLOCK) == sizeof(void*), "SRWLOCK must fit in a pointer");

void SharedMutex::lock() { AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&srw_)); }
void SharedMutex::unlock() { ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&srw_)); }
bool SharedMutex::try_lock() {
  return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&srw_)) != 0;
}
void SharedMutex::lock_shared() { AcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(&srw_)); }
void SharedMutex::unlock_shared() { ReleaseSRWLockShared(reinterpret_cast<PSRWLOCK>(&srw_)); }
bool SharedMutex::try_lock_shared() {
  return TryAcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(&srw_)) != 0;
}
#endif

void Fail(const char* code, const std::string& msg) { throw DbError(code, msg); }

namespace {

constexpr std::array<std::array<uint32_t, 256>, 8> MakeCrcTables() {
  std::array<std::array<uint32_t, 256>, 8> t{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    t[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; i++) {
    for (int s = 1; s < 8; s++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
  }
  return t;
}

const auto kCrc = MakeCrcTables();

std::atomic<bool> g_quiet{false};
std::mutex g_log_mu;

}

uint32_t Crc32c(const void* data, size_t n, uint32_t crc) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
  while (n >= 8) {
    uint32_t lo = c ^ (static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24);
    uint32_t hi = static_cast<uint32_t>(p[4]) | static_cast<uint32_t>(p[5]) << 8 |
                  static_cast<uint32_t>(p[6]) << 16 | static_cast<uint32_t>(p[7]) << 24;
    c = kCrc[7][lo & 0xFF] ^ kCrc[6][(lo >> 8) & 0xFF] ^ kCrc[5][(lo >> 16) & 0xFF] ^
        kCrc[4][lo >> 24] ^ kCrc[3][hi & 0xFF] ^ kCrc[2][(hi >> 8) & 0xFF] ^
        kCrc[1][(hi >> 16) & 0xFF] ^ kCrc[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n--) c = kCrc[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}

Rng::Rng(uint64_t seed) {
  uint64_t x = seed;
  for (auto& s : s_) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    s = z ^ (z >> 31);
  }
}

uint64_t Rng::Next() {
  auto rotl = [](uint64_t v, int k) { return (v << k) | (v >> (64 - k)); };
  uint64_t r = rotl(s_[1] * 5, 7) * 9;
  uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return r;
}

uint64_t NowMicros() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

int64_t WallMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void SleepMicros(uint64_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

void SetLogQuiet(bool quiet) { g_quiet.store(quiet); }

void LogInfo(const char* fmt, ...) {
  if (g_quiet.load()) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  int64_t now = WallMicros() / 1000;
  std::lock_guard<std::mutex> g(g_log_mu);
  std::fprintf(stderr, "[%02d:%02d:%02d.%03d] %s\n", static_cast<int>(now / 3600000 % 24),
               static_cast<int>(now / 60000 % 60), static_cast<int>(now / 1000 % 60),
               static_cast<int>(now % 1000), buf);
}

int CompareKeys(std::string_view a, std::string_view b) {
  size_t n = a.size() < b.size() ? a.size() : b.size();
  int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (c != 0) return c < 0 ? -1 : 1;
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

std::string Lower(std::string_view s) {
  std::string r(s);
  for (auto& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

std::string Trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    e--;
  return std::string(s.substr(b, e - b));
}

}
