#include <runtime/renderer/fp_viewmodel_spec.h>

#include <formats/threedi/threedi_3di3.h>

#include <algorithm>

namespace opennova::renderer {

FpViewmodelSpec fp_viewmodel_spec(bool has_def, const std::string &gfx1,
		const std::string &character_arms, const std::string &animadm,
		uint32_t flags) {
	FpViewmodelSpec spec;
	// [orig: Player_RenderFirstPersonViewModel @ 0x4ded60] No equipped slot
	// (@0x4dedb6), no def (@0x4dedc1) or no fpModel (def+0x16C, @0x4dedc7): the
	// function returns before both submits, so neither gun nor arms draw.
	if (!has_def || gfx1.empty()) return spec;
	// The arms are the character's own arms model, never a weapon.def field;
	// none -> no arms submit (@0x4df064/@0x4df06b).
	spec.arms = character_arms;
	// An empty animadm loads NO anim map (AnimMap_LoadAdmFile returns 0 for an
	// empty name) and the FP renderer then submits every bone with the root
	// matrix: no clip set [orig: AnimMap_LoadAdmFile @0x40cca1;
	// Player_RenderFirstPersonViewModel no-channel branch @0x4def88..0x4defcf].
	spec.gun = gfx1;
	spec.adm = animadm;
	// [orig: @ 0x4df057/@ 0x4defe3 — emplaced mounts omit the carried
	// character arms]
	spec.show_arms = (flags & kWeaponFlagEmplaced) == 0u && !character_arms.empty();
	return spec;
}

namespace {

// A 3x3 of rows.
struct Rows3 {
	float m[3][3];
};

Rows3 multiply(const Rows3 &a, const Rows3 &b) {
	Rows3 out{};
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
	return out;
}

// The presentation's euler basis, YXZ: Ry * Rx * Rz, radians.
Rows3 euler_yxz(float x, float y, float z) {
	const float cx = std::cos(x), sx = std::sin(x);
	const float cy = std::cos(y), sy = std::sin(y);
	const float cz = std::cos(z), sz = std::sin(z);
	const Rows3 rx{{{1.0f, 0.0f, 0.0f}, {0.0f, cx, -sx}, {0.0f, sx, cx}}};
	const Rows3 ry{{{cy, 0.0f, sy}, {0.0f, 1.0f, 0.0f}, {-sy, 0.0f, cy}}};
	const Rows3 rz{{{cz, -sz, 0.0f}, {sz, cz, 0.0f}, {0.0f, 0.0f, 1.0f}}};
	return multiply(multiply(ry, rx), rz);
}

} // namespace

FpViewmodelPose fp_viewmodel_pose(const float view_units[3], const float rot_bias_deg[3],
		const float rig_rot_deg[3]) {
	constexpr float kRad = 3.14159265358979323846f / 180.0f;
	float bias_rad[3];
	viewmodel_bias_euler_rad(rot_bias_deg, bias_rad);
	const Rows3 bias = euler_yxz(bias_rad[0], bias_rad[1], bias_rad[2]);
	const Rows3 rig = euler_yxz(rig_rot_deg[0] * kRad, rig_rot_deg[1] * kRad, rig_rot_deg[2] * kRad);
	const Rows3 basis = multiply(bias, rig);
	float offset[3];
	viewmodel_camera_local_from_view(view_units, offset);
	FpViewmodelPose pose{};
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) pose.basis[i * 3 + j] = basis.m[i][j];
		pose.origin[i] = bias.m[i][0] * offset[0] + bias.m[i][1] * offset[1] + bias.m[i][2] * offset[2];
	}
	return pose;
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
