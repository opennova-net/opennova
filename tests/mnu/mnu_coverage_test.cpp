/* MNU round-trip key-coverage: catches attributes/tags silently dropped at parse
   time. The fixed-point idempotence check (mnu_compat) cannot see these: a
   parse-dropped key is absent on both sides of parse->serialize->parse, so the
   test stays green while authored data is lost. See ADR 0002.

   For each shipped menu we take the multiset of path-qualified element and
   attribute occurrences in the tree retail's reader builds from the original
   bytes (mnu_xml, the structural translation of NapiXML_ParseElementTree
   @ 0x769d70) and in the tree it builds from serialize(parse(original)). The
   written menu may hold nothing the original did not, and every occurrence it
   lacks must be one mnu::parse reported as left out (a ParseNote whose key is
   that occurrence or an element holding it): retail does not read it, so the
   model does not keep it. The notes are printed per menu. */

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_xml.h>

#include "common/file_io.h"
#include "common/retail_paths.h"

using Occurrences = std::map<std::string, int>;

static bool occurrences(const std::string &bytes, Occurrences &out, std::string &error) {
  opennova::mnu::SourceEncoding encoding;
  std::u32string text;
  opennova::mnu::decode_source(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), encoding, text);
  opennova::mnu_xml::Document xml;
  if (!opennova::mnu_xml::parse(text, xml, error)) return false;
  std::function<void(const opennova::mnu_xml::Node &)> visit = [&](const opennova::mnu_xml::Node &node) {
    ++out[opennova::mnu_xml::path_key(node)];
    for (const auto &a : node.attributes) {
      if (a.name.empty() && !a.has_value) continue; // whitespace before '>': no attribute anyone reads
      ++out[opennova::mnu_xml::path_key(node, &a)];
    }
    for (const auto &child : node.children) visit(*child);
  };
  for (const auto &root : xml.roots) visit(*root);
  return true;
}

static bool covered(const std::string &key, const std::vector<opennova::mnu::ParseNote> &notes) {
  for (const auto &note : notes) {
    if (note.key.empty() || key.compare(0, note.key.size(), note.key) != 0) continue;
    if (key.size() == note.key.size() || key[note.key.size()] == '/' || key[note.key.size()] == '@') return true;
  }
  return false;
}

static int check_menu(const char *path) {
  std::string src;
  if (!test_io::read_file_text(path, src)) {
    printf("  MISSING %s (required fixture)\n", path);
    return 0;
  }
  opennova::mnu::Document doc;
  std::string err;
  std::vector<opennova::mnu::ParseNote> notes;
  if (!opennova::mnu::parse(src, doc, err, &notes)) {
    printf("  FAIL %s (parse: %s)\n", path, err.c_str());
    return 0;
  }
  // The writer's own layout over the records: a document with its text layout would write the
  // file back as it was read, and the check would compare the source with itself.
  doc.text_layout.reset();
  const std::string ser = opennova::mnu::serialize(doc, true, 2);
  Occurrences authored, written;
  if (!occurrences(src, authored, err) || !occurrences(ser, written, err)) {
    printf("  FAIL %s (read: %s)\n", path, err.c_str());
    return 0;
  }
  std::vector<std::string> differences;
  for (const auto &row : written) {
    const auto it = authored.find(row.first);
    const int had = it == authored.end() ? 0 : it->second;
    if (row.second > had)
      differences.push_back(row.first + " written " + std::to_string(row.second) + " authored " + std::to_string(had));
  }
  for (const auto &row : authored) {
    const auto it = written.find(row.first);
    const int kept = it == written.end() ? 0 : it->second;
    if (kept < row.second && !covered(row.first, notes))
      differences.push_back(row.first + " authored " + std::to_string(row.second) + " written " +
                            std::to_string(kept) + " with no note");
  }
  for (const auto &note : notes)
    printf("       note %s line %zu %s: %s\n", path, note.line, note.key.c_str(), note.message.c_str());
  if (!differences.empty()) {
    printf("  FAIL %s has %zu structural difference(s):\n", path, differences.size());
    for (const auto &difference : differences) printf("       %s\n", difference.c_str());
    return 0;
  }
  int total = 0;
  for (const auto &row : authored) total += row.second;
  printf("  OK   %s (%d path-qualified occurrences; %zu reported left out)\n", path, total, notes.size());
  return 1;
}

int main(void) {
  // The full shipped revx02 JO-family menu set (15 files) from the reference
  // fixture set (the whole test is gated on it). Every one must round-trip
  // without losing an authored key retail reads.
  static const char *const kMenus[] = {
      "jo_main", "jo_sp", "jo_mp", "jo_options", "jo_game", "jo_player", "jo_weapon", "jo_loadout",
      "jo_color", "jo_cmap", "jo_stat", "jo_death", "jo_vehicle", "jo_item_db", "jo_splash",
  };
  std::vector<std::string> paths;
  for (const char *name : kMenus) {
    const std::string path = retail::reference_fixture((std::string("mnu/") + name + ".mnu").c_str());
    if (path.empty())
      return retail::skip("OPENNOVA_JO_ASSETS/fixtures/mnu/jo_*.mnu (the fifteen shipped revx02 menus)");
    paths.push_back(path);
  }
  int fail = 0;
  for (const std::string &p : paths)
    if (!check_menu(p.c_str())) ++fail;
  if (fail > 0) {
    fprintf(stderr, "\n%d MNU fixture(s) lost keys on round-trip\n", fail);
    return 1;
  }
  printf("\nAll %zu menus kept every authored key retail reads.\n", paths.size());
  return 0;
}
