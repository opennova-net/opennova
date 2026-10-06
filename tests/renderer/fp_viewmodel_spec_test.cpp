// Pins the first-person viewmodel submit spec
// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60; the emplaced arms
// omission @ 0x4dedc7; the CharacterEntity arms source @0x4df05f/@0x4deff4].

#include <runtime/renderer/fp_viewmodel_spec.h>

#include <formats/threedi/threedi_3di3.h>

#include <cstdio>

namespace {

using opennova::renderer::fp_viewmodel_spec;
using opennova::renderer::FpViewmodelSpec;
using opennova::renderer::kWeaponFlagEmplaced;

bool expect(bool ok, const char *message) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", message);
	}
	return ok;
}

} // namespace

int main() {
	int failures = 0;
	auto check = [&](bool ok, const char *message) {
		if (!expect(ok, message)) ++failures;
	};

	// A fully authored def + a resolved character resolves verbatim: the gun
	// is the def's fpModel, the arms are the character's combo arms graphic.
	{
		const FpViewmodelSpec s =
				fp_viewmodel_spec(true, "m4_1st", "ArmsG", "m4_1st", 0);
		check(s.gun == "m4_1st" && s.arms == "ArmsG" && s.adm == "m4_1st" &&
						s.show_arms,
				"fpModel + character arms + animadm resolve verbatim");
	}
	// No character arms -> no arms submit (retail draws nothing @0x4df064 /
	// @0x4df06b; there is no weapon.def arms field to fall back on). A resolved
	// def's empty animadm loads NO anim map (AnimMap_LoadAdmFile @0x40cca1):
	// no clip set, never the bring-up adm (the root-matrix pose
	// @0x4def88..0x4defcf is the renderer's own).
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "spas12_1st", "", "", 0);
		check(s.arms.empty() && !s.show_arms,
				"no resolved character arms submits no arms");
		check(s.adm.empty(), "a resolved def's empty animadm sets no clip");
	}
	// A resolved def with no fpModel submits NOTHING: no gun and no arms
	// (def+0x16C null returns before both submits @0x4dedc7).
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "", "ArmsG", "m4_1st", 0);
		check(s.gun.empty(), "resolved def without fpModel submits no gun");
		check(!s.show_arms && s.arms.empty() && s.adm.empty(),
				"resolved def without fpModel submits no arms either");
	}
	// Emplaced mounts render their own FP gun but omit the carried arms
	// [orig: @ 0x4dedc7], even when the character has arms.
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "50cal_1st", "ArmsG",
				"", kWeaponFlagEmplaced);
		check(!s.show_arms && s.gun == "50cal_1st",
				"Flags 0x80 keeps the gun and drops the arms");
	}
	// No equipped def submits nothing and names no model: no default gun, no
	// arms, no clip set (no equipped slot @0x4dedb6 / no def @0x4dedc1).
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(false, "", "ArmsG", "", 0);
		check(s.gun.empty() && s.arms.empty() && s.adm.empty() && !s.show_arms,
				"no equipped def submits nothing");
		const FpViewmodelSpec stale = fp_viewmodel_spec(false, "m4_1st", "ArmsG", "m4_1st", 0);
		check(stale.gun.empty() && !stale.show_arms,
				"no equipped def ignores any model name");
	}

	// How far the arms reach into the gun's bone array: one past the highest
	// part a strip draws with, a skinned strip through its bone table, a rigid
	// one through its own part; a part with no strip reaches nothing
	// [orig: the arms submit with the gun's array @0x4DF088].
	{
		opennova::threedi::ThreediRenderObject parts[4] = {};
		opennova::threedi::ThreediTriangleStrip strips[3] = {};
		parts[0].num_strips = 1; // rigid, part 0
		parts[1].num_strips = 1; // skinned over parts 2 and 5
		parts[2].num_alpha_strips = 1;
		strips[0].num_vertices = 3;
		strips[1].num_vertices = 3;
		strips[1].bone_table[0] = 2;
		strips[1].bone_table[1] = 5;
		strips[1].bone_table_length = 2;
		strips[2].num_vertices = 3; // rigid alpha strip, part 2
		opennova::threedi::ThreediLod lod = {};
		lod.render_objects = parts;
		lod.render_object_count = 4; // part 3: a meshless helper
		lod.strips = strips;
		lod.strip_count = 3;
		opennova::threedi::Threedi3di3 arms = {};
		arms.lods = &lod;
		arms.lod_count = 1;
		check(opennova::renderer::fp_arms_part_reach(arms) == 6,
				"the skinned strip's bone table sets the reach");
		strips[1].bone_table_length = 1;
		check(opennova::renderer::fp_arms_part_reach(arms) == 3,
				"a rigid strip reaches its own part; a meshless helper nothing");
		arms.lod_count = 0;
		check(opennova::renderer::fp_arms_part_reach(arms) == 0, "no LOD reaches nothing");
	}

	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("renderer_fp_viewmodel_spec_test ok");
	return 0;
}
