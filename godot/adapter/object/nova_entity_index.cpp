// NovaEntityIndex — ported verbatim from mission_entity_registry.gd
// (2026-08-09 de-scripting).

#include "object/nova_entity_index.h"

namespace godot {

void NovaEntityIndex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build", "entries", "area_triggers"), &NovaEntityIndex::build);
	ClassDB::bind_method(D_METHOD("clear"), &NovaEntityIndex::clear);
	ClassDB::bind_method(D_METHOD("get_generation"), &NovaEntityIndex::get_generation);
	ClassDB::bind_method(D_METHOD("resolve", "bms_id", "kind", "index"),
			&NovaEntityIndex::resolve);
	ClassDB::bind_method(D_METHOD("resolve_single", "bms_id"),
			&NovaEntityIndex::resolve_single);
	ClassDB::bind_method(D_METHOD("resolve_group", "group_id"),
			&NovaEntityIndex::resolve_group);
	ClassDB::bind_method(D_METHOD("resolve_zone", "zone_index"),
			&NovaEntityIndex::resolve_zone);
	ClassDB::bind_method(D_METHOD("get_animatable_nodes"),
			&NovaEntityIndex::get_animatable_nodes);
}

void NovaEntityIndex::build(const Array &p_entries, const Array &p_area_triggers) {
	clear();
	area_triggers_ = p_area_triggers;
	for (int64_t i = 0; i < p_entries.size(); ++i) {
		const Dictionary entry = p_entries[i];
		NovaObjectModel *model =
				Object::cast_to<NovaObjectModel>(Object::cast_to<Object>(entry["model"]));
		if (model == nullptr) {
			continue;
		}
		const Dictionary ref = entry["ref"];
		const ObjectID id = ObjectID(model->get_instance_id());
		const int64_t bms_id = int64_t(ref.get("bms_id", 0));
		if (bms_id != 0) {
			by_bms_id_[bms_id] = id;
		}
		const int64_t kind = int64_t(ref.get("kind", -1));
		const int64_t index = int64_t(ref.get("index", -1));
		if (kind >= 0 && index >= 0) {
			by_kind_index_[origin_key(kind, index)] = id;
		}
		const int64_t group = int64_t(ref.get("group", -1));
		if (group >= 0) {
			by_group_[group].push_back(id);
		}
		EntityRecord record;
		record.model_id = id;
		record.position = ref.get("position", Vector3());
		record.team = int(ref.get("team", -1));
		records_.push_back(record);
	}
}

void NovaEntityIndex::clear() {
	++generation_;
	by_bms_id_.clear();
	by_kind_index_.clear();
	by_group_.clear();
	records_.clear();
	area_triggers_ = Array();
}

NovaObjectModel *NovaEntityIndex::resolve(int64_t p_bms_id, int64_t p_kind,
		int64_t p_index) const {
	if (p_bms_id != 0) {
		const ObjectID *id = by_bms_id_.getptr(p_bms_id);
		if (id != nullptr) {
			NovaObjectModel *model = live_model(*id);
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

NovaObjectModel *NovaEntityIndex::resolve_single(int64_t p_bms_id) const {
	if (p_bms_id == 0) {
		return nullptr;
	}
	const ObjectID *id = by_bms_id_.getptr(p_bms_id);
	return id != nullptr ? live_model(*id) : nullptr;
}

Array NovaEntityIndex::resolve_group(int64_t p_group_id) const {
	Array out;
	if (p_group_id < 0) {
		return out;
	}
	const Vector<ObjectID> *ids = by_group_.getptr(p_group_id);
	if (ids == nullptr) {
		return out;
	}
	for (const ObjectID &id : *ids) {
		NovaObjectModel *model = live_model(id);
		if (model != nullptr) {
			out.append(model);
		}
	}
	return out;
}

Array NovaEntityIndex::resolve_zone(int64_t p_zone_index) const {
	Array out;
	if (p_zone_index < 0 || p_zone_index >= area_triggers_.size()) {
		return out;
	}
	const Dictionary trig = area_triggers_[p_zone_index];
	const Vector3 amin = trig.get("min", Vector3());
	const Vector3 amax = trig.get("max", Vector3());
	const Vector3 lo(MIN(amin.x, amax.x), MIN(amin.y, amax.y), MIN(amin.z, amax.z));
	const Vector3 hi(MAX(amin.x, amax.x), MAX(amin.y, amax.y), MAX(amin.z, amax.z));
	const bool check_z = bool(trig.get("constrain_z", false));
	for (const EntityRecord &record : records_) {
		NovaObjectModel *model = live_model(record.model_id);
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

Array NovaEntityIndex::get_animatable_nodes() const {
	Array out;
	for (const EntityRecord &record : records_) {
		NovaObjectModel *model = live_model(record.model_id);
		if (model != nullptr) {
			out.append(model);
		}
	}
	return out;
}

} // namespace godot
