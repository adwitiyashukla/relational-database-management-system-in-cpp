#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>

#include "sql.h"

namespace rdbms {

namespace {

enum class Tok { End, Ident, QIdent, Int, Float, Str, Param, Op };

struct Token {
  Tok kind = Tok::End;
  std::string text;
  size_t pos = 0;
};

[[noreturn]] void SyntaxError(const std::string& near) {
  if (near.empty()) Fail(state::kSyntax, "syntax error at end of input");
  Fail(state::kSyntax, "syntax error at or near \"" + near + "\"");
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::vector<Token> Lex(const std::string& s) {
  std::vector<Token> out;
  size_t i = 0, n = s.size();
  while (i < n) {
    char c = s[i];
    if (IsSpace(c)) {
      i++;
      continue;
    }
    if (c == '-' && i + 1 < n && s[i + 1] == '-') {
      while (i < n && s[i] != '\n') i++;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t e = s.find("*/", i + 2);
      if (e == std::string::npos) SyntaxError("");
      i = e + 2;
      continue;
    }
    Token t;
    t.pos = i;
    if ((c == 'e' || c == 'E') && i + 1 < n && s[i + 1] == '\'') {
      size_t j = i + 2;
      std::string v;
      for (;;) {
        if (j >= n) Fail(state::kSyntax, "unterminated quoted string");
        char d = s[j];
        if (d == '\'') {
          if (j + 1 < n && s[j + 1] == '\'') {
            v.push_back('\'');
            j += 2;
            continue;
          }
          j++;
          break;
        }
        if (d == '\\' && j + 1 < n) {
          char e = s[j + 1];
          v.push_back(e == 'n'   ? '\n'
                      : e == 't' ? '\t'
                      : e == 'r' ? '\r'
                      : e == 'b' ? '\b'
                      : e == 'f' ? '\f'
                                 : e);
          j += 2;
          continue;
        }
        v.push_back(d);
        j++;
      }
      t.kind = Tok::Str;
      t.text = v;
      i = j;
    } else if (IsAlpha(c)) {
      size_t j = i;
      while (j < n && (IsAlpha(s[j]) || IsDigit(s[j]) || s[j] == '$')) j++;
      t.kind = Tok::Ident;
      t.text = Lower(s.substr(i, j - i));
      i = j;
    } else if (c == '"') {
      size_t j = i + 1;
      std::string v;
      for (;;) {
        if (j >= n) Fail(state::kSyntax, "unterminated quoted identifier");
        if (s[j] == '"') {
          if (j + 1 < n && s[j + 1] == '"') {
            v.push_back('"');
            j += 2;
            continue;
          }
          j++;
          break;
        }
        v.push_back(s[j++]);
      }
      if (v.empty()) Fail(state::kSyntax, "zero-length delimited identifier");
      t.kind = Tok::QIdent;
      t.text = v;
      i = j;
    } else if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(s[i + 1]))) {
      size_t j = i;
      bool flt = false;
      while (j < n && IsDigit(s[j])) j++;
      if (j < n && s[j] == '.') {
        flt = true;
        j++;
        while (j < n && IsDigit(s[j])) j++;
      }
      if (j < n && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (s[k] == '+' || s[k] == '-')) k++;
        if (k < n && IsDigit(s[k])) {
          flt = true;
          j = k;
          while (j < n && IsDigit(s[j])) j++;
        }
      }
      t.kind = flt ? Tok::Float : Tok::Int;
      t.text = s.substr(i, j - i);
      i = j;
    } else if (c == '\'') {
      size_t j = i + 1;
      std::string v;
      for (;;) {
        if (j >= n) Fail(state::kSyntax, "unterminated quoted string");
        if (s[j] == '\'') {
          if (j + 1 < n && s[j + 1] == '\'') {
            v.push_back('\'');
            j += 2;
            continue;
          }
          j++;
          break;
        }
        v.push_back(s[j++]);
      }
      t.kind = Tok::Str;
      t.text = v;
      i = j;
    } else if (c == '$' && i + 1 < n && IsDigit(s[i + 1])) {
      size_t j = i + 1;
      while (j < n && IsDigit(s[j])) j++;
      t.kind = Tok::Param;
      t.text = s.substr(i + 1, j - i - 1);
      i = j;
    } else {
      static const char* kTwo[] = {"<=", ">=", "<>", "!=", "||", "::"};
      bool matched = false;
      for (const char* op : kTwo) {
        if (s.compare(i, 2, op) == 0) {
          t.kind = Tok::Op;
          t.text = op;
          i += 2;
          matched = true;
          break;
        }
      }
      if (!matched) {
        if (std::string("=<>+-*/%(),;.[]").find(c) == std::string::npos)
          SyntaxError(std::string(1, c));
        t.kind = Tok::Op;
        t.text = std::string(1, c);
        i++;
      }
    }
    out.push_back(std::move(t));
  }
  Token end;
  end.pos = n;
  out.push_back(end);
  return out;
}

