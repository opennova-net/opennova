#include "lights/effect_light_director.h"
#include "mission/mission_object_placer.h"
#include "mission/static_source_convert.h"

#include "mission/mission_root.h"

#include "env/mission_environment.h"
#include "env/weather.h"
#include "render/scene_overlay_compositor.h"
#include "lights/light_spawn.h"
#include "mission/mission_data.h"
#include "object/entity_ref.h"
#include "object/object_data.h"
#include "simulation/present_event_records.h"
#include "util/axes.h"
#include "util/color_convert.h"
#include "simulation/simulation.h"

#include <runtime/mission/placement_traits.h>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <algorithm>

#include <runtime/renderer/light_draw_frame.h>
#include <runtime/renderer/light_scene.h>
#include <runtime/renderer/light_spawn.h>
#include <runtime/renderer/scene_overlay.h>

using namespace godot;

namespace {

// The placer's container lives under the per-mission subtree (MissionRoot,
// ADR 0043 d9); a bare-scene test builds the same two-level shape.
constexpr const char *kMissionObjectsPath = "MissionRoot/MissionObjects";

// A node's world transform as the entity matrix an LGHT spawn reads
// (renderer::LightPlacement: the basis rows, then the origin).
opennova::renderer::LightPlacement light_placement(const Transform3D &p_transform) {
	opennova::renderer::LightPlacement placement{};
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column)
			placement[row * 3 + column] = static_cast<float>(p_transform.basis.rows[row][column]);
		placement[9 + row] = static_cast<float>(p_transform.origin[row]);
	}
	return placement;
}

} // namespace

EffectLightDirector::EffectLightDirector() {
	scene_.instantiate();
	spawner_ = std::make_unique<opennova::renderer::LightSpawner>(scene_->engine());
}

EffectLightDirector::~EffectLightDirector() = default;

void EffectLightDirector::setup(Node *p_world, const Ref<MissionObjectPlacer> &p_placer) {
	setup_with_provider(p_world, p_placer.ptr());
	placer_provider_ = p_placer;
}

void EffectLightDirector::setup_with_provider(Node *p_world, StaticSourceProvider *p_provider) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	placer_provider_.unref();
	provider_ = p_provider;
	static_rows_revision_ = -1;
}

void EffectLightDirector::set_scene(const TypedArray<ObjectModel> &p_models, MissionEnvironment *p_environment,
		Weather *p_weather) {
	scene_set_ = true;
	scene_models_.clear();
	for (int64_t i = 0; i < p_models.size(); ++i) scene_models_.push_back(p_models[i]);
	scene_environment_ = p_environment != nullptr ? ObjectID(p_environment->get_instance_id()) : ObjectID();
	scene_weather_ = p_weather != nullptr ? ObjectID(p_weather->get_instance_id()) : ObjectID();
	reg_dirty_ = true;
}

std::vector<opennova::mission::StaticEffectSource> EffectLightDirector::_static_sources() const {
	return provider_ != nullptr ? provider_->static_item_effect_sources()
			: std::vector<opennova::mission::StaticEffectSource>();
}

std::vector<opennova::mission::StaticLightDrawSource> EffectLightDirector::_static_draw_sources() const {
	return provider_ != nullptr ? provider_->static_light_draw_sources()
			: std::vector<opennova::mission::StaticLightDrawSource>();
}

int64_t EffectLightDirector::_static_draw_source_revision() const {
	return provider_ != nullptr ? static_cast<int64_t>(provider_->static_light_draw_source_revision()) : 0;
}

Node *EffectLightDirector::_world() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(world_id_));
}

MissionRoot *EffectLightDirector::_runtime() const {
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	return Object::cast_to<MissionRoot>(static_cast<Object *>(world->call("get_runtime")));
}

Ref<Simulation> EffectLightDirector::_sim() const {
	MissionRoot *runtime = _runtime();
	if (runtime == nullptr) {
		return Ref<Simulation>();
	}
	return runtime->get_sim();
}

MissionEnvironment *EffectLightDirector::_environment() const {
	if (scene_set_) {
		return Object::cast_to<MissionEnvironment>(ObjectDB::get_instance(scene_environment_));
	}
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	return Object::cast_to<MissionEnvironment>(
			static_cast<Object *>(world->call("get_environment_node")));
}

Weather *EffectLightDirector::_weather() const {
	if (scene_set_) {
		return Object::cast_to<Weather>(ObjectDB::get_instance(scene_weather_));
	}
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	return Object::cast_to<Weather>(static_cast<Object *>(world->call("get_weather_node")));
}

Node *EffectLightDirector::_mission_objects() const {
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	return world->get_node_or_null(NodePath(kMissionObjectsPath));
}

Ref<LightScene> EffectLightDirector::scene() const {
	return scene_;
}

