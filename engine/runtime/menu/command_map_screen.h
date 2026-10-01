#pragma once

// THE CMAP SCREEN'S TABLES AND ORDERS COMPOSER (cmap.mnu): the TEAM tab's
// TEAMLIST (the full populate on the show, the incremental one on the tab
// radio and every squad change, the recruit / join / squad-colour clicks and
// the cells' custom draw, the ADDTO_* fireteam buttons), the PLAYERS tab's
// PLAYERLIST (its populate, the chat / audio mute and punt clicks and its
// custom draw), and the ORDERS tab (the GROUP and LOCATION combos, NEW_ORDER,
// the CURRENT_ORDERS edit and delete clicks and the order store behind them).
// Each is the retail CMAP routine over the menu runtime's widget state; the
// roster the walks read is the embedder's plain snapshot of the connection
// slot table, and every wire send or slot write a click asks for comes back
// as a typed result the embedder carries out.
// Witness record: docs/interface/hud-re.md "The windowed map views" (D-HUD-19).

#include <runtime/hud/game_text_lookup.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_runtime.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

// One active connection slot as the CMAP walks read it.
// [orig: the 64-byte PlayerSlotEntry — +12 index, +14 team, +20 name, +36
//  entity, +46 spectator, +48 leader, +49 fireteam, +50 mute flags, +51 squad
//  colour, +52 punt mark; entity+660 playerClass]
struct CommandMapPlayer {
	uint8_t slot = 0;
	uint8_t team = 0;
	std::string name;
	bool has_entity = false;
	uint8_t player_class = 0;
	bool spectator = false;
	uint8_t leader = 0xFF;
	uint8_t fireteam = 0;
	uint8_t mute = 0;
	uint8_t squad_color = 0;
	uint8_t punt_mark = 0xFF;
};

// The slot table's active list (slot order, as PlayerSlotTable_RebuildLinkedLists
// links it) and the local facts the walks compare against.
// [orig: g_PlayerSlotTable[5]; g_LocalPlayerEntity+0x154 (slot) / +0x162 (team,
//  signed); g_DeathScreenActive; g_NapiNPCtx.is_in_session]
struct CommandMapRoster {
	std::vector<CommandMapPlayer> players;
	uint8_t local_slot = 0xFF;
	int8_t local_team = 0;
	bool death_screen = false;
	bool in_session = false;
	// PlayerSlotTable_GetActiveSlot [orig: @0x434780].
	const CommandMapPlayer *find(uint8_t slot) const;
};

// The string tables the screen reads: gametext.bin (sections "menu",
// "Overlays", "WPNames") and Game.bin (section "Menu").
// [orig: g_TextGameText; g_TextMenuUi @0x25510f8 (Menu_InitShellResources
//  @0x552500 loads Game.bin)]
struct CommandMapText {
	hud::GameTextLookup gametext;
	hud::GameTextLookup menu_ui;
};

// The tab radios' interactive states a team-list populate leaves (`rules`
// only from the full populate).
struct CommandMapTabGates {
	bool orders = false;
	bool players = false;
	bool team = false;
	bool rules = false;
	bool sets_rules = false;
};

// The TEAMLIST populate the show runs: the table cleared, then one row per
// live, same-team, non-spectator slot holding an entity — column 0 "1" (the
// recruit box) when the row may be recruited else "-", column 1 the join box
// ("1" the row is the local leader, "0" joinable, "-" not), 2 the name, 3 the
// class, 5 the leader's name, 6 the fireteam; the row's value the slot and
// column 1's cell value the click mask (bit 0 recruit, bit 1 join). The gates:
// ORDERS while a row names the local slot its leader, PLAYERS in a session,
// TEAM / RULES in a session off the death screen.
// [orig: CMap_PopulateTeamList @0x547a50 (from sub_54B320 @0x54b480)]
CommandMapTabGates command_map_populate_team_list(MenuRuntime &menu, int table,
		const CommandMapRoster &roster, const CommandMapText &text);

// The incremental TEAMLIST update the TEAM radio and every S2C 0x71 / 0x73
// fold run: a qualifying slot's row is found by value (added with its name
// when missing; an existing row keeps its name), its other cells rewritten as
// above; then every row whose slot is gone or a spectator is removed. The
// ORDERS gate here also needs the death screen down; RULES is left alone.
// [orig: CMap_PopulateTeamList_0 @0x548d80 (from sub_549240 @0x549265 and
//  j_cmap_populate_team_list_0 @0x54e3c0)]
CommandMapTabGates command_map_update_team_list(MenuRuntime &menu, int table,
		const CommandMapRoster &roster, const CommandMapText &text);

