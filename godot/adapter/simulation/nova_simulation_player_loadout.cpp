// NovaSimulation — the LOCAL PLAYER loadout cluster: armory / usegun / mount
// interactions, the loadout (slot pool / spawn kit / map rules), the weapon
// profile, and the weapon/ammo table feeds.
#include "simulation/nova_simulation_internal.h"

#include <def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*
#include <npwire/ingame_message_id.h>

#include <godot_cpp/classes/file_access.hpp> // weapon.sav lives on the filesystem, not a mount

using namespace novasim;

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
		if (mount != nullptr) {
			if (opennova::world::WeaponSlotState *slot =
					opennova::world::resolve_mounted_ammo_slot(
							*world_, *mount))
				return slot;
		}
	}
	return &weapon_slot_;
}

const opennova::world::WeaponSlotState *
NovaSimulation::active_local_weapon_slot() const {
	if (local_usegun_slot_active_ && world_ && local_usegun_mount_.valid()) {
		const opennova::world::Entity *mount =
				world_->registry.get(local_usegun_mount_);
		if (mount != nullptr) {
			if (const opennova::world::WeaponSlotState *slot =
					opennova::world::resolve_mounted_ammo_slot(
							*world_, *mount))
				return slot;
		}
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
		opennova::world::WeaponSlotState *resolved_slot = mount != nullptr
				? opennova::world::resolve_mounted_ammo_slot(*world_, *mount)
				: nullptr;
		uint8_t resolved_adm = mount != nullptr
				? mount->primary_weapon_slot_adm : 0xFF;
		if (mount != nullptr && resolved_slot != nullptr &&
				resolved_slot != &mount->primary_weapon_slot) {
			opennova::world::Entity *carrier =
					world_->registry.get(mount->ground_target);
			if (carrier == nullptr ||
					resolved_slot != &carrier->primary_weapon_slot)
				resolved_slot = nullptr;
			else
				resolved_adm = carrier->primary_weapon_slot_adm;
		}
		if (resolved_slot == nullptr ||
				resolved_adm != local_usegun_pending_weapon_adm_) {
			select_parent = false;
		} else {
			local_usegun_slot_active_ = true;
			local_usegun_mount_ = local_usegun_pending_mount_;
			local_usegun_weapon_adm_ =
					local_usegun_pending_weapon_adm_;
			next_adm = local_usegun_weapon_adm_;
			next_slot = resolved_slot;
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
	opennova::world::WeaponSlotState *mounted_slot = mounted_parent != nullptr
			? opennova::world::resolve_mounted_ammo_slot(
					*world_, *mounted_parent)
			: nullptr;
	uint8_t mounted_adm = mounted_parent != nullptr
			? mounted_parent->primary_weapon_slot_adm : 0xFF;
	if (mounted_parent != nullptr && mounted_slot != nullptr &&
			mounted_slot != &mounted_parent->primary_weapon_slot) {
		opennova::world::Entity *carrier =
				world_->registry.get(mounted_parent->ground_target);
		if (carrier == nullptr ||
				mounted_slot != &carrier->primary_weapon_slot)
			mounted_slot = nullptr;
		else
			mounted_adm = carrier->primary_weapon_slot_adm;
	}
	const bool on_usegun = mounted_parent != nullptr &&
			player->use_gun_slot_swapped && mounted_slot != nullptr &&
			mounted_adm != 0xFF;
	const auto same_category = [&](uint8_t p_from, uint8_t p_to) {
		const opennova::world::WeaponTableEntry *from =
				world_->weapons.by_index(p_from);
		const opennova::world::WeaponTableEntry *to =
				world_->weapons.by_index(p_to);
		return from != nullptr && to != nullptr &&
				from->category == to->category;
	};
	const auto stage_parent = [&](opennova::world::Entity &p_mount,
			uint8_t target_adm) {
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
				local_usegun_weapon_adm_ == mounted_adm;
		const bool pending_matches =
				(local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
				 local_usegun_switch_ == LocalUseGunSwitch::kSwap) &&
				local_usegun_pending_mount_ == mounted_parent->handle &&
				local_usegun_pending_weapon_adm_ == mounted_adm;
		if (local_usegun_switch_ == LocalUseGunSwitch::kNone) {
			if (!active_matches) stage_parent(*mounted_parent, mounted_adm);
		} else if (!pending_matches) {
			// A later attach overwrites g_pendingWeaponSlot without changing the
			// outgoing slot. This includes direct old-gun -> new-gun swaps.
			stage_parent(*mounted_parent, mounted_adm);
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
	// Retain the parse (S6b): the by-name FSM install reads its full rows —
	// the ACCEPT chain rebuilds the slot table with no shell dictionary and no
	// render dependency [orig: WeaponSlotTable_LoadAllFromDefs @ 0x5414e0].
	if (weapon_defs_loaded_) def_free_weapons(&weapon_defs_);
	weapon_defs_ = file;
	weapon_defs_loaded_ = true;
	// The seat table may have been installed before this feed (either install
	// order is production-legal); refresh its turret clamp windows now.
	stamp_seat_spec_turret_limits();

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
