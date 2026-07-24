// NovaSimulation — the LOCAL PLAYER cluster: view effects, armory/usegun and
// mount interactions, loadout (slot pool / spawn kit / map rules), input and
// spawn, pose getters, and the equipped-weapon FSM (net-re §5.62).
#include "simulation/nova_simulation_internal.h"

using namespace novasim;

void NovaSimulation::reset_local_player_view_effects() {
	player_view_.binoculars_requested = false;
	player_view_.binoculars_raised = false;
	player_view_.binoculars_view_active = false;
	binocular_yaw_offset_deg_ = 0.0f;
	binocular_pitch_offset_deg_ = 0.0f;
	player_view_.nvg_gain = opennova::world::kNvgGainMin;
	player_view_.nvg_active = world_ != nullptr &&
			(world_->mission_attrib_flags &
					static_cast<uint32_t>(
							opennova::bms::AttribFlags::StartWithNVGOn)) != 0;
	nvg_scope_restore_ = false;
	refresh_local_player_view_effects();
}

void NovaSimulation::refresh_local_player_view_effects() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	const bool alive = local != nullptr && local->alive && local->health > 0;
	const bool round_ended = world_ != nullptr && world_->round_end.ended;
	opennova::world::player_view_update_effective_modes(
			player_view_, alive, round_ended);
}
bool NovaSimulation::local_player_in_armory_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	// The use-item armory leg rejects a seated player before consulting the type-6
	// volume bit [orig: Input_HandleActionBinding_0 @0x4e0b3f, parentSlot == 0].
	return e != nullptr && !e->mounted &&
	       (e->flags & opennova::world::kEntityFlagArmoryZone) != 0;
}

bool NovaSimulation::local_player_in_vehicle_loadout_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr &&
	       (e->flags & opennova::world::kEntityFlagVehicleLoadoutZone) != 0;
}

opennova::world::WeaponSlotState *NovaSimulation::active_local_weapon_slot() {
	if (local_usegun_slot_active_ && world_ && local_usegun_mount_.valid()) {
		opennova::world::Entity *mount =
				world_->registry.get(local_usegun_mount_);
		if (mount != nullptr &&
				mount->primary_weapon_slot_adm == local_usegun_weapon_adm_)
			return &mount->primary_weapon_slot;
	}
	return &weapon_slot_;
}

const opennova::world::WeaponSlotState *
NovaSimulation::active_local_weapon_slot() const {
	if (local_usegun_slot_active_ && world_ && local_usegun_mount_.valid()) {
		const opennova::world::Entity *mount =
				world_->registry.get(local_usegun_mount_);
		if (mount != nullptr &&
				mount->primary_weapon_slot_adm == local_usegun_weapon_adm_)
			return &mount->primary_weapon_slot;
	}
	return &weapon_slot_;
}

bool NovaSimulation::local_usegun_switch_is_instant() const {
	if (!world_ ||
			local_usegun_switch_action_ !=
				opennova::world::weapon_action::kSwitchFrom)
		return false;
	const uint8_t from_adm = local_usegun_slot_active_
			? local_usegun_weapon_adm_
			: local_usegun_saved_adm_;
	const uint8_t to_adm =
			(local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
			 local_usegun_switch_ == LocalUseGunSwitch::kSwap)
			? local_usegun_pending_weapon_adm_
			: local_usegun_saved_adm_;
	const opennova::world::WeaponTableEntry *from =
			world_->weapons.by_index(from_adm);
	const opennova::world::WeaponTableEntry *to =
			world_->weapons.by_index(to_adm);
	const int32_t flags = (from != nullptr ? from->flags : 0) |
			(to != nullptr ? to->flags : 0);
	return (flags & opennova::world::weapon_flag::kEmplaced) != 0;
}

void NovaSimulation::queue_local_usegun_weapon_switch(bool p_same_category) {
	local_usegun_switch_action_ = p_same_category
			? opennova::world::weapon_action::kSwitchRank
			: opennova::world::weapon_action::kSwitchFrom;
	weapon_switch_deferred_action_ = local_usegun_switch_action_;
	if (!weapon_active_ ||
			(local_usegun_slot_active_ &&
			 active_local_weapon_slot() == &weapon_slot_)) {
		commit_local_usegun_weapon_switch();
		return;
	}
	opennova::world::WeaponSlotState *slot = active_local_weapon_slot();
	if (local_usegun_switch_action_ ==
			opennova::world::weapon_action::kSwitchRank)
		opennova::world::weapon_fsm_queue_switch_rank(*slot);
	else
		opennova::world::weapon_fsm_queue_switch_from(*slot);
}

void NovaSimulation::commit_local_usegun_weapon_switch() {
	if (!world_ || local_usegun_switch_ == LocalUseGunSwitch::kNone) return;
	opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player == nullptr) {
		local_usegun_switch_ = LocalUseGunSwitch::kNone;
		local_usegun_slot_active_ = false;
		local_usegun_mount_ = opennova::world::EntityHandle{};
		local_usegun_weapon_adm_ = 0xFF;
		local_usegun_pending_mount_ = opennova::world::EntityHandle{};
		local_usegun_pending_weapon_adm_ = 0xFF;
		local_usegun_saved_adm_ = 0xFF;
		local_usegun_switch_action_ = -1;
		weapon_switch_deferred_action_ = -1;
		weapon_active_ = false;
		local_first_person_model_adm_ = 0xFF;
		return;
	}

	uint8_t next_adm = 0xFF;
	opennova::world::WeaponSlotState *next_slot = nullptr;
	bool select_parent =
			local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
			local_usegun_switch_ == LocalUseGunSwitch::kSwap;
	if (select_parent) {
		opennova::world::Entity *mount =
				world_->registry.get(local_usegun_pending_mount_);
		if (mount == nullptr ||
				mount->primary_weapon_slot_adm !=
						local_usegun_pending_weapon_adm_) {
			select_parent = false;
		} else {
			local_usegun_slot_active_ = true;
			local_usegun_mount_ = local_usegun_pending_mount_;
			local_usegun_weapon_adm_ =
					local_usegun_pending_weapon_adm_;
			next_adm = local_usegun_weapon_adm_;
			next_slot = &mount->primary_weapon_slot;
		}
	}
	if (!select_parent) {
		local_usegun_slot_active_ = false;
		next_adm = local_usegun_saved_adm_;
		next_slot = &weapon_slot_;
	}

	player->equipped_adm_index = next_adm;
	const opennova::world::WeaponTableEntry *next_def =
			world_->weapons.by_index(next_adm);
	// SWITCHFROM draws the committed target through TryQueueSwitchTo. SWITCHRANK
	// swaps directly and must not disturb a persistent target slot's current
	// action/phase/next fields. [orig: commits @0x543475 / @0x543539]
	if (next_slot != nullptr && next_def != nullptr &&
			local_usegun_switch_action_ ==
				opennova::world::weapon_action::kSwitchFrom)
		opennova::world::weapon_fsm_try_queue_switch_to(*next_slot);
	PendingWeaponEvent event;
	event.tick = world_->logic_tick;
	event.world_position = get_local_player_position();
	event.switch_to_weapon =
			next_def != nullptr ? String::utf8(next_def->name.c_str()) : String();
	event.clear_weapon = next_def == nullptr;
	event.preserve_slot_state = true;
	pending_weapon_events_.push_back(std::move(event));
	if (next_def == nullptr) weapon_active_ = false;

	local_usegun_switch_ = LocalUseGunSwitch::kNone;
	local_usegun_pending_mount_ = opennova::world::EntityHandle{};
	local_usegun_pending_weapon_adm_ = 0xFF;
	local_usegun_switch_action_ = -1;
	weapon_switch_deferred_action_ = -1;
	if (!local_usegun_slot_active_) {
		local_usegun_mount_ = opennova::world::EntityHandle{};
		local_usegun_weapon_adm_ = 0xFF;
		local_usegun_saved_adm_ = 0xFF;
	}
}