void EffectLightDirector::reset() {
	spawner_->reset();
	scene()->clear();
	static_sources_snapshot_.clear();
	static_rows_revision_ = -1;
	reg_models_.clear();
	reg_owners_.clear();
	reg_robj_scoped_.clear();
	reg_dirty_ = true;
	_clear_coronas();
}

void EffectLightDirector::reattach() {
	reset();
	Node *container = scene_set_ ? nullptr : _mission_objects();
	TypedArray<Node> children;
	if (scene_set_) {
		children = scene_models_;
	} else if (container != nullptr) {
		children = container->get_children();
	}
	static_sources_snapshot_ = _static_sources();
	static_rows_revision_ = -1;
	// Every building identity before any blink containment resolves
	// (renderer::LightSpawner::register_building carries the witness); a
	// batched static's tagged id wins its BMS id.
	for (size_t source_index = 0; source_index < static_sources_snapshot_.size(); ++source_index) {
		spawner_->register_building(static_sources_snapshot_[source_index].bms_id,
				opennova::renderer::LightOwnerIds::static_source(static_cast<int64_t>(source_index)));
	}
	for (int64_t i = 0; i < children.size(); ++i) {
		ObjectModel *node = Object::cast_to<ObjectModel>(static_cast<Object *>(children[i]));
		if (node != nullptr && node->get_entity_ref().is_valid()) {
			_register_entity(node, owner_id_for_node(node));
		}
	}
	for (int64_t i = 0; i < children.size(); ++i) {
		ObjectModel *node = Object::cast_to<ObjectModel>(static_cast<Object *>(children[i]));
		if (node == nullptr || node->get_entity_ref().is_null()) {
			continue;
		}
		on_wire_node_spawned(node, -1, 0);
	}
	for (size_t source_index = 0; source_index < static_sources_snapshot_.size(); ++source_index) {
		const auto &source = static_sources_snapshot_[source_index];
		const Ref<ObjectData> data = provider_ != nullptr
				? provider_->static_source_object_data(source.asset_id) : Ref<ObjectData>();
		if (data.is_null() || data->get_light_count() <= 0) {
			continue;
		}
		// Batched statics are entities too: a subobject record binds to this
		// tagged owner and the atlas draw row declares the same identity
		// (renderer::LightSpawner::spawn_entity).
		opennova::renderer::EntityLightSpawn spawn;
		spawn.owner_entity =
				opennova::renderer::LightOwnerIds::static_source(static_cast<int64_t>(source_index));
		spawn.records = data->native_model().lights;
		spawn.record_count = data->native_model().light_count;
		spawn.placement = source.world_transform;
		spawn.building = _record_is_building(source.kind, source.item_id);
		// Retail skips the blink query for a Building-type def's own records;
		// every other static (the pool-2 decorations too) resolves containment
		// once at its placement origin.
		if (!spawn.building) {
			const BlinkOwner blink =
					_blink_owner_at(from_static_source_transform(source.world_transform).origin);
			spawn.blink_owner = static_cast<uint64_t>(blink.owner);
			spawn.blink_section = blink.section;
		}
		spawner_->spawn_entity(spawn);
	}
}

int64_t EffectLightDirector::owner_id_for_node(ObjectModel *p_node) {
	if (p_node == nullptr) {
		return 0;
	}
	if (ObjectModel *owner = p_node->get_entity_light_owner()) p_node = owner;
	const Ref<EntityRef> ref = p_node->get_entity_ref();
	const int wire = ref.is_valid() ? ref->get_wire_handle() : -1;
	return wire >= 0 ? LightScene::owner_id_for_wire(wire)
					 : static_cast<int64_t>(p_node->get_instance_id());
}

void EffectLightDirector::_register_entity(ObjectModel *p_node, int64_t p_owner_id) {
	const Ref<EntityRef> ref = p_node->get_entity_ref();
	if (ref.is_null() || ref->get_bms_id() == 0 || spawner_->owner_for_bms(ref->get_bms_id()) != 0) {
		return;
	}
	spawner_->register_building(ref->get_bms_id(), static_cast<uint64_t>(p_owner_id));
}

