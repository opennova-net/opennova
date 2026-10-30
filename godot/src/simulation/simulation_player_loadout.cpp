// Simulation — the LOCAL PLAYER loadout cluster: armory / usegun / mount
// interactions, the loadout (slot pool / spawn kit / map rules), the weapon
// profile, and the weapon/ammo table feeds.
#include "simulation/simulation_internal.h"
#include "simulation/fp_viewmodel_spec.h"
#include "simulation/player_inventory.h"
#include "simulation/use_press_row.h"
#include "simulation/weapon_kit_entry.h"
#include "player/player_profiles.h"
#include "resource_index/launch_flags.h" // the /noreload flag the session copy reads
#include "util/string_convert.h"

#include <runtime/inmatch/client_runtime.h> // ClientRuntime::queue_loadout_submit
#include <runtime/inmatch/loadout_submit.h> // the 0x2F submission + 0x5A grant conversions

#include <runtime/mission/promote.h> // stash_mission_loadout_rules (the chunk-tuple conversion)
#include <runtime/profile/profile_controls.h> // the session copy's input words
#include <runtime/renderer/fp_viewmodel_spec.h> // the FP viewmodel submit rule
#include <runtime/world/local_player_view.h> // the USE key's vehicle-loadout zone gates

#include <algorithm>

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

bool Simulation::local_player_in_armory_zone() const {
	// The useitem armory arm's gate (engine local_player_in_armory_zone:
	// unmounted + the type-6 volume bit).
	return kernel_ && opennova::world::local_player_in_armory_zone(kernel_->world);
}

Simulation::UseItemArm Simulation::local_player_use_item_arm() const {
	if (!kernel_) return USE_ITEM_ARM_MOUNT_CHAIN;
	return static_cast<UseItemArm>(opennova::world::local_player_use_item_arm(
			kernel_->world, kernel_->local.view_session_inputs));
}

Simulation::UseItemArm Simulation::take_use_item_press() {
	if (!kernel_) return USE_ITEM_ARM_MOUNT_CHAIN;
	return static_cast<UseItemArm>(kernel_->local.take_use_item_press());
}

TypedArray<UsePressRow> Simulation::debug_use_press_trace(int64_t p_from_seq) const {
	TypedArray<UsePressRow> rows;
	if (!kernel_) return rows;
	const uint32_t from = static_cast<uint32_t>(
			std::clamp<int64_t>(p_from_seq, 0, static_cast<int64_t>(UINT32_MAX)));
	for (const opennova::world::UsePressRecord &record : kernel_->local.use_trace.since(from)) {
		Ref<UsePressRow> row;
		row.instantiate();
		row->assign(record);
		rows.push_back(row);
	}
	return rows;
}

int64_t Simulation::debug_use_press_next_seq() const {
	return kernel_ ? static_cast<int64_t>(kernel_->local.use_trace.next_seq()) : 0;
}

int64_t Simulation::debug_attach_requests_sent() const {
	return runtime_ != nullptr ? static_cast<int64_t>(runtime_->attach_requests_sent()) : 0;
}

void Simulation::debug_set_use_press_watch(int64_t p_handle) {
	if (!kernel_) return;
	kernel_->local.use_trace.watch =
			p_handle >= 0 && p_handle < 0xFFFF ? static_cast<uint16_t>(p_handle) : uint16_t{0xFFFF};
}

void Simulation::release_use_item_key(bool p_system_key_up) {
	if (kernel_) kernel_->local.release_use_item_key(p_system_key_up);
}

void Simulation::clear_deferred_input() {
	if (kernel_) kernel_->local.clear_deferred_input();
}

bool Simulation::is_armory_press_latched() const {
	return kernel_ && kernel_->local.armory_press_latched;
}

bool Simulation::local_player_in_vehicle_loadout_zone() const {
	// The useitem vehicle-loadout arm's gate (engine local_player_in_vehicle_loadout_zone:
	// unmounted + the type-11 volume touch).
	return kernel_ && opennova::world::local_player_in_vehicle_loadout_zone(kernel_->world);
}

bool Simulation::local_player_vehicle_zone_team_matches() const {
	// The bay's team gate (engine local_player_vehicle_zone_team_matches: the
	// ground entity's team byte 0 or equal to the player's).
	return kernel_ && opennova::world::local_player_vehicle_zone_team_matches(kernel_->world);
}

