// NovaSimulation — the LOCAL PLAYER equipped-weapon cluster: the weapon action
// FSM pump, install/def bake, clip-variant rings, switch outcomes, and the
// presentation-event drains (net-re §5.62).
#include "simulation/nova_simulation_internal.h"

#include <def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*

using namespace novasim;

opennova::world::WeaponSwitchGates NovaSimulation::local_weapon_switch_gates() const {
	opennova::world::WeaponSwitchGates gates;
	const opennova::world::Entity *e =
			world_ ? world_->registry.get(world_->cached.local_player) : nullptr;
	if (e != nullptr && e->mounted) {
		// [orig: the parentSlot {2,3,5} stance gate @ 0x4e0192 — our SeatType enum
		//  carries the original's raw values: Controller=2, Gunner=3, Driver=5;
		//  passengers (1) keep switching. The equip-commit defer gates {2,3} only
		//  @ 0x4dd6fc.]
		const auto t = e->mount_type;
		gates.seat_blocked = t == opennova::world::SeatType::Controller ||
		                     t == opennova::world::SeatType::Gunner ||
		                     t == opennova::world::SeatType::Driver;
		gates.equip_blocked = t == opennova::world::SeatType::Controller ||
		                      t == opennova::world::SeatType::Gunner;
	}
	gates.equipped_valid =
			local_inventory_.equipped_combo >= 0 &&
			local_inventory_.slot(local_inventory_.equipped_combo) != nullptr &&
			local_inventory_.slot(local_inventory_.equipped_combo)->adm_index >= 0;
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	gates.equipped_action = weapon_active_ ? active_slot->current
	                                       : opennova::world::weapon_action::kIdle;
	return gates;
}

void NovaSimulation::commit_pending_weapon_switch() {
	// The pending -> equipped commit [orig: the switchfrom/switchrank completion
	// consumes g_pendingWeaponSlot; EquippedSlot swap + the equippedAdmIndex stamp
	// @ 0x4dd727; the FP model re-resolve runs shell-side off the event].
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	nvg_scope_restore_ = false;
	if (!world_ || !local_inventory_valid_) return;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	const int32_t combo = local_inventory_.pending_combo;
	const opennova::world::WeaponInventorySlot *slot = local_inventory_.slot(combo);
	if (slot == nullptr || slot->adm_index < 0) return;
	local_inventory_.equipped_combo = combo;
	// The entity stamp is the only entity-DEPENDENT half. Retail's selection has no
	// entity-existence precondition on the PRESENTATION half — the tail of every 0x5A
	// grant runs Player_SelectWeaponSlot against the pool it just rebuilt and the FP
	// viewmodel is re-resolved by a per-frame consumer off EquippedSlot [orig:
	// NapiNPClientMsg_HandleWeaponLoadoutSync tail @0x4296E3; Player_SelectWeaponSlot
	// @0x4DD680 restamps equippedAdmIndex @0x4dd727/@0x4dd7f5;
	// Player_RenderFirstPersonViewModel @0x4DED60]. A joiner applies its grant BEFORE
	// L exists, so dropping the notification here left the viewmodel and the weapon FSM
	// running the submitted weapon while the entity and the wire followed the granted
	// one. Latch it instead and replay exactly one event at the joiner spawn block.
	if (e != nullptr) e->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
	const opennova::world::WeaponTableEntry *def =
			world_->weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	weapon_start_in_switchto_ = true;
	if (e == nullptr) {
		weapon_presentation_pending_ = true;
		return;
	}
	// The entity was present: emit directly, exactly as before (the manual-switch and
	// armory paths are unchanged), and drop any latch so it cannot replay a duplicate.
	weapon_presentation_pending_ = false;
	PendingWeaponEvent event;
	event.tick = world_->logic_tick;
	event.world_position = get_local_player_position();
	event.switch_to_weapon =
			def != nullptr ? String::utf8(def->name.c_str()) : String();
	pending_weapon_events_.push_back(std::move(event));
}

void NovaSimulation::request_local_player_weapon_category(int p_category) {
	if (player_view_.binoculars_view_active) return;
	// [orig: input cases 200-210 @ 0x4e1144 -> Player_SwitchToWeaponByHandle
	//  ((action-200)*65). The binoculars-view and fire-charge input gates have no
	//  sim mechanics yet — record note.]
	if (local_usegun_switch_ != LocalUseGunSwitch::kNone) return;
	if (!world_ || !local_inventory_valid_) return;
	if (p_category < 0 || p_category >= opennova::world::weapon_combo::kCategories)
		return;
	const opennova::world::WeaponSwitchOutcome out = opennova::world::weapon_switch_to_handle(
			world_->weapons, local_inventory_,
			p_category * opennova::world::weapon_combo::kRanksPerCategory,
			local_weapon_switch_gates());
	handle_weapon_switch_outcome(out);
}

void NovaSimulation::request_local_player_weapon_cycle(int p_direction) {
	if (player_view_.binoculars_view_active) return;
	// [orig: input cases 212/214 -> Player_CycleWeaponSlot @ 0x4dfe70; the mounted-gun
	//  elevation dual-purpose leg belongs to the vehicle channel, not this walk]
	if (local_usegun_switch_ != LocalUseGunSwitch::kNone) return;
	if (!world_ || !local_inventory_valid_) return;
	const opennova::world::WeaponSwitchOutcome out = opennova::world::weapon_cycle_slot(
			world_->weapons, local_inventory_, p_direction, local_weapon_switch_gates());
	handle_weapon_switch_outcome(out);
}

void NovaSimulation::handle_weapon_switch_outcome(
		const opennova::world::WeaponSwitchOutcome &p_out) {
	switch (p_out.kind) {
		case opennova::world::WeaponSwitchOutcome::kDeny: {
			// [orig: PlaySoundOnDedicatedServer(dword_24E08C4) @ 0x4e0354]
			PendingWeaponEvent event;
			event.tick = world_ ? world_->logic_tick : 0;
			event.world_position = get_local_player_position();
			event.switch_denied = true;
			pending_weapon_events_.push_back(std::move(event));
			break;
		}
		case opennova::world::WeaponSwitchOutcome::kMount: {
			// [orig: Player_MountWeaponSlot @ 0x4dfa40 — pending already stamped by
			//  the walk; the OUTGOING slot's FSM plays SWITCHRANK (same category) or
			//  SWITCHFROM (cross category) and its completion commits]
			if (!weapon_active_) {
				commit_pending_weapon_switch();
				break;
			}
			weapon_switch_in_flight_ = true;
			opennova::world::WeaponSlotState *active_slot =
					active_local_weapon_slot();
			const int32_t action = p_out.same_category
					? opennova::world::weapon_action::kSwitchRank
					: opennova::world::weapon_action::kSwitchFrom;
			if (active_slot->current ==
					opennova::world::weapon_action::kSwitchTo) {
				// The witnessed writer refuses during SWITCHTO. Unlike the original
				// dispatcher, this shell supplies one press edge, so retain it beside
				// the already-stamped pending combo and keep restoring next after
				// SWITCHTO's delay-start initializer writes its resume action.
				weapon_switch_deferred_action_ = action;
				active_slot->next = action;
			} else if (active_slot->next ==
					opennova::world::weapon_action::kSwitchTo) {
				// The draw is queued but has not entered yet. Preserve it; the
				// post-tick latch below attaches the requested outgoing action.
				weapon_switch_deferred_action_ = action;
			} else if (p_out.same_category) {
				weapon_switch_deferred_action_ = -1;
				opennova::world::weapon_fsm_queue_switch_rank(*active_slot);
			} else {
				weapon_switch_deferred_action_ = -1;
				opennova::world::weapon_fsm_queue_switch_from(*active_slot);
			}
			break;
		}
		default:
			break;
	}
}

