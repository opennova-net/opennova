// Simulation — the LOCAL PLAYER equipped-weapon binding. The pump, the
// install bake, the clip rings, the UseGun borrow, PowerThrow, and the
// presentation-event queue moved into engine/runtime/world (S7a, ADR 0028:
// world/player_weapon.h); this TU marshals installs and inputs, drains the
// event queue, snapshots the per-frame state dictionary, and routes the two
// wire request records (net-re §5.62).
#include "simulation/nova_simulation_internal.h"

#include <def/def.h> // the weapon.def flag mirrors pinned below

using namespace novasim;

// The world-side flag mirrors must stay the def parser's exact bits.
static_assert(opennova::world::weapon_flag::kNoClipsNoDraw ==
				static_cast<int32_t>(DEF_WEAPON_FLAG_NOCLIPSNODRAW),
		"kNoClipsNoDraw drifted from def.h");
static_assert(opennova::world::weapon_flag::kBurst ==
				static_cast<int32_t>(DEF_WEAPON_FLAG_BURST),
		"kBurst drifted from def.h");
static_assert(opennova::world::weapon_flag::kAuto ==
				static_cast<int32_t>(DEF_WEAPON_FLAG_AUTO),
		"kAuto drifted from def.h");
static_assert(opennova::world::weapon_flag::kPowerThrow ==
				static_cast<int32_t>(DEF_WEAPON_FLAG_POWERTHROW),
		"kPowerThrow drifted from def.h");
static_assert(opennova::world::weapon_flag::kForceCrouch ==
				static_cast<int32_t>(DEF_WEAPON_FLAG_FORCECROUCH),
		"kForceCrouch drifted from def.h");
static_assert(opennova::world::weapon_flag::kForceScoped ==
				static_cast<int32_t>(DEF_WEAPON_FLAG_FORCESCOPED),
		"kForceScoped drifted from def.h");
static_assert(opennova::world::weapon_flag2::kInset ==
				static_cast<int32_t>(DEF_WEAPON_FLAG2_INSET),
		"kInset drifted from def.h");

void Simulation::request_local_player_weapon_category(int p_category) {
	if (player_view_.binoculars_view_active) return;
	// [orig: input cases 200-210 @ 0x4e1144 -> Player_SwitchToWeaponByHandle
	//  ((action-200)*65). The binoculars-view and fire-charge input gates have no
	//  sim mechanics yet — record note.]
	if (local_weapon_.usegun_switch != LocalUseGunSwitch::kNone) return;
	if (!world_ || !local_inventory_valid_) return;
	if (p_category < 0 || p_category >= opennova::world::weapon_combo::kCategories)
		return;
	handle_weapon_switch_outcome(opennova::world::weapon_switch_to_handle(
			world_->weapons, local_inventory_,
			p_category * opennova::world::weapon_combo::kRanksPerCategory,
			local_weapon_switch_gates()));
}

void Simulation::request_local_player_weapon_cycle(int p_direction) {
	if (player_view_.binoculars_view_active) return;
	// [orig: input cases 212/214 -> Player_CycleWeaponSlot @ 0x4dfe70; the mounted-gun
	//  elevation dual-purpose leg belongs to the vehicle channel, not this walk]
	if (local_weapon_.usegun_switch != LocalUseGunSwitch::kNone) return;
	if (!world_ || !local_inventory_valid_) return;
	handle_weapon_switch_outcome(opennova::world::weapon_cycle_slot(
			world_->weapons, local_inventory_, p_direction,
			local_weapon_switch_gates()));
}

// The THIRD-PERSON model name for an ADM index, resolved through the SAME table the
// wire's index refers to (world::WeaponTable, 1-based with the engine's null row 0).
// Presentation asks by index rather than by name because that is what the entity and the
// player compact record carry; going through WeaponDatabase instead would couple two
// independent weapon.def parses with different index bases. Empty for the null row, an
// unknown index, or a weapon that authors no gfx3 — 27 of the 94 shipped rows author
// none, and drawing nothing there is correct.
// [orig: AdmDef_GetEntryByIndex @ 0x53fc80 -> WeaponDef.tpModel +0x170 @ 0x4e3cd3]
String Simulation::get_weapon_third_person_model(int p_adm_index) const {
	if (world_ == nullptr || p_adm_index <= 0 || p_adm_index > 0xFF) return String();
	const opennova::world::WeaponTableEntry *entry =
			world_->weapons.by_index(static_cast<uint8_t>(p_adm_index));
	if (entry == nullptr) return String();
	return String::utf8(entry->third_person_model.c_str());
}

