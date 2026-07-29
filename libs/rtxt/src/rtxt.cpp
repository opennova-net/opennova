#include "rtxt/rtxt.h"

#include <io/le.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace opennova::rtxt {
namespace {

constexpr uint32_t RTXT_MAGIC = 0x54585452;  // "RTXT" little-endian
constexpr size_t HEADER_SIZE = 16;
constexpr size_t ENTRY_SIZE = 16;
constexpr char HOTKEY_MARKER[] = "{hot}";
constexpr size_t HOTKEY_MARKER_LEN = 5;

inline bool in_range(size_t offset, size_t size_needed, size_t buffer_size) {
  if (offset > buffer_size) return false;
  return buffer_size - offset >= size_needed;
}

inline uint32_t read_u32(const uint8_t *data, size_t off) {
  return opennova::io::read_u32_le(data + off);
}

inline void write_u32(std::vector<uint8_t> &buf, uint32_t val) {
  uint8_t tmp[4];
  opennova::io::write_u32_le(tmp, val);
  buf.insert(buf.end(), tmp, tmp + 4);
}

inline void write_u32_at(std::vector<uint8_t> &buf, size_t off, uint32_t val) {
  opennova::io::write_u32_le(&buf[off], val);
}

// Read null-terminated string from buffer starting at offset.
// Returns the string and updates offset to point after the null terminator.
std::string read_string(const uint8_t *data, size_t size, size_t &offset) {
  std::string result;
  while (offset < size && data[offset] != 0) {
    result.push_back(static_cast<char>(data[offset]));
    ++offset;
  }
  if (offset < size) {
    ++offset;  // Skip null terminator
  }
  return result;
}

}  // namespace

std::string to_upper(const std::string &s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return result;
}

std::string strip_hotkey(const std::string &text, int &hotkey_index) {
  size_t pos = text.find(HOTKEY_MARKER);
  if (pos == std::string::npos) {
    hotkey_index = -1;
    return text;
  }
  hotkey_index = static_cast<int>(pos);
  return text.substr(0, pos) + text.substr(pos + HOTKEY_MARKER_LEN);
}

std::string File::get(const std::string &key) const {
  if (!lookup_built_) {
    // Fallback linear search if lookup not built.
    std::string upper_key = to_upper(key);
    for (const auto &entry : entries) {
      if (to_upper(entry.key) == upper_key) {
        return entry.text;
      }
    }
    return "";
  }
  auto it = lookup_.find(to_upper(key));
  if (it == lookup_.end()) {
    return "";
  }
  return entries[it->second].text;
}

bool File::has(const std::string &key) const {
  if (!lookup_built_) {
    std::string upper_key = to_upper(key);
    for (const auto &entry : entries) {
      if (to_upper(entry.key) == upper_key) {
        return true;
      }
    }
    return false;
  }
  return lookup_.find(to_upper(key)) != lookup_.end();
}

const Entry *File::find_in_section(const std::string &section_name,
                                   const std::string &key) const {
  // [orig: TextResource_FindEntryBySectionAndKey @ 0x75D250] — walk sections in
  // order, first name match wins, accumulating preceding string_counts to find
  // the section's first entry index. The engine never consults the entry's own
  // section_index field; neither do we, so behaviour matches even on files
  // that violate the grouping invariant.
  std::string upper_section = to_upper(section_name);
  size_t start_index = 0;
  for (const auto &section : sections) {
    if (to_upper(section.name) == upper_section) {
      // [orig: TextResource_FindKeyInSection @ 0x75D1E0] — bounded key walk,
      // first match wins, index validated against the header entry count.
      std::string upper_key = to_upper(key);
      for (uint32_t i = 0; i < section.string_count; ++i) {
        size_t index = start_index + i;
        if (index >= entries.size()) return nullptr;
        if (to_upper(entries[index].key) == upper_key) {
          return &entries[index];
        }
      }
      return nullptr;
    }
    start_index += section.string_count;
  }
  return nullptr;
}

std::string File::get_in_section(const std::string &section_name,
                                 const std::string &key) const {
  const Entry *entry = find_in_section(section_name, key);
  return entry ? entry->text : "";
}

