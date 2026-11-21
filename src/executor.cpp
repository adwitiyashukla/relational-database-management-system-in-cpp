#include "executor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace rdbms {

namespace {

[[noreturn]] void NoColumn(const std::string& n) {
  Fail(state::kNoColumn, "column \"" + n + "\" does not exist");
}

void Bind(Expr& e, const Schema* s) {
  if (e.kind == ExprKind::Column) {
    if (!s) NoColumn(e.name);
    if (!e.op.empty() && e.op != s->name)
      Fail(state::kNoTable, "missing FROM-clause entry for table \"" + e.op + "\"");
    e.col = -1;
    for (size_t i = 0; i < s->cols.size(); i++) {
      if (s->cols[i].name == e.name) e.col = static_cast<int>(i);
    }
    if (e.col < 0) NoColumn(e.name);
  }
  for (auto& a : e.args) {
    if (a) Bind(*a, s);
  }
}

bool HasColumn(const Expr& e) {
  if (e.kind == ExprKind::Column) return true;
  for (auto& a : e.args) {
    if (a && HasColumn(*a)) return true;
  }
  return false;
}

bool HasAgg(const Expr& e) {
  if (e.kind == ExprKind::Func && IsAggregate(e.name)) return true;
  for (auto& a : e.args) {
    if (a && HasAgg(*a)) return true;
  }
  return false;
}

void CollectAggs(Expr& e, std::vector<Expr*>& out) {
  if (e.kind == ExprKind::Func && IsAggregate(e.name)) {
    for (auto& a : e.args) {
      if (HasAgg(*a)) Fail(state::kGrouping, "aggregate function calls cannot be nested");
    }
    if (e.name == "count" ? (!e.star && e.args.size() != 1) : (e.star || e.args.size() != 1))
      Fail(state::kUndefinedFunc, "function " + e.name + " takes exactly one argument");
    e.agg = static_cast<int>(out.size());
    out.push_back(&e);
    return;
  }
  for (auto& a : e.args) {
    if (a) CollectAggs(*a, out);
  }
}

bool SameExpr(const Expr& a, const Expr& b) {
  if (a.kind != b.kind || a.op != b.op || a.name != b.name || a.col != b.col ||
      a.param != b.param || a.negated != b.negated || a.args.size() != b.args.size())
    return false;
  if (a.kind == ExprKind::Const) {
    if (a.value.type != b.value.type) return false;
    if (!a.value.null() && Compare(a.value, b.value) != 0) return false;
  }
  for (size_t i = 0; i < a.args.size(); i++) {
    if (!SameExpr(*a.args[i], *b.args[i])) return false;
  }
  return true;
}

void CheckGrouped(const Expr& e, const std::vector<ExprPtr>& groups) {
  for (auto& g : groups) {
    if (SameExpr(e, *g)) return;
  }
  if (e.kind == ExprKind::Func && IsAggregate(e.name)) return;
  if (e.kind == ExprKind::Column)
    Fail(state::kGrouping, "column \"" + e.name +
                               "\" must appear in the GROUP BY clause or be used in an aggregate "
                               "function");
  for (auto& a : e.args) {
    if (a) CheckGrouped(*a, groups);
  }
}

void Flatten(const Expr* e, std::vector<const Expr*>& out) {
  if (!e) return;
  if (e->kind == ExprKind::Binary && e->op == "and") {
    Flatten(e->args[0].get(), out);
    Flatten(e->args[1].get(), out);
    return;
  }
  out.push_back(e);
}

size_t Utf8Len(const std::string& s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
  }
  return n;
}

bool AddOv(int64_t a, int64_t b, int64_t* r) {
  if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
    return true;
  *r = a + b;
  return false;
}

bool SubOv(int64_t a, int64_t b, int64_t* r) {
  if ((b < 0 && a > std::numeric_limits<int64_t>::max() + b) ||
      (b > 0 && a < std::numeric_limits<int64_t>::min() + b))
    return true;
  *r = a - b;
  return false;
}

bool MulOv(int64_t a, int64_t b, int64_t* r) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  if (a == 0 || b == 0) {
    *r = 0;
    return false;
  }
  if (a > 0) {
    if (b > 0 ? a > kMax / b : b < kMin / a) return true;
  } else {
    if (b > 0 ? a < kMin / b : b < kMax / a) return true;
  }
  *r = a * b;
  return false;
}

bool AsBool(const Value& v) {
  if (v.type == Type::Bool) return v.i != 0;
  if (v.type == Type::Text) return CastTo(v, Type::Bool).i != 0;
  Fail(state::kTypeMismatch, std::string("argument must be type boolean, not ") + TypeName(v.type));
}

Value Arith(const std::string& op, Value l, Value r) {
  bool ln = l.type == Type::Int || l.type == Type::Float;
  bool rn = r.type == Type::Int || r.type == Type::Float;
  if (l.type == Type::Text && rn) {
    l = CastTo(l, r.type);
    ln = true;
  }
  if (r.type == Type::Text && ln) {
    r = CastTo(r, l.type);
    rn = true;
  }
  if (!ln || !rn)
    Fail(state::kUndefinedFunc, std::string("operator does not exist: ") + TypeName(l.type) + " " +
                                    op + " " + TypeName(r.type));
  if (l.type == Type::Int && r.type == Type::Int) {
    int64_t x = l.i, y = r.i, z = 0;
    bool ov = false;
    if (op == "+") {
      ov = AddOv(x, y, &z);
    } else if (op == "-") {
      ov = SubOv(x, y, &z);
    } else if (op == "*") {
      ov = MulOv(x, y, &z);
    } else if (op == "/") {
      if (y == 0) Fail(state::kDivZero, "division by zero");
      if (x == std::numeric_limits<int64_t>::min() && y == -1) {
        ov = true;
      } else {
        z = x / y;
      }
    } else {
      if (y == 0) Fail(state::kDivZero, "division by zero");
      z = y == -1 ? 0 : x % y;
    }
    if (ov) Fail(state::kOutOfRange, "bigint out of range");
    return Value::Int(z);
  }
  double x = l.type == Type::Int ? static_cast<double>(l.i) : l.f;
  double y = r.type == Type::Int ? static_cast<double>(r.i) : r.f;
  if (op == "+") return Value::Float(x + y);
  if (op == "-") return Value::Float(x - y);
  if (op == "*") return Value::Float(x * y);
  if (y == 0) Fail(state::kDivZero, "division by zero");
  if (op == "/") return Value::Float(x / y);
  return Value::Float(std::fmod(x, y));
}

