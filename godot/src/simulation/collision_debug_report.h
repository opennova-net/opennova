#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

// The collision-world debug payload for the F3 "Show collision" overlay
// (Simulation::get_collision_debug): the nearby BVOL instances with their
// posed volumes (the boxes movement resolves against), the vehicle platform
// probe boxes, the local player's last capsule resolve and the contact-debug
// hits channel. Godot space throughout; read-write so the view test authors
// a payload.

#define COLLISION_DEBUG_ACCESSORS(m_type, m_name, m_default)  \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define COLLISION_DEBUG_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// One BVOL volume: its type, the local-space extents and the eight posed
// world corners (index bit0 = max x, bit1 = max y, bit2 = max z).
#define COLLISION_DEBUG_VOLUME_FIELDS(X)                    \
	X(int, type, 0)                                         \
	X(float, min_x, 0.0f)                                   \
	X(float, max_x, 0.0f)                                   \
	X(float, min_y, 0.0f)                                   \
	X(float, max_y, 0.0f)                                   \
	X(float, min_z, 0.0f)                                   \
	X(float, max_z, 0.0f)                                   \
	X(PackedVector3Array, corners, PackedVector3Array())

class CollisionDebugVolume : public RefCounted {
	GDCLASS(CollisionDebugVolume, RefCounted)

public:
	COLLISION_DEBUG_VOLUME_FIELDS(COLLISION_DEBUG_ACCESSORS)

protected:
	static void _bind_methods();

private:
	COLLISION_DEBUG_VOLUME_FIELDS(COLLISION_DEBUG_MEMBER)
};

#define COLLISION_DEBUG_INSTANCE_FIELDS(X) \
	X(int, entity_handle, -1)              \
	X(Vector3, pos, Vector3())             \
	X(float, heading, 0.0f)

class CollisionDebugInstance : public RefCounted {
	GDCLASS(CollisionDebugInstance, RefCounted)

public:
	COLLISION_DEBUG_INSTANCE_FIELDS(COLLISION_DEBUG_ACCESSORS)
	TypedArray<CollisionDebugVolume> get_volumes() const { return volumes_; }
	void set_volumes(const TypedArray<CollisionDebugVolume> &p_value) { volumes_ = p_value; }
	void add_volume(const Ref<CollisionDebugVolume> &p_volume) { volumes_.push_back(p_volume); }

protected:
	static void _bind_methods();

private:
	COLLISION_DEBUG_INSTANCE_FIELDS(COLLISION_DEBUG_MEMBER)
	TypedArray<CollisionDebugVolume> volumes_;
};

// One D-VEH-1 platform probe box (`kind` "probe" or "footprint"), posed by the
// vehicle's live full-Euler placement.
#define COLLISION_PROBE_BOX_FIELDS(X)                       \
	X(int, entity_handle, -1)                               \
	X(String, kind, String())                               \
	X(PackedVector3Array, corners, PackedVector3Array())

class CollisionProbeBox : public RefCounted {
	GDCLASS(CollisionProbeBox, RefCounted)

public:
	COLLISION_PROBE_BOX_FIELDS(COLLISION_DEBUG_ACCESSORS)

protected:
	static void _bind_methods();

private:
	COLLISION_PROBE_BOX_FIELDS(COLLISION_DEBUG_MEMBER)
};

// The local player's last full resolve: the three capsule test points the
// resolver queried with their radii, and the returned foot clearance.
#define COLLISION_DEBUG_PLAYER_FIELDS(X)                    \
	X(bool, valid, false)                                   \
	X(Vector3, position, Vector3())                         \
	X(PackedVector3Array, points, PackedVector3Array())     \
	X(PackedFloat32Array, radii, PackedFloat32Array())      \
	X(float, capsule_bottom, 0.0f)                          \
	X(float, capsule_top, 0.0f)                             \
	X(float, foot_clearance, 0.0f)

class CollisionDebugPlayer : public RefCounted {
	GDCLASS(CollisionDebugPlayer, RefCounted)

public:
	COLLISION_DEBUG_PLAYER_FIELDS(COLLISION_DEBUG_ACCESSORS)

protected:
	static void _bind_methods();

private:
	COLLISION_DEBUG_PLAYER_FIELDS(COLLISION_DEBUG_MEMBER)
};

// `hits` is the contact-debug channel: stride-6 [target_handle (-1 none),
// age_ticks, kind, x, y, z] per event inside `hit_ttl`, oldest first.
class CollisionDebugReport : public RefCounted {
	GDCLASS(CollisionDebugReport, RefCounted)

public:
	static constexpr int kHitStride = 6;

	CollisionDebugReport();

	TypedArray<CollisionDebugInstance> get_instances() const { return instances_; }
	void set_instances(const TypedArray<CollisionDebugInstance> &p_value) { instances_ = p_value; }
	void add_instance(const Ref<CollisionDebugInstance> &p_instance) { instances_.push_back(p_instance); }
	TypedArray<CollisionProbeBox> get_probe_boxes() const { return probe_boxes_; }
	void set_probe_boxes(const TypedArray<CollisionProbeBox> &p_value) { probe_boxes_ = p_value; }
	void add_probe_box(const Ref<CollisionProbeBox> &p_box) { probe_boxes_.push_back(p_box); }
	Ref<CollisionDebugPlayer> get_player() const { return player_; }
	void set_player(const Ref<CollisionDebugPlayer> &p_value) { player_ = p_value; }
	int64_t get_tick() const { return tick_; }
	void set_tick(int64_t p_value) { tick_ = p_value; }
	int get_hit_stride() const { return kHitStride; }
	int64_t get_hit_ttl() const { return hit_ttl_; }
	void set_hit_ttl(int64_t p_value) { hit_ttl_ = p_value; }
	PackedFloat32Array get_hits() const { return hits_; }
	void set_hits(const PackedFloat32Array &p_value) { hits_ = p_value; }

protected:
	static void _bind_methods();

private:
	TypedArray<CollisionDebugInstance> instances_;
	TypedArray<CollisionProbeBox> probe_boxes_;
	Ref<CollisionDebugPlayer> player_;
	int64_t tick_ = 0;
	int64_t hit_ttl_ = 62;
	PackedFloat32Array hits_;
};

} // namespace godot

#undef COLLISION_DEBUG_ACCESSORS
#undef COLLISION_DEBUG_MEMBER