void NovaSimulation::sync_local_usegun_weapon_transition() {
	if (!world_ || !world_->cached.local_player.valid()) return;
	opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player == nullptr) return;
	opennova::world::Entity *mounted_parent =
			player->mounted &&
					player->mount_type == opennova::world::SeatType::Gunner
			? world_->registry.get(player->mount_target)
			: nullptr;
	const bool on_usegun =
			mounted_parent != nullptr && player->use_gun_slot_swapped &&
			mounted_parent->primary_weapon_slot_adm != 0xFF;
	const auto same_category = [&](uint8_t p_from, uint8_t p_to) {
		const opennova::world::WeaponTableEntry *from =
				world_->weapons.by_index(p_from);
		const opennova::world::WeaponTableEntry *to =
				world_->weapons.by_index(p_to);
		return from != nullptr && to != nullptr &&
				from->category == to->category;
	};
	const auto stage_parent = [&](opennova::world::Entity &p_mount) {
		const uint8_t target_adm = p_mount.primary_weapon_slot_adm;
		if (!local_usegun_slot_active_)
			local_usegun_saved_adm_ =
					player->pre_use_gun_equipped_adm_index;
		local_usegun_pending_mount_ = p_mount.handle;
		local_usegun_pending_weapon_adm_ = target_adm;
		local_usegun_switch_ = local_usegun_slot_active_
				? LocalUseGunSwitch::kSwap
				: LocalUseGunSwitch::kAttach;
		const uint8_t from_adm = local_usegun_slot_active_
				? local_usegun_weapon_adm_
				: local_usegun_saved_adm_;
		// The shared helper applied the nonlocal immediate stamp. L retains its
		// outgoing EquippedSlot until the action handler's commit seam.
		player->equipped_adm_index = from_adm;
		queue_local_usegun_weapon_switch(
				same_category(from_adm, target_adm));
	};
	const auto stage_personal = [&]() {
		const uint8_t from_adm = local_usegun_slot_active_
				? local_usegun_weapon_adm_
				: local_usegun_saved_adm_;
		player->equipped_adm_index = from_adm;
		local_usegun_pending_mount_ =
				opennova::world::EntityHandle{};
		local_usegun_pending_weapon_adm_ = 0xFF;
		local_usegun_switch_ = LocalUseGunSwitch::kDetach;
		queue_local_usegun_weapon_switch(
				same_category(from_adm, local_usegun_saved_adm_));
	};

	if (on_usegun) {
		const bool active_matches = local_usegun_slot_active_ &&
				local_usegun_mount_ == mounted_parent->handle &&
				local_usegun_weapon_adm_ ==
						mounted_parent->primary_weapon_slot_adm;
		const bool pending_matches =
				(local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
				 local_usegun_switch_ == LocalUseGunSwitch::kSwap) &&
				local_usegun_pending_mount_ == mounted_parent->handle &&
				local_usegun_pending_weapon_adm_ ==
						mounted_parent->primary_weapon_slot_adm;
		if (local_usegun_switch_ == LocalUseGunSwitch::kNone) {
			if (!active_matches) stage_parent(*mounted_parent);
		} else if (!pending_matches) {
			// A later attach overwrites g_pendingWeaponSlot without changing the
			// outgoing slot. This includes direct old-gun -> new-gun swaps.
			stage_parent(*mounted_parent);
		}
		return;
	}

	if ((local_usegun_slot_active_ ||
			 local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
			 local_usegun_switch_ == LocalUseGunSwitch::kSwap) &&
			local_usegun_switch_ != LocalUseGunSwitch::kDetach)
		stage_personal();
}

bool NovaSimulation::local_player_toggle_mount() {
	// The USE-ITEM mount toggle for the local player — the shell calls this when the
	// armory/vehicle-zone legs of the key don't apply. [orig: Input_ProcessFrame release
	// edge @0x49d6dc -> Entity_ToggleVehicleMount @0x436950]
	if (!world_) return false;
	// Authority-only: the witnessed non-authority path queues C2S 0x26 (attach) /
	// sends 0x27 (detach) and waits for the 0x0A stream to confirm [orig:
	// Entity_RequestVehicleAttach @0x4364a0 / Entity_SendDetachPacket @0x435510].
	// That joiner wire leg is unported (D-AI-11 l) — applying locally on a joiner
	// would silently desync against the host, so the toggle refuses (the armory
	// leg's MP stance).
	if (joiner_) return false;
	// The ordinary local/offline toggle requires a live EquippedSlot/Def. Script
	// and network authority paths may still attach through the world core.
	// [orig: Entity_AttachToUseGunSlot null-slot reject @0x546c07]
	const opennova::world::Entity *toggle_player =
			world_->registry.get(world_->cached.local_player);
	if (!weapon_active_ &&
			(toggle_player == nullptr || !toggle_player->mounted))
		return false;
	sync_local_usegun_weapon_transition();
	// The weapon-busy gate [orig: @0x436958-0x436977 — no EquippedSlot passes;
	// currentAction < 2 (idle/emptyidle) or == 5 (the dry click) passes, as does a
	// pending OVERHEATED (nextAction == 11); an in-flight fire/reload/switch swallows
	// the toggle].
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	const int32_t cur = active_slot->current;
	const int32_t next = active_slot->next;
	if (!(cur < 2 || cur == opennova::world::weapon_action::kEmpty ||
	      next == opennova::world::weapon_action::kOverheated))
		return false;
	const bool changed = opennova::world::player_toggle_vehicle_mount(
			*world_, world_->cached.local_player);
	if (changed) {
		// A successful ToSpecial/use-item transition clears the raw binocular
		// request, not merely the effective first-person view.
		player_view_.binoculars_requested = false;
		binocular_yaw_offset_deg_ = 0.0f;
		binocular_pitch_offset_deg_ = 0.0f;
		refresh_local_player_view_effects();
		sync_local_mounted_input_heading();
		sync_local_usegun_weapon_transition();
	}
	return changed;
}

TypedArray<Dictionary> NovaSimulation::get_attach_labels() const {
	TypedArray<Dictionary> out;
	if (!world_) return out;
	const opennova::world::Entity *player = world_->registry.get(world_->cached.local_player);
	if (player == nullptr || !player->alive || player->health <= 0) return out;
	// Armory mode = standing in the type-6 armory volume; the label pass reads the raw
	// flag [orig: is_armory_mode = entity Flags & 0x400000 @0x5a32c4].
	const bool armory_mode =
	    (player->flags & opennova::world::kEntityFlagArmoryZone) != 0;
	// The nearest-only gate [orig: Player_CanFireWeapon @0x5cf780 — EquippedSlot present
	// and parentSlot not 2/5 (ctrl/drvr); the camera-mode/underwater/scope legs live
	// host-side and are unmodeled here: docs/interface/hud-re.md (D-HUD-11)].
	const bool can_fire =
	    player->equipped_adm_index != 0xFF &&
	    !(player->mounted && opennova::world::is_vehicle_control_seat(player->mount_type));
	std::vector<opennova::world::AttachLabel> labels;
	opennova::world::collect_attach_labels(*world_, *player, armory_mode, can_fire, labels);
	for (const opennova::world::AttachLabel &l : labels) {
		Dictionary d;
		d["position"] = Vector3(l.world_pos.x, l.world_pos.y, l.world_pos.z);
		d["seat_type"] = static_cast<int>(l.type);
		d["armory"] = l.armory;
		d["nearest"] = l.nearest;
		String key;
		if (l.type == opennova::world::SeatType::Gunner) {
			// The USEGUN label text: the gun entity's primary weapon -> its weapon.def
			// attachtextid key [orig: Entity_GetWeaponSlots slot0 -> def+0x3A0 @0x5a351d].
			const opennova::world::Entity *cand = world_->registry.get(l.entity);
			if (cand != nullptr && !cand->primary_weapon.empty()) {
				const int wi = world_->weapons.index_of(cand->primary_weapon.c_str());
				if (wi >= 0)
					key = String(world_->weapons.entries[static_cast<size_t>(wi)]
					                     .attach_text_id.c_str());
			}
		}
		d["attach_text_key"] = key;
		out.push_back(d);
	}
	return out;
}

