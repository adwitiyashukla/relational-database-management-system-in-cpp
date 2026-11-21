#include "value.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace rdbms {

namespace {

constexpr int64_t kPgEpochOffset = 946684800LL * 1000000LL;

int64_t FloorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yy + (*m <= 2 ? 1 : 0);
}

uint64_t DoubleBits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}

double BitsDouble(uint64_t b) {
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}

void PutBE(std::string& out, uint64_t v, int bytes) {
  for (int i = bytes - 1; i >= 0; i--) out.push_back(static_cast<char>(v >> (8 * i)));
}

uint64_t GetBE(std::string_view s) {
  uint64_t v = 0;
  for (char c : s) v = (v << 8) | static_cast<uint8_t>(c);
  return v;
}

[[noreturn]] void BadInput(const char* type, std::string_view text) {
  Fail(state::kBadText,
       std::string("invalid input syntax for type ") + type + ": \"" + std::string(text) + "\"");
}

}

Value Value::Bool(bool b) {
  Value v;
  v.type = Type::Bool;
  v.i = b ? 1 : 0;
  return v;
}

Value Value::Int(int64_t x) {
  Value v;
  v.type = Type::Int;
  v.i = x;
  return v;
}

Value Value::Float(double x) {
  Value v;
  v.type = Type::Float;
  v.f = x;
  return v;
}

Value Value::Text(std::string x) {
  Value v;
  v.type = Type::Text;
  v.s = std::move(x);
  return v;
}

Value Value::Time(int64_t micros) {
  Value v;
  v.type = Type::Timestamp;
  v.i = micros;
  return v;
}

const char* TypeName(Type t) {
  switch (t) {
    case Type::Null:
      return "unknown";
    case Type::Bool:
      return "boolean";
    case Type::Int:
      return "bigint";
    case Type::Float:
      return "double precision";
    case Type::Text:
      return "text";
    case Type::Timestamp:
      return "timestamp";
  }
  return "unknown";
}

Type StorageType(ColType t) {
  switch (t) {
    case ColType::Bool:
      return Type::Bool;
    case ColType::SmallInt:
    case ColType::Int:
    case ColType::BigInt:
      return Type::Int;
    case ColType::Real:
    case ColType::Double:
    case ColType::Numeric:
      return Type::Float;
    case ColType::Text:
    case ColType::Varchar:
    case ColType::Char:
      return Type::Text;
    case ColType::Timestamp:
      return Type::Timestamp;
  }
  return Type::Text;
}

uint32_t TypeOid(ColType t) {
  switch (t) {
    case ColType::Bool:
      return 16;
    case ColType::SmallInt:
      return 21;
    case ColType::Int:
      return 23;
    case ColType::BigInt:
      return 20;
    case ColType::Real:
      return 700;
    case ColType::Double:
    case ColType::Numeric:
      return 701;
    case ColType::Text:
      return 25;
    case ColType::Varchar:
      return 1043;
    case ColType::Char:
      return 1042;
    case ColType::Timestamp:
      return 1114;
  }
  return 25;
}

int16_t TypeSize(ColType t) {
  switch (t) {
    case ColType::Bool:
      return 1;
    case ColType::SmallInt:
      return 2;
    case ColType::Int:
    case ColType::Real:
      return 4;
    case ColType::BigInt:
    case ColType::Double:
    case ColType::Numeric:
    case ColType::Timestamp:
      return 8;
    default:
      return -1;
  }
}

std::string ColTypeName(const Column& c) {
  switch (c.type) {
    case ColType::Bool:
      return "boolean";
    case ColType::SmallInt:
      return "smallint";
    case ColType::Int:
      return "integer";
    case ColType::BigInt:
      return "bigint";
    case ColType::Real:
      return "real";
    case ColType::Double:
      return "double precision";
    case ColType::Numeric:
      return "numeric";
    case ColType::Text:
      return "text";
    case ColType::Varchar:
      return c.len > 0 ? "character varying(" + std::to_string(c.len) + ")" : "character varying";
    case ColType::Char:
      return "character(" + std::to_string(c.len > 0 ? c.len : 1) + ")";
    case ColType::Timestamp:
      return "timestamp without time zone";
  }
  return "text";
}

