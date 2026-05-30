// RTXT string file parser (NovaLogic's localized string format).
// Used for menu text, game text, and other localized strings in strings/*.bin.
//
// Format layout (little-endian):
//   [0]  u32 magic "RTXT" (0x54585452)
//   [4]  u32 text_data_end   (absolute offset where the text blob ends)
//   [8]  u32 section_meta_size
//   [12] u32 entry_count
//   [16] entry table: entry_count * { u32 text_offset, u32 packed_xy,
//                                     u32 section_index, u32 unused }
//        (packed_xy = (x & 0xFFFF) | ((y & 0xFFFF) << 16), both int16)
//   text blob: null-terminated UTF-8 strings (text_offset is relative to here)
//   section metadata at text_data_end:
//        u32 section_count
//        section_count * { u32 name_offset, u32 string_count }
//        section names (null-terminated), then one key per entry (null-terminated)
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::rtxt {

// Position hint for text placement (from the packed X/Y field).
struct Position {
  int16_t x = 0;
  int16_t y = 0;
};

// A single string entry with its key, text, position, and section.
struct Entry {
  std::string key;        // String ID for lookup (e.g., "TAB_GENERAL")
  std::string text;       // Localized text (e.g., "{hot}General")
  Position position;      // X/Y position hint
  uint32_t section_index = 0;  // Which section this belongs to
};

// Section metadata.
struct Section {
  std::string name;            // Section name
  uint32_t string_count = 0;   // Number of strings in this section
};

// Parsed RTXT file.
struct File {
  std::vector<Entry> entries;
  std::vector<Section> sections;

  // Case-insensitive lookup by key. Returns empty string if not found.
  std::string get(const std::string &key) const;

  // Check if a key exists (case-insensitive).
  bool has(const std::string &key) const;

  // Get all entries in a section by index.
  std::vector<const Entry *> get_section_entries(uint32_t section_index) const;

  // Build internal lookup map for faster access.
  void build_lookup();

 private:
  // Uppercase key -> index in entries vector.
  std::unordered_map<std::string, size_t> lookup_;
  bool lookup_built_ = false;
};

// Parse an RTXT file from a byte buffer.
bool parse(const uint8_t *data, size_t size, File &out, std::string &error);

// Parse an RTXT file from disk.
bool parse_file(const std::string &path, File &out, std::string &error);

// Write an RTXT file to a byte buffer.
bool write(const File &file, std::vector<uint8_t> &out, std::string &error);

// Write an RTXT file to disk.
bool write_file(const File &file, const std::string &path, std::string &error);

// Strip {hot} markers from text, returning the clean text and hotkey index.
// If no hotkey marker, returns the original text and hotkey_index = -1.
std::string strip_hotkey(const std::string &text, int &hotkey_index);

// Convert a key to uppercase for case-insensitive comparison.
std::string to_upper(const std::string &s);

}  // namespace opennova::rtxt
