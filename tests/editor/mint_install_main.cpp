// mint_install <dir>: the synthetic game install of minted_install.h (the base game and its
// expansion, packed as a build packs them) written at <dir>, then each mission it holds named on
// stdout, one `mission <name>` line apiece (the base game's, then the expansion's as
// `mission <expansion>/<name>`). The editor's end-to-end scripts run over it where no game install
// is at hand (ci.yml editor-e2e: scripts/mcp/editor_mission_e2e.py and editor_terrain_e2e.py with
// --install <dir>). Run it from the repository (it reads fixtures/ there). Exits 0 when the install
// is written, 1 when it is not (said on stderr), 2 on a usage error.
#include "editor/minted_install.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: mint_install <dir>\n");
		return 2;
	}
	const std::string dir = argv[1];
	std::string error;
	if (!minted_install::mint(dir, error)) return 1; // mint() says why on stderr
	std::vector<minted_install::File> base, expansion;
	if (!minted_install::base_game(base, error) || !minted_install::expansion_game(expansion, error)) {
		std::fprintf(stderr, "mint_install: %s\n", error.c_str());
		return 1;
	}
	for (const minted_install::File &file : base)
		if (minted_install::extension_of(file.name) == ".bms") std::printf("mission %s\n", file.name.c_str());
	for (const minted_install::File &file : expansion)
		if (minted_install::extension_of(file.name) == ".bms")
			std::printf("mission %s/%s\n", minted_install::kExpansion, file.name.c_str());
	return 0;
}
