#pragma once

#include <string>
#include <string_view>

namespace rdbms {

struct Config {
  std::string host = "127.0.0.1";
  int port = 5433;
  int max_connections = 64;
  std::string data_dir = "data";
  int buffer_pool_mb = 64;
  int wal_segment_mb = 16;
  bool fsync = true;
  int group_commit_delay_us = 0;
  int checkpoint_interval_s = 30;
  int checkpoint_wal_mb = 64;
  std::string default_isolation = "read committed";
  int lock_timeout_ms = 10000;
  int gc_interval_ms = 20;
};

Config ParseConfig(std::string_view text);
Config LoadConfig(const std::string& path);

}
