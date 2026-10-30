#include "jodemo.h"

#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>

#include <games/jo/jo.h>

namespace opennova::games {

namespace {

Title make() {
	Title t;
	t.code = "jodemo";
	t.display_name = "Joint Operations (Demo)";
	// The JO Demo stamps the SAME SCR version byte (1) as retail JO on every SCR file but keys
	// them with the DEFAULT key (0xABEEFACE), verified by decrypting demores.pff: all 214
	// version-1 payloads (.def/.adm/.aip/.ptg/.ptl/.ptu/.txt) decode cleanly under DEFAULT and to
	// garbage under JO_DFX2, and the demo ships no version-2 files. So the demo forces the
	// DEFAULT key.
	t.scr_policy = VFS_SCR_FORCE_DEFAULT;
	// The program the demo's install ships beside its archives.
	t.executable = "jodemo.exe";
	t.config_table = gamecfg::ConfigTable::Jo;
	// Unwitnessed: no binary of the title was read for its player caps (DFX2-8's sites), so it
	// keeps Joint Operations', as it runs Joint Operations' rules.
	t.max_players = jo_title().max_players;
	t.max_players_host_screen = jo_title().max_players_host_screen;
	return t;
}

} // namespace

const Title &jodemo_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
