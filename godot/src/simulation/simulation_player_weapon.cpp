// Simulation — the LOCAL PLAYER equipped-weapon binding. The pump, the
// install bake, the clip rings, the UseGun borrow, PowerThrow, and the
// presentation-event queue moved into engine/runtime/world (S7a, ADR 0028:
// world/player_weapon.h); this TU marshals installs and inputs, drains the
// event queue, snapshots the per-frame state dictionary, and routes the two
// wire request records (net-re §5.62).
#include "simulation/simulation_internal.h"
#include "simulation/player_aim_overlay.h"
#include "simulation/player_weapon_event.h"
#include "simulation/player_weapon_view.h"
#include "object/weapon_def.h" // the typed weapon.def row the GUT install seams hand over
#include "util/string_convert.h"

#include <formats/def/def.h> // the weapon.def flag mirrors pinned below

#include <cstdio>

using namespace sim_internal;
using namespace opennova::def;

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

void Simulation::request_local_player_weapon_category(WeaponCategory p_category) {
	if (kernel_->local.view.binoculars_view_active) return;
	// [orig: input cases 200-210 @ 0x4e1144 -> Player_SwitchToWeaponByHandle
	//  ((action-200)*65). The binoculars-view and fire-charge input gates have no
	//  sim mechanics yet — record note.]
	if (kernel_->local.weapon.usegun_switch != LocalUseGunSwitch::kNone) return;
	if (!kernel_->local.inventory_valid) return;
	if (p_category < 0 || p_category >= opennova::world::weapon_combo::kCategories)
		return;
	handle_weapon_switch_outcome(opennova::world::weapon_switch_to_handle(
			kernel_->world.tables.weapons, kernel_->local.inventory,
			p_category * opennova::world::weapon_combo::kRanksPerCategory,
			local_weapon_switch_gates()));
}

