#pragma once

// The mission entity index, NATIVE (the former mission_entity_registry.gd):
// maps a loaded mission's live animated entity models back to the identities
// a mission ACTION targets — a single entity's SSN (bms_id), a group id, or
// an area-trigger zone. Built once from the placer's registered models, each
// carrying its EntityRef (construction-time registration, never a child
// scan); the present appliers resolve through direct typed calls.

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "object/object_model.h"

namespace godot {

class EntityIndex : public RefCounted {
	GDCLASS(EntityIndex, RefCounted)

	struct EntityRecord {
		ObjectID model_id;
		Vector3 position; // mission-space
		int team = -1;
	};

	HashMap<int64_t, ObjectID> by_bms_id_;
	HashMap<int64_t, ObjectID> by_kind_index_;
	HashMap<int64_t, Vector<ObjectID>> by_group_;
	Vector<EntityRecord> records_;
	Array area_triggers_;
	int64_t generation_ = 0;

	// Keep both signed 32-bit inputs distinct without a formatted String in
	// the per-frame present path.
	static int64_t origin_key(int64_t p_kind, int64_t p_index) {
		return (p_kind << 32) | (p_index & 0xffffffff);
	}

	ObjectModel *live_model(const ObjectID &p_id) const {
		return Object::cast_to<ObjectModel>(ObjectDB::get_instance(p_id));
	}

protected:
	static void _bind_methods();

public:
	// (Re)build from the placer's registered models (each carrying its
	// EntityRef). `area_triggers` supplies the mission's rects for zone
	// resolution (empty for wire-header joiners).
	void build(const TypedArray<ObjectModel> &p_models, const Array &p_area_triggers);
	void clear();
	int64_t get_generation() const { return generation_; }

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
