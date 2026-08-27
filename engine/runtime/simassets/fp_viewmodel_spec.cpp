#include <runtime/simassets/fp_viewmodel_spec.h>

namespace opennova::simassets {

FpViewmodelSpec fp_viewmodel_spec(bool has_def, const std::string &gfx1,
		const std::string &character_arms, const std::string &animadm,
		uint32_t flags) {
	FpViewmodelSpec spec;
	// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60] The arms are the
	// character's own arms model, never a weapon.def field; none -> no arms
	// submit (@0x4df064/@0x4df06b).
	spec.arms = character_arms;
	if (!has_def) {
		// The bring-up path: no resolved def yet — the AK set (ours, not
		// retail) keeps the FP pipeline exercisable.
		spec.gun = kBringupFallbackModel;
		spec.adm = kBringupFallbackModel;
		spec.show_arms = !character_arms.empty();
		return spec;
	}
	// A resolved def with no fpModel submits no gun; missing animadm rides the
	// bring-up adm so the clip rings stay sized.
	spec.gun = gfx1;
	spec.adm = animadm.empty() ? kBringupFallbackModel : animadm;
	// [orig: @ 0x4dedc7 — emplaced mounts omit the carried character arms]
	spec.show_arms = (flags & kWeaponFlagEmplaced) == 0u && !character_arms.empty();
	return spec;
}

} // namespace opennova::simassets