void Simulation::request_local_player_weapon_cycle(int p_direction) {
	// The next/prev-weapon actions (212/214) are dual-purpose: the engine's
	// dispatcher leg (runtime/world/local_player_view.h
	// local_player_weapon_cycle_route) refuses them while the binocular view
	// is up or a PowerThrow charge is live, steps the scope zoom by +/-2 in
	// place of a cycle while the optical view is up on a def whose
	// scope_min_mag differs from scope_max_mag, and otherwise hands the cycle
	// (weapon_cycle_slot) back to this walk. The def's scope_min_mag rides
	// from the kernel's retained weapon.def row; a shell-installed row with no
	// retained parse takes the engine's record default.
	const DefWeaponDef *row = native_equipped_weapon_row();
	const int32_t scope_min_mag = row != nullptr
			? row->scope_min_mag
			: opennova::world::ScopeZoomLimits{}.scope_min_mag;
	const opennova::world::ScopeZoomLimits limits =
			opennova::world::local_player_scope_zoom_limits(
					kernel_->world, kernel_->local.weapon, scope_min_mag);
	switch (opennova::world::local_player_weapon_cycle_route(kernel_->world,
			kernel_->local.weapon, kernel_->local.view, limits, p_direction)) {
		case opennova::world::WeaponCycleRoute::kRefused:
		case opennova::world::WeaponCycleRoute::kZoomStep:
			return;
		case opennova::world::WeaponCycleRoute::kCycle:
			break;
	}
	if (kernel_->local.weapon.usegun_switch != LocalUseGunSwitch::kNone) return;
	if (!kernel_->local.inventory_valid) return;
	handle_weapon_switch_outcome(opennova::world::weapon_cycle_slot(
			kernel_->world.tables.weapons, kernel_->local.inventory, p_direction,
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
	if (kernel_ == nullptr || p_adm_index <= 0 || p_adm_index > 0xFF) return String();
	const opennova::world::WeaponTableEntry *entry =
			kernel_->world.tables.weapons.by_index(static_cast<uint8_t>(p_adm_index));
	if (entry == nullptr) return String();
	return opennova::to_gd(entry->third_person_model);
}

Ref<PlayerAimOverlay> Simulation::get_local_player_aim_overlay() const {
	// The torso-bend overlay state: the nine per-segment orientations from the exact BAM
	// blends [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14],
	// converted once here to mission-euler degrees — yaw via the canonical (90 - heading),
	// pitch unchanged (the retail placement builder applies authored pitch as Ry(-pitch),
	// and MissionObjectPlacer performs the matching basis conjugation). The shell builds
	// Godot bases from these with that single-sourced conversion; delta(body class) is
	// identity by construction.
	if (!kernel_->world.cached.local_player.valid()) return Ref<PlayerAimOverlay>();
	const AiEntity *p = kernel_->world.ai.for_handle(kernel_->world.cached.local_player);
	const opennova::world::Entity *entity =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (!p || !entity) return Ref<PlayerAimOverlay>();

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
	Ref<PlayerAimOverlay> out;
	out.instantiate();
	out->set_state(in.aim_state, static_cast<int>(in.mount_mode), in.mount_config_valid,
			in.mount_config_valid ? in.mount_config : 0);
	// The THIRD-PERSON held weapon: its own attach basis, plus retail's draw gate.
	// The basis is not one of the nine classes above — see the anim contract.
	out->set_angles(mission_euler_from_overlay(angles[opennova::anim::kOverlayBody]), packed,
			mission_euler_from_overlay(opennova::anim::compute_held_weapon_attach_angles(in)));
	// Which of the two attach frames retail would use for this body — the same 0x80 test
	// on the weapon channel's hold state that the wire path publishes as
	// PF_HELD_WEAPON_HAND_FRAME, read here from our own infantry state so the local and
	// remote legs cannot drift. [orig: gate @ 0x4b21b6 / branch @ 0x4b220f]
	out->set_weapon(local_held_weapon_visible(*entity),
			(opennova::world::infantry_anim_flags(p->inf.wpn_state) & 0x80u) != 0);
	return out;
}

// The local-player branch of retail's held-weapon draw gate — the policy is
// world::local_held_weapon_visible [orig: Entity_CanFireWeapon @ 0x4dcb10,
// the `entityPtr == g_local_player_entity` branch @ 0x4dcbcf..0x4dcc5d];
// this wrapper supplies the sim's own state aggregates.
bool Simulation::local_held_weapon_visible(
		const opennova::world::Entity &p_entity) const {
	if (!kernel_) return false;
	return opennova::world::local_held_weapon_visible(kernel_->world, p_entity,
			kernel_->local.weapon, kernel_->local.inventory, kernel_->local.view.third_person);
}

// --- the local player's equipped-weapon FSM (net-re §5.62) --------------------------

void Simulation::set_local_player_weapon(const Ref<WeaponDef> &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, false);
}

opennova::world::WeaponInstallData Simulation::install_data_from_def(
		const DefWeaponDef &p_def, const Dictionary &p_clip_seconds) {
	// The row half is the engine's (the same builder the kernel's by-name
	// mount runs); only the clip-variant rings arrive from the shell's .adm
	// read, keyed by clip name.
	opennova::world::WeaponInstallData data =
			opennova::world::weapon_install_data_from_def(p_def);
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
				opennova::to_std(String(keys[i])),
				std::move(lengths));
	}
	return data;
}

// The production mount (S6b): the kernel finds the row in ITS retained
// weapon.def parse, bakes the FSM def and seeds the clip rings from the rig's
// own .adm through the installed asset index — one step at ACCEPT time, no
// shell dictionary and no render dependency [orig: the ACCEPT chain —
// WeaponSlotTable_LoadAllFromDefs @ 0x5414e0 + Player_MountWeaponSlot
// @ 0x4dfa40; Anim_InitActions @ 0x541fa0 bakes the delays].
bool Simulation::install_local_player_weapon_by_name(
		const String &p_weapon_name, bool p_preserve_slot_state) {
	return kernel_->install_weapon(
			opennova::to_std(p_weapon_name), p_preserve_slot_state);
}

