#include "acidgambit.h"

#include <games/jo/jo.h>

#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>

namespace opennova::games {

namespace {

Title make() {
	Title t;
	t.code = "acidgambit";
	t.display_name = "OP: Acid Gambit";
	t.title_text = "OP: Acid Gambit";
	// Its data is made by OpenNova's writers, which key version-1 SCR payloads as JO's.
	t.scr_policy = VFS_SCR_VERSION_DETECT;
	// No program of its own: the runtime runs it.
	t.executable = nullptr;
	// The command file its own wizard names, the one the release build writes into its package
	// (products/release_build.h).
	t.command_file = "ACIDGAMBIT.CMD";
	// The login its wizard names, the service's product for it (products.updater_login).
	t.updater_login = "ACIDGAMBIT";
	t.config_table = gamecfg::ConfigTable::Jo;
	// Its player caps are Joint Operations', as its rules are (acidgambit_rules.cpp).
	t.max_players = jo_title().max_players;
	t.max_players_host_screen = jo_title().max_players_host_screen;
	t.host_screen = jo_title().host_screen;
	return t;
}

} // namespace

const Title &acidgambit_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
