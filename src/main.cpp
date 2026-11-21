#include <csignal>
#include <cstdio>
#include <string>

#include "config.h"
#include "engine.h"
#include "server.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

volatile std::sig_atomic_t g_stop = 0;

#ifdef _WIN32
BOOL WINAPI OnConsole(DWORD) {
  g_stop = 1;
  return TRUE;
}
#else
void OnSignal(int) { g_stop = 1; }
#endif

void Usage() {
  std::printf(
      "usage: rdbms-server [--config path]\n"
      "  --config path   settings file to load (default: config.toml)\n");
}

}

int main(int argc, char** argv) {
  using namespace rdbms;
  std::string path = "config.toml";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      path = argv[++i];
    } else if (a == "--help" || a == "-h") {
      Usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      Usage();
      return 2;
    }
  }
  try {
    auto fs = NewDiskFs();
    Config cfg;
    if (fs->Exists(path)) {
      cfg = LoadConfig(path);
    } else {
      LogInfo("no %s found, using built-in defaults", path.c_str());
    }
    EngineOptions opt;
    opt.dir = cfg.data_dir;
    opt.pool_pages = static_cast<size_t>(cfg.buffer_pool_mb) * 1024 * 1024 / kPageSize;
    opt.wal.segment_size = static_cast<uint64_t>(cfg.wal_segment_mb) << 20;
    opt.wal.fsync = cfg.fsync;
    opt.wal.group_delay_us = static_cast<uint32_t>(cfg.group_commit_delay_us);
    opt.lock_timeout_ms = static_cast<uint32_t>(cfg.lock_timeout_ms);
    opt.checkpoint_interval_ms = static_cast<uint32_t>(cfg.checkpoint_interval_s) * 1000;
    opt.checkpoint_wal_bytes = static_cast<uint64_t>(cfg.checkpoint_wal_mb) << 20;
    opt.gc_interval_ms = static_cast<uint32_t>(cfg.gc_interval_ms);
    Engine engine(*fs, opt);
    engine.Open();
    const RecoveryStats& rs = engine.recovery();
    if (rs.fresh) {
      LogInfo("created a new database in %s", cfg.data_dir.c_str());
    } else {
      LogInfo(
          "recovery took %.1f ms: %llu log records after the checkpoint, %llu changes redone, "
          "%llu transactions rolled back (%llu changes undone), %d torn pages repaired",
          rs.millis, static_cast<unsigned long long>(rs.records),
          static_cast<unsigned long long>(rs.redone), static_cast<unsigned long long>(rs.losers),
          static_cast<unsigned long long>(rs.undone), rs.torn_pages);
    }
    Server server(engine, cfg);
    server.Start();
    LogInfo("listening on %s:%d (fsync %s, default isolation %s)", cfg.host.c_str(), server.port(),
            cfg.fsync ? "on" : "off", cfg.default_isolation.c_str());
#ifdef _WIN32
    SetConsoleCtrlHandler(OnConsole, TRUE);
#else
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);
#endif
    while (!g_stop) SleepMicros(100000);
    LogInfo("shutting down");
    server.Stop();
    engine.Close();
    LogInfo("shutdown complete");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
