#include "server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rdbms {

namespace {

constexpr int32_t kProtocol3 = 196608;
constexpr int32_t kSslRequest = 80877103;
constexpr int32_t kGssRequest = 80877104;
constexpr int32_t kCancelRequest = 80877102;

void Be16(std::string& s, int16_t v) {
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
  s.push_back(static_cast<char>(v & 0xFF));
}

void Be32(std::string& s, int32_t v) {
  for (int i = 3; i >= 0; i--) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

int32_t Get32(const char* p) {
  return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24 |
                              static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16 |
                              static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8 |
                              static_cast<uint32_t>(static_cast<uint8_t>(p[3])));
}

class Body {
 public:
  explicit Body(const std::string& s) : s_(s) {}
  std::string Str() {
    size_t e = s_.find('\0', p_);
    if (e == std::string::npos) Fail(state::kProtocol, "invalid message format");
    std::string r = s_.substr(p_, e - p_);
    p_ = e + 1;
    return r;
  }
  char Byte() {
    Need(1);
    return s_[p_++];
  }
  int16_t I16() {
    Need(2);
    int16_t v = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint8_t>(s_[p_])) << 8 |
                                     static_cast<uint8_t>(s_[p_ + 1]));
    p_ += 2;
    return v;
  }
  int32_t I32() {
    Need(4);
    int32_t v = Get32(s_.data() + p_);
    p_ += 4;
    return v;
  }
  std::string Bytes(size_t n) {
    Need(n);
    std::string r = s_.substr(p_, n);
    p_ += n;
    return r;
  }

 private:
  void Need(size_t n) const {
    if (s_.size() - p_ < n) Fail(state::kProtocol, "invalid message format");
  }
  const std::string& s_;
  size_t p_ = 0;
};

void Message(std::string& out, char type, const std::string& body) {
  out.push_back(type);
  Be32(out, static_cast<int32_t>(body.size() + 4));
  out += body;
}

std::string ErrorBody(const char* severity, const std::string& code, const std::string& msg) {
  std::string b;
  b += 'S';
  b += severity;
  b.push_back('\0');
  b += 'V';
  b += severity;
  b.push_back('\0');
  b += 'C';
  b += code;
  b.push_back('\0');
  b += 'M';
  b += msg;
  b.push_back('\0');
  b.push_back('\0');
  return b;
}

int16_t FormatAt(const std::vector<int16_t>& f, size_t i) {
  if (f.empty()) return 0;
  if (f.size() == 1) return f[0];
  return i < f.size() ? f[i] : 0;
}

std::string FormatText(const Value& v, ColType t) {
  if (t == ColType::Real && v.type == Type::Float && std::isfinite(v.f)) {
    float f = static_cast<float>(v.f);
    char buf[32];
    for (int p = 6; p <= 9; p++) {
      std::snprintf(buf, sizeof(buf), "%.*g", p, static_cast<double>(f));
      if (std::strtof(buf, nullptr) == f) break;
    }
    return buf;
  }
  return ToText(v);
}

std::string RowDescription(const std::vector<ResultColumn>& cols,
                           const std::vector<int16_t>& fmts) {
  std::string b;
  Be16(b, static_cast<int16_t>(cols.size()));
  for (size_t i = 0; i < cols.size(); i++) {
    b += cols[i].name;
    b.push_back('\0');
    Be32(b, 0);
    Be16(b, 0);
    Be32(b, static_cast<int32_t>(TypeOid(cols[i].type)));
    Be16(b, TypeSize(cols[i].type));
    Be32(b, -1);
    Be16(b, FormatAt(fmts, i));
  }
  return b;
}

std::string DataRow(const std::vector<Value>& row, const std::vector<ResultColumn>& cols,
                    const std::vector<int16_t>& fmts) {
  std::string b;
  Be16(b, static_cast<int16_t>(row.size()));
  for (size_t i = 0; i < row.size(); i++) {
    const Value& v = row[i];
    if (v.null()) {
      Be32(b, -1);
      continue;
    }
    ColType t = i < cols.size() ? cols[i].type : ColType::Text;
    std::string s =
        FormatAt(fmts, i) == 1 ? ToBinary(CastTo(v, StorageType(t)), t) : FormatText(v, t);
    Be32(b, static_cast<int32_t>(s.size()));
    b += s;
  }
  return b;
}

Value ParamValue(const std::string& raw, int16_t format, uint32_t oid) {
  if (format == 1) return FromBinary(raw, oid);
  Value v = Value::Text(raw);
  switch (oid) {
    case 16:
      return CastTo(v, Type::Bool);
    case 20:
    case 21:
    case 23:
      return CastTo(v, Type::Int);
    case 700:
    case 701:
    case 1700:
      return CastTo(v, Type::Float);
    case 1114:
    case 1184:
      return CastTo(v, Type::Timestamp);
    default:
      return v;
  }
}

