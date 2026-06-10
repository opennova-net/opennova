// RTXT string file parser (NovaLogic's localized string format).
// Used for the global tables Jointops.exe loads at init (gameerr.bin,
// gametext.bin, vmacros.bin, keyhelp.bin [orig: Game_InitSubsystems @ 0x4A6CD0]),
// the menu tables (menutxt.bin and friends), per-mission text bins, and the
// expansion override table (expansion\<exp>\<exp>.bin).
//
// Format layout (little-endian), as defined by the engine's offset->pointer
// fixup [orig: TextResource_FixupPointers @ 0x75D050]:
//   [0]  u32 magic "RTXT" (0x54585452) — present in every retail file but NEVER
//        validated by the engine; we require it on parse as a deliberate
//        strictness divergence (resource sniffing depends on it).
//   [4]  u32 section_meta_offset (a.k.a. text_data_end; 4-byte aligned, the
//        text blob is zero-padded up to it)
//   [8]  u32 section_meta_size EXCLUDING the section_count dword — never read
//        by the engine at runtime; an authoring-tool artifact we reproduce
//   [12] u32 entry_count
//   [16] entry table: entry_count * { u32 text_offset, u32 packed_xy,
//                                     u32 section_index, u32 pad (always 0) }
//        (packed_xy = (x & 0xFFFF) | ((y & 0xFFFF) << 16), both int16; no
//        runtime reader witnessed in Jointops.exe — preserved opaquely)
//   text blob: null-terminated cp1252 strings written sequentially in entry
//        order (text_offset is relative to the blob start, which immediately
//        follows the entry table)
//   section metadata at section_meta_offset:
//        u32 section_count
//        section_count * { u32 first_key_offset, u32 string_count }
//          first_key_offset = offset of the section's FIRST KEY string,
//          relative to (section_meta_offset + 4); the engine rebases it into a
//          pointer and walks keys from there [orig: TextResource_FindKeyInSection
//          @ 0x75D1E0]
//        section names (null-terminated, walked sequentially), then one key per
//        entry (null-terminated, contiguous across sections)
//
// Engine lookup model [orig: TextResource_FindEntryBySectionAndKey @ 0x75D250]:
// entries MUST be grouped contiguously by section — the engine computes an
// entry's index by accumulating preceding sections' string_counts and never
// reads the entry's own section_index field. Section and key matching is
// case-insensitive (stricmp); the first match wins.
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

  // Flat case-insensitive lookup by key across all sections, first match wins
  // [orig: TextResource_FindEntryByKey @ 0x75D450]. Returns empty string if
  // not found.
  std::string get(const std::string &key) const;

  // Check if a key exists (case-insensitive).
  bool has(const std::string &key) const;

  // Engine-faithful section-scoped lookup [orig: TextResource_FindEntryBySectionAndKey
  // @ 0x75D250]: finds the first section whose name matches (case-insensitive),
  // then scans that section's contiguous entry range (derived from accumulated
  // string_counts, NOT from Entry::section_index) for the first key match.
  // Mirrors engine behaviour even on ungrouped files. Null/empty on miss.
  const Entry *find_in_section(const std::string &section_name, const std::string &key) const;
  std::string get_in_section(const std::string &section_name, const std::string &key) const;

  // True when the engine's grouping invariant holds: every entry's
  // section_index is valid, entries are grouped contiguously per section, and
  // each section's string_count matches its run length. Vacuously true with no
  // sections.
  bool is_grouped() const;

  // Restore the grouping invariant: stable-sort entries by section_index and
  // recompute section string_counts. Rebuilds the lookup map if built.
  void normalize_grouping();

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

// Strip the first {hot} marker from text, returning the clean text and the
// marker's byte index; the accelerator is the character at that index after
// stripping. Later markers stay literal [orig: CButtonWnd_SetLabel @ 0x6572F0
// — strstr for the first marker, in-place 5-byte shift, hotkey char = byte at
// the recorded offset]. If no marker, returns the text and hotkey_index = -1.
std::string strip_hotkey(const std::string &text, int &hotkey_index);

// Convert a key to uppercase for case-insensitive comparison.
std::string to_upper(const std::string &s);

}  // namespace opennova::rtxt
