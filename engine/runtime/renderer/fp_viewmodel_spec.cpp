#include <runtime/renderer/fp_viewmodel_spec.h>

#include <formats/threedi/threedi_3di3.h>

#include <algorithm>

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

int fp_arms_part_reach(const threedi::Threedi3di3 &arms) {
	if (arms.lods == nullptr || arms.lod_count == 0) return 0;
	const threedi::ThreediLod &lod = arms.lods[0];
	if (lod.render_objects == nullptr || lod.strips == nullptr) return 0;
	// Strips run in part order, each part's opaque strips then its alpha ones.
	int reach = 0;
	size_t cursor = 0;
	for (size_t part = 0; part < lod.render_object_count; ++part) {
		const threedi::ThreediRenderObject &robj = lod.render_objects[part];
		const int32_t count = robj.num_strips + robj.num_alpha_strips;
		for (int32_t s = 0; s < count && cursor < lod.strip_count; ++s, ++cursor) {
			const threedi::ThreediTriangleStrip &strip = lod.strips[cursor];
			if (strip.num_vertices <= 0) continue;
			if (strip.bone_table_length <= 0) {
				reach = std::max(reach, static_cast<int>(part) + 1);
				continue;
			}
			const int32_t bones = std::min<int32_t>(strip.bone_table_length, threedi::kThreediStripBoneTableMax);
			for (int32_t b = 0; b < bones; ++b)
				reach = std::max(reach, static_cast<int>(strip.bone_table[b]) + 1);
		}
	}
	return reach;
}

} // namespace opennova::renderer
