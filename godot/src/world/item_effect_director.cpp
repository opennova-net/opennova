#include "world/item_effect_director.h"

#include "mission/mission_root.h"

#include "object/item_records.h"
#include "object/model_user_point.h"
#include "object/object_data.h"
#include "particle/effect_world.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/world/entity.h>
#include <runtime/world/item_effects.h>

using namespace godot;
using opennova::to_std;

namespace {

// The placer's container lives under the per-mission subtree (MissionRoot,
// ADR 0043 d9); a bare-scene test builds the same two-level shape.
constexpr const char *kMissionObjectsPath = "MissionRoot/MissionObjects";
constexpr const char *kControlStarted = "vehicle_control_started";
constexpr const char *kControlStopped = "vehicle_control_stopped";

String item_fx_point_key(uint64_t p_node_id, int p_index) {
	return vformat("itemfx:%d:%d", static_cast<int64_t>(p_node_id), p_index);
}

String item_fx_origin_key(uint64_t p_node_id) {
	return vformat("itemfx:%d:origin", static_cast<int64_t>(p_node_id));
}

ObjectModel *live_model(const ObjectID &p_id) {
	return Object::cast_to<ObjectModel>(ObjectDB::get_instance(p_id));
}

} // namespace

void ItemEffectDirectorStats::_bind_methods() {
#define ITEM_EFFECT_DIRECTOR_STATS_BIND(m_name)                                                     \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ItemEffectDirectorStats::get_##m_name);       \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ItemEffectDirectorStats::set_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	ITEM_EFFECT_DIRECTOR_STATS_FIELDS(ITEM_EFFECT_DIRECTOR_STATS_BIND)
#undef ITEM_EFFECT_DIRECTOR_STATS_BIND
}

void ItemEffectDirector::setup(Node *p_world, const Callable &p_static_sources,
		const Callable &p_item_db_source) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	provider_ = nullptr;
	static_sources_ = p_static_sources;
	item_db_source_ = p_item_db_source;
}

void ItemEffectDirector::setup_with_provider(Node *p_world, StaticSourceProvider *p_provider) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	provider_ = p_provider;
	static_sources_ = Callable();
	item_db_source_ = Callable();
}

Array ItemEffectDirector::_static_sources() const {
	if (provider_ != nullptr) {
		return provider_->static_item_effect_sources();
	}
	if (static_sources_.is_valid()) {
		return static_sources_.call();
	}
	return Array();
}

Node *ItemEffectDirector::_world() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(world_id_));
}

EffectWorld *ItemEffectDirector::_effect_world() const {
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	return Object::cast_to<EffectWorld>(static_cast<Object *>(world->call("get_effect_world")));
}

MissionRoot *ItemEffectDirector::_runtime() const {
	Node *world = _world();
	if (world == nullptr) {
		return nullptr;
	}
	return Object::cast_to<MissionRoot>(static_cast<Object *>(world->call("get_runtime")));
}

// The placer's item database through the lent seam (null before a mission /
// with no placer).
Ref<ItemDatabase> ItemEffectDirector::_resolve_item_db() const {
	if (provider_ != nullptr) {
		return provider_->static_source_item_db();
	}
	if (!item_db_source_.is_valid()) {
		return Ref<ItemDatabase>();
	}
	return Ref<ItemDatabase>(item_db_source_.call());
}

void ItemEffectDirector::set_particles_hidden(bool p_hidden) {
	const bool was_hidden = particles_hidden_;
	particles_hidden_ = p_hidden;
	if (EffectWorld *effect_world = _effect_world()) {
		effect_world->set_particles_hidden(p_hidden);
	}
	if (was_hidden && !p_hidden) {
		_retry_pending_item_effects();
	}
}

bool ItemEffectDirector::particles_hidden() const {
	return particles_hidden_;
}