void Simulation::rebake_local_player_weapon(const Ref<WeaponDef> &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, true);
}

void Simulation::install_local_player_weapon(const Ref<WeaponDef> &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state,
		bool p_allow_same_weapon_rebake) {
	if (!kernel_ || p_def.is_null()) return;
	opennova::world::local_weapon_install(kernel_->world, kernel_->local.weapon,
			install_data_from_def(p_def->value(), p_clip_seconds),
			p_preserve_slot_state, p_allow_same_weapon_rebake,
			kernel_->local.inventory_valid ? &kernel_->local.inventory : nullptr, kernel_->local.view);
}

void Simulation::clear_local_player_weapon() {
	opennova::world::local_weapon_clear(kernel_->local.weapon, kernel_->local.view);
}

void Simulation::set_local_player_first_person_model_available(bool p_available) {
	kernel_->local.weapon.first_person_model_adm = 0xFF;
	if (!p_available || !kernel_) return;
	const opennova::world::Entity *player =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (player != nullptr)
		kernel_->local.weapon.first_person_model_adm = player->equipped_adm_index;
}

bool Simulation::is_local_player_first_person_model_available() const {
	return kernel_ && kernel_->local.weapon.first_person_model_adm != 0xFF;
}

void Simulation::set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
		bool p_reload_pressed) {
	// This is the ONE funnel both roles feed (apply_frame_input, once per tick),
	// so the F3 Weapon window's hold is OR'd in here rather than raced against
	// the shell's own per-frame write.
	opennova::world::local_weapon_set_input(kernel_->local.weapon, kernel_->local.view,
			p_fire_held || player_.debug_weapon_fire_held, p_fire_pressed, p_reload_pressed);
}

Ref<PlayerWeaponView> Simulation::get_local_player_weapon_state() const {
	Ref<PlayerWeaponView> out;
	out.instantiate();
	out->assign(opennova::world::local_player_weapon_view(
			kernel_->world, kernel_->local.weapon, kernel_->local.inventory));
	return out;
}

TypedArray<PlayerWeaponEvent> Simulation::drain_local_player_weapon_events() {
	TypedArray<PlayerWeaponEvent> out;
	const uint32_t now = kernel_ ? kernel_->world.logic_tick : 0;
	for (const opennova::world::WeaponPresentationEvent &event :
			kernel_->local.weapon.events) {
		Ref<PlayerWeaponEvent> row;
		row.instantiate();
		row->assign(event, now);
		out.push_back(row);
	}
	kernel_->local.weapon.events.clear();
	return out;
}


// =========================================================================
// The F3 Weapon window's native seams (devtools; ADR 0042 d6). Records out as
// engine types, edits in through the engine's own weapon seams. Every trigger
// takes the REAL input path and its real gate — nothing here fakes ammo, and
// nothing here writes a file.
// =========================================================================

namespace {

namespace wa = opennova::world::weapon_action;

// The retained weapon.def ACTION row bound to a slot, matched the way the bake
// itself binds it: by the slot's suffix name, case-insensitively (the witnessed
// rule and its citation live on weapon_fsm_bake, engine/runtime/world/weapon_fsm.h).
// The retained weapon.def entry for a weapon name, or null.
DefWeaponDef *find_weapon_row(DefWeaponsFile &file, const std::string &name) {
	if (name.empty()) return nullptr;
	for (size_t i = 0; i < file.count; ++i) {
		if (opennova::strutil::iequals(file.entries[i].weapon_name, name)) {
			return &file.entries[i];
		}
	}
	return nullptr;
}

DefWeaponAction *find_action_row(DefWeaponDef *row, int action_id) {
	if (row == nullptr || action_id < 0 || action_id >= wa::kCount) return nullptr;
	const char *suffix = opennova::world::kWeaponActionSuffixes[action_id];
	for (size_t i = 0; i < row->actions_count; ++i) {
		if (opennova::strutil::iequals(row->actions[i].name, suffix)) {
			return &row->actions[i];
		}
	}
	return nullptr;
}

}  // namespace

