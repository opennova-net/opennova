#include "devtools/debug_control_table.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/devtools/debug_control_ids.h>

#include "env/weather.h"
#include "mission/mission_root.h"
#include "player/local_player_presenter.h"
#include "simulation/simulation.h"
#include "terrain/terrain.h"
#include "world/game_world.h"

#include <cmath>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace godot {

namespace control_id = opennova::devtools::control_id;

const char *const DebugControlTable::kReasonHostOnly =
		"Only the session host can change authoritative game state.";
const char *const DebugControlTable::kReasonConfirm =
		"This control changes authoritative state; pass confirm_authority=true on an authority-owning session.";

namespace {

// The marshalled positional Array carries typed Variants; the owner members
// take plain C++ parameters. Enums ride their integer value, floats their
// double.
template <typename T>
T from_variant(const Variant &p_value) {
	if constexpr (std::is_enum_v<T>) {
		return static_cast<T>(static_cast<int64_t>(p_value));
	} else if constexpr (std::is_same_v<T, float>) {
		return static_cast<float>(static_cast<double>(p_value));
	} else if constexpr (std::is_same_v<T, int>) {
		return static_cast<int>(static_cast<int64_t>(p_value));
	} else {
		return static_cast<T>(p_value);
	}
}

template <typename T>
Variant to_variant(T p_value) {
	if constexpr (std::is_enum_v<T>) {
		return Variant(static_cast<int64_t>(p_value));
	} else {
		return Variant(p_value);
	}
}

template <typename OwnerT, typename R, typename... P, std::size_t... I>
DebugControlTable::Outcome call_member(OwnerT *p_owner, R (OwnerT::*p_fn)(P...), const Array &p_args,
		std::index_sequence<I...>) {
	(void)p_args;
	if constexpr (std::is_void_v<R>) {
		(p_owner->*p_fn)(from_variant<std::decay_t<P>>(p_args[I])...);
		return DebugControlTable::Outcome{OK, Variant()};
	} else if constexpr (std::is_same_v<R, Error>) {
		return DebugControlTable::Outcome{(p_owner->*p_fn)(from_variant<std::decay_t<P>>(p_args[I])...),
			Variant()};
	} else {
		return DebugControlTable::Outcome{OK,
			to_variant((p_owner->*p_fn)(from_variant<std::decay_t<P>>(p_args[I])...))};
	}
}

template <typename OwnerT, typename R, typename A>
Error apply_write(OwnerT *p_owner, R (OwnerT::*p_set)(A), const Variant &p_value) {
	if constexpr (std::is_same_v<R, Error>) {
		return (p_owner->*p_set)(from_variant<std::decay_t<A>>(p_value));
	} else {
		(p_owner->*p_set)(from_variant<std::decay_t<A>>(p_value));
		return OK;
	}
}

DebugControlTable::Outcome outcome_error(Error p_error) {
	return DebugControlTable::Outcome{p_error, Variant()};
}

DebugControlTable::Outcome outcome_result(const Variant &p_result) {
	return DebugControlTable::Outcome{OK, p_result};
}

// A Vector3 argument inside the mission coordinate box (world/geom.h
// kMissionCoordMin/MaxUnits: the signed 16.16 carrier span).
Ref<DebugArgSpec> mission_position_arg(const String &p_name) {
	return DebugArgSpec::vector3(p_name)->between(Simulation::mission_coord_min(),
			Simulation::mission_coord_max());
}

// A positive entity SSN / mission group argument.
Ref<DebugArgSpec> positive_int_arg(const String &p_name) {
	return DebugArgSpec::integer(p_name)->at_least(1);
}

template <typename... Specs>
TypedArray<DebugArgSpec> args_of(const Specs &...p_specs) {
	TypedArray<DebugArgSpec> out;
	(out.push_back(p_specs), ...);
	return out;
}

PackedStringArray choices_of(std::initializer_list<const char *> p_choices) {
	PackedStringArray out;
	for (const char *choice : p_choices) {
		out.push_back(choice);
	}
	return out;
}

int audio_bus_index(const Variant &p_name) {
	return AudioServer::get_singleton()->get_bus_index(StringName(String(p_name)));
}

} // namespace

// --- construction ------------------------------------------------------------

DebugControlTable::DebugControlTable() {
	register_option_rows();
	register_terrain_rows();
	register_rendering_rows();
	register_edit_actions();
	register_audio_actions();
	register_runtime_rows();
	register_environment_rows();
	register_automation_actions();
	register_spectator_row();
}

void DebugControlTable::setup(const Ref<DebugShellHost> &p_host) {
	host_ = p_host;
}

void DebugControlTable::clear() {
	entries_.clear();
	host_.unref();
}

// --- the public surface --------------------------------------------------------

const DebugControlTable::Entry *DebugControlTable::find(const StringName &p_id) const {
	for (const Entry &entry : entries_) {
		if (entry.row->id_ == p_id) {
			return &entry;
		}
	}
	return nullptr;
}

DebugControlTable::Entry *DebugControlTable::find(const StringName &p_id) {
	for (Entry &entry : entries_) {
		if (entry.row->id_ == p_id) {
			return &entry;
		}
	}
	return nullptr;
}

Ref<DebugControlRow> DebugControlTable::control(const StringName &p_id) const {
	const Entry *entry = find(p_id);
	return entry != nullptr ? entry->row : Ref<DebugControlRow>();
}

TypedArray<StringName> DebugControlTable::row_ids() const {
	TypedArray<StringName> out;
	for (const Entry &entry : entries_) {
		out.push_back(entry.row->id_);
	}
	return out;
}

TypedArray<DebugControlRow> DebugControlTable::list_controls(const StringName &p_page,
		const String &p_filter, bool p_allow_authority) {
	TypedArray<DebugControlRow> out;
	const String needle = p_filter.strip_edges().to_lower();
	for (Entry &entry : entries_) {
		const Ref<DebugControlRow> &row = entry.row;
		if (p_page != StringName() && row->page_ != p_page) {
			continue;
		}
		if (!needle.is_empty()) {
			const String haystack = (String(row->id_) + " " + String(row->page_) + " " + row->label_ +
					" " + row->tooltip_).to_lower();
			if (!haystack.contains(needle)) {
				continue;
			}
		}
		out.push_back(row->with_state(state_of(entry, p_allow_authority)));
	}
	return out;
}

