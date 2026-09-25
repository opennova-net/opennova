#include "simulation/entity_presenter.h"
#include "util/axes.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <algorithm>
#include <base/io/fixed.h>
#include <formats/threedi/threedi_ctrl_catalog.h>

#include <runtime/world/entity_pose.h>
#include <runtime/world/entity.h>       // EntityKind: the organic-row gate of the DEATH leg
#include <runtime/world/present_rows.h> // PF_DEATH_CTRL: the org0 skin DEATH register word

#include "audio/mission_audio.h"
#include "env/mission_environment.h"
#include "lights/effect_light_director.h"
#include "particle/effect_world.h"
#include "simulation/destruction_events.h"
#include "simulation/present_event_records.h"
#include "simulation/simulation.h"
#include <runtime/world/vehicle_motor.h>
#include "world/item_effect_director.h"
#include "world/scar_draw_list.h"
#include "world/scar_presenter.h"

// The PLACED walk, the per-row legs both walks share, the statics and the
// bound surface. The wire walk (cold path, registry, hot rows) is
// entity_presenter_wire.cpp.

using namespace godot;
using opennova::world::PresentRowsView;

namespace {

// Pre-resolved catalog ordinals and native lifecycle tags. See the CTRL
// publication witness in docs/threedi/3di-gp-format-re.md.
struct CtrlRegisters {
	static constexpr int eweap_gunyaw = opennova::threedi::THREEDI_CTRL_EWEAP_GUNYAW;
	static constexpr int eweap_gunpitch = opennova::threedi::THREEDI_CTRL_EWEAP_GUNPITCH;
	static constexpr int weap_spin = opennova::threedi::THREEDI_CTRL_WEAP_SPIN;
	static constexpr int vehicle_steering = opennova::threedi::THREEDI_CTRL_VEHICLE_STEERING;
	static constexpr int vehicle_speed = opennova::threedi::THREEDI_CTRL_VEHICLE_SPEED;
	static constexpr int helo_rotor = opennova::threedi::THREEDI_CTRL_HELO_ROTOR;
	static constexpr int helo_tailrotor = opennova::threedi::THREEDI_CTRL_HELO_TAILROTOR;
	static constexpr int vehicle_wheels = opennova::threedi::THREEDI_CTRL_VEHICLE_WHEELS;
	static constexpr int helo_gear = opennova::threedi::THREEDI_CTRL_HELO_GEAR;
	static constexpr int vehicle_gun_yaw = opennova::threedi::THREEDI_CTRL_VEHICLE_GUNYAW;
	static constexpr int vehicle_gun_pitch = opennova::threedi::THREEDI_CTRL_VEHICLE_GUNPITCH;
	static constexpr int helo_gun_yaw = opennova::threedi::THREEDI_CTRL_HELO_GUNYAW;
	static constexpr int helo_gun_pitch = opennova::threedi::THREEDI_CTRL_HELO_GUNPITCH;
	static constexpr int tex_team = opennova::threedi::THREEDI_CTRL_TEX_TEAM;
	static constexpr int team_swing = opennova::threedi::THREEDI_CTRL_TEAMSWING;
	static constexpr int lfp_camp_percent = opennova::threedi::THREEDI_CTRL_LFP_CAMPPERCENT;
	static constexpr int heat_glow = opennova::threedi::THREEDI_CTRL_HEAT_GLOW;
	const std::string owner_emplaced = "present:emplaced";
	const std::string owner_vehicle_motion = "present:vehicle_motion";
	const std::string owner_sector_team = "present:sector_team";
	const std::string owner_zone = "present:zone";
	const std::string owner_world_heat = "present:world_heat";
	const std::string owner_doors = "present:doors";
	const std::string owner_death = "present:death";
	int vehicle_tires[14];
	int vehicle_tracks[4];
	int doors[30];
	static constexpr int death = opennova::threedi::THREEDI_CTRL_DEATH;
	CtrlRegisters() {
		for (int i = 0; i < 14; ++i) vehicle_tires[i] = opennova::threedi::THREEDI_CTRL_VEHICLE_TIRE00 + i;
		for (int i = 0; i < 4; ++i) vehicle_tracks[i] = opennova::threedi::THREEDI_CTRL_VEHICLE_WHEELS00 + i;
		for (int i = 0; i < 30; ++i) doors[i] = opennova::threedi::THREEDI_CTRL_DOOR_00 + i;
	}
};

const CtrlRegisters &names() {
	static const CtrlRegisters n;
	return n;
}

inline int32_t field_i(const float *p, int base, int field) {
	return static_cast<int32_t>(p[base + field]);
}

void set_owned_ctrl(ObjectModel *model, const std::string &owner,
		int reg, int32_t value) {
	model->set_ctrl_override_native(owner, reg, value);
}

void clear_owned_ctrl(ObjectModel *model, const std::string &owner,
		int reg) {
	model->clear_ctrl_override_native(owner, reg);
}

constexpr int AIM_PAYLOAD_FLOATS =
		Simulation::PF_EMPLACED_CONTROLS_VALID -
		Simulation::PF_AIM_BODY_PITCH_DEG;
static_assert(AIM_PAYLOAD_FLOATS == 30,
		"aim cache must cover body Euler plus all nine overlay triples");

} // namespace

