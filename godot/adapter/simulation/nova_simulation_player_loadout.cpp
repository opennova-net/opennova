// NovaSimulation — the LOCAL PLAYER loadout cluster: armory / usegun / mount
// interactions, the loadout (slot pool / spawn kit / map rules), the weapon
// profile, and the weapon/ammo table feeds.
#include "simulation/nova_simulation_internal.h"

#include <npruntime/loadout_submit.h> // the 0x2F submission composition (ADR 0031 PR E)

#include <cstdlib> // the chunk tuples' atol-truncation parse [orig: @ 0x42cf7c]

#include <def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*
#include <npwire/ingame_message_id.h>
#include <simassets/fp_viewmodel_spec.h> // the FP viewmodel submit rule

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
	if (!listen_server_ && !local_weapon_.active) {
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
	if (!world_) return;
	std::vector<opennova::world::WeaponKitEntry> kit;
	for (int i = 0; i < p_kit.size(); ++i) {
		opennova::world::WeaponKitEntry e = kit_entry_from_dict(p_kit[i]);
		if (!e.name.empty()) kit.push_back(std::move(e));
	}
	opennova::world::local_loadout_set_spawn_kit(*world_, local_loadout_,
			std::move(kit), p_filter_by_availability);
	// A promoted kit changes what the joiner's 0x2F pair should carry; re-arm the
	// seam (no-op for hosts and before the weapon catalog exists).
	push_joiner_loadout_kit();
}

void NovaSimulation::set_weapon_availability(const TypedArray<Dictionary> &p_pairs) {
	if (!world_) {
		local_loadout_.availability.reset();
		return;
	}
	std::vector<std::pair<std::string, int32_t>> pairs;
	for (int i = 0; i < p_pairs.size(); ++i) {
		const Dictionary d = p_pairs[i];
		std::string name = dictionary_string(d, "name", std::string());
		if (name.empty()) continue;
		pairs.emplace_back(std::move(name), int32_t(int64_t(d.get("value", 1))));
	}
	opennova::world::local_loadout_apply_availability_pairs(*world_,
			local_loadout_, pairs);
}

int NovaSimulation::get_weapon_availability(const String &p_weapon_name) const {
	if (!world_) return opennova::world::weapon_availability_value::kAllowed;
	const int idx = world_->weapons.index_of(p_weapon_name.utf8().get_data());
	if (idx < 0) return opennova::world::weapon_availability_value::kAllowed;
	return local_loadout_.availability.value_for(idx);
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
		local_loadout_.pending_player_class = p_player_class;
		push_joiner_loadout_kit();
		return true;
	}
	e->player_class = static_cast<uint8_t>(p_player_class);
	push_joiner_loadout_kit();
	return true;
}

// Called by load_from_mission_data's finish (nova_simulation.cpp): stash the
// mission's loadout/availability chunks in plain world types — the promotion
// runs at load_weapon_table time through the witnessed SP-vs-net gate, when
// the catalog can resolve names (retail's own order: Game_StartMission parses
// weapon.def @ 0x5254b3 before Mission_LoadBMSFile reads the chunks). The
// string tuples convert with retail's own atol truncation semantics.
// [orig: Mission_LoadBMSFile @ 0x40F4E0; the tuple parse over restrictionData]
void NovaSimulation::stash_mission_loadout_rules(
		const opennova::bms::File &p_file) {
	mission_availability_rows_.clear();
	mission_kit_rows_.clear();
	for (const opennova::bms::ItemAvailabilityEntry &row :
			p_file.item_availability) {
		if (row.name.empty()) continue;
		mission_availability_rows_.emplace_back(row.name,
				static_cast<int32_t>(row.status));
	}
	for (const opennova::bms::WeaponLoadoutRecord &row : p_file.loadout.entries) {
		if (row.name.empty()) continue;
		opennova::world::WeaponKitEntry entry;
		entry.name = row.name;
		entry.ammo_primary = static_cast<int32_t>(
				std::strtol(row.ammo_primary.c_str(), nullptr, 10));
		entry.ammo_secondary = static_cast<int32_t>(
				std::strtol(row.ammo_secondary.c_str(), nullptr, 10));
		entry.flags = static_cast<int32_t>(
				std::strtol(row.flags.c_str(), nullptr, 10));
		mission_kit_rows_.push_back(std::move(entry));
	}
}

bool NovaSimulation::apply_local_player_loadout(const TypedArray<Dictionary> &p_kit,
                                                int p_player_class) {
	return apply_local_player_loadout_impl(
			p_kit, p_player_class, /*p_submit_joiner_request=*/true);
}

bool NovaSimulation::apply_local_player_loadout_impl(
		const TypedArray<Dictionary> &p_kit, int p_player_class,
		bool p_submit_joiner_request) {
	// The armory ACCEPT apply — world/player_loadout.h local_loadout_apply_accept
	// [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline leg]. A joiner
	// applies its kit before L exists (the shell runs the spawn-loadout apply
	// right after runtime setup; L spawns later, on the name-match): the
	// inventory is sim-side state, the entity stamps defer to the joiner spawn
	// block. Dropping the kit here left the joiner unable to fire, reload, or
	// switch (the two-GUI regression).
	if (!world_) return false;
	std::vector<opennova::world::WeaponKitEntry> kit;
	for (int i = 0; i < p_kit.size(); ++i) {
		opennova::world::WeaponKitEntry entry = kit_entry_from_dict(p_kit[i]);
		if (!entry.name.empty()) kit.push_back(std::move(entry));
	}
	if (!opennova::world::local_loadout_apply_accept(*world_, local_loadout_,
				local_weapon_, local_inventory_, local_inventory_valid_, kit,
				p_player_class, /*validate_banned=*/p_submit_joiner_request))
		return false;
	// Always re-arm the 0x2F seam after a rebuild settles the equipped combo —
	// the S2C 0x5A grant apply (p_submit=false) refreshes only the live
	// equipped slot on the seam, exactly retail's second-submit content
	// [orig: Game_StartMission @0x525c2e].
	push_joiner_loadout_kit();
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
	opennova::world::local_loadout_sync_damage_classes(*world_, local_loadout_);
}