Ref<DebugControlState> DebugControlTable::get_state(const StringName &p_id, bool p_allow_authority) {
	Entry *entry = find(p_id);
	if (entry == nullptr) {
		Ref<DebugControlState> state;
		state.instantiate();
		state->id_ = p_id;
		state->reason_ = "Unknown debug control.";
		return state;
	}
	return state_of(*entry, p_allow_authority);
}

Ref<DebugControlState> DebugControlTable::state_of(const Entry &p_entry, bool p_allow_authority) {
	const Ref<DebugControlRow> &row = p_entry.row;
	Ref<DebugControlState> state;
	state.instantiate();
	state->id_ = row->id_;
	state->kind_ = row->kind_;
	state->desired_value_ = row->default_value_;
	const String reason = availability(row->target_);
	if (!reason.is_empty()) {
		state->value_ = row->default_value_;
		state->reason_ = reason;
		return state;
	}
	state->available_ = true;
	state->writable_ = write_allowed(p_entry, p_allow_authority);
	if (!state->writable_) {
		state->reason_ = policy_reason(p_entry, p_allow_authority);
	}
	if (row->kind_ == DebugControlRow::ACTION) {
		return state;
	}
	state->value_ = p_entry.read();
	state->desired_value_ = state->value_;
	state->authoritative_ = true;
	return state;
}

Error DebugControlTable::set_value(const StringName &p_id, const Variant &p_value,
		bool p_allow_authority) {
	Entry *entry = find(p_id);
	if (entry == nullptr) {
		return ERR_DOES_NOT_EXIST;
	}
	if (entry->row->kind_ == DebugControlRow::ACTION) {
		return ERR_INVALID_PARAMETER;
	}
	const Variant normalized = normalize_value(entry->row, p_value);
	if (normalized.get_type() == Variant::NIL) {
		return ERR_INVALID_PARAMETER;
	}
	if (!write_allowed(*entry, p_allow_authority)) {
		return ERR_UNAUTHORIZED;
	}
	return entry->write(normalized);
}

Ref<DebugInvokeResult> DebugControlTable::invoke(const StringName &p_id, const Array &p_args,
		bool p_allow_authority) {
	Entry *entry = find(p_id);
	if (entry == nullptr) {
		return invoke_result(ERR_DOES_NOT_EXIST, Variant(), p_id, p_allow_authority);
	}
	if (entry->row->kind_ != DebugControlRow::ACTION) {
		const Variant value = p_args.size() > 0 ? p_args[0] : Variant();
		return invoke_result(set_value(p_id, value, p_allow_authority), Variant(), p_id,
				p_allow_authority);
	}
	if (!write_allowed(*entry, p_allow_authority)) {
		return invoke_result(ERR_UNAUTHORIZED, Variant(), p_id, p_allow_authority);
	}
	const Ref<DebugMarshalResult> marshalled = DebugArgSpec::marshal(entry->row->args_, p_args);
	if (marshalled->is_refused()) {
		return invoke_result(ERR_INVALID_PARAMETER, Variant(), p_id, p_allow_authority);
	}
	const Outcome outcome = entry->invoke(marshalled->get_args());
	return invoke_result(outcome.error, outcome.result, p_id, p_allow_authority);
}

Ref<DebugMarshalResult> DebugControlTable::marshal_invoke_args(const StringName &p_id,
		const Variant &p_raw) const {
	const Entry *entry = find(p_id);
	if (entry == nullptr) {
		return DebugMarshalResult::refusal(vformat("Unknown debug control '%s'.", String(p_id)));
	}
	if (entry->row->kind_ != DebugControlRow::ACTION) {
		Array passthrough;
		if (p_raw.get_type() == Variant::ARRAY) {
			passthrough = p_raw;
		} else if (p_raw.get_type() != Variant::NIL) {
			passthrough.push_back(p_raw);
		}
		return DebugMarshalResult::accepted(passthrough);
	}
	const Ref<DebugMarshalResult> marshalled = DebugArgSpec::marshal(entry->row->args_, p_raw);
	if (marshalled->is_refused()) {
		return DebugMarshalResult::refusal(
				vformat("Debug action '%s' %s.", String(p_id), marshalled->get_reason()));
	}
	return marshalled;
}

Variant DebugControlTable::capture_snapshot(const String &p_filter, bool p_allow_authority) {
	Dictionary out;
	out["edit_unlocked"] = false;
	Array controls;
	const TypedArray<DebugControlRow> rows = list_controls(StringName(), p_filter, p_allow_authority);
	for (int i = 0; i < rows.size(); ++i) {
		const Ref<DebugControlRow> row = rows[i];
		controls.push_back(row->to_json_value());
	}
	out["controls"] = controls;
	return out;
}

Variant DebugControlTable::normalize_value(const Ref<DebugControlRow> &p_row, const Variant &p_value) {
	if (p_row.is_null()) {
		return Variant();
	}
	switch (p_row->kind_) {
		case DebugControlRow::CHECK:
			return p_value.get_type() == Variant::BOOL ? p_value : Variant();
		case DebugControlRow::SLIDER: {
			if (p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) {
				return Variant();
			}
			double number = p_value;
			if (!std::isfinite(number)) {
				return Variant();
			}
			number = Math::clamp(number, p_row->minimum_, p_row->maximum_);
			if (p_row->step_ > 0.0) {
				number = Math::snapped(number, p_row->step_);
			}
			return number;
		}
		case DebugControlRow::ENUM: {
			if (p_value.get_type() != Variant::INT) {
				return Variant();
			}
			const int64_t index = p_value;
			if (index < 0 || index >= p_row->choices_.size()) {
				return Variant();
			}
			return index;
		}
		case DebugControlRow::ACTION:
			break;
	}
	return Variant();
}

