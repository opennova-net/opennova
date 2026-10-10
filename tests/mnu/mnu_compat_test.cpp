/* MNU compat sweep: parse + round-trip real shipped .mnu menus.

   The fifteen revx02 JO-family menus come from the reference fixture set
   (<OPENNOVA_JO_ASSETS>/fixtures/mnu/jo_*.mnu; the whole test is gated on
   them) and are required-pass; every .mnu the packed retail install carries
   (OPENNOVA_JO_DIR, the base mount plus each expansion) is swept as a
   SKIP-LEG retail leg. Extra loose menus still sweep from argv.

   Three properties per menu:
   1. The differential: mnu_xml's reader (the structural translation of
      NapiXML_ParseElementTree @ 0x769d70) builds the same tree as an
      independent transcription of the skeptic's retail-reader port (the
      2026-09-23 grill's sk_retail_xml.py, below as `oracle`), over the same
      decoded text: tags, joined text, attributes in retail's list order, and
      children.
   2. The shipped menu writes: no WriteIssue (retail loads every shipped menu).
   3. The fixed point: serialize(parse(x)) re-parses and re-serializes to the
      same bytes, so the model the loader builds survives a save and reload. */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_xml.h>

#include "common/file_io.h"
#include "common/retail_paths.h"

namespace {

// --- the oracle: sk_retail_xml.py, transcribed line for line ---------------

struct OracleNode {
  std::u32string name;
  bool has_text = false;
  std::u32string text;
  std::vector<std::pair<std::u32string, std::pair<bool, std::u32string>>> attrs; // list (prepend) order
  OracleNode *parent = nullptr;
  std::vector<std::unique_ptr<OracleNode>> children;
};

bool oracle_ws(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0b || c == 0x0c; }

long oracle_wtol(const std::u32string &s, size_t j) {
  while (oracle_ws(s[j])) ++j;
  bool neg = false;
  if (s[j] == '+' || s[j] == '-') {
    neg = s[j] == '-';
    ++j;
  }
  long v = 0;
  while (s[j] >= '0' && s[j] <= '9') {
    v = v * 10 + long(s[j] - '0');
    ++j;
  }
  return neg ? -v : v;
}

const char *const kOracleEntities[] = {
    "&quot;", "&amp;", "&lt;", "&gt;", "&copy;", "&reg;", "&nbsp;",
    "&Agrave;", "&Aacute;", "&Acirc;", "&Atilde;", "&Auml;", "&Aring;", "&AElig;", "&Ccedil;", "&Egrave;",
    "&Eacute;", "&Ecirc;", "&Euml;", "&ETH;", "&Igrave;", "&Iacute;", "&Icirc;", "&Iuml;", "&Ntilde;",
    "&Ograve;", "&Oacute;", "&Ocirc;", "&Otilde;", "&Ouml;", "&Oslash;", "&Ugrave;", "&Uacute;", "&Ucirc;",
    "&Uuml;", "&Yacute;", "&THORN;", "&szlig;", "&agrave;", "&aacute;", "&acirc;", "&atilde;", "&auml;",
    "&aring;", "&aelig;", "&ccedil;", "&egrave;", "&eacute;", "&ecirc;", "&euml;", "&eth;", "&igrave;",
    "&iacute;", "&icirc;", "&iuml;", "&ntilde;", "&ograve;", "&oacute;", "&ocirc;", "&otilde;", "&ouml;",
    "&oslash;", "&ugrave;", "&uacute;", "&ucirc;", "&uuml;", "&yacute;", "&yuml;", "&thorn;", "&raquo;",
    "&laquo;"};
const char32_t kOracleValues[] = {0x22, 0x26, 0x3c, 0x3e, 0xa9, 0xae, 0x20, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
                                  0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xd0, 0xcc, 0xcd, 0xce, 0xcf, 0xd1, 0xd2,
                                  0xd3, 0xd4, 0xd5, 0xd6, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0,
                                  0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xf0, 0xec,
                                  0xed, 0xee, 0xef, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf8, 0xf9, 0xfa, 0xfb,
                                  0xfc, 0xfd, 0xff, 0xfe, 0xbb, 0xab};

char32_t oracle_lower(char32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

char32_t oracle_entity(const std::u32string &s, size_t pos, size_t &next) {
  const size_t start = pos;
  size_t end = pos;
  char32_t ch = s[end];
  while (ch != ';') {
    if (ch == 0 || ch == ' ') break;
    ++end;
    ch = s[end];
  }
  if (s[start + 1] == '#') {
    unsigned long v = static_cast<unsigned long>(oracle_wtol(s, start + 2)) & 0xFF;
    if (v >= 0x80) v |= 0xFF00;
    next = end + 1;
    return char32_t(v);
  }
  for (size_t e = 0; e < sizeof(kOracleEntities) / sizeof(kOracleEntities[0]); ++e) {
    const std::string name = kOracleEntities[e];
    const bool case_sensitive = e >= 7;
    bool match = start + name.size() <= s.size();
    for (size_t i = 0; match && i < name.size(); ++i) {
      const char32_t a = s[start + i], b = char32_t(static_cast<unsigned char>(name[i]));
      match = case_sensitive ? a == b : oracle_lower(a) == oracle_lower(b);
    }
    if (match) {
      next = end + 1;
      return kOracleValues[e];
    }
  }
  next = start + 1;
  return '&';
}

// False (with `why`) where the port raises Fail.
bool oracle_parse(std::u32string s, std::vector<std::unique_ptr<OracleNode>> &roots, std::string &why) {
  s.append(12, char32_t(0));
  size_t pos = 0;
  OracleNode *cur = nullptr, *last = nullptr, *saved = nullptr;
  if (s[0] == 0) return true;
  for (;;) {
    const char32_t ch = s[pos];
    if (ch == '<') {
      ++pos;
      while (s[pos] != 0 && oracle_ws(s[pos])) ++pos;
      const size_t tag_start = pos;
      if (s[pos] == 0) { why = "eof in tag"; return false; }
      while (!oracle_ws(s[pos]) && s[pos] != '>' && s[pos] != 0) {
        ++pos;
        if (s[pos] == 0) { why = "eof in tag name"; return false; }
      }
      const std::u32string tagname = s.substr(tag_start, pos - tag_start);
      if (s[tag_start] == '/') {
        if (!cur) { why = "close at root"; return false; }
        cur->has_text = true;
        cur = cur->parent;
        saved = cur;
      } else if (s.compare(tag_start, 3, U"!--") == 0) {
        char32_t c = s[pos];
        if (c != 0) {
          for (;;) {
            if (c != '-' && c != 0) {
              for (;;) {
                ++pos;
                if (s[pos] == '-' || s[pos] == 0) break;
              }
            }
            if (s.compare(pos, 3, U"-->") == 0) {
              pos += 2;
              break;
            }
            c = s[pos + 1];
            ++pos;
            if (c == 0) break;
          }
        }
      } else if (s[tag_start] == 'R' && s.compare(tag_start, 8, U"RAW_TEXT") == 0) {
        why = "RAW_TEXT present";
        return false;
      } else {
        auto n = std::make_unique<OracleNode>();
        n->name = tagname;
        n->parent = cur;
        OracleNode *raw = n.get();
        (cur ? cur->children : roots).push_back(std::move(n));
        cur = raw;
        last = raw;
        saved = cur;
      }
      while (s[pos] != '>') {
        while (s[pos] != 0 && oracle_ws(s[pos]) && s[pos] != '>') ++pos;
        const size_t k = pos;
        while (s[pos] != 0 && !oracle_ws(s[pos]) && s[pos] != '=' && s[pos] != '>') ++pos;
        const std::u32string name = s.substr(k, pos - k);
        bool has_value = false;
        std::u32string value;
        if (s[pos] == '=') {
          ++pos;
          const char32_t v30 = s[pos];
          if (v30 == '"' || v30 == '\'') {
            size_t q = pos + 1;
            const size_t st = q;
            if (s[q] != '"' && s[q] != '\'' && s[q] != 0) {
              for (;;) {
                ++q;
                if (s[q] == '"' || s[q] == '\'' || s[q] == 0) break;
              }
            }
            pos = q + 1;
            value = s.substr(st, pos - st);
          } else {
            const size_t st = pos;
            while (s[pos] != 0 && !oracle_ws(s[pos]) && s[pos] != '>' && s[pos] != '"') ++pos;
            value = s.substr(st, pos - st);
          }
          has_value = true;
        }
        if (last) last->attrs.insert(last->attrs.begin(), {name, {has_value, value}});
        while (s[pos] != 0 && !oracle_ws(s[pos]) && s[pos] != '>') ++pos;
        cur = saved;
        if (s[pos] == 0) break;
      }
      if (s[pos] == 0) { why = "eof in attrs"; return false; }
      ++pos;
    } else if (ch == '&') {
      size_t next = pos;
      const char32_t code = oracle_entity(s, pos, next);
      if (code & 0xFFFF) {
        if (!cur) { why = "entity at root"; return false; }
        cur->has_text = true;
        cur->text.push_back(char32_t(code & 0xFFFF));
      }
      pos = next;
    } else {
      if (!cur) {
        if (!oracle_ws(ch)) { why = "root text"; return false; }
        while (s[pos] != 0 && oracle_ws(s[pos])) ++pos;
      } else {
        cur->has_text = true;
        cur->text.push_back(ch);
        ++pos;
      }
    }
    if (s[pos] == 0) return true;
  }
}

std::string show(const std::u32string &s) {
  std::string out;
  for (char32_t c : s) {
    if (c >= 0x20 && c < 0x7F) out.push_back(static_cast<char>(c));
    else {
      char buf[16];
      std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
      out += buf;
    }
  }
  return out;
}

// The first difference between the two trees, or "".
std::string compare(const opennova::mnu_xml::Node &ours, const OracleNode &oracle, const std::string &path) {
  const std::string here = path + "/" + show(ours.tag);
  if (ours.tag != oracle.name) return here + ": tag " + show(ours.tag) + " vs " + show(oracle.name);
  if (ours.text != oracle.text) return here + ": text \"" + show(ours.text) + "\" vs \"" + show(oracle.text) + "\"";
  if (ours.attributes.size() != oracle.attrs.size())
    return here + ": " + std::to_string(ours.attributes.size()) + " attributes vs " +
           std::to_string(oracle.attrs.size());
  for (size_t i = 0; i < ours.attributes.size(); ++i) {
    const auto &a = ours.attributes[i];
    const auto &b = oracle.attrs[oracle.attrs.size() - 1 - i]; // the port prepends
    if (a.name != b.first || a.has_value != b.second.first || a.value != b.second.second)
      return here + ": attribute " + std::to_string(i) + " " + show(a.name) + "=" + show(a.value) + " vs " +
             show(b.first) + "=" + show(b.second.second);
  }
  if (ours.children.size() != oracle.children.size())
    return here + ": " + std::to_string(ours.children.size()) + " children vs " +
           std::to_string(oracle.children.size());
  for (size_t i = 0; i < ours.children.size(); ++i) {
    const std::string diff = compare(*ours.children[i], *oracle.children[i], here);
    if (!diff.empty()) return diff;
  }
  return std::string();
}

int count_windows(const opennova::mnu::Window &w) {
  int n = 1;
  for (const auto &c : w.children) n += count_windows(c);
  return n;
}

/* Returns 1 = OK, 0 = failed, over a menu's source bytes. */
int check_source(const char *path, const std::string &src) {
  // 1. The differential over the decoded text.
  opennova::mnu::SourceEncoding encoding;
  std::u32string text;
  opennova::mnu::decode_source(reinterpret_cast<const uint8_t *>(src.data()), src.size(), encoding, text);
  opennova::mnu_xml::Document xml;
  std::string xml_error;
  const bool ours_ok = opennova::mnu_xml::parse(text, xml, xml_error);
  std::vector<std::unique_ptr<OracleNode>> oracle_roots;
  std::string oracle_why;
  const bool oracle_ok = oracle_parse(text, oracle_roots, oracle_why);
  if (ours_ok != oracle_ok) {
    printf("  FAIL %s (reader %s, oracle %s: %s / %s)\n", path, ours_ok ? "reads" : "fails",
           oracle_ok ? "reads" : "fails", xml_error.c_str(), oracle_why.c_str());
    return 0;
  }
  if (ours_ok) {
    if (xml.roots.size() != oracle_roots.size()) {
      printf("  FAIL %s (%zu roots vs the oracle's %zu)\n", path, xml.roots.size(), oracle_roots.size());
      return 0;
    }
    for (size_t i = 0; i < xml.roots.size(); ++i) {
      const std::string diff = compare(*xml.roots[i], *oracle_roots[i], std::string());
      if (!diff.empty()) {
        printf("  FAIL %s (reader and oracle differ at %s)\n", path, diff.c_str());
        return 0;
      }
    }
  }

  opennova::mnu::Document doc;
  std::string err;
  std::vector<opennova::mnu::ParseNote> notes;
  if (!opennova::mnu::parse(src, doc, err, &notes)) {
    printf("  FAIL %s (parse: %s)\n", path, err.c_str());
    return 0;
  }
  // Retail loads every shipped menu, so none holds what it crashes or hangs on.
  for (const opennova::mnu::ParseNote &note : notes) {
    if (!note.fatal) continue;
    printf("  FAIL %s (a crash note, line %zu %s: %s)\n", path, note.line, note.key.c_str(), note.message.c_str());
    return 0;
  }
  if (doc.screens.empty()) {
    printf("  FAIL %s (parsed to zero screens)\n", path);
    return 0;
  }
  int windows = 0;
  for (const auto &s : doc.screens)
    for (const auto &root : s.roots) windows += count_windows(root);
  if (windows == 0) {
    printf("  FAIL %s (parsed to zero windows)\n", path);
    return 0;
  }

  // 2. Retail loads every shipped menu, so none has a write issue.
  const std::vector<opennova::mnu::WriteIssue> issues = opennova::mnu::write_issues(doc);
  if (!issues.empty()) {
    printf("  FAIL %s (%zu write issue(s), first: %s %s %s: %s)\n", path, issues.size(),
           issues[0].screen.c_str(), issues[0].window.c_str(), issues[0].field.c_str(), issues[0].message.c_str());
    return 0;
  }

  // 3. Fixed point: the first serialize may normalize the source, but re-parsing
  // and re-serializing it must reproduce the same bytes. Both in the writer's own
  // layout: a document with its text layout writes the file back as it was read.
  doc.text_layout.reset();
  std::string s1 = opennova::mnu::serialize(doc, true, 2);
  opennova::mnu::Document doc2;
  std::string err2;
  if (!opennova::mnu::parse(s1, doc2, err2)) {
    printf("  FAIL %s (re-parse of serialized form: %s)\n", path, err2.c_str());
    return 0;
  }
  doc2.text_layout.reset();
  std::string s2 = opennova::mnu::serialize(doc2, true, 2);
  if (s1 != s2) {
    printf("  FAIL %s (round-trip not idempotent: %zu vs %zu bytes)\n", path, s1.size(), s2.size());
    return 0;
  }

  printf("  OK   %s (screens=%zu, windows=%d, %zu source bytes; the reader matches the oracle)\n", path,
         doc.screens.size(), windows, src.size());
  return 1;
}

/* Returns 1 = OK (or absent and not required), 0 = present-but-failed. */
int try_menu(const char *path, bool required) {
  std::string src;
  if (!test_io::read_file_text(path, src)) {
    if (required) {
      printf("  MISSING %s (required fixture)\n", path);
      return 0;
    }
    printf("  SKIP %s (absent)\n", path);
    return 1;
  }
  return check_source(path, src);
}

/* Every .mnu one mount layer of the packed install serves. Returns the number
   of failures; `checked` accumulates the menus seen. */
int sweep_mount(const std::string &install, const std::string &expansion, int &checked) {
  opennova::Vfs vfs;
  if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) {
    printf("  FAIL mount_game(%s, %s): %s\n", install.c_str(), expansion.c_str(),
           vfs.last_error().c_str());
    return 1;
  }
  int fail = 0;
  for (const auto &loc : vfs.list_files()) {
    const std::string &name = loc.logical_name;
    if (name.size() < 4) continue;
    std::string ext = name.substr(name.size() - 4);
    for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".mnu") continue;
    std::vector<uint8_t> bytes;
    if (!vfs.read_file_raw(name, bytes)) {
      printf("  FAIL %s (unreadable on the %s mount)\n", name.c_str(),
             expansion.empty() ? "base" : expansion.c_str());
      ++fail;
      continue;
    }
    ++checked;
    const std::string label = (expansion.empty() ? std::string("<install>/") : expansion + "/") + name;
    if (!check_source(label.c_str(), std::string(bytes.begin(), bytes.end()))) ++fail;
  }
  return fail;
}

} // namespace