void EffectLightDirector::on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id) {
	(void)p_kind;
	(void)p_item_id;
	if (p_node == nullptr) {
		return;
	}
	const int64_t owner_id = owner_id_for_node(p_node);
	_register_entity(p_node, owner_id);
	const Ref<ObjectData> data = p_node->get_object_data();
	if (data.is_null() || data->get_light_count() <= 0) {
		return;
	}
	// The entity's one walk (renderer::LightSpawner::spawn_entity): the
	// records through the entity matrix once, the building gate on the
	// items.def type (a pool-2 decoration queries like any entity), and the
	// one blink query at the entity's position.
	const Ref<EntityRef> ref = p_node->get_entity_ref();
	opennova::renderer::EntityLightSpawn spawn;
	spawn.owner_entity = static_cast<uint64_t>(owner_id);
	spawn.records = data->native_model().lights;
	spawn.record_count = data->native_model().light_count;
	const Transform3D transform = p_node->get_global_transform();
	spawn.placement = light_placement(transform);
	spawn.building = ref.is_valid() && _record_is_building(ref->get_kind(), ref->get_item_id());
	if (!spawn.building && !spawner_->walked(spawn.owner_entity)) {
		const BlinkOwner blink = _blink_owner_at(transform.origin);
		spawn.blink_owner = static_cast<uint64_t>(blink.owner);
		spawn.blink_section = blink.section;
	}
	spawner_->spawn_entity(spawn);
}

void EffectLightDirector::on_wire_entity_destroyed(int p_wire_handle) {
	spawner_->destroy_entity(opennova::renderer::LightOwnerIds::wire(p_wire_handle));
}

int64_t EffectLightDirector::spawn_light_record(const Ref<ModelLight> &p_info,
		const Transform3D &p_world_transform, int64_t p_owner_id,
		const PackedInt64Array &p_blink_owner, bool p_spawner_is_building) {
	if (p_info.is_null()) {
		return 0;
	}
	BlinkOwner blink_owner;
	if (p_blink_owner.size() >= 2) {
		blink_owner.owner = p_blink_owner[0];
		blink_owner.section = static_cast<int>(p_blink_owner[1]);
	}
	return _spawn_light_at(p_info, p_world_transform.xform(p_info->get_position()), p_owner_id,
			blink_owner, p_spawner_is_building);
}

int64_t EffectLightDirector::_spawn_light_at(const Ref<ModelLight> &p_info,
		const Vector3 &p_world_pos, int64_t p_owner_id, const BlinkOwner &p_blink_owner,
		bool p_spawner_is_building) {
	Ref<ModelLightSpawn> spawn = ModelLightSpawn::make(p_world_pos, p_info->get_atten_end());
	spawn->set_style(p_info->get_colorgen_style());
	spawn->set_phase(p_info->get_colorgen_phase());
	spawn->set_rate(p_info->get_colorgen_rate());
	spawn->set_color_start(p_info->get_color_start());
	spawn->set_color_end(p_info->get_color_end());
	spawn->attached(p_info->get_subobject(), p_owner_id, p_spawner_is_building);
	if (p_blink_owner.valid()) {
		spawn->in_blink_box(p_blink_owner.owner, p_blink_owner.section);
	}
	spawn->set_disable_corona(p_info->get_disable_corona());
	spawn->set_disable_terrain(p_info->get_disable_lightterrain());
	spawn->set_disable_objects(p_info->get_disable_lightobjects());
	return scene()->spawn_model_light(spawn);
}

// The blink-box owner at one world point: retail runs ONE query at the
// spawning entity's position before walking its LGHT records, and slot 0's
// hit names the containing building + section every unattached record binds
// to (engine: runtime/renderer/light_scene.h). Owner 0 outdoors. Both individual ObjectModels and batched
// buildings resolve into the active-group domain their respective draw
// contexts declare.
EffectLightDirector::BlinkOwner EffectLightDirector::_blink_owner_at(const Vector3 &p_world_pos) {
	BlinkOwner out;
	const Ref<Simulation> sim = _sim();
	if (sim.is_null()) {
		return out;
	}
	const PackedInt64Array hit = sim->query_blink_owner_at(p_world_pos);
	if (hit.size() < 2) {
		return out;
	}
	const int64_t owner = _owner_id_for_bms(static_cast<int>(hit[0]));
	if (owner == 0) {
		return out;
	}
	out.owner = owner;
	out.section = static_cast<int>(hit[1]);
	return out;
}

int64_t EffectLightDirector::interior_owner_for_bms(int p_bms_id) {
	return _owner_id_for_bms(p_bms_id);
}

bool EffectLightDirector::_record_is_building(int p_kind, int p_item_id) const {
	const Ref<ItemDatabase> items =
			provider_ != nullptr ? provider_->static_source_item_db() : Ref<ItemDatabase>();
	const bool has_def = items.is_valid() && items->has_item(p_item_id);
	return opennova::mission::placed_record_is_building(p_kind, has_def,
			has_def ? items->get_item_type(p_item_id) : 0);
}

// The owner id a containing building's bms_id resolves to: the SAME id its
// own draw context declares, or owner gating never matches
// (renderer::LightSpawner::owner_for_bms).
int64_t EffectLightDirector::_owner_id_for_bms(int p_bms_id) {
	return static_cast<int64_t>(spawner_->owner_for_bms(p_bms_id));
}

