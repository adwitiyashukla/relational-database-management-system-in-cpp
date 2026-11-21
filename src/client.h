#pragma once

#include <optional>
#include <string>
#include <vector>

#include "net.h"

namespace rdbms {

struct PgResult {
  std::string tag;
  std::vector<std::string> columns;
  std::vector<std::vector<std::optional<std::string>>> rows;
  std::string code;
  std::string message;
  bool ok() const { return code.empty(); }
};

class PgClient {
 public:
  PgClient() = default;
  ~PgClient();
  PgClient(const PgClient&) = delete;
  PgClient& operator=(const PgClient&) = delete;

  void Connect(const std::string& host, int port, const std::string& user = "rdbms",
               const std::string& db = "rdbms");
  std::vector<PgResult> Query(const std::string& sql);
  PgResult Exec(const std::string& sql);
  PgResult Prepared(const std::string& sql, const std::vector<std::optional<std::string>>& params);
  void Close();
  char status() const { return status_; }
  int pid() const { return pid_; }
  int32_t secret() const { return secret_; }

 private:
  void Send(char type, const std::string& body);
  void Flush();
  bool Recv(char* type, std::string* body);
  std::vector<PgResult> Collect();

  Sock sock_ = kNoSock;
  std::string in_;
  size_t pos_ = 0;
  std::string out_;
  char status_ = 'I';
  int pid_ = 0;
  int32_t secret_ = 0;
};

void PgCancel(const std::string& host, int port, int pid, int32_t secret);

}
