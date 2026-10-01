// The host side of a radio call -- see server_radio_call.h.

#include <runtime/inmatch/server_radio_call.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include <base/io/bam.h>
#include <base/io/strutil.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/client_runtime.h> // in_active_radio_zone (the host client's zone overlay)
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_chat.h>    // nearest_location_index, carrier_vehicle
#include <runtime/inmatch/server_designations.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/geom.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/radio_call.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// The searched prefix of the rule table, byte-read from the binary: rows
// 0..38 and the type-0 row 39 (RAD_1) that ends every walk. Rows 40..70
// (RAD_2..RAD_5, RAD_8, the RAD_50CAL/MG/MK19 rows, the RAD_MP_* contexts,
// RAD_SNIPER1/2, RAD_HO_MG2, RAD_WL_50CAL2) all carry type 0 and follow the
// terminator, so no key ever reaches them.
// [orig: g_RadioCallRules @0x840C20..0x8411C0]
constexpr RadioCallRule kRadioCallRules[] = {
	{"RAD_6", 1, 500, 0, 0},
	{"RAD_MEDIC1", 2, 500, 0, 0},
	{"RAD_MEDIC2", 2, 8000, 0, 0},
	{"RAD_ENG1", 3, 1500, 0, 0},
	{"RAD_ENG2", 3, 1500, 0, 0},
	{"RAD_BT_2", 4, 1000, 0, 0},
	{"RAD_HO_2", 4, 1000, 0, 0},
	{"RAD_WL_2", 5, 1000, 0, 0},
	{"RAD_TN_1", 5, 1000, 0, 0},
	{"RAD_TN_COMM2", 5, 1000, 0, 0},
	{"RAD_BT_50CAL2", 6, 0, 0, 0},
	{"RAD_BT_M602", 6, 0, 0, 0},
	{"RAD_BT_MK192", 6, 0, 0, 0},
	{"RAD_BT_PASS2", 6, 0, 0, 0},
	{"RAD_GUNNER1", 7, 1000, 30, 3},
	{"RAD_GUNNER2", 7, 1000, 30, 3},
	{"RAD_HO_MG1", 7, 1000, 0, 0},
	{"RAD_TN_GUNN1", 7, 1000, 0, 0},
	{"RAD_TN_GUNN2", 7, 1000, 0, 0},
	{"RAD_WL_30MM1", 7, 1000, 0, 0},
	{"RAD_WL_50CAL1", 7, 1000, 0, 0},
	{"RAD_WL_MK191", 7, 1000, 0, 0},
	{"RAD_BT_MK191", 7, 1000, 0, 0},
	{"RAD_BT_50CAL1", 7, 1000, 0, 0},
	{"RAD_WL_1", 7, 1000, 0, 0},
	{"RAD_BT_1", 7, 1000, 0, 0},
	{"RAD_BT_M601", 7, 1000, 0, 0},
	{"RAD_BT_PASS1", 7, 1000, 0, 0},
	{"RAD_HO_1", 7, 1000, 0, 0},
	{"RAD_HO_PASS1", 7, 1000, 0, 0},
	{"RAD_HO_PASS2", 7, 1000, 0, 0},
	{"RAD_WL_MK192", 7, 1000, 0, 0},
	{"RAD_WL_PASS1", 7, 1000, 0, 0},
	{"RAD_WL_PASS2", 7, 1000, 0, 0},
	{"RAD_WL_30MM2", 7, 1000, 0, 0},
	{"RAD_TN_COMM1", 7, 1000, 0, 0},
	{"RAD_7", -1, 0, 30, 3},
	{"RAD_RIFLE1", -1, 0, 30, 3},
	{"RAD_RIFLE2", -1, 0, 30, 3},
	{"RAD_1", 0, 0, 0, 0},
};

// The cooldown every handled call re-arms, whole seconds
// [orig: `mov dword ptr [ebx+17Ch], 4` @0x5144d0 / @0x514716].
constexpr int32_t kRadioCallCooldownSeconds = 4;
// The radio-request latch call 6 raises on the sender, whole seconds
// [orig: entity+886 = 1Eh @0x5143f4].
constexpr uint8_t kRadioRequestSeconds = 30;
// The designation ray's reach, whole units [orig: `push 3E80000h` @0x514678].
constexpr int32_t kDesignationRayUnits = 1000;