const std::set<std::string>& Reserved() {
  static const std::set<std::string> r = {
      "from",  "where",   "group",     "having",  "order",  "limit",     "offset",
      "for",   "union",   "intersect", "except",  "join",   "inner",     "left",
      "right", "full",    "cross",     "on",      "as",     "and",       "or",
      "not",   "into",    "values",    "set",     "select", "returning", "window",
      "fetch", "natural", "using",     "then",    "else",   "end",       "when",
      "is",    "in",      "like",      "between", "asc",    "desc"};
  return r;
}

ExprPtr Make(ExprKind k) {
  auto e = std::make_unique<Expr>();
  e->kind = k;
  return e;
}

ExprPtr Const(Value v) {
  auto e = Make(ExprKind::Const);
  e->value = std::move(v);
  return e;
}

ExprPtr Bin(const std::string& op, ExprPtr l, ExprPtr r) {
  auto e = Make(ExprKind::Binary);
  e->op = op;
  e->args.push_back(std::move(l));
  e->args.push_back(std::move(r));
  return e;
}

ExprPtr Un(const std::string& op, ExprPtr x) {
  auto e = Make(ExprKind::Unary);
  e->op = op;
  e->args.push_back(std::move(x));
  return e;
}

class Parser {
 public:
  Parser(std::vector<Token> toks, const std::string& src) : t_(std::move(toks)), src_(src) {}

  std::vector<Statement> All() {
    std::vector<Statement> out;
    for (;;) {
      while (AcceptOp(";")) {
      }
      if (Peek().kind == Tok::End) break;
      size_t start = Peek().pos;
      params_ = 0;
      Statement st = Stmt();
      st.params = params_;
      st.text = Trim(src_.substr(start, Peek().pos - start));
      if (!IsOp(";") && Peek().kind != Tok::End) SyntaxError(Peek().text);
      out.push_back(std::move(st));
    }
    return out;
  }

 private:
  const Token& Peek(size_t k = 0) const { return t_[std::min(pos_ + k, t_.size() - 1)]; }
  Token Next() {
    Token t = Peek();
    if (pos_ < t_.size() - 1) pos_++;
    return t;
  }
  bool IsWord(const char* w, size_t k = 0) const {
    const Token& t = Peek(k);
    return t.kind == Tok::Ident && t.text == w;
  }
  bool IsOp(const char* o, size_t k = 0) const {
    const Token& t = Peek(k);
    return t.kind == Tok::Op && t.text == o;
  }
  bool AcceptWord(const char* w) {
    if (!IsWord(w)) return false;
    pos_++;
    return true;
  }
  bool AcceptOp(const char* o) {
    if (!IsOp(o)) return false;
    pos_++;
    return true;
  }
  void ExpectWord(const char* w) {
    if (!AcceptWord(w)) SyntaxError(Peek().text);
  }
  void ExpectOp(const char* o) {
    if (!AcceptOp(o)) SyntaxError(Peek().text);
  }
  std::string Name() {
    const Token& t = Peek();
    if (t.kind != Tok::Ident && t.kind != Tok::QIdent) SyntaxError(t.text);
    pos_++;
    return t.text;
  }
  bool AliasNext() const {
    const Token& t = Peek();
    return t.kind == Tok::QIdent || (t.kind == Tok::Ident && !Reserved().count(t.text));
  }
  void SkipRest() {
    while (!IsOp(";") && Peek().kind != Tok::End) pos_++;
  }