Ref<DebugInvokeResult> DebugControlTable::invoke_result(Error p_error, const Variant &p_result,
		const StringName &p_id, bool p_allow_authority) {
	Ref<DebugInvokeResult> out;
	out.instantiate();
	out->error_ = p_error;
	out->result_ = p_result;
	out->state_ = get_state(p_id, p_allow_authority);
	return out;
}

// --- the gates ------------------------------------------------------------------

bool DebugControlTable::has_host_authority() const {
	return host_.is_valid() && host_->has_debug_authority();
}

bool DebugControlTable::write_allowed(const Entry &p_entry, bool p_allow_authority) const {
	if (p_entry.row->requires_confirm_ && !p_allow_authority) {
		return false;
	}
	if (p_entry.row->authority_ == DebugControlRow::HOST_ONLY && !has_host_authority()) {
		return false;
	}
	return true;
}

String DebugControlTable::policy_reason(const Entry &p_entry, bool p_allow_authority) const {
	if (p_entry.row->authority_ == DebugControlRow::HOST_ONLY && !has_host_authority()) {
		return kReasonHostOnly;
	}
	if (p_entry.row->requires_confirm_ && !p_allow_authority) {
		return kReasonConfirm;
	}
	return String();
}

// --- live owner resolution (per call, never retained) ----------------------------

GameWorld *DebugControlTable::world() const {
	if (host_.is_null()) {
		return nullptr;
	}
	GameWorld *value = host_->world();
	return (value != nullptr && value->is_loaded()) ? value : nullptr;
}

MissionRoot *DebugControlTable::runtime() const {
	return host_.is_valid() ? host_->runtime() : nullptr;
}

Simulation *DebugControlTable::sim() const {
	MissionRoot *value = runtime();
	return value != nullptr ? value->get_sim().ptr() : nullptr;
}

LocalPlayerPresenter *DebugControlTable::player() const {
	return host_.is_valid() ? host_->player_presenter() : nullptr;
}

Terrain *DebugControlTable::terrain() const {
	GameWorld *value = world();
	return value != nullptr ? value->get_terrain_node() : nullptr;
}

Weather *DebugControlTable::weather() const {
	GameWorld *value = world();
	return value != nullptr ? value->get_weather_node() : nullptr;
}

Viewport *DebugControlTable::viewport() const {
	return host_.is_valid() ? host_->viewport() : nullptr;
}

String DebugControlTable::availability(Target p_target) const {
	switch (p_target) {
		case DebugControlRow::TARGET_WORLD:
			return world() != nullptr ? String() : "No game world is loaded.";
		case DebugControlRow::TARGET_PLAYER:
			return player() != nullptr ? String() : "No local player presenter is active.";
		case DebugControlRow::TARGET_SIM:
			return sim() != nullptr ? String() : "No simulation is active.";
		case DebugControlRow::TARGET_TERRAIN:
			return terrain() != nullptr ? String() : "The current world has no terrain.";
		case DebugControlRow::TARGET_VIEWPORT:
			return viewport() != nullptr ? String() : "No render viewport is available.";
		case DebugControlRow::TARGET_GAME_SHELL:
			return host_.is_valid() ? String() : "The game shell is not available.";
		case DebugControlRow::TARGET_ENVIRONMENT:
			return world() != nullptr ? String() : "The current world has no environment.";
		case DebugControlRow::TARGET_WEATHER:
			return weather() != nullptr ? String() : "The current world has no weather controller.";
	}
	return "Unknown debug owner surface.";
}

// --- row registration -----------------------------------------------------------

DebugControlTable::Entry &DebugControlTable::add(Kind p_kind, const char *p_id, const char *p_page,
		const String &p_label, const String &p_tooltip, Target p_target, Owner p_owner) {
	Ref<DebugControlRow> row;
	row.instantiate();
	row->id_ = StringName(p_id);
	row->page_ = StringName(p_page);
	row->label_ = p_label;
	row->tooltip_ = p_tooltip;
	row->kind_ = p_kind;
	row->target_ = p_target;
	row->owner_ = p_owner;
	Entry entry;
	entry.row = row;
	entries_.push_back(std::move(entry));
	return entries_.back();
}

DebugControlTable::Entry &DebugControlTable::check(const char *p_id, const char *p_page,
		const String &p_label, const String &p_tooltip, Target p_target, Owner p_owner) {
	Entry &entry = add(DebugControlRow::CHECK, p_id, p_page, p_label, p_tooltip, p_target, p_owner);
	entry.row->default_value_ = false;
	return entry;
}

DebugControlTable::Entry &DebugControlTable::slider(const char *p_id, const char *p_page,
		const String &p_label, const String &p_tooltip, Target p_target, Owner p_owner,
		double p_default, double p_minimum, double p_maximum, double p_step) {
	Entry &entry = add(DebugControlRow::SLIDER, p_id, p_page, p_label, p_tooltip, p_target, p_owner);
	entry.row->default_value_ = p_default;
	entry.row->minimum_ = p_minimum;
	entry.row->maximum_ = p_maximum;
	entry.row->step_ = p_step;
	return entry;
}

DebugControlTable::Entry &DebugControlTable::enum_row(const char *p_id, const char *p_page,
		const String &p_label, const String &p_tooltip, Target p_target, Owner p_owner,
		int p_default, const PackedStringArray &p_choices) {
	Entry &entry = add(DebugControlRow::ENUM, p_id, p_page, p_label, p_tooltip, p_target, p_owner);
	entry.row->default_value_ = static_cast<int64_t>(p_default);
	entry.row->choices_ = p_choices;
	return entry;
}

DebugControlTable::Entry &DebugControlTable::action(const char *p_id, const char *p_page,
		const String &p_label, const String &p_tooltip, Target p_target, Owner p_owner,
		const TypedArray<DebugArgSpec> &p_args) {
	Entry &entry = add(DebugControlRow::ACTION, p_id, p_page, p_label, p_tooltip, p_target, p_owner);
	entry.row->args_ = p_args;
	return entry;
}

void DebugControlTable::authoritative(Entry &p_entry) {
	p_entry.row->requires_confirm_ = true;
	p_entry.row->authority_ = DebugControlRow::HOST_ONLY;
}

