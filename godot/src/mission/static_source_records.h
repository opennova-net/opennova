#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

#include "object/object_data.h"

// MissionObjectPlacer's static read-back rows (get_static_*): value
// descriptors of successfully rendered static entities, minted fresh on
// every call — never a placed/render Node, never the placer's retained
// row. Read-write so a harness placer authors them.

#define STATIC_SOURCE_INT_ACCESSORS(m_name)                 \
	int get_##m_name() const { return m_name##_; }          \
	void set_##m_name(int p_value) { m_name##_ = p_value; }

namespace godot {

// One retained static entity for mission-start item effects: its record
// identity, graphic, base entity transform and data. Row order is placement
// order and stable for the mission; `source_index` is that row's position.
#define STATIC_EFFECT_SOURCE_INT_FIELDS(X) \
	X(kind, -1)                            \
	X(entity_index, -1)                    \
	X(bms_id, 0)                           \
	X(item_id, 0)                          \
	X(source_index, -1)                    \
	X(entity_bound_radius_q16, 0)

class StaticEffectSource : public RefCounted {
	GDCLASS(StaticEffectSource, RefCounted)

public:
#define STATIC_EFFECT_SOURCE_ACCESSOR(m_name, m_default) STATIC_SOURCE_INT_ACCESSORS(m_name)
	STATIC_EFFECT_SOURCE_INT_FIELDS(STATIC_EFFECT_SOURCE_ACCESSOR)
#undef STATIC_EFFECT_SOURCE_ACCESSOR
	String get_graphic() const { return graphic_; }
	void set_graphic(const String &p_graphic) { graphic_ = p_graphic; }
	Transform3D get_world_transform() const { return world_transform_; }
	void set_world_transform(const Transform3D &p_xform) { world_transform_ = p_xform; }
	Ref<ObjectData> get_object_data() const { return object_data_; }
	void set_object_data(const Ref<ObjectData> &p_data) { object_data_ = p_data; }

protected:
	static void _bind_methods();

private:
#define STATIC_EFFECT_SOURCE_MEMBER(m_name, m_default) int m_name##_ = m_default;
	STATIC_EFFECT_SOURCE_INT_FIELDS(STATIC_EFFECT_SOURCE_MEMBER)
#undef STATIC_EFFECT_SOURCE_MEMBER
	String graphic_;
	Transform3D world_transform_;
	Ref<ObjectData> object_data_;
};

// One retained static entity/ROBJ light draw: the atlas row stamped into
// the matching MultiMesh INSTANCE_CUSTOM.x, the effect source it belongs
// to, the record identity, the exact world AABB and the live carve state.
#define STATIC_LIGHT_DRAW_SOURCE_INT_FIELDS(X) \
	X(atlas_row, -1)                           \
	X(source_index, -1)                        \
	X(kind, -1)                                \
	X(entity_index, -1)                        \
	X(bms_id, 0)                               \
	X(item_id, 0)                              \
	X(robj_index, 0)

class StaticLightDrawSource : public RefCounted {
	GDCLASS(StaticLightDrawSource, RefCounted)

public:
#define STATIC_LIGHT_DRAW_SOURCE_ACCESSOR(m_name, m_default) STATIC_SOURCE_INT_ACCESSORS(m_name)
	STATIC_LIGHT_DRAW_SOURCE_INT_FIELDS(STATIC_LIGHT_DRAW_SOURCE_ACCESSOR)
#undef STATIC_LIGHT_DRAW_SOURCE_ACCESSOR
	AABB get_world_bounds() const { return world_bounds_; }
	void set_world_bounds(const AABB &p_bounds) { world_bounds_ = p_bounds; }
	bool is_active() const { return active_; }
	void set_active(bool p_active) { active_ = p_active; }

protected:
	static void _bind_methods();

private:
#define STATIC_LIGHT_DRAW_SOURCE_MEMBER(m_name, m_default) int m_name##_ = m_default;
	STATIC_LIGHT_DRAW_SOURCE_INT_FIELDS(STATIC_LIGHT_DRAW_SOURCE_MEMBER)
#undef STATIC_LIGHT_DRAW_SOURCE_MEMBER
	AABB world_bounds_;
	bool active_ = true;
};

// One static terrain-shadow source (the diagnostics mirror of the placer's
// StaticTerrainShadowSource for focused shell/asset tests).
#define STATIC_TERRAIN_SHADOW_SOURCE_INT_FIELDS(X) \
	X(bms_id, 0)                                   \
	X(item_id, 0)                                  \
	X(entity_kind, -1)                             \
	X(entity_index, -1)                            \
	X(team, 0)

class StaticTerrainShadowSourceRow : public RefCounted {
	GDCLASS(StaticTerrainShadowSourceRow, RefCounted)

public:
#define STATIC_TERRAIN_SHADOW_SOURCE_ACCESSOR(m_name, m_default) STATIC_SOURCE_INT_ACCESSORS(m_name)
	STATIC_TERRAIN_SHADOW_SOURCE_INT_FIELDS(STATIC_TERRAIN_SHADOW_SOURCE_ACCESSOR)
#undef STATIC_TERRAIN_SHADOW_SOURCE_ACCESSOR
	int64_t get_item_attrib() const { return item_attrib_; }
	void set_item_attrib(int64_t p_value) { item_attrib_ = p_value; }
	int64_t get_item_attrib2() const { return item_attrib2_; }
	void set_item_attrib2(int64_t p_value) { item_attrib2_ = p_value; }
	String get_graphic() const { return graphic_; }
	void set_graphic(const String &p_graphic) { graphic_ = p_graphic; }
	Transform3D get_world_transform() const { return world_transform_; }
	void set_world_transform(const Transform3D &p_xform) { world_transform_ = p_xform; }
	Ref<ObjectData> get_object_data() const { return object_data_; }
	void set_object_data(const Ref<ObjectData> &p_data) { object_data_ = p_data; }
	bool is_active() const { return active_; }
	void set_active(bool p_active) { active_ = p_active; }

protected:
	static void _bind_methods();

private:
#define STATIC_TERRAIN_SHADOW_SOURCE_MEMBER(m_name, m_default) int m_name##_ = m_default;
	STATIC_TERRAIN_SHADOW_SOURCE_INT_FIELDS(STATIC_TERRAIN_SHADOW_SOURCE_MEMBER)
#undef STATIC_TERRAIN_SHADOW_SOURCE_MEMBER
	int64_t item_attrib_ = 0;
	int64_t item_attrib2_ = 0;
	String graphic_;
	Transform3D world_transform_;
	Ref<ObjectData> object_data_;
	bool active_ = true;
};

} // namespace godot

#undef STATIC_SOURCE_INT_ACCESSORS