  Statement Stmt() {
    Statement st;
    if (AcceptWord("select")) {
      st.kind = Statement::Kind::Select;
      st.select = Select();
    } else if (AcceptWord("insert")) {
      st.kind = Statement::Kind::Insert;
      st.insert = Insert();
    } else if (AcceptWord("update")) {
      st.kind = Statement::Kind::Update;
      st.update = Update();
    } else if (AcceptWord("delete")) {
      st.kind = Statement::Kind::Delete;
      st.del = Delete();
    } else if (AcceptWord("create")) {
      ExpectWord("table");
      CreateTable(st);
    } else if (AcceptWord("drop")) {
      ExpectWord("table");
      st.kind = Statement::Kind::DropTable;
      if (AcceptWord("if")) {
        ExpectWord("exists");
        st.flag = true;
      }
      st.name = Name();
    } else if (AcceptWord("begin")) {
      st.kind = Statement::Kind::Begin;
      if (!AcceptWord("work")) AcceptWord("transaction");
      Modes(st);
    } else if (AcceptWord("start")) {
      ExpectWord("transaction");
      st.kind = Statement::Kind::Begin;
      Modes(st);
    } else if (AcceptWord("commit") || AcceptWord("end")) {
      st.kind = Statement::Kind::Commit;
      if (!AcceptWord("work")) AcceptWord("transaction");
    } else if (AcceptWord("rollback") || AcceptWord("abort")) {
      st.kind = Statement::Kind::Rollback;
      if (!AcceptWord("work")) AcceptWord("transaction");
    } else if (AcceptWord("set")) {
      Set(st);
    } else if (AcceptWord("show")) {
      st.kind = Statement::Kind::Show;
      st.name = ShowName();
    } else if (AcceptWord("checkpoint")) {
      st.kind = Statement::Kind::Checkpoint;
    } else if (AcceptWord("vacuum")) {
      st.kind = Statement::Kind::Vacuum;
      SkipRest();
    } else if (AcceptWord("analyze")) {
      st.kind = Statement::Kind::Analyze;
      SkipRest();
    } else if (AcceptWord("explain")) {
      st.kind = Statement::Kind::Explain;
      if (IsWord("analyze")) Fail(state::kNotSupported, "EXPLAIN ANALYZE is not supported");
      st.inner = std::make_unique<Statement>(Stmt());
    } else if (AcceptWord("reset") || AcceptWord("discard")) {
      st.kind = Statement::Kind::Reset;
      st.name = Lower(t_[pos_ - 1].text);
      SkipRest();
    } else {
      SyntaxError(Peek().text);
    }
    return st;
  }

  Isolation Level() {
    if (AcceptWord("serializable")) return Isolation::Serializable;
    if (AcceptWord("repeatable")) {
      ExpectWord("read");
      return Isolation::RepeatableRead;
    }
    ExpectWord("read");
    if (AcceptWord("committed")) return Isolation::ReadCommitted;
    ExpectWord("uncommitted");
    return Isolation::ReadCommitted;
  }

  void Modes(Statement& st) {
    for (;;) {
      if (AcceptWord("isolation")) {
        ExpectWord("level");
        st.iso = Level();
      } else if (AcceptWord("read")) {
        if (AcceptWord("only")) {
          st.read_only = true;
        } else {
          ExpectWord("write");
          st.read_only = false;
        }
      } else if (AcceptWord("not")) {
        ExpectWord("deferrable");
      } else if (AcceptWord("deferrable")) {
      } else {
        break;
      }
      AcceptOp(",");
    }
  }

