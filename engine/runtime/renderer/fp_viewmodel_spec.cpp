#include <runtime/renderer/fp_viewmodel_spec.h>

namespace opennova::renderer {

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
	// A resolved def with no fpModel submits no gun. An empty animadm loads NO
	// anim map (AnimMap_LoadAdmFile returns 0 for an empty name) and the FP
	// renderer then submits every bone with the root matrix: no clip set, never
	// the bring-up adm [orig: AnimMap_LoadAdmFile @0x40cca1;
	// Player_RenderFirstPersonViewModel no-channel branch @0x4def88..0x4defcf].
	spec.gun = gfx1;
	spec.adm = animadm;
	// [orig: @ 0x4dedc7 — emplaced mounts omit the carried character arms]
	spec.show_arms = (flags & kWeaponFlagEmplaced) == 0u && !character_arms.empty();
	return spec;
}

} // namespace opennova::renderer
