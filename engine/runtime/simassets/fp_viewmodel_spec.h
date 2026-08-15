#pragma once

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

namespace opennova::simassets {

struct FpViewmodelSpec {
	// Empty gun = a RESOLVED def with no fpModel intentionally submits no
	// first-person gun.
	std::string gun;
	// The selected character's arms graphic; empty = no arms submit.
	std::string arms;
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

// The no-definition BRING-UP fallback (ours, not retail): before any def
// resolves, the AK set keeps the FP pipeline exercisable.
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

} // namespace opennova::simassets