  void Set(Statement& st) {
    bool session = AcceptWord("session");
    if (!session) AcceptWord("local");
    if (AcceptWord("transaction")) {
      st.kind = Statement::Kind::SetTxn;
      Modes(st);
      return;
    }
    if (session && AcceptWord("characteristics")) {
      ExpectWord("as");
      ExpectWord("transaction");
      st.kind = Statement::Kind::SetTxn;
      st.session = true;
      Modes(st);
      return;
    }
    st.kind = Statement::Kind::SetVar;
    if (AcceptWord("time")) {
      ExpectWord("zone");
      st.name = "timezone";
    } else {
      st.name = Name();
      while (AcceptOp(".")) st.name += "." + Name();
      if (!AcceptWord("to")) ExpectOp("=");
    }
    std::string v;
    while (!IsOp(";") && Peek().kind != Tok::End) {
      Token t = Next();
      if (t.kind == Tok::Op && t.text == ",") {
        v += ",";
        continue;
      }
      if (!v.empty() && v.back() != ',') v += " ";
      v += t.text;
    }
    if (v.empty()) SyntaxError("");
    st.value = v;
  }

  std::string ShowName() {
    if (AcceptWord("transaction")) {
      ExpectWord("isolation");
      ExpectWord("level");
      return "transaction_isolation";
    }
    if (AcceptWord("time")) {
      ExpectWord("zone");
      return "timezone";
    }
    std::string n = Name();
    while (AcceptOp(".")) n += "." + Name();
    return n;
  }

  void TypeName(ColType* type, int32_t* len) {
    std::string n = Name();
    if (n == "double") {
      ExpectWord("precision");
      n = "double precision";
    } else if (n == "character" && AcceptWord("varying")) {
      n = "character varying";
    } else if (n == "timestamp" || n == "timestamptz") {
      if (AcceptWord("without") || AcceptWord("with")) {
        ExpectWord("time");
        ExpectWord("zone");
      }
      n = "timestamp";
    }
    if (!ParseColType(n, type)) Fail("42704", "type \"" + n + "\" does not exist");
    *len = -1;
    if (AcceptOp("(")) {
      Token t = Next();
      if (t.kind != Tok::Int) SyntaxError(t.text);
      *len = static_cast<int32_t>(std::stol(t.text));
      if (AcceptOp(",")) Next();
      ExpectOp(")");
    }
    if (*type == ColType::Char && *len < 0) *len = 1;
  }

