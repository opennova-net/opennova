#include "renderer/light_runtime.h"

#include <algorithm>
#include <cmath>

namespace renderer {

namespace {

constexpr float kInv255 = 1.0f / 255.0f;

// Unpack a 0x00RRGGBB engine color to linear floats (byte / 255)
// [orig: the 0.0039215689 unpack constant used at every block read,
//  e.g. CTerrainRenderer_BuildLightingShaderConstants @ 0x5c80d5..].
std::array<float, 3> unpack_color_255(uint32_t packed) {
	return {
		float((packed >> 16) & 0xFF) * kInv255,
		float((packed >> 8) & 0xFF) * kInv255,
		float(packed & 0xFF) * kInv255,
	};
}

std::array<float, 3> lerp3(const std::array<float, 3> &a, const std::array<float, 3> &b, float t) {
	return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
}

}  // namespace

std::array<float, 4> build_oed_light_attenuation(float light_range) {
    // Original OED reference: PrepareLightParams @ 0x46A500 emits
    // {1, 0, 15 / range^2, 1} after scaling the runtime range by 1.25.
    // The retail runtime computes the identical set from the light record's
    // 16.16 range [orig: Light_GetPointLightParams @ 0x5a9251..0x5a9272].
    const float range = std::max(light_range * 1.25f, 0.001f);
    return {1.0f, 0.0f, 15.0f / (range * range), 1.0f};
}

std::array<float, 4> build_depth_mask_plane(float pos_x, float pos_y, float pos_z,
                                             float dir_x, float dir_y, float dir_z,
                                             float atten_start, float atten_end) {
    const float len_sq = dir_x * dir_x + dir_y * dir_y + dir_z * dir_z;
    if (len_sq < 0.000001f)
        return {0.0f, 0.0f, 0.0f, 0.0f};

    const float inv_len = 1.0f / std::sqrt(len_sq);
    const float nx = dir_x * inv_len;
    const float ny = dir_y * inv_len;
    const float nz = dir_z * inv_len;
    const float range = std::max(atten_end - atten_start, 0.001f);
    const float distance = -(nx * pos_x + ny * pos_y + nz * pos_z) - atten_start;
    return {nx / range, ny / range, nz / range, distance / range};
}

std::array<float, 16> build_shadow_sample_matrix(const std::array<float, 16> &raw_view_proj,
                                                  uint32_t shadow_resolution) {
    std::array<float, 16> out = raw_view_proj;
    const float half = 0.5f;
    const float bias = half + (half / float(shadow_resolution == 0 ? 512 : shadow_resolution));

    out[0] *= half;
    out[4] *= half;
    out[8] *= half;
    out[12] *= half;
    out[1] *= half;
    out[5] *= half;
    out[9] *= half;
    out[13] *= half;

    out[0] += bias * out[3];
    out[1] -= bias * out[3];
    out[4] += bias * out[7];
    out[5] -= bias * out[7];
    out[8] += bias * out[11];
    out[9] -= bias * out[11];
    out[12] += bias * out[15];
    out[13] -= bias * out[15];

    return out;
}

std::array<float, 3> unpack_modulator_scale(uint32_t packed_rgb) {
	// byte * 0.015625 (= 1/64); 64 = identity gain
	// [orig: Render_UnpackModulatorToLightScale @ 0x58db30;
	//  EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaef0].
	constexpr float kInv64 = 1.0f / 64.0f;
	return {
		float((packed_rgb >> 16) & 0xFF) * kInv64,
		float((packed_rgb >> 8) & 0xFF) * kInv64,
		float(packed_rgb & 0xFF) * kInv64,
	};
}

WorldLightingBlock build_world_lighting(const WorldLightingInputs &in) {
	WorldLightingBlock out;

	// The base fill: every block color / 255
	// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c80d5..0x5c81f4].
	out.dir_color = unpack_color_255(in.light_packed);
	out.hemi_sky = unpack_color_255(in.sky_packed);
	out.hemi_ground = unpack_color_255(in.ground_packed);
	out.ceiling_color = unpack_color_255(in.ceiling_packed);
	out.floor_color = unpack_color_255(in.floor_packed);

	// The NVG hemisphere rewrite [orig: @ 0x5c8205..0x5c82e9]:
	//   c' = c * (0.25 * f) + modulator_byte * (f * 0.0015625)
	// with f = (level + 1) * 0.2 — the modulator term uses the RAW byte, so
	// its scale is byte * f / 640 = (byte/255) * f * 255/640.
	if (in.nvg_hemi_rewrite) {
		const float f = float(in.nvg_level + 1) * 0.2f;
		const float fade = 0.25f * f;
		const float mod_scale = f * 0.0015625f;
		const float mr = float((in.modulator_packed >> 16) & 0xFF) * mod_scale;
		const float mg = float((in.modulator_packed >> 8) & 0xFF) * mod_scale;
		const float mb = float(in.modulator_packed & 0xFF) * mod_scale;
		auto rewrite = [&](std::array<float, 3> &c) {
			c = { c[0] * fade + mr, c[1] * fade + mg, c[2] * fade + mb };
		};
		rewrite(out.hemi_sky);
		rewrite(out.hemi_ground);
		rewrite(out.ceiling_color);
		rewrite(out.floor_color);
	}

	// dir = -normalize(light_dir) [orig: @ 0x5c82fc..0x5c8378].
	const float len = std::sqrt(in.light_dir[0] * in.light_dir[0] +
	                            in.light_dir[1] * in.light_dir[1] +
	                            in.light_dir[2] * in.light_dir[2]);
	if (len > 0.0f) {
		out.dir = { -in.light_dir[0] / len, -in.light_dir[1] / len, -in.light_dir[2] / len };
	} else {
		out.dir = { 0.0f, 0.0f, 0.0f };
	}

	// The vehicle-scope grey override [orig: @ 0x5c8389..0x5c843c]: dir 0.1,
	// hemis/floor/ceiling 0.5, dir disabled.
	if (in.vehicle_scope_grey) {
		out.dir_enabled = false;
		out.dir_color = { 0.1f, 0.1f, 0.1f };
		out.hemi_sky = { 0.5f, 0.5f, 0.5f };
		out.hemi_ground = { 0.5f, 0.5f, 0.5f };
		out.ceiling_color = { 0.5f, 0.5f, 0.5f };
		out.floor_color = { 0.5f, 0.5f, 0.5f };
	}

	// The NVG world dim [orig: @ 0x5c8448..0x5c84f0]: dir zeroed + disabled,
	// everything else 0.25.
	if (in.nvg_world_dim) {
		out.dir_enabled = false;
		out.dir_color = { 0.0f, 0.0f, 0.0f };
		out.hemi_sky = { 0.25f, 0.25f, 0.25f };
		out.hemi_ground = { 0.25f, 0.25f, 0.25f };
		out.ceiling_color = { 0.25f, 0.25f, 0.25f };
		out.floor_color = { 0.25f, 0.25f, 0.25f };
	}

	// The store-time derivations [orig: RenderBatchCtx_StoreLightingConstants
	// @ 0x5d89e0]: the two hemisphere averages, and the dir color zeroed when
	// the enable flag is clear.
	for (int c = 0; c < 3; ++c) {
		out.indoor_ambient[c] = (out.ceiling_color[c] + out.floor_color[c]) * 0.5f;
		out.outdoor_ambient[c] = (out.hemi_sky[c] + out.hemi_ground[c]) * 0.5f;
	}
	if (!out.dir_enabled)
		out.dir_color = { 0.0f, 0.0f, 0.0f };

	return out;
}

EntityLightingUniforms compute_entity_lighting(const WorldLightingBlock &block,
                                               float effect_scale,
                                               bool interior_lerp,
                                               float interior_daylight) {
	EntityLightingUniforms out;
	if (interior_lerp) {
		// Interior-parented entities (batch entry bit 1): the hemisphere lerps
		// from the indoor pair (floor/ceiling) to the outdoor pair
		// (ground/sky) by the interior's daylight openness, and the
		// directional color scales by it too — a closed building gets pure
		// floor/ceiling ambience with no sun
		// [orig: setup_entity_lighting_and_shader_constants @ 0x5d9a6a..0x5d9c71].
		const float t = interior_daylight;
		for (int c = 0; c < 3; ++c)
			out.dir_color[c] = block.dir_color[c] * effect_scale * t;
		out.hemi_ground = lerp3(block.floor_color, block.hemi_ground, t);
		out.hemi_sky = lerp3(block.ceiling_color, block.hemi_sky, t);
		out.ambient = lerp3(block.indoor_ambient, block.outdoor_ambient, t);
	} else {
		// The outdoor path [orig: @ 0x5d9db5..0x5d9e4d].
		for (int c = 0; c < 3; ++c)
			out.dir_color[c] = block.dir_color[c] * effect_scale;
		out.hemi_ground = block.hemi_ground;
		out.hemi_sky = block.hemi_sky;
		out.ambient = block.outdoor_ambient;
	}
	return out;
}

std::array<float, 3> ff_vertex_light(const EntityLightingUniforms &u,
                                     const std::array<float, 3> &normal,
                                     const std::array<float, 3> &to_light) {
	// ambient (MaterialEmissive = AmbientColor) + directional (D3D light 0) +
	// the hemisphere DELTA lights (D3D lights 2/3, colors hemi - ambient)
	// [orig: Lighting_SetHemisphereD3DLights @ 0x5d8cb0; _FFP.fx TBoringFFP],
	// clamped to [0,1] like the D3D fixed-function vertex diffuse.
	const float ndotl = std::max(0.0f, normal[0] * to_light[0] +
	                                    normal[1] * to_light[1] +
	                                    normal[2] * to_light[2]);
	const float up = std::max(0.0f, normal[1]);
	const float down = std::max(0.0f, -normal[1]);
	std::array<float, 3> lit;
	for (int c = 0; c < 3; ++c) {
		float v = u.ambient[c] + u.dir_color[c] * ndotl +
		          (u.hemi_sky[c] - u.ambient[c]) * up +
		          (u.hemi_ground[c] - u.ambient[c]) * down;
		lit[c] = std::clamp(v, 0.0f, 1.0f);
	}
	return lit;
}

float sun_visibility_factor(int blocked_rays) {
	// quality 4 minus one per blocked ray (3 rays fired), * 0.25 —
	// 1.0 / 0.75 / 0.5 / 0.25 [orig: Entity_ComputeSunVisibility @ 0x5c6800].
	const int quality = std::clamp(4 - blocked_rays, 1, 4);
	return float(quality) * 0.25f;
}

std::array<float, 3> point_light_color(const std::array<float, 3> &rgb,
                                       float intensity,
                                       const std::array<float, 3> &modulator_scale,
                                       bool d3d_light_path) {
	// shader path: rgb * ambient-scale * intensity [orig: @ 0x5a91e7..0x5a9204];
	// D3D-light path adds the 1.5x boost [orig: @ 0x5aa4bc..0x5aa4de].
	const float boost = d3d_light_path ? 1.5f : 1.0f;
	return {
		rgb[0] * modulator_scale[0] * intensity * boost,
		rgb[1] * modulator_scale[1] * intensity * boost,
		rgb[2] * modulator_scale[2] * intensity * boost,
	};
}

std::array<float, 4> point_light_attenuation(int32_t range_fixed) {
	// range = fixed * 1.25 / 65536 (the 0.000019073486 constant)
	// [orig: Light_GetPointLightParams @ 0x5a9251].
	const float range = std::max(float(range_fixed) * 0.000019073486f, 0.001f);
	return { 1.0f, 0.0f, 15.0f / (range * range), 1.0f };
}

std::array<float, 3> terrain_surface_light(float sun_mask,
                                           const std::array<float, 3> &light,
                                           const std::array<float, 3> &sky) {
	// (t0.a * c1 + c0) / 2 then MODULATE2X — the halving and doubling cancel
	// [orig: compile_terrain_pixel_shaders @ 0x605260 shape;
	//  c0/c1 push @ 0x604420].
	return {
		sun_mask * light[0] + sky[0],
		sun_mask * light[1] + sky[1],
		sun_mask * light[2] + sky[2],
	};
}

}  // namespace renderer
