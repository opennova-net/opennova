// Pins the first-person viewmodel submit spec
// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60; the emplaced arms
// omission @ 0x4dedc7; the CharacterEntity arms source @0x4df05f/@0x4deff4].

#include <runtime/renderer/fp_viewmodel_spec.h>

#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>

#include <cmath>
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

	// Where the viewmodel's root stands before the camera (DI-13: the game's
	// first-person presenter and the editor's eye place it by one function).
	// No cant: the rig's yaw-180 alone, the view offset mapped onto the
	// camera's axes (view x forward -> -z, y left -> -x, z up -> y).
	{
		const float near = 1e-5f;
		const auto close = [&](float a, float b) { return std::fabs(a - b) < near; };
		const float units[3] = {25.188f / 256.0f, -5.494f / 256.0f, -144.952f / 256.0f};
		const float none[3] = {0.0f, 0.0f, 0.0f};
		const float rig[3] = {0.0f, opennova::renderer::kViewmodelRigYawDeg, 0.0f};
		const opennova::renderer::FpViewmodelPose pose = opennova::renderer::fp_viewmodel_pose(units, none, rig);
		const float yaw180[9] = {-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f};
		bool basis = true;
		for (int i = 0; i < 9; ++i) basis = basis && close(pose.basis[i], yaw180[i]);
		check(basis, "no cant: the basis is the rig's yaw-180");
		check(close(pose.origin[0], -units[1]) && close(pose.origin[1], units[2]) && close(pose.origin[2], -units[0]),
				"no cant: the view offset on the camera's axes");
	}
	// A cant (the JOX AK47AUTO's 5 / 3.75 / 353): its YXZ basis (yaw about y,
	// pitch about x, the roll's opposite about z) turns the rig and the offset.
	{
		const float near = 1e-5f;
		const auto close = [&](float a, float b) { return std::fabs(a - b) < near; };
		const float units[3] = {-19.46f / 256.0f, 21.19f / 256.0f, -161.31f / 256.0f};
		const float cant[3] = {5.0f, 3.75f, 353.0f};
		const float none[3] = {0.0f, 0.0f, 0.0f};
		const opennova::renderer::FpViewmodelPose pose = opennova::renderer::fp_viewmodel_pose(units, cant, none);
		const float r = 3.14159265358979323846f / 180.0f;
		const float x = 3.75f * r, y = 5.0f * r, z = 7.0f * r;
		const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z),
		            sz = std::sin(z);
		const float yxz[9] = {cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx,
		                      cx * sz, cx * cz, -sx,
		                      -sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx};
		bool basis = true;
		for (int i = 0; i < 9; ++i) basis = basis && close(pose.basis[i], yxz[i]);
		check(basis, "a cant: Ry * Rx * Rz of the def's yaw, pitch and the roll's opposite");
		const float local[3] = {-units[1], units[2], -units[0]};
		bool origin = true;
		for (int i = 0; i < 3; ++i)
			origin = origin && close(pose.origin[i], yxz[i * 3] * local[0] + yxz[i * 3 + 1] * local[1] + yxz[i * 3 + 2] * local[2]);
		check(origin, "a cant turns the view offset too");
		const float rig[3] = {0.0f, opennova::renderer::kViewmodelRigYawDeg, 0.0f};
		const opennova::renderer::FpViewmodelPose turned = opennova::renderer::fp_viewmodel_pose(units, cant, rig);
		check(close(turned.basis[0], -yxz[0]) && close(turned.basis[2], -yxz[2]) && close(turned.basis[4], yxz[4]),
				"the rig's yaw-180 composes after the cant");
		bool same_origin = true;
		for (int i = 0; i < 3; ++i) same_origin = same_origin && close(turned.origin[i], pose.origin[i]);
		check(same_origin, "the rig's axis map leaves the offset as the cant turns it");
	}

	// Both submits pose by the gun's part table, whatever the clip set is
	// named (M21B_1st over M21_1st's clips) [orig: @0x4df088].
	{
		const FpViewmodelSpec s = fp_viewmodel_spec(true, "M21B_1st", "ArmsG", "M21_1st", 0);
		check(s.rig == "M21B_1st" && s.adm == "M21_1st", "the rig is the gun's, the clips the def's");
		check(fp_viewmodel_spec(false, "M21B_1st", "ArmsG", "M21_1st", 0).rig.empty(),
				"nothing submitted, no rig");
	}

	// The FP pass's depth band: the nearest tenth [orig: @0x58a7b0].
	check(opennova::renderer::kViewmodelPassDepthMax == 0.1f, "the FP depth band ends at 0.1");

	// The frame's root: with a sim, its blended units and its ADS rotation on
	// top of the def's cant; without one, the raw def pos over 256 and the
	// def's cant alone [orig: @0x4dd380; flt_7D1D70 @0x544770].
	{
		const auto close = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
		const float pos[3] = {-19.46f, 21.19f, -161.31f};
		const float cant[3] = {5.0f, 3.75f, 353.0f};
		const float rig[3] = {0.0f, opennova::renderer::kViewmodelRigYawDeg, 0.0f};
		const float scaled[3] = {pos[0] / 256.0f, pos[1] / 256.0f, pos[2] / 256.0f};
		const opennova::renderer::FpViewmodelPose bare =
				opennova::renderer::fp_viewmodel_root(pos, cant, rig, nullptr, nullptr);
		const opennova::renderer::FpViewmodelPose want_bare =
				opennova::renderer::fp_viewmodel_pose(scaled, cant, rig);
		bool same = true;
		for (int i = 0; i < 9; ++i) same = same && close(bare.basis[i], want_bare.basis[i]);
		for (int i = 0; i < 3; ++i) same = same && close(bare.origin[i], want_bare.origin[i]);
		check(same, "no sim: the def pos over 256 under the def's cant");
		const float units[3] = {-0.2f, 0.1f, -0.6f};
		const float ads[3] = {1.0f, -2.0f, 0.5f};
		const float summed[3] = {cant[0] + ads[0], cant[1] + ads[1], cant[2] + ads[2]};
		const opennova::renderer::FpViewmodelPose live =
				opennova::renderer::fp_viewmodel_root(pos, cant, rig, units, ads);
		const opennova::renderer::FpViewmodelPose want_live =
				opennova::renderer::fp_viewmodel_pose(units, summed, rig);
		same = true;
		for (int i = 0; i < 9; ++i) same = same && close(live.basis[i], want_live.basis[i]);
		for (int i = 0; i < 3; ++i) same = same && close(live.origin[i], want_live.origin[i]);
		check(same, "a sim: its units under the def's cant plus its ADS rotation");
	}

	// The def slice: the names, the raw pos/tpos units, the Q16 rotation
	// columns as degrees, the renderfov, the flags, the zoom, the clip.
	{
		opennova::def::DefWeaponDef def = {};
		std::snprintf(def.weapon_name, sizeof(def.weapon_name), "WPN_AK47AUTO");
		std::snprintf(def.gfx1, sizeof(def.gfx1), "AKM_1st");
		std::snprintf(def.gfx3, sizeof(def.gfx3), "AKM_3rd");
		std::snprintf(def.animadm, sizeof(def.animadm), "AKM_1st");
		def.pos[0] = -19.46f;
		def.tpos[2] = -152.56f;
		def.pos_rotation_deg_q16[0] = 5 * 65536;
		def.pos_rotation_deg_q16[1] = 245760; // 3.75
		def.renderfov = 80.0f;
		def.flags = 0x102;
		def.scope_max_mag = 2.0f;
		def.clipsize = 30;
		const opennova::renderer::FpViewmodelDef d = opennova::renderer::fp_viewmodel_def(def);
		check(d.weapon_name == "WPN_AK47AUTO" && d.gfx1 == "AKM_1st" && d.gfx3 == "AKM_3rd" &&
						d.animadm == "AKM_1st",
				"the def slice carries the names");
		check(d.pos_units[0] == -19.46f && d.tpos_units[2] == -152.56f, "the raw view-bias units");
		check(d.rot_bias_deg[0] == 5.0f && d.rot_bias_deg[1] == 3.75f, "the rotation columns in degrees");
		check(d.renderfov_h_deg == 80.0f && d.flags == 0x102 && d.scope_max_mag == 2.0f && d.clipsize == 30,
				"the renderfov, flags, zoom and clip");
	}

	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("renderer_fp_viewmodel_spec_test ok");
	return 0;
}