// The THIRD-PERSON model name for an ADM index, resolved through the SAME table the
// wire's index refers to (world::WeaponTable, 1-based with the engine's null row 0).
// Presentation asks by index rather than by name because that is what the entity and the
// player compact record carry; going through NovaWeaponDatabase instead would couple two
// independent weapon.def parses with different index bases. Empty for the null row, an
// unknown index, or a weapon that authors no gfx3 — 27 of the 94 shipped rows author
// none, and drawing nothing there is correct.
// [orig: AdmDef_GetEntryByIndex @ 0x53fc80 -> WeaponDef.tpModel +0x170 @ 0x4e3cd3]
String NovaSimulation::get_weapon_third_person_model(int p_adm_index) const {
	if (world_ == nullptr || p_adm_index <= 0 || p_adm_index > 0xFF) return String();
	const opennova::world::WeaponTableEntry *entry =
			world_->weapons.by_index(static_cast<uint8_t>(p_adm_index));
	if (entry == nullptr) return String();
	return String::utf8(entry->third_person_model.c_str());
}

Dictionary NovaSimulation::get_local_player_aim_overlay() const {
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

// The local-player branch of retail's held-weapon draw gate: the weapon model is drawn
// iff the soldier may FIRE it. One predicate serves both, which is why a dead, seated or
// dry-magazine player simply has no gun in his hands.
// [orig: Entity_CanFireWeapon @ 0x4dcb10 — the `entityPtr == g_local_player_entity`
//  branch @ 0x4dcbcf..0x4dcc5d]
bool NovaSimulation::local_held_weapon_visible(
		const opennova::world::Entity &p_entity) const {
	if ((p_entity.flags & 2u) != 0) return false;          // dead [orig: @0x4dcb22]
	if (!weapon_active_) return false;                     // no EquippedSlot [orig: @0x4dcbcf]
	const opennova::world::WeaponInventorySlot *slot =
			local_inventory_.slot(local_inventory_.equipped_combo);
	if (slot == nullptr || slot->adm_index < 0) return false;
	const opennova::world::WeaponTableEntry *def =
			world_ ? world_->weapons.by_index(
							 static_cast<uint8_t>(slot->adm_index))
			       : nullptr;
	if (def == nullptr) return false;                      // no Def [orig: @0x4dcbda]
	// The ammo leg, for defs that carry the flag: an empty pool hides the weapon.
	// [orig: @0x4dcbea -> Entity_GetScoreValueBySlotType @0x5406E0, ported as
	//  weapon_pool_get]
	if ((def->flags & DEF_WEAPON_FLAG_NOCLIPSNODRAW) != 0 &&
			opennova::world::weapon_pool_get(local_inventory_, def->ammo_class_id) == 0 &&
			slot->clip <= 0)
		return false;
	// A weapon with no FIRST-person model is hidden on your OWN body even though every
	// observer still sees it — retail asymmetry, not a bug. [orig: @0x4dcc32]
	if (!def->has_first_person_model_reference) return false;
	if (!p_entity.mounted) return true;                    // [orig: @0x4dcc42]
	// Seat rule, LOCAL flavour: control and driver always hide; the gunner seat hides
	// only while the third-person camera is up. (The remote flavour hides all three —
	// that is what seat_type_blocks_weapon_channel models.) [orig: @0x4dcc44..0x4dcc5d]
	switch (p_entity.mount_type) {
		case opennova::world::SeatType::Controller:
		case opennova::world::SeatType::Driver:
			return false;
		case opennova::world::SeatType::Gunner:
			return !player_view_.third_person;
		default:
			return true; // passenger keeps its weapon
	}
}

// --- the local player's equipped-weapon FSM (net-re §5.62) --------------------------

NovaSimulation::WeaponClipRing *NovaSimulation::weapon_ring_for(const String &p_key_lower) {
	for (std::pair<String, WeaponClipRing> &kv : weapon_clip_rings_) {
		if (kv.first == p_key_lower) return &kv.second;
	}
	return nullptr;
}

float NovaSimulation::weapon_ring_take_length(const char *p_key) {
	// Serve the ring head's duration, then advance the head — the consuming read
	// [orig: Anim_GetDurationTicks @ 0x53ee10: currentEntry = *slot;
	//  *slot = *(currentEntry + 36); duration from currentEntry's data].
	WeaponClipRing *ring = weapon_ring_for(String::utf8(p_key).to_lower());
	if (ring == nullptr || ring->lengths.is_empty()) return -1.0f;
	const float served = ring->lengths[ring->head];
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
}

int NovaSimulation::weapon_ring_take_variant(const String &p_key) {
	// Serve the head as the PLAYED variant and advance — the play latch: playback
	// follows the served entry while the ring moves on [orig: AnimMap_PlayAnimBySlot
	// @ 0x40bda0: animEntry = slot[i]; slot[i] = next; animState+68 = animEntry].
	WeaponClipRing *ring = weapon_ring_for(p_key.to_lower());
	if (ring == nullptr || ring->lengths.is_empty()) return 0;
	const int served = ring->head;
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
}

void NovaSimulation::set_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, false);
}

void NovaSimulation::rebake_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, true);
}

