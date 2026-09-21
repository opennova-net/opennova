#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <net/npwire/ingame_decode.h>   // ChatUplink
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The host side of player chat (C2S 0x0D -> S2C 0x14 per recipient). The
// sender must be an in-match (or the local) slot that is not a spectator;
// one message per 1000 ms per sender; `<...>` tags are stripped; channels
// {1, 2, 4, 5, 11, 12, 13} are prefixed "name[:[location]]: " and fanned by
// the channel's recipient predicate (2 team / 4 side B / 5 side A / 11 same
// carrier / 12 same squad / 13 within 100 units per axis / 1 everyone); any
// other channel is dropped. A text that parses as three integers (any
// nonzero) reaches only the sender. Recipients other than the sender are
// staged on their transports; the sender's own copy is returned as the reply
// so the dispatcher frames it with the request. Squads are not modeled on
// this host, so the squad name suffix and channel 12 fan are empty.
// [orig: NapiNPServer_HandleChatMessage @0x513760]
std::vector<ProtocolMessage> Server_HandleChatMessage(NapiNPServerCtx &ctx,
		NapiNPConnection &sender, const ChatUplink &uplink, uint32_t now_ms,
		world::World &world);

// A downed player's manual medic call (C2S 0x2E). The body is never read: the
// requester is the connection's own player. In order: the null GameText
// lookup no-ops the whole handler; the slot must be a live-roster player with
// an armed revive window (`!slot+100567 && slot+368 > 0`); the requester
// leaves its spawn-wave group; the message is formatted from the slot name; a
// manual-preference, not-yet-latched requester publishes the live window (no
// bit 7) to the 0x580 medic set; the chat goes reliably to that set minus the
// requester, then to the requester alone (the chat is NOT preference-gated);
// the once-only latch closes; and the MEDIC_REQUEST composite sound fans to
// alive players. The listen host's own player runs this handler too.
// [orig: Server_BroadcastMedicRequest @0x515390 — the GameText lookup
//  @0x5153C9, gates @0x515406, SpawnWaveList_RemovePlayer @0x515412, sprintf
//  @0x515421, 0x54 @0x515432..0x515484, 0x14 @0x5154AC..0x51550B, latch
//  @0x515510, sound @0x515519..0x51552F]
std::vector<ProtocolMessage> Server_HandleMedicRequest(const std::string *format,
		NapiNPConnection &conn, std::vector<NapiNPConnection> &roster, world::World *world);

// The tag strip [orig: @0x513852..0x5138B6]: a '<' closes the copy, a '>'
// reopens it; a stray '>' while open is kept.
std::string chat_strip_angle_tags(const std::string &text);

// [orig: sub_4FD650 @0x4FD650 -> the three-int text parse]: true when the text
// scans as three integers with any of them nonzero.
bool chat_text_is_int_triplet(const std::string &text);

} // namespace opennova::inmatch
