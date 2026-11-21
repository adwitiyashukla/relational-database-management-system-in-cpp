#pragma once

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

#include "common.h"

namespace testing {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}

struct Reg {
  Reg(const char* n, void (*f)()) { Registry().push_back({n, f}); }
};

struct Failure : std::exception {
  std::string msg;
  explicit Failure(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

inline std::string Where(const char* file, int line) {
  const char* base = std::strrchr(file, '/');
  if (!base) base = std::strrchr(file, '\\');
  return std::string(base ? base + 1 : file) + ":" + std::to_string(line);
}

template <class A, class B>
void CheckEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  if (a == b) return;
  std::ostringstream os;
  os << Where(file, line) << ": expected " << ea << " == " << eb << " but got " << a << " vs " << b;
  throw Failure(os.str());
}

}

#define TEST(name)                             \
  static void name();                          \
  static testing::Reg reg_##name(#name, name); \
  static void name()

#define CHECK(c)                                                                              \
  do {                                                                                        \
    if (!(c)) throw testing::Failure(testing::Where(__FILE__, __LINE__) + ": " #c " failed"); \
  } while (0)

#define CHECK_EQ(a, b) testing::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_CODE(stmt, expected)                                                                 \
  do {                                                                                             \
    bool thrown_ = false;                                                                          \
    try {                                                                                          \
      stmt;                                                                                        \
    } catch (const rdbms::DbError& e_) {                                                           \
      thrown_ = true;                                                                              \
      if (e_.code() != std::string(expected))                                                      \
        throw testing::Failure(testing::Where(__FILE__, __LINE__) + ": expected " +                \
                               std::string(expected) + " but got " + e_.code() + " " + e_.what()); \
    }                                                                                              \
    if (!thrown_)                                                                                  \
      throw testing::Failure(testing::Where(__FILE__, __LINE__) + ": expected error " +            \
                             std::string(expected));                                               \
  } while (0)

int main(int argc, char** argv) {
  rdbms::SetLogQuiet(true);
  std::string filter = argc > 1 ? argv[1] : "";
  int failed = 0, ran = 0;
  for (auto& c : testing::Registry()) {
    if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
    ran++;
    auto t0 = std::chrono::steady_clock::now();
    try {
      c.fn();
      double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::printf("ok   %-44s %8.1f ms\n", c.name, ms);
    } catch (const std::exception& e) {
      failed++;
      std::printf("FAIL %-44s %s\n", c.name, e.what());
    }
    std::fflush(stdout);
  }
  std::printf("%d of %d tests passed\n", ran - failed, ran);
  return failed == 0 ? 0 : 1;
}
