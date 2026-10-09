#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <cstdint>
#include <functional>

namespace opennova::renderer {
struct ScarDrawList;
} // namespace opennova::renderer

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

	// Where a compiled list's vertices stand and who owns its entity rings (from_compiled).
	struct CompiledFrame {
		// The compile ran in mission space: the shared ring (and the entity rings' world-space form)
		// takes the world fold to Godot, an entity ring's section-local slots the model fold. False:
		// the compile ran in the device's own space already (a preview's range), every vertex as it
		// stands.
		bool mission_space = true;
		// An entity-ring owner's mission identity (bms_id, spawn_origin) for the shell's node
		// resolution, left at (0, world::kSpawnOriginNone) where the owner is gone. Null: the run has
		// no entity rings to resolve (a preview's range, a mission's shots made world-space), and its
		// entity-ring batches are left out of the record.
		std::function<void(uint16_t p_owner_packed, int32_t &r_bms_id, int64_t &r_spawn_origin)>
				owner_identity;
		// The pool's leased-ring counter, as the run reports it.
		int rings_leased = 0;
	};
	// The record ScarPresenter uploads, packed from the engine's compiled list
	// (renderer::compile_scar_draws): the triangle stream in the Godot frame, one row per batch,
	// the strip table and the pool counters. Simulation::get_scar_draw_list packs the world's
	// scars through it.
	static Ref<ScarDrawList> from_compiled(const opennova::renderer::ScarDrawList &p_list,
			const CompiledFrame &p_frame);
};

} // namespace godot
