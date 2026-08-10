#include "simassets/fp_viewmodel_spec.h"

namespace opennova::simassets {

FpViewmodelSpec fp_viewmodel_spec(bool has_def, const std::string &gfx1,
		const std::string &gfx1a, const std::string &animadm, uint32_t flags) {
	FpViewmodelSpec spec;
	if (!has_def) {
		// The bring-up path: no resolved def yet — the AK set (ours, not
		// retail) keeps the FP pipeline exercisable.
		spec.gun = kBringupFallbackModel;
		spec.arms = kDefaultArmsModel;
		spec.adm = kBringupFallbackModel;
		spec.show_arms = true;
		return spec;
	}
	// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60] A resolved def with
	// no fpModel submits no gun; missing gfx1a takes the JO default arms;
	// missing animadm rides the bring-up adm so the clip rings stay sized.
	spec.gun = gfx1;
	spec.arms = gfx1a.empty() ? kDefaultArmsModel : gfx1a;
	spec.adm = animadm.empty() ? kBringupFallbackModel : animadm;
	// [orig: @ 0x4dedc7 — emplaced mounts omit the carried character arms]
	spec.show_arms = (flags & kWeaponFlagEmplaced) == 0u;
	return spec;
}

} // namespace opennova::simassets