const opennova::world::LocalPlayerWeapon *Simulation::native_local_player_weapon() const {
	if (!kernel_ || !kernel_->local.weapon.active) return nullptr;
	return &kernel_->local.weapon;
}

const DefWeaponDef *Simulation::native_equipped_weapon_row() const {
	if (!kernel_ || !kernel_->weapon_defs_ok) return nullptr;
	// The const_cast is confined here: the finder is shared with the mutating
	// edits below, and the retained parse is this object's own member.
	return find_weapon_row(const_cast<DefWeaponsFile &>(kernel_->weapon_defs),
			kernel_->local.weapon.def_name);
}

int Simulation::native_equipped_weapon_adm_index() const {
	if (!kernel_ || kernel_->local.weapon.def_name.empty()) return -1;
	return kernel_->world.tables.weapons.index_of(kernel_->local.weapon.def_name.c_str());
}

const opennova::world::WeaponSlotState *Simulation::native_active_weapon_slot() const {
	if (!kernel_ || !kernel_->local.weapon.active) return nullptr;
	return opennova::world::active_local_weapon_slot(kernel_->world, kernel_->local.weapon);
}

const char *Simulation::native_weapon_input_block() const {
	if (!kernel_) return "no world";
	return opennova::world::local_weapon_input_block_name(
			opennova::world::local_weapon_input_block(kernel_->world, kernel_->local.weapon));
}

std::vector<std::string> Simulation::native_equipped_weapon_clip_keys() const {
	std::vector<std::string> out;
	if (!kernel_) return out;
	out.reserve(kernel_->local.weapon.clip_rings.size());
	for (const auto &entry : kernel_->local.weapon.clip_rings) {
		out.push_back(entry.first);
	}
	return out;
}

bool Simulation::debug_weapon_set_action_delays(int p_action_id, int p_delay_start,
		int p_delay_end, bool p_rebake) {
	if (!kernel_ || p_action_id < 0 || p_action_id >= wa::kCount) return false;
	opennova::world::LocalPlayerWeapon &weapon = kernel_->local.weapon;
	if (!weapon.active) return false;

	// Mirror into the retained row AS AUTHORED, so a re-install (an armory
	// accept, a respawn) keeps the edit and an untouched `auto` (-1) leg stays
	// `auto`.
	DefWeaponAction *row = find_action_row(
			kernel_->weapon_defs_ok
					? find_weapon_row(kernel_->weapon_defs, weapon.def_name)
					: nullptr,
			p_action_id);
	if (row != nullptr) {
		row->delaystart = p_delay_start;
		row->delayend = p_delay_end;
	}
	// Explicit legs patch the live slot with no re-bake, so dragging an edge
	// mid-burst never disturbs the running action; an `auto` leg keeps what
	// the clip last baked unless the edit asks for the re-bake that resolves a
	// newly-auto leg — the same-weapon path, which keeps the live slot, the
	// serials, the latches and the scope.
	opennova::world::WeaponFsmAction &baked = weapon.def.actions[p_action_id];
	if (p_delay_start >= 0) baked.delay_start = p_delay_start;
	if (p_delay_end >= 0) baked.delay_end = p_delay_end;
	if (p_rebake) {
		return row != nullptr &&
				kernel_->install_weapon(weapon.def_name, /*preserve_slot_state=*/true,
						/*allow_same_weapon_rebake=*/true);
	}
	return true;
}

