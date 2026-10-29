#include "dfx2.h"

#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>

namespace opennova::games {

namespace {

// The files DFX2's stock data never ships, which its program still reads unchecked: its archives
// hold no cmap.mnu and no vehicle.mnu (resource.pff, localres.pff, language.pff and med.pff of
// the Steam install, read 2026-10-26), and dfx2.exe loads them where JO does
// [orig: Game_StartMission @0x521530 (@0x5234a5, @0x5234c1) and Input_HandleActionBinding
// @0x49ad50 (@0x49b988) for cmap.mnu; Input_HandleActionBinding @0x49ad50 (@0x49b8ff) and
// @0x4dd4c0 (@0x4ddbf3) for vehicle.mnu (dfx2.exe)]: the commander key and a vehicle's loadout
// open nothing, as the game ships. Nor does it ship nw_cdata.coo, the menu browser's cookie jar
// (in no archive, nor beside dfx2.exe in the Steam install), which it loads where JO does, its
// HRESULT ignored, and writes itself [orig: Menu_InitShellResources @0x54f750 (@0x54f887),
// UI_SaveCookieFileIfDirty @0x554d40 (dfx2.exe)].
constexpr const char *kAbsentResourceRoles[] = { "cmap_menu", "vehicle_menu", "nw_cdata", nullptr };

Title make() {
	Title t;
	t.code = "dfx2";
	t.display_name = "Delta Force Xtreme 2";
	// [orig: Game_ParseCommandLineAndInit @0x4A7360 (dfx2.exe) - off_8144D8's literal into
	//  g_TitleText @0x4A7D60..0x4A7D6C, the twin of Game_ParseCommandLineAndInit @0x4A7310 (the
	//  title sprintf @0x4A7D30..0x4A7D3C)]
	t.title_text = "DeltaForce: Xtreme 2";
	// Retail DFX2's version-1 SCR payloads use the JO_DFX2 key, which the SCR version byte selects.
	t.scr_policy = VFS_SCR_VERSION_DETECT;
	// The program the DFX2 install ships beside its archives (docs/titles/dfx2-vs-jo-re.md §6.1,
	// DFX2-5).
	t.executable = "dfx2.exe";
	// The command file the Steam DFX2 install's UPDATE.WIZ names (CMD=DFX2.CMD,
	// PRODUCT_LOGIN=DFX2).
	t.command_file = "DFX2.CMD";
	// dfx2.exe's cfg table, one row longer than Jointops.exe's: the mpdfxcategory its host lists
	// print as DFXCATEGORY1 (gamecfg::config_var_rows, DFX2-7).
	t.config_table = gamecfg::ConfigTable::Dfx2;
	t.absent_resource_roles = kAbsentResourceRoles;
	return t;
}

} // namespace

const Title &dfx2_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