void ItemEffectDirector::register_effect_anchor(const Variant &p_owner_key,
		const Callable &p_resolver) {
	effect_anchor_resolvers_.insert(p_owner_key, p_resolver);
}

void ItemEffectDirector::unregister_effect_anchor(const Variant &p_owner_key) {
	effect_anchor_resolvers_.erase(p_owner_key);
}

bool ItemEffectDirector::has_effect_anchor(const Variant &p_owner_key) const {
	return effect_anchor_resolvers_.has(p_owner_key);
}

Ref<ItemEffectDirectorStats> ItemEffectDirector::get_stats() const {
	Ref<ItemEffectDirectorStats> stats;
	stats.instantiate();
	stats->set_registered_nodes(static_cast<int>(item_fx_registered_nodes_.size()));
	stats->set_registered_static(static_cast<int>(item_fx_registered_static_.size()));
	stats->set_pending_nodes(static_cast<int>(item_fx_pending_nodes_.size()));
	stats->set_pending_static(static_cast<int>(item_fx_pending_static_.size()));
	stats->set_control_nodes(static_cast<int>(item_fx_control_nodes_.size()));
	stats->set_control_active(static_cast<int>(item_fx_control_active_.size()));
	stats->set_owner_keys(static_cast<int>(item_fx_nodes_.size()));
	return stats;
}

void ItemEffectDirector::on_effect_world_started() {
	EffectWorld *effect_world = _effect_world();
	if (effect_world == nullptr) {
		return;
	}
	// One provider for every owned/attached group: int keys are WAC fx2ssn
	// SSNs (resolved through the runtime), String keys are the per-item
	// effect attaches (resolved to the placed node's live transform).
	effect_world->set_owner_position_provider(Callable(this, "resolve_owner_transform"));
	reattach();
}

void ItemEffectDirector::reset() {
	item_fx_nodes_.clear();
	item_fx_owner_refs_.clear();
	item_fx_registered_nodes_.clear();
	item_fx_pending_nodes_.clear();
	item_fx_registered_static_.clear();
	item_fx_pending_static_.clear();
	item_fx_control_active_.clear();
	item_fx_control_nodes_.clear();
	item_fx_control_instances_.clear();
}

Variant ItemEffectDirector::resolve_owner_transform(const Variant &p_owner_key) {
	// Registered live anchors first (the local weapon flash follows its
	// viewmodel userpoint for the emitter group's whole life [orig: the
	// actionEffectHandle per-tick tracker in WeaponAction_ProcessFrame
	// @ 0x540edf]).
	if (const Callable *anchor = effect_anchor_resolvers_.getptr(p_owner_key)) {
		if (anchor->is_valid()) {
			return anchor->call();
		}
		effect_anchor_resolvers_.erase(p_owner_key);
		return Variant();
	}
	if (p_owner_key.get_type() == Variant::STRING) {
		const String owner_key = p_owner_key;
		// A freed instance is validity-checked before any typed read.
		Node3D *node = nullptr;
		if (const ObjectID *node_id = item_fx_nodes_.getptr(owner_key)) {
			node = Object::cast_to<Node3D>(ObjectDB::get_instance(*node_id));
		}
		if (node != nullptr && node->is_inside_tree()) {
			const Ref<EntityRef> *entity_ref = item_fx_owner_refs_.getptr(owner_key);
			MissionRoot *pose_runtime = _runtime();
			if (entity_ref != nullptr && entity_ref->is_valid() && pose_runtime != nullptr &&
					pose_runtime->has_current_present_effect_snapshot()) {
				// Null here means the identity left THIS tick's replica
				// set. Do not fall back to the one-frame-old Node or the
				// group would emit once more from stale state before the
				// batched present frees it.
				return pose_runtime->presented_entity_effect_transform(*entity_ref);
			}
			return node->get_global_transform();
		}
		item_fx_nodes_.erase(owner_key);
		item_fx_owner_refs_.erase(owner_key);
		return Variant();
	}
	if (MissionRoot *ssn_runtime = _runtime()) {
		return ssn_runtime->entity_effect_transform_for_ssn(static_cast<int>(p_owner_key));
	}
	return Variant();
}