// The vehicle screen's session: the engine's inmatch::VehicleMenu on the role's client runtime
// (the joiner's or the listen host's own) carries every rule; its controller runs the screen.
opennova::inmatch::VehicleMenuSession Simulation::vehicle_menu_session() {
	opennova::inmatch::VehicleMenuSession session;
	if (!kernel_ || runtime_ == nullptr) return session;
	session.runtime = runtime_;
	session.world = &kernel_->world;
	session.items = kernel_->items_table();
	return session;
}

bool Simulation::local_player_toggle_mount() {
	// The USE-ITEM mount toggle for the local player — the shell calls this when the
	// armory/vehicle-zone legs of the key don't apply. [orig: Input_ProcessFrame release
	// edge @0x49d6dc -> Entity_ToggleVehicleMount @0x436950]
	if (!kernel_) return false;
	// The gates are the kernel's (LocalPlayer::mount_toggle_allowed); a joiner
	// then queues C2S 0x26 (attach) / 0x27 (detach) and waits for the 0x0A
	// stream to confirm (JoinerRole::queue_mount_toggle).
	if (is_joiner())
		return kernel_->local.mount_toggle_allowed() && joiner_role_ &&
				joiner_role_->queue_mount_toggle();
	// The authority's toggle is the kernel's one body (the out-of-session
	// UseGun rejection, the seat transition, the view/weapon folds).
	const bool changed = kernel_->local.toggle_mount();
	if (changed) refresh_local_player_view_effects();
	return changed;
}

bool Simulation::local_player_select_seat(int p_index) {
	if (!kernel_) return false;
	if (is_joiner()) return joiner_role_ && joiner_role_->queue_numbered_seat(p_index);
	const bool changed = kernel_->local.select_numbered_seat(p_index);
	if (changed) refresh_local_player_view_effects();
	return changed;
}

bool Simulation::local_player_radio_request_icon_viewer() const {
	if (!kernel_) {
		return false;
	}
	const opennova::world::Entity *local =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	return local != nullptr && opennova::world::friendly_tag_radio_request_viewer(*local);
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
	// Sim-side class only (engine local_loadout_set_player_class). The 0x2F WIRE
	// class is NOT taken from here: push_joiner_loadout_kit sources it from
	// player_.weapon's record (inmatch::build_joiner_loadout_kit). The push just
	// keeps the seam re-armed.
	if (!kernel_ || !opennova::world::local_loadout_set_player_class(
				kernel_->world, kernel_->local.loadout, p_player_class))
		return false;
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
			p_submit_joiner_request, opennova::world::LoadoutApplySelect::HeldWeapon);
}

bool Simulation::apply_local_player_loadout_rows(
		std::vector<opennova::world::WeaponKitEntry> p_kit, int p_player_class,
		bool p_submit_joiner_request, opennova::world::LoadoutApplySelect p_select) {
	if (!kernel_) return false;
	// The armory ACCEPT in a session, a joiner's client and a listen host's own client
	// alike: the selection becomes the process's loadout name list, off the authority the slot table
	// empties, and the one 0x2F (the armory's class, the live slot) goes out; nothing else
	// applies locally until the host's grant (engine: inmatch::armory_accept_in_session
	// carries the witness; D-NET-695, D-NET-843).
	const bool host_own_client = is_host_listening() && !is_joiner() && kernel_->world.rules.mp_session;
	const bool joiner_in_match = is_joiner() && runtime_ != nullptr && runtime_->in_match();
	if (p_submit_joiner_request && runtime_ != nullptr && (host_own_client || joiner_in_match) &&
			p_player_class >= 0 && p_player_class <= 0xFF) {
		const opennova::LoadoutSubmit submit = opennova::inmatch::armory_accept_in_session(
				kernel_->world, kernel_->local, runtime_->view().protocol(), p_kit,
				static_cast<uint8_t>(p_player_class),
				/*authority=*/host_own_client, kernel_->mission.header.mission_type);
		// The seam re-arms from the new resident kit.
		push_joiner_loadout_kit();
		return runtime_->queue_loadout_submit(submit);
	}
	if (!opennova::world::local_loadout_apply_accept(kernel_->world, kernel_->local.loadout,
				kernel_->local.weapon, kernel_->local.inventory, kernel_->local.inventory_valid, p_kit,
				p_player_class, /*validate_banned=*/p_submit_joiner_request, p_select))
		return false;
	// Always re-arm the 0x2F seam after a rebuild settles the equipped combo —
	// the S2C 0x5A grant apply (p_submit=false) refreshes only the live
	// equipped slot on the seam, exactly retail's second-submit content
	// [orig: Game_StartMission @0x525c2e].
	push_joiner_loadout_kit();
	return true;
}

