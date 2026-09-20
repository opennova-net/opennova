#include "simulation/vehicle_trail_presenter.h"

#include "particle/effect_spawn_records.h"
#include "particle/effect_world.h"
#include "simulation/simulation.h"
#include "world/item_effect_director.h"
#include "util/string_convert.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/templates/vector.hpp>

#include <runtime/world/entity.h>

using namespace godot;

namespace {

String trail_group_key(const opennova::world::VehicleTrailVisualRow &row) {
	return vformat("vehicle-trail:%d:%d:p%d", static_cast<int64_t>(row.registry_spawn_id),
			row.handle_packed, row.point);
}

} // namespace

void VehicleTrailPresenter::_bind_methods() {}

void VehicleTrailPresenter::setup(
		Simulation *p_sim, EffectWorld *p_fx, const Ref<ItemEffectDirector> &p_anchors) {
	reset_runtime_state();
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	fx_id_ = p_fx != nullptr ? p_fx->get_instance_id() : ObjectID();
	anchors_ = p_anchors;
}

Simulation *VehicleTrailPresenter::sim() const {
	return Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
}

EffectWorld *VehicleTrailPresenter::fx() const {
	return Object::cast_to<EffectWorld>(ObjectDB::get_instance(fx_id_));
}

void VehicleTrailPresenter::sync_fixed_tick_effects() {
	Simulation *simulation = sim();
	if (simulation == nullptr) {
		return;
	}
	std::vector<opennova::world::VehicleEffectEvent> effects;
	simulation->drain_vehicle_effects(effects);
	if (EffectWorld *effect_world = fx()) {
		for (const auto &effect : effects) {
			const Vector3 pos(effect.position.x, effect.position.z, -effect.position.y);
			const Vector3 dir(effect.direction.x, effect.direction.z, -effect.direction.y);
			if (effect.kind != opennova::world::VehicleEffectEvent::Kind::Spawn) {
				apply_zone_group_event(effect_world, effect, pos, dir);
				continue;
			}
			Ref<EffectSpawnOptions> options;
			options.instantiate();
			options->set_source_tick(effect.source_tick);
			options->set_force_zone(effect.force_zone);
			effect_world->spawn_effect_request(opennova::to_gd(effect.effect),
					EffectWorld::forward_pose(pos, dir), options);
		}
	}
	std::vector<opennova::world::VehicleTrailVisualRow> rows;
	simulation->fill_vehicle_trail_visual_rows(rows);
	sync_visuals(rows);
	sync_water_wakes();
}

// The zone's surface-effect group, executed as the sim orders it. Release
// stops the group (retail: sub_5F6C70 -> sub_5E5ED0 notifies each child dead)
// and drops the record; Ensure creates the group at the hit with the zone-less
// spawn (its scheduled particles search their zone); Trigger re-spawns every
// child at the hit with the zone as the spawn window. A group the scene has
// already reaped answers the trigger with false and keeps its record: retail's
// slot holds the freed instance pointer until the surface effect changes. The
// witness (WeatherParticle_UpdateAllEmitters, CEffectWorld_SpawnEmitterAtPosition
// with dest{type 4, id, hit}, sub_5F6C10 -> CEffectWorld_SpawnAllActiveChildren
// with direction (0, 1, 0)) is cited at the sim's producer (world/rotor_wash.cpp)
// and the portable scene's trigger (particle/effect_scene.h).
void VehicleTrailPresenter::apply_zone_group_event(EffectWorld *p_fx,
		const opennova::world::VehicleEffectEvent &p_event, const Vector3 &p_position,
		const Vector3 &p_direction) {
	using Kind = opennova::world::VehicleEffectEvent::Kind;
	const int zone = p_event.force_zone;
	ZoneGroup *group = zone_groups_.getptr(zone);
	switch (p_event.kind) {
		case Kind::ReleaseZoneGroup:
			if (group != nullptr) {
				if (group->group_id != 0) {
					p_fx->stop_group(group->group_id);
				}
				zone_groups_.erase(zone);
			}
			break;
		case Kind::EnsureZoneGroup: {
			if (group != nullptr && group->group_id != 0) {
				p_fx->stop_group(group->group_id);
			}
			Ref<EffectSpawnOptions> options;
			options.instantiate();
			options->set_source_tick(p_event.source_tick);
			const Ref<EffectSpawnReceipt> receipt =
					p_fx->spawn_effect_request(opennova::to_gd(p_event.effect),
							EffectWorld::forward_pose(p_position, p_direction), options);
			ZoneGroup created;
			created.effect = opennova::to_gd(p_event.effect);
			created.group_id = receipt.is_valid() && receipt->get_spawned()
					? receipt->get_group_id()
					: 0;
			zone_groups_.insert(zone, created);
			break;
		}
		case Kind::TriggerZoneGroup:
			if (group != nullptr && group->group_id != 0) {
				p_fx->trigger_group_children(group->group_id, p_position, p_direction, zone);
			}
			break;
		case Kind::Spawn:
			break;
	}
}

