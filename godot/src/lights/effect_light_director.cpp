#include "lights/effect_light_director.h"
#include "mission/mission_object_placer.h"
#include "mission/static_source_convert.h"

#include "mission/mission_root.h"

#include "env/mission_environment.h"
#include "env/water.h"
#include "env/weather.h"
#include "lights/light_spawn.h"
#include "mission/mission_data.h"
#include "object/entity_index.h"
#include "object/entity_ref.h"
#include "object/object_data.h"
#include "simulation/present_event_records.h"
#include "util/axes.h"
#include "util/color_convert.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>

#include <runtime/renderer/light_scene.h>

using namespace godot;

namespace {

// The placer's container lives under the per-mission subtree (MissionRoot,
// ADR 0043 d9); a bare-scene test builds the same two-level shape.
constexpr const char *kMissionObjectsPath = "MissionRoot/MissionObjects";
constexpr const char *kCoronaShaderPath = "res://shaders/light_corona.gdshader";

} // namespace

EffectLightDirector::EffectLightDirector() {
	scene_.instantiate();
}

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
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	return Object::cast_to<MissionEnvironment>(
			static_cast<Object *>(world->call("get_environment_node")));
}

Weather *EffectLightDirector::_weather() const {
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
	for (const KeyValue<uint64_t, SpawnedNode> &kv : spawned_nodes_) {
		_disconnect_wire_node_exit(kv.value);
	}
	scene()->clear();
	spawned_static_.clear();
	static_sources_snapshot_.clear();
	static_rows_revision_ = -1;
	static_owner_by_bms_.clear();
	spawned_nodes_.clear();
	entity_effect_handles_.clear();
	round_handles_.clear();
	blink_owner_cache_.clear();
	reg_models_.clear();
	reg_owners_.clear();
	reg_robj_scoped_.clear();
	reg_bms_ids_.clear();
	reg_dirty_ = true;
	interior_rows_ = PackedInt64Array();
	interior_index_.clear();
	interior_tick_ = -1;
	_clear_coronas();
}