void NovaSimulation::install_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state,
		bool p_allow_same_weapon_rebake) {
	using opennova::world::WeaponFsmActionRow;
	// The FP model resolve re-installs the SAME weapon once its viewmodel (and
	// .adm clip lengths) finish loading. That resolve is a render-side consumer
	// in retail with no access to the action slot [orig: the per-frame FP model
	// resolve @ 0x4ded60 vs the mount's slot state in Player_MountWeaponSlot
	// @ 0x4dfa40], so an explicitly requested same-name rebake only refreshes
	// the def and rings.
	// Resetting the slot here instead destroyed a queued/holstering SWITCHFROM
	// whenever the late viewmodel install raced a switch request: the completion
	// never fired, commit_pending_weapon_switch never ran, and the FSM def
	// desynced from equipped_adm_index (the rifle then fired the previous
	// weapon's ammo).
	const String incoming_name = p_def.get("name", String());
	const bool same_weapon_rebake = p_allow_same_weapon_rebake &&
			weapon_active_ && !weapon_start_in_switchto_ &&
			!incoming_name.is_empty() &&
			incoming_name.nocasecmp_to(weapon_def_name_) == 0;
	// A real mount invalidates the one-shot scope restore latch. The late
	// first-person-model rebake is render-side only and must not mutate view state.
	if (!same_weapon_rebake) nvg_scope_restore_ = false;
	// A mount is a new presentation epoch: no payload from the previous weapon may
	// cross this seam, even though its strings were copied into the pending records.
	// The same-weapon rebake is NOT an epoch — undrained records (including a
	// racing switch commit or deny) must survive it.
	if (!same_weapon_rebake) pending_weapon_events_.clear();
	// Mirror the weapon dict's ACTION rows into the def-agnostic bake inputs.
	std::vector<WeaponFsmActionRow> rows;
	const Array actions = p_def.get("actions", Array());
	rows.reserve(static_cast<size_t>(actions.size()));
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
		rows.push_back(row);
	}
	// Clip lengths come from the loaded viewmodel's .adm (seconds) as per-key VARIANT
	// arrays; they seed the slot rings the bake and the play events consume
	// serve-then-advance [orig: the animState slot heads (+72) built by
	// AnimMap_RegisterBoneNode @ 0x40c2d0; Anim_GetDurationTicks @ 0x53ee10].
	weapon_clip_rings_.clear();
	weapon_anim_variant_ = 0;
	{
		const Array keys = p_clip_seconds.keys();
		for (int i = 0; i < keys.size(); ++i) {
			WeaponClipRing ring;
			const Variant v = p_clip_seconds[keys[i]];
			if (v.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
				ring.lengths = v;
			} else {
				// Single-variant convenience: a plain number is a one-entry ring.
				ring.lengths.push_back(static_cast<float>(double(v)));
			}
			if (ring.lengths.is_empty()) continue;
			weapon_clip_rings_.emplace_back(String(keys[i]).to_lower(), ring);
		}
	}
	// The bake probes existence as a pure lookup and reads durations ring-wise —
	// one consuming read per 'auto' field [orig: Anim_InitActions @ 0x541fa0;
	// the lookup @ 0x5421ae, the reads @ 0x5421c5 / @ 0x5421d8].
	const auto resolve_fn = [](void *p_ctx, const char *key) -> int {
		NovaSimulation *self = static_cast<NovaSimulation *>(p_ctx);
		return self->weapon_ring_for(String::utf8(key).to_lower()) != nullptr ? 1 : 0;
	};
	const auto clip_fn = [](void *p_ctx, const char *key) -> float {
		return static_cast<NovaSimulation *>(p_ctx)->weapon_ring_take_length(key);
	};
	weapon_def_ = opennova::world::WeaponFsmDef{};
	opennova::world::weapon_fsm_bake(rows.data(), rows.size(), resolve_fn, clip_fn, this,
			weapon_def_);
	const int flags = int(p_def.get("flags", 0));
	weapon_def_.auto_fire = (flags & DEF_WEAPON_FLAG_AUTO) != 0; // [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0]
	weapon_def_.burst3 = (flags & DEF_WEAPON_FLAG_BURST) != 0;     // [orig: WeaponAction_Fire @ 0x542c8a]
	weapon_def_.flags = flags;                    // raw mask: the scope gate + fov policy read it
	weapon_def_.flags2 = int(p_def.get("flags2", 0)); // Inset (0x200) picks the 7-step ease
	// The heat model [orig: WeaponDef +0x36C/+0x370/+0x374]. Absent keys leave 0,
	// which disables the model exactly as the original's zero-init does.
	weapon_def_.heat_per_shot = int(int64_t(p_def.get("heat_per_shot", 0)));
	weapon_def_.heat_decay_per_tick = int(int64_t(p_def.get("heat_decay_per_tick", 0)));
	weapon_def_.heat_glow_threshold = int(int64_t(p_def.get("heat_glow_threshold", 0)));
	weapon_scope_max_mag_ = float(double(p_def.get("scope_max_mag", 0.0)));
	// The 3P fire attack-stamp kind [orig: weapon.def attack_anim -> the AdmDefs record
	// +0xA8; world-wac-ai-re.md §14.8.4]. The sibling special_hold (+0xA4) is NOT cached
	// here: the body updater re-reads it from the ADM table by the posed entity's own
	// equipped index every selection pass [orig: @ 0x4b5dba], which is the single source
	// both the local player and every remote player resolve through.
	weapon_attack_kind_ = int(int64_t(p_def.get("attack_anim", 0)));
	// The run-gait class [orig: 'run_anim' -> AdmDefs +0xAC; promotion @ 0x4b729d] and
	// ForceCrouch (0x40000): idle_mortar promotion + stance-change refusal.
	weapon_run_anim_ = int(int64_t(p_def.get("run_anim", 0)));
	weapon_force_crouch_ = (flags & DEF_WEAPON_FLAG_FORCECROUCH) != 0;
	// A held-AnimMap CHANGE advances a binding serial; the local InfantryState observes
	// that edge pre-tick and stamps its own 20-tick arms-dip window. Compare the
	// resolved map identity, not the weapon name: two weapon records sharing one
	// AnimMap do NOT dip. A fresh mount advances even when the map key is empty.
	// [orig: previous/current AdmDefs record +0 comparison @0x4b46d0..0x4b4701].
	const String anim_map = p_def.get("animadm", String());
	if (!weapon_active_ || anim_map.nocasecmp_to(weapon_anim_map_) != 0) {
		weapon_anim_map_ = anim_map;
		++weapon_anim_map_serial_;
		if (weapon_anim_map_serial_ == 0) ++weapon_anim_map_serial_; // reserve 0 = none
	}
	const int clipsize = int(p_def.get("clipsize", 0));
	weapon_def_.clip_capacity = clipsize > 0 ? clipsize : -1; // no clipsize key = no clip tracking
	if (same_weapon_rebake) {
		// Def + rings rebaked above; the live action slot, serials, input
		// latches, scope state, and charge state all continue untouched.
		weapon_def_name_ = incoming_name;
		return;
	}
	// A queued or in-flight SWITCHTO survives the per-equip reset — the switch flow
	// installs twice (dict-only, then with the rebuilt viewmodel's clip lengths) and
	// the draw-in must reach the second install (D-WPN-6 family artifact).
	const bool carry_switchto = !p_preserve_slot_state && weapon_active_ &&
			(weapon_slot_.next == opennova::world::weapon_action::kSwitchTo ||
			 weapon_slot_.current == opennova::world::weapon_action::kSwitchTo);
	if (!p_preserve_slot_state) {
		weapon_slot_ = opennova::world::WeaponSlotState{};
		// Ammo comes from the slot pool when the installed def IS the equipped
		// inventory slot: clip = the slot's loaded rounds, reserve = the def's
		// ammo-class pool [orig: MountSlot+0x10 +
		// Entity_GetScoreValueBySlotType @0x5406e0].
		bool ammo_from_inventory = false;
		if (local_inventory_valid_ && world_ != nullptr) {
			const opennova::world::WeaponInventorySlot *eq =
					local_inventory_.slot(local_inventory_.equipped_combo);
			const opennova::world::WeaponTableEntry *def =
					(eq != nullptr && eq->adm_index >= 0)
							? world_->weapons.by_index(
									static_cast<uint8_t>(eq->adm_index))
							: nullptr;
			if (def != nullptr &&
					incoming_name.nocasecmp_to(
							String::utf8(def->name.c_str())) == 0) {
				weapon_slot_.clip = eq->clip;
				weapon_slot_.reserve = opennova::world::weapon_pool_get(
						local_inventory_, def->ammo_class_id);
				ammo_from_inventory = true;
			}
		}
		if (!ammo_from_inventory) {
			// Fresh slot: full magazine + the def's carried reserve (the interim
			// ammo default for inventory-less installs — D-WPN-7).
			weapon_slot_.clip = clipsize > 0 ? clipsize : 0;
			weapon_slot_.reserve = int(p_def.get("startrounds", 0));
		}
		// A commit-driven inventory install starts in SWITCHTO. A UseGun commit
		// already queued SWITCHTO on the selected parent/personal slot.
		if (weapon_start_in_switchto_ || carry_switchto) {
			weapon_slot_.next =
					opennova::world::weapon_action::kSwitchTo;
			weapon_start_in_switchto_ = false;
		}
	}
	// A walk outcome that mounted but has not committed yet (its outgoing
	// SWITCHFROM was displaced by this mount) resumes through the deferred latch
	// once the draw completes, so the pending combo still commits.
	if (weapon_switch_in_flight_)
		weapon_switch_deferred_action_ = opennova::world::weapon_action::kSwitchFrom;
	// The mount is the charge epoch [orig: Player_SwitchToWeaponByHandle zeroes
	// g_fireChargeStartTick on the walk, before the mount].
	power_throw_start_tick_ = 0;
	pending_throw_charge_ = 0;
	weapon_play_serial_ = 0;
	weapon_anim_key_ = String();
	weapon_anim_variant_ = 0;
	weapon_anim_tick_ = 0;
	weapon_fired_serial_ = weapon_dry_serial_ = weapon_reload_serial_ = 0;
	weapon_reload_applied_serial_ = 0;
	weapon_reload_received_serial_ = 0;
	weapon_reload_received_entity_ =
			opennova::world::EntityHandle::kInvalid;
	weapon_reload_received_param_ = 0;
	weapon_unscope_serial_ = weapon_rescope_serial_ = 0;
	weapon_action_serial_ = 0;
	weapon_action_started_ = -1;
	weapon_action_end_serial_ = 0;
	weapon_action_finished_ = -1;
	weapon_fire_held_ = weapon_fire_pressed_ = weapon_reload_pressed_ = false;
	// A fresh mount starts at the hip with the interp cleared and the hipfire
	// latch reset [orig: Player_MountWeaponSlot zeroes the view biases @ 0x4dfbcf].
	player_view_.scope_engaged = false;
	player_view_.scope_step = 0;
	player_view_.ease_steps = opennova::world::kScopeEaseSteps;
	player_view_.scope_hipfire = true;
	weapon_def_name_ = incoming_name;
	weapon_active_ = true;
}