// Pool gates are kind-sensitive and the attach plan is the engine's
// (runtime/world/item_effects: pool 0 organics excluded, pool 1 items skip
// attrib 0x42, pools 2/3 skip attrib 0x2; the first-16 userpoint mask, the
// spawn_count==0 origin leg).
void ItemEffectDirector::reattach() {
	item_fx_nodes_.clear();
	item_fx_owner_refs_.clear();
	item_fx_registered_nodes_.clear();
	item_fx_pending_nodes_.clear();
	item_fx_registered_static_.clear();
	item_fx_pending_static_.clear();
	item_fx_control_active_.clear();
	item_fx_control_nodes_.clear();
	item_fx_control_instances_.clear();
	EffectWorld *effect_world = _effect_world();
	if (effect_world == nullptr) {
		return;
	}
	const Ref<ItemDatabase> item_db = _resolve_item_db();
	if (item_db.is_null()) {
		return;
	}
	int attached = 0;
	Node *world = _world();
	Node *container = world != nullptr ? world->get_node_or_null(NodePath(kMissionObjectsPath)) : nullptr;
	if (container != nullptr) {
		const TypedArray<Node> children = container->get_children();
		for (int64_t i = 0; i < children.size(); ++i) {
			// Boundary filter: only placed ObjectModels carry the per-item
			// effect contract (husk grafts and helper nodes skip here).
			ObjectModel *node = Object::cast_to<ObjectModel>(static_cast<Object *>(children[i]));
			const Ref<EntityRef> ref = node != nullptr ? node->get_entity_ref() : Ref<EntityRef>();
			if (ref.is_null()) {
				continue;
			}
			attached += _attach_item_effect_to_node(node, ref->get_kind(), ref->get_item_id(), item_db);
		}
	}
	{
		const Array static_sources = _static_sources();
		for (int64_t source_index = 0; source_index < static_sources.size(); ++source_index) {
			const Ref<StaticEffectSource> source = static_sources[source_index];
			attached += _attach_item_effect_to_static(source, static_cast<int>(source_index), item_db);
		}
	}
	if (attached > 0) {
		UtilityFunctions::print_verbose(vformat("GameWorld: item effects — %d emitter(s)", attached));
	}
}

void ItemEffectDirector::on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id) {
	_attach_item_effect_to_node(p_node, p_kind, p_item_id);
}

void ItemEffectDirector::_control_node_aliases(ObjectModel *p_node,
		std::vector<std::string> &r_out) const {
	r_out.clear();
	const Ref<EntityRef> ref = p_node != nullptr ? p_node->get_entity_ref() : Ref<EntityRef>();
	if (ref.is_null()) {
		return;
	}
	// A wire row's origin is the header identity it carries (which may be
	// none); a placed model's origin is its own record.
	const int origin_kind = ref->get_wire_handle() >= 0 ? ref->get_origin_kind() : ref->get_kind();
	int64_t spawn_origin = 0;
	if (origin_kind >= 0 && ref->get_index() >= 0) {
		spawn_origin = static_cast<int64_t>(opennova::world::spawn_origin_pack(
				static_cast<uint32_t>(origin_kind), static_cast<uint32_t>(ref->get_index())));
	}
	opennova::world::item_effect_identity_aliases(0, ref->get_bms_id(), spawn_origin,
			ref->get_wire_handle(), r_out);
}

bool ItemEffectDirector::_control_node_is_active(const ControlNode &p_entry) const {
	for (const std::string &alias : p_entry.aliases) {
		if (item_fx_control_active_.count(alias) != 0) {
			return true;
		}
	}
	return false;
}

