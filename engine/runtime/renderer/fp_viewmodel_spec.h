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
	// Empty gun = a RESOLVED def with no fpModel intentionally submits no
	// first-person gun.
	std::string gun;
	// The selected character's arms graphic; empty = no arms submit.
	std::string arms;
	// The clip set. Empty for a RESOLVED def with no animadm (retail loads no
	// anim map and draws the rig at the root matrix); the bring-up adm rides
	// only the no-def path.
	std::string adm;
	// False when the mount is emplaced (Flags 0x80) or no character arms
	// resolved — retail submits no arms in either case.
	bool show_arms = true;
};

// weapon.def viewmodel placement units: the parser stores pos/tpos POSITIONS
// as atof(str) * 256 (16.16 fixed-point world; scale flt_7D1D70 @ 0x544770,
// handler @ 0x54471f) and the camera ftol's the stored float straight onto
// g_view_pos, so the net WORLD offset is file_value / 256.
inline constexpr float kWeaponDefPosScale = 256.0f;
// The witnessed JOX WPN_AK47AUTO def line — the no-def bring-up fallback
// placement trio (pos hip / tpos ADS / rot-bias cant degrees).
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

// TEX_TEAM is a signed-byte store immediately before the FP lighting, heat
// and model-submit path [orig: Player_RenderFirstPersonViewModel
//  @0x4DEE96..0x4DEE9F].
inline int viewmodel_team_byte(int team) {
    int t = team & 0xFF;
    if (t >= 0x80) t -= 0x100;
    return t;
}

// The no-definition BRING-UP fallback (ours, not retail): before any def
// resolves, the AK set keeps the FP pipeline exercisable. A resolved def never
// rides it: its empty animadm is no clip set, as retail's empty-name
// AnimMap_LoadAdmFile return leaves no anim map.
inline constexpr const char *kBringupFallbackModel = "ak47_1st";
// The matching weapon.def entry name the shell resolves until first equip.
inline constexpr const char *kBringupFallbackWeapon = "WPN_AK47AUTO";
// [orig: the Flags 0x80 emplaced test @ 0x4dedc7]
inline constexpr uint32_t kWeaponFlagEmplaced = 0x80u;

// `character_arms` is the local player's resolved combo arms graphic (empty
// when the character carries no arms part).
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