bool ReturnsRows(const Statement& st) {
  return st.kind == Statement::Kind::Select || st.kind == Statement::Kind::Explain ||
         st.kind == Statement::Kind::Show;
}

}

struct Server::Conn {
  struct Prepared {
    std::shared_ptr<Statement> st;
    std::vector<uint32_t> types;
  };
  struct Portal {
    std::shared_ptr<Statement> st;
    std::vector<Value> params;
    std::vector<int16_t> formats;
    std::optional<QueryResult> result;
    size_t next = 0;
  };

  Sock sock = kNoSock;
  int pid = 0;
  int32_t secret = 0;
  std::unique_ptr<Session> session;
  std::string in;
  size_t pos = 0;
  std::string out;
  std::map<std::string, Prepared> prepared;
  std::map<std::string, Portal> portals;
  bool skip = false;

  bool Read(size_t n, std::string* dst) {
    while (in.size() - pos < n) {
      if (pos > 0) {
        in.erase(0, pos);
        pos = 0;
      }
      char buf[16384];
      long r = NetRecv(sock, buf, sizeof(buf));
      if (r <= 0) return false;
      in.append(buf, static_cast<size_t>(r));
    }
    dst->assign(in, pos, n);
    pos += n;
    return true;
  }

  bool Flush() {
    if (out.empty()) return true;
    bool ok = NetSend(sock, out.data(), out.size());
    out.clear();
    return ok;
  }

  void Error(const std::string& code, const std::string& msg) {
    Message(out, 'E', ErrorBody("ERROR", code, msg));
  }

  void Notice(const std::string& msg) { Message(out, 'N', ErrorBody("WARNING", "01000", msg)); }

  void Ready() { Message(out, 'Z', std::string(1, session->Status())); }
};

Server::Server(Engine& e, const Config& cfg) : e_(e), cfg_(cfg) {
  ParseIsolation(cfg_.default_isolation, &default_iso_);
}

Server::~Server() { Stop(); }

void Server::Start() {
  listener_ = NetListen(cfg_.host, cfg_.port, &port_);
  accept_ = std::thread(&Server::AcceptLoop, this);
}

void Server::Stop() {
  if (stopping_.exchange(true)) return;
  NetShutdown(listener_);
  NetClose(listener_);
  if (accept_.joinable()) accept_.join();
  std::unique_lock<std::mutex> l(mu_);
  for (auto& [pid, c] : conns_) NetShutdown(c->sock);
  cv_.wait_for(l, std::chrono::seconds(30), [&] { return active_ == 0; });
}

int Server::connections() {
  std::lock_guard<std::mutex> g(mu_);
  return active_;
}

void Server::AcceptLoop() {
  while (!stopping_.load()) {
    Sock s = NetAccept(listener_);
    if (s == kNoSock) {
      if (stopping_.load()) break;
      SleepMicros(2000);
      continue;
    }
    {
      std::lock_guard<std::mutex> g(mu_);
      active_++;
    }
    std::thread([this, s] {
      try {
        Serve(s);
      } catch (const std::exception& e) {
        LogInfo("connection error: %s", e.what());
      }
      NetClose(s);
      std::lock_guard<std::mutex> g(mu_);
      active_--;
      cv_.notify_all();
    }).detach();
  }
}

