#include <runtime/inmatch/server_admin_command.h>

#include <base/io/strutil.h>
#include <net/npwire/ingame_decode.h>   // ChatBroadcast
#include <net/npwire/ingame_encode.h>   // encode_chat_broadcast
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/session_hello.h>   // DisconnectEvent
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>     // compute_death_anim_state / death_cause
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace opennova::inmatch {

namespace {

// SendFiltered(0x14, 1, 0x136): the reliable chat with its 310-flush retention.
constexpr uint32_t kChatRetentionFlushes = 0x136;
constexpr int8_t kServerChatChannel = 10;   // TextChatServer / TextChatPlayer / ChangeTeam
constexpr int8_t kCommandEchoChannel = 14;  // CmdEchoPlayer
constexpr uint8_t kNoSenderSlot = 0xFF;

bool ieq(std::string_view a, std::string_view b) { return strutil::iequals(a, b); }

// The CRT atol: base 10, leading whitespace and sign, 0 when nothing parses.
int32_t atol_token(const std::string &token) {
	return static_cast<int32_t>(std::strtol(token.c_str(), nullptr, 10));
}

// Player-slot state 6: an added player still in the match with its entity.
bool slot_in_game(const NapiNPConnection &conn) {
	return conn.phase >= ConnectionPhase::PlayerAdded && conn.phase < ConnectionPhase::Goodbye &&
	       conn.link.owned_entity.valid();
}

// The host's own slot (retail's slot+5 "local player" byte): the in-process
// type-2 loopback node.
bool slot_is_local(const NapiNPConnection &conn) {
	return conn.type == NapiNPConnection::kTypeClientSide ||
	       conn.link.mode == replication::TransportMode::Loopback;
}

NapiNPConnection *slot_by_index(NapiNPServerCtx &ctx, int32_t index) {
	if (index < 0) return nullptr;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.phase < ConnectionPhase::PlayerAdded || conn.phase >= ConnectionPhase::Goodbye)
			continue;
		if (conn.reply.player_slot == static_cast<uint8_t>(index)) return &conn;
	}
	return nullptr;
}

// Network_ParseHostAndPort's dotted-quad leg: "a.b.c.d:port" (the DNS resolve
// is a platform primitive; a name that is not a dotted quad resolves nothing).
bool parse_host_and_port(const std::string &text, PeerAddr &out) {
	const size_t colon = text.rfind(':');
	if (colon == std::string::npos) return false;
	std::array<uint8_t, 4> octets{};
	size_t octet = 0;
	uint32_t value = 0;
	bool digits = false;
	for (size_t i = 0; i <= colon; ++i) {
		const char c = i < colon ? text[i] : '.';
		if (c >= '0' && c <= '9') {
			value = value * 10u + static_cast<uint32_t>(c - '0');
			if (value > 255u) return false;
			digits = true;
		} else if (c == '.') {
			if (!digits || octet >= 4) return false;
			octets[octet++] = static_cast<uint8_t>(value);
			value = 0;
			digits = false;
		} else {
			return false;
		}
	}
	if (octet != 4) return false;
	const int32_t port = atol_token(text.substr(colon + 1));
	if (port < 0 || port > 0xFFFF) return false;
	out = peer_addr_from_octets(octets, static_cast<uint16_t>(port));
	return true;
}

// Server_FindPlayerSlotByNetKeys: the active slot whose connection carries
// this UDP source [orig: @0x5008B0].
NapiNPConnection *slot_by_ip_and_port(NapiNPServerCtx &ctx, const std::string &text) {
	PeerAddr peer;
	if (!parse_host_and_port(text, peer)) return nullptr;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide) continue;
		if (conn.phase < ConnectionPhase::PlayerAdded || conn.phase >= ConnectionPhase::Goodbye)
			continue;
		if (conn.peer == peer) return &conn;
	}
	return nullptr;
}

// PlayerSlot_FindByNameOrId [orig: @0x500950]: "*NN" (two decimal digits)
// is a slot index below the table capacity; otherwise the callsign matched
// case-insensitively, null when it matches two slots.
NapiNPConnection *slot_by_name(NapiNPServerCtx &ctx, const std::string &text) {
	if (text.size() >= 3 && text[0] == '*' && text[1] >= '0' && text[1] <= '9' &&
			text[2] >= '0' && text[2] <= '9') {
		const int32_t index = (text[1] - '0') * 10 + (text[2] - '0');
		if (index < static_cast<int32_t>(ctx.config.total_player_slot_capacity()))
			return slot_by_index(ctx, index);
	}
	NapiNPConnection *found = nullptr;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.phase < ConnectionPhase::PlayerAdded || conn.phase >= ConnectionPhase::Goodbye)
			continue;
		if (!ieq(conn.reply.player_name, text)) continue;
		if (found != nullptr) return nullptr;
		found = &conn;
	}
	return found;
}

