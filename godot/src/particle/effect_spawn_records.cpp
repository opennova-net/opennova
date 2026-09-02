#include "particle/effect_spawn_records.h"

#include "particle/effect_scene.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<int64_t>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<Transform3D>() { return Variant::TRANSFORM3D; }

} // namespace

#define EFFECT_SPAWN_BIND_FIELD(m_type, m_name, m_default)                                         \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

Ref<EffectSpawnRequest> EffectSpawnRequest::make(int64_t p_effect_handle, const Transform3D &p_transform) {
	Ref<EffectSpawnRequest> out;
	out.instantiate();
	out->effect_handle_ = p_effect_handle;
	out->transform_ = p_transform;
	return out;
}

void EffectSpawnRequest::_bind_methods() {
	EFFECT_SPAWN_REQUEST_FIELDS(EFFECT_SPAWN_BIND_FIELD)
	ClassDB::bind_static_method("EffectSpawnRequest", D_METHOD("make", "effect_handle", "transform"),
			&EffectSpawnRequest::make);
}

Ref<EffectSpawnReceipt> EffectSpawnReceipt::make(bool p_spawned, int64_t p_effect_handle, int64_t p_group_id) {
	Ref<EffectSpawnReceipt> out;
	out.instantiate();
	out->spawned_ = p_spawned;
	out->accepted_ = p_spawned;
	out->status_ = p_spawned ? EffectScene::SPAWN_STATUS_SPAWNED : EffectScene::SPAWN_STATUS_INVALID_HANDLE;
	out->status_name_ = p_spawned ? "spawned" : "invalid_handle";
	out->effect_handle_ = p_effect_handle;
	out->group_id_ = p_group_id;
	return out;
}

void EffectSpawnReceipt::_bind_methods() {
	EFFECT_SPAWN_RECEIPT_FIELDS(EFFECT_SPAWN_BIND_FIELD)
	ClassDB::bind_static_method("EffectSpawnReceipt",
			D_METHOD("make", "spawned", "effect_handle", "group_id"), &EffectSpawnReceipt::make,
			DEFVAL(0), DEFVAL(0));
}

#undef EFFECT_SPAWN_BIND_FIELD