// The engine's light block gain (EnvironmentState::light_gain), the unlit 1
// with no environment.
Vector3 EffectLightDirector::light_gain() const {
	MissionEnvironment *env = _environment();
	if (env == nullptr) {
		return Vector3(1, 1, 1);
	}
	const opennova::env::Rgb gain = env->state().light_gain();
	return Vector3(gain.r, gain.g, gain.b);
}

// One frame of the static atlas rows: handed to the scene when the placer's
// revision moved, then selected (the scene publishes the RGBAF payload the
// MultiMesh rows read through INSTANCE_CUSTOM.x).
void EffectLightDirector::_render_static_light_rows(const Vector3 &p_gain, Weather *p_weather,
		int p_time_ms) {
	const int64_t revision = _static_draw_source_revision();
	if (revision != static_rows_revision_) {
		_rebuild_static_light_rows(revision);
		static_rows_revision_ = revision;
	}
	scene()->select_static_rows(p_gain, p_time_ms, p_weather);
}

// The immutable-index static atlas rows. The placer owns row identity and
// entity bounds; each row declares the engine's static-row groups
// (renderer::static_light_row_groups) and its lighting lane
// (renderer::static_light_row_lane).
void EffectLightDirector::_rebuild_static_light_rows(int64_t p_revision) {
	const auto descriptors = _static_draw_sources();
	int row_count = 0;
	for (const auto &descriptor : descriptors) {
		row_count = std::max(row_count, descriptor.atlas_row + 1);
	}
	std::vector<opennova::renderer::StaticLightRow> rows(static_cast<size_t>(row_count));
	for (const auto &descriptor : descriptors) {
		const int atlas_row = descriptor.atlas_row;
		const int source_index = descriptor.source_index;
		if (atlas_row < 0 || atlas_row >= row_count || source_index < 0 ||
				source_index >= static_cast<int>(static_sources_snapshot_.size())) {
			continue;
		}
		const auto &source = static_sources_snapshot_[source_index];
		opennova::renderer::StaticLightRow &row = rows[static_cast<size_t>(atlas_row)];
		row.query.position_fixed = opennova::renderer::light_placement_origin_fixed(source.world_transform);
		row.query.bound_radius_fixed = source.entity_bound_radius_q16;
		row.active = descriptor.active;
		// A building is a Building-type DEF: the pool-2 decorations and foliage
		// draw in the entity wave (mission::placed_record_is_building).
		opennova::renderer::StaticLightRowInputs inputs;
		inputs.static_owner = opennova::renderer::LightOwnerIds::static_source(source_index);
		inputs.robj_index = descriptor.robj_index;
		inputs.is_building = _record_is_building(descriptor.kind >= 0 ? descriptor.kind : source.kind,
				descriptor.item_id != 0 ? descriptor.item_id : source.item_id);
		if (!inputs.is_building) {
			const BlinkOwner interior =
					_blink_owner_at(from_static_source_transform(source.world_transform).origin);
			inputs.blink_hit = interior.valid();
			inputs.blink_owner_entity = static_cast<uint64_t>(interior.owner);
			inputs.blink_section = interior.section;
		}
		row.groups = opennova::renderer::static_light_row_groups(inputs);
		row.lane = opennova::renderer::static_light_row_lane(inputs.is_building,
				descriptor.robj_index, descriptor.light_transfer, inputs.blink_hit,
				source.model_floor_q16, source.entity_bound_radius_q16);
	}
	scene()->set_static_rows(p_revision, std::move(rows));
}

