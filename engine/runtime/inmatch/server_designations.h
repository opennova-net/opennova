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
// << 16 and `mode`. The mode-1 arm's S2C 0x1E notice (a designator round's
// registration, RoundData_SpawnRound @0x4ec270) is not ported with this
// function: no port caller registers mode 1.
// [orig: EntityTracker_RegisterOrUpdate @0x511580 — the owner scan
//  @0x51158d..0x5115a5, the free scan @0x5115a9..0x5115c9, the stores
//  @0x511656..0x511676]
void Server_RegisterDesignation(ServerDesignationTable &table, world::EntityHandle owner,
		const int32_t point[3], int32_t ticks, int32_t radius_units, uint8_t mode);

// One server tick: a positive life counts down, and the row whose life just
// reached zero drops its owner (freeing it). A row registered with no life
// keeps its owner until the round init clears the table.
// [orig: Server_TickUpdate @0x51e496..0x51e4ba, the tail after the send pump]
void Server_TickDesignations(ServerDesignationTable &table);

// The recipient's S2C 0x6B: every row with a life whose owner is on `team`,
// in table order, as [u16 handle][s16 x][s16 y][s16 z][u16 seconds][u8 mode]
// [u8 radius] (whole units toward zero, seconds = ticks / 62 toward zero); an
// empty batch sends nothing. Unreliable (msgClass 0). The overlay build runs
// it first on each of its 14-tick visits; the team is the recipient slot's.
// [orig: Server_SendDesignationsToPlayer @0x517F70 (ex
//  Server_SendOverlayStateToPlayer; mask 0x20, SendFiltered(0x6B, 0, 1) only
//  when the length is positive) -> NetPacket_SerializeDesignations @0x5116A0
//  (ex NetPacket_SerializeWeaponOverlaySlots; the life gate @0x5116c1, the
//  owner gate @0x5116cf, the team gate entity+354 @0x5116e3); called by
//  Server_BuildOverlayStateForPlayer @0x518002]
void Server_SendDesignationsToPlayer(NapiNPConnection &conn, const ServerDesignationTable &table,
		const world::World &world);

} // namespace opennova::inmatch