void Server::Serve(Sock s) {
  Conn c;
  c.sock = s;
  std::string head, body;
  for (;;) {
    if (!c.Read(4, &head)) return;
    int32_t len = Get32(head.data());
    if (len < 8 || len > 10000) return;
    if (!c.Read(static_cast<size_t>(len - 4), &body)) return;
    int32_t code = Get32(body.data());
    if (code == kSslRequest || code == kGssRequest) {
      if (!NetSend(s, "N", 1)) return;
      continue;
    }
    if (code == kCancelRequest) {
      if (body.size() >= 12) {
        int pid = Get32(body.data() + 4);
        int32_t secret = Get32(body.data() + 8);
        std::lock_guard<std::mutex> g(mu_);
        auto it = conns_.find(pid);
        if (it != conns_.end() && it->second->secret == secret) it->second->session->Cancel();
      }
      return;
    }
    if (code != kProtocol3) {
      Message(c.out, 'E', ErrorBody("FATAL", state::kProtocol, "unsupported frontend protocol"));
      c.Flush();
      return;
    }
    break;
  }
  std::map<std::string, std::string> params;
  {
    Body b(body);
    b.I32();
    for (;;) {
      std::string k = b.Str();
      if (k.empty()) break;
      params[k] = b.Str();
    }
  }
  c.pid = next_pid_.fetch_add(1);
  c.secret =
      static_cast<int32_t>(Rng(NowMicros() ^ static_cast<uint64_t>(c.pid)).Next() & 0x7FFFFFFF);
  c.session = std::make_unique<Session>(e_, default_iso_);
  {
    std::lock_guard<std::mutex> g(mu_);
    if (stopping_.load()) return;
    if (active_ > cfg_.max_connections) {
      Message(c.out, 'E', ErrorBody("FATAL", state::kTooMany, "sorry, too many clients already"));
      c.Flush();
      return;
    }
    conns_[c.pid] = &c;
  }
  std::string auth;
  Be32(auth, 0);
  Message(c.out, 'R', auth);
  std::string user = params.count("user") ? params["user"] : "postgres";
  std::vector<std::pair<std::string, std::string>> status = {
      {"server_version", "16.0"},
      {"server_encoding", "UTF8"},
      {"client_encoding", "UTF8"},
      {"DateStyle", "ISO, MDY"},
      {"TimeZone", "UTC"},
      {"IntervalStyle", "postgres"},
      {"integer_datetimes", "on"},
      {"standard_conforming_strings", "on"},
      {"is_superuser", "on"},
      {"session_authorization", user},
      {"application_name", params.count("application_name") ? params["application_name"] : ""}};
  for (auto& [k, v] : status) {
    std::string b = k;
    b.push_back('\0');
    b += v;
    b.push_back('\0');
    Message(c.out, 'S', b);
  }
  std::string key;
  Be32(key, c.pid);
  Be32(key, c.secret);
  Message(c.out, 'K', key);
  c.Ready();
  if (c.Flush()) Handle(c);
  {
    std::lock_guard<std::mutex> g(mu_);
    conns_.erase(c.pid);
  }
  c.session->Close();
}