void EntityPresenter::_bind_methods() {
	// --- the placed walk ---
	ClassDB::bind_method(D_METHOD("setup", "sim", "index", "placer"),
			&EntityPresenter::setup, DEFVAL(Ref<MissionObjectPlacer>()));
	ClassDB::bind_method(D_METHOD("set_output_channels", "channels"),
			&EntityPresenter::set_output_channels);
	ClassDB::bind_method(D_METHOD("get_output_channels"),
			&EntityPresenter::get_output_channels);
	ClassDB::bind_method(
			D_METHOD("present_snapshot", "snap", "stride", "layout_revision",
					"door_phases"),
			&EntityPresenter::present_snapshot, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("get_stats_record"),
			&EntityPresenter::get_stats_record);
	// --- the wire walk ---
	ClassDB::bind_method(
			D_METHOD("setup_wire", "sim", "placer", "container", "defer_index"),
			&EntityPresenter::setup_wire, DEFVAL(Ref<EntityIndex>()));
	ClassDB::bind_method(D_METHOD("set_synthetic_origin_only", "enabled"),
			&EntityPresenter::set_synthetic_origin_only);
	ClassDB::bind_method(D_METHOD("set_cold_spawn_budget", "budget"),
			&EntityPresenter::set_cold_spawn_budget);
	ClassDB::bind_method(D_METHOD("set_spectator_camera", "camera"),
			&EntityPresenter::set_spectator_camera);
	ClassDB::bind_method(
			D_METHOD("present_wire_snapshot", "snap", "stride", "layout_revision"),
			&EntityPresenter::present_wire_snapshot);
	ClassDB::bind_method(D_METHOD("set_render_culled", "wire_handle", "culled"),
			&EntityPresenter::set_render_culled);
	ClassDB::bind_method(D_METHOD("clear_render_culled"),
			&EntityPresenter::clear_render_culled);
	ClassDB::bind_method(D_METHOD("get_wire_stats_record"),
			&EntityPresenter::get_wire_stats_record);
	ClassDB::bind_method(D_METHOD("resolve_wire_handle", "wire_handle"),
			&EntityPresenter::resolve_wire_handle);
	ClassDB::bind_method(D_METHOD("held_weapon_node", "wire_handle"),
			&EntityPresenter::held_weapon_node);
	ClassDB::bind_method(D_METHOD("person_overlays_for", "wire_handle"),
			&EntityPresenter::person_overlays_for);
	ClassDB::bind_method(D_METHOD("set_entity_lighting_context", "wire_handle",
			"effect_scale", "interior_lerp", "light_transfer", "interior_bms",
			"interior_section"),
			&EntityPresenter::set_entity_lighting_context, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("muzzle_world_for", "handle", "userpoint"),
			&EntityPresenter::muzzle_world_for);
	ClassDB::bind_method(D_METHOD("wire_entity_count"),
			&EntityPresenter::wire_entity_count);
	ClassDB::bind_method(D_METHOD("wire_nodes"), &EntityPresenter::wire_nodes);
	ClassDB::bind_method(D_METHOD("register_wire_node", "handle", "node"),
			&EntityPresenter::register_wire_node);
	ClassDB::bind_method(D_METHOD("register_wire_held_weapon", "handle", "node"),
			&EntityPresenter::register_wire_held_weapon);
	ClassDB::bind_method(D_METHOD("reset_wire_runtime_state"),
			&EntityPresenter::reset_wire_runtime_state);
	ClassDB::bind_method(D_METHOD("teardown"), &EntityPresenter::teardown);
	// --- the local view's virtual display ---
	ClassDB::bind_method(D_METHOD("resolve_present_handle", "handle"),
			&EntityPresenter::resolve_present_handle);
	ClassDB::bind_method(
			D_METHOD("present_virtual_display", "active", "carrier_handle", "model"),
			&EntityPresenter::present_virtual_display);
	ClassDB::bind_method(D_METHOD("virtual_display_node"),
			&EntityPresenter::virtual_display_node);
	// --- the present passes ---
	ClassDB::bind_method(D_METHOD("setup_passes", "container", "item_db",
			"resource_root", "audio", "fx", "lights", "environment", "anchors"),
			&EntityPresenter::setup_passes);
	ClassDB::bind_method(D_METHOD("set_listener_position", "position"),
			&EntityPresenter::set_listener_position);
	ClassDB::bind_method(D_METHOD("present_passes"), &EntityPresenter::present_passes);
	ClassDB::bind_method(D_METHOD("get_fire_present_stats"),
			&EntityPresenter::get_fire_present_stats);
	ClassDB::bind_method(D_METHOD("get_destruction_present_stats"),
			&EntityPresenter::get_destruction_present_stats);
	ClassDB::bind_method(D_METHOD("get_throwable_present_stats"),
			&EntityPresenter::get_throwable_present_stats);
	ClassDB::bind_method(D_METHOD("get_scar_present_stats"),
			&EntityPresenter::get_scar_present_stats);
	ClassDB::bind_method(D_METHOD("has_active_wreck_fire", "owner_key"),
			&EntityPresenter::has_active_wreck_fire);
	ClassDB::bind_method(D_METHOD("fire_ribbon_mesh"), &EntityPresenter::fire_ribbon_mesh);
	ClassDB::bind_method(D_METHOD("scar_presenter"), &EntityPresenter::scar_presenter);
	ClassDB::bind_method(D_METHOD("present_fires", "events"),
			&EntityPresenter::present_fires);
	ClassDB::bind_method(D_METHOD("present_fire_sounds", "sounds"),
			&EntityPresenter::present_fire_sounds);
	ClassDB::bind_method(D_METHOD("present_slot_sounds", "events"),
			&EntityPresenter::present_slot_sounds);
	ClassDB::bind_method(D_METHOD("present_sound_emitters", "events"),
			&EntityPresenter::present_sound_emitters);
	ClassDB::bind_method(D_METHOD("draw_tracer_rows", "rows"),
			&EntityPresenter::draw_tracer_rows);
	ClassDB::bind_method(D_METHOD("present_destruction_drained", "events", "pieces"),
			&EntityPresenter::present_destruction_drained);
	ClassDB::bind_method(D_METHOD("present_death_piece_draws", "draws"),
			&EntityPresenter::present_death_piece_draws);
	ClassDB::bind_method(D_METHOD("death_piece_model", "slot"),
			&EntityPresenter::death_piece_model);
	ClassDB::bind_method(D_METHOD("present_throwable_visuals", "visuals"),
			&EntityPresenter::present_throwable_visuals);
	ClassDB::bind_method(D_METHOD("present_vehicle_trail_visuals", "visuals"),
			&EntityPresenter::present_vehicle_trail_visuals);
	ClassDB::bind_method(D_METHOD("present_scar_draw_list", "draw_list"),
			&EntityPresenter::present_scar_draw_list);
	BIND_ENUM_CONSTANT(PASS_PROFILE_FIRE_US);
	BIND_ENUM_CONSTANT(PASS_PROFILE_DESTRUCTION_US);
	BIND_ENUM_CONSTANT(PASS_PROFILE_THROWABLE_US);
	BIND_ENUM_CONSTANT(PASS_PROFILE_SCARS_US);
	BIND_ENUM_CONSTANT(PASS_PROFILE_SLOT_COUNT);
	// Emitted once per materialized wire body, after the wire rows are
	// presented (identity + production transform applied); never re-emitted.
	// A late subscriber replays wire_nodes() itself.
	ADD_SIGNAL(MethodInfo("wire_node_spawned",
			PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_NODE_TYPE,
					"ObjectModel"),
			PropertyInfo(Variant::INT, "kind"),
			PropertyInfo(Variant::INT, "item_id")));
	// --- the statics ---
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("held_weapon_attach_transform", "body", "attach_angles_bms",
					"hand_frame"),
			&EntityPresenter::held_weapon_attach_transform, DEFVAL(false));
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("held_weapon_hand_frame_basis", "bone_model_to_world"),
			&EntityPresenter::held_weapon_hand_frame_basis);
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("held_weapon_attach_nudge"),
			&EntityPresenter::held_weapon_attach_nudge);
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("held_weapon_hand_frame_z_rad"),
			&EntityPresenter::held_weapon_hand_frame_z_rad);
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("held_weapon_hand_frame_y_rad"),
			&EntityPresenter::held_weapon_hand_frame_y_rad);
	BIND_CONSTANT(HELD_WEAPON_BONE_INDEX);
	BIND_CONSTANT(DEFAULT_COLD_SPAWN_BUDGET);
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("aim_root_basis", "snap", "base", "fallback"),
			&EntityPresenter::aim_root_basis);
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("aim_apply", "node", "snap", "base", "drive_root_basis"),
			&EntityPresenter::aim_apply, DEFVAL(true));
	ClassDB::bind_static_method("EntityPresenter",
			D_METHOD("emplaced_apply", "node", "snap", "base", "clear_when_invalid"),
			&EntityPresenter::emplaced_apply);
	BIND_ENUM_CONSTANT(OUTPUT_TRANSFORM);
	BIND_ENUM_CONSTANT(OUTPUT_PART_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_VISIBILITY);
	BIND_ENUM_CONSTANT(OUTPUT_BODY_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_ALL);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_CORE_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_AIM_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_CONTROLS_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_VISIBILITY_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_BODY_US);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_ROWS);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_SUBMITTED_ROWS);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_BODY_ROWS);
	BIND_ENUM_CONSTANT(MISSION_PROFILE_SLOT_COUNT);
}

Simulation *EntityPresenter::sim() const {
	return sim_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_))
			: nullptr;
}

// The passes exist for the presenter's whole life (their stats read as empty
// records before setup_passes); the "Scars" child is the scar device.
EntityPresenter::EntityPresenter() :
		fire_(std::make_unique<FirePresenter>(this)) {
	destruction_.instantiate();
	throwable_.instantiate();
	vehicle_trail_.instantiate();
	ScarPresenter *scars_node = memnew(ScarPresenter);
	scars_node->set_name("Scars");
	add_child(scars_node);
	scars_id_ = scars_node->get_instance_id();
}

void EntityPresenter::setup(Object *sim, Object *index,
		const Ref<MissionObjectPlacer> &placer) {
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0) {
		release_part_anim_outputs();
	}
	Simulation *native_sim = Object::cast_to<Simulation>(sim);
	sim_id_ = native_sim != nullptr ? native_sim->get_instance_id() : ObjectID();
	index_ = Ref<EntityIndex>(Object::cast_to<EntityIndex>(index));
	placer_ = placer;
	plan_revision_ = -1; // force a rebuild against the new wiring
	plan_dirty_ = true;
	release_planned_rows();
}

void EntityPresenter::teardown() {
	reset_wire_runtime_state();
	fire_->teardown(); // frees the tracer mesh instance under the container
	release_planned_rows();
	plan_dirty_ = true;
}

// --- The present passes ------------------------------------------------------

ScarPresenter *EntityPresenter::scars() const {
	return scars_id_.is_valid()
			? Object::cast_to<ScarPresenter>(ObjectDB::get_instance(scars_id_))
			: nullptr;
}

MissionEnvironment *EntityPresenter::environment() const {
	return environment_id_.is_valid()
			? Object::cast_to<MissionEnvironment>(ObjectDB::get_instance(environment_id_))
			: nullptr;
}

void EntityPresenter::setup_passes(Node3D *p_container, const Ref<ItemDatabase> &p_item_db,
		const Ref<ResourceRoot> &p_resource_root, MissionAudio *p_audio, EffectWorld *p_fx,
		EffectLightDirector *p_lights, MissionEnvironment *p_environment,
		ItemEffectDirector *p_anchors) {
	Simulation *s = sim();
	const Ref<ItemEffectDirector> anchors(p_anchors);
	fire_->setup(s, p_container, p_audio, p_fx, p_lights, p_resource_root, p_environment);
	destruction_->setup(this, s, p_container, index_, placer_, p_item_db, anchors, p_audio,
			p_fx, p_lights);
	throwable_->setup(s, p_container, placer_, p_item_db, p_fx, anchors);
	vehicle_trail_->setup(s, p_fx, anchors);
	environment_id_ = p_environment != nullptr ? p_environment->get_instance_id() : ObjectID();
	if (ScarPresenter *scars_node = scars()) {
		scars_node->set_resource_root(p_resource_root);
	}
}

void EntityPresenter::set_listener_position(const Vector3 &p_position) {
	listener_position_ = p_position;
}

void EntityPresenter::present_scars() {
	if (ScarPresenter *scars_node = scars()) {
		scars_node->present_frame(sim(), listener_position_, environment(), index_.ptr(), this);
	}
}

void EntityPresenter::present_passes() {
	present_minefields();
	if (Simulation *simulation = sim()) simulation->advance_facial_presentation(listener_position_);
	fire_->present();
	destruction_->present();
	throwable_->present();
	// After the entity rows and the other passes: the entity-ring meshes
	// parent under section nodes the row walks may have just built.
	present_scars();
}

PackedInt64Array EntityPresenter::profile_present_passes() {
	present_minefields();
	if (Simulation *simulation = sim()) simulation->advance_facial_presentation(listener_position_);
	PackedInt64Array spans;
	spans.resize(PASS_PROFILE_SLOT_COUNT);
	Time *clock = Time::get_singleton();
	uint64_t start = clock->get_ticks_usec();
	fire_->present();
	uint64_t now = clock->get_ticks_usec();
	spans.set(PASS_PROFILE_FIRE_US, static_cast<int64_t>(now - start));
	start = now;
	destruction_->present();
	now = clock->get_ticks_usec();
	spans.set(PASS_PROFILE_DESTRUCTION_US, static_cast<int64_t>(now - start));
	start = now;
	throwable_->present();
	now = clock->get_ticks_usec();
	spans.set(PASS_PROFILE_THROWABLE_US, static_cast<int64_t>(now - start));
	start = now;
	present_scars();
	now = clock->get_ticks_usec();
	spans.set(PASS_PROFILE_SCARS_US, static_cast<int64_t>(now - start));
	return spans;
}

void EntityPresenter::sync_fixed_tick_effects() {
	vehicle_trail_->sync_fixed_tick_effects();
	throwable_->sync_fixed_tick_effects();
}

Ref<FirePresentStats> EntityPresenter::get_fire_present_stats() const {
	return fire_->get_stats();
}

Ref<DestructionPresentStats> EntityPresenter::get_destruction_present_stats() const {
	return destruction_->get_stats();
}

Ref<ThrowablePresentStats> EntityPresenter::get_throwable_present_stats() const {
	return throwable_->get_stats();
}