bool ParseColType(const std::string& raw, ColType* out) {
  std::string n = Lower(raw);
  if (n == "bool" || n == "boolean") {
    *out = ColType::Bool;
  } else if (n == "smallint" || n == "int2") {
    *out = ColType::SmallInt;
  } else if (n == "int" || n == "integer" || n == "int4") {
    *out = ColType::Int;
  } else if (n == "bigint" || n == "int8") {
    *out = ColType::BigInt;
  } else if (n == "real" || n == "float4") {
    *out = ColType::Real;
  } else if (n == "double precision" || n == "float8" || n == "float" || n == "double") {
    *out = ColType::Double;
  } else if (n == "numeric" || n == "decimal") {
    *out = ColType::Numeric;
  } else if (n == "text") {
    *out = ColType::Text;
  } else if (n == "varchar" || n == "character varying") {
    *out = ColType::Varchar;
  } else if (n == "char" || n == "character" || n == "bpchar") {
    *out = ColType::Char;
  } else if (n == "timestamp" || n == "timestamp without time zone") {
    *out = ColType::Timestamp;
  } else {
    return false;
  }
  return true;
}

std::string EncodeRow(const std::vector<Value>& row) {
  std::string out;
  Writer w(out);
  w.U16(static_cast<uint16_t>(row.size()));
  for (auto& v : row) {
    w.U8(static_cast<uint8_t>(v.type));
    switch (v.type) {
      case Type::Null:
        break;
      case Type::Bool:
        w.U8(v.i ? 1 : 0);
        break;
      case Type::Int:
      case Type::Timestamp:
        w.U64(static_cast<uint64_t>(v.i));
        break;
      case Type::Float:
        w.U64(DoubleBits(v.f));
        break;
      case Type::Text:
        w.Str(v.s);
        break;
    }
  }
  return out;
}

std::vector<Value> DecodeRow(std::string_view data) {
  Reader r(data);
  uint16_t n = r.U16();
  std::vector<Value> row(n);
  for (uint16_t i = 0; i < n; i++) {
    Type t = static_cast<Type>(r.U8());
    Value& v = row[i];
    v.type = t;
    switch (t) {
      case Type::Null:
        break;
      case Type::Bool:
        v.i = r.U8();
        break;
      case Type::Int:
      case Type::Timestamp:
        v.i = static_cast<int64_t>(r.U64());
        break;
      case Type::Float:
        v.f = BitsDouble(r.U64());
        break;
      case Type::Text:
        v.s = r.Str();
        break;
      default:
        Fail(state::kCorrupt, "bad value tag in stored row");
    }
  }
  return row;
}

std::string GroupKey(const std::vector<Value>& vals) { return EncodeRow(vals); }

std::string EncodeKey(const std::vector<Value>& vals) {
  std::string out;
  for (auto& v : vals) {
    switch (v.type) {
      case Type::Null:
        Fail(state::kNotNull, "primary key values must not be null");
      case Type::Bool:
        out.push_back(v.i ? 1 : 0);
        break;
      case Type::Int:
      case Type::Timestamp:
        PutBE(out, static_cast<uint64_t>(v.i) ^ 0x8000000000000000ull, 8);
        break;
      case Type::Float: {
        double d = v.f == 0 ? 0.0 : v.f;
        if (std::isnan(d)) d = std::numeric_limits<double>::quiet_NaN();
        uint64_t b = DoubleBits(d);
        b = (b & 0x8000000000000000ull) ? ~b : b | 0x8000000000000000ull;
        PutBE(out, b, 8);
        break;
      }
      case Type::Text:
        for (char c : v.s) {
          out.push_back(c);
          if (c == '\0') out.push_back(static_cast<char>(0xFF));
        }
        out.push_back('\0');
        out.push_back('\1');
        break;
    }
  }
  return out;
}