  void CreateTable(Statement& st) {
    st.kind = Statement::Kind::CreateTable;
    if (AcceptWord("if")) {
      ExpectWord("not");
      ExpectWord("exists");
      st.flag = true;
    }
    Schema& s = st.schema;
    s.name = Name();
    ExpectOp("(");
    std::vector<std::string> pk;
    bool pk_seen = false;
    auto set_pk = [&](std::vector<std::string> cols) {
      if (pk_seen)
        Fail("42P16", "multiple primary keys for table \"" + s.name + "\" are not allowed");
      pk_seen = true;
      pk = std::move(cols);
    };
    do {
      if (AcceptWord("constraint")) Name();
      if (AcceptWord("primary")) {
        ExpectWord("key");
        ExpectOp("(");
        std::vector<std::string> cols;
        do cols.push_back(Name());
        while (AcceptOp(","));
        ExpectOp(")");
        set_pk(cols);
        continue;
      }
      Column c;
      c.name = Name();
      TypeName(&c.type, &c.len);
      for (;;) {
        if (AcceptWord("not")) {
          ExpectWord("null");
          c.not_null = true;
        } else if (AcceptWord("null")) {
        } else if (AcceptWord("primary")) {
          ExpectWord("key");
          set_pk({c.name});
        } else if (IsWord("default") || IsWord("references") || IsWord("unique") ||
                   IsWord("check")) {
          Fail(state::kNotSupported, "column option " + Upper(Peek().text) + " is not supported");
        } else {
          break;
        }
      }
      for (auto& o : s.cols) {
        if (o.name == c.name)
          Fail(state::kDupColumn, "column \"" + c.name + "\" specified more than once");
      }
      s.cols.push_back(c);
    } while (AcceptOp(","));
    ExpectOp(")");
    if (AcceptWord("with")) {
      ExpectOp("(");
      int depth = 1;
      while (depth > 0) {
        Token t = Next();
        if (t.kind == Tok::End) SyntaxError("");
        if (t.kind == Tok::Op && t.text == "(") depth++;
        if (t.kind == Tok::Op && t.text == ")") depth--;
      }
    }
    if (s.cols.empty()) Fail(state::kSyntax, "a table needs at least one column");
    for (auto& name : pk) {
      int idx = -1;
      for (size_t i = 0; i < s.cols.size(); i++) {
        if (s.cols[i].name == name) idx = static_cast<int>(i);
      }
      if (idx < 0) Fail(state::kNoColumn, "column \"" + name + "\" named in key does not exist");
      s.cols[static_cast<size_t>(idx)].not_null = true;
      s.pk.push_back(idx);
    }
    s.hidden_rowid = s.pk.empty();
  }

  static std::string Upper(std::string s) {
    for (auto& c : s) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
  }

  std::unique_ptr<SelectStmt> Select() {
    auto s = std::make_unique<SelectStmt>();
    if (AcceptWord("distinct")) {
      s->distinct = true;
      if (IsWord("on")) Fail(state::kNotSupported, "DISTINCT ON is not supported");
    } else {
      AcceptWord("all");
    }
    do {
      SelectItem it;
      if (AcceptOp("*")) {
        it.star = true;
      } else if ((Peek().kind == Tok::Ident || Peek().kind == Tok::QIdent) && IsOp(".", 1) &&
                 IsOp("*", 2)) {
        pos_ += 3;
        it.star = true;
      } else {
        it.expr = Expression();
        if (AcceptWord("as")) {
          it.alias = Name();
        } else if (AliasNext()) {
          it.alias = Name();
        }
      }
      s->items.push_back(std::move(it));
    } while (AcceptOp(","));
    if (AcceptWord("from")) {
      if (IsOp("(")) Fail(state::kNotSupported, "subqueries in FROM are not supported");
      s->table = Name();
      if (AcceptOp(".")) s->table = Name();
      if (AcceptWord("as")) {
        Name();
      } else if (AliasNext()) {
        Name();
      }
      if (IsOp(",") || IsWord("join") || IsWord("inner") || IsWord("left") || IsWord("right") ||
          IsWord("cross") || IsWord("full") || IsWord("natural"))
        Fail(state::kNotSupported, "joins are not supported");
    }
    if (AcceptWord("where")) s->where = Expression();
    if (AcceptWord("group")) {
      ExpectWord("by");
      do s->group_by.push_back(Expression());
      while (AcceptOp(","));
    }
    if (AcceptWord("having")) s->having = Expression();
    if (IsWord("union") || IsWord("intersect") || IsWord("except"))
      Fail(state::kNotSupported, "set operations are not supported");
    if (AcceptWord("order")) {
      ExpectWord("by");
      do {
        OrderItem o;
        o.expr = Expression();
        if (AcceptWord("desc")) {
          o.desc = true;
        } else {
          AcceptWord("asc");
        }
        if (IsWord("nulls"))
          Fail(state::kNotSupported, "NULLS FIRST and NULLS LAST are not supported");
        s->order_by.push_back(std::move(o));
      } while (AcceptOp(","));
    }
    for (int round = 0; round < 2; round++) {
      if (AcceptWord("limit")) {
        if (!AcceptWord("all")) s->limit = Expression();
      }
      if (AcceptWord("offset")) {
        s->offset = Expression();
        if (!AcceptWord("rows")) AcceptWord("row");
      }
    }
    if (AcceptWord("for")) {
      if (!AcceptWord("update")) {
        if (!AcceptWord("no")) {
          ExpectWord("share");
        } else {
          ExpectWord("key");
          ExpectWord("update");
        }
      }
      s->for_update = true;
      if (AcceptWord("of")) Name();
      if (IsWord("nowait") || IsWord("skip"))
        Fail(state::kNotSupported, "NOWAIT and SKIP LOCKED are not supported");
    }
    return s;
  }

