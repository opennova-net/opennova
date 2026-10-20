#include <runtime/inmatch/vehicle_menu.h>

#include <runtime/inmatch/client_runtime.h>
#include <runtime/world/world.h>

#include <formats/def/def.h>

#include <algorithm>
#include <cstdio>

namespace opennova::inmatch {

namespace {

// The items.def `id` arm stores the authored id less 100000 at ItemDef+0x50
// [orig: ItemDef_ParseProperty `sub eax, 186A0h` @0x49EC54, the store @0x49EC5A];
// the group walk compares that word with the group's raw id.
constexpr int kItemDefIdBias = 100000;

} // namespace

std::vector<VehicleSpawnGroup> vehicle_spawn_group_table(const def::DefItemsFile &items) {
	std::vector<VehicleSpawnGroup> out;
	for (int g = 0; g < items.vehicle_spawn_id_count; ++g) {
		VehicleSpawnGroup group;
		group.item_id = items.vehicle_spawn_ids[g];
		// The first row whose id word is the group's; none found reads row 0
		// [orig: ItemDefs_LoadAndValidate @0x4A1F72..0x4A1F86, the copy
		//  @0x4A1F93..0x4A1F9F].
		size_t found = 0;
		for (size_t i = 0; i < items.count; ++i) {
			if (items.entries[i].id - kItemDefIdBias == group.item_id) {
				found = i;
				break;
			}
		}
		if (items.count != 0) group.name = items.entries[found].display_name;
		out.push_back(std::move(group));
	}
	return out;
}

// [orig: UI_PopulateVehicleWeaponSlotList @0x5638A0]
void VehicleMenu::populate(const ClientRuntime &rt, const world::World &world,
		const def::DefItemsFile *items) {
	// The selected row survives the refill by its index: the last row in
	// state 3, else row 0 [orig: @0x5638E3..0x563900].
	const int selected_index = selected_row_ >= 0 ? selected_row_ : 0;
	rows_.clear(); // CListWnd_RemoveRow(-1) @0x56390F
	selected_row_ = -1;
	// The local player's ground entity, the bay it stands on; with none the
	// list stays empty and the selection keeps its value [orig: @0x563919..0x563922].
	const world::Entity *player = world.registry.get(world.cached.local_player);
	const world::Entity *ground = player != nullptr ? world.registry.get(player->ground_target)
	                                                : nullptr;
	if (ground == nullptr) return;
	const replication::ClientVehicleSpawnAvailability &availability =
			rt.state().vehicle_spawn_availability;
	const std::vector<VehicleSpawnGroup> groups =
			items != nullptr ? vehicle_spawn_group_table(*items) : std::vector<VehicleSpawnGroup>{};
	int added = 0;
	for (size_t slot = 0; slot < groups.size(); ++slot) {
		const int32_t type = groups[slot].item_id; // [esi-8] @0x563948
		if (type == 0) continue;                   // @0x563951
		// The bay's pcvehicle_spawnlist bit, then the reply's flag and count
		// [orig: `test [eax+0AD4h], ebp` @0x563963, @0x56396F, @0x56397B..0x563983].
		const bool offered = std::find(ground->vehicle_spawn_groups.begin(),
				ground->vehicle_spawn_groups.end(),
				static_cast<uint8_t>(slot)) != ground->vehicle_spawn_groups.end();
		if (!offered || !availability.received || availability.rows.empty()) continue;
		// Every row is read, the last match standing [orig: @0x563990..0x5639A2].
		int available = 0;
		int max_count = 0;
		for (const VehicleSpawnAvailabilityRow &row : availability.rows) {
			if (type == static_cast<int32_t>(row.type_id)) {
				available = row.available;
				max_count = row.max_count;
			}
		}
		// A group with neither count is not listed [orig: @0x5639A4..0x5639AA].
		if (available == 0 && max_count == 0) continue;
		char available_text[32];
		char max_text[32];
		char line[320];
		if (available == kVehicleSpawnUnlimited) // @0x5639DB..0x5639DD
			std::snprintf(available_text, sizeof(available_text), "inf");
		else
			std::snprintf(available_text, sizeof(available_text), "%d", available); // @0x5639BB
		if (max_count == kVehicleSpawnUnlimited) // @0x5639C9..0x5639CB
			std::snprintf(max_text, sizeof(max_text), "inf");
		else
			std::snprintf(max_text, sizeof(max_text), "%d", max_count); // @0x5639F2
		if (available == kVehicleSpawnUnlimited) // @0x563A00..0x563A16
			std::snprintf(line, sizeof(line), "%s   (%s)", groups[slot].name.c_str(), available_text);
		else // @0x563A39
			std::snprintf(line, sizeof(line), "%s   (%s/%s)", groups[slot].name.c_str(),
					available_text, max_text);
		rows_.push_back({line, static_cast<int32_t>(slot)}); // UIList_AddRow @0x563A51
		++added;
	}
	// The kept index re-selected when it is still a row [orig:
	// CListWnd_SetRowVisibility @0x563A78 (@0x645190: out of range selects
	// nothing)]; the selection is that row's value, 0 when the index is past
	// the rows, -1 with none listed [orig: CListWnd_GetRowValue @0x563A90
	// (@0x6445F0: 0 out of range), the stores @0x563A95 / @0x563A9C].
	if (selected_index < static_cast<int>(rows_.size())) selected_row_ = selected_index;
	if (added != 0)
		selection_ = selected_index < static_cast<int>(rows_.size()) ? rows_[selected_index].value : 0;
	else
		selection_ = -1;
}

void VehicleMenu::open(ClientRuntime &rt, const world::World &world,
		const def::DefItemsFile *items) {
	// The init event: the screen's first parse into the scene. A later open
	// finds the file's CRC and parses nothing, and the close event that disarms
	// the poll is the scene's destroy, so the first parse and an unarmed poll
	// are one test here.
	// [orig: VehicleMenu_Open @0x564240 -- populate @0x56424E when g_VehicleMenuOpen
	//  is 0, armed @0x564253; the flag clear @0x56425D, NetPacket_SendEmptyType42
	//  @0x564267]
	if (!poll_armed_) {
		populate(rt, world, items);
		poll_armed_ = true;
		rt.clear_vehicle_spawn_availability_received();
		(void)rt.queue_vehicle_spawn_availability_request();
	}
	// The activate event, every open: the selection forgotten, the list
	// refilled [orig: VehicleMenu_OnActivate @0x563AD0 -- the store @0x563AD0, the populate].
	selection_ = -1;
	populate(rt, world, items);
}

void VehicleMenu::poll(ClientRuntime &rt, const world::World &world,
		const def::DefItemsFile *items) {
	// [orig: VehicleMenu_PollAvailability @0x563AF0 -- the armed test @0x563AF0..0x563AF7,
	//  ++ @0x563AF9..0x563AFE, `test al, 3Fh` @0x563B08, the 0x42 @0x563B0A]
	if (!poll_armed_) return;
	++poll_counter_;
	if ((poll_counter_ & 0x3F) != 0) return;
	(void)rt.queue_vehicle_spawn_availability_request();
	// With a reply in, the list refills; with none yet the counter steps back
	// so the next frame asks again [orig: @0x563B0F..0x563B18, the -1 @0x563B1D].
	if (rt.state().vehicle_spawn_availability.received)
		populate(rt, world, items);
	else
		poll_counter_ = -1;
}

void VehicleMenu::select_row(int row) {
	// The pick's event carries the row's value [orig: UI_OnItemListSelectionChanged
	// @0x563B30 -- `cmp 5000001h`, the stricmp "ITEM_LIST", the store of
	// [eventData+10h] @0x563B57]; the table selects the row it names.
	if (row < 0 || row >= static_cast<int>(rows_.size())) return;
	selected_row_ = row;
	selection_ = rows_[row].value;
}

bool VehicleMenu::click(ClientRuntime &rt, const world::World &world, bool cancel) {
	// [orig: the VEHICLE button callback @0x563B60 -- Input_ResetAllState @0x563B60
	//  (the embedder's close resets input), the user-arg test @0x563B65]
	bool sent = false;
	if (!cancel) {
		// The local player's ground entity, its def and the def's VEHICLEBAY bit
		// [orig: @0x563B6C..0x563B87: g_LocalPlayerEntity, +0x28, +0x20, `test byte
		//  [ecx+58h], 1`], then the selection [orig: @0x563B8A..0x563B93].
		const world::Entity *player = world.registry.get(world.cached.local_player);
		const world::Entity *ground =
				player != nullptr ? world.registry.get(player->ground_target) : nullptr;
		if (ground != nullptr && ground->has_item_def &&
				(ground->item_attrib2 & def::DEF_ITEM_ATTRIB2_VEHICLEBAY) != 0 &&
				selection_ != -1) {
			// The bay's handle from its pool and index [orig: @0x563B97..0x563BB1,
			//  @0x563BE8..0x563C00], then NetPacket_SendWeaponSelectType40 @0x563BCE.
			sent = rt.queue_vehicle_spawn_request(player->ground_target.packed,
					static_cast<uint8_t>(selection_));
		}
	}
	// Either button closes the screen: g_VehicleScreenOpen cleared @0x563BD9 and
	// Game_CloseInGameScreens @0x563BE3 (the embedder's).
	return sent;
}

} // namespace opennova::inmatch
