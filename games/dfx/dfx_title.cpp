#include "dfx.h"

#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>

#include <games/jo/jo.h>

namespace opennova::games {

namespace {

Title make() {
	Title t;
	t.code = "dfx";
	t.display_name = "Delta Force: Xtreme";
	t.scr_policy = VFS_SCR_VERSION_DETECT;
	// The program the DFX install ships beside its archives.
	t.executable = "DFX.EXE";
	t.config_table = gamecfg::ConfigTable::Jo;
	// Unwitnessed: no binary of the title was read for its player caps (DFX2-8's sites), so it
	// keeps Joint Operations', as it runs Joint Operations' rules.
	t.max_players = jo_title().max_players;
	t.max_players_host_screen = jo_title().max_players_host_screen;
	t.host_screen = jo_title().host_screen;
	return t;
}

} // namespace

const Title &dfx_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