void Simulation::reinit_local_player() {
	// Player_InitPlayer(1)'s re-run is the engine's (LocalPlayer::respawn_init, which carries
	// the witness); the 0x2F seam re-arms once the rebuilt kit has settled.
	if (!kernel_) return;
	kernel_->local.respawn_init(/*rebuild_kit=*/true);
	push_joiner_loadout_kit();
}

void Simulation::rebuild_local_player_loadout(bool p_select_spawn_default) {
	// The Player_InitPlayer weapon leg — world/player_loadout.h
	// local_loadout_rebuild (engine: runtime/inmatch/joiner_role.cpp). The 0x2F pair's SECOND submit
	// carries the current slot word, not the fixed 195 (engine: runtime/inmatch/loadout_submit.cpp), so the seam re-arms here AFTER the inventory
	// settles. push_joiner_loadout_kit never rebuilds (it holds a one-way
	// re-entry latch), so this cannot recurse.
	if (!kernel_) return;
	opennova::world::local_loadout_rebuild(kernel_->world, kernel_->local.loadout,
			kernel_->local.weapon, kernel_->local.inventory, kernel_->local.inventory_valid,
			p_select_spawn_default);
	push_joiner_loadout_kit();
}

bool Simulation::seed_session_kit_from_profile() {
	// The seed is engine code (inmatch::SessionWeaponProfile::seed_kit, its session and
	// single-player arms); this binding hands it the role: a live session covers a
	// LISTEN HOST as well as a joiner.
	if (!kernel_) return false;
	const uint8_t assigned =
			(is_joiner() && runtime_) ? runtime_->assigned_team() : 0;
	// The session's protocol and mission type: a protocol that fixes the loadout class writes
	// it into the profile's side block ahead of the page pick (engine
	// seed_session_kit_from_profile).
	return player_.weapon.seed_kit(kernel_->world, is_host_listening() || is_joiner(),
			session_protocol(net_, assets_.root), kernel_->mission.header.mission_type, assigned,
			kernel_->local.loadout);
}

// The loadout profile seams the joiner role keeps shell-side (the weapon.sav
// page composition); every other leg of the joiner frame is the role's (ADR
// 0043 d3, slice E8b), the redeploy's respawn resets included
// (LocalPlayer::respawn_init). The role is constructed with them.
opennova::inmatch::JoinerRole::KitSeams Simulation::joiner_kit_seams() {
	opennova::inmatch::JoinerRole::KitSeams seams;
	seams.apply_authoritative = [this] { apply_joiner_authoritative_loadout(); };
	seams.reseed_on_side_change = [this] { return reseed_session_kit_on_side_change(); };
	// A self S2C 0x50's page copy over the resident buffer (engine:
	// inmatch::reseed_session_kit_on_team_assign); its Player_InitPlayer(1) rebuilds after.
	seams.reseed_on_team_assign = [this](uint8_t p_team) {
		if (kernel_)
			opennova::inmatch::reseed_session_kit_on_team_assign(player_.weapon.record, p_team,
					kernel_->local.loadout, player_.weapon.seeded_side);
	};
	seams.push = [this] { push_joiner_loadout_kit(); };
	return seams;
}

bool Simulation::reseed_session_kit_on_side_change() {
	if (!is_joiner() && !is_host_listening()) return false;
	if (!kernel_) return false;
	const uint8_t assigned =
			(is_joiner() && runtime_) ? runtime_->assigned_team() : 0;
	if (!opennova::inmatch::reseed_session_kit_on_side_change(kernel_->world,
			player_.weapon.record, session_protocol(net_, assets_.root),
			kernel_->mission.header.mission_type, assigned, kernel_->local.loadout,
			player_.weapon.seeded_side))
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
	// A listen host's own client submits the same pair at its mission start
	// (engine: HostRole::send_own_mission_status, D-NET-695).
	if (!(is_joiner() || is_host_listening()) || !runtime_ || !kernel_) return;
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
	opennova::inmatch::build_joiner_loadout_kit(kernel_->world, player_.weapon.record,
			is_joiner() ? runtime_->assigned_team() : uint8_t{0}, kernel_->local.loadout,
			kernel_->local.inventory_valid ? kernel_->local.inventory.current_slot : -1,
			kernel_->local.inventory_valid ? &kernel_->local.inventory : nullptr,
			wire_kit);
	runtime_->set_loadout_kit(std::move(wire_kit));
	player_.pushing_joiner_loadout_kit = false;
}