void VehicleTrailPresenter::sync_visuals(
		const std::vector<opennova::world::VehicleTrailVisualRow> &p_visuals) {
	HashSet<String> seen;
	for (const auto &row : p_visuals) {
		if (row.handle_packed < 0 || row.registry_spawn_id == 0 || row.point >= 16 ||
				row.effect.empty())
			continue;
		const String key = trail_group_key(row);
		seen.insert(key);
		const Vector3 pos(row.pos.x, row.pos.z, -row.pos.y);
		const Vector3 dir(row.dir.x, row.dir.z, -row.dir.y);
		present_group(key, opennova::to_gd(row.effect), EffectWorld::forward_pose(pos, dir),
				float(row.magnitude_q16) / 65536.0f);
	}
	Vector<String> gone;
	for (const auto &kv : groups_)
		if (!seen.has(kv.key))
			gone.push_back(kv.key);
	for (const String &key : gone)
		retire_group(key);
}

void VehicleTrailPresenter::present_group(const String &p_key, const String &p_effect,
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
			p_key, callable_mp(this, &VehicleTrailPresenter::resolve_trail_anchor).bind(p_key));
	const Ref<EffectSpawnReceipt> receipt = effect_world->spawn_effect_owned_request(
			p_key, p_effect, p_transform.origin, p_transform.basis.get_column(2));
	if (receipt.is_null() || !receipt->get_spawned()) {
		anchors_->unregister_effect_anchor(p_key);
		transforms_.erase(p_key);
		effect_world->release_effect_binding(p_key);
		return;
	}
	TrailGroup group;
	group.effect = p_effect;
	group.owner_key = p_key;
	group.group_id = receipt->get_group_id();
	groups_.insert(p_key, group);
	if (!effect_world->set_group_parameters(group.group_id, p_magnitude, p_magnitude)) {
		retire_group(p_key);
	}
}

Variant VehicleTrailPresenter::resolve_trail_anchor(String p_key) {
	if (!groups_.has(p_key)) {
		return Variant();
	}
	const Transform3D *transform = transforms_.getptr(p_key);
	return transform != nullptr ? Variant(*transform) : Variant();
}

void VehicleTrailPresenter::retire_group(const String &p_key) {
	const TrailGroup *found = groups_.getptr(p_key);
	const bool had_group = found != nullptr;
	const TrailGroup group = had_group ? *found : TrailGroup();
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

void VehicleTrailPresenter::reset_runtime_state() {
	if (auto *node = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(water_mesh_id_)))
		node->queue_free();
	water_mesh_id_ = ObjectID();
	water_mesh_.unref();
	water_material_.unref();
	Vector<String> keys;
	for (const KeyValue<String, TrailGroup> &kv : groups_) {
		keys.push_back(kv.key);
	}
	for (const String &key : keys) {
		retire_group(key);
	}
	transforms_.clear();
	if (EffectWorld *effect_world = fx()) {
		for (const KeyValue<int, ZoneGroup> &kv : zone_groups_) {
			if (kv.value.group_id != 0) {
				effect_world->stop_group(kv.value.group_id);
			}
		}
	}
	zone_groups_.clear();
}

void VehicleTrailPresenter::teardown() {
	reset_runtime_state();
}
