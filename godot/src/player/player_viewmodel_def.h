#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/player_view.h>

namespace godot {

class WeaponDef;

// The weapon.def slice driving the first-person viewmodel (the former
// player_viewmodel_def.gd, ADR 0043 slice G8), decoded from the WeaponDef
// record `WeaponDatabase.get_weapon()` returns. Model names resolve the gun
// and the shared animation set (the first-person ARMS are the local player's
// character arms, never a weapon.def field: retail parses-and-discards
// gfx1a/gfx1b [orig: WeaponDefs_ParseLineCallback @0x5448d0/@0x5448e6 ->
// loc_545098]); `pos_units`/`tpos_units` are the RAW def units (/256 = world;
// the hip and ADS view biases), `rot_bias_deg` the def's yaw/pitch/roll
// degrees added to the view angles, `renderfov_h_deg` the FP projection's
// HORIZONTAL fov (record default 80.0 -- no shipped JO def sets the key).
// [orig: WeaponDef_ParseProperty @0x54d730; pos/tpos handlers
//  @0x54476b/@0x54471f; renderfov @0x54482a, default flt_7D1898 @0x53ff31]
//
// gfx3 is the THIRD-person world gun, drawn in the soldier's hands [orig:
// WeaponDef.tpModel +0x170, read @0x4e3cd3. NOTE the IDB locals in
// WeaponDef_ResolveAllReferences @0x54042c are swapped: `model_1p` there
// reads +0x170, which is THIS field.] tpos_units is the ADS view bias [orig:
// WeaponDef.CamOffsetTpos @0x124]. The record default of renderfov_h_deg is
// the engine base camera fov (world/player_view.h kPlayerCameraFovHDeg,
// Simulation.DEFAULT_PLAYER_FOV_H_DEG) [orig: renderfov default flt_7D1898
// @0x53ff31]. `flags` is the witnessed WeaponDef+8 flag mask (file tokens:
// scoped 1, sighted 2, burst 0x20, auto 0x100) -- Flags & 3 gates the ADS
// toggle [orig: Player_ToggleWeaponScope @0x4df0c0] -- and `scope_max_mag`
// the ADS zoom magnification (scoped camera FOV = 80 / zoom [orig:
// @0x4df401]; 0 = key absent, no zoom change). `clipsize` is the magazine
// size (0 = no clipsize key -> the FSM tracks no clip); the reload key is
// refused on a full magazine [orig: the reload input case @0x4e0420 compares
// the clip against clipsize before WeaponSlot_RequestReload].
#define PLAYER_VIEWMODEL_DEF_FIELDS(X)                                             \
	X(String, weapon_name, String())                                               \
	X(String, gfx1, String())      /* the FP gun model (.3di basename) */          \
	X(String, gfx3, String())      /* the third-person world gun */                \
	X(String, animadm, String())   /* the shared animation set (.adm basename) */  \
	X(Vector3, pos_units, Vector3())     /* hip view bias, raw def units */        \
	X(Vector3, rot_bias_deg, Vector3())  /* def rot columns: yaw/pitch/roll degrees */ \
	X(Vector3, tpos_units, Vector3())    /* ADS view bias */                       \
	X(float, renderfov_h_deg, opennova::world::kPlayerCameraFovHDeg)              \
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
