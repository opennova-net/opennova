// Simulation — the LOCAL PLAYER loadout cluster: armory / usegun / mount
// interactions, the loadout (slot pool / spawn kit / map rules), the weapon
// profile, and the weapon/ammo table feeds.
#include "simulation/simulation_internal.h"
#include "simulation/fp_viewmodel_spec.h"
#include "simulation/player_inventory.h"
#include "simulation/weapon_kit_entry.h"
#include "simulation/weapon_profile_summary.h"

#include <runtime/inmatch/loadout_submit.h> // the 0x2F submission + 0x5A grant conversions

#include <formats/def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*
#include <runtime/mission/promote.h> // stash_mission_loadout_rules (the chunk-tuple conversion)
#include <net/npwire/ingame_message_id.h>
#include <runtime/simassets/fp_viewmodel_spec.h> // the FP viewmodel submit rule
#include <runtime/replication/client_roster_tags.h> // the joiner's player walk of the tag pass
#include <runtime/world/friendly_tags.h> // the D-HUD-20 tag gather

#include <algorithm>
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

using namespace sim_internal;

namespace {

// The typed kit rows a GDScript producer hands the sim (null / nameless rows drop).
std::vector<opennova::world::WeaponKitEntry> kit_rows_from(const TypedArray<WeaponKitEntry> &p_kit) {
	std::vector<opennova::world::WeaponKitEntry> kit;
	for (int i = 0; i < p_kit.size(); ++i) {
		const Ref<WeaponKitEntry> row = p_kit[i];
		if (row.is_null() || row->value().name.empty()) continue;
		kit.push_back(row->value());
	}
	return kit;
}

} // namespace

namespace {

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
	if (!kernel_) return false;
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	// The use-item armory leg rejects a seated player before consulting the type-6
	// volume bit [orig: Input_HandleActionBinding_0 @0x4e0b3f, parentSlot == 0].
	return e != nullptr && !e->mounted &&
	       (e->flags & opennova::world::kEntityFlagArmoryZone) != 0;
}

bool Simulation::local_player_toggle_mount() {
	// The USE-ITEM mount toggle for the local player — the shell calls this when the
	// armory/vehicle-zone legs of the key don't apply. [orig: Input_ProcessFrame release
	// edge @0x49d6dc -> Entity_ToggleVehicleMount @0x436950]
	if (!kernel_) return false;
	// The witnessed non-authority path queues C2S 0x26 (attach) /
	// sends 0x27 (detach) and waits for the 0x0A stream to confirm [orig:
	// Entity_RequestVehicleAttach @0x4364a0 / Entity_SendDetachPacket @0x435510].
	// Joiners therefore choose a candidate locally but never mutate L until the
	// authoritative relationship echo arrives.
	// A missing EquippedSlot passes the retail action gate; active_local_weapon_slot
	// supplies the inert slot state used by the modeled gate below.
	const opennova::world::Entity *toggle_player =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
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
	if (is_joiner())
		return joiner_role_ && joiner_role_->queue_mount_toggle();
	// The authority's toggle is the kernel's one body (the out-of-session
	// UseGun rejection, the seat transition, the view/weapon folds).
	const bool changed = kernel_->local.toggle_mount();
	if (changed) refresh_local_player_view_effects();
	return changed;
}

bool Simulation::fill_attach_labels(std::vector<opennova::world::AttachLabel> &r_labels) const {
	r_labels.clear();
	if (!kernel_) return false;
	kernel_->local.collect_attach_labels(r_labels, is_joiner() ? joiner_role_ : nullptr);
	return true;
}