bool Simulation::debug_weapon_set_action_text(int p_action_id, int p_field,
		const String &p_text) {
	if (!kernel_ || p_action_id < 0 || p_action_id >= wa::kCount) return false;
	opennova::world::LocalPlayerWeapon &weapon = kernel_->local.weapon;
	if (!weapon.active) return false;
	const CharString utf8 = p_text.utf8();
	const char *value = utf8.get_data() != nullptr ? utf8.get_data() : "";

	DefWeaponAction *row = find_action_row(
			kernel_->weapon_defs_ok
					? find_weapon_row(kernel_->weapon_defs, weapon.def_name)
					: nullptr,
			p_action_id);
	opennova::world::WeaponFsmAction &baked = weapon.def.actions[p_action_id];
	const auto write = [&](char *dst, size_t dst_size, char *row_dst, size_t row_size) {
		if (row_dst != nullptr) std::snprintf(row_dst, row_size, "%s", value);
		std::snprintf(dst, dst_size, "%s", value);
	};
	// p_field is devtools::WeaponRequest::TextField; the drain static_asserts
	// the pairing rather than this TU including a devtools header.
	switch (p_field) {
		case 0:  // Anim — re-resolves the clip, so any `auto` delay re-derives.
			if (row == nullptr) return false;
			std::snprintf(row->anim, sizeof(row->anim), "%s", value);
			return kernel_->install_weapon(weapon.def_name, /*preserve_slot_state=*/true,
					/*allow_same_weapon_rebake=*/true);
		case 1:
			write(baked.soundset, sizeof(baked.soundset), row ? row->soundset : nullptr,
					row ? sizeof(row->soundset) : 0);
			return true;
		case 2:
			write(baked.soundsetend, sizeof(baked.soundsetend), row ? row->soundsetend : nullptr,
					row ? sizeof(row->soundsetend) : 0);
			return true;
		case 3:
			write(baked.particle, sizeof(baked.particle), row ? row->particle : nullptr,
					row ? sizeof(row->particle) : 0);
			return true;
		case 4:
			write(baked.particle_userpoint, sizeof(baked.particle_userpoint),
					row ? row->particleuserpoint : nullptr,
					row ? sizeof(row->particleuserpoint) : 0);
			return true;
		default:
			return false;
	}
}

bool Simulation::debug_weapon_trigger(int p_trigger) {
	if (!kernel_) return false;
	opennova::world::LocalPlayerWeapon &weapon = kernel_->local.weapon;
	// The pump's own gate: input it would zero is refused here instead.
	if (opennova::world::local_weapon_input_block(kernel_->world, weapon) !=
			opennova::world::LocalWeaponInputBlock::kNone) {
		return false;
	}
	opennova::world::WeaponSlotState *slot =
			opennova::world::active_local_weapon_slot(kernel_->world, weapon);
	if (slot == nullptr) return false;
	// p_trigger is devtools::WeaponRequest::Trigger, paired at the drain.
	switch (p_trigger) {
		case 0:  // Fire: the press edge, latched until the pump consumes it.
			kernel_->local.set_weapon_input(weapon.fire_held, true, false);
			return true;
		case 1:  // Reload: refused exactly where the input dispatcher refuses it.
			if (!opennova::world::weapon_fsm_reload_allowed(weapon.def, *slot)) return false;
			kernel_->local.set_weapon_input(weapon.fire_held, false, true);
			return true;
		case 2:  // The ADS toggle request — the same seam the right button uses.
			request_local_player_scope_toggle();
			return true;
		case 3: request_local_player_weapon_cycle(1); return true;
		case 4: request_local_player_weapon_cycle(-1); return true;
		default: return false;
	}
}

void Simulation::debug_weapon_set_fire_held(bool p_held) { player_.debug_weapon_fire_held = p_held; }

void Simulation::debug_weapon_arm_trace(bool p_armed) {
	if (!kernel_) return;
	opennova::world::weapon_trace_arm(kernel_->local.weapon, p_armed);
}

void Simulation::debug_weapon_clear_trace() {
	if (!kernel_) return;
	opennova::world::weapon_trace_clear(kernel_->local.weapon);
}