int64_t DecodeKeyInt(std::string_view key) {
  return static_cast<int64_t>(GetBE(key.substr(0, 8)) ^ 0x8000000000000000ull);
}

std::string KeySuccessor(std::string key) {
  while (!key.empty() && static_cast<uint8_t>(key.back()) == 0xFF) key.pop_back();
  if (key.empty()) return key;
  key.back() = static_cast<char>(static_cast<uint8_t>(key.back()) + 1);
  return key;
}

std::string EncodeSchema(const Schema& s) {
  std::string out;
  Writer w(out);
  w.Str(s.name);
  w.U16(static_cast<uint16_t>(s.cols.size()));
  for (auto& c : s.cols) {
    w.Str(c.name);
    w.U8(static_cast<uint8_t>(c.type));
    w.U8(c.not_null ? 1 : 0);
    w.U32(static_cast<uint32_t>(c.len));
  }
  w.U16(static_cast<uint16_t>(s.pk.size()));
  for (int k : s.pk) w.U16(static_cast<uint16_t>(k));
  w.U8(s.hidden_rowid ? 1 : 0);
  w.U32(s.root);
  return out;
}

Schema DecodeSchema(std::string_view data) {
  Reader r(data);
  Schema s;
  s.name = r.Str();
  uint16_t n = r.U16();
  for (uint16_t i = 0; i < n; i++) {
    Column c;
    c.name = r.Str();
    c.type = static_cast<ColType>(r.U8());
    c.not_null = r.U8() != 0;
    c.len = static_cast<int32_t>(r.U32());
    s.cols.push_back(c);
  }
  uint16_t k = r.U16();
  for (uint16_t i = 0; i < k; i++) s.pk.push_back(r.U16());
  s.hidden_rowid = r.U8() != 0;
  s.root = r.U32();
  return s;
}

std::string FormatDouble(double d) {
  if (std::isnan(d)) return "NaN";
  if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
  char buf[40];
  for (int prec = 15; prec <= 17; prec++) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
    if (std::strtod(buf, nullptr) == d) break;
  }
  return buf;
}

std::string FormatTimestamp(int64_t micros) {
  int64_t secs = FloorDiv(micros, 1000000);
  int64_t frac = micros - secs * 1000000;
  int64_t days = FloorDiv(secs, 86400);
  int64_t rem = secs - days * 86400;
  int64_t y;
  unsigned m, d;
  CivilFromDays(days, &y, &m, &d);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02d:%02d:%02d", static_cast<long long>(y), m,
                d, static_cast<int>(rem / 3600), static_cast<int>(rem % 3600 / 60),
                static_cast<int>(rem % 60));
  std::string out = buf;
  if (frac != 0) {
    char f[16];
    std::snprintf(f, sizeof(f), ".%06lld", static_cast<long long>(frac));
    std::string fs = f;
    while (fs.back() == '0') fs.pop_back();
    out += fs;
  }
  return out;
}

bool ParseTimestamp(std::string_view raw, int64_t* out) {
  std::string s = Trim(raw);
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  double sec = 0;
  char sep = 0;
  int n = std::sscanf(s.c_str(), "%d-%d-%d%c%d:%d:%lf", &y, &mo, &d, &sep, &h, &mi, &sec);
  if (n < 3) return false;
  if (n > 3 && sep != ' ' && sep != 'T') return false;
  if (n > 3 && n < 6) return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 ||
      sec >= 61)
    return false;
  int64_t days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  int64_t whole = static_cast<int64_t>(sec);
  int64_t micro = static_cast<int64_t>(std::llround((sec - static_cast<double>(whole)) * 1e6));
  *out = ((days * 86400 + h * 3600 + mi * 60 + whole) * 1000000) + micro;
  return true;
}

