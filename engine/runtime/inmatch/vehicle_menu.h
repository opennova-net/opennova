#pragma once

// THE VEHICLE SCREEN (vehicle.mnu VEHICLE, D-NET-NEW-L5-5): the client half of
// vehicle spawning. The USE-ITEM press in a vehicle-loadout volume opens it
// (world::local_player_use_item_arm's kVehicleLoadout arm); its ITEM_LIST lists
// the vehicle groups the volume's bay offers with the S2C 0x70 counts; ACCEPT
// asks the host for the selected group (C2S 0x40), CANCEL just closes.
//
// The screen's state is retail's: the poll flag, the frame counter and the
// selection are globals that live with the menu scene, not with one showing
// of the screen. The flag is armed by the screen's init event, which runs when
// vehicle.mnu is first parsed into the scene (a later open finds the file's CRC
// and parses nothing), and cleared only by the scene's destroy; so once the
// screen has opened, the poll asks again every 64 main frames, open or closed.
// Ours lives with the client runtime (one connection), which spans every
// mission reload on it.
// [orig: UI_DispatchScreenEvent @0x54E6A0 -- case 3 (init) VehicleMenu_Open @0x564240
//  (the call @0x54EB30), case 4 (close) VehicleMenu_Close @0x563AC0 (the call
//  @0x54EDBD), case 5 (activate) VehicleMenu_OnActivate @0x563AD0 (the call
//  @0x54EFA0); the init event's one dispatch CUIScene_ParseScriptBlock (the call
//  @0x63B8CD), the close's CUIScene_DestroyAllContent (the call @0x63C4DC); the
//  CRC skip UIScene_LoadAndParseContent @0x63C830 (@0x63C900); the activate
//  CUIScene_SelectNodeByName @0x63B6B0 (@0x63B7E2)]

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::def { struct DefItemsFile; }
namespace opennova::world { class World; }

namespace opennova::inmatch {

class ClientRuntime;

// One shared vehicle-spawn group (g_ItemGroups, 32 at most, 264-byte rows): the
// id a pcvehicle_spawnlist token named (raw, as atol read it) and the name of
// the items.def row whose id is that id, else the first row's name.
// [orig: the parse ItemDef_ParseProperty @0x4A0294..0x4A029A -> ItemGroup_GetOrCreateSlot
//  @0x49DFC0; the name resolve ItemDefs_LoadAndValidate @0x4A1DA0 -- the id walk
//  @0x4A1F72..0x4A1F86 (none found reads row 0), the copy @0x4A1F93..0x4A1F9F]
struct VehicleSpawnGroup {
	int32_t item_id = 0;
	std::string name;
};
std::vector<VehicleSpawnGroup> vehicle_spawn_group_table(const def::DefItemsFile &items);

// One ITEM_LIST row: its text and its value (the group's index).
struct VehicleMenuRow {
	std::string text;
	int32_t value = 0;
};

class VehicleMenu {
public:
	// UI_OpenMenuScreen("vehicle.mnu", "VEHICLE"): the init event on the screen's
	// first parse (VehicleMenu_Open: populate, arm the poll, clear the 0x70
	// received flag, C2S 0x42), then every open's activate event (the
	// selection forgotten, the list populated).
	// [orig: Input_HandleActionBinding_0 @0x4E0AF1..0x4E0B05 (UI_OpenMenuScreen
	//  @0x4E0AFD); UI_OpenMenuScreen @0x54E520 -- the load @0x54E584, the select
	//  @0x54E599]
	void open(ClientRuntime &rt, const world::World &world, const def::DefItemsFile *items);
	// Once per main frame after the client net frame: the poll (its 0x42 leaves
	// at the next send boundary).
	// [orig: Game_ProcessMainFrame -- Client_ProcessNetworkFrame @0x526692, then
	//  VehicleMenu_PollAvailability @0x5266A1]
	void poll(ClientRuntime &rt, const world::World &world, const def::DefItemsFile *items);
	// An ITEM_LIST pick (event 0x5000001): its row turns selected and the
	// selection takes the row's value.
	// [orig: UI_OnItemListSelectionChanged @0x563B30 -- the store @0x563B57]
	void select_row(int row);
	// The ACCEPT (cancel false) or CANCEL (cancel true) click: ACCEPT on a local
	// player standing on a vehicle bay with a selection sends C2S 0x40 [the bay's
	// handle][the selection]; either closes the screen. Returns true when a 0x40
	// was queued.
	// [orig: the VEHICLE button callback @0x563B60 (registered @0x5641D9 /
	//  @0x5641F7, user arg 0 ACCEPT / 1 CANCEL)]
	bool click(ClientRuntime &rt, const world::World &world, bool cancel);

	// The read side for the screen's presenter.
	bool poll_armed() const { return poll_armed_; }
	int32_t poll_counter() const { return poll_counter_; }
	int32_t selection() const { return selection_; }
	int selected_row() const { return selected_row_; }
	const std::vector<VehicleMenuRow> &rows() const { return rows_; }

private:
	// UI_PopulateVehicleWeaponSlotList @0x5638A0.
	void populate(const ClientRuntime &rt, const world::World &world,
			const def::DefItemsFile *items);

	bool poll_armed_ = false;    // g_VehicleMenuOpen @0x25DCF24
	int32_t poll_counter_ = 0;   // g_VehicleMenuPollCounter @0x25DCF28 (never reset)
	int32_t selection_ = 0;      // g_VehicleMenuSelection @0x25DCF2C: the selected row's value, -1 none
	// The ITEM_LIST (a single-select CListWnd): its rows and the one in state 3.
	std::vector<VehicleMenuRow> rows_;
	int selected_row_ = -1;
};

} // namespace opennova::inmatch
