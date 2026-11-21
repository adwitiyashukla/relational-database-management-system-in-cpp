#include "page.h"

namespace rdbms {

std::string MakeLeaf(std::string_view key, Xid xid, uint8_t flags, std::string_view value) {
  std::string r;
  r.resize(2 + key.size() + 9 + value.size());
  PutU16(r.data(), static_cast<uint16_t>(key.size()));
  std::memcpy(r.data() + 2, key.data(), key.size());
  PutU64(r.data() + 2 + key.size(), xid);
  r[2 + key.size() + 8] = static_cast<char>(flags);
  if (!value.empty()) std::memcpy(r.data() + 2 + key.size() + 9, value.data(), value.size());
  return r;
}

LeafRec ParseLeaf(std::string_view rec) {
  LeafRec r;
  uint16_t k = GetU16(rec.data());
  r.key = rec.substr(2, k);
  r.xid = GetU64(rec.data() + 2 + k);
  r.flags = static_cast<uint8_t>(rec[2 + k + 8]);
  r.value = rec.substr(2 + k + 9);
  return r;
}

std::string MakeBranch(std::string_view key, PageId child) {
  std::string r;
  r.resize(2 + key.size() + 4);
  PutU16(r.data(), static_cast<uint16_t>(key.size()));
  std::memcpy(r.data() + 2, key.data(), key.size());
  PutU32(r.data() + 2 + key.size(), child);
  return r;
}

PageId BranchChild(std::string_view rec) { return GetU32(rec.data() + 2 + GetU16(rec.data())); }

void Page::Format(PageId id, PageType type, uint8_t level) {
  std::memset(d_, 0, kPageSize);
  PutU32(d_ + 4, id);
  d_[16] = static_cast<char>(type);
  d_[17] = static_cast<char>(level);
  set_free_start(static_cast<uint16_t>(kHeaderSize));
  set_free_end(static_cast<uint16_t>(kPageSize));
  set_next(kNoPage);
  set_leftmost(kNoPage);
}

void Page::FormatMeta() {
  Format(kMetaPage, PageType::Meta, 0);
  PutU64(d_ + 40, kMetaMagic);
  PutU32(d_ + 48, 1);
  set_meta_next_page(2);
  set_meta_next_tree(2);
  set_free_start(64);
}

std::string_view Page::Rec(uint16_t i) const {
  return std::string_view(d_ + GetU16(slot(i)), GetU16(slot(i) + 2));
}

std::string_view Page::KeyAt(uint16_t i) const {
  const char* r = d_ + GetU16(slot(i));
  return std::string_view(r + 2, GetU16(r));
}

size_t Page::LiveBytes() const { return kPageSize - free_end() - frag(); }

size_t Page::FreeSpace() const {
  return kPageSize - kHeaderSize - count() * kSlotSize - LiveBytes();
}

void Page::Insert(uint16_t i, std::string_view rec) {
  if (static_cast<size_t>(free_end() - free_start()) < rec.size() + kSlotSize) Compact();
  uint16_t off = static_cast<uint16_t>(free_end() - rec.size());
  std::memcpy(d_ + off, rec.data(), rec.size());
  set_free_end(off);
  uint16_t n = count();
  std::memmove(slot(static_cast<uint16_t>(i + 1)), slot(i), (n - i) * kSlotSize);
  PutU16(slot(i), off);
  PutU16(slot(i) + 2, static_cast<uint16_t>(rec.size()));
  set_count(static_cast<uint16_t>(n + 1));
  set_free_start(static_cast<uint16_t>(free_start() + kSlotSize));
}

void Page::Replace(uint16_t i, std::string_view rec) {
  uint16_t old_off = GetU16(slot(i));
  uint16_t old_len = GetU16(slot(i) + 2);
  if (rec.size() <= old_len) {
    std::memcpy(d_ + old_off, rec.data(), rec.size());
    PutU16(slot(i) + 2, static_cast<uint16_t>(rec.size()));
    set_frag(static_cast<uint16_t>(frag() + old_len - rec.size()));
    return;
  }
  set_frag(static_cast<uint16_t>(frag() + old_len));
  PutU16(slot(i) + 2, 0);
  if (static_cast<size_t>(free_end() - free_start()) < rec.size()) Compact();
  uint16_t off = static_cast<uint16_t>(free_end() - rec.size());
  std::memcpy(d_ + off, rec.data(), rec.size());
  set_free_end(off);
  PutU16(slot(i), off);
  PutU16(slot(i) + 2, static_cast<uint16_t>(rec.size()));
}

void Page::Erase(uint16_t i) {
  uint16_t len = GetU16(slot(i) + 2);
  set_frag(static_cast<uint16_t>(frag() + len));
  uint16_t n = count();
  std::memmove(slot(i), slot(static_cast<uint16_t>(i + 1)), (n - i - 1) * kSlotSize);
  set_count(static_cast<uint16_t>(n - 1));
  set_free_start(static_cast<uint16_t>(free_start() - kSlotSize));
}

void Page::Compact() {
  char tmp[kPageSize];
  size_t end = kPageSize;
  uint16_t n = count();
  for (uint16_t i = 0; i < n; i++) {
    uint16_t len = GetU16(slot(i) + 2);
    end -= len;
    if (len > 0) std::memcpy(tmp + end, d_ + GetU16(slot(i)), len);
    PutU16(slot(i), static_cast<uint16_t>(end));
  }
  std::memcpy(d_ + end, tmp + end, kPageSize - end);
  set_free_end(static_cast<uint16_t>(end));
  set_frag(0);
}

uint16_t Page::LowerBound(std::string_view key, bool* exact) const {
  uint16_t lo = 0, hi = count();
  while (lo < hi) {
    uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
    if (CompareKeys(KeyAt(mid), key) < 0) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hi = mid;
    }
  }
  if (exact) *exact = lo < count() && CompareKeys(KeyAt(lo), key) == 0;
  return lo;
}