template <typename OwnerT, typename Get, typename Set>
void DebugControlTable::bind_value(Entry &p_entry, OwnerT *(DebugControlTable::*p_resolve)() const,
		Get p_get, Set p_set) {
	p_entry.read = [this, p_resolve, p_get]() -> Variant {
		OwnerT *owner = (this->*p_resolve)();
		return owner != nullptr ? to_variant((owner->*p_get)()) : Variant();
	};
	p_entry.write = [this, p_resolve, p_set](const Variant &p_value) -> Error {
		OwnerT *owner = (this->*p_resolve)();
		if (owner == nullptr) {
			return ERR_UNAVAILABLE;
		}
		return apply_write(owner, p_set, p_value);
	};
}

template <typename OwnerT, typename R, typename... P>
void DebugControlTable::bind_action(Entry &p_entry, OwnerT *(DebugControlTable::*p_resolve)() const,
		R (OwnerT::*p_fn)(P...)) {
	p_entry.invoke = [this, p_resolve, p_fn](const Array &p_args) -> Outcome {
		OwnerT *owner = (this->*p_resolve)();
		if (owner == nullptr) {
			return outcome_error(ERR_UNAVAILABLE);
		}
		return call_member(owner, p_fn, p_args, std::index_sequence_for<P...>{});
	};
}

// The world and player-camera debug toggles (the retired DebugOptions
// registry, now typed device rows). Without a live world they report
// unavailable: there is no recorded intent and no replay.
void DebugControlTable::register_option_rows() {
	bind_value(check(control_id::kHideFoliage, "Terrain", "Hide foliage",
					   "Hide the scattered vegetation (grass / bushes / trees) to see the terrain under it.",
					   DebugControlRow::TARGET_WORLD, DebugControlRow::OWNER_DEVICE),
			&DebugControlTable::world, &GameWorld::is_foliage_hidden, &GameWorld::set_foliage_hidden);
	bind_value(check(control_id::kHideParticles, "Particles", "Hide particles",
					   "Hide every particle effect (the retail master particle switch) — flip it to check whether an artifact is particles at all.",
					   DebugControlRow::TARGET_WORLD, DebugControlRow::OWNER_DEVICE),
			&DebugControlTable::world, &GameWorld::is_particles_hidden, &GameWorld::set_particles_hidden);
	bind_value(check(control_id::kForceFpArms, "Player", "Always draw FP arms",
					   "Keep the first-person arms + weapon drawn in every camera mode (debug experiment).",
					   DebugControlRow::TARGET_PLAYER, DebugControlRow::OWNER_DEVICE),
			&DebugControlTable::player, &LocalPlayerPresenter::is_debug_force_viewmodel,
			&LocalPlayerPresenter::set_debug_force_viewmodel);
	bind_value(check(control_id::kBodyInFirstPerson, "Player", "Show body in first person",
					   "Draw your own body in first person — look down to see your legs and feet (debug experiment; expect the head/shoulders to clip the camera).",
					   DebugControlRow::TARGET_PLAYER, DebugControlRow::OWNER_DEVICE),
			&DebugControlTable::player, &LocalPlayerPresenter::is_debug_body_in_first_person,
			&LocalPlayerPresenter::set_debug_body_in_first_person);
	bind_value(check(control_id::kThirdPersonOnFoot, "Player", "Third person on foot",
					   "Force the chase camera while on foot. Stock JO only resolves third person in a vehicle control seat with Chase View (F4) selected — the per-frame arbiter, net-re §5.39 — so this is the onhook debug patch's affordance, not a gameplay key.",
					   DebugControlRow::TARGET_PLAYER, DebugControlRow::OWNER_DEVICE),
			&DebugControlTable::player, &LocalPlayerPresenter::is_debug_third_person,
			&LocalPlayerPresenter::set_debug_third_person);
}

void DebugControlTable::register_terrain_rows() {
	bind_value(enum_row(control_id::kTerrainDrawMode, "Terrain", "Draw mode",
					   "Color terrain by renderer decisions instead of textures.",
					   DebugControlRow::TARGET_TERRAIN, DebugControlRow::OWNER_ENGINE, 0,
					   choices_of({"Normal", "Detail levels", "Sector colors", "Surface angle", "Height map"})),
			&DebugControlTable::terrain, &Terrain::get_debug_mode, &Terrain::set_debug_mode);
	bind_value(slider(control_id::kTerrainLodQuality, "Terrain", "Terrain detail",
					   "Scale how aggressively terrain refines toward the camera.",
					   DebugControlRow::TARGET_TERRAIN, DebugControlRow::OWNER_ENGINE, 1.0, 0.1, 4.0, 0.1),
			&DebugControlTable::terrain, &Terrain::get_lod_quality, &Terrain::set_lod_quality);
	const auto terrain_check = [this](const char *p_id, const String &p_label, const String &p_tooltip,
									   bool (Terrain::*p_get)() const, void (Terrain::*p_set)(bool)) {
		bind_value(check(p_id, "Terrain", p_label, p_tooltip, DebugControlRow::TARGET_TERRAIN,
						   DebugControlRow::OWNER_ENGINE),
				&DebugControlTable::terrain, p_get, p_set);
	};
	terrain_check(control_id::kTerrainNoFrustum, "Disable all culling",
			"Show terrain patches that the camera would normally reject.",
			&Terrain::get_debug_no_frustum, &Terrain::set_debug_no_frustum);
	terrain_check(control_id::kTerrainNoNearfar, "Disable distance culling",
			"Ignore the camera's near and far terrain planes.",
			&Terrain::get_debug_no_nearfar, &Terrain::set_debug_no_nearfar);
	terrain_check(control_id::kTerrainNoSideplanes, "Disable side-plane culling",
			"Ignore the left, right, top and bottom terrain planes.",
			&Terrain::get_debug_no_sideplanes, &Terrain::set_debug_no_sideplanes);
	terrain_check(control_id::kTerrainNoPartialSubdiv, "Disable partial subdivision",
			"Require complete terrain subdivision decisions.",
			&Terrain::get_debug_no_partial_subdiv, &Terrain::set_debug_no_partial_subdiv);
	terrain_check(control_id::kTerrainForceLeaves, "Force leaf patches",
			"Render only terrain quadtree leaves.",
			&Terrain::get_debug_force_leaves, &Terrain::set_debug_force_leaves);
	terrain_check(control_id::kTerrainForceLod0, "Force highest detail",
			"Force terrain patches to the highest available detail level.",
			&Terrain::get_debug_force_lod0, &Terrain::set_debug_force_lod0);
}

