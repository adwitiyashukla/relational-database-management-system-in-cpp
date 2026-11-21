#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "config.h"
#include "engine.h"
#include "net.h"
#include "session.h"

namespace rdbms {

class Server {
 public:
  Server(Engine& e, const Config& cfg);
  ~Server();
  void Start();
  void Stop();
  int port() const { return port_; }
  int connections();

  struct Conn;

 private:
  void AcceptLoop();
  void Serve(Sock s);
  void Handle(Conn& c);

  Engine& e_;
  Config cfg_;
  Isolation default_iso_ = Isolation::ReadCommitted;
  Sock listener_ = kNoSock;
  int port_ = 0;
  std::thread accept_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::map<int, Conn*> conns_;
  int active_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<int> next_pid_{1000};
};

}
