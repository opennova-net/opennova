// Simulation — the LOCAL PLAYER loadout cluster: armory / usegun / mount
// interactions, the loadout (slot pool / spawn kit / map rules), the weapon
// profile, and the weapon/ammo table feeds.
#include "simulation/nova_simulation_internal.h"

#include <npruntime/loadout_submit.h> // the 0x2F submission + 0x5A grant conversions

#include <def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*
#include <mission/promote.h> // stash_mission_loadout_rules (the chunk-tuple conversion)
#include <npwire/ingame_message_id.h>
#include <simassets/fp_viewmodel_spec.h> // the FP viewmodel submit rule
#include <world/friendly_tags.h> // the D-HUD-20 tag gather

#include <cstdio>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp> // weapon.sav lives on the filesystem, not a mount
#include <godot_cpp/classes/os.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace novasim;

namespace {

Dictionary weapon_profile_side_summary(const opennova::playersav::Side &side,
		bool include_kit) {
	Dictionary out;
	out["player_class"] = int(side.player_class);
	out["avatar_a"] = int(side.avatar_a);
	out["avatar_b"] = int(side.avatar_b);
	out["avatar_packed"] = int(side.avatar_packed);
	if (include_kit) {
		Array names;
		if (const opennova::playersav::KitPage *page = side.selected_page()) {
			for (const opennova::playersav::KitEntry &entry : page->entries)
				names.push_back(String::utf8(entry.name.c_str()));
		}
		out["kit"] = names;
	}
	return out;
}

Error read_weapon_profile_file(const String &path,
		opennova::playersav::File &out, bool p_clamp_classes = false) {
	if (path.is_empty()) return ERR_INVALID_PARAMETER;
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) return FileAccess::get_open_error();
	const PackedByteArray bytes =
			file->get_buffer(static_cast<int64_t>(file->get_length()));
	file->close();
	if (!opennova::playersav::read(bytes.ptr(),
			static_cast<std::size_t>(bytes.size()), out))
		return ERR_FILE_CORRUPT;
	if (p_clamp_classes)
		opennova::playersav::clamp_classes(out);
	return OK;
}

Error replace_file_atomic(const String &temp_path, const String &target_path) {
#ifdef _WIN32
	const CharWideString temp = temp_path.wide_string();
	const CharWideString target = target_path.wide_string();
	// MoveFileExW with REPLACE_EXISTING is the Windows atomic same-volume rename
	// primitive; WRITE_THROUGH keeps ACCEPT from returning before metadata lands.
	if (::MoveFileExW(temp.get_data(), target.get_data(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
		return ERR_FILE_CANT_WRITE;
#else
	const CharString temp = temp_path.utf8();
	const CharString target = target_path.utf8();
	if (std::rename(temp.get_data(), target.get_data()) != 0)
		return ERR_FILE_CANT_WRITE;
#endif
	return OK;
}

Error write_weapon_profile_atomic(const String &path,
		const opennova::playersav::File &profile) {
	if (path.is_empty()) return ERR_INVALID_PARAMETER;
	const String base_dir = path.get_base_dir();
	if (!base_dir.is_empty()) {
		const Error dir_error = DirAccess::make_dir_recursive_absolute(base_dir);
		if (dir_error != OK) return dir_error;
	}
	const String temp_path = vformat("%s.tmp.%d", path,
			OS::get_singleton()->get_process_id());
	Ref<FileAccess> file = FileAccess::open(temp_path, FileAccess::WRITE);
	if (file.is_null()) return FileAccess::get_open_error();
	const std::vector<uint8_t> encoded = opennova::playersav::write(profile);
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(encoded.size()));
	if (!encoded.empty())
		std::memcpy(bytes.ptrw(), encoded.data(), encoded.size());
	file->store_buffer(bytes);
	file->flush();
	const Error write_error = file->get_error();
	file->close();
	if (write_error != OK) {
		DirAccess::remove_absolute(temp_path);
		return write_error;
	}
	const Error rename_error = replace_file_atomic(temp_path, path);
	if (rename_error != OK)
		DirAccess::remove_absolute(temp_path);
	return rename_error;
}

bool avatar_selection_from_dictionary(const Dictionary &profile, int side,
		uint8_t &avatar_a, uint8_t &avatar_b, uint16_t &avatar_packed) {
	if (profile.is_empty() || !profile.has("avatar_a") ||
			!profile.has("avatar_b") || !profile.has("avatar_packed"))
		return false;
	const int a = profile.get("avatar_a", -1);
	const int b = profile.get("avatar_b", -1);
	const int packed = profile.get("avatar_packed", -1);
	if (a < 0 || a > 0xff || b < 0 || b > 0xff ||
			packed < 0 || packed > 0xffff ||
			((packed >> 15) & 1) != side)
		return false;
	avatar_a = static_cast<uint8_t>(a);
	avatar_b = static_cast<uint8_t>(b);
	avatar_packed = static_cast<uint16_t>(packed);
	return true;
}

} // namespace

bool Simulation::local_player_in_armory_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	// The use-item armory leg rejects a seated player before consulting the type-6
	// volume bit [orig: Input_HandleActionBinding_0 @0x4e0b3f, parentSlot == 0].
	return e != nullptr && !e->mounted &&
	       (e->flags & opennova::world::kEntityFlagArmoryZone) != 0;
}

