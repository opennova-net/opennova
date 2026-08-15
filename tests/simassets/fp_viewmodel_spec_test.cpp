// Pins the first-person viewmodel submit spec
// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60; the emplaced arms
// omission @ 0x4dedc7; the CharacterEntity arms source @0x4df05f/@0x4deff4].

#include <simassets/fp_viewmodel_spec.h>

#include <cstdio>

namespace {

using opennova::simassets::fp_viewmodel_spec;
using opennova::simassets::FpViewmodelSpec;
using opennova::simassets::kWeaponFlagEmplaced;

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
	// @0x4df06b; there is no weapon.def arms field to fall back on); missing
	// animadm rides the bring-up adm.
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "spas12_1st", "", "", 0);
		check(s.arms.empty() && !s.show_arms,
				"no resolved character arms submits no arms");
		check(s.adm == "ak47_1st", "missing animadm rides the bring-up adm");
	}
	// A resolved def with no fpModel submits NO gun (empty, not a fallback).
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "", "ArmsG", "", 0);
		check(s.gun.empty(), "resolved def without fpModel submits no gun");
	}
	// Emplaced mounts render their own FP gun but omit the carried arms
	// [orig: @ 0x4dedc7], even when the character has arms.
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "50cal_1st", "ArmsG",
				"", kWeaponFlagEmplaced);
		check(!s.show_arms && s.gun == "50cal_1st",
				"Flags 0x80 keeps the gun and drops the arms");
	}
	// The no-def bring-up path (ours, not retail) still draws the character's
	// arms with the AK set.
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(false, "", "ArmsG", "", 0);
		check(s.gun == "ak47_1st" && s.arms == "ArmsG" && s.adm == "ak47_1st" &&
						s.show_arms,
				"the bring-up path submits the AK set with the character arms");
	}

	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("simassets_fp_viewmodel_spec_test ok");
	return 0;
}
