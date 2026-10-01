#pragma once

// The host side of the visible-players table: the S2C 0x4C snapshot a C2S
// 0x23 request is answered with, and the S2C 0x4D slot notice every in-game
// client receives when a player joins (the notice makes the other clients
// re-request the snapshot, so each client's table follows the roster).
// See docs/net/novaworld-net-re.md and docs/interface/hud-re.md "The MP legs".

#include <cstdint>
#include <vector>

#include <runtime/inmatch/napi_np_connection.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The snapshot for `requester`: one {slot id, entity handle} per roster slot
// in slot order that is live (bound to an entity), not the host's own slot
// unless the host is a session peer, not a spectator, and on the requester's
// team or the requester itself. A requester spawned outside a team game
// carries team 255, so its snapshot holds itself alone. An inactive requester
// gets an empty snapshot (count 0).
// [orig: NetPacket_SerializeVisiblePlayersSnapshot @0x506320 — the requester
//  slot +4 gate @0x50635a, its team slot+0x1A0 @0x506372 forced to 255 by the
//  entity AI record's 0x200 bit @0x50637c..0x50638c (set at spawn outside the
//  team types, Entity_SpawnFromAnimSlotProperty @0x43c546..0x43c54f), the
//  per-slot gates @0x5063e0..0x506419 (+4 active, +0 entity, `!+5 ||
//  is_mp_session_peer`, `!+96481 || !+97536` — +96481 is never set — and
//  `!+100567`), the team / self / +96481 admit @0x506440, the entry slot+0x14
//  and packed handle @0x506445..0x506490]
std::vector<uint8_t> build_visible_players_snapshot(const NapiNPConnection &requester,
		const std::vector<NapiNPConnection> &roster, const world::World *world,
		uint32_t game_type, bool mp_session_peer);

// The join notice: S2C 0x4D {the joined slot id}, reliable, to every in-game
// connection. Retail's one send reaches the joined slot too (it entered state
// 6 first); that copy rides this runtime's game-start bundle, so the fan
// covers every OTHER in-match connection, the listen host's loopback included.
// [orig: Server_OnPlayerJoin @0x51a93f..0x51a97a — the slot byte slot+0x14,
//  send_mask 0x80 = every slot in state 6/7 (NapiNPServer_SendFiltered
//  @0x4c893e..0x4c8953); the slot entered state 6 @0x51a712]
void fan_spawn_slot_notice(std::vector<NapiNPConnection> &roster, const NapiNPConnection &joined);

} // namespace opennova::inmatch
