#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

// The building occlusion portal payload for the F3 occlusion view
// (Simulation::get_occlusion_portal_debug): one row per nearby building with
// its OFAC records posed into Godot space, each carrying the boundary outline
// (segment pairs) the occluder pass cancels interior edges out of. Read-write
// so the view test authors a payload.

#define OCCLUSION_PORTAL_ACCESSORS(m_type, m_name, m_default) \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define OCCLUSION_PORTAL_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// `type` is the record kind (Simulation::OCC_REC_*); the record normal points
// section_a -> section_b, section 0 = exterior.
#define OCCLUSION_PORTAL_RECORD_FIELDS(X)                    \
	X(int, type, 0)                                          \
	X(int, section_a, 0)                                     \
	X(int, section_b, 0)                                     \
	X(Vector3, pos, Vector3())                               \
	X(float, radius, 0.0f)                                   \
	X(float, glow, 0.0f)                                     \
	X(PackedVector3Array, segments, PackedVector3Array())

class OcclusionPortalRecord : public RefCounted {
	GDCLASS(OcclusionPortalRecord, RefCounted)

public:
	OCCLUSION_PORTAL_RECORD_FIELDS(OCCLUSION_PORTAL_ACCESSORS)

protected:
	static void _bind_methods();

private:
	OCCLUSION_PORTAL_RECORD_FIELDS(OCCLUSION_PORTAL_MEMBER)
};

#define OCCLUSION_PORTAL_BUILDING_FIELDS(X) \
	X(int, bms_id, 0)                       \
	X(Vector3, pos, Vector3())              \
	X(bool, visible, true)

class OcclusionPortalBuilding : public RefCounted {
	GDCLASS(OcclusionPortalBuilding, RefCounted)

public:
	OCCLUSION_PORTAL_BUILDING_FIELDS(OCCLUSION_PORTAL_ACCESSORS)
	TypedArray<OcclusionPortalRecord> get_records() const { return records_; }
	void set_records(const TypedArray<OcclusionPortalRecord> &p_value) { records_ = p_value; }
	void add_record(const Ref<OcclusionPortalRecord> &p_record) { records_.push_back(p_record); }

protected:
	static void _bind_methods();

private:
	OCCLUSION_PORTAL_BUILDING_FIELDS(OCCLUSION_PORTAL_MEMBER)
	TypedArray<OcclusionPortalRecord> records_;
};

class OcclusionPortalReport : public RefCounted {
	GDCLASS(OcclusionPortalReport, RefCounted)

public:
	TypedArray<OcclusionPortalBuilding> get_buildings() const { return buildings_; }
	void set_buildings(const TypedArray<OcclusionPortalBuilding> &p_value) { buildings_ = p_value; }
	void add_building(const Ref<OcclusionPortalBuilding> &p_building) { buildings_.push_back(p_building); }

protected:
	static void _bind_methods();

private:
	TypedArray<OcclusionPortalBuilding> buildings_;
};

} // namespace godot

#undef OCCLUSION_PORTAL_ACCESSORS
#undef OCCLUSION_PORTAL_MEMBER
