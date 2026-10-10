#include <runtime/controls/key_strings.h>

#include <cstring>
#include <string>
#include <utility>

namespace opennova::controls {

namespace {

// Retail's g_TextKeyHelp @0xB4C2B0: one process-wide table.
rtxt::File g_key_strings;
bool g_key_strings_loaded = false;

constexpr const char *kKeysSection = "Keys";

// The fallback with its "XX" untranslated marker removed (this port's
// convention; retail returns the literal untouched -- see key_strings.h). The
// marker leads the literal ("XXCtrl - ") or follows its leading blanks (the
// separator " XXor "), which stay: " XXor " -> " or ".
std::string marker_stripped(const char *fallback) {
  if (fallback == nullptr) {
    return std::string();
  }
  std::size_t lead = 0;
  while (fallback[lead] == ' ') {
    ++lead;
  }
  if (fallback[lead] == 'X' && fallback[lead + 1] == 'X') {
    return std::string(fallback, lead) + std::string(fallback + lead + 2);
  }
  return std::string(fallback);
}

// The "Text" literals' leading "!" / "|" marker, removed.
std::string text_marker_stripped(const char *fallback) {
  if (fallback == nullptr) {
    return std::string();
  }
  if (fallback[0] == '!' || fallback[0] == '|') {
    return std::string(fallback + 1);
  }
  return marker_stripped(fallback);
}

}  // namespace

void set_key_strings(rtxt::File table) {
  g_key_strings = std::move(table);
  g_key_strings_loaded = true;
}

void clear_key_strings() {
  g_key_strings = rtxt::File{};
  g_key_strings_loaded = false;
}

bool has_key_strings() {
  return g_key_strings_loaded;
}

std::string key_string(const char *key, const char *fallback) {
  // [orig: KeyHelp_GetStringWithFallback @0x51ed40] -- no table -> fallback
  // (@0x51ed47); section/key miss -> fallback (@0x51ed76); the entry's text
  // otherwise (@0x51ed78). The debug bit (dword_24C1930 & 0x10000 -> "&"
  // @0x51ed53) is a developer switch this port does not carry.
  if (!g_key_strings_loaded || key == nullptr) {
    return marker_stripped(fallback);
  }
  const rtxt::Entry *entry = g_key_strings.find_in_section(kKeysSection, key);
  if (entry == nullptr) {
    return marker_stripped(fallback);
  }
  return entry->text;
}

std::string key_help_string_raw(const char *section, const char *key, const char *fallback) {
  // [orig: KeyHelp_GetStringWithFallback @0x51ed40] -- the fallback as written.
  const rtxt::Entry *entry = g_key_strings_loaded && section != nullptr && key != nullptr
      ? g_key_strings.find_in_section(section, key)
      : nullptr;
  if (entry == nullptr) {
    return fallback != nullptr ? std::string(fallback) : std::string();
  }
  return entry->text;
}

std::string key_help_string(const char *section, const char *key, const char *fallback) {
  // [orig: KeyHelp_GetStringWithFallback @0x51ed40], any section.
  if (!g_key_strings_loaded || section == nullptr || key == nullptr) {
    return text_marker_stripped(fallback);
  }
  const rtxt::Entry *entry = g_key_strings.find_in_section(section, key);
  if (entry == nullptr) {
    return text_marker_stripped(fallback);
  }
  return entry->text;
}

}  // namespace opennova::controls
