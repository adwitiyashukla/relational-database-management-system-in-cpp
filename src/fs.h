#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "common.h"

namespace rdbms {

class File {
 public:
  virtual ~File() = default;
  virtual size_t ReadAt(uint64_t off, char* buf, size_t n) = 0;
  virtual void WriteAt(uint64_t off, const char* buf, size_t n) = 0;
  virtual void Sync() = 0;
  virtual uint64_t Size() = 0;
  virtual void Truncate(uint64_t size) = 0;
};

class FileSystem {
 public:
  virtual ~FileSystem() = default;
  virtual std::unique_ptr<File> Open(const std::string& path, bool create) = 0;
  virtual bool Exists(const std::string& path) = 0;
  virtual void Remove(const std::string& path) = 0;
  virtual std::vector<std::string> List(const std::string& dir) = 0;
  virtual void MakeDirs(const std::string& dir) = 0;
  virtual void SyncDir(const std::string& dir) = 0;
};

std::unique_ptr<FileSystem> NewDiskFs();

struct SimCrash : std::exception {
  const char* what() const noexcept override { return "simulated crash"; }
};

class SimFs : public FileSystem {
 public:
  struct Stats {
    uint64_t kept = 0;
    uint64_t dropped = 0;
    uint64_t torn = 0;
    uint64_t lost_files = 0;
  };

  explicit SimFs(uint64_t seed);
  std::unique_ptr<File> Open(const std::string& path, bool create) override;
  bool Exists(const std::string& path) override;
  void Remove(const std::string& path) override;
  std::vector<std::string> List(const std::string& dir) override;
  void MakeDirs(const std::string& dir) override;
  void SyncDir(const std::string& dir) override;

  void ArmCrash(uint64_t at_op);
  void CrashNow();
  void Restart();
  bool crashed();
  uint64_t ops();
  Stats stats();

  struct Node {
    std::string durable;
    std::string current;
    struct Op {
      bool trunc;
      uint64_t off;
      std::string data;
    };
    std::vector<Op> pending;
    bool created_durable = false;
  };

  bool BeginOpLocked();
  std::mutex& mu() { return mu_; }

 private:
  void CrashLocked();

  std::mutex mu_;
  std::map<std::string, std::shared_ptr<Node>> files_;
  std::set<std::string> dirs_;
  Rng rng_;
  uint64_t ops_ = 0;
  uint64_t crash_at_ = 0;
  bool armed_ = false;
  bool crashed_ = false;
  Stats stats_;
};

}