Dictionary Simulation::get_local_player_aim_overlay() const {
	// The torso-bend overlay state: the nine per-segment orientations from the exact BAM
	// blends [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14],
	// converted once here to mission-euler degrees — yaw via the canonical (90 - heading),
	// pitch unchanged (the retail placement builder applies authored pitch as Ry(-pitch),
	// and MissionObjectPlacer performs the matching basis conjugation). The shell builds
	// Godot bases from these with that single-sourced conversion; delta(body class) is
	// identity by construction.
	Dictionary out;
	out["valid"] = false;
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return out;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	const opennova::world::Entity *entity =
			world_->registry.get(world_->cached.local_player);
	if (!p || !entity) return out;

	opennova::anim::AimOverlayInputs in = aim_overlay_inputs_for(*p, *entity);
	// The pitch-kick term carries the arms-dip feed (the +0x371 weapon-switch
	// window drops it 0x2800000/tick; infantry_weapon_channel owns the decay)
	// [orig: @ 0x4b5cab..0x4b5cd5]. The lean term is the sim's lean angle
	// (entity+0xB0; ramp/decay in infantry_lean_tick); roll is the slope-conform
	// visual roll (entity+0x18) and torso_roll its sixteenth-step chaser
	// (entity+0x2DC, infantry_torso_roll_tick); body_pitch is the slope-conform
	// body pitch (entity+0x90; both fed by infantry_slope_pass). pitch_blend is
	// the live recoil accumulator (entity+0x380), supplied by the shared helper.
	opennova::anim::AimOverlayAngles angles[opennova::anim::kOverlayClassCount];
	opennova::anim::compute_aim_overlay_angles(in, angles);

	PackedVector3Array packed;
	packed.resize(opennova::anim::kOverlayClassCount);
	for (int i = 0; i < opennova::anim::kOverlayClassCount; ++i) {
		packed[i] = mission_euler_from_overlay(angles[i]);
	}
	out["valid"] = true;
	out["aim_state"] = in.aim_state;
	out["mount_mode"] = static_cast<int>(in.mount_mode);
	out["mount_config_valid"] = in.mount_config_valid;
	out["mount_config"] = in.mount_config_valid ? in.mount_config : 0;
	out["body"] = mission_euler_from_overlay(
			angles[opennova::anim::kOverlayBody]);
	out["angles"] = packed;
	// The THIRD-PERSON held weapon: its own attach basis, plus retail's draw gate.
	// The basis is not one of the nine classes above — see the anim contract.
	out["weapon_attach"] = mission_euler_from_overlay(
			opennova::anim::compute_held_weapon_attach_angles(in));
	out["weapon_visible"] = local_held_weapon_visible(*entity);
	// Which of the two attach frames retail would use for this body — the same 0x80 test
	// on the weapon channel's hold state that the wire path publishes as
	// PF_HELD_WEAPON_HAND_FRAME, read here from our own infantry state so the local and
	// remote legs cannot drift. [orig: gate @ 0x4b21b6 / branch @ 0x4b220f]
	out["weapon_hand_frame"] =
			(opennova::world::infantry_anim_flags(p->inf.wpn_state) & 0x80u) != 0;
	return out;
}

// The local-player branch of retail's held-weapon draw gate — the policy is
// world::local_held_weapon_visible [orig: Entity_CanFireWeapon @ 0x4dcb10,
// the `entityPtr == g_local_player_entity` branch @ 0x4dcbcf..0x4dcc5d];
// this wrapper supplies the sim's own state aggregates.
bool Simulation::local_held_weapon_visible(
		const opennova::world::Entity &p_entity) const {
	if (!world_) return false;
	return opennova::world::local_held_weapon_visible(*world_, p_entity,
			local_weapon_, local_inventory_, player_view_.third_person);
}

// --- the local player's equipped-weapon FSM (net-re §5.62) --------------------------

void Simulation::set_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, false);
}

