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

#include <runtime/replication/client_replica_pipeline.h>

#include <runtime/hud/feed_format.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>

#include <base/io/strutil.h>
#include <base/io/byte_reader.h>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::replication {

// [orig: NapiNPClientMsg_HandleTextCommand @ 0x429E70, SETCEASEFIRE branch @ 0x429F1F]
void ClientReplicaPipeline::apply_text_command(const std::vector<uint8_t> &body) {
    const auto end = std::find(body.begin(), body.end(), uint8_t{0});
    std::istringstream input(std::string(body.begin(), end));
    std::string command, value;
    if (!(input >> std::quoted(command) >> std::quoted(value))) return;
    if (strutil::iequals(command, "SETCEASEFIRE")) {
        state_.cease_fire = std::strtol(value.c_str(), nullptr, 10) != 0;
        state_.mark_changed();
    }
}

// This byte-only handler intentionally zero-fills short reads, just as
// retail does; even an empty body first restores the all-allowed table.
// [orig: NapiNPClientMsg_HandleWeaponRestrictions @0x42D4C0]
void ClientReplicaPipeline::apply_weapon_restrictions(const std::vector<uint8_t> &body) {
	state_.weapon_availability.fill(1);
	io::ByteReader reader(body.data(), body.size());
	const uint8_t count = reader.read_u8();
	for (unsigned i = 0; i < count; ++i) {
		const uint8_t index = reader.read_u8();
		const uint8_t value = reader.read_u8();
		if (index != 0xFF && (value == 0 || value == 2))
			state_.weapon_availability[index] = value;
	}
	if (!reader.ok()) ++malformed_bodies_;
	++state_.weapon_availability_revision;
	state_.mark_changed();
}

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
	const hud::GameEventKind kind = hud::game_event_kind(rec.event_type);
	ev.kind = static_cast<uint8_t>(kind);

	// A killer-less death carries zeroed victim/aux slots — normalize them to
	// "none" so no consumer can mistake slot 0 for a real actor.
	if (kind == hud::GameEventKind::SelfDeath) {
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

std::vector<PlaySoundCommand> ClientReplicaPipeline::drain_sound_commands() {
	std::vector<PlaySoundCommand> result;
	result.swap(pending_sound_commands_);
	return result;
}

} // namespace opennova::replication
