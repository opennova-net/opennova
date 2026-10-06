#pragma once

#include <cmath>

// The first-person viewmodel submit spec — which gun/arms/clip-adm models the
// shell places for the equipped def and the local player's selected character.
// Sim-consumed asset resolution (ADR 0028): the RULE is engine policy, the
// model build/scene lifetime stays shell-side.
// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60 — the def's fpModel
// (gfx1) is the gun; the ARMS are the local player's CharacterEntity arms
// model (blip+8 = the Avatars.def combo arms graphic, read @0x4df05f /
// @0x4deff4 and drawn with the gun's bone matrices @0x4df088); emplaced
// (Flags 0x80) mounts render their own FP gun but omit the arms
// @0x4df057/@0x4defe3; a player without a resolved character arms model
// submits no arms @0x4df064/@0x4df06b. weapon.def `gfx1a`/`gfx1b` are
// parsed-and-DISCARDED tokens (WeaponDefs_ParseLineCallback @0x5448d0 /
// @0x5448e6 -> loc_545098 = return 0) — never an arms source.]

#include <cstdint>
#include <string>

namespace opennova::threedi {
struct Threedi3di3;
}

namespace opennova::renderer {

struct FpViewmodelSpec {
	// The equipped def's fpModel (gfx1). Empty = nothing is submitted: with no
	// equipped def, or a def with no fpModel, neither gun nor arms draw.
	std::string gun;
	// The selected character's arms graphic; empty = no arms submit.
	std::string arms;
	// The clip set. Empty for a def with no animadm (retail loads no anim map
	// and draws the rig at the root matrix).
	std::string adm;
	// False when nothing is submitted, the mount is emplaced (Flags 0x80) or
	// no character arms resolved: retail submits no arms in any of these.
	bool show_arms = false;
};

// weapon.def viewmodel placement units: the parser stores pos/tpos POSITIONS
// as atof(str) * 256 (16.16 fixed-point world; scale flt_7D1D70 @ 0x544770,
// handler @ 0x54471f) and the camera ftol's the stored float straight onto
// g_view_pos, so the net WORLD offset is file_value / 256.
inline constexpr float kWeaponDefPosScale = 256.0f;
// The witnessed JOX WPN_AK47AUTO def line's placement trio (pos hip / tpos
// ADS / rot-bias cant degrees): the viewmodel rig's initial tunables until an
// equipped def's own rows replace them (nothing draws without one).
inline constexpr float kFallbackPosUnits[3] = {-19.46f, 21.19f, -161.31f};
inline constexpr float kFallbackTposUnits[3] = {-62.33f, 29.19f, -152.56f};
inline constexpr float kFallbackRotBiasDeg[3] = {5.0f, 3.75f, 353.0f};
// The FP render pass swaps the projection near plane 0.2 -> 0.05 while the
// viewmodel draws [orig: Render_SwapProjectionNearZ(0.05) @ 0x4dee29,
// restore @ 0x4df0aa].
inline constexpr float kViewmodelPassNearZ = 0.05f;

// The weapon `renderfov` default, HORIZONTAL degrees: every JO weapon.def
// omits the key, so every viewmodel draws through the record default.
// [orig: flt_7D1898 stored by AdmDef_InitEntryDefaults @0x53ff31; parser
//  key 'renderfov' @0x54482a; fov = WeaponDef+0x148 @0x4dee71]
inline constexpr float kWeaponRenderFovHDegDefault = 80.0f;

// The rig's authored forward onto the presentation camera's -z forward: a
// yaw of 180 degrees about the up axis — the structural equivalent of the
// original drawing its composed render-frame bone matrices with the raw
// view matrix [orig: Player_RenderFirstPersonViewModel @0x4ded60 root = the
//  view transform; the S*A^T*S copy loops @0x40c4d8..0x40c57c].
inline constexpr float kViewmodelRigYawDeg = 180.0f;

// Fold degrees into (-180, 180] (def rot columns store e.g. 353 for -7).
inline float viewmodel_wrap180_deg(float degrees) {
    float out = std::fmod(degrees + 180.0f, 360.0f);
    if (out < 0.0f) out += 360.0f;
    return out - 180.0f;
}

// The weapon.def `pos` rotation columns (degrees: yaw, pitch, roll — the
// cant ADDED to the view angles) as presentation-camera euler radians
// (x, y, z): their pitch -> x, their yaw -> y, their roll -> z with the
// opposite sense. [orig: Player_UpdateFirstPersonCamera @0x4dd444: rot =
//  view_rot + Def.Bone.rot; the parser stores degrees -> BAM @0x54471f]
inline void viewmodel_bias_euler_rad(const float rot_bias_deg[3], float out_xyz_rad[3]) {
    constexpr float kRad = 3.14159265358979323846f / 180.0f;
    out_xyz_rad[0] = viewmodel_wrap180_deg(rot_bias_deg[1]) * kRad;
    out_xyz_rad[1] = viewmodel_wrap180_deg(rot_bias_deg[0]) * kRad;
    out_xyz_rad[2] = -viewmodel_wrap180_deg(rot_bias_deg[2]) * kRad;
}

// A VIEW-FRAME offset (X = forward, Y = left, Z = up — proven by the aim
// ray's far point being {+65536000, 0, 0} through the same transform) onto
// presentation camera-local axes (x right, y up, -z forward):
//   view x (forward) -> -z, view y (left) -> -x, view z (up) -> y.
// [orig: HUD_DrawCrosshair @0x592a0f aim_direction = (1000.0, 0, 0) q16; the
//  view-local rotate Math_FixedPointTransformPoint22 @0x4dd5d8]
inline void viewmodel_camera_local_from_view(const float view[3], float out[3]) {
    out[0] = -view[1];
    out[1] = view[2];
    out[2] = -view[0];
}

// Where the viewmodel's root stands in the camera's frame (camera-local axes:
// x right, y up, -z forward), the gun and the arms drawn at it: a 3x3 basis
// (row-major, its rows the presentation's) and an origin.
struct FpViewmodelPose {
    float basis[9];
    float origin[3];
};

// The viewmodel root the first-person view draws the gun and the arms at,
// relative to the camera: the def's cant (`rot_bias_deg`, yaw / pitch / roll
// degrees, through viewmodel_bias_euler_rad) composed over the rig's axis map
// (`rig_rot_deg`, euler degrees in camera space: the yaw-180 of
// kViewmodelRigYawDeg), and the view offset (`view_units`, VIEW-FRAME world
// units: the def `pos` over kWeaponDefPosScale, or the sim's blended bias)
// mapped onto the camera's axes and turned by the cant. Each euler is the
// presentation's YXZ order (Ry * Rx * Rz). The camera adds the def's
// rotation to the view angles and turns its position into the view before
// adding it to the eye; the model is drawn at that view root [orig:
// Player_UpdateFirstPersonCamera @0x4dd380, rot = view_rot + Def.Bone.rot
// @0x4dd444, the view-local rotate Math_FixedPointTransformPoint22
// @0x4dd5d8; Player_RenderFirstPersonViewModel @0x4ded60 root = the view
// transform]. The game's first-person presenter and the editor's
// first-person eye (DI-13) place the viewmodel by it.
FpViewmodelPose fp_viewmodel_pose(const float view_units[3], const float rot_bias_deg[3],
		const float rig_rot_deg[3]);

// TEX_TEAM is a signed-byte store immediately before the FP lighting, heat
// and model-submit path [orig: Player_RenderFirstPersonViewModel
//  @0x4DEE96..0x4DEE9F].
inline int viewmodel_team_byte(int team) {
    int t = team & 0xFF;
    if (t >= 0x80) t -= 0x100;
    return t;
}

// [orig: the def Flags 0x80 emplaced test @ 0x4deddf]
inline constexpr uint32_t kWeaponFlagEmplaced = 0x80u;

// `has_def` is whether the local player has an equipped def (its mounted
// slot's), `gfx1` that def's fpModel, `character_arms` the local player's
// resolved combo arms graphic (empty when the character carries no arms
// part). Without an equipped def, or with a def whose fpModel is empty, the
// spec submits nothing: there is no default model [orig:
// Player_RenderFirstPersonViewModel @ 0x4ded60 returns with no equipped slot
// @ 0x4dedb6, no def @ 0x4dedc1, no fpModel @ 0x4dedc7 (def+0x16C, which the
// gfx1 arm stores only for a model that loaded, WeaponDefs_ParseLineCallback
// @ 0x54506c; the load is at the def load @ 0x544fce, and a model that fails
// is reported there once, "load failed" @ 0x544fdc, then never drawn)].
FpViewmodelSpec fp_viewmodel_spec(bool has_def, const std::string &gfx1,
		const std::string &character_arms, const std::string &animadm,
		uint32_t flags);

// How far into the gun's bone array an arms model reaches: one past the
// highest part its LOD0 strips draw with (a skinned strip's bone table, a
// rigid strip's own part); 0 for a model with no strip. The game draws the
// arms with the GUN's array, arms part i by the gun's part i, so a reach past
// the gun's parts leaves those arms parts without a posed matrix (the playing
// clip's raw sample, or stale stack). Stock arms stay within their guns: an
// arms model's meshless helper rows draw with nothing.
// [orig: Player_RenderFirstPersonViewModel @0x4DED60, the arms submit with
//  the gun's array @0x4DF088]
int fp_arms_part_reach(const threedi::Threedi3di3 &arms);

} // namespace opennova::renderer
