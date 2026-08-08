// Pins the first-person viewmodel submit spec
// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60; the emplaced arms
// omission @ 0x4dedc7].

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

	// A fully authored def resolves verbatim.
	{
		const FpViewmodelSpec s =
				fp_viewmodel_spec(true, "m4_1st", "armsD", "m4_1st", 0);
		check(s.gun == "m4_1st" && s.arms == "armsD" && s.adm == "m4_1st" &&
						s.show_arms,
				"authored fpModel trio resolves verbatim");
	}
	// Missing gfx1a takes the witnessed JO default arms; missing animadm rides
	// the bring-up adm.
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "spas12_1st", "", "", 0);
		check(s.arms == "armsG", "missing gfx1a takes the armsG default");
		check(s.adm == "ak47_1st", "missing animadm rides the bring-up adm");
	}
	// A resolved def with no fpModel submits NO gun (empty, not a fallback).
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "", "", "", 0);
		check(s.gun.empty(), "resolved def without fpModel submits no gun");
	}
	// Emplaced mounts render their own FP gun but omit the carried arms
	// [orig: @ 0x4dedc7].
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "50cal_1st", "", "",
				kWeaponFlagEmplaced);
		check(!s.show_arms && s.gun == "50cal_1st",
				"Flags 0x80 keeps the gun and drops the arms");
	}
	// The no-def bring-up path (ours, not retail).
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(false, "", "", "", 0);
		check(s.gun == "ak47_1st" && s.arms == "armsG" && s.adm == "ak47_1st" &&
						s.show_arms,
				"the bring-up path submits the AK set");
	}

	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("simassets_fp_viewmodel_spec_test ok");
	return 0;
}
