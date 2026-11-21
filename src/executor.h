#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine.h"
#include "sql.h"

namespace rdbms {

struct ResultColumn {
  std::string name;
  ColType type = ColType::Text;
};

struct QueryResult {
  std::string tag;
  bool has_rows = false;
  std::vector<ResultColumn> columns;
  std::vector<std::vector<Value>> rows;
  std::vector<std::string> notices;
};

struct Access {
  enum class Kind { Point, Multi, Range, Full };
  Kind kind = Kind::Full;
  std::vector<std::string> keys;
  std::string lo;
  std::optional<std::string> hi;
  bool hi_incl = false;
};

class Executor {
 public:
  Executor(Engine& e, Txn* t, const std::vector<Value>& params, int64_t now);
  QueryResult Run(Statement& st);
  static std::vector<ResultColumn> Describe(Engine& e, Statement& st);

  Value Eval(const Expr& e, const std::vector<Value>& row,
             const std::vector<Value>* aggs = nullptr) const;
  Access Plan(const Schema& s, const Expr* where) const;

 private:
  QueryResult Select(SelectStmt& s);
  QueryResult Insert(InsertStmt& s);
  QueryResult Update(UpdateStmt& s);
  QueryResult Delete(DeleteStmt& s);
  QueryResult Explain(Statement& st);
  void Each(Table& tbl, const Access& a,
            const std::function<bool(std::string_view key, std::vector<Value>& row)>& fn);
  bool Matches(const Expr* where, const std::vector<Value>& row) const;
  std::string RowKey(const Table& tbl, const std::vector<Value>& row);

  Engine& e_;
  Txn* t_;
  const std::vector<Value>& params_;
  int64_t now_;
};

std::shared_ptr<Table> RequireTable(Engine& e, const std::string& name);

}
