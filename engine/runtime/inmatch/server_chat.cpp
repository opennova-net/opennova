#include <runtime/inmatch/server_chat.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <net/npwire/ingame_encode.h>   // encode_chat_broadcast / encode_player_downed_state / encode_play_sound
#include <net/npwire/ingame_message_id.h>
#include <runtime/audio/sound_profile.h> // compose_entity_sound_set — the 0x2E MEDIC_REQUEST composite
#include <runtime/inmatch/server_message_dispatch.h> // is_medic_recipient
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// The retail handler runs `sprintf(msg_buffer[128], format, name)`; the
// shipped STRSRV_MEDREQ carries one `%s`. Substitute it here (the CRT call is a
// platform primitive) and keep retail's 127-character buffer bound.
std::string format_medic_request(const std::string &format,
		const std::string &name) {
	std::string out;
	const size_t marker = format.find("%s");
	if (marker == std::string::npos) {
		out = format;
	} else {
		out = format.substr(0, marker) + name + format.substr(marker + 2);
	}
	if (out.size() > 127) out.resize(127);
	return out;
}

constexpr uint32_t kChatThrottleMs = 1000;        // @0x513819
constexpr uint32_t kChatRetentionFlushes = 310;   // SendFiltered(0x14, 1, 310)
constexpr int32_t kProximityPerAxisFixed = 0x640000; // 100.0 world units, 16.16

// The 0x14 body for one recipient: (senderSlot, channel, text) [orig:
// NetPacket_WriteTwoBytesAndCString @0x5047A0 stores the channel FIRST].
std::vector<uint8_t> chat_body(uint8_t sender_slot, int8_t channel,
		const std::string &text) {
	ChatBroadcast chat;
	chat.channel = channel;
	chat.sender_slot = sender_slot;
	chat.text = text;
	return encode_chat_broadcast(chat);
}

// The nearest type-2044 location marker whose radius contains the sender
// (2-D distance), by the marker's spawn-order index [orig: @0x51394C..0x5139D9:
// pool 3, def type 2044, sqrt(dx^2 + dy^2) < entity+0 (the marker bound),
// nearest wins; the label is g_LocationNames[64 * entity+640]].
int nearest_location_index(const world::World &world, const world::Entity &sender) {
	int best = -1;
	int index = 0;
	float best_distance = 0.0f;
	world.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 3 || e.item_id != 2044) return;
		const int my_index = index++;
		const float dx = sender.position.x - e.position.x;
		const float dy = sender.position.y - e.position.y;
		const float distance = std::sqrt(dx * dx + dy * dy);
		if (distance < e.bound_radius && (best < 0 || distance < best_distance)) {
			best = my_index;
			best_distance = distance;
		}
	});
	return best;
}

const std::string *location_label(const NapiNPServerCtx &ctx, const world::World &world,
		int index) {
	if (index < 0) return nullptr;
	if (static_cast<size_t>(index) < ctx.mission_location_names.size())
		return &ctx.mission_location_names[static_cast<size_t>(index)];
	// Without the MissionText table the marker's own name stands in (the same
	// fallback the 0x0F writer takes).
	static std::string fallback;
	fallback.clear();
	int walk = 0;
	world.registry.for_each([&](const world::Entity &e) {
		if (e.handle.pool() != 3 || e.item_id != 2044) return;
		if (walk++ == index) fallback = e.name;
	});
	return fallback.empty() ? nullptr : &fallback;
}

// Entity_FindChildByDefType(entity, 1, 0) walks the groundEntity chain (at most
// 20 links) for the LAST link whose def type is 1 (a vehicle) — the carrier
// the sender rides or stands on. [orig: @0x43BEA0]
world::EntityHandle carrier_vehicle(const world::World &world, const world::Entity &e) {
	world::EntityHandle matched;
	world::EntityHandle link = e.ground_target;
	for (int iteration = 1; link.valid() && iteration < 20; ++iteration) {
		const world::Entity *child = world.registry.get(link);
		if (child == nullptr || !child->has_item_def) break;
		if (child->item_type == 1) matched = link;
		link = child->ground_target;
	}
	return matched;
}

} // namespace

std::string chat_strip_angle_tags(const std::string &text) {
	std::string out;
	out.reserve(text.size());
	bool outside_tag = true;
	for (const char ch : text) {
		if (ch == '<') outside_tag = false;
		else if (outside_tag) out.push_back(ch);
		if (ch == '>') outside_tag = true;
	}
	return out;
}