void EffectLightDirector::reattach() {
	reset();
	if (Node *container = _mission_objects()) {
		const TypedArray<Node> children = container->get_children();
		for (int64_t i = 0; i < children.size(); ++i) {
			ObjectModel *node = Object::cast_to<ObjectModel>(static_cast<Object *>(children[i]));
			if (node == nullptr || node->get_entity_ref().is_null()) {
				continue;
			}
			on_wire_node_spawned(node, -1, 0);
		}
	}
	static_sources_snapshot_ = _static_sources();
	static_rows_revision_ = -1;
	// Build every BMS identity before resolving any blink containment. A
	// static item can spawn inside a batched building that appears later in
	// the source walk, and retail still binds it to that building's owner
	// group.
	for (int64_t source_index = 0; source_index < static_sources_snapshot_.size(); ++source_index) {
		const auto &mapped_source = static_sources_snapshot_[source_index];
		const int bms_id = mapped_source.bms_id;
		if (bms_id != 0) {
			static_owner_by_bms_.insert(bms_id, LightScene::owner_id_for_static_source(source_index));
		}
	}
	for (int64_t source_index = 0; source_index < static_sources_snapshot_.size(); ++source_index) {
		const auto &source = static_sources_snapshot_[source_index];
		if (spawned_static_.has(static_cast<int>(source_index))) {
			continue;
		}
		const Ref<ObjectData> data = provider_->static_source_object_data(source.asset_id);
		if (data.is_null()) {
			continue;
		}
		// Batched statics are entities too: a subobject record binds to this
		// tagged owner and the atlas draw row declares the same identity.
		// [orig: Entity_SpawnGlowEffects @ 0x56c8ae; SetOwnerGroup(entity,bone)]
		const Transform3D xform = from_static_source_transform(source.world_transform);
		const bool is_building = source.kind == MissionData::KIND_BUILDING;
		// Retail skips the blink query for a building's own records; every
		// other static resolves containment once at its placement origin.
		const BlinkOwner blink_owner = is_building ? BlinkOwner() : _blink_owner_at(xform.origin);
		const Vector<int64_t> handles = _spawn_model_lights(data, xform,
				LightScene::owner_id_for_static_source(source_index), blink_owner, is_building);
		if (!handles.is_empty()) {
			spawned_static_.insert(static_cast<int>(source_index), handles);
		}
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

void EffectLightDirector::on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id) {
	(void)p_kind;
	(void)p_item_id;
	if (p_node == nullptr) {
		return;
	}
	const uint64_t node_id = p_node->get_instance_id();
	if (spawned_nodes_.has(node_id)) {
		return;
	}
	const Ref<ObjectData> data = p_node->get_object_data();
	if (data.is_null()) {
		return;
	}
	// Retail runs the blink query once per spawning entity, and skips it
	// outright for a BUILDING — a building's own unattached records stay
	// world lights even though its blink volumes contain them (the query has
	// no self-exclusion) [orig: the ItemType_Building gate @ 0x56c7ec].
	const Ref<EntityRef> ref = p_node->get_entity_ref();
	const bool is_building = ref.is_valid() && ref->get_kind() == MissionData::KIND_BUILDING;
	BlinkOwner blink_owner;
	if (!is_building) {
		blink_owner = _blink_owner_at(p_node->get_global_position());
	}
	if (data->get_light_count() <= 0) {
		return;
	}
	const int64_t owner_id = owner_id_for_node(p_node);
	entity_effect_handles_.insert(owner_id,
			_spawn_node_lights(p_node, owner_id, blink_owner, is_building));
	// Register even when pool exhaustion returned zero. Retail walks a given
	// entity once; duplicate callback delivery must not turn a later free
	// slot into an invented second spawn attempt.
	SpawnedNode record;
	record.node = ObjectID(node_id);
	record.owner_id = owner_id;
	record.tree_exiting = Callable(this, "_on_wire_node_exiting").bind(static_cast<int64_t>(node_id));
	p_node->connect("tree_exiting", record.tree_exiting, Object::CONNECT_ONE_SHOT);
	spawned_nodes_.insert(node_id, record);
}

void EffectLightDirector::_on_wire_node_exiting(int64_t p_node_id) {
	const SpawnedNode *record = spawned_nodes_.getptr(static_cast<uint64_t>(p_node_id));
	if (record == nullptr) {
		return;
	}
	const int64_t owner_id = record->owner_id;
	spawned_nodes_.erase(static_cast<uint64_t>(p_node_id));
	// Entity_Destroy has one 16-bit EffectWorld word, not an owned-light
	// list. Clear exactly the lease currently cached there; a husk swap does
	// not exit the node and therefore does not touch any authored light.
	if (const int64_t *cached_handle = entity_effect_handles_.getptr(owner_id)) {
		if (*cached_handle != 0) {
			scene()->despawn(*cached_handle);
		}
	}
	entity_effect_handles_.erase(owner_id);
}

void EffectLightDirector::_disconnect_wire_node_exit(const SpawnedNode &p_record) {
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(p_record.node));
	const Callable &on_exit = p_record.tree_exiting;
	if (node != nullptr && on_exit.is_valid() && node->is_connected("tree_exiting", on_exit)) {
		node->disconnect("tree_exiting", on_exit);
	}
}

Vector<int64_t> EffectLightDirector::_spawn_model_lights(const Ref<ObjectData> &p_data,
		const Transform3D &p_world_transform, int64_t p_owner_id, const BlinkOwner &p_blink_owner,
		bool p_spawner_is_building) {
	Vector<int64_t> handles;
	if (p_data.is_null()) {
		return handles;
	}
	for (int i = 0; i < p_data->get_light_count(); ++i) {
		const Ref<ModelLight> info = p_data->get_light_info(i);
		const int64_t handle = _spawn_light_at(info,
				p_world_transform.xform(info->get_position()), p_owner_id, p_blink_owner,
				p_spawner_is_building);
		if (handle != 0) {
			handles.push_back(handle);
		}
	}
	return handles;
}

