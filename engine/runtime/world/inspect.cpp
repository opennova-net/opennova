// world::inspect — the entity directory join and the per-entity debug card
// (ADR 0042 d5). The card bodies moved here from the Godot binding's deleted
// Dictionary getters; their witness citations moved with them.
#include <runtime/world/inspect.h>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <unordered_map>

namespace opennova::world::inspect {

namespace {

constexpr float kFixed16 = 65536.0f;

Vec3 mission_from_fixed3(const int32_t pos[3]) {
	return Vec3{static_cast<float>(pos[0]) / kFixed16,
			static_cast<float>(pos[1]) / kFixed16,
			static_cast<float>(pos[2]) / kFixed16};
}

int32_t fixed_from_mission(float v) {
	return static_cast<int32_t>(v * kFixed16);
}

// The AI half of the card — the old get_entity_debug body. A scripted remove
// (VaporizeSingle / removeSSN) despawns the registry slot while the AiEntity
// stays in the AI pool, so the registry block emits TYPED DEFAULTS rather
// than dropping fields — the card's shape is stable whether the entity is
// whole or registry-despawned.
void fill_ai_detail(World &world, const AiEntity &e, AiDetail &d,
		const std::function<std::string(int32_t)> &adm_name_resolver) {
	const Entity *ent = world.registry.get(e.handle);
	d.kind = ent ? spawn_origin_kind(ent->spawn_origin) : -1;
	d.source_index =
			ent ? static_cast<int32_t>(spawn_origin_index(ent->spawn_origin)) : -1;
	d.bms_id = ent ? ent->bms_id : 0;
	d.item_id = ent ? ent->item_id : 0;
	// NOTE: retail BMS names are Windows-1252; non-ASCII bytes will read as
	// invalid UTF-8 downstream. Names are ASCII in practice; revisit if
	// mojibake shows.
	d.name = ent ? ent->name : std::string();
	d.group_id = ent ? static_cast<int32_t>(ent->group_id) : 0;
	d.pool = ent ? e.handle.pool() : -1;
	d.engine_flags = ent ? static_cast<int64_t>(ent->engine_flags) : 0;
	d.bms_flags = ent ? static_cast<int64_t>(ent->flags) : 0;
	d.waypoint_id = ent ? static_cast<int32_t>(ent->waypoint_id) : 0;
	d.wp_number = ent ? ent->wp_number : 0;
	d.health = ent ? ent->health : 0;
	d.alive = ent ? ent->alive : false;
	d.hidden = ent ? ent->hidden : false;
	d.held = ent ? ent->held : false;
	d.disabled = ent ? ent->disabled : false;
	d.vehicle_family = -1;
	if (ent != nullptr) {
		if (const VehicleTraits *traits = world.vehicles.traits.get(ent->item_id))
			d.vehicle_family = static_cast<int32_t>(traits->family);
	}
	d.body_anim_slot = ent ? ent->body_anim_slot : -1;
	d.character_anim_slot = ent ? static_cast<int32_t>(ent->anim_slot) : -1;
	d.minimap_net_id = ent ? static_cast<int32_t>(ent->minimap_net_id) : 0;
	d.mounted = ent ? ent->mounted : false;
	d.mount_seat = ent ? static_cast<int32_t>(ent->mount_seat) : -1;
	d.mount_type = ent ? static_cast<int32_t>(ent->mount_type) : 0;
	d.mount_config_valid = ent ? ent->mounted_config_valid : false;
	d.mount_config =
			(ent && ent->mounted_config_valid) ? ent->mounted_config : 0;
	if (ent && ent->mounted) {
		if (const Entity *target = world.registry.get(ent->mount_target)) {
			d.mount_target_net_id = static_cast<int32_t>(target->net_id);
			d.mount_target_config_valid = target->emplaced_config_valid;
			d.mount_target_config =
					target->emplaced_config_valid ? target->emplaced_config : 0;
			d.mount_target_seat_count = static_cast<int32_t>(target->seats.size());
			d.mount_target_seats.reserve(target->seats.size());
			for (size_t i = 0; i < target->seats.size(); ++i)
				d.mount_target_seats.push_back(
						seat_row(target->seats[i], static_cast<int32_t>(i)));
			if (ent->mount_seat >= 0 &&
					ent->mount_seat < static_cast<int32_t>(target->seats.size())) {
				const Seat &seat = target->seats[ent->mount_seat];
				d.mount_type = static_cast<int32_t>(seat.type);
				d.mount_seat_bone = static_cast<int32_t>(seat.bone_index);
				d.mount_seat_pose_index = static_cast<int32_t>(seat.pose_index);
				d.mount_seat_source_name = seat.source_name;
				d.mount_seat_local = seat.seat_local;
				d.mount_seat_yaw_offset = static_cast<int32_t>(seat.yaw_offset);
			}
		}
	}
	d.net_id = e.net_id;
	d.wire_handle = static_cast<int32_t>(e.handle.packed);
	d.team = static_cast<int32_t>(e.team);
	d.ai_health = static_cast<int32_t>(e.health);
	d.mission_position = mission_from_fixed3(e.pos);
	d.yaw_deg = mission_yaw_deg_from_bam_heading(e.heading);
	const int32_t state = e.brain.f[AiBrain::kCurState];
	d.state = state;
	d.state_name = ai_state_name(state);
	d.pending_state = e.brain.f[AiBrain::kPendState];
	// The BRAIN kAlert register is NOT what the think tests. Every alert gate
	// in infantry.cpp reads slot.bytes()[AiSlot::kAlertByte] (slot byte 136),
	// which is also what the retail probe reads (AiSlot dword 34). Emitting the
	// brain register here made our side read 0 in every sample and produced a
	// false "we never raise alert" divergence — the fourth false friend in this
	// effort, and the first one on our own side.
	d.alert = e.slot.bytes()[AiSlot::kAlertByte];
	d.alert_brain = e.brain.f[AiBrain::kAlert];
	d.wp_channel = e.brain.f[AiBrain::kWpChannel];
	d.wp_node = e.brain.f[AiBrain::kWpNode];
	d.wp_distance = e.brain.f[AiBrain::kWpDistance];
	d.out_speed = e.brain.f[AiBrain::kOutSpeed];
	// Brain combat/movement registers. The target slots store the packed wire
	// handle + 1, 0 = null [orig: Entity_SetAITarget @0x45d760 — the container
	// rebase of the original's entity pointer].
	const int32_t target_packed = e.brain.f[AiBrain::kTargetSlot];
	d.target_valid = target_packed != 0;
	d.target_handle = target_packed != 0 ? target_packed - 1 : -1;
	if (target_packed != 0) {
		EntityHandle th;
		th.packed = static_cast<uint16_t>(target_packed - 1);
		if (const Entity *te = world.registry.get(th)) d.target_name = te->name;
	}
	const int32_t prio_packed = e.brain.f[AiBrain::kPriorityTarget];
	d.priority_target_valid = prio_packed != 0;
	d.priority_target_handle = prio_packed != 0 ? prio_packed - 1 : -1;
	d.combat_timer = e.brain.f[AiBrain::kCombatTimer];
	d.fire_delay = e.brain.f[AiBrain::kFireDelay];
	d.retarget_timer = e.brain.f[AiBrain::kRetargetTimer];
	const uint32_t cooldowns =
			static_cast<uint32_t>(e.brain.f[AiBrain::kCooldownPair]);
	d.cooldown_a = static_cast<int32_t>(cooldowns & 0xFFFF);
	d.cooldown_b = static_cast<int32_t>(cooldowns >> 16);
	d.speed_a = e.brain.f[AiBrain::kSpeedA];
	d.speed_b = e.brain.f[AiBrain::kSpeedB];
	{
		const int32_t wp[3] = {e.brain.f[AiBrain::kWorkPosX],
				e.brain.f[AiBrain::kWorkPosY], e.brain.f[AiBrain::kWorkPosZ]};
		d.work_pos = mission_from_fixed3(wp);
	}
	d.work_heading = e.brain.f[AiBrain::kWorkHeading];
	d.turret_yaw = e.brain.f[AiBrain::kActiveYaw];
	d.turret_pitch = e.brain.f[AiBrain::kActivePitch];
	// Profile summary (the read-only .aip definition) + the slot control word
	// the combat gates read (0x1 blind, 0x8, 0x200 berserk — infantry_combat's
	// scan entry tests).
	for (int ci = 0; ci < 4; ++ci)
		d.profile_class_priority[ci] = e.profile.class_priority[ci];
	d.profile_fov_primary = e.profile.fov_primary;
	d.profile_fov_secondary = e.profile.fov_secondary;
	d.profile_range_primary = e.profile.range_primary;
	d.profile_range_secondary = e.profile.range_secondary;
	d.profile_approach_cap = e.profile.approach_cap;
	d.profile_type = e.profile.type;
	d.slot_control_bits = e.slot.f[1];
	// Rotor spin, so a live round can show the blades actually turning rather
	// than only the code that says they should.
	if (const Entity *ve = world.registry.get(e.handle)) {
		d.has_vehicle_block = true;
		d.rotor_speed = ve->veh.part_spin.speed;
		d.rotor_phase = ve->veh.part_spin.angle;
		// The rotor machine's three gates, so a still rotor names its cause:
		// the seeded rate, the brain's profile type (the HELO twin runs only
		// for type 1 [orig: Entity_UpdateHeloRotorSpin @0x48FA70, the
		// `profile+0x10 == 1` test @0x48fa98]) and the engine-running claimant
		// latch (+0x170; world::Entity::primary_occupant).
		d.rotor_rate = ve->veh.part_spin.rate;
		d.profile_type = e.profile.type;
		d.primary_occupant = ve->primary_occupant.valid();
		// The mover family, so a rotor check can tell "no helicopter here"
		// from "the helicopter's blades are not turning".
		const VehicleTraits *vt = world.vehicles.traits.get(ve->item_id);
		d.veh_family = vt != nullptr ? static_cast<int32_t>(vt->family) : -1;
		d.player_control = vt != nullptr && vt->player_control;
		// Flight-command chain, so a "the helicopter will not move" report can
		// name WHICH link is dead: the pilot's packed MoveOrder, the staged
		// cyclic pair, and the altitude target.
		// Motor-internal taps for the convoy-pace hunt (AI-PARITY-CONCEPT
		// 6.12g): the integrated speed vs the command names which stage loses
		// the pace.
		d.vp = ve->pitch;
		d.vr = ve->roll;
		d.mspd = ve->veh.speed;
		for (int wi = 0; wi < 4; ++wi) d.wc[wi] = ve->veh.wheel_comp[wi];
		// per-pad contact depths (diagnostic, §6.15 flap hunt)
		for (int wi = 0; wi < 4; ++wi) d.pd[wi] = ve->veh.dbg_pad_depth[wi];
		d.macc = ve->veh.speed_accel;
		d.mgnd = ve->veh.grounded;
		d.cmd_fwd = ve->veh.cmd_speed;
		d.cmd_lat = ve->veh.cmd_lateral_speed;
		d.alt_tgt = ve->veh.net_alt_target;
		d.engine_on = ve->veh.net_engine_on;
		int32_t pilot_move = -1;
		for (const Seat &st : ve->seats) {
			if (!st.occupant.valid()) continue;
			const Entity *oc = world.registry.get(st.occupant);
			if (oc != nullptr && oc->player_class != 0)
				pilot_move = static_cast<int32_t>(oc->net_move_input);
		}
		d.pilot_move = pilot_move;
	}
	d.infantry = e.inf.active;
	d.adm_id = e.inf.active ? e.inf.adm_id : -1;
	d.adm_name = (e.inf.active && adm_name_resolver)
			? adm_name_resolver(e.inf.adm_id)
			: std::string();
	d.infantry_move_mode = e.inf.move_mode;
	// The frozen-clump instrument: this tick's integrated root step vs the
	// collision resolver's horizontal correction (16.16 fixed).
	d.root_dx = e.inf.dbg_root_dx;
	d.root_dy = e.inf.dbg_root_dy;
	d.res_dx = e.inf.dbg_res_dx;
	d.res_dy = e.inf.dbg_res_dy;
	d.contact_item = e.inf.dbg_contact_item;
	// AI DECISION STATE, named to match the retail probe (onhook ai_probe.c) so
	// the two recordings join field-for-field. Retail reads these straight off
	// the entity and its AiSlot; these are our equivalents:
	//   parent  = the carrier we are mounted to   [orig: entity->parentEntity +364]
	//   ground  = what we are standing on         [orig: entity->groundEntity +0x28]
	//   s35/37/38 = has-route / command / node    [orig: AiSlot +140/+148/+152]
	// Without them a retail-vs-OpenNova diff can see THAT a body is stuck but
	// not what order it believes it is under, which is the question that matters.
	if (const Entity *pe = ent && ent->mounted
					? world.registry.get(ent->mount_target)
					: nullptr)
		d.parent = static_cast<int32_t>(pe->bms_id);
	if (const Entity *ge = ent ? world.registry.get(ent->ground_target) : nullptr)
		d.ground = static_cast<int32_t>(ge->bms_id);
	d.s35 = e.slot.f[35];
	d.s37 = e.slot.f[37];
	d.s38 = e.slot.f[38];
	d.fires_aimed = e.inf.dbg_fires_aimed;
	d.fires_body = e.inf.dbg_fires_body;
	// The fall-through instrument: the sim's own ground value under this body
	// and whether it is airborne (a body whose ground sits far below it every
	// tick falls forever).
	d.ground_cache = e.inf.ground_cache;
	d.ground_valid = e.inf.ground_cache_valid;
	d.airborne = e.inf.airborne;
	d.anim_state = e.inf.active ? e.inf.anim_state : -1;
	d.anim_key = e.inf.active ? infantry_anim_key(e.inf.anim_state) : std::string();
	// Infantry combat diagnostics (the P1 threat-loop bring-up surface): the
	// perception/attack ranges the scan reads (AiSlot +68/+60, world units),
	// the organic ammo[0] byte (closeattack) plus clipsize, the live magazine
	// word, and the current combat target.
	d.sight_range_u = e.slot.f[AiSlot::kSightRange] / 65536.0;
	d.attack_range_u = e.slot.f[AiSlot::kAttackRange] / 65536.0;
	d.ammo_primary = e.profile.organic.ammo[0] != 0 ? e.profile.organic.ammo[0] : -1;
	d.clip_size = e.profile.clip_size;
	d.magazine = static_cast<int32_t>(e.inf.magazine);
	d.combat_target_valid = e.inf.combat_target.valid();
	// The infantry pass's live aim/reaction state (§17); the SM/vehicle chain's
	// equivalents are the brain registers above.
	d.aim_heading = e.inf.aim_heading;
	d.aim_pitch = e.inf.aim_pitch;
	d.aim_valid = e.inf.aim_valid;
	d.damage_timer = e.inf.damage_timer;
	d.same_target_ticks = e.inf.same_target_ticks;
	d.combat_move_timer = e.inf.combat_move_timer;
	// The fire-origin readback (probe surface): the launch userpoint on this
	// body's posed skeleton, resolved now by the sim's own provider — the same
	// point the fire pass, LOS rays, and aim eye read (world/pose_provider.h).
	{
		int32_t muzzle[3] = {};
		d.muzzle_valid = world.pose_provider != nullptr &&
				world.pose_provider->resolve_muzzle_pose(
						world, e.handle, muzzle);
		d.muzzle = d.muzzle_valid ? mission_from_fixed3(muzzle) : Vec3{};
	}
	// Death presentation (P1c): the damage-time selection still pending
	// consume, the live corpse countdown, and the def traits behind them
	// (world-wac-ai-re §19).
	d.death_anim_state = ent ? ent->death_anim_state : 0;
	d.corpse_timer = ent ? ent->corpse_timer : 0;
	d.deathtime_ticks = ent ? ent->deathtime_ticks : 0;
	d.leave_corpse = ent ? ent->leave_corpse : false;
}

// The registry/world half — the old get_world_entity_debug body. Pool-1
// vehicles (and anything else without an AI brain) are invisible to the
// AI-pool half; vehicle probes find and place them through this one.
void fill_world_detail(const World &world, const Entity &ent, WorldDetail &d) {
	const EntityHandle h = ent.handle;
	d.net_id = static_cast<int32_t>(ent.net_id);
	d.bms_id = ent.bms_id;
	d.pool = h.pool();
	d.kind = ent.spawn_origin == kSpawnOriginNone
			? -1
			: spawn_origin_kind(ent.spawn_origin);
	d.source_index = ent.spawn_origin == kSpawnOriginNone
			? -1
			: static_cast<int32_t>(spawn_origin_index(ent.spawn_origin));
	d.item_id = ent.item_id;
	d.name = ent.name;
	if (const std::string *item_name = world.tables.item_names.get(ent.item_id))
		d.item_name = *item_name;
	d.team = static_cast<int32_t>(ent.team);
	d.alive = ent.alive;
	d.hidden = ent.hidden;
	d.health = ent.health;
	d.health_max = ent.health_max;
	d.has_item_def = ent.has_item_def;
	d.handle = static_cast<int32_t>(h.packed);
	d.item_type = static_cast<int32_t>(ent.item_type);
	d.item_unit_type = ent.item_unit_type;
	d.item_attrib = static_cast<int64_t>(ent.item_attrib);
	d.item_attrib2 = static_cast<int64_t>(ent.item_attrib2);
	d.vehicle_family = -1;
	if (const VehicleTraits *traits = world.vehicles.traits.get(ent.item_id))
		d.vehicle_family = static_cast<int32_t>(traits->family);
	d.has_minimap_model_marker = ent.has_minimap_model_marker;
	d.is_capture_trigger = ent.is_capture_trigger;
	d.is_spawn_point = ent.is_spawn_point;
	d.zone_number = static_cast<int32_t>(ent.zone_number);
	d.zone_radius = static_cast<int32_t>(ent.zone_radius);
	d.zone_control = ent.zone_control;
	d.zone_chain_index = -1;
	for (size_t i = 0; i < world.zones.chain.zones.size(); ++i) {
		if (world.zones.chain.zones[i] == h) {
			d.zone_chain_index = static_cast<int32_t>(i);
			break;
		}
	}
	d.mission_position = ent.position;
	d.yaw = ent.yaw;
	d.pitch = ent.pitch;
	d.roll = ent.roll;
	d.primary_weapon_clip = ent.primary_weapon_slot.clip;
	d.primary_weapon_reserve = ent.primary_weapon_slot.reserve;
	d.seats.reserve(ent.seats.size());
	for (size_t i = 0; i < ent.seats.size(); ++i)
		d.seats.push_back(seat_row(ent.seats[i], static_cast<int32_t>(i)));
}

} // namespace

SeatRow seat_row(const Seat &seat, int32_t index) {
	SeatRow row;
	row.index = index;
	row.type = static_cast<int32_t>(seat.type);
	row.retail_slot = static_cast<int32_t>(seat.retail_slot);
	row.bone_index = static_cast<int32_t>(seat.bone_index);
	row.pose_index = static_cast<int32_t>(seat.pose_index);
	row.source_name = seat.source_name;
	row.local = seat.seat_local;
	row.yaw_offset = static_cast<int32_t>(seat.yaw_offset);
	row.occupied = seat.occupant.valid();
	return row;
}

std::vector<EntityRow> entity_directory(const World &world, bool with_brains) {
	std::vector<EntityRow> out;
	const AiSystem *ai = with_brains ? &world.ai : nullptr;

	// AI handle -> pool index, once (the join key; a brain's wire identity IS
	// its packed EntityHandle).
	std::unordered_map<uint16_t, int32_t> ai_by_handle;
	if (ai != nullptr) {
		for (int i = 0; i < ai->count(); ++i) {
			if (const AiEntity *e = ai->at(i))
				ai_by_handle.emplace(e->handle.packed, i);
		}
	}

	// The primary list: one row per live registry slot carrying a wire type —
	// the set the client present streams [orig: collect_visible_entities_for_
	// terrain @0x5c8c60 walks the pools]. A row without a def (item_id 0) is
	// not presented and joins the appended diagnostics below when it carries a
	// brain.
	std::vector<bool> seen_ai(ai != nullptr ? static_cast<size_t>(ai->count()) : 0, false);
	world.registry.for_each([&](const Entity &e) {
		if (e.item_id == 0) return;
		EntityRow row;
		row.ai_index = -1;
		if (ai != nullptr) {
			auto it = ai_by_handle.find(e.handle.packed);
			if (it != ai_by_handle.end()) {
				row.ai_index = it->second;
				seen_ai[static_cast<size_t>(it->second)] = true;
			}
		}
		row.editable = row.ai_index >= 0;
		row.presented = true;
		row.registry_present = true;
		row.kind = e.spawn_origin == kSpawnOriginNone
				? -1
				: spawn_origin_kind(e.spawn_origin);
		row.source_index = e.spawn_origin == kSpawnOriginNone
				? -1
				: static_cast<int32_t>(spawn_origin_index(e.spawn_origin));
		row.bms_id = e.bms_id;
		row.net_id = static_cast<int32_t>(e.net_id);
		row.item_id = e.item_id;
		row.wire_handle = e.handle.packed;
		row.name = e.name;
		if (const std::string *item_name = world.tables.item_names.get(e.item_id))
			row.item_name = *item_name;
		row.health = e.health;
		row.alive = e.alive;
		row.hidden = e.hidden;
		row.mission_position = e.position;
		row.team = static_cast<int32_t>(e.team);
		if (row.ai_index >= 0) {
			if (const AiEntity *ae = ai->at(row.ai_index)) {
				row.team = static_cast<int32_t>(ae->team);
				row.state_name = ai_state_name(ae->brain.f[AiBrain::kCurState]);
			}
		}
		out.push_back(std::move(row));
	});

	// AI-pool entries missing from the presented set, appended as diagnostics
	// (despawned registry slots and def-less rows). editable requires the
	// live registry slot the edit seams mutate.
	if (ai != nullptr) {
		for (int i = 0; i < ai->count(); ++i) {
			if (seen_ai[static_cast<size_t>(i)]) continue;
			const AiEntity *e = ai->at(i);
			if (e == nullptr) continue;
			const Entity *ent = world.registry.get(e->handle);
			EntityRow row;
			row.ai_index = i;
			row.editable = ent != nullptr;
			row.presented = false;
			row.registry_present = ent != nullptr;
			row.kind = ent ? spawn_origin_kind(ent->spawn_origin) : -1;
			row.source_index = ent
					? static_cast<int32_t>(spawn_origin_index(ent->spawn_origin))
					: -1;
			row.bms_id = ent ? ent->bms_id : 0;
			row.net_id = e->net_id;
			row.item_id = ent ? ent->item_id : 0;
			row.wire_handle = e->handle.packed;
			row.name = ent ? ent->name : std::string();
			if (const std::string *item_name =
							ent ? world.tables.item_names.get(ent->item_id) : nullptr)
				row.item_name = *item_name;
			row.state_name = ai_state_name(e->brain.f[AiBrain::kCurState]);
			row.health = ent ? ent->health : 0;
			row.team = static_cast<int32_t>(e->team);
			row.alive = ent ? ent->alive : false;
			row.hidden = ent ? ent->hidden : false;
			row.mission_position = mission_from_fixed3(e->pos);
			out.push_back(std::move(row));
		}
	}

	for (size_t i = 0; i < out.size(); ++i)
		out[i].index = static_cast<int32_t>(i);
	return out;
}

EntityCard build_entity_card(World &world, bool with_brains, EntityHandle handle,
		const std::function<std::string(int32_t)> &adm_name_resolver) {
	EntityCard card;
	const AiSystem *ai = with_brains ? &world.ai : nullptr;
	if (!handle.valid()) return card;
	card.handle = handle.packed;
	if (const Entity *ent = world.registry.get(handle)) {
		card.has_world = true;
		fill_world_detail(world, *ent, card.world);
        card.facial.display_frame = world.facials.display_frame();
        if (const FacialSlot *face = world.facials.for_entity(*ent)) {
            auto &d = card.facial;
            d.available = true;
            d.current = face->current; d.next = face->next;
            d.expression_override = face->expression_override;
            d.automatic = face->automatic;
            d.override_timer = face->override_timer;
            d.texture_priority = face->active ? face->priority : -1;
            d.blend = face->blend;
        }

	}
	if (ai != nullptr) {
		card.ai_index = ai->index_for_handle(handle);
		if (const AiEntity *e = ai->for_handle(handle)) {
			card.has_ai = true;
			fill_ai_detail(world, *e, card.ai, adm_name_resolver);
		}
	}
	card.valid = card.has_world || card.has_ai;
	return card;
}

AiDebugReport ai_debug_report(World &world) {
	AiDebugReport report;
	const AiSystem &ai = world.ai;

	// Follower counts over the WHOLE pool (the row cap below never hides a
	// route's traffic). Channel 0 is "no channel" (ai_waypoint_update_target's
	// unresolved leg), so it never counts.
	std::unordered_map<int32_t, int32_t> followers;
	for (int i = 0; i < ai.count(); ++i) {
		const AiEntity *e = ai.at(i);
		if (e == nullptr) continue;
		const int32_t channel = e->brain.f[AiBrain::kWpChannel];
		if (channel > 0) ++followers[channel];
	}

	report.rows.reserve(static_cast<size_t>(
			std::min(ai.count(), AiDebugReport::kMaxRows)));
	for (int i = 0; i < ai.count(); ++i) {
		if (static_cast<int>(report.rows.size()) >= AiDebugReport::kMaxRows)
			break;
		const AiEntity *e = ai.at(i);
		if (e == nullptr) continue;
		const Entity *ent = world.registry.get(e->handle);
		AiOverlayRow row;
		row.ai_index = i;
		row.handle = e->handle.packed;
		// Most BMS organics carry no authored name; the items.def display name
		// keeps the overlay label meaningful.
		row.name = ent ? ent->name : std::string();
		if (row.name.empty() && ent != nullptr) {
			if (const std::string *item_name = world.tables.item_names.get(ent->item_id))
				row.name = *item_name;
		}
		row.group_id = ent ? static_cast<int32_t>(ent->group_id) : 0;
		row.alive = ent ? ent->alive : e->health > 0;
		row.infantry = e->inf.active;
		for (int c = 0; c < 3; ++c) row.pos[c] = e->pos[c];
		row.state = e->brain.f[AiBrain::kCurState];
		row.state_name = ai_state_name(row.state);
		row.alert = e->slot.bytes()[AiSlot::kAlertByte];
		row.move_mode = e->inf.move_mode;
		row.out_speed = e->brain.f[AiBrain::kOutSpeed];
		row.wp_channel = e->brain.f[AiBrain::kWpChannel];
		row.wp_node = e->brain.f[AiBrain::kWpNode];
		row.wp_distance = e->brain.f[AiBrain::kWpDistance];
		// The current target: the infantry pass reads its combat_target mirror,
		// the SM chain the brain slot (packed+1 rebase, 0 = null
		// [orig: Entity_SetAITarget @0x45d760]).
		EntityHandle target;
		if (e->inf.active) {
			target = e->inf.combat_target;
		} else if (e->brain.f[AiBrain::kTargetSlot] != 0) {
			target.packed = static_cast<uint16_t>(
					e->brain.f[AiBrain::kTargetSlot] - 1);
		}
		if (target.valid()) {
			row.target_valid = true;
			row.target_handle = target.packed;
			if (const AiEntity *te = ai.for_handle(target)) {
				for (int c = 0; c < 3; ++c) row.target_pos[c] = te->pos[c];
			} else if (const Entity *tw = world.registry.get(target)) {
				row.target_pos[0] = fixed_from_mission(tw->position.x);
				row.target_pos[1] = fixed_from_mission(tw->position.y);
				row.target_pos[2] = fixed_from_mission(tw->position.z);
			}
			if (const Entity *tw = world.registry.get(target))
				row.target_name = tw->name;
		}
		row.aim_valid = e->inf.active && e->inf.aim_valid;
		row.aim_heading = e->inf.aim_heading;
		row.aim_pitch = e->inf.aim_pitch;
		// The muzzle resolve walks the posed skeleton — engaged brains only.
		if (row.target_valid && world.pose_provider != nullptr) {
			int32_t muzzle[3] = {};
			row.muzzle_valid = world.pose_provider->resolve_muzzle_pose(
					world, e->handle, muzzle);
			if (row.muzzle_valid)
				for (int c = 0; c < 3; ++c) row.muzzle[c] = muzzle[c];
		}
		row.sight_range_q16 = e->slot.f[AiSlot::kSightRange];
		row.attack_range_q16 = e->slot.f[AiSlot::kAttackRange];
		row.combat_timer = e->brain.f[AiBrain::kCombatTimer];
		row.fire_delay = e->brain.f[AiBrain::kFireDelay];
		row.damage_timer = e->inf.damage_timer;
		row.combat_move_timer = e->inf.combat_move_timer;
		report.rows.push_back(std::move(row));
	}

	for (size_t ch = 0; ch < ai.nav.channels.size(); ++ch) {
		const NavChannel &channel = ai.nav.channels[ch];
		if (channel.count <= 0) continue;
		AiNavChannelRow crow;
		crow.index = static_cast<int32_t>(ch);
		crow.loopflag = channel.loopflag;
		const int count = std::min(channel.count,
				static_cast<int32_t>(std::size(channel.entries)));
		crow.nodes.reserve(static_cast<size_t>(count));
		for (int k = 0; k < count; ++k) {
			const NavEntry *node = ai.nav.entry(channel.entries[k]);
			if (node == nullptr) continue;
			AiNavNodeRow nrow;
			nrow.pos[0] = node->f[1];
			nrow.pos[1] = node->f[2];
			nrow.pos[2] = node->f[3];
			nrow.radius_q16 = node->f[0];
			nrow.wait_ticks = node->wait_ticks;
			crow.nodes.push_back(nrow);
		}
		auto it = followers.find(crow.index);
		crow.followers = it != followers.end() ? it->second : 0;
		report.channels.push_back(std::move(crow));
	}

	for (int g = 0; g < TriggerRelations::kGroups; ++g) {
		const TriggerRelations::GroupState *state =
				world.script.relations.group_or_null(g);
		if (state == nullptr || state->initial_count <= 0) continue;
		AiGroupRow grow;
		grow.id = g;
		grow.alert = state->alert;
		grow.initial_count = state->initial_count;
		grow.live_count = state->live_count;
		report.groups.push_back(grow);
	}

	report.counters.brain_count = ai.count();
	report.counters.scheduler_budget = ai.scheduler.budget;
	report.counters.event_count = ai.events.count();
	report.counters.unported_calls = ai.unported_calls;
    report.counters.runtime_gap_calls = world.diagnostics.total_calls();
    report.counters.runtime_gap_sites = static_cast<uint32_t>(world.diagnostics.gaps().size());
    report.runtime_gaps = world.diagnostics.gaps();
	report.counters.rel_ops = static_cast<int32_t>(ai.rel_ops.size());
	report.counters.find_target_calls = ai.find_target_calls;
	return report;
}

} // namespace opennova::world::inspect