void EffectLightDirector::render_frame(Camera3D *p_camera, int64_t p_time_ms,
		const TypedArray<ObjectModel> &p_viewmodel_parts, bool p_run_census) {
	if (p_camera == nullptr) {
		scene()->clear_render_output();
		_clear_coronas();
		return;
	}
	const Vector3 gain = light_gain();
	MissionEnvironment *env = _environment();
	Weather *weather = _weather();
	const Vector3 cam_pos = p_camera->get_camera_transform().origin;
	const int time_ms = static_cast<int>(p_time_ms);
	_clear_frame_draws();
	_render_static_light_rows(gain, weather, time_ms);
	if (_walk_models()) {
		for (int64_t i = 0; i < reg_models_.size(); ++i) {
			ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(reg_models_[i]));
			// Every model the view draws selects (no camera distance gates the
			// select, renderer/light_draw_frame.h).
			if (model == nullptr || !model->is_visible_in_tree()) {
				continue;
			}
			_push_model_draw(model, i);
		}
	}
	// The first-person parts inherit the LOCAL PLAYER's interior group, so
	// the room's lights reach the arms and weapon the same way they reach
	// the third-person body standing there.
	const BlinkOwner viewmodel_interior = _local_player_interior_group();
	Vector3 local_entity_position;
	int32_t local_entity_radius_q16 = 0;
	const Ref<Simulation> sim = _sim();
	const bool has_local_query = sim.is_valid() &&
			sim->local_player_light_query(local_entity_position, local_entity_radius_q16);
	for (int64_t i = 0; i < p_viewmodel_parts.size(); ++i) {
		ObjectModel *part = Object::cast_to<ObjectModel>(static_cast<Object *>(p_viewmodel_parts[i]));
		if (part == nullptr || !part->is_visible_in_tree()) {
			continue;
		}
		frame_models_.push_back(part);
		// Both first-person submits inherit the query made for the player,
		// before viewmodel camera offsets (native EntityLightQuery contract).
		frame_entity_positions_.push_back(has_local_query ? local_entity_position : part->get_global_position());
		frame_entity_bound_radii_q16_.push_back(has_local_query ? local_entity_radius_q16 : part->get_entity_bound_radius_q16());
		// The first-person pass declares no owner group: it only sets the
		// interior group (retail Player_RenderFirstPersonViewModel
		// @0x4DEEA4..0x4DEF3C), so an owned light -- the player's own muzzle
		// glow included -- never reaches the arms or the FP gun.
		frame_owners_.push_back(0);
		frame_robj_scoped_.push_back(0);
		frame_interior_owners_.push_back(viewmodel_interior.owner);
		frame_interior_sections_.push_back(viewmodel_interior.section);
	}
	// Census select first (report rows for F3 and the seam tests), then the
	// gameplay per-model pass — its mode/isolation stamp is what the report
	// ends the frame with. The census publishes nothing to materials, so the
	// hot caller skips it while the F3 Stats capture is off; a diagnostics
	// read refreshes it on demand (run_census_now).
	census_cam_pos_ = cam_pos;
	census_time_ms_ = time_ms;
	if (p_run_census) {
		scene()->render_frame(cam_pos, kCensusRadius, gain, time_ms, weather);
		census_stale_ = false;
	} else {
		census_stale_ = true;
	}
	scene()->render_model_frame(frame_models_, frame_owners_, frame_interior_owners_,
			frame_interior_sections_, frame_robj_scoped_, gain, time_ms, weather,
			frame_entity_positions_, frame_entity_bound_radii_q16_);
	_render_coronas(p_camera, gain, time_ms, weather, frame_models_, frame_owners_, env);
}

void EffectLightDirector::_clear_frame_draws() {
	frame_models_.clear();
	frame_entity_positions_.clear();
	frame_entity_bound_radii_q16_.clear();
	frame_owners_.clear();
	// interior_*: the second witnessed group — the building each draw
	// currently stands inside, plus that blink volume's section (the engine
	// pool's LightActiveGroups; the entity lighting feed stamps it on the
	// entity's model beside its lighting context).
	frame_interior_owners_.clear();
	frame_interior_sections_.clear();
	frame_robj_scoped_.clear();
}

void EffectLightDirector::_push_model_draw(ObjectModel *p_model, int64_t p_reg_index) {
	frame_models_.push_back(p_model);
	ObjectModel *entity_model = p_model->get_entity_light_owner();
	if (entity_model == nullptr) entity_model = p_model;
	frame_entity_positions_.push_back(entity_model->get_global_position());
	frame_entity_bound_radii_q16_.push_back(entity_model->get_entity_bound_radius_q16());
	frame_owners_.push_back(reg_owners_[p_reg_index]);
	frame_robj_scoped_.push_back(reg_robj_scoped_[p_reg_index]);
	int64_t interior_owner = 0;
	int interior_section = 0;
	const int interior_bms = entity_model->get_interior_light_group_bms();
	if (interior_bms != 0) {
		const int64_t owner = _owner_id_for_bms(interior_bms);
		if (owner != 0) {
			interior_owner = owner;
			interior_section = entity_model->get_interior_light_group_section();
		}
	}
	frame_interior_owners_.push_back(interior_owner);
	frame_interior_sections_.push_back(interior_section);
}

void EffectLightDirector::render_inset_frame(Camera3D *p_camera, int64_t p_time_ms) {
	if (p_camera == nullptr) {
		release_inset_frame();
		return;
	}
	const Vector3 gain = light_gain();
	Weather *weather = _weather();
	const Transform3D camera_transform = p_camera->get_camera_transform();
	const int time_ms = static_cast<int>(p_time_ms);
	// The registry rows the Inset pass draws (its twins, or the node both
	// views draw), gathered around the Inset eye like the main pass's.
	_clear_frame_draws();
	if (_walk_models()) {
		for (int64_t i = 0; i < reg_models_.size(); ++i) {
			ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(reg_models_[i]));
			if (model == nullptr || !model->is_inset_view_drawn()) {
				continue;
			}
			_push_model_draw(model, i);
		}
	}
	scene()->render_inset_model_frame(frame_models_, frame_owners_, frame_interior_owners_,
			frame_interior_sections_, frame_robj_scoped_, gain, time_ms, weather,
			frame_entity_positions_, frame_entity_bound_radii_q16_);
	// The Inset's own walk: its prologue advanced the phase once more.
	corona_frame_ = (corona_frame_ + 1) & 3;
	scene()->collect_inset_corona_rows(camera_transform.origin,
			-camera_transform.basis.get_column(2), gain, time_ms, corona_frame_, weather,
			frame_models_, frame_owners_, _corona_fog(_environment()), inset_coronas_);
}

