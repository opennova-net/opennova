#include "particle/effect_spawn_records.h"

#include "particle/effect_scene.h"
#include "util/variant_type_of.h"

using namespace godot;

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

void EffectSpawnOptions::_bind_methods() {
	EFFECT_SPAWN_OPTIONS_FIELDS(EFFECT_SPAWN_BIND_FIELD)
	ClassDB::bind_method(D_METHOD("get_slot_key"), &EffectSpawnOptions::get_slot_key);
	ClassDB::bind_method(D_METHOD("set_slot_key", "value"), &EffectSpawnOptions::set_slot_key);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "slot_key", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT),
			"set_slot_key", "get_slot_key");
	ClassDB::bind_method(D_METHOD("get_owner_key"), &EffectSpawnOptions::get_owner_key);
	ClassDB::bind_method(D_METHOD("set_owner_key", "value"), &EffectSpawnOptions::set_owner_key);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "owner_key", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT),
			"set_owner_key", "get_owner_key");
}

#undef EFFECT_SPAWN_BIND_FIELD