Ref<ScarPresentStats> EntityPresenter::get_scar_present_stats() const {
	if (ScarPresenter *scars_node = scars()) {
		return scars_node->get_present_stats();
	}
	Ref<ScarPresentStats> empty;
	empty.instantiate();
	return empty;
}

bool EntityPresenter::has_active_wreck_fire(const String &p_owner_key) const {
	return destruction_->has_active_wreck_fire(p_owner_key);
}

void EntityPresenter::warm_fire_pipelines(const Vector3 &p_position) {
	fire_->warm_pipelines(p_position);
}

Ref<ArrayMesh> EntityPresenter::fire_ribbon_mesh() const {
	return fire_->ribbon_mesh();
}

ScarPresenter *EntityPresenter::scar_presenter() const {
	return scars();
}

// The bound data legs unwrap the test-authored records into the engine rows
// the passes consume (ADR 0043 d10: C++ consumers read the native vectors).
template <typename Row, typename Record>
static std::vector<Row> unwrap_rows(const TypedArray<Record> &p_records) {
	std::vector<Row> rows;
	rows.reserve(static_cast<size_t>(p_records.size()));
	for (int64_t i = 0; i < p_records.size(); ++i) {
		const Ref<Record> record = p_records[i];
		if (record.is_valid()) {
			rows.push_back(record->value());
		}
	}
	return rows;
}

void EntityPresenter::present_fires(const TypedArray<FirePresentationEvent> &p_events) {
	fire_->present_fires(
			unwrap_rows<opennova::world::FirePresentationRow, FirePresentationEvent>(p_events));
}

void EntityPresenter::present_fire_sounds(const TypedArray<FireSoundRow> &p_sounds) {
	fire_->present_fire_sounds(unwrap_rows<opennova::world::ReadyFireSound, FireSoundRow>(p_sounds));
}

void EntityPresenter::present_slot_sounds(const TypedArray<SlotSoundRow> &p_events) {
	fire_->present_slot_sounds(unwrap_rows<opennova::world::SoundSlotEvent, SlotSoundRow>(p_events));
}

void EntityPresenter::present_sound_emitters(const TypedArray<SoundEmitterRow> &p_events) {
	fire_->present_sound_emitters(
			unwrap_rows<opennova::world::SoundEmitterEvent, SoundEmitterRow>(p_events));
}

void EntityPresenter::draw_tracer_rows(const PackedFloat32Array &p_rows) {
	fire_->draw_tracer_rows(p_rows);
}

void EntityPresenter::present_destruction_drained(const Ref<DestructionDrain> &p_events,
		const TypedArray<DeathPieceRow> &p_pieces) {
	const opennova::world::DestructionEvents none;
	destruction_->present_drained(p_events.is_valid() ? p_events->value() : none,
			unwrap_rows<opennova::world::DeathPieceRow, DeathPieceRow>(p_pieces));
}

void EntityPresenter::present_death_piece_draws_native(
		const std::vector<opennova::world::DeathPieceDraw> &p_draws) {
	destruction_->apply_piece_draws(p_draws);
}

void EntityPresenter::present_death_piece_draws(const TypedArray<DeathPieceDraw> &p_draws) {
	destruction_->apply_piece_draws(
			unwrap_rows<opennova::world::DeathPieceDraw, DeathPieceDraw>(p_draws));
}

ObjectModel *EntityPresenter::death_piece_model(int p_slot) const {
	return destruction_->piece_model(p_slot);
}

void EntityPresenter::present_throwable_visuals(const TypedArray<ThrowableVisualRow> &p_visuals) {
	throwable_->present_visuals(
			unwrap_rows<opennova::world::ThrowableVisualRow, ThrowableVisualRow>(p_visuals));
}

void EntityPresenter::present_vehicle_trail_visuals(
		const TypedArray<VehicleTrailVisualRow> &p_visuals) {
	vehicle_trail_->sync_visuals(
			unwrap_rows<opennova::world::VehicleTrailVisualRow, VehicleTrailVisualRow>(p_visuals));
}

void EntityPresenter::present_scar_draw_list(const Ref<ScarDrawList> &p_draw_list) {
	if (ScarPresenter *scars_node = scars()) {
		scars_node->present_draw_list(p_draw_list, index_.ptr(), this);
	}
}

// Retained rows stop being "planned" the moment the plan drops them, so a model
// that later leaves the mission (a despawned row kept alive as a preview) no
// longer moves the lifetime stamp on death.
void EntityPresenter::release_planned_rows() {
	for (const Row &row : rows_) {
		ObjectModel *model =
				Object::cast_to<ObjectModel>(ObjectDB::get_instance(row.node_id));
		if (model != nullptr) {
			model->set_present_planned(false);
		}
	}
	rows_.clear();
}

void EntityPresenter::set_output_channels(int channels) {
	const int next = channels & OUTPUT_ALL;
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0 &&
			(next & OUTPUT_PART_ANIM) == 0) {
		// Turning a presentation seam off must release its retained writers;
		// otherwise the last pose survives indefinitely on persistent nodes.
		release_part_anim_outputs();
	}
	const int rising = next & ~output_channels_;
	output_channels_ = next;
	if (rising == 0) {
		return;
	}
	for (Row &row : rows_) {
		if ((rising & OUTPUT_TRANSFORM) != 0) {
			row.transform_stamp_valid = false;
		}
		if ((rising & OUTPUT_PART_ANIM) != 0) {
			row.ctrl_publish_state_valid = false;
		}
		if ((rising & OUTPUT_BODY_ANIM) != 0) {
			row.body_stamp_valid = false;
		}
	}
}

Ref<MissionPresentStats> EntityPresenter::get_stats_record() const {
	Ref<MissionPresentStats> stats;
	stats.instantiate();
	stats->moved = stat_moved_;
	stats->posed = stat_posed_;
	stats->hidden = stat_hidden_;
	stats->plan_rebuilds = stat_plan_rebuilds_;
	stats->transform_builds = stat_transform_builds_;
	stats->aim_dispatches = stat_aim_dispatches_;
	stats->rhc_dispatches = stat_rhc_dispatches_;
	stats->part_dispatches = stat_part_dispatches_;
	stats->control_dispatches = stat_control_dispatches_;
	stats->body_dispatches = stat_body_dispatches_;
	return stats;
}

namespace {

// The held weapon placement — the calibration and the full derivation live at
// pivot nudge in raw def units, X negated into the render frame — the values
// engine world/entity_pose.h (the sim-side muzzle shares them)
// [orig: flt_7C68E8 = 0.05 +X/-Y, flt_7C9BA8 = 0.051 +Z @ 0x4b2186].
constexpr int kHeldWeaponBoneIndex = opennova::world::kHeldWeaponBoneIndex;
const Vector3 kHeldWeaponAttachNudge(opennova::world::kHeldWeaponAttachNudgeX,
		opennova::world::kHeldWeaponAttachNudgeY,
		opennova::world::kHeldWeaponAttachNudgeZ);
// Hand-frame calibration [orig: Rz dbl_7C9BA0 / Ry dbl_7C9B98 via
// Math_BuildRotationMatrix4x4_ByAxis @ 0x611db0].
constexpr double kHandFrameZRad = opennova::world::kHeldWeaponHandFrameZRad;
constexpr double kHandFrameYRad = opennova::world::kHeldWeaponHandFrameYRad;

} // namespace

Basis EntityPresenter::held_weapon_hand_frame_basis(const Basis &bone_model_to_world) {
	// Row-major `Ry_e · Rz_e · M16` = the calibrations on the RIGHT in column
	// form; signs as authored (two inversions cancel — the entity_pose.h
	// ledger documents why).
	return bone_model_to_world * Basis(Vector3(0, 0, 1), kHandFrameZRad) *
			Basis(Vector3(0, 1, 0), kHandFrameYRad);
}

Variant EntityPresenter::held_weapon_attach_transform(Object *body,
		const Vector3 &attach_angles_bms, bool hand_frame) {
	Skeleton3D *skel = Object::cast_to<Skeleton3D>(find_skeleton(body));
	if (skel == nullptr || skel->get_bone_count() <= kHeldWeaponBoneIndex) {
		return Variant();
	}
	// `M16 · pivot16` IS the joint world position; the nudge rides bone 16's
	// MODEL->WORLD rotation (pose relative to REST — the rest basis is a large
	// rotation) [orig: translation overwrite @ 0x4b22cf..0x4b22f8].
	const Transform3D joint_world = skel->get_global_transform() *
			skel->get_bone_global_pose(kHeldWeaponBoneIndex);
	const Transform3D model_to_world = joint_world *
			skel->get_bone_global_rest(kHeldWeaponBoneIndex).affine_inverse();
	const Basis basis = hand_frame
			? held_weapon_hand_frame_basis(model_to_world.basis)
			: bms_to_godot_basis(attach_angles_bms);
	return Transform3D(basis,
			joint_world.origin + model_to_world.basis.xform(kHeldWeaponAttachNudge));
}

Vector3 EntityPresenter::held_weapon_attach_nudge() {
	return kHeldWeaponAttachNudge;
}

double EntityPresenter::held_weapon_hand_frame_z_rad() {
	return opennova::world::kHeldWeaponHandFrameZRad;
}

double EntityPresenter::held_weapon_hand_frame_y_rad() {
	return opennova::world::kHeldWeaponHandFrameYRad;
}

