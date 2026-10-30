#include "dfx2.h"

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
	// DFX2-8: a session of at most 250, 251 with a dedicated host's slot, at every site that holds
	// JO's 64 / 65; the host screen's cap word 100 (which its NovaWorld screen replaces with the
	// browser variable NWHL when the service sets one, a screen a headless host lacks).
	// [orig: Config_SetDefaults @0x54a2c0 (@0x54a33c..0x54a343); Game_ApplySessionSettingsToGlobals
	//  @0x54e750 (@0x54ed93..0x54ed9a); CNapiGameSession_ConnectOrHost @0x4d5100
	//  (@0x4d5255..0x4d526f); CNapiGameSession_BuildAndCreateSession @0x567707..0x56770e;
	//  Server_AllocatePlayerSlotTable @0x5191b0 (@0x5191c5) (dfx2.exe)]
	// [orig: g_HostMaxPlayersCap @0x835f0c, read by HostDialog_ReadSettings @0x553af0 (@0x553dd0),
	//  UI_MapSliderIndexToMaxPlayers @0x5540b0 and the populate's reads @0x5544cd / @0x55450a; the
	//  NWHL replace @0x5575ce in UI_ProcessLANSessionStateMachine @0x556ff0 (dfx2.exe)]
	t.max_players = 250;
	t.max_players_host_screen = 100;
	// [orig: HostDialog_PopulateWeaponRestrictionList @0x5547b0 (@0x5547c1);
	//  HostDialog_PopulateClassListCombo @0x5548b0 (@0x5548c2); @0x5562c0 (each restriction
	//  control's registration, @0x556318..0x55640b) (dfx2.exe)]
	t.host_screen = "NW_MULTI_PLAYER";
	// DFX2-36: the EXPPOINTS element and its 0x81 fade.
	// [orig: sub_5A6D20 @0x5A6D20 (@0x5A6EDE..0x5A6EE7); sub_59B7E0 @0x59B7E0; sub_59B730 called
	//  @0x4223C5 (dfx2.exe)]
	t.hud_exp_points = true;
	// DFX2-41 / DFX2-38: the DEATH screen's activation fills the three category lists, its close
	// on the triggers' clear applies them, and the serializer writes every grenade position
	// [orig: sub_550690 @0x550690 (Armory_ResolveSelectedClass @0x550ae0,
	//  UI_PopulateThreeCategoryLists @0x550ae5), run by UI_DispatchScreenEvent @0x54c231;
	//  sub_550BA0 @0x550ba0, called @0x5c9c09; WeaponLoadout_SerializeSelectionsToBuffer
	//  @0x564850 (@0x564b5c..0x564c20) (dfx2.exe)]. Jointops.exe's close calls nothing there and
	// its serializer tests each GRENADE_AMMO control [orig: nullsub_137 @0x5cac97;
	//  WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 (@0x565bcf..0x565be5)].
	t.death_screen_loadout = true;
	return t;
}

} // namespace

const Title &dfx2_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
