#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace godot {

// The F3 entity picker's card (Simulation::debug_pick_entity): one plain
// trace_projectile segment along a camera or crosshair ray. Every field is
// present with its typed default for every outcome (the stable-card
// convention): `hit` false with `blocked` = "terrain" / "water" / "proxy"
// names why the ray stopped without a pickable entity; a hit fills the
// entity identity, the Godot-space positions and the surface facts. The
// shell's picker stamps the provenance trio (source, ray origin, ray
// direction) so a snapshot replays the pick. duplicate() is the pick list's
// value copy.
#define DEBUG_PICK_CARD_FIELDS(X)                    \
	X(bool, hit, false)                              \
	X(String, blocked, String())                     \
	X(String, hit_class, String())                   \
	X(int, entity_handle, -1)                        \
	X(int, pool, -1)                                 \
	X(int, kind, -1)                                 \
	X(int, index, -1)                                \
	X(int, bms_id, 0)                                \
	X(int, net_id, 0)                                \
	X(int, item_id, 0)                               \
	X(String, name, String())                        \
	X(Vector3, position_godot, Vector3())            \
	X(float, bound_radius, 0.0f)                     \
	X(Vector3, hit_position_godot, Vector3())        \
	X(Vector3, hit_normal_godot, Vector3())          \
	X(float, distance_units, 0.0f)                   \
	X(int, section, -1)                              \
	X(int, face, -1)                                 \
	X(int, bone, -1)                                 \
	X(int, hit_zone, -1)                             \
	X(int, surface_type, -1)                         \
	X(int64_t, material_flags, 0)                    \
	X(int64_t, tick, 0)                              \
	X(String, source, String())                      \
	X(Vector3, ray_origin_godot, Vector3())          \
	X(Vector3, ray_dir_godot, Vector3())

class DebugPickCard : public RefCounted {
	GDCLASS(DebugPickCard, RefCounted)

public:
#define DEBUG_PICK_CARD_ACCESSORS(m_type, m_name, m_default)  \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	DEBUG_PICK_CARD_FIELDS(DEBUG_PICK_CARD_ACCESSORS)
#undef DEBUG_PICK_CARD_ACCESSORS

	Ref<DebugPickCard> duplicate() const;

protected:
	static void _bind_methods();

private:
#define DEBUG_PICK_CARD_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;
	DEBUG_PICK_CARD_FIELDS(DEBUG_PICK_CARD_MEMBER)
#undef DEBUG_PICK_CARD_MEMBER
};

} // namespace godot
