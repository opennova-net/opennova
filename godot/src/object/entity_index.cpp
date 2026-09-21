// EntityIndex — ported verbatim from mission_entity_registry.gd
// (2026-08-09 de-scripting).

#include "object/entity_index.h"

namespace godot {

void EntityIndex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build", "models", "mission"), &EntityIndex::build);
	ClassDB::bind_method(D_METHOD("clear"), &EntityIndex::clear);
	ClassDB::bind_method(D_METHOD("get_generation"), &EntityIndex::get_generation);
	ClassDB::bind_method(D_METHOD("resolve", "bms_id", "kind", "index"),
			&EntityIndex::resolve);
	ClassDB::bind_method(D_METHOD("resolve_single", "bms_id"),
			&EntityIndex::resolve_single);
	ClassDB::bind_method(D_METHOD("resolve_group", "group_id"),
			&EntityIndex::resolve_group);
	ClassDB::bind_method(D_METHOD("resolve_zone", "zone_index"),
			&EntityIndex::resolve_zone);
	ClassDB::bind_method(D_METHOD("get_animatable_nodes"),
			&EntityIndex::get_animatable_nodes);
}

void EntityIndex::build(const TypedArray<ObjectModel> &p_models,
		const Ref<MissionData> &p_mission) {
	clear();
	if (p_mission.is_valid()) area_triggers_ = opennova::mission::area_triggers(p_mission->native_file());
	for (int64_t i = 0; i < p_models.size(); ++i) {
		ObjectModel *model =
				Object::cast_to<ObjectModel>(Object::cast_to<Object>(p_models[i]));
		if (model == nullptr) {
			continue;
		}
		const Ref<EntityRef> ref = model->get_entity_ref();
		if (ref.is_null()) {
			continue;
		}
		const ObjectID id = ObjectID(model->get_instance_id());
		const int64_t bms_id = ref->get_bms_id();
		if (bms_id != 0) {
			by_bms_id_[bms_id] = id;
		}
		const int64_t kind = ref->get_kind();
		const int64_t index = ref->get_index();
		if (kind >= 0 && index >= 0) {
			by_kind_index_[origin_key(kind, index)] = id;
		}
		const int64_t group = ref->get_group();
		if (group >= 0) {
			by_group_[group].push_back(id);
		}
		EntityRecord record;
		record.model_id = id;
		record.position = ref->get_position();
		record.team = ref->get_team();
		records_.push_back(record);
	}
}

void EntityIndex::clear() {
	++generation_;
	by_bms_id_.clear();
	by_kind_index_.clear();
	by_group_.clear();
	records_.clear();
	area_triggers_.clear();
}

ObjectModel *EntityIndex::resolve(int64_t p_bms_id, int64_t p_kind,
		int64_t p_index) const {
	if (p_bms_id != 0) {
		const ObjectID *id = by_bms_id_.getptr(p_bms_id);
		if (id != nullptr) {
			ObjectModel *model = live_model(*id);
			if (model != nullptr) {
				return model;
			}
		}
	}
	if (p_kind >= 0 && p_index >= 0) {
		const ObjectID *id = by_kind_index_.getptr(origin_key(p_kind, p_index));
		if (id != nullptr) {
			return live_model(*id);
		}
	}
	return nullptr;
}

ObjectModel *EntityIndex::resolve_single(int64_t p_bms_id) const {
	if (p_bms_id == 0) {
		return nullptr;
	}
	const ObjectID *id = by_bms_id_.getptr(p_bms_id);
	return id != nullptr ? live_model(*id) : nullptr;
}

Array EntityIndex::resolve_group(int64_t p_group_id) const {
	Array out;
	if (p_group_id < 0) {
		return out;
	}
	const Vector<ObjectID> *ids = by_group_.getptr(p_group_id);
	if (ids == nullptr) {
		return out;
	}
	for (const ObjectID &id : *ids) {
		ObjectModel *model = live_model(id);
		if (model != nullptr) {
			out.append(model);
		}
	}
	return out;
}

Array EntityIndex::resolve_zone(int64_t p_zone_index) const {
	Array out;
	if (p_zone_index < 0 || static_cast<size_t>(p_zone_index) >= area_triggers_.size()) {
		return out;
	}
	const auto &trig = area_triggers_[static_cast<size_t>(p_zone_index)];
	const Vector3 amin(trig.min_x, trig.min_y, trig.min_z);
	const Vector3 amax(trig.max_x, trig.max_y, trig.max_z);
	const Vector3 lo(MIN(amin.x, amax.x), MIN(amin.y, amax.y), MIN(amin.z, amax.z));
	const Vector3 hi(MAX(amin.x, amax.x), MAX(amin.y, amax.y), MAX(amin.z, amax.z));
	const bool check_z = trig.constrain_z;
	for (const EntityRecord &record : records_) {
		ObjectModel *model = live_model(record.model_id);
		if (model == nullptr) {
			continue;
		}
		const Vector3 p = record.position;
		if (p.x < lo.x || p.x > hi.x || p.y < lo.y || p.y > hi.y) {
			continue;
		}
		if (check_z && (p.z < lo.z || p.z > hi.z)) {
			continue;
		}
		out.append(model);
	}
	return out;
}

Array EntityIndex::get_animatable_nodes() const {
	Array out;
	for (const EntityRecord &record : records_) {
		ObjectModel *model = live_model(record.model_id);
		if (model != nullptr) {
			out.append(model);
		}
	}
	return out;
}

} // namespace godot
