#include "simulation/occlusion_portal_report.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<PackedVector3Array>() { return Variant::PACKED_VECTOR3_ARRAY; }

} // namespace

#define OCCLUSION_PORTAL_BIND_FIELD(m_type, m_name, m_default)                                     \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void OcclusionPortalRecord::_bind_methods() { OCCLUSION_PORTAL_RECORD_FIELDS(OCCLUSION_PORTAL_BIND_FIELD) }

void OcclusionPortalBuilding::_bind_methods() {
	OCCLUSION_PORTAL_BUILDING_FIELDS(OCCLUSION_PORTAL_BIND_FIELD)
	ClassDB::bind_method(D_METHOD("get_records"), &OcclusionPortalBuilding::get_records);
	ClassDB::bind_method(D_METHOD("set_records", "value"), &OcclusionPortalBuilding::set_records);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "records", PROPERTY_HINT_ARRAY_TYPE, "OcclusionPortalRecord"),
			"set_records", "get_records");
	ClassDB::bind_method(D_METHOD("add_record", "record"), &OcclusionPortalBuilding::add_record);
}

void OcclusionPortalReport::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_buildings"), &OcclusionPortalReport::get_buildings);
	ClassDB::bind_method(D_METHOD("set_buildings", "value"), &OcclusionPortalReport::set_buildings);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "buildings", PROPERTY_HINT_ARRAY_TYPE, "OcclusionPortalBuilding"),
			"set_buildings", "get_buildings");
	ClassDB::bind_method(D_METHOD("add_building", "building"), &OcclusionPortalReport::add_building);
}

#undef OCCLUSION_PORTAL_BIND_FIELD
