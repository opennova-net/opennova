// The message-feed lane a client folds: S2C 0x1E carries one 8-byte game event
// (kill, killer-less death, medic, objective/camp) that the original turns into
// a canned "Canned Msg" sentence and posts to the on-screen feed
// [orig: NapiNPClientMsg_GameEvent -> NetPacket_HandleGameEvent @0x426270 ->
//  HUD_FormatKillEventMessage @0x422DA0 -> Chat_FormatMessage @0x422C60 ->
//  Chat_AddMessageChannel2 @0x4987F0].
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
#include <runtime/world/entity.h> // retail_pool_capacity (the SPECTATORTARGET gate)

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
    } else if (strutil::iequals(command, "SU")) {
        // The scoreboard status-suffix gate, the byte of atol(n)
        // [orig: `mov g_ScoreboardStatusSuffixEnabled, al` @0x429f71].
        state_.scoreboard_status_suffix =
                static_cast<uint8_t>(std::strtol(value.c_str(), nullptr, 10));
        state_.mark_changed();
    } else if (strutil::iequals(command, "SPECTATORTARGET")) {
        // atol(n) as a packed handle, bounded to pools 0..4 and the pool's
        // capacity, then the track [orig: @0x429fe8..0x42a04e ->
        // Entity_TrySetMinimapTrackTarget @0x52abc0].
        const long handle = std::strtol(value.c_str(), nullptr, 10);
        const uint16_t packed = static_cast<uint16_t>(handle);
        if ((packed & 0xF000u) < 0x5000u &&
                static_cast<std::size_t>(packed & 0xFFFu) < world::retail_pool_capacity(packed >> 12))
            spectate_track(packed);
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

	if (rec.event_type >= 19 && rec.event_type <= 21)
		pending_effect_commands_.push_back(rec);
	ev.feed_order = next_feed_order_++;
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
	// The sender gate runs first: an active slot whose chat is muted, or a
	// spectator slot while the spawn gate is down, drops the line and its
	// channel-13 tracking [orig: Chat_DispatchToChannel @0x42b91e..0x42b943 —
	// PlayerSlotTable_GetActiveSlot, slot+0x32 & 2, slot+0x2E &&
	// !g_SpawnSuccessGate].
	const ClientRosterSlot &sender = state_.roster[rec.sender_slot];
	if (sender.bound && ((sender.radio_mute_flags & 2u) != 0 ||
			(sender.spectator && !state_.spawn_success_gate)))
		return;
	ClientChatLine line;
	line.channel = rec.channel;
	line.sender_slot = rec.sender_slot;
	line.text = rec.text;
	post_chat_line(std::move(line));
	// Channel 13 (local) also tracks its sender's person on the map
	// [orig: Chat_DispatchToChannel @0x42b9d0 (channel 13), @0x42b9e6..0x42ba09].
	if (rec.channel == 13) {
		LocalChatSpeaker speaker;
		speaker.slot = rec.sender_slot;
		pending_effect_commands_.push_back(speaker);
	}
}

// THE JOIN/LEAVE LANE (S2C 0x32): the record rides to the HUD, which picks
// the Client template and substitutes $A. No authority gate — the listen
// host's own client posts the lines its server fans too
// [orig: NapiNPClientMsg_0x032 @0x428060 has no is_authority test]. Only the
// handled subtypes 1..5 surface [orig: the default arm @0x428099].
void ClientReplicaPipeline::apply_formatted_game_text(const std::vector<uint8_t> &body) {
	FormattedGameText rec;
	bool clean = false;
	if (!decode_formatted_game_text(body.data(), body.size(), rec, &clean)) return;
	if (!clean) ++malformed_bodies_;
	ClientGameText text;
	text.subtype = rec.subtype;
	text.text = std::move(rec.text);
	text.team = rec.team;
	text.feed_order = next_feed_order_++;
	pending_game_texts_.push_back(std::move(text));
}

std::vector<ClientEffectCommand> ClientReplicaPipeline::drain_effect_commands() {
	std::vector<ClientEffectCommand> result;
	result.swap(pending_effect_commands_);
	return result;
}

} // namespace opennova::replication