bool File::is_grouped() const {
  if (sections.empty()) return true;
  std::vector<uint32_t> run_counts(sections.size(), 0);
  uint32_t prev_section = 0;
  for (const auto &entry : entries) {
    if (entry.section_index >= sections.size()) return false;
    if (entry.section_index < prev_section) return false;
    prev_section = entry.section_index;
    ++run_counts[entry.section_index];
  }
  for (size_t i = 0; i < sections.size(); ++i) {
    if (run_counts[i] != sections[i].string_count) return false;
  }
  return true;
}

void File::normalize_grouping() {
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry &a, const Entry &b) { return a.section_index < b.section_index; });
  for (auto &section : sections) section.string_count = 0;
  for (const auto &entry : entries) {
    if (entry.section_index < sections.size()) {
      ++sections[entry.section_index].string_count;
    }
  }
  if (lookup_built_) build_lookup();
}

std::vector<const Entry *> File::get_section_entries(uint32_t section_index) const {
  std::vector<const Entry *> result;
  for (const auto &entry : entries) {
    if (entry.section_index == section_index) {
      result.push_back(&entry);
    }
  }
  return result;
}

void File::build_lookup() {
  lookup_.clear();
  for (size_t i = 0; i < entries.size(); ++i) {
    // First occurrence wins, matching the engine's forward key walk
    // [orig: TextResource_FindEntryByKey @ 0x75D450].
    lookup_.emplace(to_upper(entries[i].key), i);
  }
  lookup_built_ = true;
}