// --- the local player's loadout: slot pool, spawn kit, map rules -----------------------
// (the 2026-07-18 loadout grill; witness map in docs/net/novaworld-net-re.md §5.57)

namespace {

opennova::world::WeaponKitEntry kit_entry_from_dict(const Dictionary &d) {
	opennova::world::WeaponKitEntry e;
	e.name = dictionary_string(d, "name", std::string());
	e.ammo_primary = int(int64_t(d.get("ammo_primary", -1)));
	e.ammo_secondary = int(int64_t(d.get("ammo_secondary", -1)));
	e.flags = int(int64_t(d.get("flags", -1)));
	return e;
}

Dictionary kit_entry_to_dict(const opennova::world::WeaponKitEntry &e) {
	Dictionary d;
	d["name"] = String::utf8(e.name.c_str());
	d["ammo_primary"] = e.ammo_primary;
	d["ammo_secondary"] = e.ammo_secondary;
	d["flags"] = e.flags;
	return d;
}

} // namespace

void NovaSimulation::set_spawn_loadout(const TypedArray<Dictionary> &p_kit,
                                       bool p_filter_by_availability) {
	std::vector<opennova::world::WeaponKitEntry> kit;
	for (int i = 0; i < p_kit.size(); ++i) {
		opennova::world::WeaponKitEntry e = kit_entry_from_dict(p_kit[i]);
		if (!e.name.empty()) kit.push_back(std::move(e));
	}
	if (p_filter_by_availability && world_ != nullptr && !kit.empty()) {
		// The SP .bms promote leg: availability-filter with the knife fallback
		// [orig: Mission_LoadBMSFile @ 0x40f7ae..0x40f95c].
		kit = opennova::world::weapon_kit_filter_by_availability(kit, world_->weapons,
		                                                         weapon_availability_);
	}
	spawn_kit_set_ = !kit.empty();
	spawn_kit_ = std::move(kit);
}

void NovaSimulation::set_weapon_availability(const TypedArray<Dictionary> &p_pairs) {
	weapon_availability_.reset(); // [orig: the all-1 default @ 0x551c86]
	if (!world_ || p_pairs.is_empty()) return;
	std::vector<std::pair<std::string, int32_t>> pairs;
	for (int i = 0; i < p_pairs.size(); ++i) {
		const Dictionary d = p_pairs[i];
		std::string name = dictionary_string(d, "name", std::string());
		if (name.empty()) continue;
		pairs.emplace_back(std::move(name), int32_t(int64_t(d.get("value", 1))));
	}
	opennova::world::weapon_availability_apply_pairs(weapon_availability_, world_->weapons,
	                                                 pairs);
}

int NovaSimulation::get_weapon_availability(const String &p_weapon_name) const {
	if (!world_) return opennova::world::weapon_availability_value::kAllowed;
	const int idx = world_->weapons.index_of(p_weapon_name.utf8().get_data());
	if (idx < 0) return opennova::world::weapon_availability_value::kAllowed;
	return weapon_availability_.value_for(idx);
}

bool NovaSimulation::set_local_player_class(int p_player_class) {
	if (!world_ || p_player_class < 5 || p_player_class > 9) return false;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) {
		// A joiner's shell applies the profile class before L has spawned
		// (name-match). Latch it; the joiner spawn block stamps the entity.
		pending_local_player_class_ = p_player_class;
		return true;
	}
	e->player_class = static_cast<uint8_t>(p_player_class);
	return true;
}

bool NovaSimulation::apply_local_player_loadout(const TypedArray<Dictionary> &p_kit,
                                                int p_player_class) {
	// The armory ACCEPT apply [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline
	// leg: parse the tuples, expand sub-weapons, reset + refill the slot table, apply
	// the requested ammo, re-select the equipped slot].
	if (!world_) return false;
	// A joiner applies its kit before L exists (the shell runs the spawn-loadout
	// apply right after runtime setup; L spawns later, on the name-match). Build
	// the inventory now — it is sim-side state — and defer only the entity
	// stamps (class + equipped adm) to the joiner spawn block, mirroring the
	// host's Player_InitPlayer-time arm. Dropping the kit here left the joiner
	// unable to fire, reload, or switch (the two-GUI regression).
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (p_player_class >= 5 && p_player_class <= 9) {
		if (e != nullptr)
			e->player_class = static_cast<uint8_t>(p_player_class);
		else
			pending_local_player_class_ = p_player_class;
	}
	std::vector<opennova::world::WeaponKitEntry> kit;
	for (int i = 0; i < p_kit.size(); ++i) {
		opennova::world::WeaponKitEntry entry = kit_entry_from_dict(p_kit[i]);
		if (entry.name.empty()) continue;
		const int idx = world_->weapons.index_of(entry.name.c_str());
		if (idx < 0) continue;
		// The per-entry availability validation [orig: the server 0x2F gate
		// @ 0x515a3f — 0 drops the entry; 2 requires the armory zone, which the
		// ACCEPT flow is already gated on host-side].
		if (weapon_availability_.value_for(idx) ==
		    opennova::world::weapon_availability_value::kBanned)
			continue;
		kit.push_back(std::move(entry));
	}
	// The accepted loadout becomes the respawn kit [orig: the S2C 0x5A apply writes
	// restrictionData @ 0x4293e4; SP shares the buffer]. An explicit empty kit stays
	// empty (the armory all-NONE accept leaves the table bare).
	spawn_kit_set_ = true;
	spawn_kit_ = std::move(kit);
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// The ACCEPT leg applies the REQUESTED ammo over the default seed: pool =
	// min(req, maxclips) * clipsize when requested, else startrounds RAW on the main
	// leg; the first different-class sub-variant takes ammo_secondary with the
	// x clipsize fallback [orig: @ 0x566166 vs @ 0x566209; WeaponSlot_SetAmmoCount
	// @ 0x540b50].
	const opennova::world::WeaponTable &table = world_->weapons;
	for (const opennova::world::WeaponKitEntry &entry : spawn_kit_) {
		const int adm = table.index_of(entry.name.c_str());
		if (adm < 0) continue;
		const opennova::world::WeaponTableEntry *def =
				table.by_index(static_cast<uint8_t>(adm));
		if (def == nullptr) continue;
		if (def->clipsize != -1) {
			int32_t total = entry.ammo_primary >= 0
					? std::min<int32_t>(entry.ammo_primary, def->maxclips) * def->clipsize
					: def->startrounds;
			opennova::world::weapon_pool_set(table, local_inventory_, def->ammo_class_id,
			                                 total);
		}
		for (int k = 1; k <= def->loadout_subclasses; ++k) {
			const opennova::world::WeaponTableEntry *sub =
					(adm + k < 256) ? table.by_index(static_cast<uint8_t>(adm + k))
					                : nullptr;
			if (sub == nullptr) continue;
			if (opennova::strutil::iequals(sub->ammo_class, def->ammo_class)) continue;
			int32_t total = entry.ammo_secondary >= 0
					? std::min<int32_t>(entry.ammo_secondary, sub->maxclips)
					: static_cast<int32_t>(sub->startrounds);
			// A nonnegative sub-weapon count is expressed in clips and expands to
			// rounds; a negative fallback is the no-clip sentinel and stays raw.
			// This is what keeps the implicit satchel detonator switch-eligible.
			// [orig: WeaponLoadout_ApplyFromBuffer @ 0x5661E8..0x566215]
			if (total >= 0) total *= sub->clipsize;
			opennova::world::weapon_pool_set(table, local_inventory_, sub->ammo_class_id,
			                                 total);
			break; // the FIRST different-class sub-variant [orig: @ 0x5027c8 shape]
		}
	}
	opennova::world::weapon_inventory_recalc_clips(table, local_inventory_);
	return true;
}

