#pragma once

// The first-person viewmodel submit spec — which gun/arms/clip-adm models the
// shell places for the equipped def. Sim-consumed asset resolution (ADR 0028):
// the RULE is engine policy, the model build/scene lifetime stays shell-side.
// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60 — the def's fpModel trio
// resolves the names; both submits reuse the equipped GUN's model table while
// the adm supplies the clips; emplaced (Flags 0x80) mounts render their own FP
// gun but omit the carried character-arms model @ 0x4dedc7.]

#include <cstdint>
#include <string>

namespace opennova::simassets {

struct FpViewmodelSpec {
	// Empty gun = a RESOLVED def with no fpModel intentionally submits no
	// first-person gun.
	std::string gun;
	std::string arms;
	std::string adm;
	bool show_arms = true;
};

// The witnessed JO default arms model when a def carries no gfx1a.
inline constexpr const char *kDefaultArmsModel = "armsG";
// The no-definition BRING-UP fallback (ours, not retail): before any def
// resolves, the AK set keeps the FP pipeline exercisable.
inline constexpr const char *kBringupFallbackModel = "ak47_1st";
// [orig: the Flags 0x80 emplaced test @ 0x4dedc7]
inline constexpr uint32_t kWeaponFlagEmplaced = 0x80u;

FpViewmodelSpec fp_viewmodel_spec(bool has_def, const std::string &gfx1,
		const std::string &gfx1a, const std::string &animadm, uint32_t flags);

} // namespace opennova::simassets
