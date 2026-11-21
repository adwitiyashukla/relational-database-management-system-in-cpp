#include "session.h"

namespace rdbms {

namespace {

std::string Unquote(const std::string& v) {
  std::string s = Trim(v);
  if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
    return s.substr(1, s.size() - 2);
  return s;
}

bool ParseOnOff(const std::string& v, bool* out) {
  std::string s = Lower(Unquote(v));
  if (s == "on" || s == "true" || s == "yes" || s == "1") {
    *out = true;
    return true;
  }
  if (s == "off" || s == "false" || s == "no" || s == "0") {
    *out = false;
    return true;
  }
  return false;
}

const char* WriteVerb(Statement::Kind k) {
  switch (k) {
    case Statement::Kind::Insert:
      return "INSERT";
    case Statement::Kind::Update:
      return "UPDATE";
    case Statement::Kind::Delete:
      return "DELETE";
    default:
      return nullptr;
  }
}

}

Session::Session(Engine& e, Isolation default_iso) : e_(e), default_iso_(default_iso) {}

Session::~Session() { Close(); }

Txn* Session::Current() {
  if (!txn_) {
    Isolation iso = block_ ? block_iso_.value_or(default_iso_) : default_iso_;
    Txn* t = e_.Begin(iso);
    std::lock_guard<std::mutex> g(txn_mu_);
    txn_ = t;
  }
  return txn_;
}

Txn* Session::Take() {
  std::lock_guard<std::mutex> g(txn_mu_);
  Txn* t = txn_;
  txn_ = nullptr;
  return t;
}

void Session::EndBlock() {
  block_ = false;
  failed_ = false;
  block_iso_.reset();
  block_ro_ = false;
}

Isolation Session::CurrentIsolation() const {
  if (txn_) return txn_->iso;
  if (block_) return block_iso_.value_or(default_iso_);
  return default_iso_;
}

char Session::Status() const {
  if (failed_) return 'E';
  return block_ ? 'T' : 'I';
}

void Session::Cancel() {
  std::lock_guard<std::mutex> g(txn_mu_);
  if (txn_) txn_->cancel.store(true);
}

void Session::Close() {
  Txn* t = Take();
  if (t) {
    try {
      e_.Abort(t);
    } catch (...) {
    }
  }
  EndBlock();
}

void Session::OnError() {
  Txn* t = Take();
  if (t) e_.Abort(t);
  if (block_) failed_ = true;
}

std::vector<ResultColumn> Session::Describe(Statement& st) { return Executor::Describe(e_, st); }

QueryResult Session::Execute(Statement& st, const std::vector<Value>& params) {
  using K = Statement::Kind;
  if (failed_ && st.kind != K::Commit && st.kind != K::Rollback)
    Fail(state::kFailedTxn,
         "current transaction is aborted, commands ignored until end of transaction block");
  QueryResult q;
  switch (st.kind) {
    case K::Empty:
      return q;
    case K::Begin:
      q.tag = "BEGIN";
      if (block_) {
        q.notices.push_back("there is already a transaction in progress");
        return q;
      }
      block_ = true;
      block_iso_ = st.iso;
      block_ro_ = st.read_only.value_or(default_ro_);
      return q;
    case K::Commit: {
      q.tag = "COMMIT";
      if (!block_) {
        q.notices.push_back("there is no transaction in progress");
        return q;
      }
      bool was_failed = failed_;
      Txn* t = Take();
      EndBlock();
      if (was_failed) {
        if (t) e_.Abort(t);
        q.tag = "ROLLBACK";
        return q;
      }
      if (t) e_.Commit(t);
      return q;
    }
    case K::Rollback: {
      q.tag = "ROLLBACK";
      if (!block_) q.notices.push_back("there is no transaction in progress");
      Txn* t = Take();
      EndBlock();
      if (t) e_.Abort(t);
      return q;
    }
    case K::SetTxn:
      q.tag = "SET";
      if (st.session) {
        if (st.iso) default_iso_ = *st.iso;
        if (st.read_only) default_ro_ = *st.read_only;
        return q;
      }
      if (!block_) {
        q.notices.push_back("SET TRANSACTION can only be used in transaction blocks");
        return q;
      }
      if (st.iso && txn_ && txn_->snapshot.load() != 0)
        Fail(state::kActiveTxn, "SET TRANSACTION ISOLATION LEVEL must be called before any query");
      if (st.iso) block_iso_ = st.iso;
      if (st.read_only) block_ro_ = *st.read_only;
      return q;
    case K::SetVar:
      q.tag = "SET";
      SetVar(st.name, st.value, q);
      return q;
    case K::Show:
      return Show(st.name);
    case K::Reset:
      if (st.name == "discard") {
        if (block_) Fail(state::kActiveTxn, "DISCARD ALL cannot run inside a transaction block");
        q.tag = "DISCARD ALL";
      } else {
        q.tag = "RESET";
      }
      vars_.clear();
      return q;
    case K::CreateTable:
      if (block_) Fail(state::kActiveTxn, "CREATE TABLE cannot run inside a transaction block");
      e_.CreateTable(st.schema, st.flag);
      q.tag = "CREATE TABLE";
      return q;
    case K::DropTable:
      if (block_) Fail(state::kActiveTxn, "DROP TABLE cannot run inside a transaction block");
      if (!e_.DropTable(st.name, st.flag))
        q.notices.push_back("table \"" + st.name + "\" does not exist, skipping");
      q.tag = "DROP TABLE";
      return q;
    case K::Vacuum:
      if (block_) Fail(state::kActiveTxn, "VACUUM cannot run inside a transaction block");
      e_.Vacuum();
      q.tag = "VACUUM";
      return q;
    case K::Analyze:
      q.tag = "ANALYZE";
      return q;
    case K::Checkpoint:
      e_.Checkpoint();
      q.tag = "CHECKPOINT";
      return q;
    case K::Select:
    case K::Insert:
    case K::Update:
    case K::Delete:
    case K::Explain:
      return Run(st, params);
  }
  return q;
}

QueryResult Session::Run(Statement& st, const std::vector<Value>& params) {
  bool autocommit = !block_;
  Txn* t = Current();
  QueryResult q;
  try {
    const char* verb = WriteVerb(st.kind);
    if (verb && (block_ ? block_ro_ : default_ro_))
      Fail(state::kReadOnly, std::string("cannot execute ") + verb + " in a read-only transaction");
    t->cancel.store(false);
    e_.StartStatement(*t);
    Executor ex(e_, t, params, WallMicros());
    q = ex.Run(st);
  } catch (...) {
    OnError();
    throw;
  }
  if (autocommit) e_.Commit(Take());
  return q;
}

QueryResult Session::Show(const std::string& raw) {
  std::string name = Lower(raw);
  std::string v;
  if (name == "transaction_isolation") {
    v = IsolationName(CurrentIsolation());
  } else if (name == "default_transaction_isolation") {
    v = IsolationName(default_iso_);
  } else if (name == "transaction_read_only") {
    v = (block_ ? block_ro_ : default_ro_) ? "on" : "off";
  } else if (name == "default_transaction_read_only") {
    v = default_ro_ ? "on" : "off";
  } else if (name == "server_version") {
    v = "16.0";
  } else if (name == "server_encoding" || name == "client_encoding") {
    v = "UTF8";
  } else if (name == "standard_conforming_strings" || name == "integer_datetimes") {
    v = "on";
  } else if (name == "timezone") {
    v = "UTC";
  } else if (name == "datestyle") {
    v = "ISO, MDY";
  } else if (vars_.count(name)) {
    v = vars_[name];
  } else {
    Fail("42704", "unrecognized configuration parameter \"" + raw + "\"");
  }
  QueryResult q;
  q.has_rows = true;
  q.columns.push_back({name, ColType::Text});
  q.rows.push_back({Value::Text(v)});
  q.tag = "SHOW";
  return q;
}

void Session::SetVar(const std::string& raw, const std::string& value, QueryResult& q) {
  std::string name = Lower(raw);
  std::string v = Unquote(value);
  bool reset = Lower(v) == "default";
  if (name == "default_transaction_isolation" || name == "transaction_isolation") {
    Isolation iso = Isolation::ReadCommitted;
    if (!reset && !ParseIsolation(v, &iso))
      Fail("22023", "invalid value for parameter \"" + name + "\": \"" + v + "\"");
    if (name == "default_transaction_isolation") {
      default_iso_ = iso;
      return;
    }
    if (!block_) {
      q.notices.push_back("SET TRANSACTION can only be used in transaction blocks");
      return;
    }
    if (txn_ && txn_->snapshot.load() != 0)
      Fail(state::kActiveTxn, "SET TRANSACTION ISOLATION LEVEL must be called before any query");
    block_iso_ = iso;
    return;
  }
  if (name == "default_transaction_read_only" || name == "transaction_read_only") {
    bool on = false;
    if (!reset && !ParseOnOff(v, &on))
      Fail("22023", "parameter \"" + name + "\" requires a Boolean value");
    if (name == "default_transaction_read_only") {
      default_ro_ = on;
    } else if (block_) {
      block_ro_ = on;
    }
    return;
  }
  if (reset) {
    vars_.erase(name);
  } else {
    vars_[name] = v;
  }
}

}