// Live-node twin of the static source walk. Retail transforms every record's
// model-space point by the ENTITY placement matrix once; `subobject` is read
// only afterward as an owner-group section [orig: Entity_SpawnGlowEffects
// @0x56c82d..0x56c84e, then @0x56c89a..0x56c8ae]. No node/ROBJ/bone position
// follow exists. Return the final entity+0x1B4 value; retail retains no list.
int64_t EffectLightDirector::_spawn_node_lights(ObjectModel *p_node, int64_t p_owner_id,
		const BlinkOwner &p_blink_owner, bool p_spawner_is_building) {
	if (p_node == nullptr) {
		return 0;
	}
	const Ref<ObjectData> data = p_node->get_object_data();
	if (data.is_null()) {
		return 0;
	}
	int64_t cached_handle = 0;
	const Transform3D world_transform = p_node->get_global_transform();
	for (int index = 0; index < data->get_light_count(); ++index) {
		const Ref<ModelLight> info = data->get_light_info(index);
		cached_handle = _spawn_light_at(info, world_transform.xform(info->get_position()),
				p_owner_id, p_blink_owner, p_spawner_is_building);
	}
	return cached_handle;
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
// to [orig: Entity_SpawnGlowEffects @ 0x56c7fc -> Entity_QueryBlinkBoxesAtPoint
// @ 0x4af350]. Owner 0 outdoors. Both individual ObjectModels and batched
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

// The owner id a containing building's bms_id resolves to — the SAME id its
// own draw context declares, or owner gating never matches.
int64_t EffectLightDirector::_owner_id_for_bms(int p_bms_id) {
	if (p_bms_id == 0) {
		return 0;
	}
	if (const int64_t *static_owner = static_owner_by_bms_.getptr(p_bms_id)) {
		return *static_owner;
	}
	if (const int64_t *cached = blink_owner_cache_.getptr(p_bms_id)) {
		return *cached;
	}
	int64_t owner = 0;
	if (MissionRoot *runtime = _runtime()) {
		const Ref<EntityIndex> registry = runtime->get_entity_index();
		if (registry.is_valid()) {
			if (ObjectModel *node = registry->resolve_single(p_bms_id)) {
				owner = owner_id_for_node(node);
			}
		}
	}
	blink_owner_cache_.insert(p_bms_id, owner);
	return owner;
}

Vector3 EffectLightDirector::light_gain() const {
	Vector3 gain(1, 1, 1);
	if (MissionEnvironment *env = _environment()) {
		const Ref<EnvLightState> state = env->get_light_state();
		if (state.is_valid() && state->get_values().is_valid()) {
			gain = state->get_values()->get_gain();
		}
	}
	return gain;
}

// Build the immutable-index static atlas rows. The placer owns row identity
// and entity bounds; this device supplies the same owner/interior groups
// as the live-model pass, selects the witnessed nearest three, and publishes
// the RGBAF payload consumed through INSTANCE_CUSTOM.x.
void EffectLightDirector::_render_static_light_rows(const Vector3 &p_gain, Weather *p_weather,
		int p_time_ms) {
	const int64_t revision = _static_draw_source_revision();
	if (revision != static_rows_revision_) {
		_rebuild_static_light_rows();
		static_rows_revision_ = revision;
	}
	scene()->render_static_frame(static_rows_positions_, static_rows_bound_radii_q16_,
			static_rows_owner_entities_,
			static_rows_owner_sections_, static_rows_interior_owners_,
			static_rows_interior_sections_, static_rows_active_, p_gain, p_time_ms, p_weather,
			static_rows_revision_);
}

void EffectLightDirector::_rebuild_static_light_rows() {
	const auto descriptors = _static_draw_sources();
	int row_count = 0;
	for (int64_t i = 0; i < descriptors.size(); ++i) {
		const auto &descriptor = descriptors[i];
		row_count = std::max(row_count, descriptor.atlas_row + 1);
	}
	PackedVector3Array entity_positions;
	PackedInt32Array entity_bound_radii_q16;
	PackedInt64Array owner_entities;
	PackedInt32Array owner_sections;
	PackedInt64Array interior_owners;
	PackedInt32Array interior_sections;
	PackedByteArray active;
	entity_positions.resize(row_count);
	entity_bound_radii_q16.resize(row_count);
	owner_entities.resize(row_count);
	owner_sections.resize(row_count);
	interior_owners.resize(row_count);
	interior_sections.resize(row_count);
	active.resize(row_count);
	for (int64_t i = 0; i < descriptors.size(); ++i) {
		const auto &descriptor = descriptors[i];
		const int atlas_row = descriptor.atlas_row;
		const int source_index = descriptor.source_index;
		if (atlas_row < 0 || atlas_row >= row_count || source_index < 0 ||
				source_index >= static_sources_snapshot_.size()) {
			continue;
		}
		const auto &source = static_sources_snapshot_[source_index];
		entity_positions[atlas_row] = from_static_source_transform(source.world_transform).origin;
		entity_bound_radii_q16[atlas_row] = source.entity_bound_radius_q16;
		active[atlas_row] = descriptor.active ? 1 : 0;
		// The row's two groups are the engine's static-row policy
		// (renderer::static_light_row_groups): a building is its own
		// interior group at section zero with the owner section re-scoped
		// to this exact ROBJ; every other row is owned by its static owner
		// and carries the blink interior its placement origin resolves.
		opennova::renderer::StaticLightRowInputs inputs;
		inputs.static_owner = static_cast<uint64_t>(LightScene::owner_id_for_static_source(source_index));
		inputs.robj_index = descriptor.robj_index;
		inputs.is_building = (descriptor.kind >= 0 ? descriptor.kind : source.kind) ==
				MissionData::KIND_BUILDING;
		if (!inputs.is_building) {
			const BlinkOwner interior = _blink_owner_at(from_static_source_transform(source.world_transform).origin);
			inputs.blink_hit = interior.valid();
			inputs.blink_owner_entity = static_cast<uint64_t>(interior.owner);
			inputs.blink_section = interior.section;
		}
		const opennova::renderer::LightActiveGroups groups =
				opennova::renderer::static_light_row_groups(inputs);
		owner_entities[atlas_row] = static_cast<int64_t>(groups.owner_group_entity);
		owner_sections[atlas_row] = groups.owner_group_section;
		interior_owners[atlas_row] = static_cast<int64_t>(groups.interior_group_entity);
		interior_sections[atlas_row] = groups.interior_group_section;
	}
	static_rows_positions_ = entity_positions;
	static_rows_bound_radii_q16_ = entity_bound_radii_q16;
	static_rows_owner_entities_ = owner_entities;
	static_rows_owner_sections_ = owner_sections;
	static_rows_interior_owners_ = interior_owners;
	static_rows_interior_sections_ = interior_sections;
	static_rows_active_ = active;
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
	frame_models_.clear();
	frame_entity_positions_.clear();
	frame_entity_bound_radii_q16_.clear();
	frame_owners_.clear();
	// interior_*: the second witnessed group — the building each draw
	// currently stands inside, plus that blink volume's section (the engine
	// pool's LightActiveGroups; the rows are the sim's interior-group latch).
	frame_interior_owners_.clear();
	frame_interior_sections_.clear();
	frame_robj_scoped_.clear();
	_render_static_light_rows(gain, weather, time_ms);
	_refresh_interior_groups();
	if (Node *container = _mission_objects()) {
		_ensure_model_registry(container);
		for (int64_t i = 0; i < reg_models_.size(); ++i) {
			ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(reg_models_[i]));
			if (model == nullptr || !model->is_visible_in_tree()) {
				continue;
			}
			if (model->get_global_position().distance_to(cam_pos) > QUERY_RADIUS) {
				continue;
			}
			frame_models_.push_back(model);
			ObjectModel *entity_model = model->get_entity_light_owner();
			if (entity_model == nullptr) entity_model = model;
			frame_entity_positions_.push_back(entity_model->get_global_position());
			frame_entity_bound_radii_q16_.push_back(entity_model->get_entity_bound_radius_q16());
			frame_owners_.push_back(reg_owners_[i]);
			frame_robj_scoped_.push_back(reg_robj_scoped_[i]);
			int64_t interior_owner = 0;
			int interior_section = 0;
			const int64_t bms_id = reg_bms_ids_[i];
			if (bms_id != 0) {
				if (const int64_t *base = interior_index_.getptr(bms_id)) {
					const int64_t owner = _owner_id_for_bms(static_cast<int>(interior_rows_[*base + 1]));
					if (owner != 0) {
						interior_owner = owner;
						interior_section = static_cast<int>(interior_rows_[*base + 2]);
					}
				}
			}
			frame_interior_owners_.push_back(interior_owner);
			frame_interior_sections_.push_back(interior_section);
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
		scene()->render_frame(cam_pos, QUERY_RADIUS, gain, time_ms, weather);
		census_stale_ = false;
	} else {
		census_stale_ = true;
	}
	scene()->render_model_frame(frame_models_, frame_owners_, frame_interior_owners_,
			frame_interior_sections_, frame_robj_scoped_, gain, time_ms, weather,
			frame_entity_positions_, frame_entity_bound_radii_q16_);
	_render_coronas(p_camera, gain, time_ms, weather, frame_models_, frame_owners_, env);
}

void EffectLightDirector::run_census_now() {
	if (!census_stale_) {
		return;
	}
	// census_frame refreshes the rows without restamping the report's mode:
	// a report read with the F3 stats off keeps saying what the gameplay
	// pass (render_model_frame) reported.
	scene()->census_frame(census_cam_pos_, QUERY_RADIUS, light_gain(), census_time_ms_, _weather());
	census_stale_ = false;
}

// The interior-group rows (bms_id -> containing bms_id + section for every
// entity standing inside a blink volume) are sim tick products: fetch them
// once per logic tick into the flat rows plus a bms_id -> base-index lookup,
// so the per-frame walk reads them without allocating a row per entity.
void EffectLightDirector::_refresh_interior_groups() {
	const Ref<Simulation> sim = _sim();
	if (sim.is_null()) {
		if (!interior_index_.is_empty()) {
			interior_index_.clear();
			interior_rows_ = PackedInt64Array();
		}
		interior_tick_ = -1;
		return;
	}
	const int64_t tick = sim->get_logic_tick();
	if (tick == interior_tick_) {
		return;
	}
	interior_tick_ = tick;
	interior_rows_ = sim->get_entity_interior_groups();
	interior_index_.clear();
	int64_t i = 0;
	while (i + 2 < interior_rows_.size()) {
		interior_index_.insert(interior_rows_[i], i);
		i += 3;
	}
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
		_rebuild_model_registry(p_container);
	}
}

