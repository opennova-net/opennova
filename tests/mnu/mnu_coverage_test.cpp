/* MNU round-trip key-coverage: catches attributes/tags silently dropped at parse
   time. The fixed-point idempotence check (mnu_compat) cannot see these: a
   parse-dropped key is absent on both sides of parse->serialize->parse, so the
   test stays green while authored data is lost. See ADR 0002.

   For each committed real menu we extract a multiset of path-qualified element
   and attribute occurrences directly from the original bytes, and again from
   serialize(parse(original)). Exact multiset equality is required after the one
   documented spelling normalization. This catches both dropped constructs and
   unexpected additions; an attribute can no longer be masked by a same-named
   tag elsewhere in the file. */

#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
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

using Occurrences = std::map<std::string, int>;

static std::string path_for(const std::vector<std::string> &stack,
                            const std::string &leaf) {
  std::string path;
  for (const auto &part : stack) path += "/" + part;
  return path + "/" + leaf;
}

static std::string normalize_attr(std::string key) {
  key = upper(std::move(key));
  // The writer canonicalizes retail's two accepted disabled spellings.
  if (key == "DISABLE") return "DISABLED";
  return key;
}

// Parser-independent, quote-aware structural scanner. Keys are:
//   /SCREEN/WINDOW/ITEMS                   (element occurrence)
//   /SCREEN/WINDOW/ITEMS@MULTISELECT       (attribute occurrence)
static Occurrences extract_occurrences(const std::string &c) {
  Occurrences found;
  std::vector<std::string> stack;
  int screen_index = 0;
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
    if (j < n && c[j] == '/') {
      size_t e = c.find('>', j);
      size_t name_start = j + 1;
      while (name_start < e &&
             isspace(static_cast<unsigned char>(c[name_start])))
        ++name_start;
      size_t name_end = name_start;
      while (name_end < e &&
             !isspace(static_cast<unsigned char>(c[name_end])) &&
             c[name_end] != '>')
        ++name_end;
      // Retail fixtures contain known mismatched closers such as
      // <SCROLLUP>...</APPEARANCE>. The lenient parser closes the current node
      // on any end tag, so mirror that nesting rule while keeping the opening
      // tag's own qualified identity.
      if (!stack.empty()) stack.pop_back();
      i = (e == std::string::npos) ? n : e + 1;
      continue;
    }
    if (j < n && (c[j] == '?' || c[j] == '!')) {
      size_t e = c.find('>', j);
      i = (e == std::string::npos) ? n : e + 1;
      continue;
    }
    // Retail fixtures include one malformed attribute with an unmatched quote
    // before the element terminator. The game still terminates the tag at '>',
    // so this structural scanner must do the same.
    size_t e = c.find('>', j);
    if (e >= n) break;
    std::string inside = c.substr(j, e - j);
    while (!inside.empty() &&
           isspace(static_cast<unsigned char>(inside.back())))
      inside.pop_back();
    const bool self_closing = !inside.empty() && inside.back() == '/';
    if (self_closing) {
      inside.pop_back();
      while (!inside.empty() &&
             isspace(static_cast<unsigned char>(inside.back())))
        inside.pop_back();
    }
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
    if (!toks.empty()) {
      const std::string tag = upper(toks[0]);
      std::string component = tag;
      // SCREEN is the only retail document root. Reset defensively because
      // malformed nested close spellings in one Screen must not qualify the
      // next root beneath it.
      if (tag == "SCREEN") {
        stack.clear();
        component += "[" + std::to_string(screen_index++) + "]";
      } else if (tag == "WINDOW") {
        for (size_t k = 1; k < toks.size(); ++k) {
          const size_t eq = toks[k].find('=');
          if (eq == std::string::npos) continue;
          if (upper(toks[k].substr(0, eq)) == "NAME") {
            component += "[" + upper(toks[k].substr(eq + 1)) + "]";
            break;
          }
        }
      }
      const std::string path = path_for(stack, component);
      ++found[path];
      for (size_t k = 1; k < toks.size(); ++k) {
        std::string key = toks[k];
        const size_t eq = key.find('=');
        if (eq != std::string::npos) key = key.substr(0, eq);
        if (!key.empty()) ++found[path + "@" + normalize_attr(key)];
      }
      if (!self_closing) stack.push_back(component);
    }
    i = e + 1;
  }
  return found;
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
  const Occurrences authored = extract_occurrences(src);
  const Occurrences written = extract_occurrences(ser);
  std::vector<std::string> differences;
  for (const auto &row : authored) {
    const auto it = written.find(row.first);
    const int actual = it == written.end() ? 0 : it->second;
    if (actual != row.second) {
      differences.push_back(row.first + " expected=" +
                            std::to_string(row.second) + " actual=" +
                            std::to_string(actual));
    }
  }
  for (const auto &row : written) {
    if (!authored.count(row.first)) {
      differences.push_back(row.first + " expected=0 actual=" +
                            std::to_string(row.second));
    }
  }
  if (!differences.empty()) {
    printf("  FAIL %s has %zu structural difference(s):\n", path,
           differences.size());
    for (const auto &difference : differences)
      printf("       %s\n", difference.c_str());
    return 0;
  }
  int total = 0;
  for (const auto &row : authored) total += row.second;
  printf("  OK   %s (%d path-qualified occurrences preserved)\n", path,
         total);
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