bool ItemEffectDirector::_register_control_node(ObjectModel *p_node, int p_kind, int p_item_id) {
	ControlNode entry;
	_control_node_aliases(p_node, entry.aliases);
	if (entry.aliases.empty()) {
		return false;
	}
	entry.node = ObjectID(p_node->get_instance_id());
	entry.kind = p_kind;
	entry.item_id = p_item_id;
	item_fx_control_nodes_.insert(p_node->get_instance_id(), entry);
	return true;
}

void ItemEffectDirector::_track_control_spawn(uint64_t p_node_id, const String &p_owner_key,
		const Ref<EffectSpawnReceipt> &p_receipt) {
	ControlInstance *instance = item_fx_control_instances_.getptr(p_node_id);
	if (instance == nullptr) {
		item_fx_control_instances_.insert(p_node_id, ControlInstance());
		instance = item_fx_control_instances_.getptr(p_node_id);
	}
	const int64_t group_id = p_receipt->get_group_id();
	if (group_id > 0 && !instance->group_ids.has(group_id)) {
		instance->group_ids.push_back(group_id);
	}
	if (!instance->owner_keys.has(p_owner_key)) {
		instance->owner_keys.push_back(p_owner_key);
	}
}

void ItemEffectDirector::_stop_control_node(uint64_t p_node_id) {
	if (const ControlInstance *instance = item_fx_control_instances_.getptr(p_node_id)) {
		if (EffectWorld *effect_world = _effect_world()) {
			for (const int64_t group_id : instance->group_ids) {
				if (group_id > 0) {
					effect_world->stop_group(group_id);
				}
			}
		}
		for (const String &owner_key : instance->owner_keys) {
			item_fx_nodes_.erase(owner_key);
			item_fx_owner_refs_.erase(owner_key);
		}
	}
	item_fx_control_instances_.erase(p_node_id);
	item_fx_registered_nodes_.erase(p_node_id);
	item_fx_pending_nodes_.erase(p_node_id);
}

void ItemEffectDirector::_activate_control_nodes(const std::vector<std::string> &p_event_aliases) {
	Vector<uint64_t> node_ids;
	for (const KeyValue<uint64_t, ControlNode> &kv : item_fx_control_nodes_) {
		node_ids.push_back(kv.key);
	}
	for (const uint64_t node_id : node_ids) {
		const ControlNode *entry = item_fx_control_nodes_.getptr(node_id);
		if (entry == nullptr ||
				!opennova::world::item_effect_aliases_intersect(entry->aliases, p_event_aliases)) {
			continue;
		}
		ObjectModel *node = live_model(entry->node);
		if (node == nullptr) {
			_stop_control_node(node_id);
			item_fx_control_nodes_.erase(node_id);
			continue;
		}
		if (!_control_node_is_active(*entry)) {
			continue;
		}
		const int kind = entry->kind;
		const int item_id = entry->item_id;
		_attach_item_effect_to_node(node, kind, item_id, Ref<ItemDatabase>(), true);
	}
}

void ItemEffectDirector::_deactivate_control_nodes(const std::vector<std::string> &p_event_aliases) {
	Vector<uint64_t> node_ids;
	for (const KeyValue<uint64_t, ControlNode> &kv : item_fx_control_nodes_) {
		node_ids.push_back(kv.key);
	}
	for (const uint64_t node_id : node_ids) {
		const ControlNode *entry = item_fx_control_nodes_.getptr(node_id);
		if (entry == nullptr ||
				!opennova::world::item_effect_aliases_intersect(entry->aliases, p_event_aliases)) {
			continue;
		}
		if (!_control_node_is_active(*entry)) {
			_stop_control_node(node_id);
		}
	}
}