bool Simulation::fill_friendly_tags(std::vector<opennova::world::FriendlyTagSource> &r_tags) const {
	// The friendly-tags gather (D-HUD-20): raw positions + per-entity facts; the
	// overlay lifts, projects, and feeds the HUD compiler's element. The
	// witnessed pass is cited at the engine gather (world/friendly_tags.cpp).
	r_tags.clear();
	if (!kernel_) return false;
	const opennova::world::Entity *player =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (player == nullptr) return false;
	std::vector<opennova::world::FriendlyTagSource> &tags = r_tags;
	// The pass-level facts (retail g_death_screen_active / g_GameType): the
	// death screen bit is the client's local latch, the game type every role's
	// view carries.
	opennova::world::FriendlyTagPassContext ctx;
	ctx.death_screen = local_death_screen_active();
	ctx.game_type = runtime_ ? runtime_->game_type() : 0;
	// The player walk's slot owner. On the authority the connection table IS
	// the player-slot table: each link's owned entity, revive window, and
	// medic-request latch (retail's PlayerSlot +0x24/+0x10/+0x2C).
	const opennova::world::PlayerSlotLookup authority_slot_lookup =
			[this](opennova::world::EntityHandle entity,
					opennova::world::PlayerSlotFacts &facts) {
				const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
				if (ctx == nullptr) return false;
				for (const opennova::inmatch::NapiNPConnection &conn :
						ctx->np_protocol.connection_list) {
					if (conn.link.owned_entity != entity) continue;
					facts.revive_seconds = static_cast<uint8_t>(
							std::min<uint32_t>(conn.link.downed_revive_seconds, 0xFFu));
					facts.medic_request = conn.link.medic_request_active;
					return true;
				}
				return false;
			};
	if (!is_joiner()) ctx.slot_lookup = &authority_slot_lookup;
	opennova::world::collect_friendly_tags(kernel_->world, *player, tags, ctx);
	if (is_joiner() && runtime_) {
		// A joiner's players are decoded rows, not World twins: the roster walk
		// over ClientState supplies them (netsim/client_roster_tags.h).
		const int32_t player_hp = kernel_->world.tables.player.item_hp;
		opennova::replication::collect_roster_tags(runtime_->state(),
				runtime_->has_self_handle() ? runtime_->self_handle() : 0xFFFFu,
				runtime_->assigned_team(), ctx.death_screen, ctx.game_type, tags,
				[player_hp](uint16_t) { return player_hp; });
	}
	return true;
}

// --- the local player's loadout: slot pool, spawn kit, map rules -----------------------
// (the 2026-07-18 loadout grill; witness map in docs/net/novaworld-net-re.md §5.57)

void Simulation::set_spawn_loadout(const TypedArray<WeaponKitEntry> &p_kit,
                                       bool p_filter_by_availability) {
	if (!kernel_) return;
	std::vector<opennova::world::WeaponKitEntry> kit = kit_rows_from(p_kit);
	opennova::world::local_loadout_set_spawn_kit(kernel_->world, kernel_->local.loadout,
			std::move(kit), p_filter_by_availability);
	// A promoted kit changes what the joiner's 0x2F pair should carry; re-arm the
	// seam (no-op for hosts and before the weapon catalog exists).
	push_joiner_loadout_kit();
}


int Simulation::get_weapon_availability(const String &p_weapon_name) const {
	if (!kernel_) return opennova::world::weapon_availability_value::kAllowed;
	const int idx = kernel_->world.tables.weapons.index_of(p_weapon_name.utf8().get_data());
	if (idx < 0) return opennova::world::weapon_availability_value::kAllowed;
	return kernel_->local.loadout.availability.value_for(idx);
}

bool Simulation::set_local_player_class(int p_player_class) {
	// Sim-side class only. The 0x2F WIRE class is NOT taken from here — retail reads it
	// straight out of the assigned side's profile block, the same integer that picks the
	// kit page [orig: Game_StartMission @0x5257a0], so push_joiner_loadout_kit sources
	// both from player_.weapon_profile. The pushes below just keep the seam re-armed.
	if (!kernel_ || p_player_class < 5 || p_player_class > 9) return false;
	opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (e == nullptr) {
		// A joiner's shell applies the profile class before L has spawned
		// (name-match). Latch it; the joiner spawn block stamps the entity.
		kernel_->local.loadout.pending_player_class = p_player_class;
		push_joiner_loadout_kit();
		return true;
	}
	e->player_class = static_cast<uint8_t>(p_player_class);
	push_joiner_loadout_kit();
	return true;
}

bool Simulation::apply_local_player_loadout(const TypedArray<WeaponKitEntry> &p_kit,
                                                int p_player_class) {
	return apply_local_player_loadout_impl(
			p_kit, p_player_class, /*p_submit_joiner_request=*/true);
}