void DebugControlTable::register_rendering_rows() {
	bind_value(enum_row(control_id::kViewportDebugDraw, "Rendering", "Viewport view",
					   "Show the renderer's built-in diagnostic buffers.",
					   DebugControlRow::TARGET_VIEWPORT, DebugControlRow::OWNER_DEVICE, 0,
					   choices_of({"Normal", "Unshaded", "Lighting only", "Overdraw", "Wireframe",
							   "Normal buffer", "Voxel GI albedo", "Voxel GI lighting", "Voxel GI emission",
							   "Shadow atlas", "Directional shadow atlas", "Scene luminance", "SSAO", "SSIL",
							   "PSSM splits", "Decal atlas", "SDFGI", "SDFGI probes", "GI buffer",
							   "Disable LOD", "Cluster omni lights", "Cluster spot lights", "Cluster decals",
							   "Cluster reflection probes", "Occluders", "Motion vectors",
							   "Internal buffer"})),
			&DebugControlTable::viewport, &Viewport::get_debug_draw, &Viewport::set_debug_draw);
	bind_value(check(control_id::kOcclusionCulling, "Rendering", "Godot occlusion culling",
					   "Run Godot's occluder pass over the world viewport: the conservative second layer under the retail portal verdict, off by default (it cost ~0.6 ms of render CPU per frame and culled nothing at the measured poses). It only has something to cull with while the loaded mission placed authored OOBJ occluders (game_render_diagnostics mission_placement.authored_occluder_models counts them). Flip it to weigh the pass against what it culls (render-occlusion-re.md, Conservative device occluders).",
					   DebugControlRow::TARGET_WORLD, DebugControlRow::OWNER_DEVICE),
			&DebugControlTable::world, &GameWorld::is_occlusion_culling_enabled,
			&GameWorld::set_occlusion_culling_enabled);
}

void DebugControlTable::register_edit_actions() {
	Entry &teleport = action(control_id::kTeleportLocalPlayer, "Player", "Teleport player",
			"Move the local player to a mission-space position.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(mission_position_arg("position"),
					DebugArgSpec::number("yaw_deg")
							->between(-kTeleportYawLimitDeg, kTeleportYawLimitDeg)
							->optional(0.0),
					DebugArgSpec::number("pitch_deg")
							->between(-kTeleportPitchLimitDeg, kTeleportPitchLimitDeg)
							->optional(0.0)));
	authoritative(teleport);
	bind_action(teleport, &DebugControlTable::sim, &Simulation::debug_teleport_local_player);

	Entry &map_cycle = action(control_id::kCycleMapMode, "Player", "Cycle map mode",
			"Step the M-key map cycle: off -> window -> fullscreen -> off.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE);
	map_cycle.row->requires_confirm_ = true;
	bind_action(map_cycle, &DebugControlTable::sim, &Simulation::request_hud_map_cycle);

	Entry &health = action(control_id::kSetEntityHealth, "Entities", "Set health",
			"Set the selected simulation entity's health.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(DebugArgSpec::integer("entity")->at_least(0),
					DebugArgSpec::integer("health")->between(Simulation::ENTITY_HEALTH_MIN,
							Simulation::ENTITY_HEALTH_MAX)));
	authoritative(health);
	bind_action(health, &DebugControlTable::sim, &Simulation::debug_set_entity_health);

	Entry &position = action(control_id::kSetEntityPosition, "Entities", "Move entity",
			"Move the selected simulation entity to a mission-space position.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(DebugArgSpec::integer("entity")->at_least(0), mission_position_arg("position")));
	authoritative(position);
	bind_action(position, &DebugControlTable::sim, &Simulation::debug_set_entity_position);

	// [wire_handle, attrib, attrib2]: a packed engine handle below the invalid
	// sentinel and two unsigned 32-bit words.
	Entry &item_attrib = action(control_id::kSetEntityItemAttrib, "Entities", "Set item attribs",
			"Write both items.def attrib words on one entity by its wire_handle (brainless rows included); a per-entity override the next item-traits sweep re-stamps.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(DebugArgSpec::integer("entity")->between(0, 0xFFFE),
					DebugArgSpec::integer("attrib")->between(0, 4294967295.0),
					DebugArgSpec::integer("attrib2")->between(0, 4294967295.0)));
	authoritative(item_attrib);
	bind_action(item_attrib, &DebugControlTable::sim, &Simulation::debug_set_entity_item_attrib);
}

// Process-local audio knobs shared by F3 and runtime MCP: the AudioServer is
// the device owner, the bus name resolves per call.
void DebugControlTable::register_audio_actions() {
	Entry &volume = action(control_id::kSetAudioBusVolume, "Audio", "Set bus volume",
			"Set one named audio bus volume in decibels.",
			DebugControlRow::TARGET_GAME_SHELL, DebugControlRow::OWNER_DEVICE,
			args_of(DebugArgSpec::text("bus"),
					DebugArgSpec::number("volume_db")
							->between(AUDIO_BUS_VOLUME_MIN_DB, AUDIO_BUS_VOLUME_MAX_DB)));
	volume.invoke = [this](const Array &p_args) -> Outcome {
		if (host_.is_null()) {
			return outcome_error(ERR_UNAVAILABLE);
		}
		const int bus = audio_bus_index(p_args[0]);
		if (bus < 0) {
			return outcome_error(ERR_INVALID_PARAMETER);
		}
		AudioServer::get_singleton()->set_bus_volume_db(bus, from_variant<float>(p_args[1]));
		return outcome_error(OK);
	};

	const auto bus_flag = [this](const char *p_id, const String &p_label, const String &p_tooltip,
								  const char *p_arg, void (AudioServer::*p_set)(int32_t, bool)) {
		Entry &entry = action(p_id, "Audio", p_label, p_tooltip, DebugControlRow::TARGET_GAME_SHELL,
				DebugControlRow::OWNER_DEVICE,
				args_of(DebugArgSpec::text("bus"), DebugArgSpec::boolean(p_arg)));
		entry.invoke = [this, p_set](const Array &p_args) -> Outcome {
			if (host_.is_null()) {
				return outcome_error(ERR_UNAVAILABLE);
			}
			const int bus = audio_bus_index(p_args[0]);
			if (bus < 0) {
				return outcome_error(ERR_INVALID_PARAMETER);
			}
			(AudioServer::get_singleton()->*p_set)(bus, from_variant<bool>(p_args[1]));
			return outcome_error(OK);
		};
	};
	bus_flag(control_id::kSetAudioBusMute, "Set bus mute", "Set one named audio bus mute state.",
			"muted", &AudioServer::set_bus_mute);
	bus_flag(control_id::kSetAudioBusSolo, "Set bus solo", "Set one named audio bus solo state.",
			"soloed", &AudioServer::set_bus_solo);
	bus_flag(control_id::kSetAudioBusBypass, "Set bus effect bypass",
			"Set one named audio bus effect bypass state.", "bypassed",
			&AudioServer::set_bus_bypass_effects);
}