bool ItemEffectDirector::consume_control_effect(const Ref<MissionEffect> &p_effect) {
	if (p_effect.is_null()) {
		return false;
	}
	const String kind = p_effect->get_kind();
	const bool started = kind == kControlStarted;
	if (!started && kind != kControlStopped) {
		return false;
	}
	std::vector<std::string> aliases;
	opennova::world::item_effect_identity_aliases(p_effect->get_a(), p_effect->get_b(),
			p_effect->get_c(), p_effect->get_wire_handle(), aliases);
	if (started) {
		for (const std::string &alias : aliases) {
			item_fx_control_active_.insert(alias);
		}
		_activate_control_nodes(aliases);
	} else {
		for (const std::string &alias : aliases) {
			item_fx_control_active_.erase(alias);
		}
		_deactivate_control_nodes(aliases);
	}
	return true;
}

int ItemEffectDirector::_attach_item_effect_to_node(ObjectModel *p_node, int p_kind, int p_item_id,
		const Ref<ItemDatabase> &p_item_db_override, bool p_controller_active) {
	EffectWorld *effect_world = _effect_world();
	if (effect_world == nullptr || p_node == nullptr || p_item_id <= 0) {
		return 0;
	}
	const uint64_t node_id = p_node->get_instance_id();
	if (const ObjectID *registered = item_fx_registered_nodes_.getptr(node_id)) {
		if (ObjectDB::get_instance(*registered) != nullptr) {
			return 0;
		}
	}
	Ref<ItemDatabase> item_db = p_item_db_override;
	if (item_db.is_null()) {
		item_db = _resolve_item_db();
	}
	if (item_db.is_null()) {
		return 0;
	}
	const uint32_t attrib = item_db->get_attrib(p_item_id);
	if (p_controller_active) {
		if (!opennova::world::item_effect_controller_allows(p_kind, attrib)) {
			return 0;
		}
	} else if (!opennova::world::item_effect_pool_allows(p_kind, attrib)) {
		if (!opennova::world::item_effect_controller_allows(p_kind, attrib)) {
			return 0;
		}
		if (_register_control_node(p_node, p_kind, p_item_id) &&
				_control_node_is_active(*item_fx_control_nodes_.getptr(node_id))) {
			return _attach_item_effect_to_node(p_node, p_kind, p_item_id, item_db, true);
		}
		return 0;
	}
	const ItemParticleFx fx = item_db->get_particle_fx(p_item_id);
	const String effect = fx.effect;
	const String userpoint = fx.userpoint;
	if (effect.is_empty()) {
		return 0;
	}
	const Ref<ObjectData> data = p_node->get_object_data();
	if (data.is_null()) {
		return 0;
	}
	if (effect_world->are_particles_hidden()) {
		// The retail master switch makes every spawn facade a no-op.
		// Remember persistent item attachments so re-enabling after a hidden
		// mission load creates them exactly once instead of losing them for
		// the mission.
		PendingNode pending;
		pending.node = ObjectID(node_id);
		pending.kind = p_kind;
		pending.item_id = p_item_id;
		pending.controller_active = p_controller_active;
		item_fx_pending_nodes_.insert(node_id, pending);
		return 0;
	}
	int attached = 0;
	const Ref<EntityRef> entity_ref = p_node->get_entity_ref();
	const Transform3D node_transform = p_node->get_global_transform();
	// The first-16 case-insensitive scan and the spawn_count==0 origin leg
	// are the engine's attach plan [orig: ItemDef_GetBoneMaskByName
	// @ 0x49ea40; duplicate names all set their bit].
	const opennova::world::ItemEffectAttachPlan plan =
			opennova::world::item_effect_attach_plan(data->native_model(), to_std(userpoint).c_str());
	for (const int i : plan.user_points) {
		const Ref<ModelUserPoint> info = data->get_user_point_info(i);
		const String key = item_fx_point_key(node_id, i);
		const Ref<EffectSpawnReceipt> receipt = effect_world->spawn_effect_attached_request(
				key, effect, node_transform, info->get_position(), info->get_rotation());
		if (receipt->get_spawned()) {
			item_fx_nodes_.insert(key, ObjectID(node_id));
			item_fx_owner_refs_.insert(key, entity_ref);
			if (p_controller_active) {
				_track_control_spawn(node_id, key, receipt);
			}
			++attached;
		}
	}
	if (plan.origin_fallback) {
		// No matched point (or no authored point name): ONE emitter at the
		// entity origin.
		const String key = item_fx_origin_key(node_id);
		const Ref<EffectSpawnReceipt> receipt = effect_world->spawn_effect_attached_request(
				key, effect, node_transform, Vector3(), Vector3());
		if (receipt->get_spawned()) {
			item_fx_nodes_.insert(key, ObjectID(node_id));
			item_fx_owner_refs_.insert(key, entity_ref);
			if (p_controller_active) {
				_track_control_spawn(node_id, key, receipt);
			}
			++attached;
		}
	}
	if (attached > 0) {
		item_fx_registered_nodes_.insert(node_id, ObjectID(node_id));
		item_fx_pending_nodes_.erase(node_id);
	}
	return attached;
}