void EffectLightDirector::release_inset_frame() {
	inset_coronas_.clear();
}

void EffectLightDirector::run_census_now() {
	if (!census_stale_) {
		return;
	}
	// census_frame refreshes the rows without restamping the report's mode:
	// a report read with the F3 stats off keeps saying what the gameplay
	// pass (render_model_frame) reported.
	scene()->census_frame(census_cam_pos_, kCensusRadius, light_gain(), census_time_ms_, _weather());
	census_stale_ = false;
}

bool EffectLightDirector::_walk_models() {
	if (scene_set_) {
		if (reg_dirty_) {
			_rebuild_model_registry(scene_models_);
		}
		return true;
	}
	Node *container = _mission_objects();
	if (container == nullptr) {
		return false;
	}
	_ensure_model_registry(container);
	return true;
}

// Rebuild the MissionObjects walk registry only when membership changed.
// Owner identity and kind come off entity_ref, stamped once before a node's
// first light frame, so registration-time reads hold for its tree lifetime.
void EffectLightDirector::_ensure_model_registry(Node *p_container) {
	const uint64_t container_id = p_container->get_instance_id();
	if (container_id != reg_container_id_) {
		reg_container_id_ = container_id;
		reg_dirty_ = true;
		const Callable on_changed(this, "_on_container_membership_changed");
		if (!p_container->is_connected("child_entered_tree", on_changed)) {
			p_container->connect("child_entered_tree", on_changed);
		}
		if (!p_container->is_connected("child_exiting_tree", on_changed)) {
			p_container->connect("child_exiting_tree", on_changed);
		}
	}
	if (reg_dirty_) {
		_rebuild_model_registry(p_container->get_children());
	}
}

void EffectLightDirector::_on_container_membership_changed(Node *p_node) {
	(void)p_node;
	reg_dirty_ = true;
}

void EffectLightDirector::_rebuild_model_registry(const TypedArray<Node> &p_models) {
	reg_models_.clear();
	reg_owners_.clear();
	reg_robj_scoped_.clear();
	for (int64_t i = 0; i < p_models.size(); ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(static_cast<Object *>(p_models[i]));
		if (model == nullptr) {
			continue;
		}
		ObjectModel *entity_model = model->get_entity_light_owner();
		if (entity_model == nullptr) entity_model = model;
		const Ref<EntityRef> ref = entity_model->get_entity_ref();
		reg_models_.push_back(ObjectID(model->get_instance_id()));
		// A batched static's tagged id wins its BMS id (the registry).
		const int64_t registered = ref.is_valid() ? _owner_id_for_bms(ref->get_bms_id()) : 0;
		reg_owners_.push_back(registered != 0 ? registered : owner_id_for_node(model));
		// Only a building pass draw re-scopes its owner per ROBJ: a
		// Building-type def, not every pool-2 record.
		reg_robj_scoped_.push_back(
				ref.is_valid() && _record_is_building(ref->get_kind(), ref->get_item_id()) ? 1 : 0);
	}
	reg_dirty_ = false;
}

// The local player's interior group; owner 0 outdoors. The player is a
// spawned entity with no bms_id, so it never appears in the interior-group
// rows.
EffectLightDirector::BlinkOwner EffectLightDirector::_local_player_interior_group() {
	BlinkOwner out;
	const Ref<Simulation> sim = _sim();
	if (sim.is_null()) {
		return out;
	}
	const Simulation::InteriorGroup hit = sim->local_player_interior_group_words();
	if (hit.bms_id == 0) {
		return out;
	}
	const int64_t owner = _owner_id_for_bms(hit.bms_id);
	if (owner == 0) {
		return out;
	}
	out.owner = owner;
	out.section = hit.section;
	return out;
}