  std::unique_ptr<InsertStmt> Insert() {
    auto s = std::make_unique<InsertStmt>();
    ExpectWord("into");
    s->table = Name();
    if (AcceptOp(".")) s->table = Name();
    if (AcceptOp("(")) {
      do s->columns.push_back(Name());
      while (AcceptOp(","));
      ExpectOp(")");
    }
    if (AcceptWord("default")) {
      ExpectWord("values");
      s->rows.emplace_back();
    } else {
      if (IsWord("select")) Fail(state::kNotSupported, "INSERT ... SELECT is not supported");
      ExpectWord("values");
      do {
        ExpectOp("(");
        std::vector<ExprPtr> row;
        do row.push_back(Expression());
        while (AcceptOp(","));
        ExpectOp(")");
        s->rows.push_back(std::move(row));
      } while (AcceptOp(","));
    }
    if (IsWord("returning")) Fail(state::kNotSupported, "RETURNING is not supported");
    if (IsWord("on")) Fail(state::kNotSupported, "ON CONFLICT is not supported");
    return s;
  }

  std::unique_ptr<UpdateStmt> Update() {
    auto s = std::make_unique<UpdateStmt>();
    s->table = Name();
    if (AcceptOp(".")) s->table = Name();
    if (AcceptWord("as")) Name();
    ExpectWord("set");
    do {
      std::string col = Name();
      ExpectOp("=");
      s->sets.emplace_back(col, Expression());
    } while (AcceptOp(","));
    if (IsWord("from")) Fail(state::kNotSupported, "UPDATE ... FROM is not supported");
    if (AcceptWord("where")) s->where = Expression();
    if (IsWord("returning")) Fail(state::kNotSupported, "RETURNING is not supported");
    return s;
  }

  std::unique_ptr<DeleteStmt> Delete() {
    auto s = std::make_unique<DeleteStmt>();
    ExpectWord("from");
    s->table = Name();
    if (AcceptOp(".")) s->table = Name();
    if (AcceptWord("as")) Name();
    if (AcceptWord("where")) s->where = Expression();
    if (IsWord("returning")) Fail(state::kNotSupported, "RETURNING is not supported");
    return s;
  }

  ExprPtr Expression() { return Or(); }

  ExprPtr Or() {
    ExprPtr l = And();
    while (AcceptWord("or")) l = Bin("or", std::move(l), And());
    return l;
  }

  ExprPtr And() {
    ExprPtr l = Not();
    while (AcceptWord("and")) l = Bin("and", std::move(l), Not());
    return l;
  }

  ExprPtr Not() {
    if (AcceptWord("not")) return Un("not", Not());
    return Is();
  }

  ExprPtr Is() {
    ExprPtr l = Cmp();
    while (AcceptWord("is")) {
      bool neg = AcceptWord("not");
      if (AcceptWord("null")) {
        auto e = Make(ExprKind::IsNull);
        e->negated = neg;
        e->args.push_back(std::move(l));
        l = std::move(e);
      } else if (AcceptWord("true") || AcceptWord("false")) {
        auto e = Make(ExprKind::Func);
        e->name = t_[pos_ - 1].text == "true" ? "is_true" : "is_false";
        e->negated = neg;
        e->args.push_back(std::move(l));
        l = std::move(e);
      } else {
        SyntaxError(Peek().text);
      }
    }
    return l;
  }

