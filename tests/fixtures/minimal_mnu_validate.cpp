// Validates the minimal set's menus parse through libs/mnu and carry the boot +
// host/join structure the engine needs: main.mnu must expose the "Startup" node
// [orig: sub_552500 @ 0x552651 -> CUIScene_SelectNodeByName @ 0x63b6b0], and
// mp.mnu the LAN host/join screen [orig: @ 0x5588fa]. Authored from scratch
// (no retail asset). Navigation correctness is validated at the retail-launch
// step; this guards the files parse + carry the right screens. See
// fixtures/minimal/README.md.
#include <mnu/mnu.h>

#include <cstdio>
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

std::string path(const char *name) { return std::string(MINIMAL_FIXTURE_DIR) + "/resources/" + name; }

} // namespace

int main() {
	// main.mnu — the engine selects the "Startup" node (case-insensitive).
	{
		Document doc;
		std::string err;
		CHECK(parse_file(path("main.mnu"), doc, err), err.c_str());
		CHECK(doc.find_screen("Startup") != nullptr, "main.mnu exposes the Startup node");
	}

	// mp.mnu — the LAN multiplayer host/join screen.
	{
		Document doc;
		std::string err;
		CHECK(parse_file(path("mp.mnu"), doc, err), err.c_str());
		CHECK(doc.find_screen("LAN_MULTI_PLAYER") != nullptr, "mp.mnu exposes the LAN MP screen");
	}

	if (fail == 0) std::printf("OK: minimal menus parse (main Startup + mp host/join)\n");
	return fail == 0 ? 0 : 1;
}