// The rule distance between the sender and a recipient: the x87 length of the
// Q16 deltas (2-D for type 1, 3-D for types 2..5 and 7), saturated at
// flt_7C19E0 and truncated. [orig: @0x51457f..0x514603]
int32_t rule_distance(const world::Entity &a, const world::Entity &b, bool three_d) {
	const auto delta = [](float p, float q) {
		return static_cast<double>(static_cast<int32_t>(
				static_cast<uint32_t>(world::to_fixed(p)) - static_cast<uint32_t>(world::to_fixed(q))));
	};
	const double dx = delta(a.position.x, b.position.x);
	const double dy = delta(a.position.y, b.position.y);
	const double dz = three_d ? delta(a.position.z, b.position.z) : 0.0;
	const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
	return static_cast<int32_t>(std::min(length, 2147418112.0));
}

// Whether the rule admits a recipient other than the sender. Types 2..5 and 7
// pass a recipient in range (distance <= range << 16, or any distance when the
// range is 0) and then test it: 1 a controller or driver seat (+0x168 2 / 5),
// 2 a dead player (Flags & 2), 3 anything but a controller or driver seat, 4
// no carrier vehicle, or Flags 0x8000 (the water latch) at any range, 5 no
// carrier vehicle, 7 no further test; 6 (no distance) the sender's own
// carrier (both none counts), and any type outside 1..7 everyone.
// [orig: the jump tables @0x514578 / @0x51460e — cases @0x514615, @0x514726,
//  @0x51474f, @0x514782, @0x5147ab, @0x5147ca, @0x5147d0, default @0x5147e8]
bool rule_admits(const RadioCallRule &rule, const world::World &world,
		const world::Entity &sender, world::EntityHandle sender_carrier,
		const world::Entity &recipient) {
	const int32_t type = rule.type;
	if (type < 1 || type > 7) return true;
	int32_t distance = 0;
	if (type != 6) distance = rule_distance(sender, recipient, type != 1);
	const bool in_range = rule.range_units == 0 ||
			distance <= static_cast<int32_t>(static_cast<uint32_t>(rule.range_units) << 16);
	const bool control_seat = recipient.mount_type == world::SeatType::Controller ||
			recipient.mount_type == world::SeatType::Driver;
	const bool aboard = carrier_vehicle(world, recipient).valid();
	const uint32_t flags = recipient.flags | recipient.engine_flags;
	switch (type) {
	case 1: return in_range && control_seat;
	case 2: return in_range && (flags & world::kEntityFlagDead) != 0;
	case 3: return in_range && !control_seat;
	case 4: return (in_range && !aboard) || (flags & world::kEntityFlagDrowning) != 0;
	case 5: return in_range && !aboard;
	case 6: return carrier_vehicle(world, recipient) == sender_carrier;
	default: return in_range; // 7
	}
}