bool LikeMatch(std::string_view s, std::string_view p, bool ci) {
  auto eq = [&](char a, char b) {
    if (!ci) return a == b;
    auto lw = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    return lw(a) == lw(b);
  };
  size_t si = 0, pi = 0, star_p = std::string_view::npos, star_s = 0;
  while (si < s.size()) {
    if (pi < p.size()) {
      char pc = p[pi];
      if (pc == '\\' && pi + 1 < p.size()) {
        if (eq(s[si], p[pi + 1])) {
          si++;
          pi += 2;
          continue;
        }
      } else if (pc == '%') {
        star_p = pi++;
        star_s = si;
        continue;
      } else if (pc == '_' || eq(s[si], pc)) {
        si++;
        pi++;
        continue;
      }
    }
    if (star_p != std::string_view::npos) {
      pi = star_p + 1;
      si = ++star_s;
      continue;
    }
    return false;
  }
  while (pi < p.size() && p[pi] == '%') pi++;
  return pi == p.size();
}

int SortCompare(const Value& a, const Value& b) {
  if (a.null() && b.null()) return 0;
  if (a.null()) return 1;
  if (b.null()) return -1;
  return Compare(a, b);
}

struct AggState {
  int64_t count = 0;
  bool has = false;
  bool is_float = false;
  int64_t isum = 0;
  double fsum = 0;
  Value best;
  std::unordered_set<std::string> seen;
};

Value Final(const Expr& agg, const AggState& st) {
  const std::string& f = agg.name;
  if (f == "count") return Value::Int(st.count);
  if (!st.has) return Value::Null();
  if (f == "sum") return st.is_float ? Value::Float(st.fsum) : Value::Int(st.isum);
  if (f == "avg")
    return Value::Float((st.is_float ? st.fsum : static_cast<double>(st.isum)) /
                        static_cast<double>(st.count));
  return st.best;
}

ColType InferType(const Expr& e, const Schema* s);

ColType ValueColType(const Value& v) {
  switch (v.type) {
    case Type::Bool:
      return ColType::Bool;
    case Type::Int:
      return (v.i >= -2147483648LL && v.i <= 2147483647LL) ? ColType::Int : ColType::BigInt;
    case Type::Float:
      return ColType::Double;
    case Type::Timestamp:
      return ColType::Timestamp;
    default:
      return ColType::Text;
  }
}

bool IsIntegral(ColType t) {
  return t == ColType::SmallInt || t == ColType::Int || t == ColType::BigInt;
}

ColType InferType(const Expr& e, const Schema* s) {
  switch (e.kind) {
    case ExprKind::Const:
      return ValueColType(e.value);
    case ExprKind::Column:
      return s && e.col >= 0 ? s->cols[static_cast<size_t>(e.col)].type : ColType::Text;
    case ExprKind::Param:
      return ColType::Text;
    case ExprKind::Unary:
      return e.op == "not" ? ColType::Bool : InferType(*e.args[0], s);
    case ExprKind::Binary: {
      const std::string& op = e.op;
      if (op == "||") return ColType::Text;
      if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        ColType a = InferType(*e.args[0], s), b = InferType(*e.args[1], s);
        if (IsIntegral(a) && IsIntegral(b)) return ColType::BigInt;
        if (IsIntegral(a) && b == ColType::Text) return ColType::BigInt;
        if (a == ColType::Text && IsIntegral(b)) return ColType::BigInt;
        return ColType::Double;
      }
      return ColType::Bool;
    }
    case ExprKind::IsNull:
    case ExprKind::Between:
    case ExprKind::InList:
    case ExprKind::Like:
      return ColType::Bool;
    case ExprKind::Cast:
      return e.cast_type;
    case ExprKind::Case:
      return e.args.size() > 1 ? InferType(*e.args[e.name == "simple" ? 2 : 1], s) : ColType::Text;
    case ExprKind::Func: {
      const std::string& f = e.name;
      if (f == "count" || f == "length" || f == "char_length" || f == "character_length" ||
          f == "octet_length")
        return ColType::BigInt;
      if (f == "sum") {
        ColType a = e.args.empty() ? ColType::BigInt : InferType(*e.args[0], s);
        return IsIntegral(a) ? ColType::BigInt : ColType::Double;
      }
      if (f == "avg" || f == "round" || f == "floor" || f == "ceil" || f == "ceiling" ||
          f == "sqrt")
        return ColType::Double;
      if (f == "now" || f == "current_timestamp" || f == "statement_timestamp" ||
          f == "transaction_timestamp" || f == "clock_timestamp")
        return ColType::Timestamp;
      if (f == "is_true" || f == "is_false") return ColType::Bool;
      if (f == "lower" || f == "upper" || f == "substr" || f == "substring" || f == "concat" ||
          f == "version")
        return ColType::Text;
      if (!e.args.empty()) return InferType(*e.args[0], s);
      return ColType::Text;
    }
  }
  return ColType::Text;
}

std::string DefaultName(const Expr& e) {
  switch (e.kind) {
    case ExprKind::Column:
      return e.name;
    case ExprKind::Func:
      if (e.name == "is_true" || e.name == "is_false") return "?column?";
      return e.name;
    case ExprKind::Cast:
      return DefaultName(*e.args[0]) == "?column?"
                 ? ColTypeName(Column{"", e.cast_type, false, e.cast_len})
                 : DefaultName(*e.args[0]);
    case ExprKind::Case:
      return "case";
    default:
      return "?column?";
  }
}

std::string Quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r.push_back('\'');
    r.push_back(c);
  }
  return r + "'";
}

