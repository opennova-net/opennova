// Validates the minimal set's menus parse through engine/formats/mnu and carry the boot +
// host/join + single-player structure the engine needs: main.mnu must expose the "Startup"
// node [orig: Menu_InitShellResources @ 0x552651 -> CUIScene_SelectNodeByName @ 0x63b6b0],
// mp.mnu the LAN host/join screen [orig: @ 0x5588fa], and sp.mnu the single-player mission
// screen [orig: SinglePlayer_PopulateMissionList @ 0x561840]. Authored from scratch
// (no retail asset). Navigation correctness is validated at the retail-launch
// step; this guards the files parse + carry the right screens. See
// assets/README.md.
#include <formats/mnu/mnu.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace mnu;

namespace {

int fail = 0;
#define CHECK(c, m)                                                                                 \
	do {                                                                                            \
		if (!(c)) {                                                                                 \
			std::fprintf(stderr, "FAIL: %s\n", m);                                                  \
			++fail;                                                                                 \
		}                                                                                           \
	} while (0)

std::string path(const char *name) { return std::string(GAME_ASSETS_DIR) + "/" + name; }

// An empty file is not a menu; it must not pass by having nothing to parse.
bool non_empty(const std::string &p) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	return f && f.tellg() > 0;
}

void check_menu(const char *name, const char *screen, const char *what) {
	Document doc;
	std::string err;
	CHECK(non_empty(path(name)), (std::string(name) + " is empty").c_str());
	CHECK(parse_file(path(name), doc, err), err.c_str());
	CHECK(doc.find_screen(screen) != nullptr, what);
}

} // namespace

int main() {
	// main.mnu — the engine selects the "Startup" node (case-insensitive).
	check_menu("main.mnu", "Startup", "main.mnu exposes the Startup node");
	// mp.mnu — the LAN multiplayer host/join screen.
	check_menu("mp.mnu", "LAN_MULTI_PLAYER", "mp.mnu exposes the LAN MP screen");
	// sp.mnu — the single-player mission list, where the packed mission has to appear.
	check_menu("sp.mnu", "SINGLE_PLAYER", "sp.mnu exposes the single-player screen");

	if (fail == 0) std::printf("OK: minimal menus parse (main Startup + mp host/join + sp list)\n");
	return fail == 0 ? 0 : 1;
}