void NovaSimulation::clear_local_player_weapon() {
	nvg_scope_restore_ = false;
	weapon_active_ = false;
	weapon_def_name_ = String();
	power_throw_start_tick_ = 0;
	pending_throw_charge_ = 0;
	local_first_person_model_adm_ = 0xFF;
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	pending_weapon_events_.clear();
	weapon_fire_held_ = false;
	weapon_fire_pressed_ = false;
	weapon_reload_pressed_ = false;
	weapon_clip_rings_.clear();
	weapon_anim_variant_ = 0;
	weapon_anim_tick_ = 0;
	player_view_.scope_engaged = false;
	player_view_.scope_step = 0;
	player_view_.ease_steps = opennova::world::kScopeEaseSteps;
	player_view_.scope_hipfire = true;
	weapon_attack_kind_ = 0;
	weapon_run_anim_ = 0;
	weapon_force_crouch_ = false;
	weapon_anim_map_ = String();
}

void NovaSimulation::set_local_player_first_person_model_available(bool p_available) {
	local_first_person_model_adm_ = 0xFF;
	if (!p_available || !world_) return;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player != nullptr)
		local_first_person_model_adm_ = player->equipped_adm_index;
}

void NovaSimulation::set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
		bool p_reload_pressed) {
	if (player_view_.binoculars_view_active) {
		weapon_fire_held_ = false;
		weapon_fire_pressed_ = false;
		weapon_reload_pressed_ = false;
		return;
	}
	weapon_fire_held_ = p_fire_held;
	weapon_fire_pressed_ = weapon_fire_pressed_ || p_fire_pressed; // latch until consumed
	weapon_reload_pressed_ = weapon_reload_pressed_ || p_reload_pressed;
}