void DebugControlTable::register_runtime_rows() {
	// The runtime's own transport (MissionRoot): pause and step report the
	// engine session's own refusal for multiplayer roles (the network pump
	// must keep running); resume is the shell's resume leg, which also closes
	// a pause overlay (the in-game menu, the armory) left up.
	Entry &transport = action(control_id::kRuntimeTransport, "Sim", "Runtime transport",
			"Resume, pause, or single-step the real game runtime.",
			DebugControlRow::TARGET_GAME_SHELL, DebugControlRow::OWNER_ENGINE,
			args_of(DebugArgSpec::text("action")->one_of(choices_of({"resume", "pause", "step"}))));
	authoritative(transport);
	transport.invoke = [this](const Array &p_args) -> Outcome {
		MissionRoot *value = runtime();
		if (host_.is_null() || value == nullptr) {
			return outcome_error(ERR_UNAVAILABLE);
		}
		const String verb = p_args[0];
		if (verb == "pause") {
			return outcome_error(value->pause() ? OK : ERR_UNAVAILABLE);
		}
		if (verb == "resume") {
			return outcome_error(host_->resume());
		}
		if (verb == "step") {
			return outcome_error(value->step_once() ? OK : ERR_UNAVAILABLE);
		}
		return outcome_error(ERR_INVALID_PARAMETER);
	};

	// Leaving is the shell's gated return leg, shared with MCP's game_control,
	// without classifying leaving a multiplayer session as an authoritative
	// world mutation.
	Entry &return_to_menu = action(control_id::kRuntimeReturnToMenu, "Sim", "Return to menu",
			"Leave the current world locally and return to the game menu.",
			DebugControlRow::TARGET_GAME_SHELL, DebugControlRow::OWNER_DEVICE);
	return_to_menu.invoke = [this](const Array &) -> Outcome {
		if (host_.is_null()) {
			return outcome_error(ERR_UNAVAILABLE);
		}
		return outcome_error(host_->return_to_menu());
	};

	Entry &scripts_paused = check(control_id::kRuntimeWacPaused, "Sim", "Pause mission scripts",
			"Pause WAC scripts while the rest of the world continues.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE);
	authoritative(scripts_paused);
	bind_value(scripts_paused, &DebugControlTable::sim, &Simulation::is_wac_paused,
			&Simulation::set_wac_paused);

	Entry &mission_variable = action(control_id::kSetMissionVariable, "Vars", "Set mission variable",
			"Set one live V0..V511 mission-script variable.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(DebugArgSpec::integer("index")->between(0, Simulation::MISSION_VAR_COUNT - 1),
					DebugArgSpec::integer("value")->between(kMissionVarValueMin, kMissionVarValueMax)));
	authoritative(mission_variable);
	bind_action(mission_variable, &DebugControlTable::sim, &Simulation::set_mission_variable);
}