std::vector<ProtocolMessage> Server_HandleMedicRequest(const std::string *format,
		NapiNPConnection &conn, std::vector<NapiNPConnection> &roster, world::World *world) {
	std::vector<ProtocolMessage> replies;
	if (format == nullptr || format->empty()) return replies;
	if (world == nullptr || !conn.burst.spawned || !conn.link.owned_entity.valid())
		return replies;
	const world::Entity *requester = world->registry.get(conn.link.owned_entity);
	if (requester == nullptr || conn.link.downed_revive_seconds == 0u) return replies;
	world->zones.spawn_waves.remove_player(conn.link.owned_entity);
	// The name is the player record's own (+0x28), the string Server_PlayerAdd
	// copies into the entity's Name (+0xF4). [orig: Server_BroadcastMedicRequest
	//  `lea eax,[esi+28h]` @0x515417 -> sprintf @0x515421]
	const std::string message = format_medic_request(*format, conn.reply.player_name);
	if (!conn.link.auto_medic_enabled && !conn.link.medic_request_active) {
		PlayerDownedState state;
		state.entity_handle = conn.link.owned_entity.packed;
		state.revive_seconds = static_cast<uint8_t>(conn.link.downed_revive_seconds);
		state.medic_request_active = false; // the raw slot+368 byte
		const std::vector<uint8_t> downed = encode_player_downed_state(state);
		for (NapiNPConnection &candidate : roster) {
			if (!is_medic_recipient(candidate, *world, requester->team)) continue;
			candidate.link.transport->host_send(s2c::PLAYER_DOWNED_STATE, downed);
		}
	}
	const std::vector<uint8_t> body = chat_body(conn.reply.player_slot, 2, message);
	for (NapiNPConnection &candidate : roster) {
		if (&candidate == &conn || !is_medic_recipient(candidate, *world, requester->team))
			continue;
		candidate.link.transport->host_send(
				s2c::CHAT_BROADCAST, body, true, 0, false, kChatRetentionFlushes);
	}
	// [orig: Server_BroadcastMedicRequest @0x5154AC..0x51550B]
	ProtocolMessage own_chat = make_protocol_message(s2c::CHAT_BROADCAST, body);
	own_chat.retention_flushes = kChatRetentionFlushes;
	replies.push_back(std::move(own_chat));
	conn.link.medic_request_active = true;
	// The help call: the requester's body-model composite "<prefix>_MEDIC_REQUEST",
	// positioned at the requester, to every alive player (mask 128). The listen
	// host's own copy rides the local slot-sound route the death scream uses.
	// [orig: SoundProfile_FindByEntityAndType(entity, 1) @0x515526 ->
	//  Server_SendOverlayActionToAlive @0x50A1B0]
	char set_name[24] = {};
	audio::compose_entity_sound_set(requester->anim_slot, audio::kEntitySoundMedicRequest,
			set_name, sizeof(set_name));
	PlaySoundCommand cmd;
	cmd.flag = 1;
	cmd.sound_name = set_name;
	cmd.has_pos = true;
	cmd.pos_x = static_cast<int16_t>(world::to_fixed(requester->position.x) >> 16);
	cmd.pos_y = static_cast<int16_t>(world::to_fixed(requester->position.y) >> 16);
	cmd.pos_z = static_cast<int16_t>(world::to_fixed(requester->position.z) >> 16);
	const std::vector<uint8_t> sound_body = encode_play_sound(cmd);
	for (NapiNPConnection &candidate : roster) {
		if (!is_in_match(candidate) || candidate.link.transport == nullptr ||
				!candidate.link.owned_entity.valid())
			continue;
		const world::Entity *listener = world->registry.get(candidate.link.owned_entity);
		if (listener == nullptr || listener->health <= 0) continue; // the mask-128 alive filter
		if (candidate.link.mode == replication::TransportMode::Loopback) {
			world::SoundSlotEvent local;
			local.source_handle = conn.link.owned_entity.packed;
			local.pos[0] = world::to_fixed(requester->position.x);
			local.pos[1] = world::to_fixed(requester->position.y);
			local.pos[2] = world::to_fixed(requester->position.z);
			local.slot = 0;
			std::memcpy(local.set_name, set_name, sizeof(set_name));
			world->out.slot_sounds.push_back(local);
			continue;
		}
		candidate.link.transport->host_send(s2c::PLAY_SOUND, sound_body, /*reliable=*/false);
	}
	return replies;
}

bool chat_text_is_int_triplet(const std::string &text) {
	int values[3] = {0, 0, 0};
	const char *cursor = text.c_str();
	for (int &value : values) {
		char *end = nullptr;
		const long parsed = std::strtol(cursor, &end, 10);
		if (end == cursor) return false;
		value = static_cast<int>(parsed);
		cursor = end;
	}
	return values[0] != 0 || values[1] != 0 || values[2] != 0;
}

