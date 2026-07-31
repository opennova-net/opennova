// NovaSimulation — the LOCAL PLAYER cluster: view effects, armory/usegun and
// mount interactions, loadout (slot pool / spawn kit / map rules), input and
// spawn, pose getters, and the equipped-weapon FSM (net-re §5.62).
#include "simulation/nova_simulation_internal.h"

#include <def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*

#include <godot_cpp/classes/file_access.hpp> // weapon.sav lives on the filesystem, not a mount

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
	// The witnessed non-authority path queues C2S 0x26 (attach) /
	// sends 0x27 (detach) and waits for the 0x0A stream to confirm [orig:
	// Entity_RequestVehicleAttach @0x4364a0 / Entity_SendDetachPacket @0x435510].
	// Joiners therefore choose a candidate locally but never mutate L until the
	// authoritative relationship echo arrives.
	// A missing EquippedSlot passes the retail action gate; active_local_weapon_slot
	// supplies the inert slot state used by the modeled gate below.
	const opennova::world::Entity *toggle_player =
			world_->registry.get(world_->cached.local_player);
	if (toggle_player == nullptr || !toggle_player->alive ||
			toggle_player->health <= 0)
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
	const auto find_toggle_candidate =
			[&](opennova::world::NearestSeatHit &r_hit) {
				if (!toggle_player->mounted) {
					opennova::world::Entity *ground =
							world_->registry.get(toggle_player->ground_target);
					if (ground != nullptr && !ground->seats.empty()) {
						const int seat_index = world_->commands.find_best_seat(
								*ground, toggle_player->handle);
						if (seat_index >= 0) {
							r_hit.vehicle = ground->handle;
							r_hit.seat_index = seat_index;
							r_hit.type = ground->seats[
									static_cast<size_t>(seat_index)].type;
							return true;
						}
					}
				}
				return opennova::world::find_nearest_free_seat(
						*world_, *toggle_player, r_hit, false);
			};
	if (joiner_) {
		// The non-authority client chooses the same local nearest-seat candidate,
		// but sends only its packed carrier and authored 1-based model bone. L is
		// unchanged until the host's 0x0A relationship confirms the request.
		// [orig: Entity_RequestVehicleAttach @0x4364a0 /
		// Entity_SendDetachPacket @0x435510]
		if (!runtime_ || toggle_player == nullptr) return false;
		// A mounted release is unambiguously a detach request. Wait for the
		// authoritative 0x0A echo before a later release can select a new seat.
		if (toggle_player->mounted)
			return runtime_->queue_vehicle_detach(
					toggle_player->mount_target.packed);
		opennova::world::NearestSeatHit hit;
		if (!find_toggle_candidate(hit)) return false;
		opennova::world::Entity *vehicle = world_->registry.get(hit.vehicle);
		if (vehicle == nullptr || hit.seat_index < 0 ||
				hit.seat_index >= static_cast<int>(vehicle->seats.size()))
			return false;
		return runtime_->queue_vehicle_attach(
				hit.vehicle.packed,
				vehicle->seats[static_cast<size_t>(hit.seat_index)].bone_index);
	}
	// The null EquippedSlot rejection belongs to UseGun itself, not the
	// top-level USE action: an unarmed local player can still enter an ordinary
	// passenger/control seat. It is also deliberately an out-of-session-only
	// player gate; force/script and NAPI authority paths bypass it.
	// [orig: Entity_AttachToUseGunSlot @0x546b80, reject
	//  !is_in_session && Flags&0x100 && !EquippedSlot @0x546c07]
	if (!listen_server_ && !weapon_active_) {
		opennova::world::NearestSeatHit hit;
		if (find_toggle_candidate(hit) &&
				hit.type == opennova::world::SeatType::Gunner)
			return false;
	}
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
	// shell-side and are unmodeled here: docs/interface/hud-re.md (D-HUD-11)].
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
	// A promoted kit changes what the joiner's 0x2F pair should carry; re-arm the
	// seam (no-op for hosts and before the weapon catalog exists).
	push_joiner_loadout_kit();
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
	// Sim-side class only. The 0x2F WIRE class is NOT taken from here — retail reads it
	// straight out of the assigned side's profile block, the same integer that picks the
	// kit page [orig: Game_StartMission @0x5257a0], so push_joiner_loadout_kit sources
	// both from weapon_profile_. The pushes below just keep the seam re-armed.
	if (!world_ || p_player_class < 5 || p_player_class > 9) return false;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) {
		// A joiner's shell applies the profile class before L has spawned
		// (name-match). Latch it; the joiner spawn block stamps the entity.
		pending_local_player_class_ = p_player_class;
		push_joiner_loadout_kit();
		return true;
	}
	e->player_class = static_cast<uint8_t>(p_player_class);
	push_joiner_loadout_kit();
	return true;
}