void NovaSimulation::respawn_local_player_loadout() {
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// Player_InitPlayer clears both view effects and seeds NVG from the mission
	// StartWithNVGOn bit on every respawn.
	reset_local_player_view_effects();
}

void NovaSimulation::sync_local_player_damage_classes() {
	if (!world_) return;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return;
	e->ammo_damage_class.assign(world_->ammo.entries.size(), 0);
	const std::vector<opennova::world::WeaponKitEntry> kit =
			spawn_kit_set_ ? spawn_kit_ : opennova::world::weapon_kit_default();
	opennova::world::weapon_kit_build_damage_classes(
			kit, world_->weapons, world_->ammo.entries.size(), e->ammo_damage_class);
}

void NovaSimulation::rebuild_local_player_loadout(bool p_select_spawn_default) {
	// The Player_InitPlayer weapon leg [orig: @ 0x4e15f0: AvatarDef_BuildDisplayList
	// (restrictionData) -> WeaponSlotPool_ResetAllEntries -> WeaponSlotTable_
	// LoadAllFromDefs -> WeaponSlots_SeedAmmoPoolsFromDefs -> WeaponSlots_
	// RecalculateAmmoFromCapacity -> Player_SelectWeaponSlot(195) ->
	// Player_SwitchToWeaponByHandle(195)].
	if (!world_) return;
	// Entity-optional: a joiner rebuilds its inventory before L spawns. The
	// inventory/equip selection is sim-side state; entity stamps (class, damage
	// classes, equipped adm) re-run at the joiner spawn block once L exists.
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	const opennova::world::WeaponTable &table = world_->weapons;
	sync_local_player_damage_classes();
	if (table.empty()) return;
	const std::vector<opennova::world::WeaponKitEntry> kit =
			spawn_kit_set_ ? spawn_kit_ : opennova::world::weapon_kit_default();
	local_inventory_.reset(table);
	const std::vector<std::string> display =
			opennova::world::weapon_kit_expand_display_list(kit, table);
	const opennova::world::WeaponFillResult fill =
			opennova::world::weapon_inventory_load_from_display(table, display,
			                                                    local_inventory_);
	for (const std::string &w : fill.warnings)
		print_verbose(String::utf8(w.c_str())); // [orig: ErrorLog_WriteTimestamped]
	// Pre-spawn the class comes from the latched shell request; 8 is retail's
	// out-of-range clamp default [orig: Server_PlayerAdd class clamp @ 0x51d102].
	const uint8_t seed_class = e != nullptr
			? e->player_class
			: static_cast<uint8_t>(
					pending_local_player_class_ >= 5 && pending_local_player_class_ <= 9
							? pending_local_player_class_
							: 8);
	opennova::world::weapon_inventory_seed_pools(table, local_inventory_, seed_class);
	opennova::world::weapon_inventory_recalc_clips(table, local_inventory_);
	local_inventory_valid_ = true;
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	if (!p_select_spawn_default) return;
	const opennova::world::WeaponSwitchGates gates = local_weapon_switch_gates();
	if (!opennova::world::weapon_select_slot(
	            table, local_inventory_,
	            opennova::world::weapon_combo::kDefaultSpawnCombo,
	            !gates.equip_blocked)) {
		// An empty table (the armory all-NONE kit) equips nothing.
		if (e != nullptr) e->equipped_adm_index = 0xFF;
		return;
	}
	// Player_InitPlayer follows the select with SwitchToWeaponByHandle(195)
	// [orig: @ 0x4e1995/@ 0x4e19a1], but the switch's mount walk rides the entity's
	// AI-slot binding gate [orig: entity+0x68 test @ 0x4e023a; writer
	// Entity_AllocateAISlot @ 0x40d2f4] — modeled here as not-yet-bound during the
	// spawn rebuild (the motor/presentation bind after load), so the spawn equips
	// exactly the selected slot and plays no switch actions. The gate's init-time
	// value is an open question (D-WPN-21, docs/divergence-ledger.md).
	local_inventory_.pending_combo = local_inventory_.equipped_combo;
	commit_pending_weapon_switch();
	weapon_start_in_switchto_ = false;
}

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
	// @ 0x4dd727; the FP model re-resolve runs host-side off the event].
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	nvg_scope_restore_ = false;
	if (!world_ || !local_inventory_valid_) return;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return;
	const int32_t combo = local_inventory_.pending_combo;
	const opennova::world::WeaponInventorySlot *slot = local_inventory_.slot(combo);
	if (slot == nullptr || slot->adm_index < 0) return;
	local_inventory_.equipped_combo = combo;
	e->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
	const opennova::world::WeaponTableEntry *def =
			world_->weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	weapon_start_in_switchto_ = true;
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
				// dispatcher, this host supplies one press edge, so retain it beside
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

Dictionary NovaSimulation::get_local_player_inventory() const {
	Dictionary out;
	out["valid"] = local_inventory_valid_;
	out["equipped_combo"] = local_inventory_.equipped_combo;
	out["carry_flags"] = int64_t(local_inventory_.carry_flags);
	String equipped_name;
	Array slots;
	Dictionary pools;
	if (world_ != nullptr) {
		const opennova::world::WeaponTable &table = world_->weapons;
		for (int32_t combo = 0; combo < opennova::world::weapon_combo::kSlotCount;
		     ++combo) {
			const opennova::world::WeaponInventorySlot *s = local_inventory_.slot(combo);
			if (s == nullptr || s->adm_index < 0) continue;
			const opennova::world::WeaponTableEntry *def =
					table.by_index(static_cast<uint8_t>(s->adm_index));
			if (def == nullptr) continue;
			Dictionary row;
			row["combo"] = combo;
			row["name"] = String::utf8(def->name.c_str());
			row["clip"] = s->clip;
			slots.push_back(row);
			if (combo == local_inventory_.equipped_combo)
				equipped_name = String::utf8(def->name.c_str());
		}
		for (size_t i = 0; i < table.ammo_class_names.size() &&
		                   i < local_inventory_.pools.size();
		     ++i) {
			if (table.ammo_class_names[i].empty()) continue;
			pools[String::utf8(table.ammo_class_names[i].c_str())] =
					local_inventory_.pools[i];
		}
	}
	out["equipped_name"] = equipped_name;
	out["slots"] = slots;
	out["pools"] = pools;
	return out;
}

TypedArray<Dictionary> NovaSimulation::get_local_player_loadout() const {
	TypedArray<Dictionary> out;
	const std::vector<opennova::world::WeaponKitEntry> kit =
			spawn_kit_set_ ? spawn_kit_ : opennova::world::weapon_kit_default();
	for (const opennova::world::WeaponKitEntry &entry : kit)
		out.push_back(kit_entry_to_dict(entry));
	return out;
}