// The designation point: the sender's fire pose, a far point 1000 units down
// its view, then the entity-pool ray from the fire pose to it; the hit, else
// the far point. The local player's pose takes its own weapon view (the
// listen host's player is g_LocalPlayerEntity); a remote player's takes the
// on-foot, gunner and controller legs. The far point's water and terrain
// clips ride the ray's own terrain and water walks, as the local aim leg does
// (world/local_player_targeting.cpp).
// [orig: NapiNPServerMsg_HandleRadioCall @0x514672..0x5146ec —
//  Entity_BuildCameraFromWeaponView @0x4b0f30 (Entity_CalcWeaponFirePosition,
//  the (1000, 0, 0) transform, Terrain_ClipLineToWaterHeight, the heightmap
//  ray unless Flags 0x800000), Entity_CalcWeaponFirePosition @0x4dc750 again,
//  Physics_RaycastEntityPoolsAndUpdate @0x539580 and its hit +4..+0xC]
void designation_point(world::World &world, const world::Entity &sender, int32_t out[3]) {
	int32_t fire[6] = {};
	const world::WeaponTableEntry *held = world.tables.weapons.by_index(sender.equipped_adm_index);
	world::LocalPlayer *local = world.local_player_state;
	if (sender.handle == world.cached.local_player && local != nullptr) {
		const world::WeaponSlotState *slot = world::active_local_weapon_slot(world, local->weapon);
		world::local_weapon_fire_pose(world, local->weapon, slot != nullptr ? slot->clip : 0,
				world::player_view_scope_settled(local->view), fire);
	} else if (world::Entity *carrier = world.registry.get(sender.mount_target);
			carrier != nullptr && sender.mount_type == world::SeatType::Gunner) {
		world::usegun_fire_pose(world, *carrier, held, carrier->primary_weapon_slot.clip, fire);
	} else if (carrier != nullptr && sender.mount_type == world::SeatType::Controller &&
			(carrier->item_attrib & world::kItemAttribEweap) != 0) {
		world::controller_fire_pose(world, *carrier, held, carrier->primary_weapon_slot.clip, fire);
	} else {
		// On foot: Position + CameraOffset, (Yaw, Pitch + pitchBlend, Roll)
		// [orig: @0x4dc847..0x4dc880].
		const world::AiEntity *body = world.ai.for_handle(sender.handle);
		fire[0] = io::bam_add(body ? body->pos[0] : world::to_fixed(sender.position.x),
				body ? body->inf.eye_offset_x : sender.eye_offset_x);
		fire[1] = io::bam_add(body ? body->pos[1] : world::to_fixed(sender.position.y),
				body ? body->inf.eye_offset_y : sender.eye_offset_y);
		fire[2] = io::bam_add(body ? body->pos[2] : world::to_fixed(sender.position.z),
				body ? body->inf.eye_offset_z : sender.eye_offset_z);
		fire[3] = body ? body->heading : world::bam_heading_from_mission_yaw_deg(sender.yaw);
		fire[4] = body ? io::bam_add(body->pitch, body->inf.recoil_pitch)
		               : world::bam_from_degrees_wrapped(sender.pitch);
		fire[5] = body ? body->roll : world::bam_from_degrees_wrapped(sender.roll);
	}
	const int32_t forward[3] = {kDesignationRayUnits << 16, 0, 0};
	int32_t far_point[3];
	world::collision_matrix_from_euler(fire[3], fire[4], fire[5], fire)
			.transform_point(forward, far_point);
	std::copy_n(far_point, 3, out);
	if (world.collision == nullptr) return;
	world::ProjectileTrace trace;
	trace.owner = sender.handle;
	trace.start = {fire[0], fire[1], fire[2]};
	trace.end = {far_point[0], far_point[1], far_point[2]};
	trace.walk_terrain = ((sender.flags | sender.engine_flags) & world::kEntityFlagIndoors) == 0;
	const world::ProjectileHit hit = world.collision->trace_aim(world, trace);
	if (!hit.hit()) return;
	out[0] = hit.position_q16.x;
	out[1] = hit.position_q16.y;
	out[2] = hit.position_q16.z;
}

} // namespace

const RadioCallRule *radio_call_find_rule(const std::string &key) {
	// [orig: RadioCall_FindRuleByKey @0x5BFE50 — `if (!rules[0].type) return
	//  0` @0x5bfe86, then stricmp, and a next row of type 0 ends the walk
	//  @0x5bfea8..0x5bfeb9]
	if (kRadioCallRules[0].type == 0) return nullptr;
	for (size_t i = 0; i < std::size(kRadioCallRules); ++i) {
		if (strutil::iequals(kRadioCallRules[i].key, key)) return &kRadioCallRules[i];
		if (i + 1 >= std::size(kRadioCallRules) || kRadioCallRules[i + 1].type == 0) break;
	}
	return nullptr;
}