bool NovaSimulation::apply_local_player_loadout(const TypedArray<Dictionary> &p_kit,
                                                int p_player_class) {
	return apply_local_player_loadout_impl(
			p_kit, p_player_class, /*p_submit_joiner_request=*/true);
}

bool NovaSimulation::apply_local_player_loadout_impl(
		const TypedArray<Dictionary> &p_kit, int p_player_class,
		bool p_submit_joiner_request) {
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
		if (p_submit_joiner_request &&
		    weapon_availability_.value_for(idx) ==
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
	if (p_submit_joiner_request) push_joiner_loadout_kit();
	// A mid-session accept (the armory ACCEPT) re-submits the loadout on the wire;
	// the initial join pair stays owned by the 0x1A trigger. [orig:
	// WeaponLoadout_ApplyFromBuffer @0x565d94 — the is_in_session leg sends one 0x2F
	// with the live team/class/equipped slot; the S2C 0x5A grant then refills]
	if (p_submit_joiner_request && joiner_ && runtime_ && runtime_->in_match())
		runtime_->queue_loadout_resubmit();
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
	// The 0x2F pair's SECOND submit carries the LIVE equipped slot, not the fixed 195
	// [orig: Game_StartMission @0x525c2e passes g_currentWeaponSlot — the value the
	// spawn fill just settled]. The last push before the S2C 0x1A release used to run
	// BEFORE this rebuild, so the seam shipped a stale slot twice; re-push here, after
	// the inventory has settled. push_joiner_loadout_kit never rebuilds (it holds a
	// one-way re-entry latch), so this cannot recurse.
	if (!p_select_spawn_default) {
		push_joiner_loadout_kit();
		return;
	}
	const opennova::world::WeaponSwitchGates gates = local_weapon_switch_gates();
	if (!opennova::world::weapon_select_slot(
	            table, local_inventory_,
	            opennova::world::weapon_combo::kDefaultSpawnCombo,
	            !gates.equip_blocked)) {
		// An empty table (the armory all-NONE kit) equips nothing.
		if (e != nullptr) e->equipped_adm_index = 0xFF;
		push_joiner_loadout_kit();
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
	push_joiner_loadout_kit();
}

bool NovaSimulation::seed_session_kit_from_profile() {
	// The Game_StartMission copy: in a live session the assigned side's profile page
	// becomes the resident kit buffer [orig: @0x525813
	// Buffer_CopyUntilDoubleNull(restrictionData, page, 0x800)], which is BOTH what the
	// local slot pool is built from [orig: Player_InitPlayer @0x4e15f0 ->
	// AvatarDef_BuildDisplayList(.., restrictionData)] and what the C2S 0x2F serializes
	// [orig: NetPacket_SendLoadoutSubmit @0x42cdc0]. Retail keeps one buffer; keeping
	// two is how the local view and the wire drift apart.
	//
	// Gated on a live session exactly as retail is (`is_in_session` covers a LISTEN HOST
	// as well as a joiner), so single player and the editor keep the mission's .bms kit.
	// The S2C 0x50 team assign re-runs this for the NEW side [orig:
	// NapiNPClientMsg_TeamAssign @0x431a9a re-copies the page into restrictionData].
	if (!world_ || world_->weapons.empty()) return false;
	if (!host_listen_ && !joiner_) return false;
	uint8_t team = (joiner_ && runtime_) ? runtime_->assigned_team() : 0;
	if (team == 0) {
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		if (local != nullptr) team = local->team;
	}
	// An UNLATCHED team must not commit a page. The side selector is the S2C 0x04 tail
	// byte [orig: byte_A85B48 @0x425499], and retail cannot reach this copy before it is
	// latched — admission delivers 0x04 long before Game_StartMission runs, so
	// @0x525798's `team == 1 || team == 3` always sees a real value. Our catalog can land
	// first, and since anything-not-1-or-3 selects the RED block, seeding at team 0 would
	// commit the wrong side's page and then have to flip it. Wait instead; the pump
	// re-seeds the moment the latch (or a later 0x50 reassignment) changes the side.
	if (team == 0) return false;
	const opennova::playersav::Side &side =
			weapon_profile_.side(opennova::playersav::side_for_team(team));
	uint8_t player_class = side.player_class;
	if (player_class < 5 || player_class > 9) player_class = 8;
	const opennova::playersav::KitPage *page = side.page_for_class(player_class);
	if (page == nullptr || page->entries.empty()) return false;
	std::vector<opennova::world::WeaponKitEntry> kit;
	kit.reserve(page->entries.size());
	for (const opennova::playersav::KitEntry &e : page->entries)
		kit.push_back(opennova::world::WeaponKitEntry{e.name, e.ammo_primary,
		                                             e.ammo_secondary, e.flags});
	spawn_kit_ = std::move(kit);
	spawn_kit_set_ = true;
	weapon_profile_seeded_side_ =
			static_cast<int>(opennova::playersav::side_for_team(team));
	return true;
}

bool NovaSimulation::reseed_session_kit_on_side_change() {
	// The team latch can arrive AFTER the catalog — the S2C 0x04 tail byte on the way in
	// [orig: byte_A85B48 @0x425499], or a later S2C 0x50 reassignment moving us across
	// the line [orig: NapiNPClientMsg_TeamAssign @0x4319db]. Retail re-reads the profile
	// for the new side on the 0x50 leg and re-copies its page into restrictionData
	// @0x431a9a; the 0x04 case it simply never has, because the latch precedes
	// Game_StartMission's copy. Both collapse to the same rule here: whenever the SIDE
	// the selector names stops matching the side the resident buffer was copied from,
	// re-copy. Keyed on the side rather than the raw team so a 1<->3 (or 2<->4)
	// reassignment inside one side does not needlessly rebuild — those read the same
	// block @0x525798 — and so an in-session armory ACCEPT, which changes the buffer but
	// never the side, is not undone.
	if (!joiner_ && !host_listen_) return false;
	uint8_t team = (joiner_ && runtime_) ? runtime_->assigned_team() : 0;
	if (team == 0) {
		const opennova::world::Entity *local =
				world_ ? world_->registry.get(world_->cached.local_player) : nullptr;
		if (local != nullptr) team = local->team;
	}
	if (team == 0) return false;
	if (static_cast<int>(opennova::playersav::side_for_team(team)) ==
	    weapon_profile_seeded_side_)
		return false;
	if (!seed_session_kit_from_profile()) return false;
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	return true;
}

void NovaSimulation::push_joiner_loadout_kit() {
	// The joiner's C2S 0x2F submission content — the wire seam D-NET-168 tracked.
	//
	// It does NOT come from the mission. Mission_LoadBMSFile SKIPS both the loadout
	// chunk and the availability chunk whenever a session is live — for a listen host
	// as well as a joiner [orig: @0x40f694 `cmp is_in_session, 0` -> the fseek pair
	// @0x40f6b2 / @0x40f6e1; only the non-session branch reads, availability-filters
	// @0x40f834, knife-falls-back @0x40f899 and writes restrictionData @0x40f961].
	//
	// The MP source is the player profile, indexed BY the class. Game_StartMission
	// picks the assigned side's block, reads ONE integer out of it — the class byte —
	// and that single integer selects BOTH the wire class and which of the five
	// 2048-byte kit pages is copied into restrictionData [orig: @0x525767..@0x525836:
	// esi = (team==1||team==3) ? blue block : red block, eax = *(u8*)esi,
	// switch(class-5) -> page = esi + {6,0x806,0x1006,0x1806,0x2006},
	// Buffer_CopyUntilDoubleNull -> NetPacket_SendLoadoutSubmit(team, eax, page, 195)].
	// Because one integer drives both, retail can never submit a class-illegal kit;
	// pairing the mission's .bms kit with a hardcoded class is what made a live retail
	// host drop our WPN_SR25 (charfilter sniper, mask 2, against class 8).
	//
	// Row resolution mirrors the original builder: names that miss the catalog are
	// skipped whole, and the ammo/flags values are the atol result truncated to the
	// low byte (-1 -> 0xFF) [orig: NetPacket_SendLoadoutSubmit @0x42cdc0 —
	// AvatarDef_FindByName skip @0x42cf0b, truncation @0x42cf7c/@0x42cfbc/@0x42cff7].
	// Deliberately NO charfilter/teamfilter test runs here: retail's CLIENT submits
	// unfiltered (@0x42cdc0 tests neither adm+124 nor adm+128) and prevention lives in
	// the PLAYER_INFO kit editor [orig: populate_weapon_slot_lists @0x560430].
	if (!joiner_ || !runtime_ || !world_) return;
	// finish_load runs before the shell loads weapon.def (MissionRuntime orders
	// load_from_mission_data ahead of load_weapon_table), and a kit resolved against an
	// EMPTY catalog would skip every row — latching that would submit a zero-entry 0x2F
	// pair AND disarm the runtime's capture-default fallback. Leave the seam unarmed
	// until the catalog exists; load_weapon_table re-pushes from the carried state.
	if (world_->weapons.empty()) return;
	// rebuild_local_player_loadout re-pushes once the equipped combo has settled; this
	// latch keeps that one-way (a push must never drive a rebuild back into itself).
	if (pushing_joiner_loadout_kit_) return;
	pushing_joiner_loadout_kit_ = true;

	opennova::np::JoinerConnection::LoadoutKit wire_kit;
	// The side selector. The host's S2C 0x04 tail byte is retail's byte_A85B48, and
	// teams 1/3 read the BLUE block, 2/4 the RED one [orig: @0x525788]. Before that
	// latch lands (assigned_team() == 0) fall back to the local entity's own team when
	// L already exists. With neither, side_for_team(0) resolves to RED — anything that
	// is not 1 or 3 reads the red block @0x525798 — which is why the resident buffer is
	// NOT copied at team 0 (see seed_session_kit_from_profile) and why the pump re-seeds
	// once the latch lands. The seam itself still arms so the runtime has content.
	uint8_t team = runtime_->assigned_team();
	if (team == 0) {
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		if (local != nullptr) team = local->team;
	}
	const opennova::playersav::Side &side =
			weapon_profile_.side(opennova::playersav::side_for_team(team));
	// ONE integer: the wire class byte AND the page index [orig: eax = *(u8*)esi, the
	// switch(class-5) page map]. The [5,9] clamp is retail's own per-side session-start
	// clamp [orig: apply_session_settings_to_globals @0x5516ab..@0x5516ec]; 8
	// (rifleman) is the shipped profile default [orig: PlayerProfile_InitDefaults
	// @0x54bbe0/@0x54bbe3] and also what the host's 0x2F envelope requires — it aborts
	// on a nonzero class outside [5,9] [orig: @0x5158b1 -> @0x515fa5].
	uint8_t player_class = side.player_class;
	if (player_class < 5 || player_class > 9) player_class = 8;
	wire_kit.player_class = player_class;
	// The pair's SECOND submit carries the live equipped slot instead of the fixed 195
	// [orig: Game_StartMission @0x525c2e passes g_currentWeaponSlot].
	wire_kit.equipped_combo =
			local_inventory_valid_ ? local_inventory_.equipped_combo : -1;
	// One side block -> (clamped class, ADM-resolved rows). Retail re-reads the profile
	// block the wire team byte names on EVERY profile-sourced submission, so the row
	// resolve is shared by the applied kit below and by both resident side blocks.
	auto resolve_side = [this](const opennova::playersav::Side &s, uint8_t klass) {
		std::vector<opennova::LoadoutSubmitEntry> rows;
		const opennova::playersav::KitPage *p = s.page_for_class(klass);
		if (p == nullptr) return rows;
		for (const opennova::playersav::KitEntry &entry : p->entries) {
			const int adm = world_->weapons.index_of(entry.name.c_str());
			if (adm < 0) continue; // [orig: the AvatarDef_FindByName gate @0x42cf0b]
			rows.push_back(opennova::LoadoutSubmitEntry{
					static_cast<uint8_t>(adm),
					static_cast<uint8_t>(entry.ammo_primary),
					static_cast<uint8_t>(entry.ammo_secondary),
					static_cast<uint8_t>(entry.flags)});
		}
		return rows;
	};
	// The RESIDENT rows are the resident kit buffer, not a fresh read of the profile
	// page. Retail has exactly ONE buffer: Game_StartMission copies the profile page
	// into restrictionData, Player_InitPlayer builds the local display list from that
	// same restrictionData, and NetPacket_SendLoadoutSubmit serializes it — so what we
	// hold locally and what we tell the host are the same bytes by construction. An
	// in-session armory ACCEPT overwrites restrictionData and its re-send therefore
	// carries the ACCEPTED kit [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 ->
	// @0x565d94], which reading the profile back here would silently undo.
	// `spawn_kit_` is our restrictionData; `seed_session_kit_from_profile` is the
	// Game_StartMission copy that fills it.
	{
		const std::vector<opennova::world::WeaponKitEntry> &resident =
				spawn_kit_set_ ? spawn_kit_ : opennova::world::weapon_kit_default();
		for (const opennova::world::WeaponKitEntry &entry : resident) {
			const int adm = world_->weapons.index_of(entry.name.c_str());
			if (adm < 0) continue; // [orig: the AvatarDef_FindByName gate @0x42cf0b]
			wire_kit.rows.push_back(opennova::LoadoutSubmitEntry{
					static_cast<uint8_t>(adm),
					static_cast<uint8_t>(entry.ammo_primary),
					static_cast<uint8_t>(entry.ammo_secondary),
					static_cast<uint8_t>(entry.flags)});
		}
	}
	// BOTH side blocks stay resident on the seam. Retail keeps the whole profile in
	// memory and re-selects the side the NEW team byte names when the S2C 0x50 team
	// assign moves us across the line — class AND page together [orig:
	// NapiNPClientMsg_TeamAssign @0x431a35..@0x431a9a]. Without these the resubmit
	// would ship whatever side was resident when the seam was last pushed, i.e. the
	// OLD side's page against the NEW side's team byte.
	auto fill_side = [&](opennova::np::JoinerConnection::LoadoutKit::SideKit &out,
	                     const opennova::playersav::Side &s) {
		uint8_t k = s.player_class;
		if (k < 5 || k > 9) k = 8; // [orig: the per-side clamp @0x5516ab..@0x5516ec]
		out.set = true;
		out.player_class = k;
		out.rows = resolve_side(s, k);
	};
	fill_side(wire_kit.blue, weapon_profile_.blue);
	fill_side(wire_kit.red, weapon_profile_.red);
	runtime_->set_loadout_kit(std::move(wire_kit));
	pushing_joiner_loadout_kit_ = false;
}

Error NovaSimulation::load_weapon_profile(const String &p_path) {
	// [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0] — the caller supplies the already
	// resolved absolute path, which retail builds as
	// g_ExpansionName[0] ? "expansion\\<g_ExpansionName>\\weapon.sav" : "weapon.sav"
	// [orig: @0x54f68c..@0x54f6b7]. weapon.sav is a SAVE file on the filesystem, not a
	// PFF/mount entry, so it is read through FileAccess rather than the resource root.
	// Any failure keeps the shipped defaults installed [orig: PlayerProfile_InitDefaults
	// @0x54bb40] and reports the reason; a joiner still submits a class-legal pair.
	weapon_profile_loaded_ = false;
	weapon_profile_ = opennova::playersav::make_defaults().slots[0];
	Error result = OK;
	if (p_path.is_empty()) {
		result = ERR_INVALID_PARAMETER;
	} else {
		Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
		if (file.is_null()) {
			print_verbose(vformat(
					"weapon.sav: cannot open \"%s\" — keeping the shipped profile defaults",
					p_path));
			result = ERR_FILE_CANT_OPEN;
		} else {
			const PackedByteArray bytes =
					file->get_buffer(static_cast<int64_t>(file->get_length()));
			file->close();
			opennova::playersav::File parsed;
			if (!opennova::playersav::read(bytes.ptr(),
			                               static_cast<std::size_t>(bytes.size()),
			                               parsed)) {
				// The header gate is magic "FPBC" (0x43425046) + version "0211"
				// (0x31313230); anything else is not a profile file.
				print_verbose(vformat(
						"weapon.sav: \"%s\" is not a readable profile — keeping the shipped defaults",
						p_path));
				result = ERR_FILE_CORRUPT;
			} else {
				// The per-side [5,9] clamp retail applies at session start
				// [orig: apply_session_settings_to_globals @0x5516ab..@0x5516ec].
				opennova::playersav::clamp_classes(parsed);
				weapon_profile_ = parsed.slots[0];
				weapon_profile_loaded_ = true;
			}
		}
	}
	// The record just changed, so the resident kit buffer and the seam both have to
	// follow it. Retail never has to re-run this because PlayerProfile_LoadAllFromDisk
	// completes at boot / expansion switch, long before Game_StartMission copies a page
	// into restrictionData [orig: @0x54f4d0 vs @0x525813]; our profile can only load
	// AFTER the runtime exists (the reader is a sim method), so the copy is redone here
	// instead of depending on the shell's call order. seed_session_kit_from_profile is
	// a no-op outside a live session and before the catalog resolves names, so this
	// leaves single player and a pre-catalog load exactly as they were.
	if (seed_session_kit_from_profile())
		rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// The seam's class AND its kit page both come out of this record — re-arm it.
	push_joiner_loadout_kit();
	return result;
}

Dictionary NovaSimulation::get_weapon_profile_summary() const {
	const auto side_summary = [](const opennova::playersav::Side &s) {
		Dictionary d;
		d["player_class"] = int(s.player_class);
		d["avatar_a"] = int(s.avatar_a);
		d["avatar_b"] = int(s.avatar_b);
		d["avatar_packed"] = int(s.avatar_packed);
		Array names;
		if (const opennova::playersav::KitPage *page = s.selected_page()) {
			for (const opennova::playersav::KitEntry &entry : page->entries)
				names.push_back(String::utf8(entry.name.c_str()));
		}
		d["kit"] = names;
		return d;
	};
	Dictionary out;
	out["loaded"] = weapon_profile_loaded_;
	out["blue"] = side_summary(weapon_profile_.blue);
	out["red"] = side_summary(weapon_profile_.red);
	return out;
}

void NovaSimulation::apply_joiner_authoritative_loadout() {
	if (!joiner_ || !runtime_ || !world_ || world_->weapons.empty()) return;
	const uint64_t revision = runtime_->authoritative_loadout_revision();
	if (revision == 0 || revision <= joiner_applied_loadout_revision_) return;

	const opennova::WeaponLoadout &grant = runtime_->authoritative_loadout();
	TypedArray<Dictionary> kit;
	for (const opennova::WeaponLoadoutSlot &slot : grant.slots) {
		const opennova::world::WeaponTableEntry *def =
				world_->weapons.by_index(slot.type_id);
		if (def == nullptr) continue; // retail drops failed AdmDef lookups
		Dictionary row;
		row["name"] = String::utf8(def->name.c_str());
		// The wire bytes are signed clip counts. 0xFF is the authored/default
		// sentinel, not 255 clips [orig: 0x4295c4..0x429613].
		row["ammo_primary"] = static_cast<int>(
				static_cast<int8_t>(slot.ammo_primary));
		row["ammo_secondary"] = static_cast<int>(
				static_cast<int8_t>(slot.ammo_secondary));
		row["flags"] = static_cast<int>(
				static_cast<int8_t>(slot.ammo_alt));
		kit.push_back(row);
	}
	// Do not echo an authoritative grant back as a new C2S 0x2F request. The
	// S2C handler rebuilds the slots directly at recv-before-actions. The rebuild
	// inside still re-arms the seam, but its ROWS come from the profile page, never
	// from the grant — only the live equipped slot refreshes, which is exactly what
	// retail's second submit carries [orig: Game_StartMission @0x525c2e].
	if (apply_local_player_loadout_impl(
				kit, grant.avatar_class, /*p_submit_joiner_request=*/false))
		joiner_applied_loadout_revision_ = revision;
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
	// In a live session the resident kit buffer is the assigned side's profile page,
	// copied in the moment the catalog can resolve its names — retail's
	// Game_StartMission copy into restrictionData [orig: @0x525813], which runs before
	// Player_InitPlayer builds the display list from that same buffer. Offline this
	// no-ops and the mission's .bms kit stands.
	seed_session_kit_from_profile();
	// The LOCAL player's slot pool builds from the spawn kit (the profile page in a
	// session, the mission/armory loadout offline, else the WPN_M4AUTO default kit) and
	// selects the spawn default — the Player_InitPlayer weapon leg [orig: @ 0x4e15f0;
	// the default kit literal @ 0x5246be]. This subsumes the bare adm-index stamp above
	// for the local player.
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// The catalog exists now: arm the joiner's 0x2F seam from the carried spawn kit
	// (finish_load's earlier push deliberately no-ops against the empty catalog, and
	// a menu join that never opens PLAYER_INFO has no later class/loadout apply to
	// re-push through — without this the pair would ride zero kit entries).
	push_joiner_loadout_kit();
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
	// g_weaponScopeActive is the post-ease promotion, not raw scope intent.
	// This one value feeds body pose, per-shot recoil scaling, and CanFire.
	// [orig: promoter @0x4DE4F7; local body mirror @0x4B5D95]
	const bool scope_promoted = weapon_active_ && player_view_.scope_engaged &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	// The local-player weapon-channel inputs, refreshed before the body updater runs —
	// the Flags-bit refresh (Flags|0x10 from g_weaponScopeActive; the binoculars bit
	// stays false until a shell binoculars input exists). This is the ONLY part of the
	// weapon channel the original gates on locality [orig: @ 0x4b5d7f]; the hold kind is
	// NOT mirrored here any more — infantry_weapon_channel re-reads it from the ADM
	// table by the entity's own equipped index every selection pass, exactly as the
	// original does [orig: @ 0x4b5dba], which is the same path a remote player's pose
	// resolves through. Keeping a local-only scalar beside that table read would put two
	// sources behind one value and let the two disagree across a weapon switch.
	p->inf.aimed_shot_available = false;
	if (p->inf.active) {
		if (weapon_active_) {
			opennova::world::infantry_weapon_switch_stamp(
					p->inf, weapon_anim_map_serial_);
		}
		p->inf.scope_raised = scope_promoted;
		p->inf.binoculars_raised = player_view_.binoculars_raised;
		// The run-gait class + ForceCrouch mirror, same per-tick re-read pattern as the
		// hold kind [orig: the selection reads AdmDefs[+0x2B0]+0xAC each pass @ 0x4b72cf;
		// the ForceCrouch checks read the equipped def flags @ 0x4b7245/@ 0x4e0d8a].
		p->inf.wpn_run_anim = weapon_active_ ? weapon_run_anim_ : 0;
		p->inf.wpn_force_crouch = weapon_active_ && weapon_force_crouch_;

		// The body updater and HUD share retail's Player_CanFireWeapon verdict.
		// A promoted Scoped weapon (Flags bit 0) is rejected while moving and
		// submerged. The second predicate, misleadingly named as a vehicle-gunner
		// helper in the IDB, is actually promoted Sighted (bit 1, except SWITCHFROM)
		// with no mount gate; it bypasses those two ordinary checks. Both still lose
		// to InAir. ForceScoped is the final first-person override, but never bypasses
		// the earlier card-switch reload or seat rejection.
		// [orig: @0x5cf7c7..0x5cf886; Scoped helper @0x4dcc80;
		// Sighted helper @0x4dcd30; HUD row select @0x592b87]
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		bool mount_allows_aimed_shot = local != nullptr;
		if (local != nullptr && local->mounted) {
			mount_allows_aimed_shot =
					local->mount_type == opennova::world::SeatType::Passenger ||
					(local->mount_type == opennova::world::SeatType::Gunner &&
							local_usegun_slot_active_);
		}
		const opennova::world::WeaponSlotState &active_slot =
				*active_local_weapon_slot();
		const uint32_t weapon_flags =
				static_cast<uint32_t>(weapon_def_.flags);
		const uint32_t entity_flags = local != nullptr
				? local->flags | local->engine_flags
				: 0;
		const bool card_switch_reload =
				active_slot.current == opennova::world::weapon_action::kReload &&
				(weapon_flags & DEF_WEAPON_FLAG_NOCARDSWITCH) == 0;
		const bool scoped_aimed_shot = scope_promoted &&
				(weapon_flags & DEF_WEAPON_FLAG_SCOPED) != 0;
		const bool sighted_aimed_shot = scope_promoted &&
				(weapon_flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
				active_slot.current != opennova::world::weapon_action::kSwitchFrom;
		const bool in_air = p->inf.airborne ||
				(entity_flags & opennova::world::kEntityFlagInAir) != 0;
		// Retail adds CameraOffset.Z to this test. That offset is not represented,
		// so the fixed body Z plus the Drowning flag is the bounded projection.
		const bool submerged =
				(entity_flags & opennova::world::kEntityFlagDrowning) != 0 ||
				(world_->env.water_z != 0 && p->pos[2] < world_->env.water_z);
		const bool ordinary_aimed_shot = !in_air &&
				(sighted_aimed_shot ||
						(scoped_aimed_shot && !p->inf.player_moving)) &&
				(sighted_aimed_shot || !submerged);
		const bool force_scoped =
				(weapon_flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0;
		p->inf.aimed_shot_available = local != nullptr && local->alive &&
				local->health > 0 && weapon_active_ && mount_allows_aimed_shot &&
				!card_switch_reload && !player_view_.third_person &&
				!player_view_.binoculars_view_active &&
				(force_scoped || ordinary_aimed_shot);
	}
	if (opennova::world::Entity *entity =
				world_->registry.get(world_->cached.local_player)) {
		uint32_t view_flags = 0;
		if (player_view_.nvg_active) view_flags |= 0x4u;
		if (player_view_.binoculars_raised) view_flags |= 0x8u;
		if (scope_promoted) view_flags |= 0x10u;
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
			(weapon_def_.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0) {
		if (opennova::world::player_view_set_engaged(player_view_, false,
				(weapon_def_.flags2 & DEF_WEAPON_FLAG2_INSET) != 0))
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
	// A joiner also SENDS the select — the witnessed key handlers emit one C2S 0x1D
	// with the action id immediately; without it a retail host (and every other
	// client) never sees this player crouch or go prone.
	// [orig: cases 169/170/172 @0x4e0d77/@0x4e0df3/@0x4e0e3e]
	if (joiner_ && runtime_) {
		static constexpr uint16_t kStanceActionIds[3] = {0xAC, 0xA9, 0xAA};
		ship_to_host(runtime_->send_stance_change(
				kStanceActionIds[static_cast<size_t>(p_stance)]));
	}
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
	// infantry motor mirror, infantry.cpp). The avatar is shell-managed and not in the present
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

String NovaSimulation::get_local_player_anim_source_key() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return String();
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p || !p->inf.body_blend_active()) return String();
	return infantry_anim_key(p->inf.anim_prev);
}

int NovaSimulation::get_local_player_anim_source_phase_ticks() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.anim_prev_clip_phase : 0;
}

float NovaSimulation::get_local_player_anim_blend_weight() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 1.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.anim_blend_weight : 1.0f;
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
			(weapon_def_.flags2 & DEF_WEAPON_FLAG2_INSET) != 0)
		return false;
	// ForceScoped pins the raised sight: un-scoping is refused once settled
	// [orig: (flags1 & 0x20000000) == 0 || !g_weaponScopeActive @ 0x4df12d].
	if (player_view_.scope_engaged && (weapon_def_.flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0 &&
			!opennova::world::player_view_scope_ease_active(player_view_))
		return false;
	// The toggle latches this ease's step count (7 for Inset weapons, else 15;
	// 1 on the hipfire-return leg) and REFUSES while the previous ease runs
	// [orig: Player_ToggleWeaponScope @ 0x4df177 !activeFlag; Setup @ 0x4df1b3..0x4df36e].
	if (!opennova::world::player_view_set_engaged(player_view_, !player_view_.scope_engaged,
			(weapon_def_.flags2 & DEF_WEAPON_FLAG2_INSET) != 0))
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
				(weapon_def_.flags2 & DEF_WEAPON_FLAG2_INSET) != 0 &&
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
	// view bias for the frame — the shell reads the eased fraction as 0.
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
	// The first-person camera consumes twice entity+0x380. Export the already
	// wrapped composition separately from authoritative look pitch so the host
	// cannot accidentally apply it to third person or aim rays.
	// [orig: Player_UpdateFirstPersonCamera @0x437fdb]
	{
		float fp_pitch_recoil_deg = 0.0f;
		if (world_ && world_->ai && world_->cached.local_player.valid()) {
			if (const AiEntity *p = world_->ai->for_handle(world_->cached.local_player)) {
				const int32_t doubled =
						opennova::io::bam_dbl(p->inf.recoil_pitch);
				fp_pitch_recoil_deg = static_cast<float>(
						static_cast<double>(doubled) *
						opennova::world::kDegreesPerBam);
			}
		}
		out["fp_pitch_recoil_deg"] = fp_pitch_recoil_deg;
	}
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