Object *EntityPresenter::find_skeleton(Object *root) {
	// The recursive Skeleton3D walk the GDScript reference ran per call, native
	// (ObjectModel.rebuild() frees children, so caching the result by
	// ObjectID would go stale mid-play; the walk itself is now cheap).
	if (root == nullptr) {
		return nullptr;
	}
	if (Object::cast_to<Skeleton3D>(root) != nullptr) {
		return root;
	}
	Node *node = Object::cast_to<Node>(root);
	if (node == nullptr) {
		return nullptr;
	}
	for (int i = 0; i < node->get_child_count(); ++i) {
		Object *found = find_skeleton(node->get_child(i));
		if (found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

Basis EntityPresenter::aim_root_basis(const PackedFloat32Array &snap, int base,
		const Basis &fallback) {
	return aim_root_basis_native({snap.ptr(), snap.size()}, base, fallback);
}

Basis EntityPresenter::aim_root_basis_native(PresentRowsView snap, int base,
		const Basis &fallback) {
	const float *p = snap.ptr();
	if (field_i(p, base, Simulation::PF_AIM_OVERLAY_VALID) == 0) {
		return fallback;
	}
	return bms_to_godot_basis(
			Vector3(p[base + Simulation::PF_AIM_BODY_PITCH_DEG],
					p[base + Simulation::PF_AIM_BODY_YAW_DEG],
					p[base + Simulation::PF_AIM_BODY_ROLL_DEG]));
}

void EntityPresenter::aim_apply(Object *node, const PackedFloat32Array &snap,
		int base, bool drive_root_basis) {
	ObjectModel *model = Object::cast_to<ObjectModel>(node);
	if (model == nullptr) {
		return;
	}
	const float *p = snap.ptr();
	// The mounted-seat selector's final skeletal verdict applies to placed and
	// wire models alike [orig: BN17 special row @ 0x4b1290].
	model->set_right_hand_collapsed(
			field_i(p, base, Simulation::PF_RIGHT_HAND_COLLAPSED) != 0);
	if (field_i(p, base, Simulation::PF_AIM_OVERLAY_VALID) == 0) {
		model->clear_aim_overlay();
		return;
	}
	aim_apply_valid(model, {snap.ptr(), snap.size()}, base, drive_root_basis);
}

void EntityPresenter::aim_apply_valid(Object *node,
		PresentRowsView snap, int base, bool drive_root_basis) {
	ObjectModel *model = Object::cast_to<ObjectModel>(node);
	if (model == nullptr) {
		return;
	}
	const Basis current_basis = model->get_basis();
	const Basis body_basis = aim_root_basis_native(snap, base, current_basis);
	if (drive_root_basis && current_basis != body_basis) {
		model->set_basis(body_basis);
	}
	const Basis inverse_body = body_basis.inverse();
	const float *p = snap.ptr();
	Basis deltas[ObjectModel::kAimOverlayClasses];
	for (int overlay_class = 0; overlay_class < ObjectModel::kAimOverlayClasses;
			++overlay_class) {
		const int offset = base + Simulation::PF_AIM_ANGLES +
				overlay_class * Simulation::PF_AIM_CLASS_STRIDE;
		deltas[overlay_class] = inverse_body *
				bms_to_godot_basis(
						Vector3(p[offset], p[offset + 1], p[offset + 2]));
	}
	model->set_aim_overlay_deltas(deltas);
}

namespace {

void emplaced_clear_typed(ObjectModel *model);
void vehicle_motion_clear_typed(ObjectModel *model);
void zone_team_clear_typed(ObjectModel *model);
void world_heat_clear_typed(ObjectModel *model);

int emplaced_apply_typed(ObjectModel *model, PresentRowsView snap,
		int base, bool clear_when_invalid) {
	const CtrlRegisters &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, Simulation::PF_EMPLACED_CONTROLS_VALID) == 1) {
		set_owned_ctrl(model, n.owner_emplaced, n.eweap_gunyaw,
				field_i(p, base, Simulation::PF_EWEAP_GUNYAW));
		set_owned_ctrl(model, n.owner_emplaced, n.eweap_gunpitch,
				field_i(p, base, Simulation::PF_EWEAP_GUNPITCH));
		set_owned_ctrl(model, n.owner_emplaced, n.weap_spin,
				field_i(p, base, Simulation::PF_WEAP_SPIN));
		return 3;
	}
	// Nodes persist when a row stops publishing: remove only the three controls
	// this presenter owns — a bulk CTRL clear would also erase live WAC channels.
	if (clear_when_invalid) {
		emplaced_clear_typed(model);
	}
	return 0;
}

void emplaced_clear_typed(ObjectModel *model) {
	const CtrlRegisters &n = names();
	clear_owned_ctrl(model, n.owner_emplaced, n.eweap_gunyaw);
	clear_owned_ctrl(model, n.owner_emplaced, n.eweap_gunpitch);
	clear_owned_ctrl(model, n.owner_emplaced, n.weap_spin);
}

void focal_sway_apply_typed(ObjectModel *model, const float *p, int base) {
	using namespace opennova::world;
	const bool active = p[base + PF_FOCAL_SWAY_VALID] != 0;
	const float *m = p + base + PF_FOCAL_SWAY_BASIS_0;
	Basis basis;
	if (active) {
		basis.set_column(0, Vector3(m[0], m[3], m[6]));
		basis.set_column(1, Vector3(m[1], m[4], m[7]));
		basis.set_column(2, Vector3(m[2], m[5], m[8]));
	}
	model->set_focal_sway(active, basis,
			active ? Vector3(p[base + PF_FOCAL_SWAY_X], p[base + PF_FOCAL_SWAY_Y],
							 p[base + PF_FOCAL_SWAY_Z])
				   : Vector3());
}

int vehicle_motion_apply_typed(ObjectModel *model,
		PresentRowsView snap, int base) {
	using namespace opennova::world;
	const CtrlRegisters &n = names();
	const float *p = snap.ptr();
	const int mask = field_i(p, base, Simulation::PF_VEHICLE_MOTION_VALID) == 1
			? field_i(p, base, Simulation::PF_VEHICLE_CTRL_MASK)
			: 0;
	int writes = 0;
	const auto apply = [&](int name, int flag, int field) {
		if ((mask & flag) != 0) {
			set_owned_ctrl(model, n.owner_vehicle_motion, name, field_i(p, base, field));
			++writes;
		} else {
			clear_owned_ctrl(model, n.owner_vehicle_motion, name);
		}
	};
	apply(n.vehicle_steering, VC_STEERING, Simulation::PF_VEHICLE_STEERING);
	apply(n.vehicle_speed, VC_SPEED, Simulation::PF_VEHICLE_SPEED);
	apply(n.helo_rotor, VC_ROTORS, Simulation::PF_VEHICLE_ROTOR);
	apply(n.helo_tailrotor, VC_ROTORS, Simulation::PF_VEHICLE_TAIL_ROTOR);
	apply(n.vehicle_wheels, VC_WHEELS, Simulation::PF_VEHICLE_WHEELS);
	apply(n.helo_gear, VC_HELO_GEAR, Simulation::PF_VEHICLE_GEAR);
	for (int i = 0; i < 14; ++i)
		apply(n.vehicle_tires[i], i < 6 ? VC_TIRES | VC_TANK_TIRES : VC_TANK_TIRES,
				Simulation::PF_VEHICLE_TIRE00 + i);
	for (int i = 0; i < 4; ++i)
		apply(n.vehicle_tracks[i], VC_TRACKS, Simulation::PF_VEHICLE_TRACK_LEFT + (i & 1));
	apply(n.vehicle_gun_yaw, VC_VEHICLE_GUN, Simulation::PF_VEHICLE_GUN_YAW);
	apply(n.vehicle_gun_pitch, VC_VEHICLE_GUN, Simulation::PF_VEHICLE_GUN_PITCH);
	apply(n.helo_gun_yaw, VC_HELO_GUN, Simulation::PF_VEHICLE_GUN_YAW);
	apply(n.helo_gun_pitch, VC_HELO_GUN, Simulation::PF_VEHICLE_GUN_PITCH);
	return writes;
}

void vehicle_motion_clear_typed(ObjectModel *model) {
	const CtrlRegisters &n = names();
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_steering);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_speed);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.helo_rotor);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.helo_tailrotor);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_wheels);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.helo_gear);
	for (int tire : n.vehicle_tires)
		clear_owned_ctrl(model, n.owner_vehicle_motion, tire);
	for (int track : n.vehicle_tracks)
		clear_owned_ctrl(model, n.owner_vehicle_motion, track);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_gun_yaw);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.vehicle_gun_pitch);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.helo_gun_yaw);
	clear_owned_ctrl(model, n.owner_vehicle_motion, n.helo_gun_pitch);
}

int zone_team_apply_typed(ObjectModel *model, PresentRowsView snap,
		int base) {
	const CtrlRegisters &n = names();
	const float *p = snap.ptr();
	int writes = 0;
	if (field_i(p, base, Simulation::PF_TEX_TEAM_VALID) == 1) {
		set_owned_ctrl(model, n.owner_sector_team, n.tex_team,
				field_i(p, base, Simulation::PF_TEX_TEAM));
		++writes;
	} else {
		clear_owned_ctrl(model, n.owner_sector_team, n.tex_team);
	}
	if (field_i(p, base, Simulation::PF_ZONE_CTRL_VALID) == 1) {
		// TEAMSWING is an unconditional store inside the packed-zone-byte
		// branch, including literal zero for team 1.
		set_owned_ctrl(model, n.owner_zone, n.team_swing,
				field_i(p, base, Simulation::PF_TEAMSWING));
		++writes;
	} else {
		clear_owned_ctrl(model, n.owner_zone, n.team_swing);
	}
	if (field_i(p, base, Simulation::PF_LFP_CAMPPERCENT_VALID) == 1) {
		set_owned_ctrl(model, n.owner_zone, n.lfp_camp_percent,
				field_i(p, base, Simulation::PF_LFP_CAMPPERCENT));
		++writes;
	} else {
		// A numbered zone without a timer-list entry does not write LFP at all.
		// Releasing our bounded per-model writer represents that omission; it is
		// deliberately not a fabricated zero store.
		clear_owned_ctrl(model, n.owner_zone, n.lfp_camp_percent);
	}
	return writes;
}