// One 62.5 Hz pump of the local player's slot, after the world logic tick. The world
// tick now owns the parallel NPC UseGun parent-slot pump; this binding method remains the
// first-person player's presentation/input seam.
// [orig: WeaponAction_ProcessAllEntities @0x542690 pumps every pooled entity]
void NovaSimulation::tick_local_player_weapon() {
	if (!world_ || !world_->cached.local_player.valid()) return;
	sync_local_usegun_weapon_transition();
	if (!weapon_active_) return;
	opennova::world::WeaponSlotState &active_slot =
			*active_local_weapon_slot();
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	const bool player_alive = player != nullptr && player->alive &&
			player->health > 0;
	const bool usegun_switch_pending =
			local_usegun_switch_ != LocalUseGunSwitch::kNone;
	if (!player_alive && !usegun_switch_pending) {
		active_slot.refire_queued = false;
		power_throw_start_tick_ = 0;
		pending_throw_charge_ = 0;
		weapon_fire_pressed_ = false;
		weapon_reload_pressed_ = false;
		return;
	}
	// Capture the outgoing slot identity. A switch completion below changes the
	// active selection, but this tick's ammo bridge still belongs to the slot the
	// FSM actually pumped.
	const bool borrowed_usegun_slot = local_usegun_slot_active_;
	opennova::world::WeaponFsmInputs in;
	const bool accept_weapon_input = player_alive && !usegun_switch_pending;
	in.fire_held = accept_weapon_input && weapon_fire_held_;
	in.fire_pressed = accept_weapon_input && weapon_fire_pressed_;
	// PowerThrow: the press never fires — it starts the windup; the release
	// converts the held time into the charge byte and fires. [orig: press gate
	// @ 0x4e08fd (def Flags sign bit 0x80000000, fireable + ammo ->
	// g_fireChargeStartTick = tick), release @ 0x4e07e9 -> WeaponSlot_RequestFire
	// with the computed charge; world-wac-ai-re §27.]
	bool power_throw_release = false;
	if ((weapon_def_.flags & DEF_WEAPON_FLAG_POWERTHROW) != 0) {
		if (!accept_weapon_input) {
			power_throw_start_tick_ = 0;
			pending_throw_charge_ = 0;
		} else if (weapon_fire_held_ || weapon_fire_pressed_) {
			// The windup refuses while a switch action runs OR is queued — the
			// press gate's fireable term, not just the current action [orig: the
			// fireable check @ 0x4e08fd]. Without the queued/deferred legs a
			// press landing inside the draw-in latched a windup whose release
			// the FSM then refused, leaking the charge byte onto a later shot.
			const bool fireable =
					active_slot.current == opennova::world::weapon_action::kIdle &&
					active_slot.next == opennova::world::weapon_action::kIdle &&
					!weapon_switch_in_flight_ && weapon_switch_deferred_action_ < 0;
			const bool has_ammo =
					active_slot.clip > 0 || weapon_def_.clip_capacity < 0;
			if (power_throw_start_tick_ == 0 && fireable && has_ammo)
				power_throw_start_tick_ = world_->logic_tick;
			in.fire_held = false;
			in.fire_pressed = false;
		} else if (power_throw_start_tick_ != 0) {
			const int32_t held = static_cast<int32_t>(
					world_->logic_tick - power_throw_start_tick_);
			pending_throw_charge_ =
					opennova::world::power_throw_charge_from_hold(held);
			power_throw_start_tick_ = 0;
			in.fire_pressed = true;
			in.fire_held = false;
			power_throw_release = true;
		}
	} else {
		power_throw_start_tick_ = 0;
	}
	// The dispatch gate runs here now: the raw reload edge is refused on a full
	// magazine or an empty reserve [orig: input case 0xD3 @ 0x4e0420].
	in.reload_pressed = accept_weapon_input && weapon_reload_pressed_ &&
			opennova::world::weapon_fsm_reload_allowed(
					weapon_def_, active_slot);
	in.is_local = true;
	in.is_authority = !joiner_; // the joiner defers the refill to the §5.58 round-trip
	in.auto_reload = true;      // [orig: g_autoReloadEnabled @ 0x24D2118, default on]
	// The weapon FSM consumes the promoted/settled scope bit, not the raw
	// requested-engagement bit. Player_UpdatePerFrame runs before the weapon
	// pump in retail and only promotes g_weaponScopeActive after the ease has
	// completed [orig: promoter @ 0x4de4f7; weapon pump @ 0x526786].
	in.scope_active = player_view_.scope_engaged &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	in.instant_emplaced_switch = local_usegun_switch_is_instant();
	// The heat window is a deadline against the logic tick, not a stored level.
	// `submerged` stays false: the sim has no per-entity water test at the weapon
	// site yet, and above water is what the runtime actually plays (D-WPN-29).
	// [orig: current_tick @ 0x24C1968; the water gate @ 0x54101c]
	in.current_tick = static_cast<int32_t>(world_->logic_tick);
	if (!accept_weapon_input) active_slot.refire_queued = false;
	opennova::world::WeaponFsmEvents ev;
	opennova::world::weapon_fsm_tick(
			weapon_def_, active_slot, in, ev);
	// A release whose fire request the FSM refused must not leave the charge
	// latched for a later unrelated shot — the charge byte is consumed by the
	// very fire it triggers [orig: descriptor +20 consume @ 0x4ec5bb].
	if (power_throw_release && !ev.fired &&
			active_slot.current != opennova::world::weapon_action::kFire &&
			active_slot.next != opennova::world::weapon_action::kFire)
		pending_throw_charge_ = 0;
	// SWITCHTO seeds next=prev at the end of its delay-start phase. Reapply the
	// retained one-shot after every draw tick so its eventual transition performs
	// the pending inventory handoff without requiring another key press.
	if (weapon_switch_deferred_action_ >= 0) {
		if (active_slot.current == weapon_switch_deferred_action_) {
			weapon_switch_deferred_action_ = -1;
		} else {
			active_slot.next = weapon_switch_deferred_action_;
		}
	}
	weapon_fire_pressed_ = false; // edges consume on the first tick of the frame
	weapon_reload_pressed_ = false;
	PendingWeaponEvent pending;
	pending.tick = world_->logic_tick;
	bool has_presentation_event = false;
	if (ev.play_anim) {
		++weapon_play_serial_;
		weapon_anim_key_ = String::utf8(ev.anim_key);
		weapon_anim_tick_ = world_->logic_tick;
		// The play consumes the slot ring and latches the served variant — the shell
		// plays exactly this variant on every viewmodel part
		// [orig: AnimMap_PlayAnimBySlot @ 0x40bda0 advances the head and latches
		//  the served entry at animState+68].
		weapon_anim_variant_ = weapon_ring_take_variant(weapon_anim_key_);
		pending.anim_key = weapon_anim_key_;
		pending.anim_variant = weapon_anim_variant_;
		has_presentation_event = true;
	}
	if (ev.action_started >= 0) {
		// Copy the begin leg while this def is mounted; a later weapon switch cannot
		// change the queued sound/effect payload.
		// [orig: ActionSlot_ExecuteActionWithEffect @ 0x541860].
		++weapon_action_serial_;
		weapon_action_started_ = ev.action_started;
		pending.action_started = ev.action_started;
		if (ev.action_started < opennova::world::weapon_action::kCount) {
			const opennova::world::WeaponFsmAction &act = weapon_def_.actions[ev.action_started];
			pending.action_soundset = String::utf8(act.soundset);
			pending.action_particle = String::utf8(act.particle);
			pending.action_particle_userpoint = String::utf8(act.particle_userpoint);
		}
		has_presentation_event = true;
	}
	if (ev.action_finished >= 0) {
		// The END leg: the finished action's soundsetend — fire rows carry the gunshot
		// here, reload rows the completion sound
		// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100].
		++weapon_action_end_serial_;
		weapon_action_finished_ = ev.action_finished;
		pending.action_finished = ev.action_finished;
		if (ev.action_finished < opennova::world::weapon_action::kCount) {
			pending.action_end_soundset =
					String::utf8(weapon_def_.actions[ev.action_finished].soundsetend);
		}
		has_presentation_event = true;
	}
	if (ev.action_effect >= 0 && ev.action_effect < opennova::world::weapon_action::kCount) {
		// The recoil-row DIRECT effect leg — casing eject / bolt smoke at the arbiter
		// tick. Copied like the begin leg so a weapon switch cannot swap the payload.
		// [orig: WeaponAction_Recoil @ 0x542dd0 spawn @ 0x542f64]
		const opennova::world::WeaponFsmAction &act = weapon_def_.actions[ev.action_effect];
		pending.action_effect = ev.action_effect;
		pending.effect_particle = String::utf8(act.particle);
		pending.effect_particle_userpoint = String::utf8(act.particle_userpoint);
		has_presentation_event = true;
	}
	// Preserve the retail call order within one pump: clip start, begin leg, then
	// finish leg. Records themselves stay in logic-tick order until the shell drains.
	if (has_presentation_event) {
		pending.world_position = get_local_player_position();
		pending.scope_settled = player_view_.scope_engaged &&
				!opennova::world::player_view_scope_ease_active(player_view_);
		pending.third_person = player_view_.third_person;
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		pending.vehicle_attack_context =
				local != nullptr && mount_blocks_weapon_channel(*local);
		pending_weapon_events_.push_back(std::move(pending));
	}
	if (ev.fired) {
		++weapon_fired_serial_;
		// The 3P body attack stamp — knife/grenade kinds only; rifle fire stamps NO body
		// state (the FP clip plays on the weapon adm channel, and the fire path's only
		// other anim side effect drives the .3di control registers)
		// [orig: WeaponAction_Fire @ 0x542bbc..0x542bea; ActionSlot_TryAllocCtrlRegAnim
		//  @ 0x401f00 -> dword_83FCE8].
		AiEntity *p = world_->ai ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
		if (p && p->inf.active)
			opennova::world::infantry_weapon_attack_stamp(p->inf, weapon_attack_kind_);
		// Local/SP fire already passed the same FSM/ammo authority that the remote
		// C2S 0x06 handler validates. Append the host's round-ring record and spawn
		// the authoritative projectile here; the loopback server handler correctly
		// ignores this player because retail's local action has already done both.
		// [orig: WeaponAction_Fire @ 0x542c5e ->
		// Entity_FireWeaponAndSendPacket @ 0x42bd80 local re-entry ->
		// RoundData_AddRound @ 0x4fdb40 inline RoundData_SpawnRound @ 0x4ec0d0]
		opennova::world::Entity *shooter =
				world_->registry.get(world_->cached.local_player);
		if (shooter != nullptr && p != nullptr) {
			const uint8_t adm_index = shooter->equipped_adm_index;
			const opennova::world::WeaponTableEntry *adm =
					world_->weapons.by_index(adm_index);
			if (adm != nullptr && adm->ammo_index >= 0) {
				opennova::world::Vec3 origin = shooter->position;
				if (local_eye_valid_) {
					origin.x = local_eye_mission_[0];
					origin.y = local_eye_mission_[1];
					origin.z = local_eye_mission_[2];
				} else {
					origin.z += 1.0f;
				}
				// The round bearing frame IS the engine heading frame: RoundSim's
				// (cos, sin) mission-axis mapping is wire-validated on the 0x06 yaw
				// BAM (round_sim.cpp spawn, D-NET-153), and the retail spawner runs
				// raw descriptor angles through sin/cos [orig: RoundData_SpawnRound
				// trig @ 0x4ec511..0x4ec5fb]. Applying the (90 - heading)
				// mission-yaw flip here mirrored every local shot across the NE
				// diagonal (impacts landed 90 deg off the aim ray - the
				// fp_impact_probe pin; the same mistake D-NET-153 records for the
				// wire leg).
				const int32_t dir_yaw = p->heading;
				// Fire position/direction sees the undoubled recoil accumulator;
				// the camera is the separate 2*R consumer. Spread below still
				// samples R>>8 before this shot adds its own impulse.
				// [orig: Entity_CalcWeaponFirePosition @0x4DC847]
				const int32_t dir_pitch = opennova::io::bam_add(
						p->pitch, p->inf.recoil_pitch);
				local_round_sequence_ =
						static_cast<uint16_t>(local_round_sequence_ + 1u);
				const uint16_t shot_seq = local_round_sequence_;

				opennova::world::RoundEvent round_event;
				round_event.shooter_handle = joiner_
						? joiner_self_wire_handle_
						: world_->cached.local_player.packed;
				round_event.origin_x = static_cast<int32_t>(
						std::lround(double(origin.x) * kFixed16));
				round_event.origin_y = static_cast<int32_t>(
						std::lround(double(origin.y) * kFixed16));
				round_event.origin_z = static_cast<int32_t>(
						std::lround(double(origin.z) * kFixed16));
				round_event.dir_yaw = dir_yaw;
				round_event.dir_pitch = dir_pitch;
				round_event.shot_seq = shot_seq;
				const uint32_t clip_before_consume = static_cast<uint32_t>(
						std::max(0, ev.fired_clip_before_consume));
				round_event.mode_flags = static_cast<uint8_t>(
						((clip_before_consume & 0x3u) << 4u) | 0x02u);
				const bool vehicle_attack_context =
						mount_blocks_weapon_channel(*shooter);
				const bool scope_settled = player_view_.scope_engaged &&
						!opennova::world::player_view_scope_ease_active(player_view_);
				// The ordinary on-foot hip-fire leg is exact: retail passes
				// Weapon_GetScopeZoomLevel(false, 12), which returns 12, and the
				// server's bit-6-clearing composite preserves it. The predicate
				// reads the PROMOTED scope bit, so ADS raise and third-person use
				// the same 12. Settled-FP/mounted zoom levels remain D-WPN-8.
				if (player_view_.third_person ||
						(!scope_settled && !vehicle_attack_context &&
								(weapon_def_.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0)) {
					round_event.subtype = 12;
				}
				round_event.adm_index = adm_index;
				// The PowerThrow charge rides the ring/wire slot_byte (ring+32,
				// wire flags|0x80 leg) and scales the spawned round's launch
				// speed [orig: WeaponAction_Fire arg 6 <- MountSlot+0x5C ->
				// descriptor +20 @ 0x4ec5bb; deserializer restore @ 0x42f769].
				round_event.slot_byte = pending_throw_charge_;
				if (!joiner_) world_->rounds.add(round_event);

				opennova::world::RoundSpawnParams round;
				round.owner = world_->cached.local_player;
				round.shooter_handle = round_event.shooter_handle;
				round.origin = origin;
				round.dir_yaw_bam = dir_yaw;
				round.dir_pitch_bam = dir_pitch;
				round.ammo_index = adm->ammo_index;
				round.adm_index = adm_index;
				round.shot_seq = shot_seq;
				round.subtype = round_event.subtype;
				round.charge = pending_throw_charge_;
				if (joiner_ && runtime_ != nullptr)
					// The joiner's OWN predicted round runs the wire-proxy walk with
					// the local mount exclusion dead, so resolve the carrier gate from
					// the self wire row like any decoded remote round — otherwise a
					// mounted joiner's fire stops on its own vehicle's proxy.
					round.shooter_carrier_handle = wire_carrier_exclusion_for(
							runtime_->state(), joiner_self_wire_handle_,
							item_seat_specs_);
				world_->round_sim.spawn(
						*world_, round,
						joiner_
								? opennova::world::RoundConsequenceMode::VisualOnly
								: opennova::world::RoundConsequenceMode::Authoritative);

				if (joiner_ && runtime_) {
					// The client-side half of Entity_FireWeaponAndSendPacket predicts
					// above, then queues the fixed C2S 0x06 descriptor. The pose helper
					// writes full XYZ, rounded Yaw/Pitch high words, and retail's five
					// low-word deltas against the live shooter pose. The runtime stamps
					// its own currentTick when accepting it.
					// [orig: @0x42A62F/@0x42A6A1..0x42A890]
					opennova::ClientFiredRound fire;
					fire.shooter_handle = joiner_self_wire_handle_;
					fire.fire_flags = round_event.mode_flags;
					fire.adm_index = adm_index;
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
							runtime_->local_player_slot(), shot_seq);
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
					fire.extra_byte1 = (adm != nullptr && adm->ammo_index >= 0)
							? static_cast<uint8_t>(adm->ammo_index)
							: uint8_t(0);
					fire.extra_byte2 = round_event.subtype;
					fire.misc_byte = pending_throw_charge_;
					const std::array<int32_t, 5> fire_pose = {
							round_event.origin_x,
							round_event.origin_y,
							round_event.origin_z,
							dir_yaw,
							dir_pitch,
					};
					const std::array<int32_t, 5> shooter_pose = {
							p->pos[0],
							p->pos[1],
							p->pos[2],
							p->heading,
							p->pitch,
					};
					opennova::set_client_fired_round_pose(
							fire, fire_pose, shooter_pose);
					runtime_->queue_fired_round(fire);
				}
				pending_throw_charge_ = 0;
			}
		}
	}
	if (ev.dry_fired) ++weapon_dry_serial_;
	if (ev.reload_requested) {
		++weapon_reload_serial_;
		opennova::WeaponReload reload;
		bool have_wire_reload = false;
		if (!borrowed_usegun_slot && local_inventory_valid_ &&
				local_inventory_.equipped_combo >= 0) {
			reload.entity_handle = joiner_
					? joiner_self_wire_handle_
					: world_->cached.local_player.packed;
			reload.reload_param = static_cast<uint16_t>(
					local_inventory_.equipped_combo);
			have_wire_reload = true;
		} else if (borrowed_usegun_slot && local_usegun_mount_.valid()) {
			// parentSlot==3 addresses the ENTITY THAT OWNS the selected slot,
			// not the actor. Wire-header materialization preserves the host's
			// packed handles, so this path is identical for host and joiner.
			// [orig: WeaponAction_Reload @0x543108..0x543157]
			opennova::world::Entity *mount =
					world_->registry.get(local_usegun_mount_);
			opennova::world::WeaponSlotState *mounted_slot = mount != nullptr
					? opennova::world::resolve_mounted_ammo_slot(
							*world_, *mount)
					: nullptr;
			opennova::world::Entity *slot_owner = mount;
			uint8_t mounted_adm = mount != nullptr
					? mount->primary_weapon_slot_adm : 0xFF;
			if (mount != nullptr && mounted_slot != nullptr &&
					mounted_slot != &mount->primary_weapon_slot) {
				slot_owner = world_->registry.get(mount->ground_target);
				if (slot_owner == nullptr || mounted_slot !=
						&slot_owner->primary_weapon_slot) {
					mounted_slot = nullptr;
				} else {
					mounted_adm = slot_owner->primary_weapon_slot_adm;
				}
			}
			const opennova::world::WeaponTableEntry *mounted_def =
					world_->weapons.by_index(mounted_adm);
			if (mounted_slot != nullptr && slot_owner != nullptr &&
					mounted_def != nullptr) {
				reload.entity_handle = slot_owner->handle.packed;
				reload.reload_param = static_cast<uint16_t>(
						static_cast<uint16_t>(mounted_def->category) * 65u +
						static_cast<uint16_t>(mounted_def->rank));
				have_wire_reload = true;
			}
		}
		if (have_wire_reload && runtime_) {
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
	if (ev.reload_applied) {
		++weapon_reload_applied_serial_;
		// The refill stamps the 3P body reload-anim window on the entity — 80 ticks; the
		// infantry weapon channel then wants state 65 reload until it expires (and the
		// locked clip plays to its end). In the original the stamp lives inside the
		// refill itself; the SP/listen-host loopback applies it at reload start.
		// [orig: WeaponSlot_ReloadAmmo @ 0x54173c; world-wac-ai-re.md §14.8.5]
		AiEntity *p = world_->ai ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
		if (p && p->inf.active) p->inf.reload_anim_ticks = 80;
	}
	// --- the slot-pool bridge: the pool model is authoritative for ammo -------------
	// [orig: the FSM's clip lives on the MountSlot (+0x10) and the reserve is the
	//  per-ammo-class pool — one storage, two views; this port mirrors between the
	//  single-slot FSM state and the inventory]
	if (local_usegun_switch_ != LocalUseGunSwitch::kNone &&
			ev.switch_completed &&
			active_slot.current == local_usegun_switch_action_)
		commit_local_usegun_weapon_switch();
	if (local_inventory_valid_ && world_ != nullptr &&
			!borrowed_usegun_slot) {
		opennova::world::WeaponInventorySlot *eq =
				local_inventory_.slot(local_inventory_.equipped_combo);
		const opennova::world::WeaponTableEntry *eq_def =
				(eq != nullptr && eq->adm_index >= 0)
						? world_->weapons.by_index(static_cast<uint8_t>(eq->adm_index))
						: nullptr;
		if (eq != nullptr && eq_def != nullptr) {
			if (ev.reload_applied) {
				// The witnessed refill: refund the remaining clip to the pool, draw a
				// full clip clamped by it [orig: WeaponSlot_ReloadAmmo @ 0x541720,
				// §5.58] — overriding the FSM's single-class transfer (D-WPN-2).
				// eq->clip still holds the pre-reload remaining rounds (mirrored on
				// the previous tick); the refund below consumes it.
				opennova::world::weapon_inventory_reload_slot(world_->weapons,
				                                              local_inventory_,
				                                              local_inventory_.equipped_combo);
				active_slot.clip = eq->clip;
			} else {
				eq->clip = active_slot.clip; // fire consume mirrors down
			}
			active_slot.reserve =
					opennova::world::weapon_pool_get(local_inventory_, eq_def->ammo_class_id);
			// The post-recoil auto-switch [orig: WeaponAction_Recoil tail @ 0x543062:
			// def+0x168 -> Player_SwitchToWeaponByHandle(def[+0x164]*65) — the
			// grenade/LAW switchback, unconditional per throw].
			if (ev.action_finished == opennova::world::weapon_action::kRecoil &&
			    eq_def->has_switchcategory) {
				handle_weapon_switch_outcome(opennova::world::weapon_switch_to_handle(
						world_->weapons, local_inventory_,
						eq_def->switchcategory *
								opennova::world::weapon_combo::kRanksPerCategory,
						local_weapon_switch_gates()));
			}
		}
		// A queued manual switch commits at the outgoing SWITCHFROM/SWITCHRANK
		// swap seam [orig: the completion consumes g_pendingWeaponSlot].
		if (weapon_switch_in_flight_ && ev.switch_completed) {
			commit_pending_weapon_switch();
		}
	}
	// The FSM's scope side effects land on the sim-owned engaged bit: forced
	// unscope (one-shot / reload stash) and the pump's rescope-after-reload
	// [orig: g_weaponScopeActive writes; the rescope block @ 0x54139e].
	if (ev.unscope) {
		++weapon_unscope_serial_;
		// The forced paths run the same refusing toggle — a mid-ease unscope keeps
		// the scope (rare: a reload requested inside the raise ease)
		// [orig: @ 0x543136 calls Player_ToggleWeaponScope, activeFlag-gated].
		opennova::world::player_view_set_engaged(player_view_, false,
				(weapon_def_.flags2 & DEF_WEAPON_FLAG2_INSET) != 0);
	}
	if (ev.rescope) {
		++weapon_rescope_serial_;
		opennova::world::player_view_set_engaged(player_view_, true,
				(weapon_def_.flags2 & DEF_WEAPON_FLAG2_INSET) != 0);
	}
}

Dictionary NovaSimulation::get_local_player_weapon_state() const {
	Dictionary out;
	out["active"] = weapon_active_;
	if (!weapon_active_) return out;
	const opennova::world::WeaponSlotState &active_slot =
			*active_local_weapon_slot();
	out["current"] = active_slot.current;
	out["next"] = active_slot.next;
	out["phase"] = static_cast<int>(active_slot.phase);
	out["switch_deferred"] = weapon_switch_deferred_action_;
	out["switch_in_flight"] = weapon_switch_in_flight_;
	out["pending_combo"] = local_inventory_.pending_combo;
	out["anim_key"] = weapon_anim_key_;
	out["anim_variant"] = weapon_anim_variant_;
	const uint32_t anim_age_ticks = world_ && !weapon_anim_key_.is_empty()
			? world_->logic_tick - weapon_anim_tick_ : 0;
	out["anim_age_ticks"] = static_cast<int64_t>(anim_age_ticks);
	out["play_serial"] = static_cast<int64_t>(weapon_play_serial_);
	// The last-started action's audio/effect legs remain useful snapshot diagnostics;
	// ordered delivery uses drain_local_player_weapon_events().
	// [orig: ActionSlot_ExecuteActionWithEffect
	// @ 0x541860 -> ActionSlot_SpawnEffect @ 0x401f20].
	out["action_serial"] = static_cast<int64_t>(weapon_action_serial_);
	if (weapon_action_started_ >= 0 &&
			weapon_action_started_ < opennova::world::weapon_action::kCount) {
		const opennova::world::WeaponFsmAction &act = weapon_def_.actions[weapon_action_started_];
		out["action_started"] = weapon_action_started_;
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
	out["action_end_serial"] = static_cast<int64_t>(weapon_action_end_serial_);
	if (weapon_action_finished_ >= 0 &&
			weapon_action_finished_ < opennova::world::weapon_action::kCount) {
		out["action_end_soundset"] =
				String::utf8(weapon_def_.actions[weapon_action_finished_].soundsetend);
	} else {
		out["action_end_soundset"] = String();
	}
	// The PowerThrow windup for the HUD charge bar [orig: HUD_DrawPowerThrowChargeBar
	// @ 0x599830 (ex kong "HUD_DrawWeaponReloadBar" misnomer — it only draws the
	// windup): gates = def+8 sign bit, g_fireChargeStartTick != 0, ammo available;
	// the drawer derives the fill from held ticks].
	const bool windup_active = (weapon_def_.flags & DEF_WEAPON_FLAG_POWERTHROW) != 0 &&
			power_throw_start_tick_ != 0 && world_ != nullptr &&
			(active_slot.clip > 0 || weapon_def_.clip_capacity < 0);
	out["windup_active"] = windup_active;
	out["windup_held_ticks"] = windup_active
			? static_cast<int64_t>(world_->logic_tick - power_throw_start_tick_)
			: static_cast<int64_t>(0);
	out["fired_serial"] = static_cast<int64_t>(weapon_fired_serial_);
	out["dry_serial"] = static_cast<int64_t>(weapon_dry_serial_);
	out["reload_serial"] = static_cast<int64_t>(weapon_reload_serial_);
	out["reload_applied_serial"] =
			static_cast<int64_t>(weapon_reload_applied_serial_);
	out["reload_received_serial"] =
			static_cast<int64_t>(weapon_reload_received_serial_);
	out["reload_received_entity"] =
			static_cast<int64_t>(weapon_reload_received_entity_);
	out["reload_received_param"] =
			static_cast<int64_t>(weapon_reload_received_param_);
	out["unscope_serial"] = static_cast<int64_t>(weapon_unscope_serial_);
	out["rescope_serial"] = static_cast<int64_t>(weapon_rescope_serial_);
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
			// Retail tests Position.Z + CameraOffset.Z here. CameraOffset.Z is
			// not represented in the current world model, so raw fixed body Z
			// plus the independently mirrored Drowning flag is our bounded projection.
			// [orig: HUD_DrawCrosshair @0x592b35; source gate @0x4ec2de]
			const bool below_water = world_->env.water_z != 0 &&
					body->pos[2] < world_->env.water_z;
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
						  weapon_def_, active_slot,
						  static_cast<int32_t>(world_->logic_tick))
				: 0;
		out["heat"] = heat > opennova::world::weapon_heat::kFull
				? opennova::world::weapon_heat::kFull
				: heat;
		out["heat_glow"] = std::clamp(heat, 0, 0x10000);
	}
	out["borrowed_usegun_slot"] = local_usegun_slot_active_;
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
					p->inf, weapon_active_, blocked_mount)) {
			out["body_anim_key"] = infantry_anim_key(p->inf.wpn_state);
			out["body_anim_phase"] = p->inf.wpn_clip_phase;
		}
	}
	return out;
}