opennova::world::WeaponInstallData Simulation::install_data_from_dict(
		const Dictionary &p_def, const Dictionary &p_clip_seconds) {
	using opennova::world::WeaponFsmActionRow;
	opennova::world::WeaponInstallData data;
	data.name = String(p_def.get("name", String())).utf8().get_data();
	data.animadm = String(p_def.get("animadm", String())).utf8().get_data();
	data.flags = int(p_def.get("flags", 0));
	data.flags2 = int(p_def.get("flags2", 0));
	data.heat_per_shot = int(int64_t(p_def.get("heat_per_shot", 0)));
	data.heat_decay_per_tick = int(int64_t(p_def.get("heat_decay_per_tick", 0)));
	data.heat_glow_threshold = int(int64_t(p_def.get("heat_glow_threshold", 0)));
	data.scope_max_mag = float(double(p_def.get("scope_max_mag", 0.0)));
	data.attack_anim = int(int64_t(p_def.get("attack_anim", 0)));
	data.run_anim = int(int64_t(p_def.get("run_anim", 0)));
	data.clipsize = int(p_def.get("clipsize", 0));
	data.startrounds = int(p_def.get("startrounds", 0));
	// Mirror the weapon dict's ACTION rows into the def-agnostic bake inputs.
	const Array actions = p_def.get("actions", Array());
	data.rows.reserve(static_cast<size_t>(actions.size()));
	for (int i = 0; i < actions.size(); ++i) {
		const Dictionary a = actions[i];
		WeaponFsmActionRow row;
		const CharString name = String(a.get("name", "")).utf8();
		const CharString anim = String(a.get("anim", "")).utf8();
		const CharString function = String(a.get("function", "")).utf8();
		snprintf(row.name, sizeof(row.name), "%s", name.get_data());
		snprintf(row.anim, sizeof(row.anim), "%s", anim.get_data());
		snprintf(row.function, sizeof(row.function), "%s", function.get_data());
		row.delaystart = static_cast<int32_t>(int64_t(a.get("delaystart", -1)));
		row.delayend = static_cast<int32_t>(int64_t(a.get("delayend", -1)));
		// The audio/effect legs ride the bake into the pool entries
		// [orig: ActionDef_ParseScriptLine @ 0x4023c0 rows].
		const CharString soundset = String(a.get("soundset", "")).utf8();
		const CharString soundsetend = String(a.get("soundsetend", "")).utf8();
		const CharString particle = String(a.get("particle", "")).utf8();
		const CharString userpoint = String(a.get("particleuserpoint", "")).utf8();
		snprintf(row.soundset, sizeof(row.soundset), "%s", soundset.get_data());
		snprintf(row.soundsetend, sizeof(row.soundsetend), "%s", soundsetend.get_data());
		snprintf(row.particle, sizeof(row.particle), "%s", particle.get_data());
		snprintf(row.particleuserpoint, sizeof(row.particleuserpoint), "%s", userpoint.get_data());
		data.rows.push_back(row);
	}
	const Array keys = p_clip_seconds.keys();
	for (int i = 0; i < keys.size(); ++i) {
		std::vector<float> lengths;
		const Variant v = p_clip_seconds[keys[i]];
		if (v.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
			const PackedFloat32Array arr = v;
			lengths.reserve(static_cast<size_t>(arr.size()));
			for (int j = 0; j < arr.size(); ++j) lengths.push_back(arr[j]);
		} else {
			// Single-variant convenience: a plain number is a one-entry ring.
			lengths.push_back(static_cast<float>(double(v)));
		}
		data.clip_rings.emplace_back(
				std::string(String(keys[i]).utf8().get_data()),
				std::move(lengths));
	}
	return data;
}

