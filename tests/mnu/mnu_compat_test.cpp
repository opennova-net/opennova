/* MNU compat sweep: parse + round-trip real shipped .mnu menus.

   Three committed fixtures (real JO-family menus, sourced from the revx02 menu
   set) are required-pass; the rest of that set is swept skip-without-fail when
   present on the developer's disk. CI green requires only the committed three.

   This closes the gap the hand-authored widgets.mnu / all_widgets.mnu fixtures
   leave open: those exercise the widget vocabulary but are not shipped content,
   so they don't prove GOALS.md's acid test, "original content loads as-is."

   Fidelity property checked: a deliberately forgiving, normalizing parser (see
   the quirks list in mnu_xml.h: unquoted attrs, bare booleans, 3-dash and
   markup-bearing comments) cannot be expected to reproduce the original bytes,
   so we assert the FIXED POINT instead. serialize(parse(x)) must re-parse and
   re-serialize to an identical string. That proves the AST the loader builds
   from real game content survives a save/reload without losing or mutating
   structure, which is the achievable form of "loads as-is" for this format
   (the same standard the MUS text round-trip test holds itself to). */

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "mnu/mnu.h"

static bool read_file(const char *path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

static int count_windows(const mnu::Window &w) {
  int n = 1;
  for (const auto &c : w.children) n += count_windows(c);
  return n;
}

/* Returns 1 = OK (or absent and not required), 0 = present-but-failed. */
static int try_menu(const char *path, bool required) {
  std::string src;
  if (!read_file(path, src)) {
    if (required) {
      printf("  MISSING %s (required fixture)\n", path);
      return 0;
    }
    printf("  SKIP %s (absent)\n", path);
    return 1;
  }

  mnu::Document doc;
  std::string err;
  if (!mnu::parse(src, doc, err)) {
    printf("  FAIL %s (parse: %s)\n", path, err.c_str());
    return 0;
  }
  if (doc.screens.empty()) {
    printf("  FAIL %s (parsed to zero screens)\n", path);
    return 0;
  }
  int windows = 0;
  for (const auto &s : doc.screens) windows += count_windows(s.root_window);
  if (windows == 0) {
    printf("  FAIL %s (parsed to zero windows)\n", path);
    return 0;
  }

  /* Fixed-point idempotence: the first serialize may normalize the source, but
     re-parsing and re-serializing it must reproduce the same bytes. */
  std::string s1 = mnu::serialize(doc, true, 2);
  mnu::Document doc2;
  std::string err2;
  if (!mnu::parse(s1, doc2, err2)) {
    printf("  FAIL %s (re-parse of serialized form: %s)\n", path, err2.c_str());
    return 0;
  }
  std::string s2 = mnu::serialize(doc2, true, 2);
  if (s1 != s2) {
    printf("  FAIL %s (round-trip not idempotent: %zu vs %zu bytes)\n", path,
           s1.size(), s2.size());
    return 0;
  }

  printf("  OK   %s (screens=%zu, windows=%d, %zu source bytes)\n", path,
         doc.screens.size(), windows, src.size());
  return 1;
}

int main(void) {
  int fail = 0;

  /* All 15 committed revx02 menus are required-pass; the fixed-point idempotence
     proof now matches mnu_coverage's set (both run over fixtures/mnu/jo_*.mnu). */
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
  for (const char *p : fixtures)
    if (!try_menu(p, true)) ++fail;

  /* Developer-only: a menu not yet committed (e.g. PRE.MNU) still sweeps when
     present on disk, without failing CI. */
  try_menu("C:/Users/taylor/Desktop/revx02/PRE.MNU", false);

  if (fail > 0) {
    fprintf(stderr, "\n%d required MNU fixture(s) FAILED\n", fail);
    return 1;
  }
  return 0;
}