std::vector<ProtocolMessage> Server_HandleRadioCall(NapiNPServerCtx *ctx,
		NapiNPConnection &sender, const RadioCallRequest &request,
		std::vector<NapiNPConnection> &roster, world::World &world) {
	std::vector<ProtocolMessage> replies;
	if (ctx == nullptr) return replies;
	// The sender's player slot and its live player, off the cooldown
	// [orig: @0x514340..0x51438d — the slot @0x514353, the spectator latch
	//  +100567 @0x514365, the entity @0x514372, Flags & 2 @0x51437c, +380
	//  @0x514386].
	if (!sender.link.owned_entity.valid() || sender.link.spectator) return replies;
	world::Entity *entity = world.registry.get(sender.link.owned_entity);
	if (entity == nullptr) return replies;
	if (((entity->flags | entity->engine_flags) & world::kEntityFlagDead) != 0) return replies;
	if (sender.link.radio_call_cooldown_seconds != 0) return replies;

	TrackedPlayerVoice call;
	call.event = static_cast<uint8_t>(request.value & 0xFF); // the low byte @0x5143b0
	call.player_index = world::pool0_index_byte(entity->handle); // @0x5143bb..0x5143c5
	// The call's RAD_ key, flags 6 (no body prefix): the A&S active-zone
	// context reads the host process's client-side zone overlay.
	// [orig: RadioCall_FindRuleByKey(movsx call, entity) @0x5143c0..0x5143cd]
	const uint32_t game_type = ctx->config.game_type;
	const bool in_zone = game_type == 0x10010u && ctx->host_client != nullptr &&
			in_active_radio_zone(world, *entity, *ctx->host_client);
	const RadioCallRule *rule = radio_call_find_rule(world::radio_call_key(
			world, *entity, static_cast<int8_t>(call.event), 6, game_type, in_zone));
	// The sender's radio-request latch [orig: @0x5143d6..0x5143f4].
	entity->radio_request = 0;
	if (call.event == 6) {
		entity->radio_request = 1;
		entity->radio_request_seconds = kRadioRequestSeconds;
	}
	// The nearest location marker's index word, 0xFFFF when none
	// [orig: @0x5143fb..0x514494].
	call.location = static_cast<int16_t>(nearest_location_index(world, *entity));
	const std::vector<uint8_t> body = encode_tracked_player_voice(call);
	const uint8_t team = entity->team;
	const auto send = [&](NapiNPConnection &recipient) {
		// msgClass 0: the unreliable class [orig: SendFiltered(0x6D, 0, 1)
		// @0x5144c8 / @0x51480e].
		if (&recipient == &sender) {
			ProtocolMessage own = make_protocol_message(s2c::TRACKED_PLAYER_VOICE, body);
			own.reliable = false;
			replies.push_back(std::move(own));
		} else if (recipient.link.transport != nullptr) {
			recipient.link.transport->host_send(s2c::TRACKED_PLAYER_VOICE, body, false);
		}
	};
	const auto recipient_entity = [&](const NapiNPConnection &recipient) {
		return recipient.link.owned_entity.valid()
				? world.registry.get(recipient.link.owned_entity) : nullptr;
	};

	if (rule == nullptr) {
		// No rule: the sender's team, mask 0x180 (slot state 6/7 and the team
		// byte), the sender included [orig: @0x51449d..0x5144c8].
		for (NapiNPConnection &recipient : roster) {
			if (!active_player_recipient(recipient)) continue;
			const world::Entity *other = recipient_entity(recipient);
			if (other != nullptr && other->team == team) send(recipient);
		}
		sender.link.radio_call_cooldown_seconds = kRadioCallCooldownSeconds; // @0x5144d0
		return replies;
	}

	// The per-slot fan: an active slot on the sender's team whose NetPlayer is
	// in state 10 (the port's in-match connection short of the round-end
	// state 11), the sender always, any other slot as its rule admits, each
	// unicast (mask 0x20). A recipient without a player is skipped (retail
	// reads its entity unconditionally). [orig: @0x5144e6..0x51480e — the
	// sender's carrier Entity_FindChildByDefType @0x5144e6, active +4
	// @0x514510, team +0x1A0 @0x51451a, NetPlayer +0xA0 == 10 @0x514537, the
	// sender @0x514544]
	const world::EntityHandle sender_carrier = carrier_vehicle(world, *entity);
	for (NapiNPConnection &recipient : roster) {
		if (!is_in_match(recipient) || recipient.burst.game_state == 11) continue;
		const world::Entity *other = recipient_entity(recipient);
		if (other == nullptr || other->team != team) continue;
		if (&recipient != &sender &&
				!rule_admits(*rule, world, *entity, sender_carrier, *other))
			continue;
		send(recipient);
	}
	// A mode-3 rule marks the sender's aim point for its seconds
	// [orig: `cmp byte ptr [esi+10h], 3` @0x514668 ->
	//  EntityTracker_RegisterOrUpdate(entity, point, 62 * seconds, 0, mode)
	//  @0x5146f0..0x51470c].
	if (rule->designation_mode == 3) {
		int32_t point[3];
		designation_point(world, *entity, point);
		Server_RegisterDesignation(ctx->designations, entity->handle, point,
				62 * rule->designation_seconds, 0, rule->designation_mode);
	}
	sender.link.radio_call_cooldown_seconds = kRadioCallCooldownSeconds; // @0x514716
	return replies;
}

} // namespace opennova::inmatch
