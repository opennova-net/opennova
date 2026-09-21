#pragma once

// Device adapter for the portable presentation entity index. ObjectDB owns
// liveness; the engine owns identity, group and mission-zone selection.

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "mission/mission_data.h"
#include <runtime/world/entity_index.h>
#include <vector>
#include "object/object_model.h"

namespace godot {

class EntityIndex : public RefCounted {
	GDCLASS(EntityIndex, RefCounted)

	opennova::world::EntityIndex index_;

	ObjectModel *live_model(uint64_t p_id) const {
		return p_id == 0 ? nullptr : Object::cast_to<ObjectModel>(ObjectDB::get_instance(ObjectID(p_id)));
	}
	void append_live(const std::vector<uint64_t> &p_ids, Array &r_out) const;

protected:
	static void _bind_methods();

public:
	// (Re)build from the placer's registered models (each carrying its
	// EntityRef). The native mission supplies its rects for zone
	// resolution (empty for wire-header joiners).
	void build(const TypedArray<ObjectModel> &p_models,
			const Ref<MissionData> &p_mission);
	void clear();
	int64_t get_generation() const { return static_cast<int64_t>(index_.generation()); }

	// THE resolver for the present path: by file id (bms_id) first — stable
	// for saved loose missions — then by (kind, index), which also supports an
	// in-memory bms::File with bms_id 0 in isolated tests/tooling previews.
	ObjectModel *resolve(int64_t p_bms_id, int64_t p_kind, int64_t p_index) const;
	ObjectModel *resolve_single(int64_t p_bms_id) const;
	// All live members of a group ([] for unknown/empty).
	Array resolve_group(int64_t p_group_id) const;
	// All live entities whose mission-space position falls inside area-trigger
	// `zone_index` (X/Y always tested; Z only when the trigger constrains it).
	Array resolve_zone(int64_t p_zone_index) const;
	// The model column of the animatable set (F3 Animation & models rows).
	// Freed instances are filtered here.
	Array get_animatable_nodes() const;
};

} // namespace godot