// The Environment rows: the hosted mission clock (GameWorld coordinates the
// weather/audio consumers so a paused scrub is an immediate visible change),
// the wind and lightning on the Weather node, the WAC weather commands and
// the weather-home read, each on the Weather node's command seam — the bound
// Simulation's command layer on a mission, the standalone home otherwise.
// Args mirror the WAC signatures.
void DebugControlTable::register_environment_rows() {
	Entry &time_of_day = slider(control_id::kEnvironmentTimeOfDay, "Environment", "Time of day",
			"Scrub the mission clock by minute of day (0 = 00:00, 1439 = 23:59).",
			DebugControlRow::TARGET_ENVIRONMENT, DebugControlRow::OWNER_ENGINE, 720.0, 0.0, 1439.0,
			1.0);
	authoritative(time_of_day);
	bind_value(time_of_day, &DebugControlTable::world, &GameWorld::get_debug_mission_minute_of_day,
			&GameWorld::debug_set_mission_minute_of_day);

	Entry &wind = slider(control_id::kEnvironmentWindStrength, "Environment", "Wind strength",
			"Scale the wind driving foliage sway and weather gusts.",
			DebugControlRow::TARGET_WEATHER, DebugControlRow::OWNER_ENGINE, 100.0, 0.0, 100.0, 1.0);
	authoritative(wind);
	bind_value(wind, &DebugControlTable::weather, &Weather::get_wind_strength, &Weather::set_wind_strength);

	Entry &lightning_short = action(control_id::kEnvironmentLightningShort, "Environment",
			"Lightning (short)", "Trigger the weather controller's short lightning strike.",
			DebugControlRow::TARGET_WEATHER, DebugControlRow::OWNER_ENGINE);
	authoritative(lightning_short);
	bind_action(lightning_short, &DebugControlTable::weather, &Weather::trigger_lightning_short);

	Entry &lightning_long = action(control_id::kEnvironmentLightningLong, "Environment",
			"Lightning (long)", "Trigger the weather controller's long lightning strike.",
			DebugControlRow::TARGET_WEATHER, DebugControlRow::OWNER_ENGINE);
	authoritative(lightning_long);
	bind_action(lightning_long, &DebugControlTable::weather, &Weather::trigger_lightning_long);

	// One authoritative WAC weather command row over the resolved Weather
	// owner with the marshalled args.
	const auto weather_command = [this](const char *p_id, const String &p_label,
										 const String &p_tooltip, const TypedArray<DebugArgSpec> &p_args,
										 auto p_command) {
		Entry &entry = action(p_id, "Environment", p_label, p_tooltip, DebugControlRow::TARGET_WEATHER,
				DebugControlRow::OWNER_ENGINE, p_args);
		authoritative(entry);
		bind_action(entry, &DebugControlTable::weather, p_command);
	};
	const TypedArray<DebugArgSpec> percent_seconds =
			args_of(DebugArgSpec::integer("percent"), DebugArgSpec::integer("seconds"));
	weather_command(control_id::kEnvironmentRain, "Rain",
			"rain(percent, seconds): rain percent over a transition.", percent_seconds,
			&Weather::command_rain);
	weather_command(control_id::kEnvironmentSnow, "Snow",
			"snow(percent, seconds): snow percent over a transition.", percent_seconds,
			&Weather::command_snow);
	weather_command(control_id::kEnvironmentOvercast, "Overcast",
			"overcast(percent, seconds): overcast blend over a transition.", percent_seconds,
			&Weather::command_overcast);
	weather_command(control_id::kEnvironmentFogDistance, "Fog distance",
			"fogdist(metres): the fog distance target (2 m .. the 1024 m reference).",
			args_of(DebugArgSpec::integer("metres")), &Weather::command_fog_distance);
	weather_command(control_id::kEnvironmentMoveFog, "Move fog",
			"movefog(metres, seconds): the fog distance target over a transition.",
			args_of(DebugArgSpec::integer("metres"), DebugArgSpec::integer("seconds")),
			&Weather::command_move_fog);
	weather_command(control_id::kEnvironmentSkySpeed, "Sky speed",
			"skyspeed(rate): the cloud scroll rate target.", args_of(DebugArgSpec::integer("rate")),
			&Weather::command_sky_speed);
	weather_command(control_id::kEnvironmentQuake, "Quake",
			"quake(seconds): the earthquake jitter duration.",
			args_of(DebugArgSpec::integer("seconds")), &Weather::command_quake);
	weather_command(control_id::kEnvironmentFogType, "Fog type", "fogtype(type): the fog model 0..3.",
			args_of(DebugArgSpec::integer("type")), &Weather::command_fog_type);
	// The remaining WAC weather handlers the F3 Environment strip drives
	// (ADR 0043 d12: one table for both surfaces).
	weather_command(control_id::kEnvironmentSkyHeight, "Sky height",
			"skyheight(height): the sky dome height (raw 16.16 units).",
			args_of(DebugArgSpec::integer("height")), &Weather::command_sky_height);
	weather_command(control_id::kEnvironmentTimeOfDayMinutes, "Time of day (WAC)",
			"tod(minute): the WAC time-of-day command by minute of day (the script's clock math, unlike the exact environment_time_of_day scrub).",
			args_of(DebugArgSpec::integer("minute")), &Weather::command_time_of_day_minutes);
	weather_command(control_id::kEnvironmentSunFade, "Sun fade",
			"sunfade(percent, seconds): the sun fade target over a transition.", percent_seconds,
			&Weather::command_sun_fade);
	weather_command(control_id::kEnvironmentColorFade, "Color fade",
			"colorfade(seconds): the color-block fade duration.",
			args_of(DebugArgSpec::integer("seconds")), &Weather::command_color_fade);
	weather_command(control_id::kEnvironmentWindScale, "Wind scale",
			"wind(value): the `wind` named value (256 = the retail default, the ambient sway every map has).",
			args_of(DebugArgSpec::integer("value")), &Weather::command_wind_scale);
	weather_command(control_id::kEnvironmentBlockColor, "Color block",
			"sun/sky/ground/floor/ceiling/cloud/fogcolor/skyfogcolor/gain(r, g, b): one weather color block, by target (0 sun, 1 sky, 2 ground, 3 floor, 4 ceiling, 5 cloud, 6 fog, 7 skyfog, 8 gain) and packed 0xRRGGBB.",
			args_of(DebugArgSpec::integer("target")->between(0, 8),
					DebugArgSpec::integer("rgb")->between(0, 0xFFFFFF)),
			&Weather::command_weather_color);
	weather_command(control_id::kEnvironmentLightningColor, "Lightning color",
			"lightning(r, g, b): the lightning color, packed 0xRRGGBB.",
			args_of(DebugArgSpec::integer("rgb")->between(0, 0xFFFFFF)),
			&Weather::command_lightning_color);

	// The weather home as the render owner sees it: the WeatherState snapshot
	// plus the smoothed color blocks and the combined terrain light (the
	// precipitation diffuse) — a read, no authority needed.
	Entry &snapshot = action(control_id::kEnvironmentWeatherSnapshot, "Environment",
			"Weather snapshot",
			"Read the weather home: clock, springs, sequencers, the smoothed color blocks.",
			DebugControlRow::TARGET_WEATHER, DebugControlRow::OWNER_ENGINE);
	snapshot.invoke = [this](const Array &) -> Outcome {
		Weather *value = weather();
		if (value == nullptr) {
			return outcome_error(ERR_UNAVAILABLE);
		}
		Dictionary home = value->get_weather_snapshot();
		home["smooth_fill"] = value->get_smooth_fill();
		home["smooth_sun"] = value->get_smooth_sun();
		home["smooth_fog"] = value->get_smooth_fog();
		home["smooth_sky"] = value->get_smooth_sky();
		home["terrain_light_combined_rgb"] = value->get_terrain_light_combined_rgb();
		return outcome_result(home);
	};
}