bool ItemEffectDirector::_spawn_static_item_effect(EffectWorld *p_effect_world,
		const String &p_effect, const Transform3D &p_transform) {
	Ref<EffectSpawnOptions> options;
	options.instantiate();
	options->set_admission(EffectScene::ADMISSION_ALWAYS);
	options->set_binding(EffectScene::BINDING_WORLD);
	options->set_render_domain(EffectScene::RENDER_DOMAIN_WORLD);
	const Ref<EffectSpawnReceipt> receipt =
			p_effect_world->spawn_effect_request(p_effect, p_transform, options);
	return receipt->get_spawned();
}

int ItemEffectDirector::_attach_item_effect_to_static(const Ref<StaticEffectSource> &p_source,
		int p_source_index, const Ref<ItemDatabase> &p_item_db_override) {
	EffectWorld *effect_world = _effect_world();
	if (effect_world == nullptr || p_source.is_null() || p_source_index < 0) {
		return 0;
	}
	if (item_fx_registered_static_.has(p_source_index)) {
		return 0;
	}
	const int item_id = p_source->get_item_id();
	const int kind = p_source->get_kind();
	if (item_id <= 0) {
		return 0;
	}
	Ref<ItemDatabase> item_db = p_item_db_override;
	if (item_db.is_null()) {
		item_db = _resolve_item_db();
	}
	if (item_db.is_null() ||
			!opennova::world::item_effect_pool_allows(kind, item_db->get_attrib(item_id))) {
		return 0;
	}
	const ItemParticleFx fx = item_db->get_particle_fx(item_id);
	const String effect = fx.effect;
	const String userpoint = fx.userpoint;
	const Ref<ObjectData> data = p_source->get_object_data();
	if (effect.is_empty() || data.is_null()) {
		return 0;
	}
	if (effect_world->are_particles_hidden()) {
		item_fx_pending_static_.insert(p_source_index, p_source);
		return 0;
	}
	const Transform3D entity_transform = p_source->get_world_transform();
	int attached = 0;
	// The same engine attach plan as the animated leg [orig:
	// ItemDef_GetBoneMaskByName @ 0x49ea40]; static sources compose the
	// EffectWorld.spawn_effect_attached local pose once with their placement
	// transform, and the spawn_count==0 leg keeps the entity basis (an
	// attached origin pose at the moment it becomes world-bound).
	const opennova::world::ItemEffectAttachPlan plan =
			opennova::world::item_effect_attach_plan(data->native_model(), to_std(userpoint).c_str());
	for (const int i : plan.user_points) {
		const Ref<ModelUserPoint> info = data->get_user_point_info(i);
		const Transform3D local_pose = EffectWorld::forward_pose(info->get_position(), info->get_rotation());
		if (_spawn_static_item_effect(effect_world, effect, entity_transform * local_pose)) {
			++attached;
		}
	}
	if (plan.origin_fallback && _spawn_static_item_effect(effect_world, effect, entity_transform)) {
		++attached;
	}
	if (attached > 0) {
		item_fx_registered_static_.insert(p_source_index);
		item_fx_pending_static_.erase(p_source_index);
	}
	return attached;
}