bool Simulation::install_local_player_weapon_by_name(
		const String &p_weapon_name, bool p_preserve_slot_state) {
	if (!weapon_defs_loaded_ || p_weapon_name.is_empty() || !world_) return false;
	const std::string want(p_weapon_name.utf8().get_data());
	const DefWeaponDef *row = nullptr;
	for (size_t i = 0; i < weapon_defs_.count; ++i) {
		if (opennova::strutil::iequals(weapon_defs_.entries[i].weapon_name, want)) {
			row = &weapon_defs_.entries[i];
			break;
		}
	}
	if (row == nullptr) return false;
	// The rig's clip lengths through the sim's own mounted index — the same
	// animadm chain the FP viewmodel rides ("ak47_1st" when unauthored). A
	// missing root/rig leaves the rings empty and the 'auto' delays collapse,
	// exactly the model-never-loads behavior of the shell path.
	const char *adm = row->animadm[0] != '\0' ? row->animadm : "ak47_1st";
	weapon_clip_index_.load(
			asset_root_.is_valid() ? &asset_root_->native_index() : nullptr, adm);
	opennova::world::WeaponInstallData data;
	data.name = row->weapon_name;
	data.animadm = row->animadm;
	data.flags = row->flags;
	data.flags2 = row->flags2;
	data.heat_per_shot = row->heat_per_shot;
	data.heat_decay_per_tick = row->heat_decay_per_tick;
	data.heat_glow_threshold = row->heat_glow_threshold;
	data.scope_max_mag = row->scope_max_mag;
	data.attack_anim = row->attack_anim;
	data.run_anim = row->run_anim;
	data.clipsize = row->clipsize;
	data.startrounds = row->startrounds;
	data.rows.reserve(row->actions_count);
	for (size_t a = 0; a < row->actions_count; ++a) {
		const DefWeaponAction &act = row->actions[a];
		opennova::world::WeaponFsmActionRow r;
		snprintf(r.name, sizeof(r.name), "%s", act.name);
		snprintf(r.anim, sizeof(r.anim), "%s", act.anim);
		snprintf(r.function, sizeof(r.function), "%s", act.function);
		r.delaystart = act.delaystart;
		r.delayend = act.delayend;
		snprintf(r.soundset, sizeof(r.soundset), "%s", act.soundset);
		snprintf(r.soundsetend, sizeof(r.soundsetend), "%s", act.soundsetend);
		snprintf(r.particle, sizeof(r.particle), "%s", act.particle);
		snprintf(r.particleuserpoint, sizeof(r.particleuserpoint), "%s",
				act.particleuserpoint);
		data.rows.push_back(r);
	}
	// The exact key set the shell's clip bake fed: the idle pair + every
	// ACTION row's anim key (game_world._setup_local_player_weapon).
	const auto add_key = [&](const char *key) {
		if (key == nullptr || key[0] == '\0') return;
		const std::string lowered = opennova::strutil::to_lower(key);
		for (const auto &kv : data.clip_rings)
			if (kv.first == lowered) return;
		if (const std::vector<float> *lengths = weapon_clip_index_.lengths_for(key))
			data.clip_rings.emplace_back(lowered, *lengths);
	};
	add_key("anim_wpn_idle");
	add_key("anim_wpn_empty_idle");
	for (size_t a = 0; a < row->actions_count; ++a) add_key(row->actions[a].anim);
	opennova::world::local_weapon_install(*world_, local_weapon_, data,
			p_preserve_slot_state, /*allow_same_weapon_rebake=*/false,
			local_inventory_valid_ ? &local_inventory_ : nullptr, player_view_);
	return true;
}

void Simulation::rebake_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, true);
}

void Simulation::install_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state,
		bool p_allow_same_weapon_rebake) {
	if (!world_) return;
	opennova::world::local_weapon_install(*world_, local_weapon_,
			install_data_from_dict(p_def, p_clip_seconds),
			p_preserve_slot_state, p_allow_same_weapon_rebake,
			local_inventory_valid_ ? &local_inventory_ : nullptr, player_view_);
}

void Simulation::clear_local_player_weapon() {
	opennova::world::local_weapon_clear(local_weapon_, player_view_);
}

void Simulation::set_local_player_first_person_model_available(bool p_available) {
	local_weapon_.first_person_model_adm = 0xFF;
	if (!p_available || !world_) return;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player != nullptr)
		local_weapon_.first_person_model_adm = player->equipped_adm_index;
}

void Simulation::set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
		bool p_reload_pressed) {
	opennova::world::local_weapon_set_input(local_weapon_, player_view_,
			p_fire_held, p_fire_pressed, p_reload_pressed);
}