bool ParseInt(std::string_view raw, int64_t* out) {
  std::string s = Trim(raw);
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') {
    neg = s[0] == '-';
    i = 1;
  }
  if (i == s.size()) return false;
  uint64_t v = 0;
  for (; i < s.size(); i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    uint64_t next = v * 10 + static_cast<uint64_t>(s[i] - '0');
    if (next / 10 != v || next > 9223372036854775808ull) return false;
    v = next;
  }
  if (!neg && v > 9223372036854775807ull) return false;
  *out = neg ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v);
  return true;
}

bool ParseFloat(std::string_view raw, double* out) {
  std::string s = Lower(Trim(raw));
  if (s.empty()) return false;
  if (s == "nan") {
    *out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (s == "infinity" || s == "inf" || s == "+infinity") {
    *out = std::numeric_limits<double>::infinity();
    return true;
  }
  if (s == "-infinity" || s == "-inf") {
    *out = -std::numeric_limits<double>::infinity();
    return true;
  }
  char* end = nullptr;
  errno = 0;
  double d = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size() || errno == ERANGE) return false;
  *out = d;
  return true;
}

std::string ToText(const Value& v) {
  switch (v.type) {
    case Type::Null:
      return std::string();
    case Type::Bool:
      return v.i ? "t" : "f";
    case Type::Int:
      return std::to_string(v.i);
    case Type::Float:
      return FormatDouble(v.f);
    case Type::Text:
      return v.s;
    case Type::Timestamp:
      return FormatTimestamp(v.i);
  }
  return std::string();
}

std::string ToBinary(const Value& v, ColType t) {
  std::string out;
  switch (t) {
    case ColType::Bool:
      out.push_back(v.i ? 1 : 0);
      break;
    case ColType::SmallInt:
      PutBE(out, static_cast<uint64_t>(v.i), 2);
      break;
    case ColType::Int:
      PutBE(out, static_cast<uint64_t>(v.i), 4);
      break;
    case ColType::BigInt:
      PutBE(out, static_cast<uint64_t>(v.i), 8);
      break;
    case ColType::Real: {
      float f = static_cast<float>(v.type == Type::Int ? static_cast<double>(v.i) : v.f);
      uint32_t b;
      std::memcpy(&b, &f, 4);
      PutBE(out, b, 4);
      break;
    }
    case ColType::Double:
    case ColType::Numeric:
      PutBE(out, DoubleBits(v.type == Type::Int ? static_cast<double>(v.i) : v.f), 8);
      break;
    case ColType::Timestamp:
      PutBE(out, static_cast<uint64_t>(v.i - kPgEpochOffset), 8);
      break;
    default:
      out = ToText(v);
      break;
  }
  return out;
}

Value FromBinary(std::string_view data, uint32_t oid) {
  auto need = [&](size_t n) {
    if (data.size() != n) Fail(state::kProtocol, "bad binary parameter length");
  };
  switch (oid) {
    case 16:
      need(1);
      return Value::Bool(data[0] != 0);
    case 21:
      need(2);
      return Value::Int(static_cast<int16_t>(GetBE(data)));
    case 23:
      need(4);
      return Value::Int(static_cast<int32_t>(GetBE(data)));
    case 20:
      need(8);
      return Value::Int(static_cast<int64_t>(GetBE(data)));
    case 700: {
      need(4);
      uint32_t b = static_cast<uint32_t>(GetBE(data));
      float f;
      std::memcpy(&f, &b, 4);
      return Value::Float(f);
    }
    case 701:
      need(8);
      return Value::Float(BitsDouble(GetBE(data)));
    case 1114:
    case 1184:
      need(8);
      return Value::Time(static_cast<int64_t>(GetBE(data)) + kPgEpochOffset);
    case 0:
    case 25:
    case 705:
    case 1042:
    case 1043:
      return Value::Text(std::string(data));
    default:
      Fail(state::kNotSupported, "binary format for type oid " + std::to_string(oid));
  }
}