void zone_team_clear_typed(ObjectModel *model) {
	const CtrlRegisters &n = names();
	clear_owned_ctrl(model, n.owner_sector_team, n.tex_team);
	clear_owned_ctrl(model, n.owner_zone, n.team_swing);
	clear_owned_ctrl(model, n.owner_zone, n.lfp_camp_percent);
}

int world_heat_apply_typed(ObjectModel *model, PresentRowsView snap,
		int base) {
	const CtrlRegisters &n = names();
	const float *p = snap.ptr();
	if (field_i(p, base, Simulation::PF_WORLD_HEAT_GLOW_VALID) == 1) {
		// The ewep writer publishes cold zero too.
		// [orig: HUD_CacheWeaponSlotInfo @ 0x440969 / @ 0x440991, the 'ewep'
		//  render-class row's CTRL callback @0x82CFA0, also called directly
		//  by a UseGun seat attachment @ 0x546518]
		set_owned_ctrl(model, n.owner_world_heat, n.heat_glow,
				field_i(p, base, Simulation::PF_WORLD_HEAT_GLOW));
		return 1;
	}
	world_heat_clear_typed(model);
	return 0;
}

void world_heat_clear_typed(ObjectModel *model) {
	const CtrlRegisters &n = names();
	clear_owned_ctrl(model, n.owner_world_heat, n.heat_glow);
}

} // namespace

int EntityPresenter::wire_controls_apply(ObjectModel *model,
		PresentRowsView snap, int base) {
	int writes = 0;
	writes += emplaced_apply_typed(model, snap, base, true);
	writes += vehicle_motion_apply_typed(model, snap, base);
	focal_sway_apply_typed(model, snap.ptr(), base);
	writes += zone_team_apply_typed(model, snap, base);
	writes += world_heat_apply_typed(model, snap, base);
	return writes;
}

int EntityPresenter::emplaced_apply(Object *node,
		const PackedFloat32Array &snap, int base, bool clear_when_invalid) {
	ObjectModel *model = Object::cast_to<ObjectModel>(node);
	return model != nullptr
			? emplaced_apply_typed(model, {snap.ptr(), snap.size()}, base, clear_when_invalid)
			: 0;
}

// --- The per-row legs both walks share ---------------------------------------

void EntityPresenter::stamp_destroy_phases(ObjectModel *model, const float *p, int base) {
	model = destruction_->visual_model(model);
	static const std::string owner("present:destruction");
	// Preserve publication before later culling and owner-scoped zero releases.
	// Retail's direct six-slot writer and the retained-bus boundary are
	// documented in docs/threedi/3di-gp-format-re.md (D-3DI-2).
	model->begin_ctrl_update();
	for (int i = 0; i < 6; ++i) {
		const int32_t phase = field_i(p, base, Simulation::PF_OBJECT_DESTROY + i);
		if (phase != 0) model->set_ctrl_override_native(owner, opennova::threedi::THREEDI_CTRL_OBJECT_DESTROY + i, phase);
		else model->clear_ctrl_override_native(owner, opennova::threedi::THREEDI_CTRL_OBJECT_DESTROY + i);
	}
	model->end_ctrl_update();
}

void EntityPresenter::stamp_match_terrain(ObjectModel *model, const float *p,
		int base) {
	model->set_match_terrain_enabled(
			(field_i(p, base, Simulation::PF_STANCE_BITS) & 0x03) != 0);
}

bool EntityPresenter::stamp_right_hand_collapsed(ObjectModel *model,
		const float *p, int base, int32_t &last_rhc) {
	const int32_t rhc = field_i(p, base, Simulation::PF_RIGHT_HAND_COLLAPSED);
	if (rhc == last_rhc) {
		return false;
	}
	model->set_right_hand_collapsed(rhc != 0);
	last_rhc = rhc;
	return true;
}

bool EntityPresenter::aim_payload_changed(const float *p, int base,
		std::array<float, kAimPayloadFloats> &cache, bool &cache_valid) {
	static_assert(kAimPayloadFloats == AIM_PAYLOAD_FLOATS,
			"the shared aim cache must span exactly aim_apply_valid's reads");
	const float *payload = p + base + Simulation::PF_AIM_BODY_PITCH_DEG;
	if (cache_valid) {
		bool same = true;
		for (int i = 0; i < kAimPayloadFloats; ++i) {
			if (payload[i] != cache[static_cast<size_t>(i)]) {
				same = false;
				break;
			}
		}
		if (same) {
			return false;
		}
	}
	std::copy_n(payload, kAimPayloadFloats, cache.begin());
	cache_valid = true;
	return true;
}

void EntityPresenter::stamp_section_mask(ObjectModel *model, const float *p,
		int base, int64_t &last_mask) {
	model = destruction_->visual_model(model);
	// The sim's destroyed sections (entity+0x138), the model's own hidden
	// mask beside the occlusion frame's verdict (ObjectModel ORs the two).
	if (field_i(p, base, Simulation::PF_SECTION_MASK_VALID) != 0) {
		const int64_t hidden_mask = static_cast<int64_t>(
				static_cast<uint32_t>(field_i(
						p, base, Simulation::PF_SECTION_MASK_LO)) |
				(static_cast<uint32_t>(field_i(
						p, base, Simulation::PF_SECTION_MASK_HI))
						<< 16));
		if (hidden_mask != last_mask) {
			model->set_destroyed_section_mask(hidden_mask);
			last_mask = hidden_mask;
		}
	} else if (last_mask != -2 && last_mask != 0) {
		model->set_destroyed_section_mask(0);
		last_mask = 0;
	}
}

// --- The placed walk ---------------------------------------------------------

int64_t EntityPresenter::current_index_generation() {
	return index_.is_valid() ? index_->get_generation() : 0;
}

bool EntityPresenter::row_plan_is_current(int64_t size, int stride,
		int64_t layout_revision) {
	if (plan_dirty_ || plan_revision_ != layout_revision ||
			plan_stride_ != stride || plan_snapshot_size_ != size ||
			plan_index_generation_ != current_index_generation() ||
			plan_model_lifetime_generation_ !=
					ObjectModel::lifetime_generation()) {
		return false;
	}
	return true;
}

void EntityPresenter::rebuild_row_plan(const float *p, int64_t size, int stride,
		int64_t layout_revision) {
	if ((output_channels_ & OUTPUT_PART_ANIM) != 0) {
		release_part_anim_outputs();
	}
	++stat_plan_rebuilds_;
	release_planned_rows();
	plan_revision_ = layout_revision;
	plan_stride_ = stride;
	plan_snapshot_size_ = size;
	plan_index_generation_ = current_index_generation();
	plan_model_lifetime_generation_ = ObjectModel::lifetime_generation();
	plan_dirty_ = false;
	if (index_.is_null()) {
		plan_dirty_ = true;
		return;
	}
	const int64_t count = size / stride;
	rows_.reserve(static_cast<size_t>(count));
	for (int64_t r = 0; r < count; ++r) {
		const int base = static_cast<int>(r * stride);
		const int32_t bms_id = field_i(p, base, Simulation::PF_BMS_ID);
		const int32_t kind = field_i(p, base, Simulation::PF_KIND);
		const int32_t idx = field_i(p, base, Simulation::PF_INDEX);
		ObjectModel *model = index_->resolve(bms_id, kind, idx);
		if (model == nullptr) {
			continue;
		}
		Row row;
		row.base = base;
		row.node_id = model->get_instance_id();
		row.model = model;
		model->set_present_planned(true);
		row.entity_kind = kind;
		row.entity_index = idx;
		row.bms_id = bms_id;
		row.handle = field_i(p, base, Simulation::PF_WIRE_HANDLE);
		rows_.push_back(row);
	}
}

void EntityPresenter::release_part_anim_outputs() {
	for (const Row &row : rows_) {
		ObjectModel *model =
				Object::cast_to<ObjectModel>(ObjectDB::get_instance(row.node_id));
		if (model == nullptr) continue;
		model->begin_ctrl_update();
		model->clear_part_phase(1);
		model->clear_part_phase(2);
		emplaced_clear_typed(model);
		vehicle_motion_clear_typed(model);
		zone_team_clear_typed(model);
		world_heat_clear_typed(model);
		// The ordinal DOOR_xx bus has no fixed register set: release whatever
		// this writer owns instead of probing all 30 names.
		model->clear_ctrl_overrides_owned_native(names().owner_doors);
		model->clear_ctrl_overrides_owned_native("present:destruction");
		model->clear_ctrl_overrides_owned_native(names().owner_death);
		model->end_ctrl_update();
	}
}

const String &EntityPresenter::infantry_key(int state) {
	auto it = infantry_keys_.find(state);
	if (it == infantry_keys_.end()) {
		it = infantry_keys_.emplace(state, Simulation::infantry_anim_key(state))
					 .first;
	}
	return it->second;
}

void EntityPresenter::present_snapshot(const PackedFloat32Array &snap,
		int stride, int64_t layout_revision, const PackedInt32Array &door_phases) {
	present_rows({snap.ptr(), snap.size()}, stride, layout_revision,
			door_phases.ptr(), door_phases.size());
}

void EntityPresenter::present_rows(PresentRowsView snap, int stride,
		int64_t layout_revision, const int32_t *door_phases, int64_t door_phase_count) {
	present_snapshot_impl(snap, stride, layout_revision, door_phases, door_phase_count, nullptr);
}

PackedInt64Array EntityPresenter::profile_present_snapshot(
		PresentRowsView snap, int stride, int64_t layout_revision,
		const int32_t *door_phases, int64_t door_phase_count) {
	MissionFrameProfile profile;
	present_snapshot_impl(snap, stride, layout_revision, door_phases, door_phase_count, &profile);
	PackedInt64Array result;
	result.resize(MISSION_PROFILE_SLOT_COUNT);
	result.set(MISSION_PROFILE_CORE_US, profile.core_us);
	result.set(MISSION_PROFILE_AIM_US, profile.aim_us);
	result.set(MISSION_PROFILE_CONTROLS_US, profile.controls_us);
	result.set(MISSION_PROFILE_VISIBILITY_US, profile.visibility_us);
	result.set(MISSION_PROFILE_BODY_US, profile.body_us);
	result.set(MISSION_PROFILE_ROWS, profile.rows);
	result.set(MISSION_PROFILE_SUBMITTED_ROWS, profile.submitted_rows);
	result.set(MISSION_PROFILE_BODY_ROWS, profile.body_rows);
	return result;
}