// The recruit box's one-second hold: every row's column 0 goes back to "0"
// (recruitable) or "-" once its timer (column 2's cell value) is below `now`.
// Retail runs it from the cell's custom draw; the frame update runs it for
// every row before the compile, which draws the same.
// [orig: CMap_EntityWidgetHandler @0x5484a0..0x5484e2 (unsigned compare)]
void command_map_team_list_timers(MenuRuntime &menu, int table, uint32_t now_ms);

// A TEAMLIST press (the 0x8000001 cell event with a left press or its
// double-click form): column 0 on a recruitable row asks for C2S 0x46 and
// holds the box checked for a second; column 1 on a joinable row asks for C2S
// 0x43; column 4 steps the slot's squad colour (+51 = (+51 + 1) % 14). Those
// three columns then undo the table's own selection write; the others keep it.
// [orig: CMap_EntityWidgetHandler @0x548380 — @0x548588..0x5486a4]
struct CommandMapTeamClick {
	bool send_recruit = false;  // NetPacket_SendTeamChange(local, slot) — C2S 0x46
	bool send_join = false;     // NetPacket_SendWeaponSlotSwitch(slot) — C2S 0x43
	bool set_squad_color = false;
	uint8_t slot = 0;
	uint8_t squad_color = 0;
};
CommandMapTeamClick command_map_team_list_click(MenuRuntime &menu, int table, int row, int column,
		int32_t state, const CommandMapRoster &roster, uint32_t now_ms);

// The TEAMLIST custom draw: the header cell cleared to 0x2040 then its label;
// a body cell's row-state pass, the Map column's squad colour (or the team
// colour) over the cell inset by 2, then the content; a state-2 row outlined.
// [orig: CMap_EntityWidgetHandler @0x5483d1..0x54857b; g_SquadColors @0x83b450;
//  g_MinimapOverlayColorTable @0x840a10 (entries 10 / 9 / 12)]
void command_map_team_list_paint(const MenuTableCellEvent &event, MenuTableCellCanvas &canvas,
		int32_t row_value, const CommandMapRoster &roster);

// The PLAYERLIST populate: the table cleared, one row per live slot holding
// an entity — 0 the name; a spectator's class column the spectator suffix and
// both boxes "1", else the class name (same team) or "---", the chat box and
// the audio box "1" unless muted; the punt box "0" on the death screen else
// "1" while the slot's punt mark is set; the row coloured by team.
// [orig: CMap_UpdatePlayerListUI @0x547e00]
void command_map_populate_player_list(MenuRuntime &menu, int table,
		const CommandMapRoster &roster, const CommandMapText &text);

// A PLAYERLIST press: column 2 / 3 toggle another non-spectator player's
// chat (bit 1) / audio (bit 0) mute and rewrite the box; column 4 (off the
// death screen, another player) clears every punt box and mark, then — unless
// the press un-voted that player — marks it and asks for C2S 0x3F. The mute
// and punt columns undo the table's selection write.
// [orig: CMap_HandlePlayerListCallback @0x5486b0 — @0x5487af..0x54897e]
struct CommandMapPlayerClick {
	bool set_mute = false;
	uint8_t slot = 0;
	uint8_t mute = 0;
	bool clear_punt_marks = false;
	bool set_punt_mark = false;
	uint8_t punt_mark = 0xFF;
	bool send_punt = false; // C2S 0x3F [target]
};
CommandMapPlayerClick command_map_player_list_click(MenuRuntime &menu, int table, int row,
		int column, int32_t state, const CommandMapRoster &roster);

// The PLAYERLIST custom draw: the header as the TEAMLIST's, a body cell's
// row-state pass then content, a state-2 row outlined.
// [orig: CMap_HandlePlayerListCallback @0x5486fa..0x5487a2]
void command_map_player_list_paint(const MenuTableCellEvent &event, MenuTableCellCanvas &canvas);

// ADDTO_NO_FIRETEAM / A / B / C: the selected TEAMLIST rows whose slot names
// the local slot its leader, for C2S 0x45 — none selected, no send
// [orig: @0x548d4d..0x548d53].
// [orig: CCommandMap_SendWeaponActionToTeammates @0x548cb0]
std::vector<uint8_t> command_map_fireteam_members(const MenuRuntime &menu, int table,
		const CommandMapRoster &roster);

