#pragma once

// The host's designation table: the marks a radio call's target (and, in
// retail, a designator round) leave on the recipient team's maps. Each of the
// 251 rows holds an owner, a Q16 point, a remaining life in server ticks, a
// Q16 radius and a mode byte; every server tick ages the lives, a round init
// clears the table, and the per-player overlay build sends the recipient
// team's live rows as one S2C 0x6B batch. See docs/interface/hud-re.md
// "The MP legs" and docs/net/novaworld-net-re.md (S2C 0x6B).
// [orig: g_ServerDesignations @0xC84810 (ex stru_C84810), 251 x 28 B]

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <runtime/world/entity.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

struct NapiNPConnection;

// One row [orig: +0 the owner entity, +4/+8/+12 the point, +16 the remaining
// ticks, +20 the radius << 16, +24 the mode byte].
struct ServerDesignation {
	world::EntityHandle owner;
	int32_t point[3] = {};
	int32_t remaining_ticks = 0;
	int32_t radius_q16 = 0;
	uint8_t mode = 0;
};
inline constexpr size_t kServerDesignationCount = 251;
using ServerDesignationTable = std::array<ServerDesignation, kServerDesignationCount>;

// Register `owner`'s mark, or refresh the row it already holds: the owner's
// row first, else the first free row (no life and no owner); with neither the
// mark is dropped. The row takes the owner, the point, `ticks`, `radius_units`
// << 16 and `mode`. True when the mode-1 notice is due: a mode-1 mark on a
// fresh row or on the owner's row while its mode was not 1 (the caller sends
// it, Server_RouteDesignationMarks). A full table reads a null row's mode in
// retail (an original crash, no stock wire effect); ours drops the mark.
// [orig: EntityTracker_RegisterOrUpdate @0x511580 — the owner scan
//  @0x51158d..0x5115a5, the free scan @0x5115a9..0x5115c9, the mode-1 tests
//  @0x5115D2 / @0x51168D and the row's mode @0x5115D4 (the fresh test
//  @0x5115DA..0x5115DC), the stores
//  @0x511656..0x511676]
bool Server_RegisterDesignation(ServerDesignationTable &table, world::EntityHandle owner,
		const int32_t point[3], int32_t ticks, int32_t radius_units, uint8_t mode);

// The world's designator marks (WorldOutbox::designation_marks), in the order
// the rounds spawned: each registers its firer's mode-1 row, and a due notice
// sends S2C 0x1E [0x30][the firer's pool-0 index][0xFF 0xFF][0 0 0 0] to every
// state-6/7 slot whose own team byte is the firer entity's team (mask 0x180).
// [orig: EntityTracker_RegisterOrUpdate @0x511580 - the filter entity+354
//  @0x5115E8, mask 384 @0x5115ED, Pool_GetIndexFromPtr @0x511608, 48 @0x511618,
//  0xFFFF @0x51161F, 0 @0x511632, SendFiltered(0x1E, 1, 0, 8) @0x511645]
void Server_RouteDesignationMarks(std::vector<NapiNPConnection> &roster,
		ServerDesignationTable &table, world::World &world);

// One server tick: a positive life counts down, and the row whose life just
// reached zero drops its owner (freeing it). A row registered with no life
// keeps its owner until the round init clears the table.
// [orig: Server_TickUpdate @0x51e496..0x51e4ba, the tail after the send pump]
void Server_TickDesignations(ServerDesignationTable &table);

// The recipient's S2C 0x6B: every row with a life whose owner is on `team`,
// in table order, as [u16 handle][s16 x][s16 y][s16 z][u16 seconds][u8 mode]
// [u8 radius] (whole units toward zero, seconds = ticks / 62 toward zero); an
// empty batch sends nothing. Unreliable (msgClass 0). The overlay build runs
// it first on each of its 14-tick visits; the team is the recipient slot's
// (+0x1A0, zero-extended) against the owner entity's (+0x162, sign-extended).
// [orig: Server_SendDesignationsToPlayer @0x517F70 (ex
//  Server_SendOverlayStateToPlayer; mask 0x20, SendFiltered(0x6B, 0, 1) only
//  when the length is positive) -> NetPacket_SerializeDesignations @0x5116A0
//  (ex NetPacket_SerializeWeaponOverlaySlots; the life gate @0x5116c1, the
//  owner gate @0x5116cf, the team gate entity+354 @0x5116e3); called by
//  Server_BuildOverlayStateForPlayer @0x518002]
void Server_SendDesignationsToPlayer(NapiNPConnection &conn, const ServerDesignationTable &table,
		const world::World &world);

} // namespace opennova::inmatch
