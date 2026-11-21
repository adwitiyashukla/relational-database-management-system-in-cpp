#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace rdbms {

enum class Type : uint8_t { Null = 0, Bool = 1, Int = 2, Float = 3, Text = 4, Timestamp = 5 };

struct Value {
  Type type = Type::Null;
  int64_t i = 0;
  double f = 0;
  std::string s;

  static Value Null() { return Value(); }
  static Value Bool(bool b);
  static Value Int(int64_t v);
  static Value Float(double v);
  static Value Text(std::string v);
  static Value Time(int64_t micros);
  bool null() const { return type == Type::Null; }
  bool truthy() const { return type == Type::Bool && i != 0; }
};

enum class ColType : uint8_t {
  Bool,
  SmallInt,
  Int,
  BigInt,
  Real,
  Double,
  Numeric,
  Text,
  Varchar,
  Char,
  Timestamp
};

struct Column {
  std::string name;
  ColType type = ColType::Text;
  bool not_null = false;
  int32_t len = -1;
};

struct Schema {
  std::string name;
  std::vector<Column> cols;
  std::vector<int> pk;
  bool hidden_rowid = false;
  PageId root = kNoPage;
};

Type StorageType(ColType t);
uint32_t TypeOid(ColType t);
int16_t TypeSize(ColType t);
std::string ColTypeName(const Column& c);
bool ParseColType(const std::string& name, ColType* out);

std::string EncodeRow(const std::vector<Value>& row);
std::vector<Value> DecodeRow(std::string_view data);
std::string EncodeKey(const std::vector<Value>& vals);
std::string KeySuccessor(std::string key);
int64_t DecodeKeyInt(std::string_view key);
std::string EncodeSchema(const Schema& s);
Schema DecodeSchema(std::string_view data);
std::string GroupKey(const std::vector<Value>& vals);

int Compare(const Value& a, const Value& b);
std::string ToText(const Value& v);
std::string ToBinary(const Value& v, ColType t);
Value FromBinary(std::string_view data, uint32_t oid);
Value Cast(const Value& v, ColType to);
Value CastTo(const Value& v, Type to);
std::string FormatDouble(double d);
std::string FormatTimestamp(int64_t micros);
bool ParseTimestamp(std::string_view s, int64_t* out);
bool ParseInt(std::string_view s, int64_t* out);
bool ParseFloat(std::string_view s, double* out);
const char* TypeName(Type t);

}