// The controls scripted runs drive over MCP in place of the retired NW_*
// environment hooks (ADR 0041): the deploy pick, the viewmodel A/B rig, the
// joiner diagnostics trace, and the one-shot entity mutations the AI probe
// used to read from its env console.
void DebugControlTable::register_automation_actions() {
	Entry &deploy_pick = action(control_id::kDeployPick, "Sim", "Deploy pick",
			"Send one deployment pick (0 = the Default Spawn) while the deploy screen is owed; the host silently drops invalid or contested picks.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(DebugArgSpec::integer("zone")->at_least(0)->optional(static_cast<int64_t>(0))));
	deploy_pick.row->requires_confirm_ = true;
	bind_action(deploy_pick, &DebugControlTable::sim, &Simulation::send_deployment_pick);

	Entry &viewmodel = action(control_id::kSetViewmodelWeapon, "Player", "Set viewmodel weapon",
			"Rig the first-person viewmodel and action FSM to a weapon.def name (A/B against another SKU's def).",
			DebugControlRow::TARGET_WORLD, DebugControlRow::OWNER_DEVICE,
			args_of(DebugArgSpec::text("weapon")));
	viewmodel.row->requires_confirm_ = true;
	viewmodel.invoke = [this](const Array &p_args) -> Outcome {
		GameWorld *value = world();
		if (value == nullptr) {
			return outcome_error(ERR_UNAVAILABLE);
		}
		return outcome_result(value->set_local_player_weapon_by_name(from_variant<String>(p_args[0])));
	};

	Entry &clear_viewmodel = action(control_id::kClearViewmodelWeapon, "Player",
			"Clear viewmodel weapon", "Drop the equipped viewmodel (the armory NONE row).",
			DebugControlRow::TARGET_WORLD, DebugControlRow::OWNER_DEVICE);
	clear_viewmodel.row->requires_confirm_ = true;
	bind_action(clear_viewmodel, &DebugControlTable::world, &GameWorld::clear_local_player_weapon);

	Entry &diagnostics = check(control_id::kNetJoinerDiagnostics, "Net", "Joiner diagnostics",
			"Emit the per-second joiner freeze-tripwire trace (renders via print_verbose; run with --verbose).",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE);
	bind_value(diagnostics, &DebugControlTable::sim, &Simulation::is_joiner_network_diagnostics_enabled,
			&Simulation::set_joiner_network_diagnostics_enabled);

	Entry &kill_group = action(control_id::kKillGroup, "Entities", "Kill group",
			"Kill every live entity of a mission group; returns the count killed.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(positive_int_arg("group")));
	authoritative(kill_group);
	bind_action(kill_group, &DebugControlTable::sim, &Simulation::debug_kill_group);

	Entry &crew_vehicle = action(control_id::kCrewVehicle, "Entities", "Crew vehicle",
			"Seat an AI occupant (by SSN) into a vehicle (by SSN) as its pilot.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(positive_int_arg("occupant_ssn"), positive_int_arg("vehicle_ssn")));
	authoritative(crew_vehicle);
	bind_action(crew_vehicle, &DebugControlTable::sim, &Simulation::debug_crew_vehicle);

	Entry &crew_local = action(control_id::kCrewLocalPlayer, "Entities", "Crew local player",
			"Seat the local player into a vehicle (by SSN).",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(positive_int_arg("vehicle_ssn")));
	authoritative(crew_local);
	bind_action(crew_local, &DebugControlTable::sim, &Simulation::debug_crew_local_player);

	Entry &look = action(control_id::kLocalPlayerLook, "Player", "Local player look",
			"Feed one mouse-look delta (dx_px, dy_px screen pixels) through the local player's look path.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE,
			args_of(DebugArgSpec::number("dx_px"), DebugArgSpec::number("dy_px")));
	look.row->requires_confirm_ = true;
	bind_action(look, &DebugControlTable::sim, &Simulation::add_local_player_look);
}

void DebugControlTable::register_spectator_row() {
	Entry &spectator = check(control_id::kLocalSpectator, "Player", "Spectator free camera",
			"Detach the authority-owned local player from gameplay and unlock the free camera while the match continues ticking.",
			DebugControlRow::TARGET_SIM, DebugControlRow::OWNER_ENGINE);
	spectator.read = [this]() -> Variant {
		Simulation *value = sim();
		return value != nullptr ? Variant(value->is_local_spectator()) : Variant();
	};
	// The engine's own refusal (no local authority player) reads as
	// unauthorized: tooling cannot manufacture spectator authority.
	spectator.write = [this](const Variant &p_value) -> Error {
		Simulation *value = sim();
		if (value == nullptr) {
			return ERR_UNAVAILABLE;
		}
		return value->set_local_spectator(from_variant<bool>(p_value)) ? OK : ERR_UNAUTHORIZED;
	};
	authoritative(spectator);
}

// --- binding -------------------------------------------------------------------

void DebugControlTable::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "host"), &DebugControlTable::setup);
	ClassDB::bind_method(D_METHOD("get_host"), &DebugControlTable::get_host);
	ClassDB::bind_method(D_METHOD("clear"), &DebugControlTable::clear);
	ClassDB::bind_method(D_METHOD("control", "id"), &DebugControlTable::control);
	ClassDB::bind_method(D_METHOD("row_ids"), &DebugControlTable::row_ids);
	ClassDB::bind_method(D_METHOD("list_controls", "page", "filter", "allow_authority"),
			&DebugControlTable::list_controls, DEFVAL(StringName()), DEFVAL(String()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_state", "id", "allow_authority"), &DebugControlTable::get_state,
			DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_value", "id", "value", "allow_authority"),
			&DebugControlTable::set_value, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("invoke", "id", "args", "allow_authority"), &DebugControlTable::invoke,
			DEFVAL(Array()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("marshal_invoke_args", "id", "raw"),
			&DebugControlTable::marshal_invoke_args);
	ClassDB::bind_method(D_METHOD("capture_snapshot", "filter", "allow_authority"),
			&DebugControlTable::capture_snapshot, DEFVAL(String()), DEFVAL(false));
	ClassDB::bind_static_method("DebugControlTable", D_METHOD("normalize_value", "row", "value"),
			&DebugControlTable::normalize_value);
	BIND_CONSTANT(AUDIO_BUS_VOLUME_MIN_DB);
	BIND_CONSTANT(AUDIO_BUS_VOLUME_MAX_DB);
}

} // namespace godot