void EntityPresenter::present_snapshot_impl(PresentRowsView snap,
		int stride, int64_t layout_revision, const int32_t *door_phases, int64_t door_phase_count,
		MissionFrameProfile *p_profile) {
	if (stride < Simulation::PF_STRIDE || index_.is_null()) {
		return;
	}
	const float *p = snap.ptr();
	const int64_t size = snap.size();
	// The door side table's (row index, count, phases...) entries sit in row
	// order, so one cursor advanced along the ascending row walk finds each
	// door row's entry without a search.
	const int32_t *door_table = door_phases;
	const int64_t door_table_size = door_phase_count;
	int64_t door_cursor = 0;
	if (!row_plan_is_current(size, stride, layout_revision)) {
		rebuild_row_plan(p, size, stride, layout_revision);
	}
	for (Row &row : rows_) {
		// Main-thread Node destruction advances this stamp in PREDELETE. Once
		// a planned model was freed by a notification dispatched during this
		// walk, no retained pointer is trusted for the rest of it: every later
		// row resolves cold through ObjectDB (a freed row's visibility intent
		// died with its node) and the plan rebinds on the next call, so one
		// free never drops a frame of presentation for the surviving rows.
		if (plan_model_lifetime_generation_ !=
				ObjectModel::lifetime_generation()) {
			plan_dirty_ = true;
		}
		ObjectModel *model = row.model;
		if (plan_dirty_) {
			model = Object::cast_to<ObjectModel>(
					ObjectDB::get_instance(row.node_id));
			if (model == nullptr) {
				continue;
			}
		}
		const int base = row.base;
		uint64_t profile_phase_start = p_profile != nullptr
				? Time::get_singleton()->get_ticks_usec()
				: 0;
		if (p_profile != nullptr) {
			++p_profile->rows;
		}
		stamp_match_terrain(model, p, base);
		stamp_destroy_phases(model, p, base);
		if ((output_channels_ & OUTPUT_TRANSFORM) != 0) {
			// Compare the six packed source floats before constructing either
			// the placement Basis or Transform3D.
			// Position is already Godot-space (x, z, -y); rotation is
			// mission-space degrees, built through the ONE placement convention
			// so a sim-driven entity sits exactly where placement would put it.
			const bool aim_owns_root =
					field_i(p, base,
							Simulation::PF_AIM_OVERLAY_VALID) != 0;
			const int rotation_field = aim_owns_root
					? Simulation::PF_AIM_BODY_PITCH_DEG
					: Simulation::PF_PITCH_DEG;
			const std::array<float, 6> next_stamp = {
				p[base + Simulation::PF_POS_X],
				p[base + Simulation::PF_POS_Y],
				p[base + Simulation::PF_POS_Z],
				p[base + rotation_field],
				p[base + rotation_field + 1],
				p[base + rotation_field + 2],
			};
			// The pass owns these transforms: compare against the last APPLIED
			// value instead of reading the node property back per row (the aim
			// leg below runs with drive_root_basis=false, so nothing else
			// rewrites the root between frames). On a cache miss (fresh plan —
			// including the every-call rebuild of revisionless fake sources)
			// fall back to one live read so an unchanged row never re-dirties
			// the node's tree, exactly like the GDScript live compare did.
			if (!row.transform_stamp_valid ||
					row.transform_stamp != next_stamp) {
				const Transform3D next = model->compose_entity_transform(
						bms_to_godot_basis(
								Vector3(next_stamp[3], next_stamp[4],
										next_stamp[5])),
						Vector3(next_stamp[0], next_stamp[1], next_stamp[2]));
				++stat_transform_builds_;
				if (row.transform_stamp_valid ||
						model->get_transform() != next) {
					model->set_transform(next);
					++stat_moved_;
				}
				if (placer_.is_valid()) {
					// The render node and terrain projection registry consume the
					// same present pose. The placer owns admission and exact-value
					// gating, so rejected rows and repeated snapshots remain free.
					placer_->update_static_terrain_shadow_source_transform(
							static_cast<MissionData::EntityKind>(row.entity_kind),
							row.entity_index, next);
				}
				row.transform_stamp = next_stamp;
				row.transform_stamp_valid = true;
			}
		}
		const bool present_visible =
				field_i(p, base, Simulation::PF_HIDDEN) == 0 &&
				field_i(p, base,
						Simulation::PF_LOCAL_VIEW_SUPPRESSED) == 0;
		// Camera submission: retail runs the presentation writers per
		// SUBMITTED model [orig: Terrain_RenderSectorModels @ 0x5c5d30
		// computes per drawn model; cull/submit @ Entity_RenderVehicleModel
		// @ 0x4407d0]. A row the renderer would not submit — sim-hidden,
		// occlusion-held, or bounds off-screen — skips the aim/part/CTRL
		// dispatch legs below. The applied stamps keep their last-dispatched
		// values, so the next submitted frame re-applies exactly what changed
		// while the row was out (set legs are per-submission re-asserts;
		// falling edges latched in the publish state still clear).
		model->set_parachute_deployed(field_i(p, base, Simulation::PF_PARACHUTE_DEPLOYED) != 0);
		model->set_slot_march_offset(Vector3(p[base + Simulation::PF_SLOT_MARCH_OFFSET_X],
				p[base + Simulation::PF_SLOT_MARCH_OFFSET_Y],
				p[base + Simulation::PF_SLOT_MARCH_OFFSET_Z]));
		const bool submitted = present_visible &&
				!model->is_occlusion_hidden() &&
				model->is_on_screen();
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->core_us += now - profile_phase_start;
			profile_phase_start = now;
			if (submitted) {
				++p_profile->submitted_rows;
			}
		}
		if (submitted)
			focal_sway_apply_typed(model, p, base);
		// Aim overlay with the capability lookups hoisted into the row plan and
		// the no-overlay clear gated to the valid->invalid edge (the node-side
		// setters no-op on repeats; these gates skip the dispatch itself).
		bool body_dependency_changed = false;
		if (submitted && stamp_right_hand_collapsed(model, p, base, row.rhc)) {
			++stat_rhc_dispatches_;
			body_dependency_changed = true;
		}
		if (submitted) {
			const int32_t aim_valid =
					field_i(p, base, Simulation::PF_AIM_OVERLAY_VALID);
			if (aim_valid != 0) {
				if (aim_payload_changed(p, base, row.aim_payload,
						row.aim_payload_valid)) {
					aim_apply_valid(model, snap, base, false);
					++stat_aim_dispatches_;
					body_dependency_changed = true;
				}
				row.aim_valid = 1;
			} else if (row.aim_valid != 0) {
				model->clear_aim_overlay();
				++stat_aim_dispatches_;
				row.aim_valid = 0;
				row.aim_payload_valid = false;
				body_dependency_changed = true;
			} else {
				row.aim_valid = 0;
			}
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->aim_us += now - profile_phase_start;
			profile_phase_start = now;
		}
		if (submitted && (output_channels_ & OUTPUT_PART_ANIM) != 0) {
			const int32_t active1_code =
					field_i(p, base, Simulation::PF_ACTIVE1);
			const int32_t active2_code =
					field_i(p, base, Simulation::PF_ACTIVE2);
			// The phase-domain predicate lives with the integrator
			// (world/ai.h part_anim_phase_active).
			const int32_t active1 =
					opennova::world::part_anim_phase_active(active1_code) ? 1 : 0;
			const int32_t active2 =
					opennova::world::part_anim_phase_active(active2_code) ? 1 : 0;
			const int32_t phase1 = active1 != 0
					? opennova::world::decode_present_part_anim_phase(
							snap, base, 1)
					: 0;
			const int32_t phase2 = active2 != 0
					? opennova::world::decode_present_part_anim_phase(
							snap, base, 2)
					: 0;
			const int32_t controls_valid =
					field_i(p, base,
							Simulation::PF_EMPLACED_CONTROLS_VALID) == 1
					? 1
					: 0;
			const int32_t vehicle_valid =
					field_i(p, base,
							Simulation::PF_VEHICLE_MOTION_VALID) == 1
					? 1
					: 0;
			const int32_t tex_team_valid =
					field_i(p, base, Simulation::PF_TEX_TEAM_VALID) == 1
					? 1
					: 0;
			const int32_t zone_valid =
					field_i(p, base, Simulation::PF_ZONE_CTRL_VALID) == 1
					? 1
					: 0;
			const int32_t lfp_valid =
					field_i(p, base,
							Simulation::PF_LFP_CAMPPERCENT_VALID) == 1
					? 1
					: 0;
			const int32_t heat_valid =
					field_i(p, base,
							Simulation::PF_WORLD_HEAT_GLOW_VALID) == 1
					? 1
					: 0;
			const int32_t door_count = std::clamp(
					field_i(p, base, Simulation::PF_DOOR_COUNT), 0, 30);
			const std::array<int32_t, CTRL_PUBLISH_COUNT>
					next_ctrl_publish_state = {
				active1,
				active2,
				controls_valid,
				vehicle_valid,
				tex_team_valid,
				zone_valid,
				lfp_valid,
				heat_valid,
				door_count,
			};
			const bool cold = !row.ctrl_publish_state_valid;
			const auto was_published = [&](CtrlPublishField field) {
				return row.ctrl_publish_state[
							   static_cast<size_t>(field)] != 0;
			};
			const bool set_part1 = active1 != 0;
			const bool set_part2 = active2 != 0;
			const bool clear_part1 = active1 == 0 &&
					(cold || was_published(CTRL_PUBLISH_PART1));
			const bool clear_part2 = active2 == 0 &&
					(cold || was_published(CTRL_PUBLISH_PART2));
			// A valid writer executes for every retail model submission. This
			// reasserts ownership after any intervening producer, while the
			// model-side setter makes an unchanged owner/value a true no-op.
			// Omitted writers clear only on a cold/falling edge.
			const bool emplaced_work = controls_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_EMPLACED);
			const bool vehicle_work = vehicle_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_VEHICLE);
			const bool zone_work = tex_team_valid != 0 || zone_valid != 0 ||
					lfp_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_TEX_TEAM) ||
					was_published(CTRL_PUBLISH_ZONE) ||
					was_published(CTRL_PUBLISH_LFP);
			const bool heat_work = heat_valid != 0 || cold ||
					was_published(CTRL_PUBLISH_HEAT);
			const bool door_work = door_count != 0 || cold ||
					was_published(CTRL_PUBLISH_DOORS);
			// The org0 skin bone-callback's DEATH register (the corpse fade)
			// rides the organic row's PF_DEATH_CTRL word, which the engine
			// fills with world::death_ctrl_register_value over the
			// authoritative dead flag and corpse timer. 0xFFFF is retail's
			// LIVE value, not a clear, so the writer publishes it on every
			// submission with no edge state; the model-side setter makes an
			// unchanged owner/value a no-op. Only organic rows carry the
			// register (the callback is the organic skin's).
			const bool death_work = row.entity_kind ==
					static_cast<int32_t>(opennova::world::EntityKind::Organic);
			const bool any_work = set_part1 || set_part2 ||
					clear_part1 || clear_part2 || emplaced_work ||
					vehicle_work || zone_work || heat_work || door_work ||
					death_work;
			if (any_work) {
				model->begin_ctrl_update();
			}
			if (death_work) {
				model->set_ctrl_override_native(names().owner_death, names().death,
						field_i(p, base, opennova::world::PF_DEATH_CTRL));
				++stat_control_dispatches_;
			}
			// A falling/cold EWEAP release precedes generic phase replay. This
			// preserves the master compatibility surface for a third-party node
			// that aliases a part channel onto EWEAP, without the old
			// unconditional clear/re-add on every valid frame. Production
			// ObjectModel uses fixed VEHICLE_SPECIAL1/2 and cannot alias it.
			if (emplaced_work && controls_valid == 0) {
				emplaced_clear_typed(model);
				stat_control_dispatches_ += 3;
			}
			// PANM phases are integrated by the engine [orig:
			// Entity_ApplyCommand @ 0x43ab60 case 0x22]. ACTIVE is the
			// publication bit: inactive releases this presenter's slot.
			if (set_part1) {
				model->set_part_phase(1, phase1);
				++stat_posed_;
				++stat_part_dispatches_;
			} else if (clear_part1) {
				model->clear_part_phase(1);
				++stat_part_dispatches_;
			}
			if (set_part2) {
				model->set_part_phase(2, phase2);
				++stat_posed_;
				++stat_part_dispatches_;
			} else if (clear_part2) {
				model->clear_part_phase(2);
				++stat_part_dispatches_;
			}
			if (emplaced_work && controls_valid != 0) {
				const int applied = emplaced_apply_typed(model, snap, base, false);
				stat_posed_ += applied;
				stat_control_dispatches_ += applied;
			}
			if (vehicle_work) {
				const int applied = vehicle_motion_apply_typed(model, snap, base);
				stat_posed_ += applied;
				stat_control_dispatches_ += 2;
			}
			if (zone_work) {
				const int applied = zone_team_apply_typed(model, snap, base);
				stat_posed_ += applied;
				stat_control_dispatches_ += 3;
			}
			if (heat_work) {
				const int applied = world_heat_apply_typed(model, snap, base);
				stat_posed_ += applied;
				++stat_control_dispatches_;
			}
			if (door_work) {
				// Retail writes exactly num_doors slots of the ordinal bus and
				// never clears [orig: build_bone_transforms @0x4E3070 loop
				// @0x4e312a..0x4e3145; BoneCallback_AnimatedBones_World @0x4E3180
				// loop @0x4e3201..0x4e3218]; releasing a shrunk row is the port's
				// retained-override bookkeeping. A cold row cannot enumerate what
				// it owned, so the owner-scoped release stands in for a
				// 30-register probe; a warm row shrinks by its published count.
				if (cold) {
					model->clear_ctrl_overrides_owned_native(names().owner_doors);
				}
				const int previous_count = cold ? 0 :
						row.ctrl_publish_state[CTRL_PUBLISH_DOORS];
				const int32_t *phases = nullptr;
				int available = 0;
				if (door_count > 0) {
					const int32_t row_index = static_cast<int32_t>(base / stride);
					while (door_cursor + 2 <= door_table_size &&
							door_table[door_cursor] < row_index) {
						door_cursor += 2 + std::max<int64_t>(0, door_table[door_cursor + 1]);
					}
					if (door_cursor + 2 <= door_table_size &&
							door_table[door_cursor] == row_index) {
						available = static_cast<int>(std::clamp<int64_t>(
								door_table[door_cursor + 1], 0,
								std::min<int64_t>(30, door_table_size - door_cursor - 2)));
						phases = door_table + door_cursor + 2;
					}
				}
				for (int i = 0; i < std::max(door_count, previous_count); ++i) {
					const int name = names().doors[i];
					if (i < door_count) {
						model->set_ctrl_override_native(names().owner_doors, name,
								i < available ? phases[i] : 0);
					} else {
						model->clear_ctrl_override_native(names().owner_doors, name);
					}
					++stat_control_dispatches_;
				}
			}
			if (any_work) model->end_ctrl_update();
			row.ctrl_publish_state = next_ctrl_publish_state;
			row.ctrl_publish_state_valid = true;
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->controls_us += now - profile_phase_start;
			profile_phase_start = now;
		}
		if ((output_channels_ & OUTPUT_VISIBILITY) != 0) {
			stamp_section_mask(model, p, base, row.destroyed_section_mask);
			// Death is not disappearance (corpses and husks keep rendering until
			// the sim despawns via PF_HIDDEN); the local first-person UseGun
			// parent's own world model is presentation-suppressed.
			// [orig: Entity_RenderVehicleModel @ 0x4407d0 cull/submit;
			// Flags&4 husk pick @ 0x413086].
			// Two-bit visibility ownership: this walk owns the model's
			// present bit (the sim's intent), the render-occlusion frame owns
			// its occlusion-hidden bit, and the node's visible flag is their
			// product — a claimed node stays hidden through a sim show and
			// releases onto the sim's intent. The node flag is reconciled live
			// so a foreign write (a Stop restore, a preview) never outlives
			// one frame.
			if (model->is_present_visible() != present_visible ||
					model->is_visible() !=
							(present_visible && !model->is_occlusion_hidden())) {
				model->set_present_visible(present_visible);
			}
			if (!present_visible) {
				++stat_hidden_;
			}
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->visibility_us += now - profile_phase_start;
			profile_phase_start = now;
		}
		// Unsubmitted models (hidden, occlusion-held, off-screen) skip skeletal
		// writes: the simulation resolves AI fire origins from its own pose
		// (world/pose_provider.h), never from a presented skeleton.
		if ((output_channels_ & OUTPUT_BODY_ANIM) != 0 && submitted) {
			if (p_profile != nullptr) {
				++p_profile->body_rows;
			}
			// Main-body skeletal clip: infantry poses to the exact anim-state
			// phase that produced root motion; PF_BODY_ANIM_SLOT is the coarse
			// fallback for compatible non-infantry nodes.
			const int32_t anim_state =
					field_i(p, base, Simulation::PF_ANIM_STATE);
			int32_t body_mode = BODY_NONE;
			int32_t body_selector = -1;
			int32_t body_phase = 0;
			int32_t body_source_selector = -1;
			int32_t body_source_phase = 0;
			float body_blend_weight = 1.0f;
			// Each channel poses its served ring entry (PF_ANIM_VARIANT /
			// PF_ANIM_SOURCE_VARIANT).
			int32_t body_variant = 0;
			int32_t body_source_variant = 0;
			String body_clip_key;
			String body_source_clip_key;
			{
				const String key =
						anim_state >= 0 ? infantry_key(anim_state) : String();
				const int32_t source_state = field_i(
						p, base, Simulation::PF_ANIM_SOURCE_STATE);
				const String source_key =
						source_state >= 0 ? infantry_key(source_state) : String();
				if (!key.is_empty()) {
					body_selector = anim_state;
					body_phase = field_i(
							p, base, Simulation::PF_ANIM_PHASE_TICKS);
					body_variant = field_i(p, base, Simulation::PF_ANIM_VARIANT);
					body_clip_key = key;
					const float target_weight =
							p[base + Simulation::PF_ANIM_BLEND_WEIGHT];
					if (source_state >= 0 && !source_key.is_empty() &&
							target_weight < 1.0f) {
						body_mode = BODY_BLEND_AT;
						body_source_selector = source_state;
						body_source_phase = field_i(
								p, base,
								Simulation::PF_ANIM_SOURCE_PHASE_TICKS);
						body_source_clip_key = source_key;
						body_blend_weight = target_weight;
						body_source_variant = field_i(
								p, base, Simulation::PF_ANIM_SOURCE_VARIANT);
					} else {
						body_mode = BODY_CLIP_AT;
					}
				} else if (source_state >= 0 && !source_key.is_empty()) {
					body_mode = BODY_CLIP_AT;
					body_selector = source_state;
					body_phase = field_i(
							p, base,
							Simulation::PF_ANIM_SOURCE_PHASE_TICKS);
					body_variant = field_i(
							p, base, Simulation::PF_ANIM_SOURCE_VARIANT);
					body_clip_key = source_key;
				}
			}
			if (body_mode == BODY_NONE) {
				const int32_t body_anim_slot =
						field_i(p, base, Simulation::PF_BODY_ANIM_SLOT);
				if (body_anim_slot >= 0) {
					body_mode = BODY_SLOT_AT;
					body_selector = body_anim_slot;
					body_phase = field_i(
							p, base, Simulation::PF_ANIM_PHASE_TICKS);
				}
			}
			const bool body_stamp_changed =
					!row.body_stamp_valid || row.body_mode != body_mode ||
					row.body_selector != body_selector ||
					row.body_phase != body_phase ||
					row.body_source_selector != body_source_selector ||
					row.body_source_phase != body_source_phase ||
					row.body_blend_weight != body_blend_weight ||
					row.body_variant != body_variant ||
					row.body_source_variant != body_source_variant;
			const bool force_external_pose =
					body_dependency_changed &&
					(body_mode == BODY_CLIP_AT ||
							body_mode == BODY_BLEND_AT ||
							body_mode == BODY_SLOT_AT);
			if (body_stamp_changed || force_external_pose) {
				switch (body_mode) {
					case BODY_CLIP_AT:
						model->play_body_clip_at(body_clip_key, body_phase, body_variant);
						++stat_body_dispatches_;
						break;
					case BODY_BLEND_AT:
						model->play_body_blend_at(body_source_clip_key,
								body_source_phase, body_clip_key, body_phase,
								body_blend_weight, body_source_variant, body_variant);
						++stat_body_dispatches_;
						break;
					case BODY_SLOT_AT:
						model->play_body_anim_at(body_selector, body_phase);
						++stat_body_dispatches_;
						break;
					case BODY_SLOT_PLAY:
					case BODY_NONE:
						break;
				}
				row.body_mode = body_mode;
				row.body_selector = body_selector;
				row.body_phase = body_phase;
				row.body_source_selector = body_source_selector;
				row.body_source_phase = body_source_phase;
				row.body_blend_weight = body_blend_weight;
				row.body_variant = body_variant;
				row.body_source_variant = body_source_variant;
				row.body_stamp_valid = true;
			}
		} else if ((output_channels_ & OUTPUT_BODY_ANIM) != 0) {
			// Desired state can keep changing while a hidden, non-muzzle row is
			// ineligible. Keep the applied stamp dirty so visibility catches up.
			row.body_stamp_valid = false;
		}
		if (p_profile != nullptr) {
			const uint64_t now = Time::get_singleton()->get_ticks_usec();
			p_profile->body_us += now - profile_phase_start;
			profile_phase_start = now;
		}
	}
}

