#include "client.h"

#include "common.h"

namespace rdbms {

namespace {

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

int16_t Get16(const char* p) {
  return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint8_t>(p[0])) << 8 |
                              static_cast<uint8_t>(p[1]));
}

void ParseError(const std::string& body, PgResult* r) {
  size_t i = 0;
  while (i < body.size() && body[i] != '\0') {
    char field = body[i++];
    size_t e = body.find('\0', i);
    if (e == std::string::npos) break;
    std::string v = body.substr(i, e - i);
    if (field == 'C') r->code = v;
    if (field == 'M') r->message = v;
    i = e + 1;
  }
  if (r->code.empty()) r->code = "XX000";
}

}

PgClient::~PgClient() { Close(); }

void PgClient::Close() {
  if (sock_ == kNoSock) return;
  Send('X', "");
  Flush();
  NetClose(sock_);
  sock_ = kNoSock;
}

void PgClient::Send(char type, const std::string& body) {
  out_.push_back(type);
  Be32(out_, static_cast<int32_t>(body.size() + 4));
  out_ += body;
}

void PgClient::Flush() {
  if (out_.empty() || sock_ == kNoSock) return;
  bool ok = NetSend(sock_, out_.data(), out_.size());
  out_.clear();
  if (!ok) Fail(state::kIo, "connection to server was lost");
}

bool PgClient::Recv(char* type, std::string* body) {
  auto need = [&](size_t n) {
    while (in_.size() - pos_ < n) {
      if (pos_ > 0) {
        in_.erase(0, pos_);
        pos_ = 0;
      }
      char buf[16384];
      long r = NetRecv(sock_, buf, sizeof(buf));
      if (r <= 0) return false;
      in_.append(buf, static_cast<size_t>(r));
    }
    return true;
  };
  if (!need(5)) return false;
  *type = in_[pos_];
  int32_t len = Get32(in_.data() + pos_ + 1);
  if (len < 4) return false;
  if (!need(1 + static_cast<size_t>(len))) return false;
  body->assign(in_, pos_ + 5, static_cast<size_t>(len - 4));
  pos_ += 1 + static_cast<size_t>(len);
  return true;
}

void PgClient::Connect(const std::string& host, int port, const std::string& user,
                       const std::string& db) {
  sock_ = NetConnect(host, port);
  std::string body;
  Be32(body, 196608);
  for (const std::string& s : {std::string("user"), user, std::string("database"), db}) {
    body += s;
    body.push_back('\0');
  }
  body.push_back('\0');
  std::string msg;
  Be32(msg, static_cast<int32_t>(body.size() + 4));
  msg += body;
  if (!NetSend(sock_, msg.data(), msg.size())) Fail(state::kIo, "could not send startup message");
  char type;
  std::string b;
  for (;;) {
    if (!Recv(&type, &b)) Fail(state::kIo, "server closed the connection during startup");
    if (type == 'E') {
      PgResult r;
      ParseError(b, &r);
      Close();
      Fail(r.code.c_str(), r.message);
    }
    if (type == 'R' && Get32(b.data()) != 0)
      Fail(state::kProtocol, "unsupported authentication request");
    if (type == 'K') {
      pid_ = Get32(b.data());
      secret_ = Get32(b.data() + 4);
    }
    if (type == 'Z') {
      status_ = b[0];
      return;
    }
  }
}

std::vector<PgResult> PgClient::Collect() {
  std::vector<PgResult> out;
  PgResult cur;
  char type;
  std::string b;
  for (;;) {
    if (!Recv(&type, &b)) Fail(state::kIo, "connection to server was lost");
    switch (type) {
      case 'T': {
        cur.columns.clear();
        int16_t n = Get16(b.data());
        size_t p = 2;
        for (int16_t i = 0; i < n; i++) {
          size_t e = b.find('\0', p);
          cur.columns.push_back(b.substr(p, e - p));
          p = e + 1 + 18;
        }
        break;
      }
      case 'D': {
        int16_t n = Get16(b.data());
        size_t p = 2;
        std::vector<std::optional<std::string>> row;
        for (int16_t i = 0; i < n; i++) {
          int32_t len = Get32(b.data() + p);
          p += 4;
          if (len < 0) {
            row.emplace_back();
          } else {
            row.emplace_back(b.substr(p, static_cast<size_t>(len)));
            p += static_cast<size_t>(len);
          }
        }
        cur.rows.push_back(std::move(row));
        break;
      }
      case 'C':
        cur.tag = b.substr(0, b.find('\0'));
        out.push_back(std::move(cur));
        cur = PgResult();
        break;
      case 'I':
        out.push_back(std::move(cur));
        cur = PgResult();
        break;
      case 'E':
        ParseError(b, &cur);
        out.push_back(std::move(cur));
        cur = PgResult();
        break;
      case 'Z':
        status_ = b[0];
        return out;
      default:
        break;
    }
  }
}

std::vector<PgResult> PgClient::Query(const std::string& sql) {
  std::string body = sql;
  body.push_back('\0');
  Send('Q', body);
  Flush();
  return Collect();
}

PgResult PgClient::Exec(const std::string& sql) {
  std::vector<PgResult> rs = Query(sql);
  for (auto& r : rs) {
    if (!r.ok()) return r;
  }
  return rs.empty() ? PgResult() : rs.back();
}

PgResult PgClient::Prepared(const std::string& sql,
                            const std::vector<std::optional<std::string>>& params) {
  std::string p;
  p.push_back('\0');
  p += sql;
  p.push_back('\0');
  Be16(p, 0);
  Send('P', p);
  std::string b;
  b.push_back('\0');
  b.push_back('\0');
  Be16(b, 0);
  Be16(b, static_cast<int16_t>(params.size()));
  for (auto& v : params) {
    if (!v) {
      Be32(b, -1);
    } else {
      Be32(b, static_cast<int32_t>(v->size()));
      b += *v;
    }
  }
  Be16(b, 0);
  Send('B', b);
  Send('D', std::string("P") + std::string(1, '\0'));
  std::string e;
  e.push_back('\0');
  Be32(e, 0);
  Send('E', e);
  Send('S', "");
  Flush();
  std::vector<PgResult> rs = Collect();
  for (auto& r : rs) {
    if (!r.ok()) return r;
  }
  return rs.empty() ? PgResult() : rs.back();
}

void PgCancel(const std::string& host, int port, int pid, int32_t secret) {
  Sock s = NetConnect(host, port);
  std::string m;
  Be32(m, 16);
  Be32(m, 80877102);
  Be32(m, pid);
  Be32(m, secret);
  NetSend(s, m.data(), m.size());
  NetClose(s);
}

}
