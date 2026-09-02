#include "simulation/collision_debug_report.h"

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
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<PackedVector3Array>() { return Variant::PACKED_VECTOR3_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedFloat32Array>() { return Variant::PACKED_FLOAT32_ARRAY; }

} // namespace

#define COLLISION_DEBUG_BIND_FIELD(m_type, m_name, m_default)                                      \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void CollisionDebugVolume::_bind_methods() { COLLISION_DEBUG_VOLUME_FIELDS(COLLISION_DEBUG_BIND_FIELD) }
void CollisionProbeBox::_bind_methods() { COLLISION_PROBE_BOX_FIELDS(COLLISION_DEBUG_BIND_FIELD) }
void CollisionDebugPlayer::_bind_methods() { COLLISION_DEBUG_PLAYER_FIELDS(COLLISION_DEBUG_BIND_FIELD) }

void CollisionDebugInstance::_bind_methods() {
	COLLISION_DEBUG_INSTANCE_FIELDS(COLLISION_DEBUG_BIND_FIELD)
	ClassDB::bind_method(D_METHOD("get_volumes"), &CollisionDebugInstance::get_volumes);
	ClassDB::bind_method(D_METHOD("set_volumes", "value"), &CollisionDebugInstance::set_volumes);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "volumes", PROPERTY_HINT_ARRAY_TYPE, "CollisionDebugVolume"),
			"set_volumes", "get_volumes");
	ClassDB::bind_method(D_METHOD("add_volume", "volume"), &CollisionDebugInstance::add_volume);
}

CollisionDebugReport::CollisionDebugReport() {
	player_.instantiate();
}

void CollisionDebugReport::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_instances"), &CollisionDebugReport::get_instances);
	ClassDB::bind_method(D_METHOD("set_instances", "value"), &CollisionDebugReport::set_instances);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "instances", PROPERTY_HINT_ARRAY_TYPE, "CollisionDebugInstance"),
			"set_instances", "get_instances");
	ClassDB::bind_method(D_METHOD("add_instance", "instance"), &CollisionDebugReport::add_instance);
	ClassDB::bind_method(D_METHOD("get_probe_boxes"), &CollisionDebugReport::get_probe_boxes);
	ClassDB::bind_method(D_METHOD("set_probe_boxes", "value"), &CollisionDebugReport::set_probe_boxes);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "probe_boxes", PROPERTY_HINT_ARRAY_TYPE, "CollisionProbeBox"),
			"set_probe_boxes", "get_probe_boxes");
	ClassDB::bind_method(D_METHOD("add_probe_box", "box"), &CollisionDebugReport::add_probe_box);
	ClassDB::bind_method(D_METHOD("get_player"), &CollisionDebugReport::get_player);
	ClassDB::bind_method(D_METHOD("set_player", "value"), &CollisionDebugReport::set_player);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "player", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT,
						 "CollisionDebugPlayer"),
			"set_player", "get_player");
	ClassDB::bind_method(D_METHOD("get_tick"), &CollisionDebugReport::get_tick);
	ClassDB::bind_method(D_METHOD("set_tick", "value"), &CollisionDebugReport::set_tick);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tick"), "set_tick", "get_tick");
	ClassDB::bind_method(D_METHOD("get_hit_stride"), &CollisionDebugReport::get_hit_stride);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hit_stride", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_hit_stride");
	ClassDB::bind_method(D_METHOD("get_hit_ttl"), &CollisionDebugReport::get_hit_ttl);
	ClassDB::bind_method(D_METHOD("set_hit_ttl", "value"), &CollisionDebugReport::set_hit_ttl);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hit_ttl"), "set_hit_ttl", "get_hit_ttl");
	ClassDB::bind_method(D_METHOD("get_hits"), &CollisionDebugReport::get_hits);
	ClassDB::bind_method(D_METHOD("set_hits", "value"), &CollisionDebugReport::set_hits);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "hits"), "set_hits", "get_hits");
}

#undef COLLISION_DEBUG_BIND_FIELD
