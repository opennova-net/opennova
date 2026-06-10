/* MNU round-trip key-coverage: catches attributes/tags silently dropped at parse
   time. The fixed-point idempotence check (mnu_compat) cannot see these: a
   parse-dropped key is absent on both sides of parse->serialize->parse, so the
   test stays green while authored data is lost. See ADR 0002.

   For each committed real menu we extract the uppercased set of element tags +
   attribute keys (including bare flags) directly from the original bytes, and
   again from serialize(parse(original)). Every original key must survive into the
   serialized output, modulo a small allowlist of deliberate normalizations.

   The extractor is intentionally textual and parser-independent: it sees what the
   model might drop. Limitation: element tags and attribute keys share one set, so
   a dropped attribute whose name also appears as an element tag elsewhere would be
   masked. Acceptable for this corpus; flagged here so a future stricter,
   context-aware variant knows what it is replacing. */

#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "mnu/mnu.h"

static std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return s;
}

static bool read_file(const char *path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// Uppercased element tags + attribute keys (incl. bare flags), skipping comments
// and element text. Quote-aware so '>' / whitespace inside a value don't confuse
// the scan.
static std::set<std::string> extract_keys(const std::string &c) {
  std::set<std::string> keys;
  size_t i = 0, n = c.size();
  while (i < n) {
    if (c[i] != '<') {
      ++i;
      continue;
    }
    if (c.compare(i, 4, "<!--") == 0) {  // comment (covers 3-dash <!--- too)
      size_t e = c.find("-->", i + 4);
      i = (e == std::string::npos) ? n : e + 3;
      continue;
    }
    size_t j = i + 1;
    if (j < n && (c[j] == '/' || c[j] == '?' || c[j] == '!')) {  // close/decl
      size_t e = c.find('>', j);
      i = (e == std::string::npos) ? n : e + 1;
      continue;
    }
    // Find the matching unquoted '>'.
    size_t e = j;
    char q = 0;
    for (; e < n; ++e) {
      char ch = c[e];
      if (q) {
        if (ch == q) q = 0;
      } else if (ch == '"' || ch == '\'') {
        q = ch;
      } else if (ch == '>') {
        break;
      }
    }
    if (e >= n) break;
    std::string inside = c.substr(j, e - j);
    // Tokenize on whitespace, respecting quotes.
    std::vector<std::string> toks;
    std::string cur;
    char tq = 0;
    for (char ch : inside) {
      if (tq) {
        if (ch == tq)
          tq = 0;
        else
          cur += ch;
      } else if (ch == '"' || ch == '\'') {
        tq = ch;
      } else if (isspace(static_cast<unsigned char>(ch))) {
        if (!cur.empty()) {
          toks.push_back(cur);
          cur.clear();
        }
      } else {
        cur += ch;
      }
    }
    if (!cur.empty()) toks.push_back(cur);
    for (size_t k = 0; k < toks.size(); ++k) {
      std::string key = toks[k];
      if (k > 0) {  // attribute: key is the part before '='
        size_t eq = key.find('=');
        if (eq != std::string::npos) key = key.substr(0, eq);
      }
      if (!key.empty()) keys.insert(upper(key));
    }
    i = e + 1;
  }
  return keys;
}

// Deliberate normalizations: original key intentionally renamed on serialize.
static bool allowlisted(const std::string &key) {
  static const std::set<std::string> ok = {
      "DISABLE",  // bare DISABLE/disable -> serialized as disabled="true"
  };
  return ok.count(key) != 0;
}

static int check_menu(const char *path) {
  std::string src;
  if (!read_file(path, src)) {
    printf("  MISSING %s (required fixture)\n", path);
    return 0;
  }
  mnu::Document doc;
  std::string err;
  if (!mnu::parse(src, doc, err)) {
    printf("  FAIL %s (parse: %s)\n", path, err.c_str());
    return 0;
  }
  const std::string ser = mnu::serialize(doc, true, 2);
  std::set<std::string> a = extract_keys(src), b = extract_keys(ser);
  std::vector<std::string> missing;
  for (const auto &k : a)
    if (!b.count(k) && !allowlisted(k)) missing.push_back(k);
  if (!missing.empty()) {
    printf("  FAIL %s dropped %zu key(s):", path, missing.size());
    for (const auto &m : missing) printf(" %s", m.c_str());
    printf("\n");
    return 0;
  }
  printf("  OK   %s (%zu distinct keys preserved)\n", path, a.size());
  return 1;
}

int main(void) {
  // The full shipped revx02 JO-family menu set (15 files), committed under
  // fixtures/mnu/. Every one must round-trip without losing an authored key.
  const char *fixtures[] = {
      "fixtures/mnu/jo_main.mnu",    "fixtures/mnu/jo_sp.mnu",
      "fixtures/mnu/jo_mp.mnu",      "fixtures/mnu/jo_options.mnu",
      "fixtures/mnu/jo_game.mnu",    "fixtures/mnu/jo_player.mnu",
      "fixtures/mnu/jo_weapon.mnu",  "fixtures/mnu/jo_loadout.mnu",
      "fixtures/mnu/jo_color.mnu",   "fixtures/mnu/jo_cmap.mnu",
      "fixtures/mnu/jo_stat.mnu",    "fixtures/mnu/jo_death.mnu",
      "fixtures/mnu/jo_vehicle.mnu", "fixtures/mnu/jo_item_db.mnu",
      "fixtures/mnu/jo_splash.mnu",
  };
  int fail = 0;
  for (const char *p : fixtures)
    if (!check_menu(p)) ++fail;
  if (fail > 0) {
    fprintf(stderr, "\n%d MNU fixture(s) lost keys on round-trip\n", fail);
    return 1;
  }
  printf("\nAll %zu menus preserved every authored key.\n",
         sizeof(fixtures) / sizeof(fixtures[0]));
  return 0;
}