int main(int argc, char **argv) {
  int fail = 0;

  /* The fifteen shipped revx02 menus from the reference fixture set are
     required-pass; the fixed-point idempotence proof matches mnu_coverage's
     set. */
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
  for (const std::string &p : paths)
    if (!try_menu(p.c_str(), true)) ++fail;

  /* Developer-only: extra loose menus passed on the command line still sweep
     without failing the ctest registration, which passes none. */
  for (int i = 1; i < argc; ++i)
    try_menu(argv[i], false);

  if (fail > 0) {
    fprintf(stderr, "\n%d required MNU fixture(s) FAILED\n", fail);
    return 1;
  }

  /* The retail leg: every .mnu the packed install serves, base mount and each
     expansion. */
  const std::string install = retail::install();
  if (install.empty())
    return retail::skip_leg("OPENNOVA_JO_DIR (the packed install's .mnu set)");
  int checked = 0;
  fail += sweep_mount(install, std::string(), checked);
  for (const std::string &expansion : retail::expansions())
    fail += sweep_mount(install, expansion, checked);
  if (fail > 0) {
    fprintf(stderr, "\n%d installed menu(s) FAILED\n", fail);
    return 1;
  }
  if (checked == 0) {
    fprintf(stderr, "\nthe packed install served no .mnu\n");
    return 1;
  }
  printf("retail leg: %d installed menu(s) parsed, matched the oracle and round-tripped\n", checked);
  return 0;
}