Value CastTo(const Value& v, Type to) {
  if (v.null() || v.type == to) return v;
  switch (to) {
    case Type::Null:
      return Value::Null();
    case Type::Bool:
      if (v.type == Type::Int) return Value::Bool(v.i != 0);
      if (v.type == Type::Text) {
        std::string s = Lower(Trim(v.s));
        if (s == "t" || s == "true" || s == "yes" || s == "on" || s == "1" || s == "y")
          return Value::Bool(true);
        if (s == "f" || s == "false" || s == "no" || s == "off" || s == "0" || s == "n")
          return Value::Bool(false);
        BadInput("boolean", v.s);
      }
      break;
    case Type::Int:
      if (v.type == Type::Float) {
        if (std::isnan(v.f) || v.f >= 9.2233720368547758e18 || v.f < -9.2233720368547758e18)
          Fail(state::kOutOfRange, "bigint out of range");
        return Value::Int(static_cast<int64_t>(std::nearbyint(v.f)));
      }
      if (v.type == Type::Bool) return Value::Int(v.i);
      if (v.type == Type::Text) {
        int64_t x;
        if (!ParseInt(v.s, &x)) BadInput("bigint", v.s);
        return Value::Int(x);
      }
      break;
    case Type::Float:
      if (v.type == Type::Int) return Value::Float(static_cast<double>(v.i));
      if (v.type == Type::Text) {
        double d;
        if (!ParseFloat(v.s, &d)) BadInput("double precision", v.s);
        return Value::Float(d);
      }
      break;
    case Type::Text:
      return Value::Text(ToText(v));
    case Type::Timestamp:
      if (v.type == Type::Text) {
        int64_t t;
        if (!ParseTimestamp(v.s, &t)) BadInput("timestamp", v.s);
        return Value::Time(t);
      }
      break;
  }
  Fail(state::kTypeMismatch,
       std::string("cannot convert ") + TypeName(v.type) + " to " + TypeName(to));
}

Value Cast(const Value& v, ColType to) {
  if (v.null()) return v;
  Value r = CastTo(v, StorageType(to));
  switch (to) {
    case ColType::SmallInt:
      if (r.i < -32768 || r.i > 32767) Fail(state::kOutOfRange, "smallint out of range");
      break;
    case ColType::Int:
      if (r.i < -2147483648LL || r.i > 2147483647LL)
        Fail(state::kOutOfRange, "integer out of range");
      break;
    case ColType::Real:
      r.f = static_cast<double>(static_cast<float>(r.f));
      break;
    default:
      break;
  }
  return r;
}

int Compare(const Value& a, const Value& b) {
  if (a.type == b.type) {
    switch (a.type) {
      case Type::Null:
        return 0;
      case Type::Bool:
      case Type::Int:
      case Type::Timestamp:
        return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
      case Type::Float: {
        bool an = std::isnan(a.f), bn = std::isnan(b.f);
        if (an || bn) return an == bn ? 0 : (an ? 1 : -1);
        return a.f < b.f ? -1 : (a.f > b.f ? 1 : 0);
      }
      case Type::Text:
        return CompareKeys(a.s, b.s);
    }
  }
  if ((a.type == Type::Int || a.type == Type::Float) &&
      (b.type == Type::Int || b.type == Type::Float)) {
    return Compare(CastTo(a, Type::Float), CastTo(b, Type::Float));
  }
  if (a.type == Type::Text) return Compare(CastTo(a, b.type), b);
  if (b.type == Type::Text) return Compare(a, CastTo(b, a.type));
  Fail(state::kTypeMismatch,
       std::string("cannot compare ") + TypeName(a.type) + " with " + TypeName(b.type));
}

}
