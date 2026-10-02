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
struct Entity;
struct EntityHandle;
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

// THE JOIN/LEAVE LINES — S2C 0x32 (npwire FormattedGameText), reliable, to
// every OTHER in-match connection (the listen host's loopback included; the
// joiner's own slot is not in-game yet, the leaver's is going): subtype 1
// {name, team} from the player add, ahead of its 0x46 push, and subtype 2
// {name, team} from the disconnect, before the leaver's entity is torn down.
// The name is the slot's own (the ClientAuth NA the reply echoes); the team
// the slot's live entity's, else the reserved assignment. The squadron pair
// 3/4 beside them needs a NovaWorld clan-registry node, which an opennova
// host never keeps (LAN accounts are 0).
// [orig: Server_PlayerAdd @0x51d213..0x51d291 (send_mask 0x80, [u8 1][cstr
//  slot+0x28][u8 slot+0x1A0]; subtype 3 @0x51d174..0x51d20e);
//  Server_HandlePlayerDisconnect @0x51b69b..0x51b6d3 (send_mask 0x80,
//  NetPacket_SerializeMinimapSlot_0(.., 2); subtype 4 @0x51b6d8..0x51b781)]
void broadcast_player_joined_text(std::vector<NapiNPConnection> &roster,
		const NapiNPConnection &joined, const world::World *world);
void broadcast_player_leaving_text(std::vector<NapiNPConnection> &roster,
		const NapiNPConnection &leaver, const world::World *world);

// The co-op dialog line: when the authority's dialog playback loads line
// `line` of dialog `dialog_name` (the entry index before it advances), S2C
// 0x28 [cstr name][i16 line] goes reliable to every in-match slot but the
// listen host (mask 0x90). The embedder's dialog playback reports the line.
// [orig: Dialog_UpdatePlayback @0x44E470 — Dialog_LoadAudioClip @0x44e599 then
//  Server_SendEntityStateToAll(dialog+4, entry) @0x44e5a5; Server_SendEntityStateToAll
//  @0x50A0D0 — the authority @0x50A0D7, mask 0x90 @0x50A0ED, SendFiltered(0x28, 1)
//  @0x50A115]
void Server_BroadcastDialogLine(NapiNPServerCtx &ctx, const std::string &dialog_name,
		int16_t line);

// The nearest type-2044 location marker whose radius contains `sender` (2-D
// distance), by the marker's spawn-order index (the marker's +0x280 word), -1
// when none. The chat location tag and the radio call's location word share
// the walk. [orig: @0x51394C..0x5139D9 (chat) and NapiNPServerMsg_HandleRadioCall
// @0x5143fb..0x514494: pool 3, def type 2044, sqrt(dx^2 + dy^2) < entity+0
// (the marker bound), nearest wins; the label is g_LocationNames[64 * entity+640]]
int nearest_location_index(const world::World &world, const world::Entity &sender);

// Entity_FindChildByDefType(entity, 1, 0) walks the groundEntity chain (at most
// 20 links) for the LAST link whose def type is 1 (a vehicle) — the carrier
// the entity rides or stands on; invalid when none. [orig: @0x43BEA0]
world::EntityHandle carrier_vehicle(const world::World &world, const world::Entity &e);

// The tag strip [orig: @0x513852..0x5138B6]: a '<' closes the copy, a '>'
// reopens it; a stray '>' while open is kept.
std::string chat_strip_angle_tags(const std::string &text);

// [orig: sub_4FD650 @0x4FD650 -> the three-int text parse]: true when the text
// scans as three integers with any of them nonzero.
bool chat_text_is_int_triplet(const std::string &text);

} // namespace opennova::inmatch