// One 62.5 Hz pump of the local player's slot, after the world logic tick — the
// world-side pump body with this binding routing the two wire request records.
// [orig: WeaponAction_ProcessAllEntities @0x542690 pumps every pooled entity]
void Simulation::tick_local_player_weapon() {
	if (!world_) return;
	opennova::world::LocalWeaponPumpIO io;
	io.view = &player_view_;
	io.inventory = local_inventory_valid_ ? &local_inventory_ : nullptr;
	io.is_authority = !joiner_;
	io.self_wire_handle = joiner_bridge_.self_wire_handle();
	if (joiner_ && runtime_ != nullptr) {
		// The joiner's OWN predicted round runs the wire-proxy walk with
		// the local mount exclusion dead, so resolve the carrier gate from
		// the self wire row like any decoded remote round — otherwise a
		// mounted joiner's fire stops on its own vehicle's proxy.
		io.carrier_exclusion = [this]() {
			return wire_carrier_exclusion_for(runtime_->state(),
					joiner_bridge_.self_wire_handle(), item_seat_specs_);
		};
	}
	opennova::world::local_weapon_pump_tick(*world_, local_weapon_, io);
	if (io.fired.valid && joiner_ && runtime_ != nullptr) {
		// The client-side half of Entity_FireWeaponAndSendPacket: the pump
		// predicted the round; queue the fixed C2S 0x06 descriptor. The pose
		// helper writes full XYZ, rounded Yaw/Pitch high words, and retail's
		// five low-word deltas against the live shooter pose. The runtime
		// stamps its own currentTick when accepting it.
		// [orig: @0x42A62F/@0x42A6A1..0x42A890]
		opennova::ClientFiredRound fire;
		fire.shooter_handle = joiner_bridge_.self_wire_handle();
		fire.fire_flags = io.fired.round.mode_flags;
		fire.adm_index = io.fired.adm_index;
		fire.target_handle = 0xFFFF;
		// hit_part is NOT a bare sequence: it is
		// (own roster slot << 9) | (shot_seq & 0x1FF). The host copies the
		// raw word straight into the GLOBAL word_B7C670 on the network arm
		// [orig: Server_ClientFiredRound @0x50BAA0 @0x50c2ba / @0x50c774],
		// and its own composition of the same word packs the shooter's
		// per-player record slot+20 into bits 9.. exactly this way
		// [orig: @0x50bda5 `(*((WORD*)v91 + 10) << 9) | (packet & 0x1FF)`].
		// Sending a bare sequence leaves those bits ZERO, i.e. roster slot
		// 0, so every round we fired claimed the same owner. Witnessed on
		// the wire: a retail joiner at mySlot=1 sends
		// 0x0201/0x0202/0x0203 where we sent 0x0001/0x0002/0x0003
		// (.scratch/golden/retail-coop-playerinfo-join.pcapng vs
		// opennova-joiner-profile-kit-verified.pcapng).
		// SCOPE, corrected 2026-07-26: the packing above is ROUND ATTRIBUTION
		// only. word_B7C670's entire causal reach is the round record's net id
		// [orig: CEntityManager_AllocateSlot @0x4EAAE6 adopt-or-mint, @0x4EABD5
		// stores it at the round record's +120]. An earlier revision of this
		// comment ALSO blamed it for a retail host's own first-person weapon
		// reacting to our shots; that was WRONG, and the symptom survived this
		// fix. The actual mechanism is D-NET-184 and is not packet-driven.
		fire.hit_part = opennova::pack_fired_round_hit_part(
				runtime_->local_player_slot(), io.fired.shot_seq);
		// entity+0x160 — the shooter's current AMMO-DEFINITION index, a u16 index
		// into g_ammoDefTable (stride 276). The host stores it onto the remote
		// shooter's entity [orig: the send-side read Entity_FireWeaponAndSendPacket
		// @0x42C01A; the equip-time source WeaponSlot_InitFromEntityDef @0x54673B
		// copies admEntry[1]'s low word; retail seeds 3 beside the WPN_M4AUTO
		// default in PlayerClass_InitEntity @0x4B1105, which is why a retail
		// client was captured sending 0x03]. Only the LOW BYTE crosses the wire —
		// the writer's parameter is a char @0x42a7da and the receiver reads one
		// byte @0x51347d — so indices >= 256 are untransmittable by design.
		// We shipped 0 here until 2026-07-26 (D-WPN-8).
		fire.extra_byte1 = io.fired.ammo_index >= 0
				? static_cast<uint8_t>(io.fired.ammo_index)
				: uint8_t(0);
		fire.extra_byte2 = io.fired.round.subtype;
		fire.misc_byte = io.fired.charge;
		const std::array<int32_t, 5> fire_pose = {
				io.fired.round.origin_x,
				io.fired.round.origin_y,
				io.fired.round.origin_z,
				io.fired.round.dir_yaw,
				io.fired.round.dir_pitch,
		};
		const std::array<int32_t, 5> shooter_pose = {
				io.fired.shooter_pose[0],
				io.fired.shooter_pose[1],
				io.fired.shooter_pose[2],
				io.fired.shooter_pose[3],
				io.fired.shooter_pose[4],
		};
		opennova::set_client_fired_round_pose(fire, fire_pose, shooter_pose);
		runtime_->queue_fired_round(fire);
	}
	if (io.reload.valid && runtime_ != nullptr) {
		opennova::WeaponReload reload;
		reload.entity_handle = io.reload.entity_handle;
		reload.reload_param = io.reload.reload_param;
		if (joiner_) {
			runtime_->queue_reload_request(reload);
		} else if (host_owner_.serve_and_play) {
			// Authority already performed WeaponSlot_ReloadAmmo above. The
			// loopback request exists to relay 0x49 to every client; the
			// server handler's local-connection gate prevents a second refill.
			host_loop_.client_send(
					0x25, opennova::encode_weapon_reload(reload));
		}
	}
}