void ItemEffectDirector::_retry_pending_item_effects() {
	EffectWorld *effect_world = _effect_world();
	if (effect_world == nullptr || effect_world->are_particles_hidden()) {
		return;
	}
	Vector<uint64_t> pending_ids;
	for (const KeyValue<uint64_t, PendingNode> &kv : item_fx_pending_nodes_) {
		pending_ids.push_back(kv.key);
	}
	for (const uint64_t node_id : pending_ids) {
		const PendingNode *entry = item_fx_pending_nodes_.getptr(node_id);
		if (entry == nullptr) {
			continue;
		}
		// A wire node may have despawned while particles were disabled: the
		// freed instance is rejected before any typed read.
		ObjectModel *node = live_model(entry->node);
		if (node == nullptr) {
			item_fx_pending_nodes_.erase(node_id);
			continue;
		}
		const int kind = entry->kind;
		const int item_id = entry->item_id;
		const bool controller_active = entry->controller_active;
		if (controller_active) {
			const ControlNode *control_entry = item_fx_control_nodes_.getptr(node_id);
			if (control_entry == nullptr || !_control_node_is_active(*control_entry)) {
				item_fx_pending_nodes_.erase(node_id);
				continue;
			}
		}
		_attach_item_effect_to_node(node, kind, item_id, Ref<ItemDatabase>(), controller_active);
	}
	Vector<int> static_ids;
	for (const KeyValue<int, Ref<StaticEffectSource>> &kv : item_fx_pending_static_) {
		static_ids.push_back(kv.key);
	}
	for (const int source_index : static_ids) {
		const Ref<StaticEffectSource> *source = item_fx_pending_static_.getptr(source_index);
		if (source == nullptr) {
			continue;
		}
		const Ref<StaticEffectSource> pending = *source;
		_attach_item_effect_to_static(pending, source_index);
	}
}

void ItemEffectDirector::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "world", "static_sources", "item_db_source"),
			&ItemEffectDirector::setup);
	ClassDB::bind_method(D_METHOD("set_particles_hidden", "hidden"),
			&ItemEffectDirector::set_particles_hidden);
	ClassDB::bind_method(D_METHOD("particles_hidden"), &ItemEffectDirector::particles_hidden);
	ClassDB::bind_method(D_METHOD("register_effect_anchor", "owner_key", "resolver"),
			&ItemEffectDirector::register_effect_anchor);
	ClassDB::bind_method(D_METHOD("unregister_effect_anchor", "owner_key"),
			&ItemEffectDirector::unregister_effect_anchor);
	ClassDB::bind_method(D_METHOD("has_effect_anchor", "owner_key"),
			&ItemEffectDirector::has_effect_anchor);
	ClassDB::bind_method(D_METHOD("get_stats"), &ItemEffectDirector::get_stats);
	ClassDB::bind_method(D_METHOD("on_effect_world_started"),
			&ItemEffectDirector::on_effect_world_started);
	ClassDB::bind_method(D_METHOD("reset"), &ItemEffectDirector::reset);
	ClassDB::bind_method(D_METHOD("resolve_owner_transform", "owner_key"),
			&ItemEffectDirector::resolve_owner_transform);
	ClassDB::bind_method(D_METHOD("reattach"), &ItemEffectDirector::reattach);
	ClassDB::bind_method(D_METHOD("on_wire_node_spawned", "node", "kind", "item_id"),
			&ItemEffectDirector::on_wire_node_spawned);
	ClassDB::bind_method(D_METHOD("consume_control_effect", "effect"),
			&ItemEffectDirector::consume_control_effect);
}