bool parse(const uint8_t *data, size_t size, File &out, std::string &error) {
  out = File{};

  // Validate header size.
  if (size < HEADER_SIZE) {
    error = "RTXT file too small to contain header";
    return false;
  }

  // Check magic.
  uint32_t magic = read_u32(data, 0);
  if (magic != RTXT_MAGIC) {
    error = "Invalid RTXT magic (expected 'RTXT')";
    return false;
  }

  // Parse header.
  uint32_t text_data_end = read_u32(data, 4);
  read_u32(data, 8);  // section_meta_size (recomputed on write)
  uint32_t entry_count = read_u32(data, 12);

  // Validate entry table fits.
  size_t entry_table_size = static_cast<size_t>(entry_count) * ENTRY_SIZE;
  if (!in_range(HEADER_SIZE, entry_table_size, size)) {
    error = "Entry table extends beyond file";
    return false;
  }

  // Validate text data end position.
  if (text_data_end > size) {
    error = "TextDataEndPosition extends beyond file";
    return false;
  }

  // Text data starts after entry table.
  size_t text_data_start = HEADER_SIZE + entry_table_size;

  // ...and text_data_end must not point back INTO the entry table. Only the
  // upper bound (> size) was checked, so a backward end offset was accepted and
  // the section count was then read out of the entry table's own bytes —
  // parsing structure out of unrelated data instead of failing.
  if (text_data_end < text_data_start) {
    error = "RTXT text data end precedes the text data start";
    return false;
  }

  // Parse entry table (we'll fill in keys later).
  struct RawEntry {
    uint32_t text_offset;
    int16_t x;
    int16_t y;
    uint32_t section_index;
  };
  std::vector<RawEntry> raw_entries;
  raw_entries.reserve(entry_count);

  for (uint32_t i = 0; i < entry_count; ++i) {
    size_t off = HEADER_SIZE + static_cast<size_t>(i) * ENTRY_SIZE;
    RawEntry raw{};
    raw.text_offset = read_u32(data, off);
    uint32_t packed_xy = read_u32(data, off + 4);
    raw.x = static_cast<int16_t>(packed_xy & 0xFFFF);
    raw.y = static_cast<int16_t>((packed_xy >> 16) & 0xFFFF);
    raw.section_index = read_u32(data, off + 8);
    // off + 12 is unused (always 0)
    raw_entries.push_back(raw);
  }

  // Parse section metadata (located at text_data_end).
  if (text_data_end >= size) {
    // No section metadata - this is technically valid but unusual.
    // Just create entries without keys.
    for (uint32_t i = 0; i < entry_count; ++i) {
      Entry entry{};
      entry.key = "ENTRY_" + std::to_string(i);
      entry.position.x = raw_entries[i].x;
      entry.position.y = raw_entries[i].y;
      entry.section_index = raw_entries[i].section_index;

      // Read text.
      size_t text_pos = text_data_start + raw_entries[i].text_offset;
      if (text_pos < text_data_end) {
        while (text_pos < text_data_end && data[text_pos] != 0) {
          entry.text.push_back(static_cast<char>(data[text_pos]));
          ++text_pos;
        }
      }
      out.entries.push_back(std::move(entry));
    }
    out.build_lookup();
    return true;
  }

  // Read section count.
  if (!in_range(text_data_end, 4, size)) {
    error = "Cannot read section count";
    return false;
  }
  uint32_t section_count = read_u32(data, text_data_end);

  // Read section info entries (8 bytes each).
  size_t section_info_start = text_data_end + 4;
  size_t section_info_size = static_cast<size_t>(section_count) * 8;
  if (!in_range(section_info_start, section_info_size, size)) {
    error = "Section info table extends beyond file";
    return false;
  }

  struct SectionInfo {
    uint32_t name_offset;
    uint32_t string_count;
  };
  std::vector<SectionInfo> section_infos;
  section_infos.reserve(section_count);
  for (uint32_t i = 0; i < section_count; ++i) {
    size_t off = section_info_start + static_cast<size_t>(i) * 8;
    SectionInfo info{};
    info.name_offset = read_u32(data, off);
    info.string_count = read_u32(data, off + 4);
    section_infos.push_back(info);
  }

  // Section names follow the section info table.
  size_t section_names_start = section_info_start + section_info_size;
  size_t pos = section_names_start;

  // Read section names.
  out.sections.reserve(section_count);
  for (uint32_t i = 0; i < section_count; ++i) {
    Section section{};
    section.name = read_string(data, size, pos);
    section.string_count = section_infos[i].string_count;
    out.sections.push_back(std::move(section));
  }

  // Key names follow section names - one per entry.
  std::vector<std::string> keys;
  keys.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    keys.push_back(read_string(data, size, pos));
  }

  // Build final entries.
  out.entries.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    Entry entry{};
    if (i < keys.size()) {
      entry.key = keys[i];
    } else {
      entry.key = "ENTRY_" + std::to_string(i);
    }
    entry.position.x = raw_entries[i].x;
    entry.position.y = raw_entries[i].y;
    entry.section_index = raw_entries[i].section_index;

    // Read text from text data section.
    size_t text_pos = text_data_start + raw_entries[i].text_offset;
    if (text_pos < text_data_end) {
      while (text_pos < text_data_end && data[text_pos] != 0) {
        entry.text.push_back(static_cast<char>(data[text_pos]));
        ++text_pos;
      }
    }
    out.entries.push_back(std::move(entry));
  }

  out.build_lookup();
  return true;
}

bool parse_file(const std::string &path, File &out, std::string &error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "Unable to open RTXT file: " + path;
    return false;
  }

  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  return parse(data.data(), data.size(), out, error);
}