Dictionary Simulation::get_local_player_weapon_state() const {
	Dictionary out;
	const opennova::world::LocalPlayerWeapon &w = local_weapon_;
	out["active"] = w.active;
	if (!w.active) return out;
	const opennova::world::WeaponSlotState &active_slot =
			*active_local_weapon_slot();
	out["current"] = active_slot.current;
	out["next"] = active_slot.next;
	out["phase"] = static_cast<int>(active_slot.phase);
	out["switch_deferred"] = w.switch_deferred_action;
	out["switch_in_flight"] = w.switch_in_flight;
	out["pending_combo"] = local_inventory_.pending_combo;
	out["anim_key"] = String::utf8(w.anim_key.c_str());
	out["anim_variant"] = w.anim_variant;
	const uint32_t anim_age_ticks = world_ && !w.anim_key.empty()
			? world_->logic_tick - w.anim_tick : 0;
	out["anim_age_ticks"] = static_cast<int64_t>(anim_age_ticks);
	out["play_serial"] = static_cast<int64_t>(w.play_serial);
	// The last-started action's audio/effect legs remain useful snapshot diagnostics;
	// ordered delivery uses drain_local_player_weapon_events().
	// [orig: ActionSlot_ExecuteActionWithEffect
	// @ 0x541860 -> ActionSlot_SpawnEffect @ 0x401f20].
	out["action_serial"] = static_cast<int64_t>(w.action_serial);
	if (w.action_started >= 0 &&
			w.action_started < opennova::world::weapon_action::kCount) {
		const opennova::world::WeaponFsmAction &act = w.def.actions[w.action_started];
		out["action_started"] = w.action_started;
		out["action_soundset"] = String::utf8(act.soundset);
		out["action_particle"] = String::utf8(act.particle);
		out["action_particle_userpoint"] = String::utf8(act.particle_userpoint);
	} else {
		out["action_started"] = -1;
		out["action_soundset"] = String();
		out["action_particle"] = String();
		out["action_particle_userpoint"] = String();
	}
	// The latest END-leg snapshot diagnostic: fire rows carry the per-shot gunshot
	// here (GS_*), reload rows the completion sound. Ordered delivery uses the batch.
	// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100 plays
	//  ActionDef+12 at the owner entity].
	out["action_end_serial"] = static_cast<int64_t>(w.action_end_serial);
	if (w.action_finished >= 0 &&
			w.action_finished < opennova::world::weapon_action::kCount) {
		out["action_end_soundset"] =
				String::utf8(w.def.actions[w.action_finished].soundsetend);
	} else {
		out["action_end_soundset"] = String();
	}
	// The PowerThrow windup for the HUD charge bar [orig: HUD_DrawPowerThrowChargeBar
	// @ 0x599830 (ex kong "HUD_DrawWeaponReloadBar" misnomer — it only draws the
	// windup): gates = def+8 sign bit, g_fireChargeStartTick != 0, ammo available;
	// the drawer derives the fill from held ticks].
	const bool windup_active =
			(w.def.flags & opennova::world::weapon_flag::kPowerThrow) != 0 &&
			w.power_throw_start_tick != 0 && world_ != nullptr &&
			(active_slot.clip > 0 || w.def.clip_capacity < 0);
	out["windup_active"] = windup_active;
	out["windup_held_ticks"] = windup_active
			? static_cast<int64_t>(world_->logic_tick - w.power_throw_start_tick)
			: static_cast<int64_t>(0);
	out["fired_serial"] = static_cast<int64_t>(w.fired_serial);
	out["dry_serial"] = static_cast<int64_t>(w.dry_serial);
	out["reload_serial"] = static_cast<int64_t>(w.reload_serial);
	out["reload_applied_serial"] =
			static_cast<int64_t>(w.reload_applied_serial);
	out["reload_received_serial"] =
			static_cast<int64_t>(w.reload_received_serial);
	out["reload_received_entity"] =
			static_cast<int64_t>(w.reload_received_entity);
	out["reload_received_param"] =
			static_cast<int64_t>(w.reload_received_param);
	out["unscope_serial"] = static_cast<int64_t>(w.unscope_serial);
	out["rescope_serial"] = static_cast<int64_t>(w.rescope_serial);
	out["clip"] = active_slot.clip;
	out["reserve"] = active_slot.reserve;
	out["kick"] = static_cast<int>(active_slot.kick);
	// Crosshair spread remains in retail's exact integer domains through the
	// presentation edge: choose the stance triplet, then add the two arithmetic
	// shifts. Category order is prone/crouch/stand; airborne or submerged forces
	// stand, and a parent attachment finally forces crouch. The +3 triplet is the
	// shared Player_CanFireWeapon verdict stamped before the body tick.
	// [orig: HUD_DrawCrosshair @0x592b07..0x592b87]
	{
		int32_t recoil_pitch = 0;
		int32_t weapon_weight_spread = 0;
		int category = 2;
		bool aimed_shot_available = false;
		const opennova::world::Entity *local = nullptr;
		const AiEntity *body = nullptr;
		if (world_ && world_->ai && world_->cached.local_player.valid()) {
			local = world_->registry.get(world_->cached.local_player);
			body = world_->ai->for_handle(world_->cached.local_player);
		}
		if (body != nullptr) {
			recoil_pitch = body->inf.recoil_pitch;
			weapon_weight_spread = body->inf.weapon_weight_spread;
			aimed_shot_available = body->inf.aimed_shot_available;
			// The shared witnessed eye-projection classifier
			// (world/round_sim.h entity_eye_below_water)
			// [orig: HUD_DrawCrosshair @0x592b35]
			const bool below_water = opennova::world::entity_eye_below_water(
					*world_, body->pos[2],
					local != nullptr ? local->eye_offset_z : 0);
			if (body->inf.stance ==
					opennova::world::InfantryState::Stance::kProne)
				category = 0;
			else if (body->inf.stance ==
					opennova::world::InfantryState::Stance::kCrouch)
				category = 1;
			if (body->inf.airborne || below_water ||
					(local != nullptr &&
					 ((local->flags | local->engine_flags) &
							(opennova::world::kEntityFlagInAir |
							 opennova::world::kEntityFlagDrowning)) != 0))
				category = 2;
		}
		if (local != nullptr && local->mounted) category = 1;
		const int row = category + (aimed_shot_available ? 3 : 0);
		const opennova::world::WeaponTableEntry *weapon =
				world_ != nullptr && local != nullptr
				? world_->weapons.by_index(local->equipped_adm_index)
				: nullptr;
		const int32_t authored_error = weapon != nullptr
				? weapon->error_fp16[row]
				: 0;
		const int32_t live_error = opennova::io::bam_add(
				authored_error,
				opennova::io::bam_add(
						opennova::io::bam_sar(recoil_pitch, 7),
						opennova::io::bam_sar(weapon_weight_spread, 7)));
		out["recoil_pitch_bam"] = recoil_pitch;
		out["weapon_weight_spread_bam"] = weapon_weight_spread;
		out["aimed_shot_available"] = aimed_shot_available;
		out["hud_spread_row"] = row;
		out["hud_spread_fp16"] = live_error;
	}
	// Weapon heat has two retail consumers with different clamps: HUD info stops
	// at 0xFFFF, while the first-person model publishes HEAT_GLOW on the signed
	// CTRL bus through the exact 0x10000 endpoint.
	// [orig: HUD_BuildEntityInfo @ 0x4B852E..0x4B854D;
	//  Player_RenderFirstPersonViewModel @ 0x4DEEC2..0x4DEEF5]
	{
		const int32_t heat = world_ != nullptr
				? opennova::world::weapon_slot_accumulated_heat(
						  w.def, active_slot,
						  static_cast<int32_t>(world_->logic_tick))
				: 0;
		out["heat"] = heat > opennova::world::weapon_heat::kFull
				? opennova::world::weapon_heat::kFull
				: heat;
		out["heat_glow"] = std::clamp(heat, 0, 0x10000);
	}
	out["borrowed_usegun_slot"] = w.usegun_slot_active;
	out["emplaced_controls_valid"] = false;
	out["emplaced_gun_yaw"] = 0;
	out["emplaced_gun_pitch"] = 0;
	if (world_ && world_->cached.local_player.valid()) {
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		const opennova::world::Entity *mount =
				local != nullptr && local->mounted &&
						local->mount_type == opennova::world::SeatType::Gunner
				? world_->registry.get(local->mount_target)
				: nullptr;
		EmplacedWeaponControls emplaced;
		if (mount != nullptr &&
				mount->primary_weapon_owner == local->handle &&
				emplaced_weapon_controls_for(
						*world_, ai_.get(), *mount, emplaced)) {
			out["emplaced_controls_valid"] = true;
			out["emplaced_gun_yaw"] =
					static_cast<int>(emplaced.gun_yaw);
			out["emplaced_gun_pitch"] =
					static_cast<int>(emplaced.gun_pitch);
		}
	}
	// Read-only diagnostics for the local FIRE -> RoundData_AddRound seam. The last
	// row lets parity tests pin the observed tag-2 mode byte without exposing mutable
	// ring state. [orig: ((MountSlot.clip & 3) << 4) | 2 sampled before consume
	// @ WeaponAction_Fire 0x542c11 / 0x542c75].
	out["round_ring_count"] = world_ ? world_->rounds.count : 0;
	if (world_ && world_->rounds.count > 0) {
		const int last = world_->rounds.cursor == 0
				? opennova::world::RoundRing::kCapacity - 1
				: world_->rounds.cursor - 1;
		const opennova::world::RoundEvent &round = world_->rounds.records[
				static_cast<std::size_t>(last)];
		out["last_round_flags"] = round.mode_flags;
		out["last_round_subtype"] = round.subtype;
		out["last_round_slot_byte"] = round.slot_byte;
		out["last_round_seq"] = round.shot_seq;
	}
	// The 3P body's weapon channel (the entity's secondary AnimMap channel): the clip key
	// + its own playhead for the shell's mask-bone override. The key remains populated
	// when the state id matches the primary because the two playheads are independent.
	// Empty means the override gate is off (weapon in hands + allowed mount class +
	// primary state flag 0x40).
	// [orig: gate @ 0x4b14a7; producer @ 0x4b5dad; world-wac-ai-re.md §14.8].
	out["body_anim_key"] = String();
	out["body_anim_phase"] = 0;
	if (world_ && world_->ai && world_->cached.local_player.valid()) {
		const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
		const opennova::world::Entity *entity =
				world_->registry.get(world_->cached.local_player);
		const bool blocked_mount =
				entity != nullptr && mount_blocks_weapon_channel(*entity);
		if (p && entity && opennova::world::infantry_weapon_channel_visible(
					p->inf, w.active, blocked_mount)) {
			out["body_anim_key"] = infantry_anim_key(p->inf.wpn_state);
			out["body_anim_phase"] = p->inf.wpn_clip_phase;
		}
	}
	return out;
}