  ExprPtr Cmp() {
    ExprPtr l = Pred();
    for (;;) {
      static const char* kOps[] = {"=", "<>", "!=", "<=", ">=", "<", ">"};
      bool hit = false;
      for (const char* op : kOps) {
        if (AcceptOp(op)) {
          l = Bin(std::string(op) == "!=" ? "<>" : op, std::move(l), Pred());
          hit = true;
          break;
        }
      }
      if (!hit) break;
    }
    return l;
  }

  ExprPtr Pred() {
    ExprPtr l = Concat();
    for (;;) {
      size_t save = pos_;
      bool neg = AcceptWord("not");
      if (AcceptWord("between")) {
        auto e = Make(ExprKind::Between);
        e->negated = neg;
        e->args.push_back(std::move(l));
        e->args.push_back(Concat());
        ExpectWord("and");
        e->args.push_back(Concat());
        l = std::move(e);
      } else if (AcceptWord("in")) {
        ExpectOp("(");
        if (IsWord("select")) Fail(state::kNotSupported, "subqueries are not supported");
        auto e = Make(ExprKind::InList);
        e->negated = neg;
        e->args.push_back(std::move(l));
        do e->args.push_back(Expression());
        while (AcceptOp(","));
        ExpectOp(")");
        l = std::move(e);
      } else if (AcceptWord("like") || AcceptWord("ilike")) {
        auto e = Make(ExprKind::Like);
        e->negated = neg;
        e->distinct = t_[pos_ - 1].text == "ilike";
        e->args.push_back(std::move(l));
        e->args.push_back(Concat());
        l = std::move(e);
      } else {
        pos_ = save;
        break;
      }
    }
    return l;
  }

  ExprPtr Concat() {
    ExprPtr l = Add();
    while (AcceptOp("||")) l = Bin("||", std::move(l), Add());
    return l;
  }

  ExprPtr Add() {
    ExprPtr l = Mul();
    for (;;) {
      if (AcceptOp("+")) {
        l = Bin("+", std::move(l), Mul());
      } else if (AcceptOp("-")) {
        l = Bin("-", std::move(l), Mul());
      } else {
        break;
      }
    }
    return l;
  }

  ExprPtr Mul() {
    ExprPtr l = Unary();
    for (;;) {
      if (AcceptOp("*")) {
        l = Bin("*", std::move(l), Unary());
      } else if (AcceptOp("/")) {
        l = Bin("/", std::move(l), Unary());
      } else if (AcceptOp("%")) {
        l = Bin("%", std::move(l), Unary());
      } else {
        break;
      }
    }
    return l;
  }

  ExprPtr Unary() {
    if (AcceptOp("-")) {
      ExprPtr e = Unary();
      if (e->kind == ExprKind::Const && e->value.type == Type::Int &&
          e->value.i != std::numeric_limits<int64_t>::min()) {
        e->value.i = -e->value.i;
        return e;
      }
      if (e->kind == ExprKind::Const && e->value.type == Type::Float) {
        e->value.f = -e->value.f;
        return e;
      }
      return Un("neg", std::move(e));
    }
    if (AcceptOp("+")) return Unary();
    return Postfix();
  }

  ExprPtr Postfix() {
    ExprPtr e = Primary();
    while (AcceptOp("::")) {
      auto c = Make(ExprKind::Cast);
      TypeName(&c->cast_type, &c->cast_len);
      c->args.push_back(std::move(e));
      e = std::move(c);
    }
    return e;
  }

