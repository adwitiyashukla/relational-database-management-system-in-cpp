#pragma once

#include <functional>
#include <optional>

#include "buffer.h"
#include "wal.h"

namespace rdbms {

struct Store {
  BufferPool& pool;
  LogManager& log;
};

struct LeafChange {
  bool remove = false;
  Xid xid = 0;
  uint8_t flags = 0;
  std::string value;
};

void ApplyLeaf(Page p, std::string_view key, const std::string* rec);

class BTree {
 public:
  using Decide = std::function<std::optional<LeafChange>(const LeafRec* cur)>;
  using Logger = std::function<void(PageRef& leaf, const LeafRec* before, const LeafChange& ch)>;
  using Visit = std::function<bool(const LeafRec& rec)>;

  struct Shape {
    size_t leaves = 0;
    size_t internals = 0;
    size_t records = 0;
    int height = 0;
  };

  BTree(Store& store, uint32_t id, PageId root) : s_(store), id_(id), root_(root) {}

  static PageId Create(Store& store);
  uint32_t id() const { return id_; }
  PageId root() const { return root_; }

  bool Get(std::string_view key, const std::function<void(const LeafRec*)>& fn);
  void Scan(std::string_view lo, const std::string* hi, bool hi_inclusive, const Visit& fn);
  bool Modify(std::string_view key, const Decide& decide, const Logger& log);
  std::string LastKey();
  Shape Validate();

 private:
  PageRef DescendRead(std::string_view key);
  PageRef DescendWrite(std::string_view key);
  void Split(std::string_view key, size_t need);
  void LogImages(const std::vector<PageRef*>& pages);

  Store& s_;
  uint32_t id_;
  PageId root_;
};

}