Array Simulation::drain_local_player_weapon_events() {
	Array out;
	const uint32_t now = world_ ? world_->logic_tick : 0;
	for (const opennova::world::WeaponPresentationEvent &event :
			local_weapon_.events) {
		Dictionary row;
		// Unsigned subtraction intentionally preserves age across logic-tick wrap.
		row["age_ticks"] = static_cast<int64_t>(now - event.tick);
		// mission (x,y,z) -> Godot (x, z, -y) — the shooter position the
		// world-side record carries in mission space.
		row["world_position"] = Vector3(event.world_position.x,
				event.world_position.z, -event.world_position.y);
		row["anim_key"] = String::utf8(event.anim_key.c_str());
		row["anim_variant"] = event.anim_variant;
		row["action_started"] = event.action_started;
		row["action_soundset"] = String::utf8(event.action_soundset.c_str());
		row["action_particle"] = String::utf8(event.action_particle.c_str());
		row["action_particle_userpoint"] =
				String::utf8(event.action_particle_userpoint.c_str());
		row["scope_settled"] = event.scope_settled;
		row["third_person"] = event.third_person;
		row["vehicle_attack_context"] = event.vehicle_attack_context;
		row["action_finished"] = event.action_finished;
		row["action_end_soundset"] =
				String::utf8(event.action_end_soundset.c_str());
		row["action_effect"] = event.action_effect;
		row["effect_particle"] = String::utf8(event.effect_particle.c_str());
		row["effect_particle_userpoint"] =
				String::utf8(event.effect_particle_userpoint.c_str());
		row["switch_to_weapon"] = String::utf8(event.switch_to_weapon.c_str());
		row["clear_weapon"] = event.clear_weapon;
		row["preserve_slot_state"] = event.preserve_slot_state;
		row["switch_denied"] = event.switch_denied;
		out.push_back(row);
	}
	local_weapon_.events.clear();
	return out;
}