// weapon.def -> the sim world's armory table. Mirrors the retail load site (Game_StartMission
// parses literally "weapon.def" through WeaponDefs_LoadFile right after AnimDef_InitAll wipes
// the AdmDef table [orig: @0x5254b3/@0x5254bd]); build_weapon_table ports the witnessed
// allocation rule (null@0 + by-name-reuse-else-lowest-free = file order; §5.57, D-NET-141).
Error NovaSimulation::load_weapon_table(const Ref<NovaResourceRoot> &p_resource_root,
                                        const String &p_name) {
	if (!world_) return ERR_UNCONFIGURED;
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;

	DefWeaponsFile file = {};
	if (def_parse_weapons_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0)
		return ERR_CANT_OPEN;
	world_->weapons = opennova::np::build_weapon_table(file);
	def_free_weapons(&file);

	// The host's own player spawns in finish_load, BEFORE this feed — re-stamp its equipped
	// default now that WPN_M4AUTO resolves by name [orig: PlayerClass_InitEntity @0x4B1116].
	// Joiners spawn after the feed and get the default in Server_BuildPlayerInfoAndAdd.
	// (D-NET-143)
	const int m4 = world_->weapons.index_of("WPN_M4AUTO");
	if (m4 >= 0) {
		std::vector<opennova::world::EntityHandle> handles;
		world_->registry.for_each([&](const opennova::world::Entity &e) {
			if (e.item_id == opennova::world::kPlayerInfantryTypeId &&
			    e.equipped_adm_index == 0xFF)
				handles.push_back(e.handle);
		});
		for (const opennova::world::EntityHandle h : handles) {
			if (opennova::world::Entity *e = world_->registry.get(h))
				e->equipped_adm_index = static_cast<uint8_t>(m4);
		}
	}
	// The LOCAL player's slot pool builds from the spawn kit (the mission/armory
	// loadout when one was promoted, else the WPN_M4AUTO default kit) and selects the
	// spawn default — the Player_InitPlayer weapon leg [orig: @ 0x4e15f0; the default
	// kit literal @ 0x5246be]. This subsumes the bare adm-index stamp above for the
	// local player.
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	return OK;
}

// ammo.def -> the sim world's ballistics table + the weapon round_type resolve. Mirrors the
// retail load site (Game_StartMission parses literally "ammo.def" through AmmoDef_LoadAll
// @0x40b0b0, the sibling of the weapon.def load [orig: @0x52548a]); the resolve binds each
// adm's fired round to its AmmoTable index (the original's adm+84 pair; §5.60). Call AFTER
// load_weapon_table — an empty armory leaves every round_type unresolved and the fire
// pipeline echoes without spawning sim rounds.
Error NovaSimulation::load_ammo_table(const Ref<NovaResourceRoot> &p_resource_root,
                                      const String &p_name) {
	if (!world_) return ERR_UNCONFIGURED;
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;

	DefAmmoFile file = {};
	if (def_parse_ammo_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0)
		return ERR_CANT_OPEN;
	world_->ammo = opennova::np::build_ammo_table(file);
	def_free_ammo(&file);
	opennova::np::resolve_weapon_round_types(world_->weapons, world_->ammo);
	sync_local_player_damage_classes();
	return OK;
}

void NovaSimulation::apply_player_input_pre_tick() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return;
	refresh_local_player_view_effects();
	opennova::world::apply_player_body_input(*p, opennova::world::pack_player_body_input(player_input_));
	// The local-player weapon-channel inputs, refreshed before the body updater runs —
	// the per-tick re-read of the held AdmDefs record kind + the Flags-bit refresh
	// (Flags|0x10 from g_weaponScopeActive; the binoculars bit stays false until a host
	// binoculars input exists). [orig: @ 0x4b5d7f..0x4b5dc0]
	if (p->inf.active) {
		if (weapon_active_) {
			opennova::world::infantry_weapon_switch_stamp(
					p->inf, weapon_anim_map_serial_);
		}
		p->inf.wpn_hold_kind = weapon_active_ ? weapon_hold_kind_ : 0;
		p->inf.scope_raised = weapon_active_ && player_view_.scope_engaged;
		p->inf.binoculars_raised = player_view_.binoculars_raised;
		// The run-gait class + ForceCrouch mirror, same per-tick re-read pattern as the
		// hold kind [orig: the selection reads AdmDefs[+0x2B0]+0xAC each pass @ 0x4b72cf;
		// the ForceCrouch checks read the equipped def flags @ 0x4b7245/@ 0x4e0d8a].
		p->inf.wpn_run_anim = weapon_active_ ? weapon_run_anim_ : 0;
		p->inf.wpn_force_crouch = weapon_active_ && weapon_force_crouch_;
	}
	if (opennova::world::Entity *entity =
				world_->registry.get(world_->cached.local_player)) {
		uint32_t view_flags = 0;
		if (player_view_.nvg_active) view_flags |= 0x4u;
		if (player_view_.binoculars_raised) view_flags |= 0x8u;
		if (weapon_active_ && player_view_.scope_engaged) view_flags |= 0x10u;
		entity->flags = (entity->flags & ~0x1cu) | view_flags;
	}
}

void NovaSimulation::sync_local_mounted_input_heading() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	const AiEntity *body = world_->ai->for_handle(world_->cached.local_player);
	if (player == nullptr || !player->mounted || body == nullptr ||
			!body->inf.is_local_player)
		return;

	// Entity_RequestVehicleAttach writes one retail Yaw before the relationship.
	// The core mirrors that snap into target_heading; carry it through our extra
	// host input record so apply_player_input_pre_tick cannot undo it next frame.
	// Pitch remains untouched because the retail request snap is yaw-only.
	player_input_.look_heading = body->inf.target_heading;
}

bool NovaSimulation::spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!loaded_ || !world_ || !world_->ai) return false;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up (the faithful §5.0
	// mode-3 path), so an explicit spawn is a no-op success there. The legacy LAN host + any non-listen
	// caller (no auto-spawn) still spawn at the requested pose below.
	if (has_local_player()) return true;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x, -z, y): the inverse of the present (x,y,z) -> (x, z, -y) remap.
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return false;
	resolve_new_infantry_adm_ids();
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(p_yaw_deg);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	reset_local_player_view_effects();
	return true;
}

int NovaSimulation::spawn_local_player_at_start() {
	if (!loaded_ || !world_ || !world_->ai) return -1;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up via the SAME
	// select_player_spawn start-marker scan (Server_BuildPlayerInfoAndAdd), so when a player already
	// exists this is a no-op success (the player is at its start, input seeded by bringup_host_runtime).
	if (has_local_player()) return 1;
	// Pick the player-start marker the original would — scan the 60xx start-marker family (SP/DM,
	// coop, team), FARTHEST from the enemy set — instead of the first NPC's position. Finds the
	// authored start whatever the mission mode (e.g. a 6001-only SP training mission like 00TRa).
	// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0; net-re §5.2c]
	const opennova::world::SpawnPointResult sel = opennova::world::select_player_spawn(*world_);
	opennova::world::PlayerSpawn spawn;
	if (sel.found) {
		spawn.position = sel.position; // mission space, straight from the chosen marker
		spawn.yaw = sel.yaw;
	} else {
		// No player-start marker authored: spawn at the mission origin (the terrain clamp grounds
		// it). NEVER fall back to an NPC's position — that is the bug this replaces.
		spawn.position = {0.0f, 0.0f, 0.0f};
		spawn.yaw = 0;
	}
	// SP keeps the player's own team; the marker's team is not copied [orig: §5.2c]. Team 1 mirrors
	// the prior placeholder until the MP team path lands.
	spawn.team = 1;
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return -1;
	resolve_new_infantry_adm_ids();
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(spawn.yaw);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	reset_local_player_view_effects();
	return sel.found ? 1 : 0;
}

bool NovaSimulation::has_local_player() const {
	return world_ && world_->cached.local_player.valid();
}

int NovaSimulation::get_local_player_wire_handle() const {
	// The handle the wire stream knows the local player by. On a JOINER that is H (the
	// host-assigned wire identity), NOT the local sim handle L — L lives in the joiner's
	// own pool and collides with a host-side slot (e.g. the host player), so excluding L
	// from the wire present would wrongly hide a remote entity. On the host, the local
	// player's own pool-0 handle IS its wire handle.
	if (joiner_) return static_cast<int>(joiner_self_wire_handle_);
	return (world_ && world_->cached.local_player.valid())
			? static_cast<int>(world_->cached.local_player.packed) : 0;
}