bool Simulation::apply_local_player_loadout_impl(
		const TypedArray<WeaponKitEntry> &p_kit, int p_player_class,
		bool p_submit_joiner_request) {
	// The armory ACCEPT apply — world/player_loadout.h local_loadout_apply_accept
	// [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline leg]. A joiner
	// applies its kit before L exists (the shell runs the spawn-loadout apply
	// right after runtime setup; L spawns later, on the name-match): the
	// inventory is sim-side state, the entity stamps defer to the joiner spawn
	// block. Dropping the kit here left the joiner unable to fire, reload, or
	// switch (the two-GUI regression).
	if (!kernel_) return false;
	return apply_local_player_loadout_rows(kit_rows_from(p_kit), p_player_class,
			p_submit_joiner_request);
}

bool Simulation::apply_local_player_loadout_rows(
		std::vector<opennova::world::WeaponKitEntry> p_kit, int p_player_class,
		bool p_submit_joiner_request) {
	if (!kernel_) return false;
	if (!opennova::world::local_loadout_apply_accept(kernel_->world, kernel_->local.loadout,
				kernel_->local.weapon, kernel_->local.inventory, kernel_->local.inventory_valid, p_kit,
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
	if (p_submit_joiner_request && is_joiner() && runtime_ && runtime_->in_match())
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
	player_.hud_map_control.reset_spawn();
}

void Simulation::rebuild_local_player_loadout(bool p_select_spawn_default) {
	// The Player_InitPlayer weapon leg — world/player_loadout.h
	// local_loadout_rebuild [orig: @ 0x4e15f0]. The 0x2F pair's SECOND submit
	// carries the LIVE equipped slot, not the fixed 195 [orig:
	// Game_StartMission @0x525c2e passes g_currentWeaponSlot — the value the
	// spawn fill just settled], so the seam re-arms here AFTER the inventory
	// settles. push_joiner_loadout_kit never rebuilds (it holds a one-way
	// re-entry latch), so this cannot recurse.
	if (!kernel_) return;
	opennova::world::local_loadout_rebuild(kernel_->world, kernel_->local.loadout,
			kernel_->local.weapon, kernel_->local.inventory, kernel_->local.inventory_valid,
			p_select_spawn_default);
	push_joiner_loadout_kit();
}

bool Simulation::seed_session_kit_from_profile() {
	// The composition is engine code now (inmatch::seed_session_kit_from_profile,
	// ADR 0031 PR E — the Game_StartMission copy semantics live there). This
	// binding keeps the ROLE gate: a live session covers a LISTEN HOST as
	// well as a joiner, so single player and the editor keep the mission's
	// .bms kit.
	if (!kernel_) return false;
	if (!is_host_listening() && !is_joiner()) return false;
	const uint8_t assigned =
			(is_joiner() && runtime_) ? runtime_->assigned_team() : 0;
	return opennova::inmatch::seed_session_kit_from_profile(kernel_->world,
			player_.weapon_profile, assigned, kernel_->local.loadout,
			player_.weapon_profile_seeded_side);
}

// The loadout profile seams the joiner role keeps shell-side (the weapon.sav
// page composition, the respawn rebuild with its view/map resets); every other
// leg of the joiner frame is the role's (ADR 0043 d3, slice E8b). The role is
// constructed with them.
opennova::inmatch::JoinerRole::KitSeams Simulation::joiner_kit_seams() {
	opennova::inmatch::JoinerRole::KitSeams seams;
	seams.apply_authoritative = [this] { apply_joiner_authoritative_loadout(); };
	seams.reseed_on_side_change = [this] { return reseed_session_kit_on_side_change(); };
	seams.push = [this] { push_joiner_loadout_kit(); };
	seams.respawn = [this] { respawn_local_player_loadout(); };
	return seams;
}

bool Simulation::reseed_session_kit_on_side_change() {
	if (!is_joiner() && !is_host_listening()) return false;
	if (!kernel_) return false;
	const uint8_t assigned =
			(is_joiner() && runtime_) ? runtime_->assigned_team() : 0;
	if (!opennova::inmatch::reseed_session_kit_on_side_change(kernel_->world,
			player_.weapon_profile, assigned, kernel_->local.loadout,
			player_.weapon_profile_seeded_side))
		return false;
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	return true;
}

void Simulation::push_joiner_loadout_kit() {
	// The C2S 0x2F submission content builder is engine code now
	// (inmatch::build_joiner_loadout_kit, ADR 0031 PR E — the one-class-integer
	// rule, the resident-buffer rows, and both side blocks live there, with
	// their witnesses). This binding keeps the seam wiring: the role gate,
	// the empty-catalog arm delay, the re-entry latch, and the pump handoff.
	if (!is_joiner() || !runtime_ || !kernel_) return;
	// The role hook runs before the shell loads weapon.def (MissionRoot orders
	// load_from_mission_data ahead of load_weapon_table), and a kit resolved against an
	// EMPTY catalog would skip every row — latching that would submit a zero-entry 0x2F
	// pair AND disarm the runtime's capture-default fallback. Leave the seam unarmed
	// until the catalog exists; load_weapon_table re-pushes from the carried state.
	if (kernel_->world.tables.weapons.empty()) return;
	// rebuild_local_player_loadout re-pushes once the equipped combo has settled; this
	// latch keeps that one-way (a push must never drive a rebuild back into itself).
	if (player_.pushing_joiner_loadout_kit) return;
	player_.pushing_joiner_loadout_kit = true;
	opennova::inmatch::JoinerConnection::LoadoutKit wire_kit;
	opennova::inmatch::build_joiner_loadout_kit(kernel_->world, player_.weapon_profile,
			runtime_->assigned_team(), kernel_->local.loadout,
			kernel_->local.inventory_valid ? kernel_->local.inventory.equipped_combo : -1,
			kernel_->local.inventory_valid ? &kernel_->local.inventory : nullptr,
			wire_kit);
	runtime_->set_loadout_kit(std::move(wire_kit));
	player_.pushing_joiner_loadout_kit = false;
}

String Simulation::weapon_profile_relpath(const String &p_expansion_name) {
	return String(opennova::playersav::weapon_sav_relpath(
			std::string(p_expansion_name.utf8().get_data()))
					.c_str());
}

Ref<WeaponProfileSummary> Simulation::read_weapon_profile_summary(const String &p_path) {
	opennova::playersav::File profile = opennova::playersav::make_defaults();
	// Raw bytes for the menu: the PLAYER_INFO screen selects its rows straight
	// from the profile globals; the [5,9] class clamp is a SESSION-START step
	// (apply_session_settings_to_globals) that load_weapon_profile applies.
	const Error error = read_weapon_profile_file(p_path, profile);
	Ref<WeaponProfileSummary> out;
	out.instantiate();
	// OpenNova's active profile is slot 0. Retail indexes the same five-record
	// array by g_curProfileSlot @0x25506B8 (0x1080C stride) before reading or
	// writing its record; see docs/playerinfo/avatars-re.md.
	out->assign(profile.slots[0], int(error), error == OK);
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

Ref<FpViewmodelSpec> Simulation::fp_viewmodel_spec(bool p_has_def, const String &p_gfx1,
		const String &p_character_arms, const String &p_animadm, int p_flags) {
	Ref<FpViewmodelSpec> out;
	out.instantiate();
	out->assign(opennova::simassets::fp_viewmodel_spec(p_has_def,
			std::string(p_gfx1.utf8().get_data()),
			std::string(p_character_arms.utf8().get_data()),
			std::string(p_animadm.utf8().get_data()),
			static_cast<uint32_t>(p_flags)));
	return out;
}

Error Simulation::load_weapon_profile(const String &p_path) {
	// The caller supplies the already resolved absolute path (retail builds it
	// as g_ExpansionName[0] ? "expansion\\<g_ExpansionName>\\weapon.sav" :
	// "weapon.sav" [orig: @0x54f68c..@0x54f6b7]). weapon.sav is a SAVE file on
	// the filesystem, not a PFF/mount entry, so it is read through FileAccess;
	// the defaults / header-gate / class-clamp law is the engine's
	// playersav::profile_or_defaults. A joiner still submits a class-legal pair.
	player_.weapon_profile_loaded = false;
	Error result = OK;
	PackedByteArray bytes;
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
			bytes = file->get_buffer(static_cast<int64_t>(file->get_length()));
			file->close();
		}
	}
	player_.weapon_profile_loaded = opennova::playersav::profile_or_defaults(
			bytes.ptr(), static_cast<std::size_t>(bytes.size()), player_.weapon_profile);
	if (result == OK && !player_.weapon_profile_loaded) {
		print_verbose(vformat(
				"weapon.sav: \"%s\" is not a readable profile — keeping the shipped defaults",
				p_path));
		result = ERR_FILE_CORRUPT;
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

void Simulation::apply_joiner_authoritative_loadout() {
	if (!is_joiner() || !runtime_ || !kernel_ || kernel_->world.tables.weapons.empty()) return;
	const uint64_t revision = runtime_->authoritative_loadout_revision();
	if (revision == 0 || revision <= net_.joiner_applied_loadout_revision) return;

	// The grant -> kit-row conversion (name resolve + SIGNED clip reinterpret)
	// is inmatch::kit_from_authoritative_grant's.
	const opennova::WeaponLoadout &grant = runtime_->authoritative_loadout();
	std::vector<opennova::world::WeaponKitEntry> kit;
	opennova::inmatch::kit_from_authoritative_grant(kernel_->world.tables.weapons, grant, kit);
	// Do not echo an authoritative grant back as a new C2S 0x2F request. The
	// S2C handler rebuilds the slots directly at recv-before-actions. The rebuild
	// inside still re-arms the seam, but its ROWS come from the profile page, never
	// from the grant — only the live equipped slot refreshes, which is exactly what
	// retail's second submit carries [orig: Game_StartMission @0x525c2e].
	if (apply_local_player_loadout_rows(
				std::move(kit), grant.avatar_class, /*p_submit_joiner_request=*/false))
		net_.joiner_applied_loadout_revision = revision;
}

Ref<PlayerInventory> Simulation::get_local_player_inventory() const {
	opennova::world::LocalInventoryView v;
	v.valid = kernel_->local.inventory_valid;
	v.equipped_combo = kernel_->local.inventory.equipped_combo;
	v.carry_flags = kernel_->local.inventory.carry_flags;
	if (kernel_ != nullptr) {
		const opennova::world::WeaponTable &table = kernel_->world.tables.weapons;
		for (int32_t combo = 0; combo < opennova::world::weapon_combo::kSlotCount;
		     ++combo) {
			const opennova::world::WeaponInventorySlot *s = kernel_->local.inventory.slot(combo);
			if (s == nullptr || s->adm_index < 0) continue;
			const opennova::world::WeaponTableEntry *def =
					table.by_index(static_cast<uint8_t>(s->adm_index));
			if (def == nullptr) continue;
			opennova::world::LocalInventoryView::Slot slot;
			slot.combo = combo;
			slot.name = def->name;
			slot.clip = s->clip;
			v.slots.push_back(std::move(slot));
			if (combo == kernel_->local.inventory.equipped_combo)
				v.equipped_name = def->name;
		}
	}
	Ref<PlayerInventory> out;
	out.instantiate();
	out->assign(v);
	return out;
}

TypedArray<WeaponKitEntry> Simulation::get_local_player_loadout() const {
	TypedArray<WeaponKitEntry> out;
	const std::vector<opennova::world::WeaponKitEntry> kit =
			kernel_->local.loadout.spawn_kit_set ? kernel_->local.loadout.spawn_kit : opennova::world::weapon_kit_default();
	for (const opennova::world::WeaponKitEntry &entry : kit) {
		Ref<WeaponKitEntry> row;
		row.instantiate();
		row->assign(entry);
		out.push_back(row);
	}
	return out;
}

// score.ini -> this session's scoring awards (world::score_rules).
// Retail overlays the file onto 12 hardcoded 452-byte gametype rows
// [orig: GameType_CreateDefaultSettings @0x52DD00 -> ScoreConfig_LoadFile @0x52D8A0];
// the defaults are not ported, so an absent file leaves the rules !valid (every award
// a no-op) rather than guessing values.
Error Simulation::load_score_config(const Ref<ResourceRoot> &p_resource_root,
                                    const String &p_name) {
	if (!kernel_) return ERR_UNCONFIGURED;
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	// LOOSE-FIRST on purpose: retail gates the load on File_IsSingleFile("score.ini")
	// [orig: @0x436ED0], which is a FindFirstFileA check on disk — score.ini is a loose
	// file next to the executable, not an archive member (unlike weapon.def/ammo.def,
	// which live in localres.pff). An archive-preferring lookup finds nothing here.
	const PackedByteArray bytes =
			p_resource_root->read_file(file_name, ResourceRoot::LOOKUP_FORCE_LOOSE_FIRST);
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;

	opennova::score::File parsed;
	std::string error;
	if (!opennova::score::parse(bytes.ptr(), static_cast<size_t>(bytes.size()), parsed, error))
		return ERR_PARSE_ERROR;
	assets_.score_config = std::move(parsed);
	assets_.score_config_loaded = true;
	refresh_score_rules();
	return OK;
}

// Resolve the session's score row from the mission's game-mode bit. Called from BOTH
// load sites so either order works: the config landing after the mission, or before it.
// The mode bit -> g_GameType code word is the already-ported ladder
// [orig: AI_GetTaskTypeFromFlags @0x40DAE0 -> Game_StartMission @0x524360].
void Simulation::refresh_score_rules() {
	if (!kernel_) return;
	if (!assets_.score_config_loaded) {
		kernel_->world.tables.score_rules = opennova::world::ScoreRules{};
		return;
	}
	const uint32_t mode = opennova::bms::selected_game_mode(
			static_cast<opennova::bms::AttribFlags>(kernel_->world.tables.mission_attrib_flags));
	kernel_->world.tables.score_rules = opennova::world::build_score_rules(
			assets_.score_config, opennova::game_type::for_mission_mode(mode));
}

// weapon.def -> the sim world's armory table. Mirrors the retail load site (Game_StartMission
// parses literally "weapon.def" through WeaponDefs_LoadFile right after AnimDef_InitAll wipes
// the AdmDef table [orig: @0x5254b3/@0x5254bd]); build_weapon_table ports the witnessed
// allocation rule (null@0 + by-name-reuse-else-lowest-free = file order; §5.57, D-NET-141).
Error Simulation::load_weapon_table(const Ref<ResourceRoot> &p_resource_root,
                                        const String &p_name) {
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	// The armory build, the retained rows, the seat-spec turret re-stamp, the
	// witnessed WPN_M4AUTO re-stamp (D-NET-143), the mission loadout-chunk
	// promotion through the SP-vs-net gate and the spawn-kit rebuild are the
	// kernel's ONE table-load body; this binding hands it the mounted source.
	const opennova::ResourceIndex *index = &p_resource_root->native_index();
	opennova::mission::BootFileSource files;
	files.has_file = [index](const std::string &name) {
		return index->has_file(name);
	};
	files.read_file = [index](const std::string &name, std::vector<uint8_t> &out) {
		return index->read_file(name, out);
	};
	if (!kernel_->load_weapon_table(files, index,
				std::string(file_name.utf8().get_data())))
		return ERR_FILE_NOT_FOUND;
	// In a live session the resident kit buffer is the assigned side's profile
	// page, copied in the moment the catalog can resolve its names — retail's
	// Game_StartMission copy into restrictionData [orig: @0x525813], which runs
	// before Player_InitPlayer builds the display list from that same buffer.
	// Offline this no-ops and the mission's .bms kit stands.
	if (seed_session_kit_from_profile())
		rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// The catalog exists now: arm the joiner's 0x2F seam from the carried spawn
	// kit (the earlier pushes deliberately no-op against the empty catalog, and
	// a menu join that never opens PLAYER_INFO has no later class/loadout apply
	// to re-push through — without this the pair would ride zero kit entries).
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
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	// The ballistics build + the weapon round_type resolve + the damage-class
	// sync are the kernel's one body [orig: AmmoDef_LoadAll @0x40b0b0].
	const opennova::ResourceIndex *index = &p_resource_root->native_index();
	opennova::mission::BootFileSource files;
	files.has_file = [index](const std::string &name) {
		return index->has_file(name);
	};
	files.read_file = [index](const std::string &name, std::vector<uint8_t> &out) {
		return index->read_file(name, out);
	};
	if (!kernel_->load_ammo_table(files, std::string(file_name.utf8().get_data())))
		return ERR_FILE_NOT_FOUND;
	return OK;
}
