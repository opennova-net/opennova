#include "jo.h"

#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>

namespace opennova::games {

namespace {

Title make() {
	Title t;
	t.code = "jo";
	t.display_name = "Joint Operations";
	// [orig: Game_ParseCommandLineAndInit @0x4A7310 (the title sprintf @0x4A7D30..0x4A7D3C),
	//  off_8154D8's literal, into g_TitleText]
	t.title_text = "Joint Operations: Typhoon Rising";
	// Retail JO's version-1 SCR payloads use the JO_DFX2 key, which the SCR version byte selects.
	t.scr_policy = VFS_SCR_VERSION_DETECT;
	// The program JO:CA's install ships beside its archives (the name the in-match identity's AP
	// carries, the running program's own: docs/titles/dfx2-vs-jo-re.md §6.1, DFX2-5).
	t.executable = "Jointops.exe";
	// The command file JO:CA's UPDATE.WIZ names (docs/update/updater-re.md §2). A CMD names no
	// territory placeholder, so the territory never changes it.
	t.command_file = "JOPTR.CMD";
	// Jointops.exe's cfg table (gamecfg::config_var_rows).
	t.config_table = gamecfg::ConfigTable::Jo;
	return t;
}

} // namespace

const Title &jo_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
