#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <optional>

#include "executor.h"

namespace rdbms {

class Session {
 public:
  Session(Engine& e, Isolation default_iso);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  QueryResult Execute(Statement& st, const std::vector<Value>& params);
  std::vector<ResultColumn> Describe(Statement& st);
  void OnError();
  char Status() const;
  void Cancel();
  void Close();

 private:
  QueryResult Run(Statement& st, const std::vector<Value>& params);
  QueryResult Show(const std::string& name);
  void SetVar(const std::string& name, const std::string& value, QueryResult& q);
  Txn* Current();
  Txn* Take();
  void EndBlock();
  Isolation CurrentIsolation() const;

  Engine& e_;
  Txn* txn_ = nullptr;
  bool block_ = false;
  bool failed_ = false;
  Isolation default_iso_;
  bool default_ro_ = false;
  std::optional<Isolation> block_iso_;
  bool block_ro_ = false;
  std::map<std::string, std::string> vars_;
  std::mutex txn_mu_;
};

}