void NovaSimulation::set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
                                      bool p_lean_left, bool p_lean_right, bool p_jump) {
	player_input_.forward = p_forward;
	player_input_.back = p_back;
	player_input_.left = p_left;
	player_input_.right = p_right;
	// Lean keys -> MoveOrder bits 6/7 [orig: g_inputFlags 0x2000/0x4000 packed
	// @ 0x4df708-0x4df741]; jump is a per-frame edge the motor consumes once grounded.
	player_input_.lean_left = p_lean_left;
	player_input_.lean_right = p_lean_right;
	player_input_.jump = p_jump;
	// Stance comes from the sim-owned SELECT latches (request_local_player_stance —
	// the C2S 0x1D apply semantics [orig: @ 0x501c60]).
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	// The movement-held latch and the unscope-on-move [orig:
	// Player_PackInputStateToEntity @ 0x4df450 — any of the four direction keys
	// sets byte_B7653B (blocks scope-UP on Scoped weapons @ 0x4df29c) and, while
	// SETTLED at scope on a Scoped (flags 1) weapon, routes through
	// Player_ToggleWeaponScope @ 0x4df4c9..0x4df4ec = the full unscope. The
	// toggle's ForceScoped pin (@ 0x4df12d) keeps pinned sights raised].
	const bool move_held = p_forward || p_back || p_left || p_right;
	if (opennova::world::player_view_move_input(player_view_, move_held,
			weapon_active_ ? weapon_def_.flags : 0) &&
			(weapon_def_.flags & 0x20000000) == 0) {
		if (opennova::world::player_view_set_engaged(player_view_, false,
				(weapon_def_.flags2 & 0x200) != 0))
			opennova::world::weapon_fsm_queue_scope_down(
					*active_local_weapon_slot());
	}
	refresh_local_player_view_effects();
}

void NovaSimulation::add_local_player_look(float p_dx_px, float p_dy_px) {
	// The scoped sensitivity reduction divides by the CURRENT zoom magnification —
	// the slot zoom seeded from the def's scope_max_mag [orig: sens /
	// Player_GetClampedWeaponElevation() @ 0x499714, applied while scoped and the
	// binocular view is down; no binoculars input exists yet]. Engaged-at-scope is
	// the sim's own bit; the zoom-adjust keys are an unported tail, so the seed
	// (scope_max_mag) IS the current zoom.
	int32_t scoped_zoom = 0;
	if (!player_view_.binoculars_view_active && weapon_active_ &&
			player_view_.scope_engaged && weapon_scope_max_mag_ > 1.0f)
		scoped_zoom = static_cast<int32_t>(weapon_scope_max_mag_);
	const bool prone = (stance_latch_ == 2); // [orig: MoveOrder & 0x100 @ 0x4e0ff7]
	// Godot supplies float relative motion; the original consumes whole center-lock
	// pixels. Accumulate the fraction so slow motion is not truncated away.
	look_px_accum_x_ += p_dx_px;
	look_px_accum_y_ += p_dy_px;
	const int32_t dx = static_cast<int32_t>(look_px_accum_x_);
	const int32_t dy = static_cast<int32_t>(look_px_accum_y_);
	look_px_accum_x_ -= static_cast<float>(dx);
	look_px_accum_y_ -= static_cast<float>(dy);
	if (dx == 0 && dy == 0) return;
	opennova::world::player_look_apply(player_input_.look_heading, player_input_.look_pitch,
	                                   look_settings_, dx, dy, scoped_zoom, prone);
}

void NovaSimulation::set_local_player_mouse(int p_sensitivity, bool p_invert_y) {
	// The mousescale clamp [orig: @ 0x49b19b-0x49b1b9: >= 0x200 -> 0x1FF, <= 0 -> 1].
	int s = p_sensitivity;
	if (s < opennova::world::kMouseSensitivityMin) s = opennova::world::kMouseSensitivityMin;
	if (s > opennova::world::kMouseSensitivityMax) s = opennova::world::kMouseSensitivityMax;
	look_settings_.sensitivity = s;
	look_settings_.invert_y = p_invert_y;
}

bool NovaSimulation::request_local_player_stance(int p_stance) {
	if (p_stance < 0 || p_stance > 2) return false;
	// ForceCrouch weapons refuse stance changes [orig: the case-169/170/172 gate
	// Entity_CheckWeaponSeatFlags(equipped, 0x40000) @ 0x4e0d8a; the seat-kind-3
	// mount refusal rides the unported mounting slice].
	if (weapon_active_ && weapon_force_crouch_) return false;
	if (stance_latch_ == p_stance) return false;
	// SELECT with mutual exclusion — the 0x1D apply writes one stance bit and clears
	// the other [orig: NapiNPServerMsg_HandleStanceChange @ 0x501c60: 169 -> crouch,
	// 170 -> prone, 172 -> clear both].
	stance_latch_ = p_stance;
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	return true;
}

Vector3 NovaSimulation::get_local_player_position() const {
	if (!world_ || !world_->cached.local_player.valid()) return Vector3();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return Vector3();
	// mission (x,y,z) -> Godot (x, z, -y).
	return Vector3(e->position.x, e->position.z, -e->position.y);
}

float NovaSimulation::get_local_player_yaw_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	// Engine heading (BAM32) -> mission yaw degrees, the (90 - heading) convention.
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(p->heading));
}

float NovaSimulation::get_local_player_pitch_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	return static_cast<float>(static_cast<double>(p->pitch) * opennova::world::kDegreesPerBam);
}

int NovaSimulation::get_local_player_body_anim_slot() const {
	if (!world_ || !world_->cached.local_player.valid()) return -1;
	// The same Entity.body_anim_slot the present pass reads for NPC models (written by the
	// infantry motor mirror, infantry.cpp). The avatar is host-managed and not in the present
	// registry, so main_game drives its body clip from this getter.
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->body_anim_slot : -1;
}

String NovaSimulation::get_local_player_anim_key() const {
	// The local player's full anim-state clip key ("anim_<name>"), straight from the motor's
	// selected state. Unlike the 8-slot BodyAnim enum (get_local_player_body_anim_slot), this carries
	// stance + jump (anim_idle_crouch / anim_walk_prone_forward / anim_jump_loop / ...), so
	// main_game drives the 3rd-person avatar via play_body_clip(key) for full stance fidelity.
	// [orig: off_8135F0 names ARE the .adm keys without the "anim_" prefix]
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return String();
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return String();
	return infantry_anim_key(p->inf.anim_state);
}

int NovaSimulation::get_local_player_anim_phase_ticks() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.clip_phase : 0;
}