std::string ExprText(const Expr& e) {
  switch (e.kind) {
    case ExprKind::Const:
      if (e.value.null()) return "NULL";
      if (e.value.type == Type::Text || e.value.type == Type::Timestamp)
        return Quote(ToText(e.value));
      if (e.value.type == Type::Bool) return e.value.i ? "true" : "false";
      return ToText(e.value);
    case ExprKind::Column:
      return e.name;
    case ExprKind::Param:
      return "$" + std::to_string(e.param);
    case ExprKind::Unary:
      return e.op == "not" ? "(NOT " + ExprText(*e.args[0]) + ")"
                           : "(-" + ExprText(*e.args[0]) + ")";
    case ExprKind::Binary: {
      std::string op = e.op == "and" ? "AND" : e.op == "or" ? "OR" : e.op;
      return "(" + ExprText(*e.args[0]) + " " + op + " " + ExprText(*e.args[1]) + ")";
    }
    case ExprKind::IsNull:
      return "(" + ExprText(*e.args[0]) + (e.negated ? " IS NOT NULL)" : " IS NULL)");
    case ExprKind::Between:
      return "(" + ExprText(*e.args[0]) + (e.negated ? " NOT BETWEEN " : " BETWEEN ") +
             ExprText(*e.args[1]) + " AND " + ExprText(*e.args[2]) + ")";
    case ExprKind::InList: {
      std::string r = "(" + ExprText(*e.args[0]) + (e.negated ? " NOT IN (" : " IN (");
      for (size_t i = 1; i < e.args.size(); i++) r += (i > 1 ? ", " : "") + ExprText(*e.args[i]);
      return r + "))";
    }
    case ExprKind::Like:
      return "(" + ExprText(*e.args[0]) + (e.negated ? " NOT " : " ") +
             (e.distinct ? "ILIKE " : "LIKE ") + ExprText(*e.args[1]) + ")";
    case ExprKind::Cast:
      return ExprText(*e.args[0]) + "::" + ColTypeName(Column{"", e.cast_type, false, e.cast_len});
    case ExprKind::Case:
      return "CASE ... END";
    case ExprKind::Func: {
      std::string r = e.name + "(";
      if (e.star) r += "*";
      if (e.distinct) r += "DISTINCT ";
      for (size_t i = 0; i < e.args.size(); i++) r += (i ? ", " : "") + ExprText(*e.args[i]);
      return r + ")";
    }
  }
  return "?";
}

Value StoreValue(const Value& v, const Column& c) {
  if (v.null()) return v;
  Value r = Cast(v, c.type);
  if (c.len > 0 && (c.type == ColType::Varchar || c.type == ColType::Char) &&
      Utf8Len(r.s) > static_cast<size_t>(c.len))
    Fail(state::kTooLong, "value too long for type " + ColTypeName(c));
  return r;
}

void CheckNotNull(const Schema& s, const std::vector<Value>& row) {
  for (size_t i = 0; i < s.cols.size(); i++) {
    if (s.cols[i].not_null && row[i].null())
      Fail(state::kNotNull, "null value in column \"" + s.cols[i].name + "\" of relation \"" +
                                s.name + "\" violates not-null constraint");
  }
}

[[noreturn]] void Duplicate(const Schema& s) {
  Fail(state::kUnique, "duplicate key value violates unique constraint \"" + s.name + "_pkey\"");
}

struct Output {
  Expr* expr = nullptr;
  int col = -1;
  std::string name;
  ColType type = ColType::Text;
};

std::vector<Output> Outputs(SelectStmt& s, const Schema* schema) {
  std::vector<Output> outs;
  for (auto& it : s.items) {
    if (it.star) {
      if (!schema) Fail(state::kSyntax, "SELECT * with no tables specified is not valid");
      for (size_t i = 0; i < schema->cols.size(); i++)
        outs.push_back({nullptr, static_cast<int>(i), schema->cols[i].name, schema->cols[i].type});
      continue;
    }
    Bind(*it.expr, schema);
    outs.push_back({it.expr.get(), -1, it.alias.empty() ? DefaultName(*it.expr) : it.alias,
                    InferType(*it.expr, schema)});
  }
  return outs;
}

}

std::shared_ptr<Table> RequireTable(Engine& e, const std::string& name) {
  auto t = e.FindTable(name);
  if (!t) Fail(state::kNoTable, "relation \"" + name + "\" does not exist");
  return t;
}

Executor::Executor(Engine& e, Txn* t, const std::vector<Value>& params, int64_t now)
    : e_(e), t_(t), params_(params), now_(now) {}

std::vector<ResultColumn> Executor::Describe(Engine& e, Statement& st) {
  std::vector<ResultColumn> cols;
  switch (st.kind) {
    case Statement::Kind::Select: {
      SelectStmt& s = *st.select;
      std::shared_ptr<Table> tbl;
      if (!s.table.empty()) tbl = RequireTable(e, s.table);
      for (auto& o : Outputs(s, tbl ? &tbl->schema : nullptr)) cols.push_back({o.name, o.type});
      break;
    }
    case Statement::Kind::Explain:
      cols.push_back({"QUERY PLAN", ColType::Text});
      break;
    case Statement::Kind::Show:
      cols.push_back({st.name, ColType::Text});
      break;
    default:
      break;
  }
  return cols;
}

QueryResult Executor::Run(Statement& st) {
  switch (st.kind) {
    case Statement::Kind::Select:
      return Select(*st.select);
    case Statement::Kind::Insert:
      return Insert(*st.insert);
    case Statement::Kind::Update:
      return Update(*st.update);
    case Statement::Kind::Delete:
      return Delete(*st.del);
    case Statement::Kind::Explain:
      return Explain(st);
    default:
      Fail(state::kInternal, "statement cannot run in the executor");
  }
}