uint16_t Page::UpperBound(std::string_view key) const {
  uint16_t lo = 0, hi = count();
  while (lo < hi) {
    uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
    if (CompareKeys(KeyAt(mid), key) <= 0) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hi = mid;
    }
  }
  return lo;
}

PageId Page::ChildFor(std::string_view key) const {
  uint16_t ub = UpperBound(key);
  if (ub == 0) return leftmost();
  return BranchChild(Rec(static_cast<uint16_t>(ub - 1)));
}

void Page::Seal() { PutU32(d_, Crc32c(d_ + 4, kPageSize - 4)); }

bool Page::Verify(PageId expect) const {
  return GetU32(d_) == Crc32c(d_ + 4, kPageSize - 4) && id() == expect;
}

bool Page::IsZero() const {
  for (size_t i = 0; i < kPageSize; i += 8) {
    if (GetU64(d_ + i) != 0) return false;
  }
  return true;
}

std::string Page::Image() const {
  uint16_t fs = free_start(), fe = free_end();
  std::string img;
  img.resize(4 + fs + (kPageSize - fe));
  PutU16(img.data(), fs);
  PutU16(img.data() + 2, fe);
  std::memcpy(img.data() + 4, d_, fs);
  std::memcpy(img.data() + 4 + fs, d_ + fe, kPageSize - fe);
  return img;
}

void Page::Restore(char* d, std::string_view image) {
  uint16_t fs = GetU16(image.data());
  uint16_t fe = GetU16(image.data() + 2);
  if (fs > fe || fe > kPageSize || image.size() != 4 + fs + (kPageSize - fe))
    Fail(state::kCorrupt, "bad page image");
  std::memset(d, 0, kPageSize);
  std::memcpy(d, image.data() + 4, fs);
  std::memcpy(d + fe, image.data() + 4 + fs, kPageSize - fe);
}

}