double Simulation::weapon_def_pos_scale() {
	return opennova::renderer::kWeaponDefPosScale;
}

Vector3 Simulation::viewmodel_fallback_pos_units() {
	const float *v = opennova::renderer::kFallbackPosUnits;
	return Vector3(v[0], v[1], v[2]);
}

Vector3 Simulation::viewmodel_fallback_tpos_units() {
	const float *v = opennova::renderer::kFallbackTposUnits;
	return Vector3(v[0], v[1], v[2]);
}

Vector3 Simulation::viewmodel_fallback_rot_bias_deg() {
	const float *v = opennova::renderer::kFallbackRotBiasDeg;
	return Vector3(v[0], v[1], v[2]);
}

double Simulation::viewmodel_pass_near_z() {
	return opennova::renderer::kViewmodelPassNearZ;
}

Ref<FpViewmodelSpec> Simulation::fp_viewmodel_spec(bool p_has_def, const String &p_gfx1,
		const String &p_character_arms, const String &p_animadm, int p_flags) {
	Ref<FpViewmodelSpec> out;
	out.instantiate();
	out->assign(opennova::renderer::fp_viewmodel_spec(p_has_def,
			opennova::to_std(p_gfx1),
			opennova::to_std(p_character_arms),
			opennova::to_std(p_animadm),
			static_cast<uint32_t>(p_flags)));
	return out;
}

Error Simulation::use_player_profile(const Ref<PlayerProfiles> &p_profiles) {
	if (p_profiles.is_null()) return ERR_INVALID_PARAMETER;
	const opennova::profile::PlayerProfiles &profiles = p_profiles->native();
	// The weapon record with the session start's class clamp, the player.sav record for
	// single player's session words.
	player_.weapon.use(profiles.current_weapons());
	player_.profile_record = profiles.current();
	player_.profile_record_set = true;
	// The mission start's session copy of the record's input words, and its
	// inverse OPTIONS_AUTOMEDIC word onto a live joiner runtime.
	apply_session_input();
	install_auto_medic_preference();
	// The record just changed, so the resident kit buffer and the seam both have to
	// follow it. Retail never has to re-run this because PlayerProfile_LoadAllFromDisk
	// completes at boot / expansion switch, long before Game_StartMission copies a page
	// into restrictionData [orig: @0x54f4d0 vs @0x525813]; our profile can only reach
	// the sim AFTER the runtime exists, so the copy is redone here instead of
	// depending on the shell's call order. seed_session_kit_from_profile is a no-op
	// outside a live session and before the catalog resolves names, so this leaves
	// single player and a pre-catalog load exactly as they were. The rebuild keeps the
	// fov target the mission start's environment and script init left (the engine's
	// world::local_loadout_rebuild_after_start).
	if (seed_session_kit_from_profile())
		opennova::world::local_loadout_rebuild_after_start(kernel_->world, kernel_->local.loadout,
				kernel_->local.weapon, kernel_->local.inventory, kernel_->local.inventory_valid);
	// The seam's class AND its kit page both come out of this record — re-arm it once,
	// after the rebuild settled the kit.
	push_joiner_loadout_kit();
	return OK;
}

namespace {

// The words the session holds, as profile_controls.h names them.
opennova::profile::SessionInput live_input(const opennova::mission::MissionKernel &kernel) {
	opennova::profile::SessionInput live;
	live.mouse_sensitivity = kernel.local.look_settings.sensitivity;
	live.invert_mouse = kernel.local.look_settings.invert_y;
	live.scope_scale = kernel.local.look_settings.scope_scale;
	live.auto_reload = kernel.world.rules.auto_reload;
	return live;
}

void install_input(opennova::mission::MissionKernel &kernel,
		const opennova::profile::SessionInput &input) {
	kernel.local.look_settings.sensitivity = input.mouse_sensitivity;
	kernel.local.look_settings.invert_y = input.invert_mouse;
	kernel.local.look_settings.scope_scale = input.scope_scale;
	kernel.world.rules.auto_reload = input.auto_reload;
}

} // namespace