// The four suffix lookups, in retail's test order [orig: @0x4D23F2..0x4D2505].
NapiNPConnection *resolve_target(NapiNPServerCtx &ctx, std::string_view suffix,
		const std::string &token) {
	if (ieq(suffix, "ByIndex")) return slot_by_index(ctx, atol_token(token));
	if (ieq(suffix, "ByIpAndPort")) return slot_by_ip_and_port(ctx, token);
	if (ieq(suffix, "ByName")) return slot_by_name(ctx, token);
	// ByPCID: PlayerSlot_FindByEntityTypeName @0x5009E0 matches the slot
	// entity's items.def type name; every pool-0 player here is the one
	// infantry type, so the lookup cannot single out a slot.
	return nullptr;
}

std::vector<uint8_t> chat_body(int8_t channel, const std::string &text) {
	ChatBroadcast chat;
	chat.channel = channel;
	chat.sender_slot = kNoSenderSlot;
	chat.text = text;
	return encode_chat_broadcast(chat);
}

void send_reliable(NapiNPConnection &conn, uint8_t tag, const std::vector<uint8_t> &body) {
	if (conn.link.transport == nullptr) return;
	conn.link.transport->host_send(tag, body, /*reliable=*/true, 0, false, kChatRetentionFlushes);
}

// send_mask 0x80: every in-game slot (state 6/7).
void fan_in_game(NapiNPServerCtx &ctx, uint8_t tag, const std::vector<uint8_t> &body) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn)) continue;
		send_reliable(conn, tag, body);
	}
}

// The Cycle / EndMission / GameOver winner token: a nonzero number is clamped
// to 0..4; zero (or a non-number) is one of the five team names; nothing
// selects 0. [orig: @0x4D30F6..0x4D31BF]
int32_t parse_winner_team(const std::vector<std::string> &args) {
	if (args.empty()) return 0;
	const int32_t numeric = atol_token(args[0]);
	if (numeric != 0) {
		if (numeric < 0) return 0;
		return numeric > 4 ? 4 : numeric;
	}
	if (ieq(args[0], "Green")) return 0;
	if (ieq(args[0], "Blue")) return 1;
	if (ieq(args[0], "Red")) return 2;
	if (ieq(args[0], "Yellow")) return 3;
	if (ieq(args[0], "Violet")) return 4;
	return 0;
}

} // namespace

