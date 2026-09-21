// EntityIndex device glue: register model tokens and resolve live ObjectDB instances.

#include "object/entity_index.h"

#include <utility>

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
	std::vector<opennova::world::EntityIndexEntry> entries;
	entries.reserve(static_cast<size_t>(p_models.size()));
	for (int64_t i = 0; i < p_models.size(); ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(Object::cast_to<Object>(p_models[i]));
		if (model == nullptr) continue;
		const Ref<EntityRef> ref = model->get_entity_ref();
		if (ref.is_null()) continue;
		const Vector3 position = ref->get_position();
		entries.push_back({static_cast<uint64_t>(model->get_instance_id()),
				ref->get_bms_id(), ref->get_kind(), ref->get_index(), ref->get_group(),
				{position.x, position.y, position.z}});
	}
	std::vector<opennova::mission::AreaTriggerRecord> zones;
	if (p_mission.is_valid()) zones = opennova::mission::area_triggers(p_mission->native_file());
	index_.build(std::move(entries), std::move(zones));
}

void EntityIndex::clear() {
	index_.clear();
}

ObjectModel *EntityIndex::resolve(int64_t p_bms_id, int64_t p_kind, int64_t p_index) const {
	for (const auto id : index_.candidates(p_bms_id, p_kind, p_index))
		if (ObjectModel *model = live_model(id)) return model;
	return nullptr;
}

ObjectModel *EntityIndex::resolve_single(int64_t p_bms_id) const {
	return live_model(index_.by_bms_id(p_bms_id));
}

void EntityIndex::append_live(const std::vector<uint64_t> &p_ids, Array &r_out) const {
	for (const auto id : p_ids)
		if (ObjectModel *model = live_model(id)) r_out.append(model);
}

Array EntityIndex::resolve_group(int64_t p_group_id) const {
	Array out;
	append_live(index_.group(p_group_id), out);
	return out;
}

Array EntityIndex::resolve_zone(int64_t p_zone_index) const {
	Array out;
	append_live(index_.zone(p_zone_index), out);
	return out;
}

Array EntityIndex::get_animatable_nodes() const {
	Array out;
	for (const auto &entry : index_.entries())
		if (ObjectModel *model = live_model(entry.token)) out.append(model);
	return out;
}

} // namespace godot
