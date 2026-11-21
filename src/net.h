#pragma once

#include <cstdint>
#include <string>

namespace rdbms {

using Sock = intptr_t;
constexpr Sock kNoSock = -1;

void NetInit();
Sock NetListen(const std::string& host, int port, int* bound_port);
Sock NetAccept(Sock listener);
Sock NetConnect(const std::string& host, int port);
bool NetSend(Sock s, const char* data, size_t n);
long NetRecv(Sock s, char* buf, size_t n);
void NetShutdown(Sock s);
void NetClose(Sock s);

}
