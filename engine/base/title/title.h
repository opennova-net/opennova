#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opennova::gamecfg {
enum class ConfigTable : uint8_t; // formats/gamecfg/game_cfg.h
} // namespace opennova::gamecfg

namespace opennova {

// The container format a NEW archive of any title is written in, as a PffFormat value (0 =
// PFF_FORMAT_PFF3): PFF3, ADR 0008's default for a newly created archive, which every JO-family
// title opens [orig: PFF_Open @ 0x7682e0]. One value for every title: a title that ships another
// flavour makes it a Title field.
inline constexpr int kNewArchivePffFormat = 0;

// The HUD's label fonts by screen width, as a title's executable loads them at each resolution
// change (runtime/hud/hud_math.h hud_label_font_choice reads them): the Arial pair of the first
// tier whose bound holds the width, each slot at (width << 16) / the tier's divisor, and the two
// large slots, one file each, at (width << 16) / the large divisor.
// [orig: HUD_InitAllFonts @0x51EE20] [orig: the twin @0x51BD70 (dfx2.exe)]
struct TitleHudFontTier {
	int32_t max_width = 0; // the widest screen the tier holds; 0 ends the table (any width)
	const char *normal_fnt = nullptr; // g_HUDLabelFont
	const char *bold_fnt = nullptr;   // g_HUDLabelFontBold
	int32_t divisor = 0;
};
struct TitleHudFonts {
	const TitleHudFontTier *tiers = nullptr; // its last tier's max_width is 0
	const char *large_fnt = nullptr;         // g_HUDLabelFontLarge
	const char *impact38_fnt = nullptr;      // g_HUDLabelFontImpact38
	int32_t large_divisor = 0;
};

// The order a title's mission catalog takes (runtime/mission/mission_catalog.h sort_rows), the
// comparator its boot scan's qsort is handed: by file name, Jointops.exe's; by the [Info] TITLE
// (the row's display title), dfx2.exe's. Each case-insensitive.
// [orig: MissionList_ScanAndBuildFromFiles @ 0x563170 - qsort(.., 0x11E8,
//  Mission_CompareMapNames) @ 0x5635e4..0x5635f0; Mission_CompareMapNames @ 0x5628e0, stricmp on
//  entry+0] [orig: MissionList_ScanAndBuildFromFiles @0x5620d0 - qsort(.., 0x11E8,
//  CompareFunction) @0x562544..0x562550; CompareFunction @0x561840, _stricmp on entry+1044
//  (dfx2.exe)]
enum class MissionListOrder : uint8_t { FileName, Title };

// A title (CONTEXT.md "Title"): the retail game a game runs as, by its short code (the `/game
// <code>` launch flag, a project's target game). Its rows live in games/<code>/<code>_title.cpp,
// which games::install() hands to install_titles; engine/ holds the type and the lookups alone.
// A field no binary of the title has been read for is null, never another title's value.
struct Title {
	// The short launch-flag token, lowercase ("jo", "dfx2", "acidgambit").
	const char *code = nullptr;
	// The name a player and an artist read ("Joint Operations").
	const char *display_name = nullptr;
	// The title text the executable copies into g_TitleText: the _connectlog.txt header's TITLE
	// line and the VERSION command's title word.
	const char *title_text = nullptr;
	// The program the title's install ships beside its archives, which a player starts (and the
	// editor's Play in the game install).
	const char *executable = nullptr;
	// The command file a game folder's stock UPDATE.WIZ names for the title (its CMD), which
	// the launch reads for the folder's product (ADR 0067 d2).
	const char *command_file = nullptr;
	// The updater login of a title whose content OpenNova's service serves with no wizard in the
	// folder yet (its own wizard's PRODUCT_LOGIN, the product the updater installs: OP: Acid
	// Gambit's). Null for a retail title, whose folder's own UPDATE.WIZ names its login.
	const char *updater_login = nullptr;
	// The game.cfg table the title's executable walks (gamecfg::config_var_rows).
	gamecfg::ConfigTable config_table{};
	// DFX2-8: the player caps the title's executable compiles in. `max_players` is the most a
	// session takes: the config default's clamp and the NovaWorld host leg's (64, dfx2.exe's 250),
	// one more with a dedicated host's own slot (the session create's, the published and the slot
	// table's ceiling: 65, 251); `max_players_host_screen` the host screen's MAX_PLAYERS cap (64,
	// dfx2.exe's 100). On the Title, not the title's rules (runtime/world/title_rules.h) nor its
	// Protocol: they are rules, not wire, and the host leg (net/novaworld/connect_or_host.h) and the
	// service's hosted limits read them beside the session's caps (inmatch title_player_caps). The
	// game.cfg rows and ceilings keyed by config_table stay formats/gamecfg's. 0 unwitnessed.
	int32_t max_players = 0;
	int32_t max_players_host_screen = 0;
	// The mp.mnu screen the host dialog's restriction lists and their radios live on, the one the
	// title's executable names at each of them (runtime/inmatch/host_settings.h): Jointops.exe's
	// MULTI_PLAYER_HOST, dfx2.exe's NW_MULTI_PLAYER, the same code over other data
	// (docs/titles/dfx2-vs-jo-re.md §10). Null unwitnessed: the lists are found on no screen.
	const char *host_screen = nullptr;
	// The roles of the boot's required resources (base/gameprofile/required_resources.h) the
	// title's stock data never ships and its program still reads unchecked: each is optional for
	// the title. Null-terminated; null for none.
	const char *const *absent_resource_roles = nullptr;
	// Whether the title's executable mounts an expansion (`/exp` / `/mod`: an expansion folder's
	// archives over the base game's), which a project may build as (the editor's
	// check_project_expansion). False where no binary of the title was read for it.
	bool expansions = false;
	// Every .def name the title's executable opens, each at its open site: a definitions text under
	// any other name the game never reads (the editor's game_never_reads). Null-terminated, as
	// absent_resource_roles is; null: no claim, the title's binary unread for them.
	const char *const *read_defs = nullptr;
	// DFX2-36: the executable's HUD draws the EXPPOINTS element (runtime/hud/hud_exp_points.h:
	// the label, the S2C 0x81 score and the fade its fanfare starts, the S2C 0x80 / 0x82 tracker's
	// lines). Jointops.exe parses the EXPPOINTS line and reads none of it, and its 0x81 starts no
	// fade. On the Title, not the title's rules nor its Protocol: it is the executable's own HUD,
	// single player's too, and changes no wire byte and no world.
	// [orig: HUD_ParseHudposToken @0x59F370 (@0x5A0463..0x5A04AF, no reader)] [orig: sub_5A6D20
	//  @0x5A6EDE, sub_59B7E0 @0x59B7E0, sub_59B730 called @0x4223C5 (dfx2.exe)]
	bool hud_exp_points = false;
	// DFX2-41: the deploy screen picks the loadout. death.mnu's DEATH screen carries PRIMARY /
	// SECONDARY / ACCESSORY lists its activation fills for the player's class, and the screen's
	// close on its triggers' clear submits them (the loadout apply, a C2S 0x2F in session); its
	// spawn points are a SPAWNPOINTS_TABLE, not a list. With it the loadout serializer writes every
	// grenade position whose weapon resolves, shown or not, as the death screen authors no grenade
	// control (DFX2-38). False (Jointops.exe): the DEATH screen's SPAWNPOINTS_LIST, a close that
	// applies nothing, and only the shown grenade positions serialized.
	bool death_screen_loadout = false;
	// The HUD's label fonts by screen width (TitleHudFonts); null where no binary of the title was
	// read for them, which loads none (the HUD's slots fall back to its hudpos font).
	const TitleHudFonts *hud_fonts = nullptr;
	// DFX2-22: the order its mission catalog takes (MissionListOrder above): every list the
	// catalog feeds, the single-player screen's among them, shows it.
	MissionListOrder mission_list_order = MissionListOrder::FileName;
	// DFX2-4: the magic word player.sav and weapon.sav open with (runtime/profile/player_profiles.h):
	// the one each file's load gates on and its save writes, ahead of the "0211" version word both
	// titles share. Jointops.exe's "FPBC", dfx2.exe's "2XFD": neither game reads the other's
	// profile files. 0 unwitnessed.
	uint32_t profile_magic = 0;
	// The name the profile load gives record 0 before player.sav's records are read over it
	// (runtime/profile/player_profiles.h): dfx2.exe's literal "Bravo 1"; null, the process's
	// account name, as Jointops.exe reads it through GetUserNameA.
	const char *profile_first_name = nullptr;
	// Whether the executable's command line takes `/maxplayers N`, the cap a host file's
	// MaxPlayers line yields to (inmatch::read_host_file's max_player_arg): Jointops.exe's does;
	// dfx2.exe's walk knows no such switch, and its host file's MaxPlayers line stores its own
	// value alone. False unwitnessed.
	bool max_players_switch = false;
	// DFX2-29: the kit pages PlayerProfile_InitDefaults copies into a fresh profile's weapon
	// record (runtime/profile/player_profiles.h init_defaults), each a null-terminated list of
	// weapon names (every entry "<name> -1 -1 -1"; an empty list an empty page): the
	// single-player page, and the ten class pages, side 0's classes 5..9 then side 1's. Each title
	// a binary was read for names its own (Jointops.exe's are playersav::kJointOpsSinglePlayerKit
	// and kJointOpsClassKits); null unwitnessed, a title no launch runs (refused by name), whose
	// fresh record init_defaults gives no page.
	const char *const *profile_single_player_kit = nullptr;
	const char *const *const *profile_class_kits = nullptr;
	// DFX2-41: the DEATH screen's map pane, the executable's values: its zoom ceiling (the show's
	// fit, the right drag and the wheel clamp to it, and a fit landing on it resets the pan, the
	// target and the zoom ease), and the colour of the frame it draws round the map after it (0:
	// none). Jointops.exe's: 10.0 and no frame [orig: CMap_OverlayInputHandler @0x554310 -
	// flt_7C44B4 @0x5544dd; DeathScreen_UpdateUI @0x553150 - @0x5533ad / @0x5533f2;
	// DeathScreen_ZoomStep @0x5535d0].
	float death_map_zoom_max = 10.0f;
	uint32_t death_map_frame_color = 0;
	// DFX2-41: the DEATH screen's reveal: the ticks past the local death after which its
	// DEATH_SHROUD shows (at once under the deploy overlay), and whether its MAIN window hides
	// with the shroud at the activation and shows with it. Jointops.exe's: 240, MAIN untouched
	// [orig: DeathScreen_UpdateShroudReveal @0x554730 - the compare @0x554752; DeathScreen_UpdateUI
	//  @0x553150 - the shroud's hide @0x55325f].
	int32_t death_shroud_reveal_ticks = 240;
	bool death_shroud_main = false;
	// DFX2-41: the DEATH screen's content refresh also writes its team lists, TEAMBLUE and
	// TEAMRED, and its STATIC_TEAM line (world::deploy_team_lists). False (Jointops.exe): the call
	// there is an empty stub [orig: nullsub_177 @0x553140].
	bool death_screen_team_lists = false;
};

// Fills the registry with `titles` (each outliving the program) and names its default: the title
// a launch, a project or a session that names none runs as. A second call replaces the first.
void install_titles(const Title *const *titles, size_t count, const Title *default_title);

// Every lookup below aborts on an EMPTY registry, naming the invariant: a program that reaches
// them calls games::install() at its start (games/games.h).

// The title whose code is `code`, compared case-insensitively; null for an unknown one.
const Title *title_by_code(std::string_view code);
// The title a launch that names none runs as.
const Title &default_title();
// The installed titles, in install order; title_at is null past the end.
size_t title_count();
const Title *title_at(size_t index);
// The title whose command_file is `command_file` (compared case-insensitively, as the updater
// opens the file); null for none.
const Title *title_by_command_file(std::string_view command_file);

} // namespace opennova