void EffectLightDirector::_on_container_membership_changed(Node *p_node) {
	(void)p_node;
	reg_dirty_ = true;
	// A bms id's resolved owner can change with membership
	// (despawn/respawn), so the blink-owner cache follows the registry.
	blink_owner_cache_.clear();
}

void EffectLightDirector::_rebuild_model_registry(Node *p_container) {
	reg_models_.clear();
	reg_owners_.clear();
	reg_robj_scoped_.clear();
	reg_bms_ids_.clear();
	const TypedArray<Node> children = p_container->get_children();
	for (int64_t i = 0; i < children.size(); ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(static_cast<Object *>(children[i]));
		if (model == nullptr) {
			continue;
		}
		ObjectModel *entity_model = model->get_entity_light_owner();
		if (entity_model == nullptr) entity_model = model;
		const Ref<EntityRef> ref = entity_model->get_entity_ref();
		reg_models_.push_back(ObjectID(model->get_instance_id()));
		const int64_t *static_owner = ref.is_valid()
				? static_owner_by_bms_.getptr(ref->get_bms_id()) : nullptr;
		reg_owners_.push_back(static_owner != nullptr ? *static_owner : owner_id_for_node(model));
		reg_robj_scoped_.push_back(
				ref.is_valid() && ref->get_kind() == MissionData::KIND_BUILDING ? 1 : 0);
		reg_bms_ids_.push_back(ref.is_valid() ? ref->get_bms_id() : 0);
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
	const PackedInt64Array hit = sim->local_player_interior_group();
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

// The corona device leg: fetch this frame's additive quads from the portable
// walk and rebuild the MultiMesh (instance origin = segment center, uniform
// scale = half-size, instance color = the premultiplied additive color).
// The models/owners arrays are the per-model pass's own walk — models with
// an occlusion section-mask verdict gate their owned coronas on the
// visible-section bit [orig: Terrain_IsBuildingSectionBitSet @ 0x5c6960];
// the env fog rides in as the fog-to-black fold
// [orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x5aafb6].
void EffectLightDirector::_render_coronas(Camera3D *p_camera, const Vector3 &p_gain,
		int p_time_ms, Weather *p_weather, const TypedArray<Node3D> &p_models, const PackedInt64Array &p_owners,
		MissionEnvironment *p_env) {
	corona_frame_ = (corona_frame_ + 1) & 3;
	Ref<EnvLightValues> fog;
	if (p_env != nullptr) {
		const Ref<EnvLightState> state = p_env->get_light_state();
		if (state.is_valid()) {
			fog = state->get_values();
		}
	}
	MultiMeshInstance3D *instance = _ensure_corona_instance();
	if (instance == nullptr) {
		return;
	}
	// One native buffer write instead of two RenderingServer commands per
	// row (the witnessed jitter re-centers every corona every frame, so
	// there is no change to gate on); rows past this frame's count stay
	// hidden through visible_instance_count.
	const Transform3D camera_transform = p_camera->get_camera_transform();
	const int rows = scene()->fill_corona_multimesh(camera_transform.origin,
			-camera_transform.basis.get_column(2), p_gain, p_time_ms, corona_frame_, p_weather,
			p_models, p_owners, fog, instance->get_multimesh());
	instance->set_visible(rows > 0);
}

void EffectLightDirector::_clear_coronas() {
	MultiMeshInstance3D *instance = _corona_instance();
	if (instance == nullptr) {
		return;
	}
	const Ref<MultiMesh> mesh = instance->get_multimesh();
	if (mesh.is_valid()) {
		mesh->set_instance_count(0);
	}
	instance->set_visible(false);
}

MultiMeshInstance3D *EffectLightDirector::_corona_instance() const {
	return Object::cast_to<MultiMeshInstance3D>(ObjectDB::get_instance(corona_instance_id_));
}

MultiMeshInstance3D *EffectLightDirector::_ensure_corona_instance() {
	if (MultiMeshInstance3D *existing = _corona_instance()) {
		return existing;
	}
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
	mmi->set_name("EffectLightCoronas");
	Ref<MultiMesh> mesh;
	mesh.instantiate();
	mesh->set_transform_format(MultiMesh::TRANSFORM_3D);
	mesh->set_use_colors(true);
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(2.0f, 2.0f)); // VERTEX.xy in [-1, 1] x half_size
	Ref<ShaderMaterial> material;
	material.instantiate();
	const Ref<Shader> shader = ResourceLoader::get_singleton()->load(kCoronaShaderPath);
	material->set_shader(shader);
	material->set_shader_parameter("u_corona_tex", _corona_texture());
	quad->set_material(material);
	mesh->set_mesh(quad);
	mmi->set_multimesh(mesh);
	mmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	// Coronas draw in the mirror scene too [orig: the
	// Water_RenderReflectedWorldScene call @ 0x5c85fd].
	mmi->set_layer_mask(Water::VISUAL_LAYER_WORLD);
	// The quads billboard in-shader from rows anywhere in the world; the
	// static AABB only seeds Godot's sort and the cull margin keeps the
	// instance from being frustum-culled once the camera leaves that box.
	mmi->set_custom_aabb(AABB(Vector3(-512, -512, -512), Vector3(1024, 1024, 1024)));
	mmi->set_extra_cull_margin(1.0e6f);
	world->add_child(mmi);
	corona_instance_id_ = mmi->get_instance_id();
	return mmi;
}

Ref<ImageTexture> EffectLightDirector::_corona_texture() {
	if (corona_texture_.is_valid()) {
		return corona_texture_;
	}
	const int size = LightScene::corona_texture_size();
	const Ref<Image> image = Image::create_from_data(size, size, false, Image::FORMAT_RGBA8,
			LightScene::corona_texture_rgba8());
	corona_texture_ = ImageTexture::create_from_image(image);
	return corona_texture_;
}

void EffectLightDirector::advance_fixed_tick() {
	scene()->advance_fixed_tick();
}

void EffectLightDirector::on_muzzle_fire(int64_t p_shooter_handle, const Vector3 &p_world_pos) {
	const int64_t owner_id = LightScene::owner_id_for_wire(p_shooter_handle);
	int64_t handle = 0;
	if (const int64_t *cached = entity_effect_handles_.getptr(owner_id)) {
		handle = *cached;
	}
	if (handle == 0) {
		handle = scene()->spawn_glow(GlowSpawn::make(p_world_pos, LightScene::muzzle_glow_radius(),
				LightScene::muzzle_glow_color())
						->fading(3, -1)
						->owned_by(owner_id, 0));
		if (handle == 0) {
			return;
		}
		entity_effect_handles_.insert(owner_id, handle);
	}
	scene()->set_light_fade(handle, LightScene::muzzle_glow_fade_mode(),
			LightScene::muzzle_glow_fade_ticks());
	scene()->set_light_owner(handle, owner_id, 0);
	scene()->set_light_position(handle, p_world_pos);
	scene()->set_light_blend(handle, 1.0f);
}

void EffectLightDirector::on_impact_light(const Vector3 &p_world_pos, float p_radius,
		const Color &p_color, int p_duration_ticks) {
	if (p_radius <= 0.0f) {
		return;
	}
	Ref<GlowSpawn> spawn =
			GlowSpawn::make(p_world_pos + Vector3(0.0f, p_radius * 0.5f, 0.0f), p_radius, p_color);
	spawn->fading(2, p_duration_ticks);
	spawn->set_corona_lower_half_radius(true);
	scene()->spawn_glow(spawn);
}

void EffectLightDirector::on_death_light(const Vector3 &p_world_pos, float p_radius) {
	if (p_radius <= 0.0f) {
		return;
	}
	scene()->spawn_glow(GlowSpawn::make(p_world_pos, p_radius, LightScene::death_flash_color())
								->fading(LightScene::death_flash_fade_mode(),
										LightScene::death_flash_fade_ticks())
								->masking(true, false, false));
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
	HashSet<int64_t> seen;
	for (const opennova::world::RoundGlowRow &row : p_rows) {
		const int64_t id = static_cast<int64_t>(row.id);
		seen.insert(id);
		const float radius = row.radius;
		// Mission (x, y, z-up) -> Godot (x, z, -y), the presentation drains' rule.
		const Vector3 pos = mission_to_godot(row.pos);
		const int64_t *cached = round_handles_.getptr(id);
		if (cached == nullptr || *cached == 0) {
			// The spawn rides radius/2 above the round; the per-tick follow
			// re-centers at the raw round position [orig: @ 0x4ec8d6 vs the
			// @ 0x4eaa9f SetPositionAndBounds follow] — the engine's
			// round-glow law.
			const int64_t handle = scene()->spawn_glow(
					GlowSpawn::make(pos + Vector3(0.0f, opennova::renderer::round_glow_spawn_lift(radius), 0.0f),
							radius, opennova::color_from_rgb24(row.color_rgb24))
							->masking(false, true, false));
			if (handle != 0) {
				round_handles_.insert(id, handle);
			}
		} else {
			scene()->set_light_position(*cached,
					pos + Vector3(0.0f, opennova::renderer::kRoundGlowFollowLift, 0.0f));
		}
	}
	Vector<int64_t> dropped;
	for (const KeyValue<int64_t, int64_t> &kv : round_handles_) {
		if (!seen.has(kv.key)) {
			dropped.push_back(kv.key);
		}
	}
	for (const int64_t id : dropped) {
		scene()->despawn(round_handles_[id]);
		round_handles_.erase(id);
	}
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
	ClassDB::bind_method(D_METHOD("advance_fixed_tick"), &EffectLightDirector::advance_fixed_tick);
	ClassDB::bind_method(D_METHOD("on_muzzle_fire", "shooter_handle", "world_pos"),
			&EffectLightDirector::on_muzzle_fire);
	ClassDB::bind_method(D_METHOD("on_death_light", "world_pos", "radius"),
			&EffectLightDirector::on_death_light);
	ClassDB::bind_method(D_METHOD("sync_round_glows", "rows"),
			&EffectLightDirector::sync_round_glow_records);
	ClassDB::bind_method(D_METHOD("get_report"), &EffectLightDirector::get_report);
	ClassDB::bind_method(D_METHOD("_on_wire_node_exiting", "node_id"),
			&EffectLightDirector::_on_wire_node_exiting);
	ClassDB::bind_method(D_METHOD("_on_container_membership_changed", "node"),
			&EffectLightDirector::_on_container_membership_changed);
}
