// The message-feed lane a client folds: S2C 0x1E carries one 8-byte game event
// (kill, killer-less death, medic, objective/camp) that the original turns into
// a canned "Canned Msg" sentence and posts to the on-screen feed
// [orig: NapiNPClientMsg_GameEvent -> NetPacket_HandleGameEvent @0x426270 ->
//  HUD_FormatKillEventMessage @0x422DA0 -> Chat_FormatMessage @0x422C60 ->
//  Chat_AddDebugMessage @0x4987F0].
//
// This TU folds the WIRE half only: the record, its witnessed classification,
// and the two conventions a consumer cannot recover on its own —
//   * a SelfDeath's victim/aux slots are LITERAL ZERO on the wire
//     [orig: GameEvent_PlayerDeath @0x516DD0 leaves v41/v42 = 0], so they are
//     normalized to "none" here; reading them charges the death to entity 0
//     (the host);
//   * camp events 59/60 reuse the slots with different meaning — attacker is
//     the LEVEL index (whose WPNames key is built from index + 1) and victim
//     is the TEAM (1 = blue, 2 = red) [orig: case 59 @0x4272D7 / case 60
//     @0x4273DC], and retail emits for team 1/2 only (no else branch).
// String resolution ($A/$B against the roster, the STRCND lookup, the team
// suffix) happens in the HUD feed model where the name table lives.

#include "netsim/client_replica_pipeline.h"

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::netsim {

void ClientReplicaPipeline::apply_game_event(const std::vector<uint8_t> &body) {
	GameEventRecord rec;
	size_t consumed = 0;
	if (!decode_game_event(body.data(), body.size(), rec, consumed)) {
		++malformed_bodies_;
		return;
	}

	ClientGameEvent ev;
	ev.event_type = rec.event_type;
	ev.attacker_index = rec.attacker_index;
	ev.victim_index = rec.victim_index;
	ev.aux_index = rec.aux_index;
	ev.pos_x = rec.pos_x;
	ev.pos_y = rec.pos_y;
	const GameEventKind kind = game_event_kind(rec.event_type);
	ev.kind = static_cast<uint8_t>(kind);

	// A killer-less death carries zeroed victim/aux slots — normalize them to
	// "none" so no consumer can mistake slot 0 for a real actor.
	if (kind == GameEventKind::SelfDeath) {
		ev.victim_index = 0xFF;
		ev.aux_index = 0xFF;
	}

	pending_game_events_.push_back(ev);
}

// THE PLAYER-CHAT LANE (S2C 0x14): [channel][sender_slot][cstr], the order
// D-NET-215 settled. The fold carries the record; the ring/colour routing is
// the HUD channel table and the sender gate is the roster's, both at the
// embedder [orig: NapiNPClientMsg_ChatMessage @0x42f240 tail-jumps into
//  Chat_DispatchToChannel(body[1], (char)body[0], &body[2]) @0x42b910].
void ClientReplicaPipeline::apply_chat_broadcast(const std::vector<uint8_t> &body) {
	ChatBroadcast rec;
	if (!decode_chat_broadcast(body.data(), body.size(), rec)) {
		++malformed_bodies_;
		return;
	}
	ClientChatLine line;
	line.channel = rec.channel;
	line.sender_slot = rec.sender_slot;
	line.text = rec.text;
	pending_chat_lines_.push_back(std::move(line));
}

} // namespace opennova::netsim
