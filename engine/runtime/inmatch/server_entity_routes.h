#pragma once

// The host tick's routes for the records the world produced: the item and
// removal events with the HUD relays, the water-surface crossings and the
// guided-round updates. Server_TickUpdate orders them (server_tick.cpp). Beside
// them, the door record's other S2C 0x37 leg: the answer to a client's request.

#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// Item callbacks' state packets, explosion effects, authoritative removals
// and the HUD relays, fanned to the active remote slots, then released.
void Server_FanEntityEvents(NapiNPServerCtx &ctx, world::World &world);
// The water-surface crossings the motor recorded, one S2C 0x34 each to every
// in-match player, dead or alive (the authority's own as a local slot sound
// through its loopback, single player included), then released.
void Server_RouteWaterCrossings(NapiNPServerCtx &ctx, world::World &world);
// The guided-round updates, queued behind the tick's 0x0A so each follows the
// frame that carried its round's birth.
void Server_RouteGuidance(NapiNPServerCtx &ctx, world::World &world);

// The host side of a client's door request (C2S 0x1A -> S2C 0x37). The sender
// must hold a player slot; the body [u16 handle][i16 value][u8 number] (zero
// defaults) addresses record (first + number - 1) of the handle's pool row,
// gated on a def, a non-negative index, a non-zero number and number <= the
// def's door count. A closed or closing record (0/3) opens (1) only for value
// 1; an opening or open record (1/2) takes the value only when number is 3,
// literally. The record's resulting state goes back to the sender alone
// (mask 0x30) as S2C 0x37 {handle, state, number}; nothing else moves (no
// sound, no phase: the host's door tick carries an opening record on).
// [orig: NapiNPServerMsg_HandleDoorSlotRequest @0x514B20 (ex NapiNPServerMsg_HandleVoteUpdate) — the
//  authority @0x514b24, the player slot @0x514b31..0x514b40, the reads
//  @0x514b5f..0x514b91, the pool/def/index gates @0x514ba8..0x514c08, the
//  record switch @0x514c20..0x514c35, the reply NetPacket_WriteTwoShortsAndByte
//  @0x514c6f, SendFiltered(0x37, mask 48) @0x514c46..0x514c74]
std::vector<ProtocolMessage> Server_HandleDoorRowRequest(const NapiNPConnection &sender,
		const std::vector<uint8_t> &payload, world::World &world);

} // namespace opennova::inmatch
