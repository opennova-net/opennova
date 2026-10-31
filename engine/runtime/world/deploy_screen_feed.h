#pragma once

// The DEATH deploy screen's content feed (D-HUD-19 residue b): the two row
// loops and the STATIC message rules of the death-screen refresh, Godot-free
// with every lookup injected. The presenter turns these into list rows and
// widget text; the wire facts come from the joiner's ClientState (the 0x0A
// sub-block-0 timers, the 0x6E wave groups) or the authority's own view.
// [orig: UI_UpdateDeathScreenContent @0x5536a0 — the STATIC_RESPAWN_MSG1
//  arm @0x5538e7..0x553a7b, the zone loop + ListWidget_SortRows @0x553c5a,
//  the occupant loop @0x553c5f..0x553de3, STATIC_PSPRESPAWN_MSG1 /
//  STATIC_MEDIC_MSG1 / STATIC_CALLMEDIC_MSG @0x553e10..0x553f60]
// Witness record: docs/interface/hud-re.md (D-HUD-19).

#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::menu {
class MenuRuntime; // runtime/menu/menu_runtime.h
} // namespace opennova::menu

namespace opennova::world {

// The DEATH deploy screen re-reads its content every 16 ticks (0.256 s at the
// 62.5 Hz tick); the cadence rides the tick, not a wall-clock timer.
// [orig: every 16 ticks @0x55477d]
inline constexpr int32_t kDeployRefreshTicks = 16;

// THE DEATH_SHROUD REVEAL. The screen's show event hides DEATH_SHROUD (the
// window that holds the MAP, the spawn list and every static); then, every
// main frame while the screen is loaded, the shroud shows once the deploy
// overlay is up or more than the title's reveal ticks (Title::
// death_shroud_reveal_ticks: Jointops.exe's 240, dfx2.exe's 360) have passed
// since the local death stamp (a signed difference), and only inside that
// branch does the content refresh run, on each tick whose low four bits are
// zero.
// [orig: DeathScreen_UpdateUI @0x553150 — CWnd_SetShown(DEATH_SHROUD, 0)
//  @0x55325f; DeathScreen_UpdateShroudReveal @0x554730 — `dword_25A39BC &&
//  (g_DeployScreenActive || (int)(g_CurrentTick - g_CameraLerpStartTick) >
//  240)` @0x554737..0x554752, CWnd_SetShown(DEATH_SHROUD, 1) @0x554771,
//  `(g_CurrentTick & 0xF) == 0` -> UI_UpdateDeathScreenContent @0x55477d;
//  run from UI_ProcessMenuScreenFrame @0x54e67c] [orig: sub_552770 @0x552770 -
//  @0x552777..0x552792, the MAIN and DEATH_SHROUD shows @0x5527a4..0x5527d3,
//  sub_551E10 and sub_5503A0 @0x5527e1 / @0x5527e6 (dfx2.exe)]
inline bool death_shroud_revealed(bool deploy_screen_active, int32_t ticks_since_death,
		int32_t reveal_ticks) {
	return deploy_screen_active || ticks_since_death > reveal_ticks;
}
// Whether a refresh tick (low four bits zero) lies in (prev_tick, tick]: the
// device samples once per display frame, so it asks whether the main frames
// since its last sample reached one.
inline bool deploy_refresh_due(int64_t prev_tick, int64_t tick) {
	if (tick <= prev_tick) return false;
	const int64_t first = (prev_tick + kDeployRefreshTicks) & ~int64_t{kDeployRefreshTicks - 1};
	return first <= tick;
}

// One player queued on a zone's wave [orig: the member handles at
// unk_A85CC4[zoneIdx*8+i], named through entity->Name @0x553d5a..0x553d7f].
struct DeployOccupant {
    uint16_t handle = 0xFFFF;
    std::string name;
    bool self = false; // "<b><cFF4040>** %s **" for the local player
};

// One team-owned spawn zone the list shows [orig: the first loop
// @0x553aef..0x553c55 — def present, local team, SECURED (zone-timer
// EntryById[9] >= [10]) with attrib 0x40000; text "<team color>'<letter>' <name>",
// row value = index + 1].
struct DeployZoneRow {
    int index = 0;        // SpawnZoneList index (0-based; -1 for a banked zone outside it)
    char letter = 'A';    // 'A' + index ('@' for -1)
    std::string name_key; // WPNames/STRWPNAME%03d (index + 1)
    bool secured = false; // the first loop's listing gate
    uint16_t wave_countdown = 0; // entity+548 from the 0x6E fold
    std::vector<DeployOccupant> occupants; // the second loop's members
};

// The compiled row model of SPAWNPOINTS_LIST after both loops: the Default
// row (value 0), the zone rows (value index+1) in the witnessed
// ListWidget_SortRows(list, 0, 1) order — a case-insensitive ascending sort
// over the row TEXT [orig: cmp @0x6448a0 with sortParams {0 string, 1
// ascending} -> stricmp(a, b)] — then, per team zone with a wave entry, the
// occupant rows (value -1) inserted AFTER the row whose value is index+1,
// followed by ONE empty row. A zone with occupants but NO list row (it was
// not secured — the second loop has no secured gate) inserts at position 0,
// i.e. right after the Default row: the witnessed quirk, kept.
// [orig: the search @0x553c9c..0x553cbd, `insert_pos = 0` default @0x553c8e,
//  the member's sprintf of the entity's name (+0xF4) @0x553d8e, its
//  UIList_AddRow(text, -1, 0, insert_pos + 1) @0x553da5, the empty row
//  @0x553dbb]
struct DeployListRow {
    std::string text;
    int value = 0; // 0 default, index+1 zone, -1 occupant/blank (never a pick)
};

// The rows as the death screen's code-page list holds them: each text with
// its inline tags stripped (the compiled list has no markup channel yet,
// D-HUD-19). The rows are the page's bytes already (build_deploy_rows), as the
// game's list draws them (a glyph per byte): an occupant's row (value -1)
// carries the member's roster name as the roster holds it [orig: the sprintf
// of the member entity's name @0x553d8e, the UIList_AddRow call @0x553da5]
// (D-MNU-46).
std::vector<std::string> deploy_list_items(const std::vector<DeployListRow> &rows);

// The zone-row text before the sort: "<color>'<letter>' <name>" [orig:
// sprintf("%s'%c' %s") @0x553c1f, the letter index + 0x41 @0x553c16]; the
// Default row "<color>'<key>' <home>" [orig: sprintf("%s'%s' %s")
// @0x553b30..0x553b36]. Each row is the code page's bytes throughout, as the
// game's: the builder encodes the embedder's UTF-8 key, home and zone names
// (utf8_to_retail_text) before it joins them to the tag and the letter, a raw
// byte past 'Z' (D-MNU-46). `team_color_tag` is "<c4040FF>" or "<cFF2020>"
// for team 2 [orig: the team test and the tag @0x553adb..0x553af1].
struct DeployListInput {
    std::string team_color_tag;
    std::string default_key;  // Menu/DEFAULT_SPAWN_KEY
    std::string default_home; // Menu/<dword_7C75E0 key> ("HOME")
    std::vector<DeployZoneRow> zones;
    // WPNames/STRWPNAME%03d resolver (the embedder's gametext table).
    std::function<std::string(const std::string &key)> zone_name;
};

std::vector<DeployListRow> build_deploy_rows(const DeployListInput &in);

// The rows' colour tag for the listing team: "<cFF2020>" for team 2, else
// "<c4040FF>" [orig: the team test and the tag @0x553adb..0x553af1].
const char *deploy_list_team_color_tag(int team);

// The STATIC_RESPAWN_MSG1 arm: hidden, then the penalty timer wins
// ("%s  <cFF4040>%i" over Overlays/STROVER_PENALTYTIMER), else the wave zone
// listing the local player (`word_A85BC0 != -1` and SpawnZoneList_IndexOf
// >= 0): a NUMBERED zone (entity+538) prints "'<WPNames name>':  <cFF4040><countdown>",
// a lettered one "%c:  <cFF4040>%d" [orig: @0x5538e7..0x553a7b].
struct DeployStatusLine {
    enum class Kind { None, Penalty, Wave };
    Kind kind = Kind::None;
    int seconds = 0;   // penalty seconds or the wave countdown
    bool numbered = false;
    int zone_index = -1; // the wave zone's list index (Wave only)
};

struct DeployStatusInput {
    int penalty_seconds = 0;       // dword_A85B5C (0x0A slot+360)
    int self_zone_index = -1;      // SpawnZoneList_IndexOf(word_A85BC0), -1 none
    bool self_zone_numbered = false; // entity+538 nonzero
    int self_zone_countdown = 0;   // entity+548
};

DeployStatusLine build_deploy_status(const DeployStatusInput &in);

// The STATIC_RESPAWN_MSG1 text for a status line — the three sprintf arms
// [orig: @0x5538e7..0x553a7b: penalty "%s  <cFF4040>%i" over the
// Overlays/STROVER_PENALTYTIMER label; numbered wave "'%s':  <cFF4040>%d" over
// the WPNames name; lettered wave "%c:  <cFF4040>%d" with 'A' + index
// @0x553a56..0x553a64]. `penalty_label` and `zone_name` are the embedder-resolved
// strings (with their fallbacks already applied); "" for Kind::None.
std::string deploy_status_text(const DeployStatusLine &line,
                               const std::string &penalty_label,
                               const std::string &zone_name);

// The other three statics [orig: @0x553e10..0x553f60]: STATIC_PSPRESPAWN_MSG1
// shows while the spawn-target hold (dword_A85B68, slot+364) is nonzero;
// STATIC_MEDIC_MSG1 + STATIC_CALLMEDIC_MSG show while the local revive
// window (dword_A85B60, slot+368) is nonzero AND the local entity's +0x1E0
// word is zero (`!weaponSlots[16]` @0x553ec5). That word is the "a medic is
// reviving me" latch — set by S2C 0x3A (NapiNPClientMsg_0x03A @0x422680) and
// by the host's revive sender (@0x517cd0 stamps the victim), cleared by the
// local respawn (Game_InitNewRound @0x422740) and mission start; it is NOT a
// mount reference (the pre-2026-09-10 reading; the IDB's weaponSlots[44]
// blob swallows it).
struct DeployStaticsInput {
    int hold_seconds = 0;
    int revive_seconds = 0;
    bool local_medic_reviving = false; // entity+0x1E0 != 0
};
struct DeployStaticsVisibility {
    bool psp_respawn = false;
    bool medic = false; // both MEDIC_MSG1 and CALLMEDIC_MSG
};
DeployStaticsVisibility deploy_statics_visibility(const DeployStaticsInput &in);

// The three statics' texts [orig: @0x553e10..0x553f60]: STATIC_PSPRESPAWN_MSG1
// is "<STROVER_PSPRESPAWN>  <cFF4040><hold>", STATIC_MEDIC_MSG1
// "<STROVER_MEDICTIMER>  <cFF4040><revive>", and STATIC_CALLMEDIC_MSG the
// STROVER_CALLMEDIC format with its `%s` taking the MedicReq binding's display
// string (KeyBinding_FormatDisplayString — the embedder resolves the label); a
// format without `%s` shows as it is. The Overlays keys resolve through the
// game-text seam with the retail English fallbacks.
struct DeployStaticsText {
    std::string psp_respawn;
    std::string medic_timer;
    std::string call_medic;
};
DeployStaticsText deploy_statics_text(const DeployStaticsInput &in,
                                      const std::string &medic_key_label,
                                      const hud::GameTextLookup &lookup);



// The DEATH screen's team-service pair, BUTTON_TEAMLIST and SWAP_TEAMS, shown
// together: never outside a session, in a co-op or non-team game type, for a
// local player on team 0, or for a dead one under permanent death; else by the
// TeamChoose bit (0x4) of the session rules word: the hosting process's own
// game settings (ctx+0x68, the hosted-session latch, is set) or, on a joiner,
// the S2C 0x08 copy. (SWAP_TEAMS is the only
// one with a DEATH callback; BUTTON_TEAMLIST's action is the menu's.)
// [orig: DeathScreen_UpdateUI @0x553150 — the gates @0x553412..0x553443, the
//  ctx+0x68 pick @0x553445 between multiplayerAttributeFlags_34C @0x55344D and
//  dword_A821E4 @0x55345C, the shows @0x553489 / @0x5534AA;
//  UI_RegisterDeathScreenCallbacks @0x554610 (SWAP_TEAMS @0x554683)]
struct DeployTeamButtonsInput {
    bool in_session = false;      // g_NapiNPCtx.is_in_session
    uint32_t game_type = 0;       // g_GameType
    uint8_t team = 0;             // g_LocalPlayerEntity->Team (+0x162)
    bool permanent_death = false; // g_SessionPermanentDeath
    bool dead = false;            // g_LocalPlayerEntity->Flags & 2
    uint32_t rules_word = 0;      // the host's mp_attributes / a joiner's 0x08 word
};
bool deploy_team_buttons_shown(const DeployTeamButtonsInput &in);

// DFX2-41, the DEATH screen's loadout picker (Title::death_screen_loadout): on the screen's
// activation its PRIMARY / SECONDARY / ACCESSORY captions and outlines (the lists inside them)
// show for a player on a team who is not lying dead under permanent death, and only then does
// the activation resolve the class and fill the three lists (the armory's populate over this
// screen); else they hide and nothing fills.
// [orig: sub_550690 @0x550690 - the gate @0x550a3d..0x550a53, the captions and outlines
//  @0x550a55..0x550acd, Armory_ResolveSelectedClass @0x550ae0, UI_PopulateThreeCategoryLists
//  @0x550ae5; run by UI_DispatchScreenEvent's DEATH activation @0x54c231 (dfx2.exe)]
inline bool deploy_loadout_shown(uint8_t team, bool permanent_death, bool dead) {
	return team != 0 && !(permanent_death && dead);
}
// DFX2-41, the DEATH screen's RESTART_LEVEL (death.mnu authors it off screen, its R hotkey; the
// registration names no screen): its click restarts the mission, the in-game RESTART's exit
// (World::ingame_restart_command), out of a session in an assault game (0x10020, the 0x20000 bit
// aside) with the local player dead, after the in-game screens' close and a queued deploy pick
// 0 (input 12); otherwise nothing.
// [orig: sub_550BF0 @0x550bf0 - the click @0x550bf8, the gates @0x550bff..0x550c1e,
//  Game_CloseInGameScreens @0x550c20, Input_QueueEvent(12, 0) @0x550c2d, g_MissionExitReason = 4
//  @0x550c35; registered by sub_5512A0 @0x551310 (dfx2.exe)]
inline bool deploy_restart_level_allowed(uint32_t game_type, bool in_session, bool dead) {
	return (game_type & 0xFFFDFFFFu) == 0x10020u && !in_session && dead;
}
inline constexpr const char *kDeployLoadoutCaptions[3] = { "STATIC_PRIMARY", "STATIC_SECONDARY",
	"STATIC_ACCESSORY" };
inline constexpr const char *kDeployLoadoutOutlines[3] = { "PRIMARY_OUTLINE", "SECONDARY_OUTLINE",
	"ACCESSORY_OUTLINE" };

// --- DFX2-41: the SPAWNPOINTS_TABLE fill ---------------------------------------------------------
// dfx2.exe's DEATH screen lists its spawn points in a three-column table, refilled with the
// content: its scroll top and selected row kept, every row removed, the table and
// STATIC_LIST_TITLE shown; then the Default row, one row per spawn zone, column 0's sort flag
// set and the rows sorted by it, the scroll top and the selected row (by index) restored.
// [orig: sub_551450 @0x551450 - the keeps @0x55149d / @0x5514aa, CTableWnd_RemoveRow @0x5514ae,
//  the shows @0x5514b7..0x5514d9, the zone walk @0x55156d..0x551d1c, sub_6CBF40(0, 1) @0x551d28,
//  CTableWnd_SortByColumn @0x551d35, the restores @0x551d41..0x551d53; called from sub_551E10
//  @0x5521ed, the twin of UI_UpdateDeathScreenContent (dfx2.exe)]
//
// A zone gets a row when its entity has a definition, stands on the local player's team, has
// no zone-timer entry or a secured one ([9] >= [10]), its definition carries the spawn-zone
// attribute 0x40000 and SpawnZoneList holds it [orig: @0x551591..0x551624 (dfx2.exe)]: the
// embedder's walk over the handle table, in its order, hands those in as DeployTableZone.
struct DeployTableZone {
	int index = 0; // SpawnZoneList index: the row's value is index + 1
	uint8_t vehicle_category = 0; // the definition's +406 byte
	int32_t class_id = 0; // the definition's +80 class id
	uint32_t health_color = 0xFFDDDDDDu; // deploy_table_health_color over the zone's health
	bool greyed = false; // no driver (+368) and the definition's +92 word 1
	// The text column's lines past its blank head, in the fill's order: the seat lines
	// (deploy_table_seat_line), the mounted gunners' (deploy_table_gunner_line), the zone's
	// waiting members' (deploy_table_waiting_line), each ending CR LF.
	std::vector<std::string> lines;
};
// One row as the fill writes it: the column texts, the row value (the insert's column-0 cell
// value), column 1's cell value, the row colour (CTableWnd_SetRowColor, the colour flag set)
// and, for a zone's row alone, the state-1 colour bit (sub_6CD530's: set for a greyed row).
struct DeployTableRow {
	std::string cells[3];
	int32_t value = 0;
	int32_t cell1_value = 0;
	uint32_t color = 0;
	bool zone = false; // a zone's row (the Default row is not)
	bool state1_color = false;
};
// A zone's health colour: the zone's health (+286, a signed word) over its difficulty-scaled
// ceiling (0 taken as 1) in 16.16, above 0xC000 unsigned the fill's own 0xFFDDDDDD, above
// 0x6FFF signed the stance colour middle, else the stance colour bad (hudpos.def's
// stancecolor_middle / stancecolor_bad): the band rule the vehicle panel's seats take
// (hud::seat_health_band).
// [orig: sub_5A19D0 @0x5a19d0 - Entity_GetMaxHealthWithDifficulty @0x5a19e1, the guard
//  @0x5a19e7, the tests @0x5a1a12 / @0x5a1a20, g_StanceColorMiddle @0x5a1a3a,
//  g_StanceColorBad @0x5a1a22, the argument @0x5a1a36; called with 0xFFDDDDDD @0x55164c
//  (dfx2.exe)]
uint32_t deploy_table_health_color(int32_t health, int32_t max_health, uint32_t stance_middle,
		uint32_t stance_bad);
// The column-1 icon of a zone: 1 (a PSP) unless the definition's vehicle category names a
// vehicle (1 ground vehicle, 2 tank, 0xC motorcycle: 2; 3 helicopter, 4 plane: 4; 5..8 boat:
// 3), then the class id's own for 0x210..0x229. The names the same two switches pick feed a
// type line the fill overwrites with "\r\n" before any use, so no row shows them.
// [orig: the category switch @0x55170a, the class jump table @0x551771, "%d" @0x55185b, the
//  type line's sprintf @0x551890 and its overwrite @0x551895..0x5518ac (dfx2.exe)]
int deploy_table_icon(uint8_t vehicle_category, int32_t class_id);
// A seat line: the occupant's display name, "  <i>(Driver)<-i>" for the driver seat (slot 8) or
// the zone's driver, "  <i>(Gunner)<-i>" for the UseGun seat (slot 9); seats walk 8, 9, 0..7.
// [orig: the seat walk @0x5518b3..0x5519c0, (i + 8) % 10 @0x5518c0 (dfx2.exe)]
std::string deploy_table_seat_line(const std::string &name, int slot, bool is_driver);
// A mounted gunner's line: the name and its seat weapon's attach text in "  <i>(%s)<-i>", or
// "  <i>(Gunner)<-i>" with none (null: no slot, no weapon or no attachtextid; a key the gametext
// lacks resolved to "", which shows "()") [orig: @0x551a94..0x551b6b (dfx2.exe)].
std::string deploy_table_gunner_line(const std::string &name, const std::string *seat_weapon);
// A waiting member's line: "<b><cC0FFFF>** %s **<-b> <i>waiting</i>" for the local player,
// "<cC0F0F0>%s <i>waiting</i>" for another [orig: @0x551bd3..0x551c67 (dfx2.exe)].
std::string deploy_table_waiting_line(const std::string &name, bool self);
// The rows in insert order: the Default row (value 0; "' '", "0", "\r\nDefault"; the local team's
// colour, 0xFF00FF00 team 0 / 0xFFFF0000 team 2 / 0xFF00AFFF else, then 0xFFDDDDDD over it),
// then a zone's ("'<A + index>'", its icon, "\r\n" and its lines; its health colour, or
// 0xFF404040 and the state-1 colour bit when greyed).
// [orig: the Default row @0x5514de..0x551568; the zone row @0x551639..0x55168c, the letter
//  @0x5516ce, CTableWnd_AddRow @0x551ca0, its column-1 value @0x551cad, the cells
//  @0x551cbf..0x551ce0, sub_6CD420 @0x551cef, sub_6CD530 @0x551cfc (dfx2.exe)]
std::vector<DeployTableRow> build_deploy_table_rows(
		int local_team, const std::vector<DeployTableZone> &zones);
// The table's refill over the menu: DEATH's SPAWNPOINTS_TABLE (none: nothing, false), its
// scroll top and its selected row (the first in state 3) kept, every row removed, the table and
// STATIC_LIST_TITLE shown; each row inserted (its insert text "0" for the Default row, "" for a
// zone's, its value), column 1's cell value, the three cells, the row colour, and a zone row's
// state-1 colour bit; column 0 ascending and the rows sorted by it; the scroll top put back
// while it is under the row count (the frame clamps it to the last page) and the kept row index
// selected again, whatever row now stands there.
// [orig: sub_551450 @0x551450 - UI_FindScreenControl @0x551481 / @0x5514cc, sub_6CC7A0
//  @0x55149d, sub_6CC860 @0x5514aa, CTableWnd_RemoveRow @0x5514ae, CWnd_SetShown @0x5514b7 /
//  @0x5514d9, CTableWnd_AddRow @0x5514f2 / @0x551ca6, the column-1 value @0x5514f9 /
//  @0x551cad, CTableWnd_SetCellText @0x551508..0x551526 / @0x551cbf..0x551ce0, sub_6CD420
//  @0x551568 / @0x551cef, sub_6CD530 @0x551cfc, sub_6CBF40(0, 1) @0x551d28,
//  CTableWnd_SortByColumn @0x551d35, sub_6CC7B0 @0x551d41 (its bound @0x6cc7b0..0x6cc7c0),
//  CTableWnd_SetRowSelected @0x551d53 (dfx2.exe)]
bool fill_deploy_spawn_table(menu::MenuRuntime &menu, const std::vector<DeployTableRow> &rows);

class World;
struct SpawnZoneRegistry;
// The table's cell event (0x8000001) on a data row whose state is not 1 (locked), by its press
// form, both reading column 1's cell record: a press (0x1000002) whose value less one is 0 or
// more pans the map to that SpawnZoneList index; a double click's second press (0x1000004)
// whose value is not -1 queues that value as the deploy pick (input 12). -1: none. A row with
// no column-1 record (a table of one column) does nothing.
// [orig: sub_550CF0 @0x550cf0 - the event @0x550d1a, the forms @0x550e44..0x550e52, the state
//  test @0x550e5d, CTableWnd_GetCellRecord @0x550e73 / @0x550ec8, the pan arm
//  @0x550e7d..0x550e91, Input_QueueEvent(12, value) @0x550ee2 (dfx2.exe)]
struct DeployTableClick {
	int pan_zone_index = -1;
	int32_t pick_value = -1;
};
DeployTableClick deploy_table_click(const menu::MenuRuntime &menu, int table, int row,
		int32_t row_state, bool double_click);
// The map's pan target for a SpawnZoneList index: the zone entity's position less the local
// player's, x and y (16.16). False past the list, for a zone the world no longer holds, or with
// no local player.
// [orig: sub_550CF0 - SpawnZoneList_GetByIndex @0x550e87 (index < g_SpawnZoneCount
//  @0x43b73a), the deltas @0x550e93..0x550eb1 (dfx2.exe)]
bool deploy_table_pan_target(const World &world, const SpawnZoneRegistry &zones, int index,
		int32_t &dx, int32_t &dy);
// The DEATH map's move leg past its pan / zoom (hud::DeathMapView::on_event's true): DEATH's
// SPAWNPOINTS_TABLE, where the screen has one, has every row deselected. False without it.
// [orig: CMap_OverlayInputHandler @0x55452e..0x554541] [orig: sub_550F40 @0x5511c7..0x5511da
//  (dfx2.exe)]
bool deploy_map_moved(menu::MenuRuntime &menu);

// Both instruction widgets and the permanent-death status pair. A secured
// spawn leaves the first widget's prior text intact and hides the second;
// the later RESPawn1 assignment overwrites the intermediate RESPawn2 text.
// [orig: UI_UpdateDeathScreenContent @ 0x5536A0; SpawnZoneList_HasAny @ 0x43B910]
struct DeployInstructionsInput {
    uint32_t game_type = 0;
    uint8_t team = 0;
    bool dead = false;
    bool permanent_death = false;
    bool spectators_allowed = false;
    bool check_secured_spawn = false; // S2C 0x0F game_flags bit1
    bool has_spawn_zones = false;
    bool has_full_team_spawn = false; // actual timer entry[9] >= entry[10]
    std::string player_name;
    std::string clan;
    std::string kill_announcement;
    int32_t round_ticks = -1;
    int alive_players = 0;
};
struct DeployInstructions {
    bool permanent_death = false;
    bool show_first = false;
    bool show_second = false;
    bool replace_first = true;
    std::string first_text;
    std::string second_text;
    bool show_round_status = false;
    std::string round_text;
    std::string remaining_players_text;
};
// The text seam is the engine's one game-text lookup (hud/game_text_lookup.h).
DeployInstructions build_deploy_instructions(
        const DeployInstructionsInput &in, const hud::GameTextLookup &lookup);

// The DEATH screen's STATIC facts for one client as one value the embedder
// fills (its Godot record wraps it by value, ADR 0043 d10): the sub-block-0
// timers, the queued status line, the psp/medic show gates and the medic-call
// cooldown/serial. The witnesses live on the builders above and on the
// embedder's fold of the 0x0A / 0x6E state.
struct DeployScreenStatus {
    int penalty_seconds = 0;
    int revive_seconds = 0;
    int hold_seconds = 0;
    DeployStatusLine line;
    DeployStaticsVisibility statics;
    DeployStaticsText statics_text;
    DeployInstructions instructions;
    std::string respawn_text;
    int medic_cooldown_ticks = 0;
    int medic_request_serial = 0;
    bool team_buttons_shown = false; // deploy_team_buttons_shown
    bool loadout_shown = false;      // deploy_loadout_shown
    bool restart_level = false;      // deploy_restart_level_allowed
    bool shroud_main = false;        // Title::death_shroud_main
};

} // namespace opennova::world