void NovaSimulation::rebuild_local_player_loadout(bool p_select_spawn_default) {
	// The Player_InitPlayer weapon leg — world/player_loadout.h
	// local_loadout_rebuild [orig: @ 0x4e15f0]. The 0x2F pair's SECOND submit
	// carries the LIVE equipped slot, not the fixed 195 [orig:
	// Game_StartMission @0x525c2e passes g_currentWeaponSlot — the value the
	// spawn fill just settled], so the seam re-arms here AFTER the inventory
	// settles. push_joiner_loadout_kit never rebuilds (it holds a one-way
	// re-entry latch), so this cannot recurse.
	if (!world_) return;
	opennova::world::local_loadout_rebuild(*world_, local_loadout_,
			local_weapon_, local_inventory_, local_inventory_valid_,
			p_select_spawn_default);
	push_joiner_loadout_kit();
}

bool NovaSimulation::seed_session_kit_from_profile() {
	// The composition is engine code now (np::seed_session_kit_from_profile,
	// ADR 0031 PR E — the Game_StartMission copy semantics live there). This
	// binding keeps the ROLE gate: a live session covers a LISTEN HOST as
	// well as a joiner, so single player and the editor keep the mission's
	// .bms kit.
	if (!world_) return false;
	if (!host_listen_ && !joiner_) return false;
	const uint8_t assigned =
			(joiner_ && runtime_) ? runtime_->assigned_team() : 0;
	return opennova::np::seed_session_kit_from_profile(*world_,
			weapon_profile_, assigned, local_loadout_,
			weapon_profile_seeded_side_);
}

bool NovaSimulation::reseed_session_kit_on_side_change() {
	if (!joiner_ && !host_listen_) return false;
	if (!world_) return false;
	const uint8_t assigned =
			(joiner_ && runtime_) ? runtime_->assigned_team() : 0;
	if (!opennova::np::reseed_session_kit_on_side_change(*world_,
			weapon_profile_, assigned, local_loadout_,
			weapon_profile_seeded_side_))
		return false;
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	return true;
}

void NovaSimulation::push_joiner_loadout_kit() {
	// The C2S 0x2F submission content builder is engine code now
	// (np::build_joiner_loadout_kit, ADR 0031 PR E — the one-class-integer
	// rule, the resident-buffer rows, and both side blocks live there, with
	// their witnesses). This binding keeps the seam wiring: the role gate,
	// the empty-catalog arm delay, the re-entry latch, and the pump handoff.
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
	opennova::np::build_joiner_loadout_kit(*world_, weapon_profile_,
			runtime_->assigned_team(), local_loadout_,
			local_inventory_valid_ ? local_inventory_.equipped_combo : -1,
			wire_kit);
	runtime_->set_loadout_kit(std::move(wire_kit));
	pushing_joiner_loadout_kit_ = false;
}

String NovaSimulation::weapon_profile_relpath(const String &p_expansion_name) {
	return String(opennova::playersav::weapon_sav_relpath(
			std::string(p_expansion_name.utf8().get_data()))
					.c_str());
}

Dictionary NovaSimulation::fp_viewmodel_spec(bool p_has_def, const String &p_gfx1,
		const String &p_gfx1a, const String &p_animadm, int p_flags) {
	const opennova::simassets::FpViewmodelSpec spec =
			opennova::simassets::fp_viewmodel_spec(p_has_def,
					std::string(p_gfx1.utf8().get_data()),
					std::string(p_gfx1a.utf8().get_data()),
					std::string(p_animadm.utf8().get_data()),
					static_cast<uint32_t>(p_flags));
	Dictionary out;
	out["gun"] = String(spec.gun.c_str());
	out["arms"] = String(spec.arms.c_str());
	out["adm"] = String(spec.adm.c_str());
	out["show_arms"] = spec.show_arms;
	return out;
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
			local_loadout_.spawn_kit_set ? local_loadout_.spawn_kit : opennova::world::weapon_kit_default();
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
	// The mission's stashed loadout/availability chunks promote NOW, through
	// the witnessed SP-vs-net gate inside the engine (world/player_loadout.h
	// local_loadout_promote_mission_rules): the catalog can resolve names —
	// retail's own effective order, Game_StartMission parses weapon.def
	// @ 0x5254b3 before Mission_LoadBMSFile reads the chunks. In a live
	// session the gate skips both chunks (listen host and joiner alike) and
	// the profile page below is the kit source instead. A promoted kit resets
	// the view effects like the respawn it implies.
	// [orig: Mission_LoadBMSFile @ 0x40F4E0 — gate @ 0x40f694]
	if (opennova::world::local_loadout_promote_mission_rules(*world_,
				local_loadout_, mission_availability_rows_,
				mission_kit_rows_)) {
		reset_local_player_view_effects();
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
