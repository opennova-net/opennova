#pragma once

// The command map's USER WAYPOINTS: the pool-4 "user waypoint" entries
// (items.def 106089, type id 6089) the bit-17 map leg labels, and the CMAP
// screen's own table of the waypoints this player placed. A waypoint is a
// plain pool-4 row: its position (the Z re-sampled from the terrain), its
// name, its owner and team 0. The table holds up to 16 of them with one hover
// byte each; the CMAP render places its delete button beside the hovered one.
// Witness record: docs/interface/hud-re.md "The command map" (D-HUD-19).
// [orig: Waypoint_CreateForPlayer @0x4dfcb0; the table g_ConnectionSlotTable
//  @0x252dcd0 (16 x {entity*, hover byte}), its count dword_252DD50 — both
//  IDB names are misnomers]

#include <runtime/world/entity.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

class World;

// The items.def type id Waypoint_CreateForPlayer resolves.
// [orig: ItemList_FindIndexByTypeId(6089) @0x4dfcf0]
inline constexpr int32_t kUserWaypointTypeId = 6089;
// The pool the waypoints live in [orig: Pool_AllocEntry(4, 1) @0x4dfdf4].
inline constexpr int kUserWaypointPool = 4;
// The pool-4 name buffer (entity+244 up to the +288 dword).
inline constexpr size_t kUserWaypointNameMax = 43;

// Creates one waypoint for `owner` at (x, y): every pool-4 row with a def,
// the same owner, the same x and y dwords and the same name
// (case-insensitive) is wiped first (a plain memset — no destroy callback,
// and the placed-waypoint table is not touched), then a new row is allocated
// at the first free slot with the def's ordinal, x and y exactly as given,
// the terrain height at (x, y), the name ("Unknown" when null) and the owner.
// `received_line` gets the SYSTEM-ring line "Waypoint \"%s\" received from
// %s." when the owner is not the local player (empty otherwise). Returns the
// new row (invalid when pool 4 is full).
// [orig: Waypoint_CreateForPlayer @0x4dfcb0 — the dedupe walk
//  @0x4dfd96..0x4dfdee, Pool_AllocEntry(4, 1) @0x4dfdf4 (the first row whose
//  +0x1C is zero, Pool_AllocEntry @0x442230), the line @0x4dfe13..0x4dfe43
//  (Chat_AddMessageChannel2(msg, -1, 930))]
EntityHandle waypoint_create_for_player(World &world, int32_t x, int32_t y, const char *name,
		EntityHandle owner, std::string &received_line);

// Entity_Destroy on one pool row, whatever it holds (an empty row is left
// alone) [orig: j_Entity_Destroy @0x4dbcb0].
void destroy_pool_row(World &world, EntityHandle handle);

// One placed-waypoint table entry read the way retail reads its stored row
// pointer: whatever the row now holds, with no liveness test. A wiped or
// destroyed row reads zeroed (0, 0, 0, ""), and a row the allocator re-took
// reads its new occupant. The table itself never learns of a wipe, so the
// entry keeps its slot, counts toward the 16 and still sends its C2S 0x4F.
// [orig: Server_BroadcastChatToAllPlayers @0x549200 (entity +4 / +8 / +12 /
//  +244 through NetPacket_WriteTypeNameAndPosition @0x42b160),
//  CMapWindow_HandleEvent @0x549dd7 / @0x549ba0 (entity +4); the only
//  table writers are the append, the delete compaction and the clear;
//  Entity_Destroy's memset @0x43ea70]
struct UserWaypointRow {
	int32_t x = 0; // +4, 16.16
	int32_t y = 0; // +8, 16.16
	int32_t z = 0; // +12, 16.16
	std::string name; // +244
};
UserWaypointRow user_waypoint_row(const World &world, EntityHandle handle);

// The CMAP screen's placed-waypoint table.
struct UserWaypointTable {
	static constexpr int kCapacity = 16;
	struct Entry {
		EntityHandle handle; // the pool row the waypoint was created in
		bool hover = false;
	};
	std::array<Entry, kCapacity> entries{};
	int count = 0;

	// The whole table and its count zeroed [orig: sub_54B280 @0x54b2f8 on the
	// screen's first load; sub_5491b0 from the PreMenu state exit @0x5688b6].
	void clear() {
		entries = {};
		count = 0;
	}
	// Append after a successful create, hover off [orig:
	// CMap_HandleWaypointCreateConfirm @0x54a1c3..0x54a1e5].
	bool append(EntityHandle handle);
	// The first hovered live entry's index, -1 when none [orig:
	// CMap_DestroyFirstActiveLoadingEntity @0x5479b0..0x5479d3 — the entry, its
	// hover byte and the bound delete button].
	int first_hovered() const;
	// Drop entry `index` and shift the tail down one (the vacated last slot
	// zeroed), count - 1 [orig: @0x5479f3..0x547a37].
	void remove_at(int index);
};

// THE CMAP SCREEN'S WORLD HALVES (inmatch ClientRuntime adds each one's
// C2S send). The confirm: with fewer than 16 placed, the name cut to 31
// characters, a waypoint for the local player at (x, y), appended to the
// table; the new row, invalid when nothing was placed.
// [orig: CMap_HandleWaypointCreateConfirm @0x54a0d0 — the char[32] name with
//  [31] = 0 @0x54a126, `dword_252DD50 < 16` @0x54a1a0, the append
//  @0x54a1c3..0x54a1e5]
EntityHandle place_user_waypoint(World &world, int32_t x, int32_t y, const std::string &name);
// The delete button: the first hovered placed waypoint destroyed and removed
// from the table; its row, invalid when none is hovered.
// [orig: CMap_DestroyFirstActiveLoadingEntity @0x547990 — the count gate
//  @0x5479a5, j_Entity_Destroy @0x5479ee, the compaction @0x5479f3..0x547a37]
EntityHandle delete_hovered_user_waypoint(World &world);
// CLEAR_WAYPOINTS: every placed waypoint destroyed, the table cleared;
// `removed` gets each row in table order.
// [orig: Server_DestroyBatchedEntities @0x548110 (a misnomer) — the count
//  gate @0x548121, the walk @0x548130..0x548150, the memset @0x54815e]
void clear_user_waypoints(World &world, std::vector<EntityHandle> &removed);

} // namespace opennova::world
