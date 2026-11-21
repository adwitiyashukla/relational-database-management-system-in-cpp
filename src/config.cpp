#include "config.h"

#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "common.h"

namespace rdbms {

namespace {

struct ConfigValue {
  enum Kind { kString, kInt, kBool } kind;
  std::string s;
  long long i = 0;
  bool b = false;
};

[[noreturn]] void Bad(int line, const std::string& msg) {
  Fail(state::kInternal, "config line " + std::to_string(line) + ": " + msg);
}

ConfigValue ParseValue(const std::string& raw, int line) {
  ConfigValue v{};
  if (raw.empty()) Bad(line, "missing value");
  if (raw[0] == '"') {
    v.kind = ConfigValue::kString;
    size_t i = 1;
    for (; i < raw.size() && raw[i] != '"'; i++) {
      if (raw[i] == '\\' && i + 1 < raw.size()) i++;
      v.s.push_back(raw[i]);
    }
    if (i >= raw.size()) Bad(line, "unterminated string");
    if (!Trim(raw.substr(i + 1)).empty()) Bad(line, "unexpected text after string");
    return v;
  }
  if (raw == "true" || raw == "false") {
    v.kind = ConfigValue::kBool;
    v.b = raw == "true";
    return v;
  }
  std::string digits;
  for (char c : raw) {
    if (c != '_') digits.push_back(c);
  }
  size_t start = digits[0] == '-' ? 1 : 0;
  if (start == digits.size() || digits.size() > 18) Bad(line, "bad value '" + raw + "'");
  for (size_t i = start; i < digits.size(); i++) {
    if (digits[i] < '0' || digits[i] > '9') Bad(line, "bad value '" + raw + "'");
  }
  v.kind = ConfigValue::kInt;
  v.i = std::stoll(digits);
  return v;
}

std::string StripComment(const std::string& s) {
  bool in_str = false;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) in_str = !in_str;
    if (s[i] == '#' && !in_str) return s.substr(0, i);
  }
  return s;
}

}

Config ParseConfig(std::string_view text) {
  Config c;
  using Setter = std::function<void(const ConfigValue&, int)>;
  auto str = [](std::string* dst) -> Setter {
    return [dst](const ConfigValue& v, int line) {
      if (v.kind != ConfigValue::kString) Bad(line, "expected a string");
      *dst = v.s;
    };
  };
  auto num = [](int* dst, long long lo, long long hi) -> Setter {
    return [dst, lo, hi](const ConfigValue& v, int line) {
      if (v.kind != ConfigValue::kInt) Bad(line, "expected an integer");
      if (v.i < lo || v.i > hi)
        Bad(line, "value must be between " + std::to_string(lo) + " and " + std::to_string(hi));
      *dst = static_cast<int>(v.i);
    };
  };
  auto flag = [](bool* dst) -> Setter {
    return [dst](const ConfigValue& v, int line) {
      if (v.kind != ConfigValue::kBool) Bad(line, "expected true or false");
      *dst = v.b;
    };
  };
  std::map<std::string, Setter> keys = {
      {"server.host", str(&c.host)},
      {"server.port", num(&c.port, 0, 65535)},
      {"server.max_connections", num(&c.max_connections, 1, 10000)},
      {"storage.data_dir", str(&c.data_dir)},
      {"storage.buffer_pool_mb", num(&c.buffer_pool_mb, 1, 65536)},
      {"wal.segment_mb", num(&c.wal_segment_mb, 1, 1024)},
      {"wal.fsync", flag(&c.fsync)},
      {"wal.group_commit_delay_us", num(&c.group_commit_delay_us, 0, 100000)},
      {"checkpoint.interval_s", num(&c.checkpoint_interval_s, 1, 86400)},
      {"checkpoint.wal_mb", num(&c.checkpoint_wal_mb, 1, 65536)},
      {"txn.default_isolation", str(&c.default_isolation)},
      {"txn.lock_timeout_ms", num(&c.lock_timeout_ms, 1, 3600000)},
      {"gc.interval_ms", num(&c.gc_interval_ms, 1, 60000)},
  };
  std::istringstream in{std::string(text)};
  std::string raw;
  std::string section;
  int line = 0;
  while (std::getline(in, raw)) {
    line++;
    std::string s = Trim(StripComment(raw));
    if (s.empty()) continue;
    if (s.front() == '[') {
      if (s.back() != ']') Bad(line, "bad section header");
      section = Trim(s.substr(1, s.size() - 2));
      continue;
    }
    auto eq = s.find('=');
    if (eq == std::string::npos) Bad(line, "expected key = value");
    std::string key = Trim(s.substr(0, eq));
    std::string full = section.empty() ? key : section + "." + key;
    auto it = keys.find(full);
    if (it == keys.end()) Bad(line, "unknown setting '" + full + "'");
    it->second(ParseValue(Trim(s.substr(eq + 1)), line), line);
  }
  if (c.host.empty()) Fail(state::kInternal, "config: server.host must not be empty");
  if (c.data_dir.empty()) Fail(state::kInternal, "config: storage.data_dir must not be empty");
  std::string iso = Lower(c.default_isolation);
  if (iso != "read committed" && iso != "repeatable read" && iso != "serializable")
    Fail(state::kInternal,
         "config: txn.default_isolation must be read committed, repeatable read or serializable");
  c.default_isolation = iso;
  return c;
}

Config LoadConfig(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) Fail(state::kIo, "cannot open config file " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ParseConfig(ss.str());
}

}