Dictionary NovaSimulation::get_local_player_aim_overlay() const {
	// The torso-bend overlay state: the nine per-segment orientations from the exact BAM
	// blends [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14],
	// converted once here to mission-euler degrees — yaw via the canonical (90 - heading),
	// pitch unchanged (the retail placement builder applies authored pitch as Ry(-pitch),
	// and MissionObjectPlacer performs the matching basis conjugation). The host builds
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
	// The head-look decay term carries the arms-dip feed (the +0x371 weapon-switch
	// window drops it 0x2800000/tick; infantry_weapon_channel owns the decay)
	// [orig: @ 0x4b5cab..0x4b5cd5]. The lean term is the sim's lean angle
	// (entity+0xB0; ramp/decay in infantry_lean_tick); roll is the slope-conform
	// visual roll (entity+0x18) and torso_roll its sixteenth-step chaser
	// (entity+0x2DC, infantry_torso_roll_tick); body_pitch is the slope-conform
	// body pitch (entity+0x90; both fed by infantry_slope_pass). pitch_blend
	// stays 0 until its sim source (recoil impulses) is ported — the formulas
	// carry the term so it drops in without touching this seam.
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
	return out;
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
	weapon_def_.auto_fire = (flags & 0x100) != 0; // [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0]
	weapon_def_.burst3 = (flags & 0x20) != 0;     // [orig: WeaponAction_Fire @ 0x542c8a]
	weapon_def_.flags = flags;                    // raw mask: the scope gate + fov policy read it
	weapon_def_.flags2 = int(p_def.get("flags2", 0)); // Inset (0x200) picks the 7-step ease
	// The heat model [orig: WeaponDef +0x36C/+0x370/+0x374]. Absent keys leave 0,
	// which disables the model exactly as the original's zero-init does.
	weapon_def_.heat_per_shot = int(int64_t(p_def.get("heat_per_shot", 0)));
	weapon_def_.heat_decay_per_tick = int(int64_t(p_def.get("heat_decay_per_tick", 0)));
	weapon_def_.heat_glow_threshold = int(int64_t(p_def.get("heat_glow_threshold", 0)));
	weapon_scope_max_mag_ = float(double(p_def.get("scope_max_mag", 0.0)));
	// The 3P body-channel kinds [orig: weapon.def special_hold/attack_anim -> the
	// AdmDefs record +0xA4/+0xA8; world-wac-ai-re.md §14.8.4].
	weapon_hold_kind_ = int(int64_t(p_def.get("special_hold", 0)));
	weapon_attack_kind_ = int(int64_t(p_def.get("attack_anim", 0)));
	// The run-gait class [orig: 'run_anim' -> AdmDefs +0xAC; promotion @ 0x4b729d] and
	// ForceCrouch (0x40000): idle_mortar promotion + stance-change refusal.
	weapon_run_anim_ = int(int64_t(p_def.get("run_anim", 0)));
	weapon_force_crouch_ = (flags & 0x40000) != 0;
	// A held-AnimMap CHANGE advances a host serial; the local InfantryState observes
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
	weapon_hold_kind_ = 0;
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

bool NovaSimulation::request_local_player_scope_toggle() {
	// [orig: input case 6 @ 0x4e0420 gates currentAction not in {RELOAD, SWITCHFROM};
	//  Player_ToggleWeaponScope @ 0x4df0c0 gates def Flags & 3, flips g_scopeEngaged
	//  @ 0x82CE94, and queues the scopeup/scopedown FSM state @ 0x53f050/0x53f080]
	if (!weapon_active_) return false;
	opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	if (!opennova::world::weapon_fsm_scope_toggle_allowed(
			weapon_def_, *active_slot)) return false;
	// Scope-UP is refused while a movement key is held on a Scoped weapon
	// [orig: byte_B7653B && (flags & 1) -> return @ 0x4df29c].
	if (!player_view_.scope_engaged &&
			opennova::world::player_view_scope_up_blocked(player_view_, weapon_def_.flags))
		return false;
	// Inset optics cannot be raised under NVG. Non-Inset sights retain the
	// original independent behavior.
	if (!player_view_.scope_engaged && player_view_.nvg_active &&
			(weapon_def_.flags2 & 0x200) != 0)
		return false;
	// ForceScoped pins the raised sight: un-scoping is refused once settled
	// [orig: (flags1 & 0x20000000) == 0 || !g_weaponScopeActive @ 0x4df12d].
	if (player_view_.scope_engaged && (weapon_def_.flags & 0x20000000) != 0 &&
			!opennova::world::player_view_scope_ease_active(player_view_))
		return false;
	// The toggle latches this ease's step count (7 for Inset weapons, else 15;
	// 1 on the hipfire-return leg) and REFUSES while the previous ease runs
	// [orig: Player_ToggleWeaponScope @ 0x4df177 !activeFlag; Setup @ 0x4df1b3..0x4df36e].
	if (!opennova::world::player_view_set_engaged(player_view_, !player_view_.scope_engaged,
			(weapon_def_.flags2 & 0x200) != 0))
		return false;
	if (player_view_.scope_engaged)
		opennova::world::weapon_fsm_queue_scope_up(*active_slot);
	else
		opennova::world::weapon_fsm_queue_scope_down(*active_slot);
	return true;
}

bool NovaSimulation::request_local_player_binoculars_toggle() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	if (local == nullptr) return false;
	// Retail refuses binoculars while a PowerThrow charge is live. Allowing the
	// view to rise would suppress held weapon input and turn the charge into an
	// unintended release [orig: g_fireChargeStartTick @ 0xB76800; action 26 gate].
	if (power_throw_start_tick_ != 0) return false;
	// An active scope also blocks binoculars in a gunner parent slot.
	if (player_view_.scope_engaged && local->mounted &&
			local->mount_type == opennova::world::SeatType::Gunner)
		return false;

	const bool requested =
			opennova::world::player_view_toggle_binoculars(player_view_);
	if (requested) {
		const double unit =
				(static_cast<double>(std::rand()) + 0.5) /
				(static_cast<double>(RAND_MAX) + 1.0);
		const double angle = unit * kTau;
		binocular_yaw_offset_deg_ =
				static_cast<float>(std::cos(angle) * kBinocularAimOffsetDeg);
		binocular_pitch_offset_deg_ =
				static_cast<float>(std::sin(angle) * kBinocularAimOffsetDeg);
	} else {
		binocular_yaw_offset_deg_ = 0.0f;
		binocular_pitch_offset_deg_ = 0.0f;
	}
	refresh_local_player_view_effects();
	return requested;
}

bool NovaSimulation::request_local_player_nvg_toggle() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	if (local == nullptr) return false;

	if (!player_view_.nvg_active) {
		nvg_scope_restore_ = false;
		if (weapon_active_ && player_view_.scope_engaged &&
				(weapon_def_.flags2 & 0x200) != 0 &&
				!opennova::world::player_view_scope_ease_active(player_view_)) {
			nvg_scope_restore_ = request_local_player_scope_toggle();
		}
		return opennova::world::player_view_toggle_nvg(player_view_);
	}

	// Clear NVG before the normal scope-up request so the Inset refusal no
	// longer applies, then consume the one-shot restore latch.
	opennova::world::player_view_toggle_nvg(player_view_);
	const bool restore_scope = nvg_scope_restore_;
	nvg_scope_restore_ = false;
	if (restore_scope && !player_view_.scope_engaged)
		request_local_player_scope_toggle();
	return false;
}

int NovaSimulation::request_local_player_nvg_gain(int p_delta) {
	return opennova::world::player_view_adjust_nvg_gain(player_view_, p_delta);
}

void NovaSimulation::set_local_player_camera_third_person(bool p_third_person) {
	player_view_.third_person = p_third_person; // [orig: g_camera_mode @ 0xA890C8]
	refresh_local_player_view_effects();
}

// One 62.5 Hz tick of the view state, before the weapon pump: the ADS ease and the
// third-person anchor chase run at the WORLD cadence, so camera lag is identical at
// any render rate. Retail's Player_UpdatePerFrame call precedes the later
// WeaponAction_ProcessAllEntities call, so this tick's settle promoter is visible to
// action routing while an action's unscope/rescope begins easing on the next tick
// [orig: call sites @ 0x42c18e / @ 0x526786; promoter @ 0x4de4f7].
void NovaSimulation::tick_local_player_view() {
	if (!world_ || !world_->cached.local_player.valid()) {
		opennova::world::player_view_update_effective_modes(
				player_view_, false, world_ != nullptr && world_->round_end.ended);
		player_view_.tp_anchor_valid = false;
		return;
	}
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return;
	refresh_local_player_view_effects();
	// The anchor-chase target is Position + CameraOffset — the posed head-bone eye
	// [orig: ThirdPersonCamera_Update @ 0x437b70..76], fed by the host's per-frame
	// skeleton sample (see local_eye_mission_). Without a sample: Position + 1.0,
	// the witnessed NON-person bump [orig: @ 0x437e8f].
	const float eye[3] = {
		local_eye_valid_ ? local_eye_mission_[0] : e->position.x,
		local_eye_valid_ ? local_eye_mission_[1] : e->position.y,
		local_eye_valid_ ? local_eye_mission_[2] : e->position.z + 1.0f,
	};
	opennova::world::player_view_tick(player_view_, eye);
}

