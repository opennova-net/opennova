#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

// One frame's impact-scar draw list as the presenter uploads it
// (Simulation.get_scar_draw_list -> ScarPresenter.present): the triangle
// stream (three per quad, already in the Godot frame — world space for the
// shared ring, section-local for an entity ring), one row per batch (owner
// packed handle, strip index, section, flags bit 0 entity-local / bit 1
// building, first vertex + count, and the owner's mission identity for the
// shell's node resolution), the world-space form of the entity-ring batches
// (`world_vertices`, a batch's run starting at `batch_world_first`, -1 when
// its owner's section matrix did not resolve; the batch's UVs and colours
// serve both forms), the 32-row strip table (TGA name + GfxShader mode word),
// and the pool counters. Read-write so a test authors one; the engine's
// renderer/scar_draw compile carries the witnesses.
class ScarDrawList : public RefCounted {
	GDCLASS(ScarDrawList, RefCounted)

#define SCAR_DRAW_LIST_FIELDS(X)                        \
	X(PackedVector3Array, vertices)                     \
	X(PackedVector2Array, uvs)                          \
	X(PackedColorArray, colors)                         \
	X(PackedInt32Array, batch_owner)                    \
	X(PackedInt32Array, batch_texture)                  \
	X(PackedInt32Array, batch_section)                  \
	X(PackedInt32Array, batch_flags)                    \
	X(PackedInt32Array, batch_first)                    \
	X(PackedInt32Array, batch_count)                    \
	X(PackedInt32Array, batch_bms_id)                   \
	X(PackedInt64Array, batch_spawn_origin)             \
	X(PackedVector3Array, world_vertices)               \
	X(PackedInt32Array, batch_world_first)              \
	X(PackedStringArray, strip_names)                   \
	X(PackedInt32Array, strip_mode_words)               \
	X(int, slots_live)                                  \
	X(int, slots_culled)                                \
	X(int, rings_leased)

#define SCAR_DRAW_LIST_MEMBER(m_type, m_name) m_type m_name##_{};
	SCAR_DRAW_LIST_FIELDS(SCAR_DRAW_LIST_MEMBER)
#undef SCAR_DRAW_LIST_MEMBER

protected:
	static void _bind_methods();

public:
#define SCAR_DRAW_LIST_ACCESSORS(m_type, m_name)                          \
	const m_type &get_##m_name() const { return m_name##_; }              \
	void set_##m_name(const m_type &p_value) { m_name##_ = p_value; }
	SCAR_DRAW_LIST_FIELDS(SCAR_DRAW_LIST_ACCESSORS)
#undef SCAR_DRAW_LIST_ACCESSORS

	// Entity-local batch flag bits (batch_flags).
	enum { FLAG_ENTITY_LOCAL = 1, FLAG_BUILDING = 2 };
};

} // namespace godot
