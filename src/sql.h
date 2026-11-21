#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "txn.h"
#include "value.h"

namespace rdbms {

enum class ExprKind {
  Const,
  Column,
  Param,
  Unary,
  Binary,
  Func,
  IsNull,
  Between,
  InList,
  Like,
  Cast,
  Case
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::Const;
  Value value;
  std::string name;
  std::string op;
  int param = 0;
  std::vector<ExprPtr> args;
  bool negated = false;
  bool distinct = false;
  bool star = false;
  ColType cast_type = ColType::Text;
  int32_t cast_len = -1;
  int col = -1;
  int agg = -1;
};

struct SelectItem {
  ExprPtr expr;
  std::string alias;
  bool star = false;
};

struct OrderItem {
  ExprPtr expr;
  bool desc = false;
};

struct SelectStmt {
  std::vector<SelectItem> items;
  std::string table;
  ExprPtr where;
  std::vector<ExprPtr> group_by;
  ExprPtr having;
  std::vector<OrderItem> order_by;
  ExprPtr limit;
  ExprPtr offset;
  bool for_update = false;
  bool distinct = false;
};

struct InsertStmt {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<ExprPtr>> rows;
};

struct UpdateStmt {
  std::string table;
  std::vector<std::pair<std::string, ExprPtr>> sets;
  ExprPtr where;
};

struct DeleteStmt {
  std::string table;
  ExprPtr where;
};

struct Statement {
  enum class Kind {
    Empty,
    Select,
    Insert,
    Update,
    Delete,
    CreateTable,
    DropTable,
    Begin,
    Commit,
    Rollback,
    SetTxn,
    SetVar,
    Show,
    Checkpoint,
    Vacuum,
    Analyze,
    Explain,
    Reset
  };
  Kind kind = Kind::Empty;
  std::unique_ptr<SelectStmt> select;
  std::unique_ptr<InsertStmt> insert;
  std::unique_ptr<UpdateStmt> update;
  std::unique_ptr<DeleteStmt> del;
  Schema schema;
  std::string name;
  std::string value;
  bool flag = false;
  bool session = false;
  std::optional<Isolation> iso;
  std::optional<bool> read_only;
  std::unique_ptr<Statement> inner;
  int params = 0;
  std::string text;
};

std::vector<Statement> ParseSql(const std::string& sql);
bool IsAggregate(const std::string& fn);

}