std::vector<ProtocolMessage> Server_HandleChatMessage(NapiNPServerCtx &ctx,
		NapiNPConnection &sender, const ChatUplink &uplink, uint32_t now_ms,
		world::World &world) {
	std::vector<ProtocolMessage> replies;
	if (!ctx.is_authority) return replies;                       // @0x51378A
	if (!sender.link.owned_entity.valid()) return replies;       // conn+352 / +192
	const world::Entity *sender_entity = world.registry.get(sender.link.owned_entity);
	if (sender_entity == nullptr) return replies;
	const bool local_slot = sender.link.mode == replication::TransportMode::Loopback;
	// (state 10/11 || slot+5) && (!spectator || g_SpawnSuccessGate) [orig: @0x5137D7]
	if (!(is_in_match(sender) || local_slot)) return replies;
	if (sender.link.spectator) return replies;
	// The 1000 ms per-sender throttle [orig: slot+100360 @0x5137FF..0x513820].
	if (sender.reply.chat_last_ms != 0 && now_ms < sender.reply.chat_last_ms + kChatThrottleMs)
		return replies;
	sender.reply.chat_last_ms = now_ms;

	const int8_t channel = static_cast<int8_t>(uplink.channel);
	const bool sender_only = chat_text_is_int_triplet(uplink.text);  // @0x51383A
	const std::string stripped = chat_strip_angle_tags(uplink.text);
	// Channels outside the set send nothing at all [orig: @0x5138E1].
	if (channel != 12 && channel != 13 && channel != 1 && channel != 5 &&
			channel != 4 && channel != 11 && channel != 2)
		return replies;
	std::string formatted = sender.reply.player_name;              // slot+40 @0x5138FE
	// The squad-name suffix (slot+100572 -> the clan/squad node name +81)
	// [orig: @0x513903..0x513930]: squads are not modeled on this host.
	if (channel == 2) {
		const int location = nearest_location_index(world, *sender_entity);
		if (const std::string *label = location_label(ctx, world, location)) {
			formatted += ":[";                                       // @0x5139EC
			formatted += *label;                                     // @0x513A0A
			formatted += "]";                                        // @0x513A1E
		}
	}
	formatted += ": ";                                             // @0x513A39
	formatted += stripped;                                         // @0x513A50

	// The per-channel recipient predicate and the channel byte the body
	// carries (2/4/5 all write 2) [orig: the switch @0x513A5B].
	const uint8_t sender_team = sender_entity->team;
	const world::EntityHandle sender_carrier =
			channel == 11 ? carrier_vehicle(world, *sender_entity) : world::EntityHandle{};
	if (channel == 11 && !sender_carrier.valid()) return replies; // @0x513D6F
	int8_t wire_channel = channel;
	if (channel == 4 || channel == 5) wire_channel = 2;
	const int32_t sender_pos[3] = {
			world::to_fixed(sender_entity->position.x),
			world::to_fixed(sender_entity->position.y),
			world::to_fixed(sender_entity->position.z)};

	std::vector<NapiNPConnection> &roster = ctx.np_protocol.connection_list;
	for (NapiNPConnection &recipient : roster) {
		// slot active, NetPlayer state 10/11 [orig: each loop head]
		if (!is_in_match(recipient) || recipient.link.transport == nullptr) continue;
		const bool is_sender = &recipient == &sender;
		if (sender_only && !is_sender) continue;                   // (!dy || v == sender)
		const world::Entity *recipient_entity = recipient.link.owned_entity.valid()
				? world.registry.get(recipient.link.owned_entity) : nullptr;
		const uint8_t recipient_team = recipient_entity != nullptr ? recipient_entity->team : 0;
		bool admit = false;
		switch (channel) {
		case 2: // same team, or the recipient is a spectator (slot+96481 is never set)
			admit = recipient_team == sender_team || recipient.link.spectator;
			break;
		case 4: admit = recipient_team == 2; break;
		case 5: admit = recipient_team == 1; break;
		case 11: // state 10 only, same carrier vehicle
			admit = recipient_entity != nullptr &&
					carrier_vehicle(world, *recipient_entity) == sender_carrier;
			break;
		case 12: // same squad id (unmodeled: no squads) and !dy — nobody
			admit = false;
			break;
		case 13: { // within 100 units on every axis [orig: @0x513FC2..0x51401E]
			if (recipient_entity == nullptr) break;
			const int32_t dx = world::to_fixed(recipient_entity->position.x) - sender_pos[0];
			const int32_t dy = world::to_fixed(recipient_entity->position.y) - sender_pos[1];
			const int32_t dz = world::to_fixed(recipient_entity->position.z) - sender_pos[2];
			admit = std::abs(dx) <= kProximityPerAxisFixed &&
					std::abs(dy) <= kProximityPerAxisFixed &&
					std::abs(dz) <= kProximityPerAxisFixed;
			break;
		}
		default: admit = true; break; // channel 1: everyone
		}
		if (!admit) continue;
		const std::vector<uint8_t> body =
				chat_body(sender.reply.player_slot, wire_channel, formatted);
		if (is_sender) {
			ProtocolMessage own = make_protocol_message(s2c::CHAT_BROADCAST, body);
			own.retention_flushes = kChatRetentionFlushes;
			replies.push_back(std::move(own));
		} else {
			recipient.link.transport->host_send(
					s2c::CHAT_BROADCAST, body, true, 0, false, kChatRetentionFlushes);
		}
	}
	return replies;
}

} // namespace opennova::inmatch