  ExprPtr Primary() {
    const Token t = Peek();
    switch (t.kind) {
      case Tok::Int: {
        pos_++;
        int64_t v;
        if (ParseInt(t.text, &v)) return Const(Value::Int(v));
        return Const(Value::Float(std::strtod(t.text.c_str(), nullptr)));
      }
      case Tok::Float:
        pos_++;
        return Const(Value::Float(std::strtod(t.text.c_str(), nullptr)));
      case Tok::Str:
        pos_++;
        return Const(Value::Text(t.text));
      case Tok::Param: {
        pos_++;
        auto e = Make(ExprKind::Param);
        e->param = std::stoi(t.text);
        if (e->param < 1) Fail(state::kNoParam, "there is no parameter $" + t.text);
        params_ = std::max(params_, e->param);
        return e;
      }
      case Tok::Op:
        if (t.text == "(") {
          pos_++;
          if (IsWord("select")) Fail(state::kNotSupported, "subqueries are not supported");
          ExprPtr e = Expression();
          ExpectOp(")");
          return e;
        }
        SyntaxError(t.text);
      case Tok::QIdent: {
        pos_++;
        auto e = Make(ExprKind::Column);
        e->name = t.text;
        if (AcceptOp(".")) {
          e->op = e->name;
          e->name = Name();
        }
        return e;
      }
      case Tok::Ident:
        return Word();
      case Tok::End:
        SyntaxError("");
    }
    SyntaxError(t.text);
  }

  ExprPtr Word() {
    std::string w = Next().text;
    if (w == "null") return Const(Value::Null());
    if (w == "true") return Const(Value::Bool(true));
    if (w == "false") return Const(Value::Bool(false));
    if (w == "cast" && AcceptOp("(")) {
      auto c = Make(ExprKind::Cast);
      c->args.push_back(Expression());
      ExpectWord("as");
      TypeName(&c->cast_type, &c->cast_len);
      ExpectOp(")");
      return c;
    }
    if (w == "case") return Case();
    if ((w == "current_timestamp" || w == "localtimestamp") && !IsOp("(")) {
      auto f = Make(ExprKind::Func);
      f->name = "now";
      return f;
    }
    if (Peek().kind == Tok::Str) {
      ColType ct;
      if (ParseColType(w, &ct) || w == "timestamp") {
        auto c = Make(ExprKind::Cast);
        c->cast_type = w == "timestamp" ? ColType::Timestamp : ct;
        c->args.push_back(Const(Value::Text(Next().text)));
        return c;
      }
    }
    if (AcceptOp("(")) {
      auto f = Make(ExprKind::Func);
      f->name = w;
      if (AcceptOp("*")) {
        f->star = true;
      } else if (!IsOp(")")) {
        if (AcceptWord("distinct")) f->distinct = true;
        do f->args.push_back(Expression());
        while (AcceptOp(","));
      }
      ExpectOp(")");
      if (IsWord("over")) Fail(state::kNotSupported, "window functions are not supported");
      return f;
    }
    auto e = Make(ExprKind::Column);
    e->name = w;
    if (AcceptOp(".")) {
      e->op = e->name;
      e->name = Name();
    }
    return e;
  }

  ExprPtr Case() {
    auto e = Make(ExprKind::Case);
    if (!IsWord("when")) {
      e->name = "simple";
      e->args.push_back(Expression());
    }
    if (!IsWord("when")) SyntaxError(Peek().text);
    while (AcceptWord("when")) {
      e->args.push_back(Expression());
      ExpectWord("then");
      e->args.push_back(Expression());
    }
    if (AcceptWord("else")) {
      e->negated = true;
      e->args.push_back(Expression());
    }
    ExpectWord("end");
    return e;
  }

  std::vector<Token> t_;
  const std::string& src_;
  size_t pos_ = 0;
  int params_ = 0;
};

}

std::vector<Statement> ParseSql(const std::string& sql) {
  Parser p(Lex(sql), sql);
  return p.All();
}

bool IsAggregate(const std::string& fn) {
  return fn == "count" || fn == "sum" || fn == "min" || fn == "max" || fn == "avg";
}

}
