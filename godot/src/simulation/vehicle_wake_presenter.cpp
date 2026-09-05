#include "simulation/vehicle_wake_presenter.h"

#include "mission/mission_object_placer.h"
#include "object/entity_index.h"
#include "object/model_user_point.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "particle/effect_spawn_records.h"
#include "particle/effect_world.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"
#include "world/item_effect_director.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/vector.hpp>

#include <runtime/world/entity.h>
#include <runtime/world/item_effects.h>

using namespace godot;

namespace {

String wake_group_key(const opennova::world::VehicleWakeVisualRow &p_row, int p_slot, int p_point) {
	return vformat("vehicle-wake:%d:%d:w%d:p%d", static_cast<int64_t>(p_row.registry_spawn_id),
			p_row.handle_packed, p_slot, p_point);
}

} // namespace

void VehicleWakePresenter::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("resolve_wake_anchor", "key"), &VehicleWakePresenter::resolve_wake_anchor);
}

void VehicleWakePresenter::setup(Simulation *p_sim, EntityPresenter *p_entities,
		const Ref<EntityIndex> &p_index, EffectWorld *p_fx,
		const Ref<ItemEffectDirector> &p_anchors) {
	reset_runtime_state();
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	entities_id_ = p_entities != nullptr ? p_entities->get_instance_id() : ObjectID();
	index_ = p_index;
	fx_id_ = p_fx != nullptr ? p_fx->get_instance_id() : ObjectID();
	anchors_ = p_anchors;
}

Simulation *VehicleWakePresenter::sim() const {
	return Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
}

EntityPresenter *VehicleWakePresenter::entities() const {
	return Object::cast_to<EntityPresenter>(ObjectDB::get_instance(entities_id_));
}

EffectWorld *VehicleWakePresenter::fx() const {
	return Object::cast_to<EffectWorld>(ObjectDB::get_instance(fx_id_));
}

void VehicleWakePresenter::sync_fixed_tick_effects() {
	Simulation *simulation = sim();
	if (simulation == nullptr) {
		return;
	}
	std::vector<opennova::world::VehicleWakeVisualRow> rows;
	simulation->fill_vehicle_wake_visual_rows(rows);
	sync_visuals(rows);
}

ObjectModel *VehicleWakePresenter::resolve_model(
		const opennova::world::VehicleWakeVisualRow &p_row) const {
	ObjectModel *model = nullptr;
	if (index_.is_valid() &&
			(p_row.bms_id != 0 || p_row.spawn_origin != opennova::world::kSpawnOriginNone)) {
		model = index_->resolve(p_row.bms_id,
				opennova::world::spawn_origin_kind(p_row.spawn_origin),
				opennova::world::spawn_origin_index(p_row.spawn_origin));
	}
	if (model == nullptr) {
		if (EntityPresenter *presenter = entities()) {
			model = presenter->resolve_wire_handle(p_row.handle_packed);
		}
	}
	return model;
}

void VehicleWakePresenter::sync_visuals(
		const std::vector<opennova::world::VehicleWakeVisualRow> &p_visuals) {
	HashSet<String> seen;
	for (const opennova::world::VehicleWakeVisualRow &row : p_visuals) {
		if (row.handle_packed < 0 || row.registry_spawn_id == 0 || !row.afloat) {
			continue;
		}
		ObjectModel *model = resolve_model(row);
		if (model == nullptr || model->get_object_data().is_null()) {
			continue; // retain no stale anchor; retry the unresolved row next tick
		}
		sync_lane(row, model, 3, row.w3_effect, row.w3_userpoint, row.w3_magnitude_q16, seen);
		sync_lane(row, model, 4, row.w4_effect, row.w4_userpoint, row.w4_magnitude_q16, seen);
	}

	Vector<String> gone;
	for (const KeyValue<String, WakeGroup> &kv : groups_) {
		if (!seen.has(kv.key)) {
			gone.push_back(kv.key);
		}
	}
	for (const String &key : gone) {
		retire_group(key);
	}
}

