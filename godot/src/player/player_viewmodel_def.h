#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/renderer/fp_viewmodel_spec.h>

namespace godot {

class WeaponDef;

// The weapon.def slice driving the first-person viewmodel (the former
// player_viewmodel_def.gd, ADR 0043 slice G8): the engine's
// renderer::FpViewmodelDef (which carries the field witnesses) decoded from
// the WeaponDef record `WeaponDatabase.get_weapon()` returns, as a record the
// rig, the HUD and the probes read.
#define PLAYER_VIEWMODEL_DEF_FIELDS(X)                                             \
	X(String, weapon_name, String())                                               \
	X(String, gfx1, String())      /* the FP gun model (.3di basename) */          \
	X(String, gfx3, String())      /* the third-person world gun */                \
	X(String, animadm, String())   /* the shared animation set (.adm basename) */  \
	X(Vector3, pos_units, Vector3())     /* hip view bias, raw def units */        \
	X(Vector3, rot_bias_deg, Vector3())  /* def rot columns: yaw/pitch/roll degrees */ \
	X(Vector3, tpos_units, Vector3())    /* ADS view bias */                       \
	X(float, renderfov_h_deg, opennova::renderer::kWeaponRenderFovHDegDefault)     \
	X(int, flags, 0)                                                               \
	X(float, scope_max_mag, 0.0f)                                                  \
	X(int, clipsize, 0)

class PlayerViewmodelDef : public RefCounted {
	GDCLASS(PlayerViewmodelDef, RefCounted)

public:
#define PLAYER_VIEWMODEL_DEF_ACCESSORS(m_type, m_name, m_default) \
	m_type get_##m_name() const { return m_name##_; }              \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	PLAYER_VIEWMODEL_DEF_FIELDS(PLAYER_VIEWMODEL_DEF_ACCESSORS)
#undef PLAYER_VIEWMODEL_DEF_ACCESSORS

	// Decode one WeaponDef; null for no weapon (weapon.def or the weapon name
	// unresolved -- callers keep their built-in fallbacks).
	static Ref<PlayerViewmodelDef> from_weapon_def(const Ref<WeaponDef> &p_def);

protected:
	static void _bind_methods();

private:
#define PLAYER_VIEWMODEL_DEF_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;
	PLAYER_VIEWMODEL_DEF_FIELDS(PLAYER_VIEWMODEL_DEF_MEMBER)
#undef PLAYER_VIEWMODEL_DEF_MEMBER
};

} // namespace godot