Value Executor::Eval(const Expr& e, const std::vector<Value>& row,
                     const std::vector<Value>* aggs) const {
  switch (e.kind) {
    case ExprKind::Const:
      return e.value;
    case ExprKind::Column:
      if (e.col < 0 || static_cast<size_t>(e.col) >= row.size())
        Fail(state::kNoColumn, "column \"" + e.name + "\" does not exist");
      return row[static_cast<size_t>(e.col)];
    case ExprKind::Param:
      if (static_cast<size_t>(e.param) > params_.size())
        Fail(state::kNoParam, "there is no parameter $" + std::to_string(e.param));
      return params_[static_cast<size_t>(e.param - 1)];
    case ExprKind::Unary: {
      Value v = Eval(*e.args[0], row, aggs);
      if (v.null()) return v;
      if (e.op == "not") return Value::Bool(!AsBool(v));
      if (v.type == Type::Text) v = CastTo(v, Type::Float);
      if (v.type == Type::Int) {
        if (v.i == std::numeric_limits<int64_t>::min())
          Fail(state::kOutOfRange, "bigint out of range");
        return Value::Int(-v.i);
      }
      if (v.type == Type::Float) return Value::Float(-v.f);
      Fail(state::kUndefinedFunc, std::string("operator does not exist: - ") + TypeName(v.type));
    }
    case ExprKind::Binary: {
      const std::string& op = e.op;
      if (op == "and") {
        Value l = Eval(*e.args[0], row, aggs);
        if (!l.null() && !AsBool(l)) return Value::Bool(false);
        Value r = Eval(*e.args[1], row, aggs);
        if (!r.null() && !AsBool(r)) return Value::Bool(false);
        if (l.null() || r.null()) return Value::Null();
        return Value::Bool(true);
      }
      if (op == "or") {
        Value l = Eval(*e.args[0], row, aggs);
        if (!l.null() && AsBool(l)) return Value::Bool(true);
        Value r = Eval(*e.args[1], row, aggs);
        if (!r.null() && AsBool(r)) return Value::Bool(true);
        if (l.null() || r.null()) return Value::Null();
        return Value::Bool(false);
      }
      Value l = Eval(*e.args[0], row, aggs);
      Value r = Eval(*e.args[1], row, aggs);
      if (l.null() || r.null()) return Value::Null();
      if (op == "||") return Value::Text(ToText(l) + ToText(r));
      if (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=") {
        int c = Compare(l, r);
        bool b = op == "="    ? c == 0
                 : op == "<>" ? c != 0
                 : op == "<"  ? c < 0
                 : op == "<=" ? c <= 0
                 : op == ">"  ? c > 0
                              : c >= 0;
        return Value::Bool(b);
      }
      return Arith(op, l, r);
    }
    case ExprKind::IsNull: {
      bool r = Eval(*e.args[0], row, aggs).null();
      return Value::Bool(e.negated ? !r : r);
    }
    case ExprKind::Between: {
      Value a = Eval(*e.args[0], row, aggs);
      Value lo = Eval(*e.args[1], row, aggs);
      Value hi = Eval(*e.args[2], row, aggs);
      if (a.null() || lo.null() || hi.null()) return Value::Null();
      bool r = Compare(a, lo) >= 0 && Compare(a, hi) <= 0;
      return Value::Bool(e.negated ? !r : r);
    }
    case ExprKind::InList: {
      Value a = Eval(*e.args[0], row, aggs);
      if (a.null()) return a;
      bool saw_null = false;
      for (size_t i = 1; i < e.args.size(); i++) {
        Value v = Eval(*e.args[i], row, aggs);
        if (v.null()) {
          saw_null = true;
          continue;
        }
        if (Compare(a, v) == 0) return Value::Bool(!e.negated);
      }
      if (saw_null) return Value::Null();
      return Value::Bool(e.negated);
    }
    case ExprKind::Like: {
      Value s = Eval(*e.args[0], row, aggs);
      Value p = Eval(*e.args[1], row, aggs);
      if (s.null() || p.null()) return Value::Null();
      bool r = LikeMatch(ToText(s), ToText(p), e.distinct);
      return Value::Bool(e.negated ? !r : r);
    }
    case ExprKind::Cast: {
      Value v = Cast(Eval(*e.args[0], row, aggs), e.cast_type);
      if (!v.null() && e.cast_len > 0 &&
          (e.cast_type == ColType::Varchar || e.cast_type == ColType::Char) &&
          v.s.size() > static_cast<size_t>(e.cast_len))
        v.s.resize(static_cast<size_t>(e.cast_len));
      return v;
    }
    case ExprKind::Case: {
      size_t i = 0;
      bool simple = e.name == "simple";
      Value operand;
      if (simple) operand = Eval(*e.args[i++], row, aggs);
      size_t end = e.args.size() - (e.negated ? 1 : 0);
      for (; i + 1 < end; i += 2) {
        Value w = Eval(*e.args[i], row, aggs);
        bool hit = simple ? (!w.null() && !operand.null() && Compare(operand, w) == 0)
                          : (!w.null() && AsBool(w));
        if (hit) return Eval(*e.args[i + 1], row, aggs);
      }
      if (e.negated) return Eval(*e.args.back(), row, aggs);
      return Value::Null();
    }
    case ExprKind::Func:
      break;
  }
  const std::string& f = e.name;
  if (IsAggregate(f)) {
    if (!aggs || e.agg < 0) Fail(state::kGrouping, "aggregate functions are not allowed here");
    return (*aggs)[static_cast<size_t>(e.agg)];
  }
  auto arg = [&](size_t i) { return Eval(*e.args[i], row, aggs); };
  auto need = [&](size_t lo, size_t hi) {
    if (e.args.size() < lo || e.args.size() > hi)
      Fail(state::kUndefinedFunc, "function " + f + " with " + std::to_string(e.args.size()) +
                                      " arguments does not exist");
  };
  if (f == "now" || f == "current_timestamp" || f == "statement_timestamp" ||
      f == "transaction_timestamp" || f == "clock_timestamp") {
    need(0, 0);
    return Value::Time(now_);
  }
  if (f == "version") {
    need(0, 0);
    return Value::Text("PostgreSQL 16.0 (relational-database-management-system-in-cpp)");
  }
  if (f == "abs") {
    need(1, 1);
    Value v = arg(0);
    if (v.null()) return v;
    if (v.type == Type::Text) v = CastTo(v, Type::Float);
    if (v.type == Type::Int) {
      if (v.i == std::numeric_limits<int64_t>::min())
        Fail(state::kOutOfRange, "bigint out of range");
      return Value::Int(v.i < 0 ? -v.i : v.i);
    }
    return Value::Float(std::fabs(CastTo(v, Type::Float).f));
  }
  if (f == "mod") {
    need(2, 2);
    Value a = arg(0), b = arg(1);
    if (a.null() || b.null()) return Value::Null();
    return Arith("%", a, b);
  }
  if (f == "round" || f == "floor" || f == "ceil" || f == "ceiling" || f == "sqrt") {
    need(1, f == "round" ? 2 : 1);
    Value v = arg(0);
    if (v.null()) return v;
    double x = CastTo(v, Type::Float).f;
    if (f == "floor") return Value::Float(std::floor(x));
    if (f == "ceil" || f == "ceiling") return Value::Float(std::ceil(x));
    if (f == "sqrt") {
      if (x < 0) Fail("2201F", "cannot take square root of a negative number");
      return Value::Float(std::sqrt(x));
    }
    int64_t digits = e.args.size() > 1 ? CastTo(arg(1), Type::Int).i : 0;
    double scale = std::pow(10.0, static_cast<double>(digits));
    return Value::Float(std::round(x * scale) / scale);
  }
  if (f == "length" || f == "char_length" || f == "character_length" || f == "octet_length") {
    need(1, 1);
    Value v = arg(0);
    if (v.null()) return v;
    std::string s = ToText(v);
    return Value::Int(static_cast<int64_t>(f == "octet_length" ? s.size() : Utf8Len(s)));
  }
  if (f == "lower" || f == "upper") {
    need(1, 1);
    Value v = arg(0);
    if (v.null()) return v;
    std::string s = ToText(v);
    for (auto& c : s) {
      if (f == "lower" && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (f == "upper" && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return Value::Text(s);
  }
  if (f == "substr" || f == "substring") {
    need(2, 3);
    Value v = arg(0), st = arg(1);
    if (v.null() || st.null()) return Value::Null();
    std::string s = ToText(v);
    int64_t from = CastTo(st, Type::Int).i - 1;
    int64_t len = static_cast<int64_t>(s.size());
    if (e.args.size() == 3) {
      Value l = arg(2);
      if (l.null()) return l;
      len = CastTo(l, Type::Int).i;
      if (len < 0) Fail("22011", "negative substring length not allowed");
    }
    int64_t a = std::max<int64_t>(from, 0);
    int64_t b = std::min<int64_t>(static_cast<int64_t>(s.size()), from + len);
    if (b <= a) return Value::Text(std::string());
    return Value::Text(s.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
  }
  if (f == "concat") {
    std::string s;
    for (size_t i = 0; i < e.args.size(); i++) {
      Value v = arg(i);
      if (!v.null()) s += ToText(v);
    }
    return Value::Text(s);
  }
  if (f == "coalesce") {
    for (size_t i = 0; i < e.args.size(); i++) {
      Value v = arg(i);
      if (!v.null()) return v;
    }
    return Value::Null();
  }
  if (f == "nullif") {
    need(2, 2);
    Value a = arg(0), b = arg(1);
    if (!a.null() && !b.null() && Compare(a, b) == 0) return Value::Null();
    return a;
  }
  if (f == "greatest" || f == "least") {
    Value best;
    for (size_t i = 0; i < e.args.size(); i++) {
      Value v = arg(i);
      if (v.null()) continue;
      if (best.null() || (f == "greatest" ? Compare(v, best) > 0 : Compare(v, best) < 0)) best = v;
    }
    return best;
  }
  if (f == "is_true" || f == "is_false") {
    Value v = arg(0);
    bool r = !v.null() && AsBool(v) == (f == "is_true");
    return Value::Bool(e.negated ? !r : r);
  }
  Fail(state::kUndefinedFunc, "function " + f + "() does not exist");
}

bool Executor::Matches(const Expr* where, const std::vector<Value>& row) const {
  if (!where) return true;
  Value v = Eval(*where, row);
  return !v.null() && AsBool(v);
}

std::string Executor::RowKey(const Table& tbl, const std::vector<Value>& row) {
  std::vector<Value> k;
  for (int c : tbl.schema.pk) k.push_back(row[static_cast<size_t>(c)]);
  return EncodeKey(k);
}

Access Executor::Plan(const Schema& s, const Expr* where) const {
  Access a;
  if (!where || s.hidden_rowid || s.pk.empty()) return a;
  std::vector<const Expr*> conj;
  Flatten(where, conj);
  auto constant = [&](const Expr& x, ColType t, Value* out) {
    if (HasColumn(x) || HasAgg(x)) return false;
    try {
      Value v = Eval(x, {});
      if (v.null()) return false;
      *out = Cast(v, t);
      return true;
    } catch (const DbError&) {
      return false;
    }
  };
  auto col_of = [](const Expr& x) { return x.kind == ExprKind::Column ? x.col : -1; };
  std::vector<Value> eq;
  for (size_t j = 0; j < s.pk.size(); j++) {
    int c = s.pk[j];
    ColType t = s.cols[static_cast<size_t>(c)].type;
    bool found = false;
    for (const Expr* x : conj) {
      if (x->kind != ExprKind::Binary || x->op != "=") continue;
      Value v;
      if ((col_of(*x->args[0]) == c && constant(*x->args[1], t, &v)) ||
          (col_of(*x->args[1]) == c && constant(*x->args[0], t, &v))) {
        eq.push_back(v);
        found = true;
        break;
      }
    }
    if (!found) break;
  }
  if (eq.size() == s.pk.size()) {
    a.kind = Access::Kind::Point;
    a.keys.push_back(EncodeKey(eq));
    return a;
  }
  if (eq.empty() && s.pk.size() == 1) {
    int c = s.pk[0];
    ColType t = s.cols[static_cast<size_t>(c)].type;
    for (const Expr* x : conj) {
      if (x->kind != ExprKind::InList || x->negated || col_of(*x->args[0]) != c) continue;
      std::vector<std::string> keys;
      bool ok = true;
      for (size_t i = 1; i < x->args.size() && ok; i++) {
        Value v;
        if (constant(*x->args[i], t, &v)) {
          keys.push_back(EncodeKey({v}));
        } else if (HasColumn(*x->args[i])) {
          ok = false;
        }
      }
      if (!ok) continue;
      std::sort(keys.begin(), keys.end(),
                [](const std::string& p, const std::string& q) { return CompareKeys(p, q) < 0; });
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      a.kind = Access::Kind::Multi;
      a.keys = std::move(keys);
      return a;
    }
  }
  size_t p = eq.size();
  int c = s.pk[p];
  ColType t = s.cols[static_cast<size_t>(c)].type;
  std::optional<Value> lower, upper;
  auto lo = [&](const Value& v) {
    if (!lower || Compare(v, *lower) > 0) lower = v;
  };
  auto hi = [&](const Value& v) {
    if (!upper || Compare(v, *upper) < 0) upper = v;
  };
  for (const Expr* x : conj) {
    if (x->kind == ExprKind::Binary) {
      const std::string& op = x->op;
      Value v;
      if (col_of(*x->args[0]) == c && constant(*x->args[1], t, &v)) {
        if (op == ">" || op == ">=") lo(v);
        if (op == "<" || op == "<=") hi(v);
      } else if (col_of(*x->args[1]) == c && constant(*x->args[0], t, &v)) {
        if (op == "<" || op == "<=") lo(v);
        if (op == ">" || op == ">=") hi(v);
      }
    } else if (x->kind == ExprKind::Between && !x->negated && col_of(*x->args[0]) == c) {
      Value v;
      if (constant(*x->args[1], t, &v)) lo(v);
      if (constant(*x->args[2], t, &v)) hi(v);
    }
  }
  if (p == 0 && !lower && !upper) return a;
  a.kind = Access::Kind::Range;
  std::vector<Value> lk = eq;
  if (lower) lk.push_back(*lower);
  a.lo = lk.empty() ? std::string() : EncodeKey(lk);
  if (upper) {
    std::vector<Value> hk = eq;
    hk.push_back(*upper);
    std::string h = EncodeKey(hk);
    if (p + 1 == s.pk.size()) {
      a.hi = h;
      a.hi_incl = true;
    } else {
      std::string sk = KeySuccessor(h);
      if (!sk.empty()) a.hi = sk;
    }
  } else if (p > 0) {
    std::string sk = KeySuccessor(EncodeKey(eq));
    if (!sk.empty()) a.hi = sk;
  }
  return a;
}

void Executor::Each(Table& tbl, const Access& a,
                    const std::function<bool(std::string_view, std::vector<Value>&)>& fn) {
  BTree& tree = *tbl.tree;
  if (a.kind == Access::Kind::Point || a.kind == Access::Kind::Multi) {
    for (auto& k : a.keys) {
      std::string v;
      if (!e_.Read(*t_, tree, k, &v)) continue;
      std::vector<Value> row = DecodeRow(v);
      if (!fn(k, row)) return;
    }
    return;
  }
  e_.Scan(*t_, tree, a.lo, a.hi ? &*a.hi : nullptr, a.hi_incl,
          [&](std::string_view k, std::string_view v) {
            std::vector<Value> row = DecodeRow(v);
            return fn(k, row);
          });
}

QueryResult Executor::Select(SelectStmt& s) {
  std::shared_ptr<Table> tbl;
  const Schema* schema = nullptr;
  if (!s.table.empty()) {
    tbl = RequireTable(e_, s.table);
    schema = &tbl->schema;
  }
  std::vector<Output> outs = Outputs(s, schema);
  if (s.where) {
    Bind(*s.where, schema);
    if (HasAgg(*s.where)) Fail(state::kGrouping, "aggregate functions are not allowed in WHERE");
  }
  for (auto& g : s.group_by) {
    if (g->kind == ExprKind::Const && g->value.type == Type::Int) {
      int64_t n = g->value.i;
      if (n < 1 || static_cast<size_t>(n) > s.items.size() ||
          !s.items[static_cast<size_t>(n - 1)].expr)
        Fail("42P10", "GROUP BY position " + std::to_string(n) + " is not in select list");
      Expr* src = s.items[static_cast<size_t>(n - 1)].expr.get();
      auto copy = std::make_unique<Expr>();
      std::function<void(Expr&, const Expr&)> clone = [&](Expr& d, const Expr& x) {
        d.kind = x.kind;
        d.value = x.value;
        d.name = x.name;
        d.op = x.op;
        d.param = x.param;
        d.negated = x.negated;
        d.distinct = x.distinct;
        d.star = x.star;
        d.cast_type = x.cast_type;
        d.cast_len = x.cast_len;
        d.col = x.col;
        for (auto& c : x.args) {
          auto n2 = std::make_unique<Expr>();
          clone(*n2, *c);
          d.args.push_back(std::move(n2));
        }
      };
      clone(*copy, *src);
      g = std::move(copy);
    }
    Bind(*g, schema);
    if (HasAgg(*g)) Fail(state::kGrouping, "aggregate functions are not allowed in GROUP BY");
  }
  if (s.having) Bind(*s.having, schema);
  struct Ord {
    Expr* expr;
    int out;
    bool desc;
  };
  std::vector<Ord> ords;
  for (auto& o : s.order_by) {
    int out = -1;
    if (o.expr->kind == ExprKind::Const && o.expr->value.type == Type::Int) {
      int64_t n = o.expr->value.i;
      if (n < 1 || static_cast<size_t>(n) > outs.size())
        Fail("42P10", "ORDER BY position " + std::to_string(n) + " is not in select list");
      out = static_cast<int>(n - 1);
    } else if (o.expr->kind == ExprKind::Column && o.expr->op.empty()) {
      for (size_t i = 0; i < outs.size(); i++) {
        if (outs[i].name == o.expr->name) {
          out = static_cast<int>(i);
          break;
        }
      }
    }
    if (out < 0) Bind(*o.expr, schema);
    ords.push_back({o.expr.get(), out, o.desc});
  }
  std::vector<Expr*> aggs;
  for (auto& o : outs) {
    if (o.expr) CollectAggs(*o.expr, aggs);
  }
  if (s.having) CollectAggs(*s.having, aggs);
  for (auto& o : ords) {
    if (o.out < 0) CollectAggs(*o.expr, aggs);
  }
  bool grouped = !aggs.empty() || !s.group_by.empty() || s.having;
  if (grouped) {
    for (auto& o : outs) {
      if (!o.expr)
        Fail(state::kGrouping,
             "column \"" + o.name +
                 "\" must appear in the GROUP BY clause or be used in an aggregate function");
      CheckGrouped(*o.expr, s.group_by);
    }
    if (s.having) CheckGrouped(*s.having, s.group_by);
    for (auto& o : ords) {
      if (o.out < 0) CheckGrouped(*o.expr, s.group_by);
    }
  }
  if (s.for_update && (grouped || !tbl || s.distinct))
    Fail(state::kNotSupported,
         "FOR UPDATE is not allowed with aggregates, DISTINCT or without a table");
  int64_t limit = -1, offset = 0;
  if (s.limit) {
    Value v = Eval(*s.limit, {});
    if (!v.null()) {
      limit = CastTo(v, Type::Int).i;
      if (limit < 0) Fail("2201W", "LIMIT must not be negative");
    }
  }
  if (s.offset) {
    Value v = Eval(*s.offset, {});
    if (!v.null()) {
      offset = CastTo(v, Type::Int).i;
      if (offset < 0) Fail("2201X", "OFFSET must not be negative");
    }
  }
  bool early = !grouped && ords.empty() && !s.distinct && !s.for_update && limit >= 0;
  std::vector<std::vector<Value>> rows;
  std::vector<std::string> keys;
  if (!tbl) {
    std::vector<Value> empty;
    if (Matches(s.where.get(), empty)) rows.push_back(empty);
  } else if (!early || limit > 0) {
    Access a = Plan(*schema, s.where.get());
    Each(*tbl, a, [&](std::string_view k, std::vector<Value>& row) {
      if (!Matches(s.where.get(), row)) return true;
      rows.push_back(std::move(row));
      if (s.for_update) keys.emplace_back(k);
      return !(early && static_cast<int64_t>(rows.size()) >= offset + limit);
    });
  }
  if (s.for_update) {
    std::vector<std::vector<Value>> kept;
    for (size_t i = 0; i < rows.size(); i++) {
      std::string orig = EncodeRow(rows[i]);
      std::vector<Value> latest_row;
      bool keep = false;
      e_.Write(*t_, *tbl->tree, keys[i], Engine::Op::Lock,
               [&](const std::string* latest) -> std::optional<std::string> {
                 latest_row = DecodeRow(*latest);
                 if (*latest != orig && !Matches(s.where.get(), latest_row)) return std::nullopt;
                 keep = true;
                 return *latest;
               });
      if (keep) kept.push_back(std::move(latest_row));
    }
    rows = std::move(kept);
  }
  struct OutRow {
    std::vector<Value> vals;
    std::vector<Value> sort;
  };
  std::vector<OutRow> result;
  auto project = [&](const std::vector<Value>& row, const std::vector<Value>* av) {
    OutRow r;
    for (auto& o : outs)
      r.vals.push_back(o.expr ? Eval(*o.expr, row, av) : row[static_cast<size_t>(o.col)]);
    for (auto& o : ords)
      r.sort.push_back(o.out >= 0 ? r.vals[static_cast<size_t>(o.out)] : Eval(*o.expr, row, av));
    return r;
  };
  if (!grouped) {
    for (auto& row : rows) result.push_back(project(row, nullptr));
  } else {
    struct Group {
      std::vector<Value> first;
      std::vector<AggState> st;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string, size_t> index;
    for (auto& row : rows) {
      std::vector<Value> gv;
      for (auto& g : s.group_by) gv.push_back(Eval(*g, row));
      std::string gk = GroupKey(gv);
      auto it = index.find(gk);
      size_t gi;
      if (it == index.end()) {
        gi = groups.size();
        index.emplace(gk, gi);
        groups.push_back({row, std::vector<AggState>(aggs.size())});
      } else {
        gi = it->second;
      }
      for (size_t i = 0; i < aggs.size(); i++) {
        const Expr& agg = *aggs[i];
        AggState& st = groups[gi].st[i];
        if (agg.star) {
          st.count++;
          continue;
        }
        Value v = Eval(*agg.args[0], row);
        if (v.null()) continue;
        if (agg.distinct && !st.seen.insert(GroupKey({v})).second) continue;
        st.count++;
        if (agg.name == "sum" || agg.name == "avg") {
          if (v.type == Type::Text) v = CastTo(v, Type::Float);
          if (v.type != Type::Int && v.type != Type::Float)
            Fail(state::kUndefinedFunc,
                 "function " + agg.name + "(" + TypeName(v.type) + ") does not exist");
          if (v.type == Type::Float || st.is_float) {
            if (!st.is_float) {
              st.fsum = static_cast<double>(st.isum);
              st.is_float = true;
            }
            st.fsum += v.type == Type::Float ? v.f : static_cast<double>(v.i);
          } else {
            int64_t r;
            if (AddOv(st.isum, v.i, &r)) {
              st.is_float = true;
              st.fsum = static_cast<double>(st.isum) + static_cast<double>(v.i);
            } else {
              st.isum = r;
            }
          }
          st.has = true;
        } else if (agg.name == "min" || agg.name == "max") {
          if (!st.has || (agg.name == "min" ? Compare(v, st.best) < 0 : Compare(v, st.best) > 0))
            st.best = v;
          st.has = true;
        }
      }
    }
    if (groups.empty() && s.group_by.empty())
      groups.push_back({std::vector<Value>(schema ? schema->cols.size() : 0),
                        std::vector<AggState>(aggs.size())});
    for (auto& g : groups) {
      std::vector<Value> av;
      for (size_t i = 0; i < aggs.size(); i++) av.push_back(Final(*aggs[i], g.st[i]));
      if (s.having) {
        Value h = Eval(*s.having, g.first, &av);
        if (h.null() || !AsBool(h)) continue;
      }
      result.push_back(project(g.first, &av));
    }
  }
  if (s.distinct) {
    std::unordered_set<std::string> seen;
    std::vector<OutRow> uniq;
    for (auto& r : result) {
      if (seen.insert(GroupKey(r.vals)).second) uniq.push_back(std::move(r));
    }
    result = std::move(uniq);
  }
  if (!ords.empty()) {
    std::stable_sort(result.begin(), result.end(), [&](const OutRow& a, const OutRow& b) {
      for (size_t i = 0; i < ords.size(); i++) {
        int c = SortCompare(a.sort[i], b.sort[i]);
        if (c != 0) return ords[i].desc ? c > 0 : c < 0;
      }
      return false;
    });
  }
  QueryResult q;
  q.has_rows = true;
  for (auto& o : outs) q.columns.push_back({o.name, o.type});
  for (size_t i = static_cast<size_t>(offset); i < result.size(); i++) {
    if (limit >= 0 && static_cast<int64_t>(i) - offset >= limit) break;
    q.rows.push_back(std::move(result[i].vals));
  }
  q.tag = "SELECT " + std::to_string(q.rows.size());
  return q;
}

QueryResult Executor::Insert(InsertStmt& s) {
  auto tbl = RequireTable(e_, s.table);
  const Schema& sc = tbl->schema;
  std::vector<size_t> targets;
  if (s.columns.empty()) {
    for (size_t i = 0; i < sc.cols.size(); i++) targets.push_back(i);
  } else {
    for (auto& name : s.columns) {
      size_t idx = sc.cols.size();
      for (size_t i = 0; i < sc.cols.size(); i++) {
        if (sc.cols[i].name == name) idx = i;
      }
      if (idx == sc.cols.size())
        Fail(state::kNoColumn,
             "column \"" + name + "\" of relation \"" + sc.name + "\" does not exist");
      if (std::find(targets.begin(), targets.end(), idx) != targets.end())
        Fail(state::kDupColumn, "column \"" + name + "\" specified more than once");
      targets.push_back(idx);
    }
  }
  int64_t n = 0;
  for (auto& vals : s.rows) {
    if (vals.size() > targets.size())
      Fail(state::kSyntax, "INSERT has more expressions than target columns");
    if (!s.columns.empty() && !vals.empty() && vals.size() < targets.size())
      Fail(state::kSyntax, "INSERT has more target columns than expressions");
    std::vector<Value> row(sc.cols.size());
    for (size_t i = 0; i < vals.size(); i++) {
      Bind(*vals[i], nullptr);
      row[targets[i]] = StoreValue(Eval(*vals[i], {}), sc.cols[targets[i]]);
    }
    CheckNotNull(sc, row);
    std::string key =
        sc.hidden_rowid ? EncodeKey({Value::Int(e_.NextRowId(*tbl))}) : RowKey(*tbl, row);
    std::string enc = EncodeRow(row);
    try {
      e_.Write(*t_, *tbl->tree, key, Engine::Op::Insert,
               [&](const std::string*) { return std::optional<std::string>(enc); });
    } catch (const DbError& err) {
      if (err.code() == state::kUnique) Duplicate(sc);
      throw;
    }
    n++;
  }
  QueryResult q;
  q.tag = "INSERT 0 " + std::to_string(n);
  return q;
}

QueryResult Executor::Update(UpdateStmt& s) {
  auto tbl = RequireTable(e_, s.table);
  const Schema& sc = tbl->schema;
  std::vector<std::pair<size_t, Expr*>> sets;
  bool pk_touched = false;
  for (auto& [name, ex] : s.sets) {
    size_t idx = sc.cols.size();
    for (size_t i = 0; i < sc.cols.size(); i++) {
      if (sc.cols[i].name == name) idx = i;
    }
    if (idx == sc.cols.size())
      Fail(state::kNoColumn,
           "column \"" + name + "\" of relation \"" + sc.name + "\" does not exist");
    Bind(*ex, &sc);
    if (HasAgg(*ex)) Fail(state::kGrouping, "aggregate functions are not allowed in UPDATE");
    if (std::find(sc.pk.begin(), sc.pk.end(), static_cast<int>(idx)) != sc.pk.end())
      pk_touched = true;
    sets.emplace_back(idx, ex.get());
  }
  if (s.where) {
    Bind(*s.where, &sc);
    if (HasAgg(*s.where)) Fail(state::kGrouping, "aggregate functions are not allowed in WHERE");
  }
  std::vector<std::pair<std::string, std::string>> cand;
  Each(*tbl, Plan(sc, s.where.get()), [&](std::string_view k, std::vector<Value>& row) {
    if (Matches(s.where.get(), row)) cand.emplace_back(std::string(k), EncodeRow(row));
    return true;
  });
  int64_t n = 0;
  for (auto& [key, orig] : cand) {
    bool done = false;
    std::optional<std::vector<Value>> moved;
    e_.Write(*t_, *tbl->tree, key, Engine::Op::Update,
             [&](const std::string* latest) -> std::optional<std::string> {
               std::vector<Value> cur = DecodeRow(*latest);
               if (*latest != orig && !Matches(s.where.get(), cur)) return std::nullopt;
               std::vector<Value> next = cur;
               for (auto& [ci, ex] : sets) next[ci] = StoreValue(Eval(*ex, cur), sc.cols[ci]);
               CheckNotNull(sc, next);
               if (pk_touched && RowKey(*tbl, next) != key) {
                 moved = next;
                 return std::nullopt;
               }
               done = true;
               return EncodeRow(next);
             });
    if (moved) {
      e_.Write(*t_, *tbl->tree, key, Engine::Op::Delete,
               [](const std::string*) { return std::optional<std::string>(std::string()); });
      std::string enc = EncodeRow(*moved);
      try {
        e_.Write(*t_, *tbl->tree, RowKey(*tbl, *moved), Engine::Op::Insert,
                 [&](const std::string*) { return std::optional<std::string>(enc); });
      } catch (const DbError& err) {
        if (err.code() == state::kUnique) Duplicate(sc);
        throw;
      }
      done = true;
    }
    if (done) n++;
  }
  QueryResult q;
  q.tag = "UPDATE " + std::to_string(n);
  return q;
}

QueryResult Executor::Delete(DeleteStmt& s) {
  auto tbl = RequireTable(e_, s.table);
  const Schema& sc = tbl->schema;
  if (s.where) {
    Bind(*s.where, &sc);
    if (HasAgg(*s.where)) Fail(state::kGrouping, "aggregate functions are not allowed in WHERE");
  }
  std::vector<std::pair<std::string, std::string>> cand;
  Each(*tbl, Plan(sc, s.where.get()), [&](std::string_view k, std::vector<Value>& row) {
    if (Matches(s.where.get(), row)) cand.emplace_back(std::string(k), EncodeRow(row));
    return true;
  });
  int64_t n = 0;
  for (auto& [key, orig] : cand) {
    bool done = false;
    e_.Write(*t_, *tbl->tree, key, Engine::Op::Delete,
             [&](const std::string* latest) -> std::optional<std::string> {
               if (*latest != orig && !Matches(s.where.get(), DecodeRow(*latest)))
                 return std::nullopt;
               done = true;
               return std::string();
             });
    if (done) n++;
  }
  QueryResult q;
  q.tag = "DELETE " + std::to_string(n);
  return q;
}

QueryResult Executor::Explain(Statement& st) {
  Statement& in = *st.inner;
  std::vector<std::string> lines;
  auto scan = [&](const std::string& table, Expr* where, size_t indent) {
    auto tbl = RequireTable(e_, table);
    const Schema& sc = tbl->schema;
    if (where) Bind(*where, &sc);
    Access a = Plan(sc, where);
    std::string pad(indent, ' ');
    std::string line;
    switch (a.kind) {
      case Access::Kind::Point:
        line = "Index Scan using " + sc.name + "_pkey on " + sc.name;
        break;
      case Access::Kind::Multi:
        line = "Index Scan using " + sc.name + "_pkey on " + sc.name + " (" +
               std::to_string(a.keys.size()) + " keys)";
        break;
      case Access::Kind::Range:
        line = "Index Range Scan using " + sc.name + "_pkey on " + sc.name;
        break;
      case Access::Kind::Full:
        line = "Seq Scan on " + sc.name;
        break;
    }
    lines.push_back(pad + line);
    if (where) lines.push_back(pad + "  Filter: " + ExprText(*where));
  };
  switch (in.kind) {
    case Statement::Kind::Select: {
      SelectStmt& s = *in.select;
      size_t indent = 0;
      if (s.limit) {
        lines.push_back("Limit");
        indent += 2;
      }
      if (!s.order_by.empty()) {
        lines.push_back(std::string(indent, ' ') + "Sort");
        indent += 2;
      }
      bool agg = !s.group_by.empty() || s.having;
      for (auto& it : s.items) {
        if (it.expr && HasAgg(*it.expr)) agg = true;
      }
      if (agg) {
        lines.push_back(std::string(indent, ' ') +
                        (s.group_by.empty() ? "Aggregate" : "HashAggregate"));
        indent += 2;
      }
      if (s.table.empty()) {
        lines.push_back(std::string(indent, ' ') + "Result");
      } else {
        scan(s.table, s.where.get(), indent);
      }
      break;
    }
    case Statement::Kind::Update:
      lines.push_back("Update on " + in.update->table);
      scan(in.update->table, in.update->where.get(), 2);
      break;
    case Statement::Kind::Delete:
      lines.push_back("Delete on " + in.del->table);
      scan(in.del->table, in.del->where.get(), 2);
      break;
    case Statement::Kind::Insert:
      RequireTable(e_, in.insert->table);
      lines.push_back("Insert on " + in.insert->table);
      lines.push_back("  Values (" + std::to_string(in.insert->rows.size()) + " rows)");
      break;
    default:
      Fail(state::kNotSupported, "EXPLAIN only supports SELECT, INSERT, UPDATE and DELETE");
  }
  QueryResult q;
  q.has_rows = true;
  q.columns.push_back({"QUERY PLAN", ColType::Text});
  for (auto& l : lines) q.rows.push_back({Value::Text(l)});
  q.tag = "EXPLAIN";
  return q;
}

}
