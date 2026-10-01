#pragma once

#include <vector>

#include <net/npwire/emote_wire.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_connection.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The host side of an emote (C2S 0x14 -> S2C 0x2D). The sender must hold a
// live, non-spectator slot whose player is alive and whose emote cooldown has
// run out; the broadcast {emote, the sender's pool-0 index} goes to every
// in-match slot whose player stands within 100 units of the sender on every
// axis, the sender included, and the cooldown re-arms to 2 seconds. The
// sender's own copy is returned as the reply (the dispatcher frames it with
// the request); every other recipient is staged on its transport.
// [orig: NapiNPServerMsg_HandleEmoteRequest @0x501E00 — authority
//  @0x501e0c, the player and slot @0x501e16..0x501e2d, the spectator latch
//  +100567 @0x501e33, the entity @0x501e40, the dead bit and the +376
//  cooldown @0x501e55, the body @0x501e70..0x501e92, the recipients (slot
//  active, NetPlayer state 10/11, |d| <= 6553600 per axis) @0x501eb3..0x501f14,
//  SendFiltered(0x2D, msgClass 0) @0x501f38, the re-arm @0x501f53]
std::vector<ProtocolMessage> Server_HandleEmoteRequest(NapiNPConnection &sender,
		const EmoteRequest &request, std::vector<NapiNPConnection> &roster,
		const world::World &world);

} // namespace opennova::inmatch