void VehicleWakePresenter::sync_lane(const opennova::world::VehicleWakeVisualRow &p_row,
		ObjectModel *p_model, int p_slot, const std::string &p_effect,
		const std::string &p_userpoint, uint32_t p_magnitude_q16, HashSet<String> &r_seen) {
	if (p_effect.empty() || p_userpoint.empty() || p_magnitude_q16 == 0) {
		return;
	}
	const Ref<ObjectData> data = p_model->get_object_data();
	const opennova::world::ItemEffectAttachPlan plan =
			opennova::world::item_effect_attach_plan(data->native_model(), p_userpoint.c_str());
	if (plan.user_points.empty()) {
		return; // W3/W4 never take the general item-effect origin fallback
	}
	const Transform3D vehicle_transform =
			MissionObjectPlacer::entity_transform(Vector3(p_row.pos.x, p_row.pos.y, p_row.pos.z),
					Vector3(p_row.pitch_deg, p_row.yaw_deg, p_row.roll_deg));
	const float water_height = static_cast<float>(p_row.water_z) / 65536.0f;
	const float magnitude = static_cast<float>(p_magnitude_q16) / 65536.0f;
	const String effect = String::utf8(p_effect.c_str());
	for (const int point_index : plan.user_points) {
		const Ref<ModelUserPoint> point = data->get_user_point_info(point_index);
		if (point.is_null()) {
			continue;
		}
		Transform3D transform = vehicle_transform *
				EffectWorld::forward_pose(point->get_position(), point->get_rotation());
		transform.origin.y = water_height;
		const String key = wake_group_key(p_row, p_slot, point_index);
		r_seen.insert(key);
		present_group(key, effect, transform, magnitude);
	}
}

void VehicleWakePresenter::present_group(const String &p_key, const String &p_effect,
		const Transform3D &p_transform, float p_magnitude) {
	bool has_group = groups_.has(p_key);
	if (has_group && groups_[p_key].effect != p_effect) {
		retire_group(p_key);
		has_group = false;
	}
	transforms_.insert(p_key, p_transform);
	EffectWorld *effect_world = fx();
	if (has_group) {
		if (effect_world == nullptr ||
				!effect_world->set_group_parameters(
						groups_[p_key].group_id, p_magnitude, p_magnitude)) {
			retire_group(p_key);
		}
		return;
	}
	if (effect_world == nullptr || anchors_.is_null()) {
		transforms_.erase(p_key);
		return;
	}
	anchors_->register_effect_anchor(
			p_key, callable_mp(this, &VehicleWakePresenter::resolve_wake_anchor).bind(p_key));
	const Ref<EffectSpawnReceipt> receipt = effect_world->spawn_effect_owned_request(
			p_key, p_effect, p_transform.origin, p_transform.basis.get_column(2));
	if (receipt.is_null() || !receipt->get_spawned()) {
		anchors_->unregister_effect_anchor(p_key);
		transforms_.erase(p_key);
		effect_world->release_effect_binding(p_key);
		return;
	}
	WakeGroup group;
	group.effect = p_effect;
	group.owner_key = p_key;
	group.group_id = receipt->get_group_id();
	groups_.insert(p_key, group);
	if (!effect_world->set_group_parameters(group.group_id, p_magnitude, p_magnitude)) {
		retire_group(p_key);
	}
}

Variant VehicleWakePresenter::resolve_wake_anchor(String p_key) {
	if (!groups_.has(p_key)) {
		return Variant();
	}
	const Transform3D *transform = transforms_.getptr(p_key);
	return transform != nullptr ? Variant(*transform) : Variant();
}

void VehicleWakePresenter::retire_group(const String &p_key) {
	const WakeGroup *found = groups_.getptr(p_key);
	const bool had_group = found != nullptr;
	const WakeGroup group = had_group ? *found : WakeGroup();
	groups_.erase(p_key);
	transforms_.erase(p_key);
	if (!had_group) {
		return;
	}
	if (anchors_.is_valid()) {
		anchors_->unregister_effect_anchor(group.owner_key);
	}
	if (EffectWorld *effect_world = fx()) {
		if (group.group_id != 0) {
			effect_world->stop_group(group.group_id);
		}
		effect_world->release_effect_binding(group.owner_key);
	}
}

void VehicleWakePresenter::reset_runtime_state() {
	Vector<String> keys;
	for (const KeyValue<String, WakeGroup> &kv : groups_) {
		keys.push_back(kv.key);
	}
	for (const String &key : keys) {
		retire_group(key);
	}
	transforms_.clear();
}

void VehicleWakePresenter::teardown() {
	reset_runtime_state();
}
