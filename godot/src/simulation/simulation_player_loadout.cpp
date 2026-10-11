// Simulation — the LOCAL PLAYER loadout cluster: armory / usegun / mount
// interactions, the loadout (slot pool / spawn kit / map rules), the weapon
// profile, and the weapon/ammo table feeds.
#include "simulation/simulation_internal.h"
#include "simulation/fp_viewmodel_spec.h"
#include "simulation/player_inventory.h"
#include "simulation/weapon_kit_entry.h"
#include "player/player_profiles.h"
#include "resource_index/launch_flags.h" // the /noreload flag the session copy reads
#include "util/string_convert.h"

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
	if (!kernel_) return false;
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	// The use-item armory leg rejects a seated player before consulting the type-6
	// volume bit [orig: Input_HandleActionBinding_0 @0x4e0b3f, parentSlot == 0].
	return e != nullptr && !e->mounted &&
	       (e->flags & opennova::world::kEntityFlagArmoryZone) != 0;
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

bool Simulation::local_player_toggle_mount() {
	// The USE-ITEM mount toggle for the local player — the shell calls this when the
	// armory/vehicle-zone legs of the key don't apply. [orig: Input_ProcessFrame release
	// edge @0x49d6dc -> Entity_ToggleVehicleMount @0x436950]
	if (!kernel_) return false;
	// The witnessed non-authority path queues C2S 0x26 (attach) /
	// sends 0x27 (detach) and waits for the 0x0A stream to confirm [orig:
	// Entity_RequestVehicleAttach @0x4364a0 / Entity_SendDetachPacket @0x435510].
	// Joiners therefore choose a candidate locally and change L's relation only
	// when the authoritative echo arrives; the attach request turns L to the
	// seat before it queues the 0x26, as retail's does.
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

bool Simulation::local_player_select_seat(int p_index) {
	if (!kernel_) return false;
	if (is_joiner()) return joiner_role_ && joiner_role_->queue_numbered_seat(p_index);
	const bool changed = kernel_->local.select_numbered_seat(p_index);
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
	// The friendly-tags gather (D-HUD-20): the role's world walk plus a
	// joiner's roster walk (inmatch/role_feeds.h collect_friendly_tags); the
	// overlay lifts, projects, and feeds the HUD compiler's element.
	return opennova::inmatch::collect_friendly_tags(role_view(), r_tags);
}

opennova::inmatch::HudRoleFacts Simulation::hud_role_facts(uint32_t p_voice_menus) const {
	// The breath bar's, the MP session lines', the HUDLS scan's and the asked-for
	// F9 / F10 menus' facts for this client (inmatch/role_feeds.h hud_role_facts
	// carries the witnesses).
	return opennova::inmatch::hud_role_facts(role_view(), p_voice_menus);
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
	kernel_->local.hud_map_control.reset_spawn();
}

void Simulation::rebuild_local_player_loadout(bool p_select_spawn_default) {
	// The Player_InitPlayer weapon leg — world/player_loadout.h
	// local_loadout_rebuild [orig: @ 0x4e15f0]. The 0x2F pair's SECOND submit
	// carries the LIVE equipped slot, not the fixed 195 [orig:
	// Game_StartMission @0x525c2e passes g_CurrentWeaponSlot — the value the
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
// page composition, the respawn's view/map resets); every other leg of the
// joiner frame is the role's (ADR 0043 d3, slice E8b). The role is constructed
// with them.
opennova::inmatch::JoinerRole::KitSeams Simulation::joiner_kit_seams() {
	opennova::inmatch::JoinerRole::KitSeams seams;
	seams.apply_authoritative = [this] { apply_joiner_authoritative_loadout(); };
	seams.reseed_on_side_change = [this] { return reseed_session_kit_on_side_change(); };
	seams.push = [this] { push_joiner_loadout_kit(); };
	// A joiner's redeploy rebuilds no kit of its own: the release bundle's 0x5A,
	// which the host built from the slot's loadout buffer, has already rebuilt the
	// slots with its counts (apply_authoritative above), and retail's client runs
	// Player_InitPlayer only at the mission start and a team change (D-NET-378;
	// the witness is the JoinerRole::KitSeams note in runtime/inmatch/joiner_role.h).
	seams.respawn = [this] {
		reset_local_player_view_effects();
		kernel_->local.hud_map_control.reset_spawn();
	};
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
	// The weapon record with the session start's class clamp (playersav
	// clamp_classes), the player.sav record for single player's session words.
	opennova::playersav::File clamped;
	clamped.slots[0] = profiles.current_weapons();
	opennova::playersav::clamp_classes(clamped);
	player_.weapon_profile = clamped.slots[0];
	player_.weapon_profile_loaded = true;
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
	// single player and a pre-catalog load exactly as they were.
	if (seed_session_kit_from_profile())
		rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// The seam's class AND its kit page both come out of this record — re-arm it.
	push_joiner_loadout_kit();
	return OK;
}

namespace {

// The words the session holds, as profile_controls.h names them.
opennova::profile::SessionInput live_input(const opennova::mission::MissionKernel &kernel) {
	opennova::profile::SessionInput live;
	live.mouse_sensitivity = kernel.local.look_settings.sensitivity;
	live.invert_mouse = kernel.local.look_settings.invert_y;
	live.auto_reload = kernel.world.rules.auto_reload;
	return live;
}

void install_input(opennova::mission::MissionKernel &kernel,
		const opennova::profile::SessionInput &input) {
	kernel.local.look_settings.sensitivity = input.mouse_sensitivity;
	kernel.local.look_settings.invert_y = input.invert_mouse;
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
		if (apply_local_player_loadout_rows(
					std::move(kit), grant.avatar_class, /*p_submit_joiner_request=*/false))
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
			kernel_->local.loadout.spawn_kit_set ? kernel_->local.loadout.spawn_kit : opennova::world::weapon_kit_default();
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
	if (!kernel_->load_weapon_table(files, &p_resource_root->native_assets(),
				opennova::to_std(file_name)))
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
	if (!kernel_->load_ammo_table(files, opennova::to_std(file_name)))
		return ERR_FILE_NOT_FOUND;
	return OK;
}
