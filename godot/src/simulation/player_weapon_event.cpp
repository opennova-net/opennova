#include "simulation/player_weapon_event.h"

using namespace godot;

void PlayerWeaponEvent::assign(const opennova::world::WeaponPresentationEvent &p_value,
		uint32_t p_now_tick) {
	value_ = p_value;
	// Unsigned subtraction intentionally preserves age across logic-tick wrap.
	age_ticks_ = static_cast<int>(p_now_tick - p_value.tick);
}

Vector3 PlayerWeaponEvent::get_world_position() const {
	// mission (x,y,z) -> Godot (x, z, -y).
	return Vector3(value_.world_position.x, value_.world_position.z, -value_.world_position.y);
}

String PlayerWeaponEvent::get_anim_key() const { return String::utf8(value_.anim_key.c_str()); }
String PlayerWeaponEvent::get_action_soundset() const { return String::utf8(value_.action_soundset.c_str()); }
String PlayerWeaponEvent::get_action_particle() const { return String::utf8(value_.action_particle.c_str()); }
String PlayerWeaponEvent::get_action_particle_userpoint() const {
	return String::utf8(value_.action_particle_userpoint.c_str());
}
String PlayerWeaponEvent::get_action_end_soundset() const {
	return String::utf8(value_.action_end_soundset.c_str());
}
String PlayerWeaponEvent::get_effect_particle() const { return String::utf8(value_.effect_particle.c_str()); }
String PlayerWeaponEvent::get_effect_particle_userpoint() const {
	return String::utf8(value_.effect_particle_userpoint.c_str());
}
String PlayerWeaponEvent::get_switch_to_weapon() const { return String::utf8(value_.switch_to_weapon.c_str()); }

void PlayerWeaponEvent::_bind_methods() {
#define PLAYER_WEAPON_EVENT_FIELD(m_variant, m_name)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerWeaponEvent::get_##m_name);       \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name, PROPERTY_HINT_NONE, "",                   \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),                \
			"", "get_" #m_name);
	PLAYER_WEAPON_EVENT_FIELD(Variant::INT, age_ticks)
	PLAYER_WEAPON_EVENT_FIELD(Variant::VECTOR3, world_position)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, anim_key)
	PLAYER_WEAPON_EVENT_FIELD(Variant::INT, anim_variant)
	PLAYER_WEAPON_EVENT_FIELD(Variant::INT, action_started)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, action_soundset)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, action_particle)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, action_particle_userpoint)
	PLAYER_WEAPON_EVENT_FIELD(Variant::BOOL, scope_settled)
	PLAYER_WEAPON_EVENT_FIELD(Variant::BOOL, third_person)
	PLAYER_WEAPON_EVENT_FIELD(Variant::BOOL, vehicle_attack_context)
	PLAYER_WEAPON_EVENT_FIELD(Variant::INT, action_finished)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, action_end_soundset)
	PLAYER_WEAPON_EVENT_FIELD(Variant::INT, action_effect)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, effect_particle)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, effect_particle_userpoint)
	PLAYER_WEAPON_EVENT_FIELD(Variant::STRING, switch_to_weapon)
	PLAYER_WEAPON_EVENT_FIELD(Variant::BOOL, clear_weapon)
	PLAYER_WEAPON_EVENT_FIELD(Variant::BOOL, preserve_slot_state)
	PLAYER_WEAPON_EVENT_FIELD(Variant::BOOL, switch_denied)
#undef PLAYER_WEAPON_EVENT_FIELD
}
