#pragma once

#include "common.h"

namespace rdbms {

enum class PageType : uint8_t { Free = 0, Meta = 1, Leaf = 2, Internal = 3 };

constexpr size_t kHeaderSize = 40;
constexpr size_t kSlotSize = 4;
constexpr size_t kMaxKeySize = 255;
constexpr size_t kMaxRecordSize = 2000;
constexpr size_t kMaxBranchEntry = 2 + kMaxKeySize + 4 + kSlotSize;
constexpr uint8_t kDeleted = 1;
constexpr uint64_t kMetaMagic = 0x31544D534D424452ull;

struct LeafRec {
  std::string_view key;
  Xid xid = 0;
  uint8_t flags = 0;
  std::string_view value;
  bool deleted() const { return (flags & kDeleted) != 0; }
};

std::string MakeLeaf(std::string_view key, Xid xid, uint8_t flags, std::string_view value);
LeafRec ParseLeaf(std::string_view rec);
std::string MakeBranch(std::string_view key, PageId child);
PageId BranchChild(std::string_view rec);

class Page {
 public:
  explicit Page(char* d) : d_(d) {}
  char* data() const { return d_; }

  PageId id() const { return GetU32(d_ + 4); }
  void set_id(PageId id) { PutU32(d_ + 4, id); }
  Lsn lsn() const { return GetU64(d_ + 8); }
  void set_lsn(Lsn l) { PutU64(d_ + 8, l); }
  PageType type() const { return static_cast<PageType>(static_cast<uint8_t>(d_[16])); }
  bool leaf() const { return type() == PageType::Leaf; }
  uint8_t level() const { return static_cast<uint8_t>(d_[17]); }
  uint16_t count() const { return GetU16(d_ + 18); }
  PageId next() const { return GetU32(d_ + 24); }
  void set_next(PageId p) { PutU32(d_ + 24, p); }
  PageId leftmost() const { return GetU32(d_ + 28); }
  void set_leftmost(PageId p) { PutU32(d_ + 28, p); }

  void Format(PageId id, PageType type, uint8_t level);
  std::string_view Rec(uint16_t i) const;
  std::string_view KeyAt(uint16_t i) const;
  size_t FreeSpace() const;
  size_t LiveBytes() const;
  void Insert(uint16_t i, std::string_view rec);
  void Replace(uint16_t i, std::string_view rec);
  void Erase(uint16_t i);
  void Compact();
  uint16_t LowerBound(std::string_view key, bool* exact) const;
  uint16_t UpperBound(std::string_view key) const;
  PageId ChildFor(std::string_view key) const;

  void Seal();
  bool Verify(PageId expect) const;
  bool IsZero() const;

  std::string Image() const;
  static void Restore(char* d, std::string_view image);

  uint64_t meta_magic() const { return GetU64(d_ + 40); }
  PageId meta_next_page() const { return GetU32(d_ + 52); }
  void set_meta_next_page(PageId p) { PutU32(d_ + 52, p); }
  uint32_t meta_next_tree() const { return GetU32(d_ + 56); }
  void set_meta_next_tree(uint32_t t) { PutU32(d_ + 56, t); }
  void FormatMeta();

 private:
  uint16_t free_start() const { return GetU16(d_ + 20); }
  void set_free_start(uint16_t v) { PutU16(d_ + 20, v); }
  uint16_t free_end() const { return GetU16(d_ + 22); }
  void set_free_end(uint16_t v) { PutU16(d_ + 22, v); }
  uint16_t frag() const { return GetU16(d_ + 32); }
  void set_frag(uint16_t v) { PutU16(d_ + 32, v); }
  void set_count(uint16_t v) { PutU16(d_ + 18, v); }
  char* slot(uint16_t i) const { return d_ + kHeaderSize + i * kSlotSize; }

  char* d_;
};

}