// The corona device leg: fetch this frame's additive quads from the portable
// walk for the post-particle overlay stage (append_overlay). The
// models/owners arrays are the per-model pass's own walk — models with an
// occlusion section-mask verdict gate their owned coronas on the
// visible-section bit (engine: runtime/renderer/light_scene.h);
// the env fog rides in as the fog-to-black fold
// (engine: runtime/renderer/light_scene.h).
void EffectLightDirector::_render_coronas(Camera3D *p_camera, const Vector3 &p_gain,
		int p_time_ms, Weather *p_weather, const TypedArray<Node3D> &p_models, const PackedInt64Array &p_owners,
		MissionEnvironment *p_env) {
	// The main scene's mirror walks at the phase ahead of this advance, the Inset's mirror at this
	// one (render_mirror_coronas).
	mirror_corona_phase_[0] = corona_frame_;
	corona_frame_ = (corona_frame_ + 1) & 3;
	mirror_corona_phase_[1] = corona_frame_;
	const Transform3D camera_transform = p_camera->get_camera_transform();
	scene()->collect_corona_rows(camera_transform.origin,
			-camera_transform.basis.get_column(2), p_gain, p_time_ms, corona_frame_, p_weather,
			p_models, p_owners, _corona_fog(p_env));
	coronas_ = scene()->last_corona_quads();
}

Ref<EnvLightValues> EffectLightDirector::_corona_fog(MissionEnvironment *p_env) const {
	if (p_env == nullptr) {
		return Ref<EnvLightValues>();
	}
	const Ref<EnvLightState> state = p_env->get_light_state();
	return state.is_valid() ? state->get_values() : Ref<EnvLightValues>();
}

void EffectLightDirector::render_mirror_coronas(Camera3D *p_mirror, Camera3D *p_inset_mirror,
		int64_t p_time_ms) {
	MissionEnvironment *env = _environment();
	Camera3D *const mirrors[2] = { p_mirror, p_inset_mirror };
	if (env == nullptr || (p_mirror == nullptr && p_inset_mirror == nullptr)) {
		mirror_coronas_[0].clear();
		mirror_coronas_[1].clear();
		return;
	}
	// The reflected subscene's dry block, the primary fog the walk folds to black.
	const Ref<EnvLightValues> light = _corona_fog(env);
	const opennova::env::SceneFogValues fog = env->state().build_water_mirror_fog();
	if (mirror_corona_fog_.is_null()) {
		mirror_corona_fog_.instantiate();
	}
	mirror_corona_fog_->fog_enabled = light.is_valid() && light->fog_enabled;
	mirror_corona_fog_->fog_type = fog.type;
	mirror_corona_fog_->fog_start = fog.start;
	mirror_corona_fog_->fog_end = fog.end;
	const Vector3 gain = light_gain();
	Weather *weather = _weather();
	for (int view = 0; view < 2; ++view) {
		Camera3D *mirror = mirrors[view];
		if (mirror == nullptr) {
			mirror_coronas_[view].clear();
			continue;
		}
		// Over the main walk's draw list: each owner's gate reads that mirror's own masks.
		const Transform3D eye = mirror->get_camera_transform();
		scene()->collect_mirror_corona_rows(view, eye.origin, -eye.basis.get_column(2), gain,
				static_cast<int>(p_time_ms), mirror_corona_phase_[view], weather, frame_models_,
				frame_owners_, mirror_corona_fog_, mirror_coronas_[view]);
	}
}

void EffectLightDirector::_clear_coronas() {
	coronas_.clear();
	inset_coronas_.clear();
	mirror_coronas_[0].clear();
	mirror_coronas_[1].clear();
}

void EffectLightDirector::append_overlay(SceneOverlaySubmission &r_submission) {
	if (coronas_.empty() && inset_coronas_.empty() && mirror_coronas_[0].empty() &&
			mirror_coronas_[1].empty()) {
		return;
	}
	// The coronas draw after particle pass B, in the main scene and in each
	// mirror's reflected scene (retail EffectWorld_RenderLightCoronas(1)
	// from Terrain_RenderWorldScene @ 0x5c96ad and from
	// Water_RenderReflectedWorldScene @ 0x5c85fd); the weapon Inset pass's
	// own walk and each mirror's ride their own slots (scene_overlay.h
	// kInsetOverlayOrder, kMirrorOverlayOrder, kInsetMirrorOverlayOrder).
	const uint32_t texture = r_submission.texture_index(_corona_texture(),
			opennova::renderer::texture_stage_flags(opennova::renderer::TextureStage::LightCorona));
	opennova::renderer::append_corona_overlay(coronas_, texture, r_submission.frame);
	opennova::renderer::append_corona_overlay(inset_coronas_, texture, r_submission.frame,
			opennova::renderer::SceneOverlaySlot::InsetLightCoronas);
	opennova::renderer::append_corona_overlay(mirror_coronas_[0], texture, r_submission.frame,
			opennova::renderer::SceneOverlaySlot::MirrorLightCoronas);
	opennova::renderer::append_corona_overlay(mirror_coronas_[1], texture, r_submission.frame,
			opennova::renderer::SceneOverlaySlot::InsetMirrorLightCoronas);
}