// THE ORDERS STORE: one entry per order the leader issued — the GROUP row's
// value (a fireteam | 0x10000000 or a member slot), the composed text and the
// packed code (bits 0..8 the entry's index, 9..11 WAITFOR, 12..15
// DIRECTIONAL, 16..23 COMMAND_ORDER, 24..31 LOCATION); the combos' row values
// the composer reads back. It lives with the screen: nothing but the next
// session entry clears it.
// [orig: dword_252DD80 (136-byte entries {group, text[128], code}),
//  g_CMapOrderCount @0x252dd84, the capacity dword_252DD88; cleared by
//  sub_5491b0 from the PreMenu state exit @0x5688b6]
struct CommandMapOrders {
	struct Order {
		int32_t group = 0;
		std::string text;
		uint32_t code = 0;
	};
	std::vector<Order> orders;
	std::vector<int32_t> group_values;
	std::vector<int32_t> location_values;
	void clear() {
		orders.clear();
		group_values.clear();
		location_values.clear();
	}
};

// The widget ids the ORDERS tab reads (-1 for an absent control).
struct CommandMapOrderWidgets {
	int group = -1;
	int command_order = -1;
	int location = -1;
	int directional = -1;
	int waitfor = -1;
	int current_orders = -1;
};

// What the LOCATION combo lists besides "My Position" and the squad: the
// placed waypoints' names (the 16-slot table, empty for a free slot), the
// spawn zones the minimap banks hold (each its SpawnZoneList index, -1 for
// an unregistered one), and the mission's location names.
struct CommandMapLocations {
	std::vector<std::string> user_waypoints;
	std::vector<int> zone_indices;
	std::vector<std::string> location_names;
};

// The ORDERS radio: GROUP gets one row per fireteam first met among the
// local team's other players (value fireteam | 0x10000000) and one per
// member the local slot leads (value the slot); LOCATION gets My Position
// (0x200000FF), the placed waypoints (index | 0x20000000), the banked zones
// through STROVER_OBJECTIVEPOINT_SHORT (index | 0x10000000), the location
// names (their index) and the led members (slot | 0x40000000); each of the
// five combos then selects row 0 when its first selected row is 0.
// [orig: CMap_OnOpenPopulate @0x5492a0 — @0x54934c..0x5497cd]
void command_map_populate_orders(MenuRuntime &menu, const CommandMapOrderWidgets &widgets,
		CommandMapOrders &store, const CommandMapRoster &roster,
		const CommandMapLocations &locations, const CommandMapText &text);

// One order to send as C2S 0x44: kind 1 to a fireteam's members (the local
// team's players the local slot leads in that fireteam), kind 0 to one member.
struct CommandMapOrderSend {
	bool send = false;
	uint8_t kind = 0;
	std::string text;
	std::vector<uint8_t> targets;
};

// NEW_ORDER: the order for the selected GROUP row (an existing entry with the
// same group value is rewritten in place), its text the group, command and
// location rows joined by "-" (plus "-" and the direction / wait rows when
// not their row 0), its code packed; a new entry adds a CURRENT_ORDERS row
// (value the entry's index, column 0 "0", column 1 the text), an existing one
// rewrites its row; then the send.
// [orig: CMap_BuildAndSendOrderCommand @0x5472d0]
CommandMapOrderSend command_map_new_order(MenuRuntime &menu, const CommandMapOrderWidgets &widgets,
		CommandMapOrders &store, const CommandMapRoster &roster);

// A CURRENT_ORDERS press: column 1 re-selects the five combos from the row's
// order code (GROUP from the code's low nine bits: the entry's index) and
// undoes the table's selection write; column 0 cancels the order (an empty
// C2S 0x44 to its targets), removes its row and entry and renumbers the rows
// after it.
// [orig: CCommandMap_HandleOrderAction @0x548990]
CommandMapOrderSend command_map_current_orders_click(MenuRuntime &menu,
		const CommandMapOrderWidgets &widgets, CommandMapOrders &store, int row, int column,
		int32_t state, int32_t cell_value, const CommandMapRoster &roster);

// After the menu rebuild: CURRENT_ORDERS re-listed from the store (retail's
// table lives in the process-lifetime in-game menu, so its rows survive).
void command_map_seed_current_orders(MenuRuntime &menu, int table, const CommandMapOrders &store);

} // namespace opennova::menu
