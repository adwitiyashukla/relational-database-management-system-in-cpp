#include "net.h"

#include <mutex>

#include "common.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace rdbms {

namespace {

#ifdef _WIN32
using RawSock = SOCKET;
const RawSock kBad = INVALID_SOCKET;
int LastError() { return WSAGetLastError(); }
#else
using RawSock = int;
const RawSock kBad = -1;
int LastError() { return errno; }
#endif

RawSock Raw(Sock s) { return static_cast<RawSock>(s); }

sockaddr_in Resolve(const std::string& host, int port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (host.empty() || host == "*" || host == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    return addr;
  }
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) return addr;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
    Fail(state::kIo, "could not resolve host " + host);
  addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return addr;
}

void NoDelay(RawSock s) {
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef __APPLE__
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

}

void NetInit() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

Sock NetListen(const std::string& host, int port, int* bound_port) {
  NetInit();
  RawSock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBad) Fail(state::kIo, "socket failed: " + std::to_string(LastError()));
#ifndef _WIN32
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  sockaddr_in addr = Resolve(host, port);
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    int err = LastError();
    NetClose(static_cast<Sock>(s));
    Fail(state::kIo, "could not bind " + host + ":" + std::to_string(port) + " (error " +
                         std::to_string(err) + ")");
  }
  if (listen(s, 128) != 0) {
    NetClose(static_cast<Sock>(s));
    Fail(state::kIo, "listen failed");
  }
  sockaddr_in got{};
#ifdef _WIN32
  int len = sizeof(got);
#else
  socklen_t len = sizeof(got);
#endif
  getsockname(s, reinterpret_cast<sockaddr*>(&got), &len);
  if (bound_port) *bound_port = ntohs(got.sin_port);
  return static_cast<Sock>(s);
}

Sock NetAccept(Sock listener) {
  RawSock c = accept(Raw(listener), nullptr, nullptr);
  if (c == kBad) return kNoSock;
  NoDelay(c);
  return static_cast<Sock>(c);
}

Sock NetConnect(const std::string& host, int port) {
  NetInit();
  RawSock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBad) Fail(state::kIo, "socket failed");
  sockaddr_in addr = Resolve(host, port);
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    NetClose(static_cast<Sock>(s));
    Fail(state::kIo, "could not connect to " + host + ":" + std::to_string(port));
  }
  NoDelay(s);
  return static_cast<Sock>(s);
}

bool NetSend(Sock s, const char* data, size_t n) {
  size_t done = 0;
  while (done < n) {
#ifdef _WIN32
    int r = send(Raw(s), data + done, static_cast<int>(n - done), 0);
#elif defined(MSG_NOSIGNAL)
    long r = static_cast<long>(send(Raw(s), data + done, n - done, MSG_NOSIGNAL));
#else
    long r = static_cast<long>(send(Raw(s), data + done, n - done, 0));
#endif
    if (r <= 0) {
#ifndef _WIN32
      if (r < 0 && errno == EINTR) continue;
#endif
      return false;
    }
    done += static_cast<size_t>(r);
  }
  return true;
}

long NetRecv(Sock s, char* buf, size_t n) {
  for (;;) {
#ifdef _WIN32
    int r = recv(Raw(s), buf, static_cast<int>(n), 0);
#else
    long r = static_cast<long>(recv(Raw(s), buf, n, 0));
    if (r < 0 && errno == EINTR) continue;
#endif
    return r;
  }
}

void NetShutdown(Sock s) {
  if (s == kNoSock) return;
#ifdef _WIN32
  shutdown(Raw(s), SD_BOTH);
#else
  shutdown(Raw(s), SHUT_RDWR);
#endif
}

void NetClose(Sock s) {
  if (s == kNoSock) return;
#ifdef _WIN32
  closesocket(Raw(s));
#else
  close(Raw(s));
#endif
}

}