void Server::Handle(Conn& c) {
  std::string head, body;
  for (;;) {
    if (!c.Read(5, &head)) return;
    char type = head[0];
    int32_t len = Get32(head.data() + 1);
    if (len < 4 || len > (1 << 30)) return;
    if (!c.Read(static_cast<size_t>(len - 4), &body)) return;
    if (type == 'X') return;
    if (c.skip && type != 'S') continue;
    try {
      switch (type) {
        case 'Q': {
          std::string sql = body.substr(0, body.find('\0'));
          std::vector<Statement> stmts;
          try {
            stmts = ParseSql(sql);
          } catch (const DbError& e) {
            c.session->OnError();
            c.Error(e.code(), e.what());
            c.Ready();
            break;
          }
          if (stmts.empty()) Message(c.out, 'I', "");
          for (auto& st : stmts) {
            try {
              if (st.params > 0) Fail(state::kNoParam, "there is no parameter $1");
              QueryResult q = c.session->Execute(st, {});
              for (auto& n : q.notices) c.Notice(n);
              if (q.has_rows) {
                Message(c.out, 'T', RowDescription(q.columns, {}));
                for (auto& row : q.rows) {
                  Message(c.out, 'D', DataRow(row, q.columns, {}));
                  if (c.out.size() > (1u << 20) && !c.Flush()) return;
                }
              }
              Message(c.out, 'C', q.tag + std::string(1, '\0'));
            } catch (const DbError& e) {
              c.session->OnError();
              c.Error(e.code(), e.what());
              break;
            } catch (const std::exception& e) {
              c.session->OnError();
              c.Error(state::kInternal, e.what());
              break;
            }
          }
          c.Ready();
          break;
        }
        case 'P': {
          Body b(body);
          std::string name = b.Str();
          std::string sql = b.Str();
          int16_t n = b.I16();
          std::vector<uint32_t> types;
          for (int16_t i = 0; i < n; i++) types.push_back(static_cast<uint32_t>(b.I32()));
          std::vector<Statement> stmts = ParseSql(sql);
          if (stmts.size() > 1)
            Fail(state::kSyntax, "cannot insert multiple commands into a prepared statement");
          auto st = std::make_shared<Statement>();
          if (!stmts.empty()) *st = std::move(stmts[0]);
          if (types.size() < static_cast<size_t>(st->params))
            types.resize(static_cast<size_t>(st->params), 0);
          if (!name.empty() && c.prepared.count(name))
            Fail("42P05", "prepared statement \"" + name + "\" already exists");
          c.prepared[name] = {st, types};
          Message(c.out, '1', "");
          break;
        }
        case 'B': {
          Body b(body);
          std::string portal = b.Str();
          std::string name = b.Str();
          int16_t nf = b.I16();
          std::vector<int16_t> fmts;
          for (int16_t i = 0; i < nf; i++) fmts.push_back(b.I16());
          int16_t np = b.I16();
          std::vector<std::optional<std::string>> raw;
          for (int16_t i = 0; i < np; i++) {
            int32_t l = b.I32();
            if (l < 0) {
              raw.emplace_back();
            } else {
              raw.emplace_back(b.Bytes(static_cast<size_t>(l)));
            }
          }
          int16_t nr = b.I16();
          std::vector<int16_t> rfmts;
          for (int16_t i = 0; i < nr; i++) rfmts.push_back(b.I16());
          auto it = c.prepared.find(name);
          if (it == c.prepared.end())
            Fail("26000", "prepared statement \"" + name + "\" does not exist");
          auto& pre = it->second;
          if (np != pre.st->params)
            Fail(state::kProtocol, "bind message supplies " + std::to_string(np) +
                                       " parameters, but prepared statement \"" + name +
                                       "\" requires " + std::to_string(pre.st->params));
          Conn::Portal p;
          p.st = pre.st;
          p.formats = rfmts;
          for (size_t i = 0; i < raw.size(); i++) {
            p.params.push_back(raw[i] ? ParamValue(*raw[i], FormatAt(fmts, i), pre.types[i])
                                      : Value::Null());
          }
          c.portals[portal] = std::move(p);
          Message(c.out, '2', "");
          break;
        }
        case 'D': {
          Body b(body);
          char kind = b.Byte();
          std::string name = b.Str();
          if (kind == 'S') {
            auto it = c.prepared.find(name);
            if (it == c.prepared.end())
              Fail("26000", "prepared statement \"" + name + "\" does not exist");
            std::string pd;
            Be16(pd, static_cast<int16_t>(it->second.types.size()));
            for (uint32_t t : it->second.types) Be32(pd, static_cast<int32_t>(t == 0 ? 25 : t));
            Message(c.out, 't', pd);
            if (ReturnsRows(*it->second.st)) {
              Message(c.out, 'T', RowDescription(c.session->Describe(*it->second.st), {}));
            } else {
              Message(c.out, 'n', "");
            }
          } else {
            auto it = c.portals.find(name);
            if (it == c.portals.end()) Fail("34000", "portal \"" + name + "\" does not exist");
            if (ReturnsRows(*it->second.st)) {
              Message(c.out, 'T',
                      RowDescription(c.session->Describe(*it->second.st), it->second.formats));
            } else {
              Message(c.out, 'n', "");
            }
          }
          break;
        }
        case 'E': {
          Body b(body);
          std::string name = b.Str();
          int32_t max = b.I32();
          auto it = c.portals.find(name);
          if (it == c.portals.end()) Fail("34000", "portal \"" + name + "\" does not exist");
          Conn::Portal& p = it->second;
          if (p.st->kind == Statement::Kind::Empty) {
            Message(c.out, 'I', "");
            break;
          }
          if (!p.result) {
            p.result = c.session->Execute(*p.st, p.params);
            p.next = 0;
            for (auto& n : p.result->notices) c.Notice(n);
          }
          QueryResult& q = *p.result;
          if (q.has_rows) {
            size_t end = q.rows.size();
            if (max > 0) end = std::min(end, p.next + static_cast<size_t>(max));
            for (; p.next < end; p.next++) {
              Message(c.out, 'D', DataRow(q.rows[p.next], q.columns, p.formats));
              if (c.out.size() > (1u << 20) && !c.Flush()) return;
            }
            if (p.next < q.rows.size()) {
              Message(c.out, 's', "");
              break;
            }
          }
          Message(c.out, 'C', q.tag + std::string(1, '\0'));
          break;
        }
        case 'S':
          c.skip = false;
          c.Ready();
          if (!c.Flush()) return;
          break;
        case 'H':
          if (!c.Flush()) return;
          break;
        case 'C': {
          Body b(body);
          char kind = b.Byte();
          std::string name = b.Str();
          if (kind == 'S') {
            c.prepared.erase(name);
          } else {
            c.portals.erase(name);
          }
          Message(c.out, '3', "");
          break;
        }
        case 'p':
          break;
        default:
          Fail(state::kProtocol, std::string("unsupported message type '") + type + "'");
      }
    } catch (const DbError& e) {
      c.session->OnError();
      c.Error(e.code(), e.what());
      if (type == 'Q') {
        c.Ready();
      } else {
        c.skip = true;
      }
    } catch (const std::exception& e) {
      c.session->OnError();
      c.Error(state::kInternal, e.what());
      if (type == 'Q') {
        c.Ready();
      } else {
        c.skip = true;
      }
    }
    if (type == 'Q' && !c.Flush()) return;
    if (c.out.size() > 65536 && !c.Flush()) return;
  }
}

}
