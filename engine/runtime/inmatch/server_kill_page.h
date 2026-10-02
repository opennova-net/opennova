#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// Whether the pool row `handle` belongs on a joiner's kill page: a row with an
// item def (a nonzero type index and a def) whose last S2C 0x26 left inside
// [window_min, window_max] (the host's GetTickCount stamp, entity+560), not a
// player, and dead (Flags 0x2 or 0x4, else health at or below zero).
// [orig: NetSync_IsEntityEligibleInWindow @0x507AA0 — the handle gates
//  @0x507AA8..0x507AE2, the window @0x507AE7..0x507AF7, +0x1C / +0x20
//  @0x507AF9..0x507AFF, Flags @0x507B05..0x507B1E, Health +0x11E @0x507AE6]
bool kill_page_entity_eligible(const world::World &world, uint32_t window_min,
		uint32_t window_max, uint16_t handle);

// The S2C 0x4E kill page from `start`: pools 0, 1 and 2 walked in handle
// order from `start` to each pool's used count, a pool past its end handing
// over to the next (pool 0 -> 0x1000 -> 0x2000; the begin hands over without
// testing the next pool for rows); every eligible handle is written after
// the leading word, at most 33 of them, and the leading word is the handle
// the walk stopped at (0xFFFF when it ran out). Empty (nothing sent) while
// spawns are suspended or the round is over, or when the walk has no first
// handle. The used count is the pool's live high-water slot + 1, the model the
// 0x10 builder streams by.
// [orig: Server_CollectValidWeaponSlots @0x516000 (the gates @0x51602E, the
//  begin @0x516048..0x516057, the loop @0x516090..0x5160D1, the leading word
//  @0x5160D7); WeaponLoadout_IteratorBegin @0x501680; ItemPoolIterator_Advance
//  @0x501740; Pool_GetUsedCount @0x441F80]
std::vector<uint8_t> Server_CollectKillPage(const world::World &world, bool spawns_held,
		uint32_t window_min, uint32_t window_max, uint16_t start);

// The host side of C2S 0x28 [u32 window_min][u32 window_max][u16 start] (a
// short read is 0): on the authority, for a sender holding a player slot, the
// page from `start`, sent reliable to the sender alone while it is in the
// match (mask 0xA0) when it is not empty. The round-over latch stands for the
// spawn gates. [orig: NapiNPServerMsg_HandleWeaponLoadoutRequest @0x51A550 —
//  the authority @0x51A557, the slot @0x51A561..0x51A578, the reads
//  @0x51A58C..0x51A5B0, the page @0x51A5C0, the send @0x51A5D1..0x51A5F4]
std::vector<ProtocolMessage> Server_HandleKillPageRequest(const NapiNPServerCtx *ctx,
		const NapiNPConnection &conn, const std::vector<uint8_t> &payload,
		const world::World *world);

} // namespace opennova::inmatch