bool Simulation::local_player_in_vehicle_loadout_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr &&
	       (e->flags & opennova::world::kEntityFlagVehicleLoadoutZone) != 0;
}

bool Simulation::local_player_toggle_mount() {
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
	// The weapon-busy gate + candidate search live engine-side
	// (world/vehicle_attach.h). A missing EquippedSlot passes the retail gate;
	// active_local_weapon_slot supplies the inert slot state.
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	if (!opennova::world::weapon_state_allows_mount_toggle(
				active_slot->current, active_slot->next))
		return false;
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
		opennova::world::VehicleSeatSelection hit;
		if (!opennova::world::find_mount_toggle_candidate(
					*world_, *toggle_player, hit))
			return false;
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
		opennova::world::VehicleSeatSelection hit;
		if (opennova::world::find_mount_toggle_candidate(
					*world_, *toggle_player, hit) &&
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

TypedArray<Dictionary> Simulation::get_attach_labels() const {
	TypedArray<Dictionary> out;
	if (!world_) return out;
	const opennova::world::Entity *player = world_->registry.get(world_->cached.local_player);
	if (player == nullptr || !player->alive || player->health <= 0) return out;
	// Armory mode = standing in the type-6 armory volume; the label pass reads the raw
	// flag [orig: is_armory_mode = entity Flags & 0x400000 @0x5a32c4].
	const bool armory_mode =
	    (player->flags & opennova::world::kEntityFlagArmoryZone) != 0;
	// The nearest-only gate [orig: Player_CanFireWeapon @0x5cf780 — EquippedSlot present
	// plus the live mount, camera, scope, movement, air, and water gates. The
	// query computes it directly so camera changes cannot lag one logic tick.
	const AiEntity *body = world_->ai != nullptr
			? world_->ai->for_handle(world_->cached.local_player)
			: nullptr;
	const bool can_fire = local_player_can_fire_weapon(body);
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

TypedArray<Dictionary> Simulation::get_friendly_tags() const {
	// The friendly-tags gather (D-HUD-20): raw positions + per-entity facts; the
	// presenter lifts, projects, and feeds the HUD compiler's element. The
	// witnessed pass is cited at the engine gather (world/friendly_tags.cpp).
	TypedArray<Dictionary> out;
	if (!world_) return out;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player == nullptr) return out;
	std::vector<opennova::world::FriendlyTagSource> tags;
	opennova::world::collect_friendly_tags(*world_, *player, tags);
	for (const opennova::world::FriendlyTagSource &t : tags) {
		Dictionary d;
		d["position"] = Vector3(t.position.x, t.position.y, t.position.z);
		// The eye height above the entity origin in mission units (16.16 ->
		// float); the anchor witness lives at the gather (friendly_tags.h).
		d["eye_height"] = static_cast<float>(t.eye_offset_z) / 65536.0f;
		d["name"] = String::utf8(t.name.c_str());
		d["entity_id"] = static_cast<int>(t.net_id);
		d["health_ratio_fp16"] = t.health_ratio_fp16;
		d["player"] = t.player;
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

void Simulation::set_spawn_loadout(const TypedArray<Dictionary> &p_kit,
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

void Simulation::set_weapon_availability(const TypedArray<Dictionary> &p_pairs) {
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

int Simulation::get_weapon_availability(const String &p_weapon_name) const {
	if (!world_) return opennova::world::weapon_availability_value::kAllowed;
	const int idx = world_->weapons.index_of(p_weapon_name.utf8().get_data());
	if (idx < 0) return opennova::world::weapon_availability_value::kAllowed;
	return local_loadout_.availability.value_for(idx);
}

bool Simulation::set_local_player_class(int p_player_class) {
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
// tuple conversion (retail's atol truncation) is mission::stash_mission_loadout_rules's.
void Simulation::stash_mission_loadout_rules(
		const opennova::bms::File &p_file) {
	opennova::mission::stash_mission_loadout_rules(p_file,
			mission_availability_rows_, mission_kit_rows_);
}

bool Simulation::apply_local_player_loadout(const TypedArray<Dictionary> &p_kit,
                                                int p_player_class) {
	return apply_local_player_loadout_impl(
			p_kit, p_player_class, /*p_submit_joiner_request=*/true);
}

bool Simulation::apply_local_player_loadout_impl(
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
	return apply_local_player_loadout_rows(std::move(kit), p_player_class,
			p_submit_joiner_request);
}

bool Simulation::apply_local_player_loadout_rows(
		std::vector<opennova::world::WeaponKitEntry> p_kit, int p_player_class,
		bool p_submit_joiner_request) {
	if (!world_) return false;
	if (!opennova::world::local_loadout_apply_accept(*world_, local_loadout_,
				local_weapon_, local_inventory_, local_inventory_valid_, p_kit,
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

void Simulation::respawn_local_player_loadout() {
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// Player_InitPlayer clears both view effects and seeds NVG from the mission
	// StartWithNVGOn bit on every respawn.
	reset_local_player_view_effects();
	// The respawn edge also returns the map state to its spawn defaults
	// (witness at hud::HudMapControl — Game_InitRespawnState +
	// Player_InitPlayer zoom seeds).
	hud_map_control_.reset_spawn();
}

void Simulation::sync_local_player_damage_classes() {
	if (!world_) return;
	opennova::world::local_loadout_sync_damage_classes(*world_, local_loadout_);
}

void Simulation::rebuild_local_player_loadout(bool p_select_spawn_default) {
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

bool Simulation::seed_session_kit_from_profile() {
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

bool Simulation::reseed_session_kit_on_side_change() {
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

void Simulation::push_joiner_loadout_kit() {
	// The C2S 0x2F submission content builder is engine code now
	// (np::build_joiner_loadout_kit, ADR 0031 PR E — the one-class-integer
	// rule, the resident-buffer rows, and both side blocks live there, with
	// their witnesses). This binding keeps the seam wiring: the role gate,
	// the empty-catalog arm delay, the re-entry latch, and the pump handoff.
	if (!joiner_ || !runtime_ || !world_) return;
	// finish_load runs before the shell loads weapon.def (MissionPresentation orders
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
			local_inventory_valid_ ? &local_inventory_ : nullptr,
			wire_kit);
	runtime_->set_loadout_kit(std::move(wire_kit));
	pushing_joiner_loadout_kit_ = false;
}

String Simulation::weapon_profile_relpath(const String &p_expansion_name) {
	return String(opennova::playersav::weapon_sav_relpath(
			std::string(p_expansion_name.utf8().get_data()))
					.c_str());
}

Dictionary Simulation::read_weapon_profile_summary(const String &p_path) {
	opennova::playersav::File profile = opennova::playersav::make_defaults();
	// Raw bytes for the menu: the PLAYER_INFO screen selects its rows straight
	// from the profile globals; the [5,9] class clamp is a SESSION-START step
	// (apply_session_settings_to_globals) that load_weapon_profile applies.
	const Error error = read_weapon_profile_file(p_path, profile);
	Dictionary out;
	out["error"] = int(error);
	out["loaded"] = error == OK;
	// OpenNova's active profile is slot 0. Retail indexes the same five-record
	// array by g_curProfileSlot @0x25506B8 (0x1080C stride) before reading or
	// writing its record; see docs/playerinfo/avatars-re.md.
	out["blue"] = weapon_profile_side_summary(profile.slots[0].blue, false);
	out["red"] = weapon_profile_side_summary(profile.slots[0].red, false);
	return out;
}

Error Simulation::save_weapon_profile_selection(const String &p_path,
		const Dictionary &p_profile) {
	if (p_path.is_empty() || p_profile.is_empty())
		return ERR_INVALID_PARAMETER;

	// The ACCEPT snapshot (PlayerCharacterSelectionState.snapshot): the shared
	// PLAYERCLASS value plus side_profiles[blue, red], each carrying the side's
	// authored nationality/division ids and packed character id.
	const int player_class = p_profile.get("player_class", -1);
	if (player_class < opennova::playersav::kMinPlayerClass ||
			player_class > opennova::playersav::kMaxPlayerClass)
		return ERR_INVALID_PARAMETER;
	const Array side_profiles = p_profile.get("side_profiles", Array());

	opennova::playersav::File profile;
	if (FileAccess::file_exists(p_path)) {
		const Error read_error = read_weapon_profile_file(p_path, profile);
		// Never replace an unrecognized/short existing file. ACCEPT must be
		// recoverable even when the user's profile needs manual repair.
		if (read_error != OK) return read_error;
	} else {
		profile = opennova::playersav::make_defaults();
	}

	bool updated = false;
	for (int side = 0; side < 2; ++side) {
		Dictionary selected;
		if (side < side_profiles.size() &&
				side_profiles[side].get_type() == Variant::DICTIONARY)
			selected = side_profiles[side];
		if (selected.is_empty()) continue;
		uint8_t avatar_a = 0;
		uint8_t avatar_b = 0;
		uint16_t avatar_packed = 0;
		if (!avatar_selection_from_dictionary(selected, side,
				avatar_a, avatar_b, avatar_packed))
			return ERR_INVALID_PARAMETER;
		opennova::playersav::update_avatar_selection(profile, 0,
				side == 0 ? opennova::playersav::SideId::Blue
				          : opennova::playersav::SideId::Red,
				static_cast<uint8_t>(player_class), avatar_a, avatar_b,
				avatar_packed);
		updated = true;
	}
	if (!updated) return ERR_INVALID_PARAMETER;

	// `write()` recreates every modeled record, while the temp + same-volume
	// replace keeps the previous file intact until the new one is complete.
	return write_weapon_profile_atomic(p_path, profile);
}

double Simulation::weapon_def_pos_scale() {
	return opennova::simassets::kWeaponDefPosScale;
}

Vector3 Simulation::viewmodel_fallback_pos_units() {
	const float *v = opennova::simassets::kFallbackPosUnits;
	return Vector3(v[0], v[1], v[2]);
}

Vector3 Simulation::viewmodel_fallback_tpos_units() {
	const float *v = opennova::simassets::kFallbackTposUnits;
	return Vector3(v[0], v[1], v[2]);
}

Vector3 Simulation::viewmodel_fallback_rot_bias_deg() {
	const float *v = opennova::simassets::kFallbackRotBiasDeg;
	return Vector3(v[0], v[1], v[2]);
}

double Simulation::viewmodel_pass_near_z() {
	return opennova::simassets::kViewmodelPassNearZ;
}

String Simulation::viewmodel_bringup_fallback_weapon() {
	return String(opennova::simassets::kBringupFallbackWeapon);
}

Dictionary Simulation::fp_viewmodel_spec(bool p_has_def, const String &p_gfx1,
		const String &p_character_arms, const String &p_animadm, int p_flags) {
	const opennova::simassets::FpViewmodelSpec spec =
			opennova::simassets::fp_viewmodel_spec(p_has_def,
					std::string(p_gfx1.utf8().get_data()),
					std::string(p_character_arms.utf8().get_data()),
					std::string(p_animadm.utf8().get_data()),
					static_cast<uint32_t>(p_flags));
	Dictionary out;
	out["gun"] = String(spec.gun.c_str());
	out["arms"] = String(spec.arms.c_str());
	out["adm"] = String(spec.adm.c_str());
	out["show_arms"] = spec.show_arms;
	return out;
}

Error Simulation::load_weapon_profile(const String &p_path) {
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

Dictionary Simulation::get_weapon_profile_summary() const {
	Dictionary out;
	out["loaded"] = weapon_profile_loaded_;
	out["blue"] = weapon_profile_side_summary(weapon_profile_.blue, true);
	out["red"] = weapon_profile_side_summary(weapon_profile_.red, true);
	return out;
}

void Simulation::apply_joiner_authoritative_loadout() {
	if (!joiner_ || !runtime_ || !world_ || world_->weapons.empty()) return;
	const uint64_t revision = runtime_->authoritative_loadout_revision();
	if (revision == 0 || revision <= joiner_applied_loadout_revision_) return;

	// The grant -> kit-row conversion (name resolve + SIGNED clip reinterpret)
	// is np::kit_from_authoritative_grant's.
	const opennova::WeaponLoadout &grant = runtime_->authoritative_loadout();
	std::vector<opennova::world::WeaponKitEntry> kit;
	opennova::np::kit_from_authoritative_grant(world_->weapons, grant, kit);
	// Do not echo an authoritative grant back as a new C2S 0x2F request. The
	// S2C handler rebuilds the slots directly at recv-before-actions. The rebuild
	// inside still re-arms the seam, but its ROWS come from the profile page, never
	// from the grant — only the live equipped slot refreshes, which is exactly what
	// retail's second submit carries [orig: Game_StartMission @0x525c2e].
	if (apply_local_player_loadout_rows(
				std::move(kit), grant.avatar_class, /*p_submit_joiner_request=*/false))
		joiner_applied_loadout_revision_ = revision;
}

Dictionary Simulation::get_local_player_inventory() const {
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

TypedArray<Dictionary> Simulation::get_local_player_loadout() const {
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
Error Simulation::load_weapon_table(const Ref<ResourceRoot> &p_resource_root,
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
	world_->weapons = opennova::np::build_weapon_table(
			file, &p_resource_root->native_index());
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
			    e.equipped_adm_index == opennova::world::kAdmSlotNone)
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
Error Simulation::load_ammo_table(const Ref<ResourceRoot> &p_resource_root,
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