Ref<ImageTexture> EffectLightDirector::_corona_texture() {
	if (corona_texture_.is_valid()) {
		return corona_texture_;
	}
	const int size = LightScene::corona_texture_size();
	const Ref<Image> image = Image::create_from_data(size, size, false, Image::FORMAT_RGBA8,
			LightScene::corona_texture_rgba8());
	// Built from pixels, so the device's texture carries the box-filtered pixel
	// chain the corona's draws sample with point mips
	// (renderer::TextureStage::LightCorona).
	image->generate_mipmaps();
	corona_texture_ = ImageTexture::create_from_image(image);
	return corona_texture_;
}

void EffectLightDirector::advance_fixed_tick() {
	scene()->advance_fixed_tick();
}

void EffectLightDirector::on_muzzle_fire(int64_t p_shooter_handle, const Vector3 &p_world_pos) {
	// The shooter's cached word and the re-arm rule (renderer::LightSpawner::muzzle_fire).
	spawner_->muzzle_fire(opennova::renderer::LightOwnerIds::wire(p_shooter_handle),
			godot_to_mission_q16(p_world_pos));
}

void EffectLightDirector::on_impact_light(const Vector3 &p_world_pos, float p_radius,
		const Color &p_color, int p_duration_ticks) {
	scene()->spawn_impact_flash(p_world_pos, p_radius, p_color, p_duration_ticks);
}

void EffectLightDirector::on_death_light(const Vector3 &p_world_pos, float p_radius) {
	scene()->spawn_death_flash(p_world_pos, p_radius);
}

void EffectLightDirector::sync_round_glow_records(const Array &p_rows) {
	std::vector<opennova::world::RoundGlowRow> rows;
	rows.reserve(static_cast<size_t>(p_rows.size()));
	for (int64_t i = 0; i < p_rows.size(); ++i) {
		const Ref<RoundGlowRow> row = p_rows[i];
		if (row.is_valid()) {
			rows.push_back(row->value());
		}
	}
	sync_round_glows(rows);
}

void EffectLightDirector::sync_round_glows(const std::vector<opennova::world::RoundGlowRow> &p_rows) {
	spawner_->sync_round_glows(p_rows);
}

Ref<EffectLightReport> EffectLightDirector::get_report() {
	run_census_now();
	return scene()->get_report();
}

void EffectLightDirector::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "world", "placer"), &EffectLightDirector::setup);
	ClassDB::bind_method(D_METHOD("reset"), &EffectLightDirector::reset);
	ClassDB::bind_method(D_METHOD("reattach"), &EffectLightDirector::reattach);
	ClassDB::bind_static_method("EffectLightDirector", D_METHOD("owner_id_for_node", "node"),
			&EffectLightDirector::owner_id_for_node);
	ClassDB::bind_method(D_METHOD("on_wire_node_spawned", "node", "kind", "item_id"),
			&EffectLightDirector::on_wire_node_spawned);
	ClassDB::bind_method(D_METHOD("spawn_light_record", "info", "world_transform", "owner_id",
								 "blink_owner", "spawner_is_building"),
			&EffectLightDirector::spawn_light_record, DEFVAL(0), DEFVAL(PackedInt64Array()),
			DEFVAL(false));
	ClassDB::bind_method(D_METHOD("scene"), &EffectLightDirector::scene);
	ClassDB::bind_method(D_METHOD("light_gain"), &EffectLightDirector::light_gain);
	ClassDB::bind_method(D_METHOD("render_frame", "camera", "time_ms", "viewmodel_parts",
								 "run_census"),
			&EffectLightDirector::render_frame, DEFVAL(TypedArray<ObjectModel>()),
			DEFVAL(true));
	ClassDB::bind_method(D_METHOD("render_inset_frame", "camera", "time_ms"),
			&EffectLightDirector::render_inset_frame);
	ClassDB::bind_method(D_METHOD("release_inset_frame"), &EffectLightDirector::release_inset_frame);
	ClassDB::bind_method(D_METHOD("advance_fixed_tick"), &EffectLightDirector::advance_fixed_tick);
	ClassDB::bind_method(D_METHOD("on_muzzle_fire", "shooter_handle", "world_pos"),
			&EffectLightDirector::on_muzzle_fire);
	ClassDB::bind_method(D_METHOD("on_death_light", "world_pos", "radius"),
			&EffectLightDirector::on_death_light);
	ClassDB::bind_method(D_METHOD("sync_round_glows", "rows"),
			&EffectLightDirector::sync_round_glow_records);
	ClassDB::bind_method(D_METHOD("get_report"), &EffectLightDirector::get_report);
	ClassDB::bind_method(D_METHOD("on_wire_entity_destroyed", "wire_handle"),
			&EffectLightDirector::on_wire_entity_destroyed);
	ClassDB::bind_method(D_METHOD("_on_container_membership_changed", "node"),
			&EffectLightDirector::_on_container_membership_changed);
}