void NovaSimulation::set_local_player_eye(const Vector3 &p_eye_godot, bool p_valid) {
	// Godot (x, y, z) -> mission (x, -z, y), the get_local_player_position inverse.
	local_eye_mission_[0] = p_eye_godot.x;
	local_eye_mission_[1] = -p_eye_godot.z;
	local_eye_mission_[2] = p_eye_godot.y;
	local_eye_valid_ = p_valid;
}

Dictionary NovaSimulation::get_local_player_view() const {
	Dictionary out;
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	out["scope_engaged"] = player_view_.scope_engaged;
	out["binoculars_requested"] = player_view_.binoculars_requested;
	out["binoculars_raised"] = player_view_.binoculars_raised;
	out["binoculars_view_active"] = player_view_.binoculars_view_active;
	out["binocular_yaw_offset_deg"] = binocular_yaw_offset_deg_;
	out["binocular_pitch_offset_deg"] = binocular_pitch_offset_deg_;
	out["nvg_active"] = player_view_.nvg_active;
	out["nvg_visible"] =
			opennova::world::player_view_nvg_visible(player_view_);
	out["nvg_gain"] = player_view_.nvg_gain;
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	out["mounted"] = local != nullptr && local->mounted;
	// Structural proxy for Player_IsVehicleHasAttackCapability until mounted
	// weapon inventory is modeled: these seat classes replace the on-foot
	// upper-body weapon channel; passenger seats do not.
	out["vehicle_attack_context"] = local != nullptr && mount_blocks_weapon_channel(*local);
	out["scope_fraction"] = opennova::world::player_view_scope_fraction(player_view_);
	// The NoCardSwitch reload rule: while the equipped slot is mid-RELOAD on a
	// weapon WITHOUT NoCardSwitch (flags 0x2000000), the FP camera drops the ADS
	// view bias for the frame — the host reads the eased fraction as 0.
	// [orig: Player_UpdateFirstPersonCamera @ 0x4dd439/@ 0x4dd4cc; the same
	//  predicate is Player_IsReloadingCardSwitchWeapon @ 0x4dcdd0 (ex kong
	//  "Player_IsDriverInVehicle"), whose one caller refuses fire @ 0x5cf7be]
	out["suppress_view_bias"] = weapon_active_ &&
			active_slot->current == opennova::world::weapon_action::kReload &&
			(weapon_def_.flags & opennova::world::weapon_flag::kNoCardSwitch) == 0;
	// On the supported on-foot first-person path, the standard SIGHTS card replaces
	// the FP viewmodel once ADS settles. Scoped and Sighted are asymmetric selectors;
	// NoCardSwitch clears both unless ForceScoped overrides it. The frame draws the
	// card or the FP viewmodel, never both. [orig: Render_ProcessMainSceneFrame
	// @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15; suppression @0x4dcce0]
	out["scope_card_active"] = weapon_active_ &&
			opennova::world::weapon_sights_card_eligible(
					weapon_def_, *active_slot) &&
			player_view_.scope_engaged && !player_view_.third_person &&
			!player_view_.binoculars_view_active &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	out["fov_h_deg"] = opennova::world::player_view_fov_h_deg(player_view_,
			weapon_active_ ? weapon_def_.flags : 0,
			weapon_active_ ? weapon_scope_max_mag_ : 0.0f);
	// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position map.
	out["tp_anchor"] = Vector3(player_view_.tp_anchor[0], player_view_.tp_anchor[2],
			-player_view_.tp_anchor[1]);
	out["tp_anchor_valid"] = player_view_.tp_anchor_valid;
	// The FP camera roll in degrees: roll = torsoRoll + lean/4 [orig: the on-foot
	// person leg @ 0x437fe6 — g_view_rot_roll = entity+0x2DC + (entity+0xB0 >> 2)].
	{
		float fp_roll_deg = 0.0f;
		if (world_ && world_->ai && world_->cached.local_player.valid()) {
			if (const AiEntity *p = world_->ai->for_handle(world_->cached.local_player)) {
				const int32_t roll_bam = opennova::io::bam_add(
						p->inf.torso_roll, opennova::io::bam_sar(p->inf.lean_angle, 2));
				fp_roll_deg = static_cast<float>(
						static_cast<double>(roll_bam) * opennova::world::kDegreesPerBam);
			}
		}
		out["fp_roll_deg"] = fp_roll_deg;
	}
	return out;
}

float NovaSimulation::fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect) {
	return opennova::world::fov_vertical_from_horizontal_deg(p_fov_h_deg, p_aspect);
}

// One 62.5 Hz pump of the local player's slot, after the world logic tick. The world
// tick now owns the parallel NPC UseGun parent-slot pump; this host method remains the
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
	if ((weapon_def_.flags & 0x80000000u) != 0) {
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
		// The play consumes the slot ring and latches the served variant — the host
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
	// finish leg. Records themselves stay in logic-tick order until the host drains.
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
				const int32_t dir_pitch = p->pitch;
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
								(weapon_def_.flags & 0x20000000) == 0)) {
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
				round.charge = pending_throw_charge_;
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
					fire.hit_part = shot_seq;
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
		} else if (borrowed_usegun_slot && !joiner_ &&
				local_usegun_mount_.valid()) {
			// parentSlot==3 addresses the PARENT entity, not the actor, and
			// recomputes the combo from the mounted Def. The authority's local
			// parent handle is already the canonical wire handle. A joiner has
			// no proven local-parent -> host-H map yet, so that half remains
			// explicitly deferred in D-WPN-8.
			// [orig: WeaponAction_Reload @0x543108..0x543157]
			const opennova::world::WeaponTableEntry *mounted_def =
					world_->weapons.by_index(local_usegun_weapon_adm_);
			if (mounted_def != nullptr) {
				reload.entity_handle = local_usegun_mount_.packed;
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
				(weapon_def_.flags2 & 0x200) != 0);
	}
	if (ev.rescope) {
		++weapon_rescope_serial_;
		opennova::world::player_view_set_engaged(player_view_, true,
				(weapon_def_.flags2 & 0x200) != 0);
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
	const bool windup_active = (weapon_def_.flags & 0x80000000u) != 0 &&
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
	// Weapon heat, clamped where the original's info builder clamps it — the drawer
	// downstream reads a plain 0..0xFFFF level and self-hides at 0.
	// [orig: HUD_BuildEntityInfo @ 0x4b852e -> hudInfo+60, the clamp @ 0x4b854d]
	{
		const int32_t heat = world_ != nullptr
				? opennova::world::weapon_slot_accumulated_heat(
						  weapon_def_, active_slot,
						  static_cast<int32_t>(world_->logic_tick))
				: 0;
		out["heat"] = heat > opennova::world::weapon_heat::kFull
				? opennova::world::weapon_heat::kFull
				: heat;
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
	// + its own playhead for the host's mask-bone override. The key remains populated
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

// HUD health/team. The original rebuilds these into its per-frame HUD info struct every frame
// (health ratio at +92 = currentHealth/maxHealth, team byte at +374). We surface the raw values
// and let the HUD compute the ratio. [orig: HUD_BuildEntityInfo @0x4b8440]
int NovaSimulation::get_local_player_health() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->health : 0;
}

int NovaSimulation::get_local_player_max_health() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 100;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p || p->inf.max_health <= 0) return 100;
	return p->inf.max_health;
}

int NovaSimulation::get_local_player_team() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->team) : 0;
}

int NovaSimulation::get_local_player_class() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->player_class) : 0;
}

String NovaSimulation::get_local_player_weapon_name() const {
	if (!world_ || !world_->cached.local_player.valid()) return String();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return String();
	const opennova::world::WeaponTableEntry *weapon =
			world_->weapons.by_index(e->equipped_adm_index);
	return weapon ? String(weapon->name.c_str()) : String();
}