void Simulation::apply_session_input() {
	if (!kernel_) return;
	install_input(*kernel_, opennova::profile::session_input(
			player_.profile_record, LaunchFlags::no_reload()));
}

Error Simulation::apply_ingame_options(const Ref<PlayerProfiles> &p_profiles) {
	if (p_profiles.is_null()) return ERR_INVALID_PARAMETER;
	// The record's copy the own revive mark reads follows the Accept too.
	player_.profile_record = p_profiles->native().current();
	player_.profile_record_set = true;
	if (kernel_)
		install_input(*kernel_, opennova::profile::ingame_accept_input(
				player_.profile_record, live_input(*kernel_)));
	return OK;
}

int Simulation::get_session_mouse_sensitivity() const {
	return kernel_ ? kernel_->local.look_settings.sensitivity : 0;
}

bool Simulation::is_session_mouse_inverted() const {
	return kernel_ && kernel_->local.look_settings.invert_y;
}

bool Simulation::is_session_auto_reload() const {
	return kernel_ && kernel_->world.rules.auto_reload;
}

void Simulation::apply_joiner_authoritative_loadout() {
	if (!is_joiner() || !runtime_ || !kernel_ || kernel_->world.tables.weapons.empty()) return;
	const uint64_t revision = runtime_->authoritative_loadout_revision();
	if (revision != 0 && revision > net_.joiner_applied_loadout_revision) {
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
		// The apply's select is the session protocol's (engine inmatch::grant_apply_select).
		if (apply_local_player_loadout_rows(std::move(kit), grant.avatar_class,
					/*p_submit_joiner_request=*/false,
					opennova::inmatch::grant_apply_select(runtime_->view().protocol())))
			net_.joiner_applied_loadout_revision = revision;
	}
	// The S2C 0x0F pool image lands AFTER the 0x5A slot rebuild, exactly as the
	// retail client handler copies g_LocalAmmoPools and re-draws every clip (the
	// engine's weapon_inventory_apply_authority_pools carries the witness). It
	// waits for a valid inventory: the rebuild above is what makes one on a joiner.
	const uint64_t pools_revision = runtime_->authoritative_ammo_pools_revision();
	if (pools_revision != 0 && pools_revision > net_.joiner_applied_ammo_pools_revision &&
	    kernel_->local.inventory_valid) {
		opennova::world::weapon_inventory_apply_authority_pools(
				kernel_->world.tables.weapons, kernel_->local.inventory,
				runtime_->authoritative_ammo_pools());
		net_.joiner_applied_ammo_pools_revision = pools_revision;
	}
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
			slot.clip = opennova::world::weapon_inventory_loaded_rounds(
					table, kernel_->local.inventory, combo);
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
			opennova::world::local_loadout_kit(kernel_->world, kernel_->local.loadout);
	for (const opennova::world::WeaponKitEntry &entry : kit) {
		Ref<WeaponKitEntry> row;
		row.instantiate();
		row->assign(entry);
		out.push_back(row);
	}
	return out;
}

// weapon.def -> the sim world's armory table. Mirrors the retail load site (Game_StartMission
// parses literally "weapon.def" through WeaponDefs_LoadFile right after AnimDef_InitAll wipes
// the AdmDef table (engine: runtime/inmatch/host_boot.cpp)); build_weapon_table ports the witnessed
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
	const opennova::mission::BootFileSource files =
			opennova::mission::boot_files_from_index(p_resource_root->native_index());
	if (!kernel_->load_weapon_table(files, &p_resource_root->native_assets(),
				opennova::to_std(file_name)))
		return ERR_FILE_NOT_FOUND;
	// In a live session the resident kit buffer is the assigned side's profile
	// page, copied in the moment the catalog can resolve its names — retail's
	// Game_StartMission copy into restrictionData [orig: @0x525813], which runs
	// before Player_InitPlayer builds the display list from that same buffer.
	// Offline the profile's single-player page fills it unless the mission's .bms
	// kit promoted.
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
// @0x40b0b0, the sibling of the weapon.def load (engine: base/gameprofile/required_resources.cpp)); the resolve binds each
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
	// sync are the kernel's one body (engine: formats/def/def_ammo.cpp).
	const opennova::mission::BootFileSource files =
			opennova::mission::boot_files_from_index(p_resource_root->native_index());
	if (!kernel_->load_ammo_table(files, opennova::to_std(file_name)))
		return ERR_FILE_NOT_FOUND;
	return OK;
}
