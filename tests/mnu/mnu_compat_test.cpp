/* MNU compat sweep: parse + round-trip real shipped .mnu menus.

   The fifteen revx02 JO-family menus come from the reference fixture set
   (<OPENNOVA_JO_ASSETS>/fixtures/mnu/jo_*.mnu; the whole test is gated on
   them) and are required-pass; every .mnu the packed retail install carries
   (OPENNOVA_JO_DIR, the base mount plus each expansion) is swept as a
   SKIP-LEG retail leg. Extra loose menus still sweep from argv.

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

#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <formats/mnu/mnu.h>

#include "common/file_io.h"
#include "common/retail_paths.h"

static int count_windows(const opennova::mnu::Window &w) {
  int n = 1;
  for (const auto &c : w.children) n += count_windows(c);
  return n;
}

/* Returns 1 = OK, 0 = failed, over a menu's source bytes. */
static int check_source(const char *path, const std::string &src) {
  opennova::mnu::Document doc;
  std::string err;
  if (!opennova::mnu::parse(src, doc, err)) {
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
  std::string s1 = opennova::mnu::serialize(doc, true, 2);
  opennova::mnu::Document doc2;
  std::string err2;
  if (!opennova::mnu::parse(s1, doc2, err2)) {
    printf("  FAIL %s (re-parse of serialized form: %s)\n", path, err2.c_str());
    return 0;
  }
  std::string s2 = opennova::mnu::serialize(doc2, true, 2);
  if (s1 != s2) {
    printf("  FAIL %s (round-trip not idempotent: %zu vs %zu bytes)\n", path,
           s1.size(), s2.size());
    return 0;
  }

  printf("  OK   %s (screens=%zu, windows=%d, %zu source bytes)\n", path,
         doc.screens.size(), windows, src.size());
  return 1;
}

/* Returns 1 = OK (or absent and not required), 0 = present-but-failed. */
static int try_menu(const char *path, bool required) {
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
static int sweep_mount(const std::string &install, const std::string &expansion, int &checked) {
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
  printf("retail leg: %d installed menu(s) parsed and round-tripped\n", checked);
  return 0;
}
