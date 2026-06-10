// Shared checks for grilling rtxt against real Joint Operations data.
//
// Two layers, deliberately independent:
//   * check_format_invariants() asserts the on-disk rules directly on raw retail
//     bytes, with no opennova::rtxt code in the loop. It documents the format as
//     witnessed in IDA (TextResource_FixupPointers @ 0x75D050) and guards against
//     a self-consistent-but-wrong reimpl coercing parity green.
//   * check_parity() runs parse -> write -> byte-compare through libs/rtxt.
#pragma once

#include <rtxt/rtxt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rtxt_real {

inline uint32_t read_u32(const std::vector<uint8_t> &d, size_t off) {
  return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
         (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

// Asserts the four invariants every retail RTXT bin obeys (98/98 in the JO
// retail install). Returns false and prints the first violation if any fails.
inline bool check_format_invariants(const std::string &name, const std::vector<uint8_t> &d) {
  auto fail = [&](const char *what) {
    std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), what);
    return false;
  };
  if (d.size() < 16 || read_u32(d, 0) != 0x54585452u) return fail("not an RTXT file");
  uint32_t meta_off = read_u32(d, 4);
  uint32_t meta_size = read_u32(d, 8);
  uint32_t entry_count = read_u32(d, 12);
  size_t text_start = 16 + 16ull * entry_count;
  if (meta_off > d.size() || text_start > meta_off) return fail("header ranges out of bounds");

  // I-ALIGN: section meta starts 4-aligned [orig: alloc/fixup expects dword-aligned rows].
  if (meta_off % 4 != 0) return fail("section meta offset not 4-aligned");

  // I-HDR8: header[+8] is the meta block size EXCLUDING the section_count dword.
  if (meta_size != d.size() - meta_off - 4) return fail("header[+8] != meta bytes minus count dword");

  uint32_t section_count = read_u32(d, meta_off);
  size_t rows_start = meta_off + 4;
  size_t names_start = rows_start + 8ull * section_count;
  if (names_start > d.size()) return fail("section rows out of bounds");

  // I-GROUP: per-section counts sum to entry_count; entries carry nondecreasing
  // section indices and a zero pad word [orig: TextResource_FindEntryBySectionAndKey
  // @ 0x75D250 derives entry indices by accumulating string_counts].
  uint64_t count_sum = 0;
  for (uint32_t i = 0; i < section_count; ++i) count_sum += read_u32(d, rows_start + 8ull * i + 4);
  if (count_sum != entry_count) return fail("sum(section string_count) != entry_count");
  uint32_t prev_section = 0;
  uint64_t text_cursor = 0;
  for (uint32_t i = 0; i < entry_count; ++i) {
    size_t e = 16 + 16ull * i;
    if (read_u32(d, e) != text_cursor) return fail("text offsets not sequential per entry order");
    size_t p = text_start + read_u32(d, e);
    while (p < meta_off && d[p] != 0) ++p;
    text_cursor += (p - (text_start + read_u32(d, e))) + 1;
    uint32_t section = read_u32(d, e + 8);
    if (section < prev_section) return fail("entries not grouped by section");
    prev_section = section;
    if (read_u32(d, e + 12) != 0) return fail("entry pad word not zero");
  }
  // Text blob padding up to the aligned meta_off is zero.
  for (size_t p = text_start + text_cursor; p < meta_off; ++p) {
    if (d[p] != 0) return fail("text blob padding not zero");
  }

  // I-KEYOFF: each section row's first dword is the offset of that section's
  // FIRST KEY string, relative to (meta_off + 4) [orig: TextResource_FixupPointers
  // @ 0x75D050 rebases row[+0] against meta+4; TextResource_FindKeyInSection
  // @ 0x75D1E0 walks keys from the fixed-up pointer].
  size_t pos = names_start;
  for (uint32_t i = 0; i < section_count; ++i) {
    while (pos < d.size() && d[pos] != 0) ++pos;
    ++pos;  // skip NUL
  }
  size_t key_cursor = pos;
  for (uint32_t i = 0; i < section_count; ++i) {
    uint32_t row_off = read_u32(d, rows_start + 8ull * i);
    if (row_off != key_cursor - rows_start) return fail("section row[+0] != first-key offset rel. meta+4");
    uint32_t strings = read_u32(d, rows_start + 8ull * i + 4);
    for (uint32_t k = 0; k < strings; ++k) {
      while (key_cursor < d.size() && d[key_cursor] != 0) ++key_cursor;
      ++key_cursor;
    }
  }
  return true;
}

// parse -> write -> byte-compare. The editor's core promise on real game data.
inline bool check_parity(const std::string &name, const std::vector<uint8_t> &d) {
  opennova::rtxt::File file;
  std::string error;
  if (!opennova::rtxt::parse(d.data(), d.size(), file, error)) {
    std::fprintf(stderr, "FAIL %s: parse error: %s\n", name.c_str(), error.c_str());
    return false;
  }
  std::vector<uint8_t> out;
  if (!opennova::rtxt::write(file, out, error)) {
    std::fprintf(stderr, "FAIL %s: write error: %s\n", name.c_str(), error.c_str());
    return false;
  }
  if (out.size() != d.size()) {
    std::fprintf(stderr, "FAIL %s: size %zu != original %zu\n", name.c_str(), out.size(), d.size());
    return false;
  }
  for (size_t i = 0; i < d.size(); ++i) {
    if (out[i] != d[i]) {
      std::fprintf(stderr, "FAIL %s: byte %zu differs (0x%02X != 0x%02X)\n", name.c_str(), i,
                   out[i], d[i]);
      return false;
    }
  }
  return true;
}

inline bool load_file(const std::string &path, std::vector<uint8_t> &out) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(static_cast<size_t>(size));
  size_t got = size > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
  std::fclose(f);
  return got == out.size();
}

}  // namespace rtxt_real
