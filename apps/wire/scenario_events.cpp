#include "scenario_events.h"

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace opennova::wire {

namespace {

std::string format(const char *fmt, ...) {
	char buffer[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	return buffer;
}

uint16_t read_u16(const std::vector<uint8_t> &body, size_t at) {
	return static_cast<uint16_t>(body[at] | (body[at + 1] << 8));
}

// The decoded fields for one S2C message; empty when the tag is not one a
// scenario asserts on.
std::string server_fields(const InGameMessage &m, bool &decoded) {
	const uint8_t *body = m.payload.data();
	const size_t len = m.payload.size();
	size_t consumed = 0;
	switch (static_cast<uint8_t>(m.tag & 0xFFu)) {
		case s2c::ENTITY_DEATH: {
			EntityDeathRecord v;
			decoded = decode_entity_death(body, len, v, consumed);
			return format("kind=entity_death entity=0x%04x anim=%d",
					unsigned(v.entity_handle), int(v.death_anim_state_id));
		}
		case s2c::GAME_EVENT: {
			GameEventRecord v;
			decoded = decode_game_event(body, len, v, consumed);
			return format("kind=game_event type=%u attacker=%u victim=%u aux=%u pos=%d,%d",
					unsigned(v.event_type), unsigned(v.attacker_index),
					unsigned(v.victim_index), unsigned(v.aux_index), int(v.pos_x),
					int(v.pos_y));
		}
		case s2c::DEATH_CAMERA_TARGET: {
			DeathCameraTarget v;
			decoded = decode_death_camera_target(body, len, v, consumed);
			return format("kind=death_camera pos=%d,%d,%d", int(v.x), int(v.y), int(v.z));
		}
		case s2c::PLAYER_DOWNED_STATE: {
			PlayerDownedState v;
			decoded = decode_player_downed_state(body, len, v, consumed);
			return format("kind=downed entity=0x%04x revive=%u medic=%u",
					unsigned(v.entity_handle), unsigned(v.revive_seconds),
					v.medic_request_active ? 1u : 0u);
		}
		case s2c::TICK_SEED: {
			uint32_t seed = 0;
			decoded = decode_tick_seed(body, len, seed);
			return format("kind=tick_seed seed=%u", unsigned(seed));
		}
		case s2c::KILL_SYNC: {
			KillRecord v;
			decoded = decode_kill_record(body, len, v, consumed);
			return format("kind=kill_record victim_slot=0x%04x section=%d",
					unsigned(v.victim_slot), int(v.section));
		}
		case s2c::WEAPON_RELOAD: {
			WeaponReload v;
			decoded = decode_weapon_reload(body, len, v, consumed);
			return format("kind=weapon_reload entity=0x%04x param=%u",
					unsigned(v.entity_handle), unsigned(v.reload_param));
		}
		case s2c::TEAM_ASSIGN: {
			TeamAssign v;
			decoded = decode_team_assign(body, len, v, consumed);
			return format("kind=team_assign entity=0x%04x team=%u net_id=%u anim_slot=%u",
					unsigned(v.entity_handle), unsigned(v.team), unsigned(v.net_id),
					unsigned(v.anim_slot));
		}
		case s2c::PLAYER_SYNC: {
			// The roster: slot 0 is the host's own player.
			PlayerSync v;
			decoded = decode_player_sync(body, len, v);
			if (v.removal) return format("kind=player_sync slot=%u removal=1", unsigned(v.slot_id));
			const std::string team = (v.field_bitmask & 0x0004u) != 0
					? std::to_string(v.team) : std::string("-");
			return format("kind=player_sync slot=%u entity=0x%04x team=%s",
					unsigned(v.slot_id), unsigned(v.entity_slot_id), team.c_str());
		}
		case s2c::ZONE_TIMER_VALUE: {
			ZoneTimerValue v;
			decoded = decode_zone_timer_value(body, len, v, consumed);
			return format("kind=zone_timer zone=0x%04x mode=%u value=%d limit=%d rate=%d "
			              "b544=%u b545=%u",
					unsigned(v.zone_handle), unsigned(v.mode), int(v.value_s),
					int(v.limit_s), int(v.rate), unsigned(v.byte544), unsigned(v.byte545));
		}
		default:
			return {};
	}
}

std::string client_fields(const InGameMessage &m, ScenarioEventTracker &tracker,
		bool &decoded) {
	const uint8_t *body = m.payload.data();
	const size_t len = m.payload.size();
	size_t consumed = 0;
	switch (static_cast<uint8_t>(m.tag & 0xFFu)) {
		case c2s::FIRED_ROUND: {
			ClientFiredRound v;
			decoded = decode_client_fired_round(body, len, v, consumed);
			return format("kind=fired_round shooter=0x%04x adm=%u flags=%u target=0x%04x",
					unsigned(v.shooter_handle), unsigned(v.adm_index),
					unsigned(v.fire_flags), unsigned(v.target_handle));
		}
		case c2s::WEAPON_RELOAD_REQUEST: {
			WeaponReload v;
			decoded = decode_weapon_reload(body, len, v, consumed);
			return format("kind=reload_request entity=0x%04x param=%u",
					unsigned(v.entity_handle), unsigned(v.reload_param));
		}
		case c2s::VEHICLE_ATTACH_REQUEST: {
			// [u16 sender][u16 vehicle][u8 bone][u8 pad]; the catalog keeps it
			// printer-only [orig: NapiNPServerMsg_HandleVehicleAttach @0x502390].
			decoded = len >= 5;
			if (!decoded) return "kind=vehicle_attach";
			return format("kind=vehicle_attach sender=0x%04x vehicle=0x%04x bone=%u",
					unsigned(read_u16(m.payload, 0)), unsigned(read_u16(m.payload, 2)),
					unsigned(m.payload[4]));
		}
		case c2s::VEHICLE_DETACH_REQUEST: {
			// [u16 self][u16 vehicle][u16 junk]
			// [orig: NapiNPServerMsg_HandleVehicleDetach @0x4FC980].
			decoded = len >= 4;
			if (!decoded) return "kind=vehicle_detach";
			return format("kind=vehicle_detach entity=0x%04x vehicle=0x%04x",
					unsigned(read_u16(m.payload, 0)), unsigned(read_u16(m.payload, 2)));
		}
		case c2s::ENTITY_UPLINK: {
			EntityPacketSubHeader header;
			size_t header_bytes = 0;
			if (!decode_entity_packet_sub_header(body, len, header, header_bytes) ||
					header.sub_op != ENTITY_SUB_OP_EXTENDED)
				return {};
			PlayerExtendedUplink uplink;
			size_t body_bytes = 0;
			decoded = decode_player_extended_uplink(body + header_bytes, len - header_bytes,
					uplink, body_bytes);
			if (!decoded) return {};
			const auto last = tracker.carrier.find(m.session);
			if (last != tracker.carrier.end() && last->second == uplink.carrier_handle)
				return {};
			tracker.carrier[m.session] = uplink.carrier_handle;
			return format("kind=carrier handle=0x%04x carrier=0x%04x",
					unsigned(header.handle), unsigned(uplink.carrier_handle));
		}
		default:
			return {};
	}
}

} // namespace

std::string format_scenario_event(const InGameMessage &message, uint64_t ts_nanos,
		ScenarioEventTracker &tracker) {
	// Settings-update records share the low dispatch byte but are not these
	// gameplay messages.
	if (message.settings_update) return {};
	bool decoded = false;
	const std::string fields = message.dir == 'S'
			? server_fields(message, decoded)
			: message.dir == 'C' ? client_fields(message, tracker, decoded) : std::string();
	if (fields.empty()) return {};
	return format("SCENARIO_EVENT frame=%d ts_ns=%llu dir=%c session=%d tag=0x%02x ",
	              message.frame_index, static_cast<unsigned long long>(ts_nanos),
	              message.dir, message.session, unsigned(message.tag & 0xFFu)) +
	       fields + (decoded ? " decode=1" : " decode=0");
}

} // namespace opennova::wire
