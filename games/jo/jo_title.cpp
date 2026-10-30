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
	// DFX2-8: a session of at most 64, 65 with a dedicated host's slot: the config default's clamp
	// 1..64, the session apply's published cap and the session create's 65, the host leg's 64 / 65
	// and the slot table's 1..65; the host screen's cap word 64.
	// [orig: Config_SetDefaults @ 0x54d030 (@0x54d0ab..0x54d0b0); Game_ApplySessionSettingsToGlobals
	//  @ 0x551500 (@0x551b43..0x551b48); CNapiGameSession_ConnectOrHost @ 0x4d4f10
	//  (@0x4d5065..0x4d507b); CNapiGameSession_BuildAndCreateSession @ 0x5694d0
	//  (@0x569567..0x56956c); Server_AllocatePlayerSlotTable @ 0x51c180 (@0x51c195)]
	// [orig: g_HostScreenMaxPlayers @0x83C79C, read by HostDialog_ReadSettings @ 0x555940
	//  (@0x555c20..0x555c2d), UI_MapSliderIndexToMaxPlayers @ 0x555f00 and
	//  UI_PopulateHostSettingsFromConfig @ 0x555fe0 (@0x55631d, @0x55635a)]
	t.max_players = 64;
	t.max_players_host_screen = 64;
	// [orig: HostDialog_PopulateWeaponRestrictionList @0x556600 (@0x556611);
	//  HostDialog_PopulateClassListCombo @0x556700 (@0x556712); UI_RegisterHostSettingsCallbacks
	//  @0x5580f0 (each restriction control's screen)]
	t.host_screen = "MULTI_PLAYER_HOST";
	return t;
}

} // namespace

const Title &jo_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