bool write(const File &file, std::vector<uint8_t> &out, std::string &error) {
  (void)error;
  out.clear();

  uint32_t entry_count = static_cast<uint32_t>(file.entries.size());
  uint32_t section_count = static_cast<uint32_t>(file.sections.size());

  // Build text data and track offsets.
  std::vector<uint8_t> text_data;
  std::vector<uint32_t> text_offsets;
  text_offsets.reserve(entry_count);

  for (const auto &entry : file.entries) {
    text_offsets.push_back(static_cast<uint32_t>(text_data.size()));
    for (char c : entry.text) {
      text_data.push_back(static_cast<uint8_t>(c));
    }
    text_data.push_back(0);  // Null terminator
  }

  // Calculate positions. The section meta block starts 4-byte aligned; the gap
  // after the text blob is zero-padded (every retail file obeys this; the
  // engine's allocator/fixup expects dword-aligned section rows).
  size_t entry_table_start = HEADER_SIZE;
  size_t entry_table_size = static_cast<size_t>(entry_count) * ENTRY_SIZE;
  size_t text_data_start = entry_table_start + entry_table_size;
  while ((text_data_start + text_data.size()) % 4 != 0) {
    text_data.push_back(0);
  }
  size_t text_data_end = text_data_start + text_data.size();

  // Build section metadata.
  std::vector<uint8_t> section_meta;

  // Section count.
  write_u32(section_meta, section_count);

  // Section info entries (first-key offsets filled in once keys are laid out).
  size_t section_info_start = section_meta.size();
  for (const auto &section : file.sections) {
    write_u32(section_meta, 0);  // First-key offset placeholder
    write_u32(section_meta, section.string_count);
  }

  // Section names (walked sequentially by the engine; no per-name offsets).
  for (const auto &section : file.sections) {
    for (char c : section.name) {
      section_meta.push_back(static_cast<uint8_t>(c));
    }
    section_meta.push_back(0);  // Null terminator
  }

  // Key names, one per entry in entry order, and each section's first-key
  // offset relative to (section_meta_offset + 4) — the engine rebases that
  // field into a pointer to the section's first key
  // [orig: TextResource_FixupPointers @ 0x75D050]. Sections claim consecutive
  // key runs sized by string_count, so the offsets are prefix sums.
  {
    std::vector<uint32_t> key_offsets;  // per entry, relative to meta+4
    key_offsets.reserve(entry_count);
    for (const auto &entry : file.entries) {
      key_offsets.push_back(static_cast<uint32_t>(section_meta.size() - 4));
      for (char c : entry.key) {
        section_meta.push_back(static_cast<uint8_t>(c));
      }
      section_meta.push_back(0);  // Null terminator
    }
    uint32_t keys_end = static_cast<uint32_t>(section_meta.size() - 4);
    size_t first_entry = 0;
    for (size_t i = 0; i < file.sections.size(); ++i) {
      uint32_t off = first_entry < key_offsets.size()
                         ? key_offsets[first_entry]
                         : keys_end;  // empty trailing section points at the blob end
      write_u32_at(section_meta, section_info_start + i * 8, off);
      first_entry += file.sections[i].string_count;
    }
  }

  // Header field [+8] counts the meta block EXCLUDING the section_count dword
  // (verified on all 98 retail bins; the engine never reads it).
  uint32_t section_meta_size = static_cast<uint32_t>(section_meta.size() - 4);

  // Write header.
  write_u32(out, RTXT_MAGIC);
  write_u32(out, static_cast<uint32_t>(text_data_end));
  write_u32(out, section_meta_size);
  write_u32(out, entry_count);

  // Write entry table.
  for (size_t i = 0; i < file.entries.size(); ++i) {
    const auto &entry = file.entries[i];
    write_u32(out, text_offsets[i]);

    // Pack X/Y into single uint32.
    uint32_t packed_xy = (static_cast<uint32_t>(entry.position.x) & 0xFFFF) |
                         ((static_cast<uint32_t>(entry.position.y) & 0xFFFF) << 16);
    write_u32(out, packed_xy);

    write_u32(out, entry.section_index);
    write_u32(out, 0);  // Unused
  }

  // Write text data.
  out.insert(out.end(), text_data.begin(), text_data.end());

  // Write section metadata.
  out.insert(out.end(), section_meta.begin(), section_meta.end());

  return true;
}

bool write_file(const File &file, const std::string &path, std::string &error) {
  std::vector<uint8_t> data;
  if (!write(file, data, error)) {
    return false;
  }

  std::ofstream f(path, std::ios::binary);
  if (!f) {
    error = "Unable to open file for writing: " + path;
    return false;
  }

  f.write(reinterpret_cast<const char *>(data.data()),
          static_cast<std::streamsize>(data.size()));
  if (!f) {
    error = "Failed to write RTXT file: " + path;
    return false;
  }

  return true;
}

}  // namespace opennova::rtxt