namespace godot {
namespace {
Transform3D mine_transform(const opennova::world::CollisionMatrix &matrix) {
	const int32_t *m = matrix.m;
	constexpr float rotation_scale = 1.0f / 4194304.0f;
	// World mission XYZ -> Godot XZ-Y; model YZX -> mission XYZ.
	const Basis basis(Vector3(m[1], m[9], -static_cast<double>(m[5])) * rotation_scale,
			Vector3(m[2], m[10], -static_cast<double>(m[6])) * rotation_scale,
			Vector3(m[0], m[8], -static_cast<double>(m[4])) * rotation_scale);
	return Transform3D(basis, Vector3(m[3], m[11], -static_cast<double>(m[7])) *
			opennova::io::kInvFp16One);
}
}

void EntityPresenter::reset_minefields() {
	for (const auto &entry : minefield_nodes_) {
		if (auto *node = Object::cast_to<Node>(ObjectDB::get_instance(entry.second.node)))
			node->queue_free();
	}
	minefield_nodes_.clear();
	minefield_draws_.clear();
}

void EntityPresenter::present_minefields() {
	Simulation *simulation = sim();
	if (simulation == nullptr || placer_.is_null()) return;
	simulation->fill_minefield_draw_rows(minefield_draws_);
	for (auto &entry : minefield_nodes_) entry.second.seen = false;
	for (const auto &draw : minefield_draws_) {
		const uint32_t key = (static_cast<uint32_t>(draw.owner.packed) << 4) | draw.slot;
		auto &entry = minefield_nodes_[key];
		auto *node = Object::cast_to<ObjectModel>(ObjectDB::get_instance(entry.node));
		if (node != nullptr && entry.spawn_id != draw.registry_spawn_id) {
			node->queue_free();
			node = nullptr;
		}
		ObjectModel *source = index_.is_valid() ? index_->resolve(draw.bms_id,
				opennova::world::spawn_origin_kind(draw.spawn_origin),
				opennova::world::spawn_origin_index(draw.spawn_origin)) : nullptr;
		if (source == nullptr) source = resolve_wire_handle(draw.owner.packed);
		if (node == nullptr) {
			Node3D *parent = source != nullptr ? source : container();
			if (parent == nullptr) parent = this;
			node = placer_->build_model_from_graphic(String(draw.model.c_str()), String(),
					parent, String(), String(), true);
			if (node == nullptr) continue;
			node->set_name(String("MineMarker_") + String::num_int64(key));
			node->set_rigid_parts(true);
			entry.node = node->get_instance_id();
			entry.spawn_id = draw.registry_spawn_id;
		}
		entry.seen = true;
		node->set_authored_lod_owner(source, true);
		node->set_active_lod(source != nullptr ? source->get_active_lod() : 0);
		node->set_global_transform(mine_transform(draw.transform));
		// Parenting under the source makes this frame's later occlusion
		// verdict apply to the markers immediately. Their pose stays native.
		node->set_present_visible(source == nullptr || source->is_present_visible());
	}
	for (auto it = minefield_nodes_.begin(); it != minefield_nodes_.end();) {
		if (it->second.seen) { ++it; continue; }
		if (auto *node = Object::cast_to<Node>(ObjectDB::get_instance(it->second.node)))
			node->queue_free();
		it = minefield_nodes_.erase(it);
	}
}

// --- The local view's virtual display ------------------------------------------

ObjectModel *EntityPresenter::resolve_present_handle(int p_handle) const {
	if (ObjectModel *wire_node = resolve_wire_handle(p_handle)) {
		return wire_node;
	}
	// A per-frame caller asks for the same handle again: try the row that
	// answered last before walking the plan.
	const size_t count = rows_.size();
	if (present_handle_hint_ >= count || rows_[present_handle_hint_].handle != p_handle) {
		size_t found = count;
		for (size_t i = 0; i < count; ++i) {
			if (rows_[i].handle == p_handle) {
				found = i;
				break;
			}
		}
		if (found == count) {
			return nullptr;
		}
		present_handle_hint_ = found;
	}
	return Object::cast_to<ObjectModel>(
			ObjectDB::get_instance(rows_[present_handle_hint_].node_id));
}

ObjectModel *EntityPresenter::virtual_display_node() const {
	return virtual_display_id_.is_valid()
			? Object::cast_to<ObjectModel>(ObjectDB::get_instance(virtual_display_id_))
			: nullptr;
}

void EntityPresenter::reset_virtual_display() {
	if (ObjectModel *node = virtual_display_node()) {
		node->queue_free();
	}
	virtual_display_id_ = ObjectID();
	virtual_display_graphic_ = String();
	virtual_display_carrier_id_ = ObjectID();
}

void EntityPresenter::present_virtual_display(bool p_active, int p_carrier_handle,
		const String &p_model) {
	ObjectModel *node = virtual_display_node();
	ObjectModel *carrier = p_active && !p_model.is_empty() && placer_.is_valid()
			? resolve_present_handle(p_carrier_handle)
			: nullptr;
	if (carrier == nullptr) {
		// No swap this frame (on foot, another seat, the chase camera, no
		// authored display, the carrier gone): the built model waits hidden.
		if (node != nullptr && node->is_present_visible()) {
			node->set_present_visible(false);
		}
		return;
	}
	// A sibling of its carrier: the same world, the same render layers, and a
	// member of the container the light select and the mission unload walk.
	Node3D *parent = Object::cast_to<Node3D>(carrier->get_parent());
	if (parent == nullptr) parent = container();
	if (parent == nullptr) parent = this;
	// One model per graphic key AND carrier node: the light select reads a
	// model's light owner once, when it registers the container's members, so
	// another hull (or a respawned wire body) takes a fresh model. The placer
	// keeps the parsed graphic, so a rebuild never re-reads the file.
	const ObjectID carrier_id(carrier->get_instance_id());
	if (node != nullptr &&
			(virtual_display_graphic_ != p_model ||
					virtual_display_carrier_id_ != carrier_id ||
					node->get_parent() != parent)) {
		reset_virtual_display();
		node = nullptr;
	}
	if (node == nullptr) {
		virtual_display_graphic_ = p_model;
		virtual_display_carrier_id_ = carrier_id;
		node = placer_->build_model_from_graphic(p_model, String(), parent);
		if (node == nullptr) {
			return;
		}
		node->set_name("VirtualDisplay");
		// One entity, one submission: the display draws in its carrier's
		// place, so it shares the carrier's light query and groups.
		node->set_entity_light_owner(carrier);
		virtual_display_id_ = node->get_instance_id();
	}
	// ...and the carrier's lighting context (both setters are change-gated:
	// an unchanged frame writes nothing).
	node->set_thermal_entity_wave(carrier->get_thermal_entity_wave());
	node->set_entity_lighting_context(carrier->get_lighting_effect_scale(),
			carrier->is_interior_lerp(), carrier->get_interior_daylight());
	const Transform3D pose = carrier->get_global_transform();
	if (node->get_global_transform() != pose) {
		node->set_global_transform(pose);
	}
	// The carrier's collector gate still holds: a hull the occlusion frame
	// does not collect draws nothing, its display included.
	const bool collected = !carrier->is_occlusion_hidden() &&
			!wire_render_culled_.has(p_carrier_handle);
	if (node->is_present_visible() != collected) {
		node->set_present_visible(collected);
	}
}
} // namespace godot