ServerCommandOutcome Server_ExecuteServerCommand(NapiNPServerCtx &ctx, world::World *world,
		std::string_view verb, std::string_view target_suffix,
		const std::vector<std::string> &args) {
	ServerCommandOutcome outcome;
	// The common gates: the player table exists, the receiver is the
	// authority and in a session [orig: dword_24C0CA0 / is_authority /
	// ctx+0x68 @0x4D23C0..0x4D23E7 and each verb's copy].
	const bool gated = world != nullptr && ctx.is_authority != 0 && ctx.is_in_session != 0;
	if (!gated) return outcome;
	const bool targeted = ieq(verb, "PuntPlayer") || ieq(verb, "TextChatPlayer") ||
	                      ieq(verb, "CmdEchoPlayer") || ieq(verb, "KillPlayer");
	if (targeted) {
		const size_t needed = (ieq(verb, "TextChatPlayer") || ieq(verb, "CmdEchoPlayer")) ? 2 : 1;
		if (args.size() < needed) return outcome;
		NapiNPConnection *target = resolve_target(ctx, target_suffix, args[0]);
		if (target == nullptr) return outcome;
		if (ieq(verb, "PuntPlayer")) {
			// The host's own slot stops hosting instead [orig: @0x4D2515..0x4D251F];
			// a remote gets the chat-coded punt 39 carrying the target token
			// [orig: CNapiNPConnection_TrySendChatMessage(slot, "", 39, token) @0x4D254D].
			outcome.handled = true;
			if (slot_is_local(*target)) {
				outcome.stop_hosting = true;
				return outcome;
			}
			DisconnectEvent event;
			event.ds = 1;
			event.dc = 2;
			event.dpc = 39;
			event.ddstr = args[0];
			Server_StageHostDisconnect(*target, event);
			return outcome;
		}
		if (ieq(verb, "TextChatPlayer") || ieq(verb, "CmdEchoPlayer")) {
			// send_mask 0x20 = the resolved slot only [orig: @0x4D2738/@0x4D289F].
			outcome.handled = true;
			const int8_t channel = ieq(verb, "TextChatPlayer") ? kServerChatChannel : kCommandEchoChannel;
			send_reliable(*target, s2c::CHAT_BROADCAST, chat_body(channel, args[1]));
			return outcome;
		}
		// KillPlayer: the slot must be in the game; health and the kill credit
		// are cleared and the death check runs the full player-death event.
		// [orig: @0x4D29C6..0x4D29EC -> Entity_CheckAndProcessDeath @0x51B550 ->
		//  GameEvent_PlayerDeath @0x516DD0 for a Flags&0x100 player]
		if (!slot_in_game(*target)) return outcome;
		world::Entity *entity = world->registry.get(target->link.owned_entity);
		if (entity == nullptr) return outcome;
		outcome.handled = true;
		entity->health = 0;
		entity->last_attacker = world::EntityHandle{};
		entity->death_anim_state = world::compute_death_anim_state(
				0, 0, world::death_cause::kGeneric);
		world::RoundDeath death;
		death.victim = entity->handle;
		death.victim_handle = entity->handle.packed;
		death.event_flags = entity->cause_flags & 0xF00u;
		world->round_sim.deaths.push_back(death);
		return outcome;
	}
	if (ieq(verb, "TextChatServer")) {
		// Channel 10 from no slot to every in-game player [orig: @0x4D25A5..0x4D25E6;
		// a dedicated host also shows it locally @0x4D2603 — the listen host's
		// copy rides its loopback slot here].
		if (args.empty()) return outcome;
		outcome.handled = true;
		fan_in_game(ctx, s2c::CHAT_BROADCAST, chat_body(kServerChatChannel, args[0]));
		return outcome;
	}
	if (ieq(verb, "ChangeTeam") || ieq(verb, "SwapTeam")) {
		// Server_ChangeEntityTeam @0x518D70 (team 2 -> 1, 1 -> 2, then the
		// "Changing team...." chat to the slot) is not modeled on this host
		// (D-NET-148: the in-match team change is unported).
		return outcome;
	}
	if (ieq(verb, "Cycle") || ieq(verb, "EndMission") || ieq(verb, "GameOver")) {
		// The round end with the parsed winner, then the 620-tick linger in
		// place of the 2790 Server_ProcessRoundEnd stored [orig: @0x4D31BF..0x4D31CA].
		outcome.handled = true;
		world->process_round_end(parse_winner_team(args));
		ctx.round_end_linger_override_ticks = 0x26C;
		return outcome;
	}
	if (ieq(verb, "Earthquake")) {
		// Env_QuakeTicks = 6 * seconds; the argument is clamped 0..40 and
		// defaults to 30 [orig: @0x4D2AC2..0x4D2B13].
		outcome.handled = true;
		int32_t seconds = 30;
		if (!args.empty()) {
			seconds = atol_token(args[0]);
			if (seconds < 0) seconds = 0;
			else if (seconds > 40) seconds = 40;
		}
		world->commands.quake(seconds);
		return outcome;
	}
	if (ieq(verb, "Lightning")) {
		// Timer A = 16 locally and the "SETFLASH1 16" text command to every
		// in-game player [orig: @0x4D2B5D..0x4D2BAC, the S2C 0x24 via
		// NetBuffer_WriteString2 @0x5079C0 and SendFiltered(0x24, 1, 0x136)].
		outcome.handled = true;
		world->weather.command_flash();
		std::vector<uint8_t> body;
		for (const char c : std::string_view("SETFLASH1 16")) body.push_back(static_cast<uint8_t>(c));
		body.push_back(0);
		fan_in_game(ctx, s2c::TEXT_COMMAND, body);
		return outcome;
	}
	if (ieq(verb, "TimeOfDay")) {
		// HHMM, 1200 when absent or outside 0..2399; hours = floor(v * 0.01),
		// minutes = the remainder, packed as a minute of day
		// [orig: @0x4D2BE3..0x4D2C94 -> Environment_SetCurrentTime @0x57C4B0;
		//  the lighting caches reset @0x4D2C9C/@0x4D2CA1 ride env.generation here].
		outcome.handled = true;
		int32_t hhmm = 1200;
		if (!args.empty()) {
			hhmm = atol_token(args[0]);
			if (hhmm < 0 || hhmm >= 2400) hhmm = 1200;
		}
		const int32_t hours = static_cast<int32_t>(std::floor(static_cast<double>(hhmm) * 0.01));
		const int32_t minutes = hhmm - hours * 100;
		world->commands.set_time_of_day_minutes(hours * 60 + minutes);
		return outcome;
	}
	if (ieq(verb, "SetServerName")) {
		// Napi_CopyString(..., 32) into the config, the wire name and the
		// session name, then the HostSetup / Host "ServerName" vars and
		// Game_SaveConfig [orig: @0x4D2CF5..0x4D2DDF].
		if (args.empty()) return outcome;
		outcome.handled = true;
		outcome.config_changed = true;
		ctx.config.server_name = args[0].substr(0, 31);
		ctx.np_protocol.session_name = ctx.config.server_name;
		return outcome;
	}
	if (ieq(verb, "SetServerMsg")) {
		// Napi_CopyString(..., 128) into the custom text, then the Host "Msg"
		// var and Game_SaveConfig [orig: @0x4D2D8A..0x4D2DDF].
		if (args.empty()) return outcome;
		outcome.handled = true;
		outcome.config_changed = true;
		ctx.config.custom_text = args[0].substr(0, 127);
		return outcome;
	}
	if (ieq(verb, "SetMPReset")) {
		if (args.empty()) return outcome;
		outcome.handled = true;
		outcome.config_changed = true;
		ctx.config.multiplayer_reset = atol_token(args[0]);
		return outcome;
	}
	// ReloadPlayer (Entity_UpdateWeaponOverlayFrameState @0x4DC340 over the
	// slot's weapon tables) and DisarmPlayer (sub_4DC440) act on the retail
	// weapon-overlay tables, which this host does not model.
	return outcome;
}

} // namespace opennova::inmatch
