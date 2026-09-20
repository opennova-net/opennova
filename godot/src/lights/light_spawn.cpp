#include "lights/light_spawn.h"
#include "util/variant_type_of.h"

using namespace godot;

#define LIGHT_SPAWN_BIND_FIELD(m_type, m_name, m_default)                                           \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                       \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);              \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void GlowSpawn::_bind_methods() {
	GLOW_SPAWN_FIELDS(LIGHT_SPAWN_BIND_FIELD)
	ClassDB::bind_static_method("GlowSpawn", D_METHOD("make", "position", "radius", "color"),
			&GlowSpawn::make);
	ClassDB::bind_method(D_METHOD("owned_by", "owner_entity", "owner_section"),
			&GlowSpawn::owned_by, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("fading", "fade_mode", "fade_duration"), &GlowSpawn::fading);
	ClassDB::bind_method(D_METHOD("masking", "disable_corona", "disable_terrain", "disable_objects"),
			&GlowSpawn::masking);
}

Ref<GlowSpawn> GlowSpawn::make(const Vector3 &p_position, float p_radius, const Color &p_color) {
	Ref<GlowSpawn> out;
	out.instantiate();
	out->set_position(p_position);
	out->set_radius(p_radius);
	out->set_color(p_color);
	return out;
}

Ref<GlowSpawn> GlowSpawn::owned_by(int64_t p_owner_entity, int p_owner_section) {
	owner_entity_ = p_owner_entity;
	owner_section_ = p_owner_section;
	return Ref<GlowSpawn>(this);
}

Ref<GlowSpawn> GlowSpawn::fading(int p_fade_mode, int p_fade_duration) {
	fade_mode_ = p_fade_mode;
	fade_duration_ = p_fade_duration;
	return Ref<GlowSpawn>(this);
}

Ref<GlowSpawn> GlowSpawn::masking(bool p_disable_corona, bool p_disable_terrain,
		bool p_disable_objects) {
	disable_corona_ = p_disable_corona;
	disable_terrain_ = p_disable_terrain;
	disable_objects_ = p_disable_objects;
	return Ref<GlowSpawn>(this);
}

void ModelLightSpawn::_bind_methods() {
	MODEL_LIGHT_SPAWN_FIELDS(LIGHT_SPAWN_BIND_FIELD)
	ClassDB::bind_static_method("ModelLightSpawn", D_METHOD("make", "position", "atten_end"),
			&ModelLightSpawn::make);
	ClassDB::bind_method(
			D_METHOD("attached", "attach_bone", "spawning_entity", "spawner_is_building"),
			&ModelLightSpawn::attached, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("in_blink_box", "owner_entity", "section"),
			&ModelLightSpawn::in_blink_box);
	ClassDB::bind_method(D_METHOD("masking", "disable_corona", "disable_terrain", "disable_objects"),
			&ModelLightSpawn::masking);
}

Ref<ModelLightSpawn> ModelLightSpawn::masking(bool p_disable_corona, bool p_disable_terrain,
		bool p_disable_objects) {
	disable_corona_ = p_disable_corona;
	disable_terrain_ = p_disable_terrain;
	disable_objects_ = p_disable_objects;
	return Ref<ModelLightSpawn>(this);
}

Ref<ModelLightSpawn> ModelLightSpawn::make(const Vector3 &p_position, float p_atten_end) {
	Ref<ModelLightSpawn> out;
	out.instantiate();
	out->set_position(p_position);
	out->set_atten_end(p_atten_end);
	return out;
}

Ref<ModelLightSpawn> ModelLightSpawn::attached(int p_attach_bone, int64_t p_spawning_entity,
		bool p_spawner_is_building) {
	attach_bone_ = p_attach_bone;
	spawning_entity_ = p_spawning_entity;
	spawner_is_building_ = p_spawner_is_building;
	return Ref<ModelLightSpawn>(this);
}

Ref<ModelLightSpawn> ModelLightSpawn::in_blink_box(int64_t p_owner_entity, int p_section) {
	blink_owner_entity_ = p_owner_entity;
	blink_section_ = p_section;
	return Ref<ModelLightSpawn>(this);
}

#undef LIGHT_SPAWN_BIND_FIELD