Array NovaSimulation::drain_local_player_weapon_events() {
	Array out;
	const uint32_t now = world_ ? world_->logic_tick : 0;
	for (const PendingWeaponEvent &event : pending_weapon_events_) {
		Dictionary row;
		// Unsigned subtraction intentionally preserves age across logic-tick wrap.
		row["age_ticks"] = static_cast<int64_t>(now - event.tick);
		row["world_position"] = event.world_position;
		row["anim_key"] = event.anim_key;
		row["anim_variant"] = event.anim_variant;
		row["action_started"] = event.action_started;
		row["action_soundset"] = event.action_soundset;
		row["action_particle"] = event.action_particle;
		row["action_particle_userpoint"] = event.action_particle_userpoint;
		row["scope_settled"] = event.scope_settled;
		row["third_person"] = event.third_person;
		row["vehicle_attack_context"] = event.vehicle_attack_context;
		row["action_finished"] = event.action_finished;
		row["action_end_soundset"] = event.action_end_soundset;
		row["action_effect"] = event.action_effect;
		row["effect_particle"] = event.effect_particle;
		row["effect_particle_userpoint"] = event.effect_particle_userpoint;
		row["switch_to_weapon"] = event.switch_to_weapon;
		row["clear_weapon"] = event.clear_weapon;
		row["preserve_slot_state"] = event.preserve_slot_state;
		row["switch_denied"] = event.switch_denied;
		out.push_back(row);
	}
	pending_weapon_events_.clear();
	return out;
}
