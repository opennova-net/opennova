// The terrain leg of the dynamic light pool: the per-patch collect + gate +
// pixel-constant build, the projection contract, and the procedural textures
// the light passes sample. Witness map in light_terrain_pass.h.
// [orig: Terrain_RenderSectorBatch @0x6095f9..0x6098bc;
//  Light_SetupTerrainProjectedPassPS @0x5aab30; Light_SetupTerrainProjectedPass
//  @0x5aa830; Lighting_InitTextures @0x5a94f0;
//  Texture_GenerateProceduralFalloffTexture @0x5a92c0;
//  GTexture_GenerateNormalMapCubeMap @0x685570]
#include <runtime/renderer/light_terrain_pass.h>

#include <runtime/renderer/light_scene_internal.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace opennova::renderer {

namespace {

int32_t fixed_from_float_clamped(double value) {
	const double scaled = value * 65536.0;
	if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
		return std::numeric_limits<int32_t>::max();
	}
	if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
		return std::numeric_limits<int32_t>::min();
	}
	return static_cast<int32_t>(scaled);
}

// The interior texel coordinate of the 64-wide procedural textures
// [orig: Texture_GenerateProceduralFalloffTexture @0x5a92c0 — for both modes
//  ((index - 1) * flt_7C3B90 (2.0) + 1) / inner (width - 2 = 62) - 1].
double falloff_axis(int index) {
	constexpr double kSpan = 2.0;        // flt_7C3B90
	constexpr double kInner = kFalloffTextureSize - 2;
	return (static_cast<double>(index - 1) * kSpan + 1.0) / kInner - 1.0;
}

bool falloff_border(int x, int y) {
	return x <= 0 || x >= kFalloffTextureSize - 1 || y <= 0 ||
			y >= kFalloffTextureSize - 1;
}

} // namespace

TerrainLightUv terrain_light_uv_disc(const std::array<float, 3> &point_mission,
		const std::array<float, 3> &light_mission, float inv_scale) {
	// d3d.x = -mission.y, d3d.z = mission.x [orig: @0x611210]; u from the
	// camera-matrix column 0 row, v from column 2 [orig: @0x5aa8a4..0x5aa915].
	TerrainLightUv uv;
	uv.u = (light_mission[1] - point_mission[1]) * inv_scale + 0.5f;
	uv.v = (point_mission[0] - light_mission[0]) * inv_scale + 0.5f;
	return uv;
}

TerrainLightUv terrain_light_uv_height(const std::array<float, 3> &point_mission,
		const std::array<float, 3> &light_mission, float inv_scale) {
	// d3d.y = mission.z [orig: @0x611210]; u from column 1, v fixed at 0.5
	// [orig: @0x5aa93b..0x5aa986].
	TerrainLightUv uv;
	uv.u = (point_mission[2] - light_mission[2]) * inv_scale + 0.5f;
	uv.v = 0.5f;
	return uv;
}

std::array<float, 3> terrain_light_cube_vector(const std::array<float, 3> &point_mission,
		const std::array<float, 3> &light_mission, float inv_scale) {
	// Transform 16 negates every term of the camera-matrix columns 2, 0, 1
	// and their translations [orig: Light_SetupTerrainProjectedPassPS
	// @0x5aab99..0x5aaca5]: (u, v, w) = (light - point).(d3d.z, d3d.x, d3d.y)
	// * inv, with d3d = (-mission.y, mission.z, mission.x) [orig: @0x611210].
	return {
		(light_mission[0] - point_mission[0]) * inv_scale,
		(point_mission[1] - light_mission[1]) * inv_scale,
		(light_mission[2] - point_mission[2]) * inv_scale,
	};
}

uint32_t cube_normalize_texel_argb(int face, int col, int row, int size) {
	// The per-face axis C and row-0 axis U [orig:
	// GTexture_GenerateNormalMapCubeMap @0x685644..0x6858b2, the six switch
	// arms over faces 0..5].
	static constexpr float kAxis[kCubeNormalizeFaces][3] = {
		{1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
		{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
	};
	static constexpr float kUp[kCubeNormalizeFaces][3] = {
		{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
		{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
	};
	if (face < 0 || face >= kCubeNormalizeFaces || size < 2) {
		return 0u;
	}
	const float *c = kAxis[face];
	const float *u = kUp[face];
	// X = C x U [orig: @0x6858ba..0x68590b].
	const float x[3] = {
		c[1] * u[2] - c[2] * u[1],
		c[2] * u[0] - c[0] * u[2],
		c[0] * u[1] - c[1] * u[0],
	};
	// (size - (size >> 31)) >> 1 is size / 2 for the positive sides
	// [orig: @0x6859e8..0x6859ee]; rt and ct are fild / fidiv quotients.
	const int half = size / 2;
	const double rt = static_cast<double>(half - row) / static_cast<double>(half);
	const double ct = static_cast<double>(half - col) / static_cast<double>(half);
	// The face terms are 0 or +-1, so every product below is exact; the
	// components are stored as floats before the normalize
	// [orig: @0x685a66..0x685aab].
	const float dir[3] = {
		static_cast<float>(c[0] + u[0] * rt + x[0] * ct),
		static_cast<float>(c[1] + u[1] * rt + x[1] * ct),
		static_cast<float>(c[2] + u[2] * rt + x[2] * ct),
	};
	// D3DXVec3Normalize [orig: sub_68B032 @0x685aaf]: a platform primitive,
	// a standard normalize here.
	const float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	uint32_t bytes[3] = {};
	for (int axis = 0; axis < 3; ++axis) {
		const float n = length > 0.0f ? dir[axis] / length : 0.0f;
		// trunc(n * 127.5 + 128.0) under the truncating control word
		// [orig: @0x685ab4..0x685b48]; the range stays inside 0.5..255.5.
		const int encoded = static_cast<int>(static_cast<double>(n) * 127.5 + 128.0);
		bytes[axis] = static_cast<uint32_t>(std::clamp(encoded, 0, 255));
	}
	return 0xFF000000u | (bytes[0] << 16) | (bytes[1] << 8) | bytes[2];
}

uint32_t falloff_texture_light2d_argb(int x, int y) {
	// Mode 2 [orig: @0x5a92e8..0x5a9402 — border 0 @0x5a93db; the two exp
	//  expansions; ftol @0x5a93a6; < 0 -> 0xFF000000 @0x5a93b7; > 255 -> 255
	//  @0x5a93c7; 0x010101 * i | 0xFF000000 @0x5a93d2].
	if (falloff_border(x, y)) {
		return 0u;
	}
	const double fx = falloff_axis(x);
	const double fy = falloff_axis(y);
	constexpr double kGain = 4.0;       // flt_7C44B8
	constexpr double kScale = 255.0;    // dbl_7D9F98
	const double value =
			std::exp(-kGain * fx * fx) * std::exp(-kGain * fy * fy) * kScale;
	const int intensity = static_cast<int>(value); // _ftol2_sse truncates
	if (intensity < 0) {
		return 0xFF000000u;
	}
	const uint32_t clamped = static_cast<uint32_t>(std::min(intensity, 255));
	return (0x010101u * clamped) | 0xFF000000u;
}

uint32_t falloff_texture_spot2d_argb(int x, int y) {
	// Mode 1 [orig: @0x5a941a..0x5a94cd — border 0 @0x5a94aa; 1 - (x^2 +
	//  y^2) times 255 @0x5a9465..0x5a9487; < 0 -> 0 @0x5a9490; the rgb form
	//  0x01010101 * i @0x5a94a0 (the alpha-only form i << 24 is never
	//  requested by Lighting_InitTextures)].
	if (falloff_border(x, y)) {
		return 0u;
	}
	const double fx = falloff_axis(x);
	const double fy = falloff_axis(y);
	constexpr double kScale = 255.0;    // dbl_7D9F98
	const double value = (1.0 - (fx * fx + fy * fy)) * kScale;
	int intensity = static_cast<int>(value);
	if (intensity < 0) {
		intensity = 0;
	}
	return 0x01010101u * static_cast<uint32_t>(intensity);
}

uint32_t falloff_texture_spot1d_argb(int x, int row) {
	// [orig: Lighting_InitTextures @0x5a95a6..0x5a9612 — column 0 / 63 ->
	//  0xFFFFFFFF @0x5a95de; else ((col * (row + 1)) >> 1) * 0x010101 |
	//  0xFF000000 @0x5a95be..0x5a95d2]. Eight rows, 64 columns.
	if (x <= 0 || x >= kFalloffTextureSize - 1) {
		return 0xFFFFFFFFu;
	}
	const int gray = (x * (row + 1)) >> 1;
	return (0x010101u * static_cast<uint32_t>(gray)) | 0xFF000000u;
}

TerrainLightPatchBounds terrain_patch_light_bounds(const float aabb_min[3],
		const float aabb_max[3], float sector_ox, float sector_oz) {
	// The traverse_quadtree world AABB: sector origin + the sector-local mesh
	// AABB on x/z, height untouched (terrain_frame.cpp / quadtree.cpp). Then
	// the render-frame -> mission fold: mission = (x, -z, y).
	const double world_min[3] = {
		static_cast<double>(sector_ox) + aabb_min[0],
		static_cast<double>(aabb_min[1]),
		static_cast<double>(sector_oz) + aabb_min[2],
	};
	const double world_max[3] = {
		static_cast<double>(sector_ox) + aabb_max[0],
		static_cast<double>(aabb_max[1]),
		static_cast<double>(sector_oz) + aabb_max[2],
	};
	const int32_t a[3] = {
		fixed_from_float_clamped(world_min[0]),
		fixed_from_float_clamped(-world_min[2]),
		fixed_from_float_clamped(world_min[1]),
	};
	const int32_t b[3] = {
		fixed_from_float_clamped(world_max[0]),
		fixed_from_float_clamped(-world_max[2]),
		fixed_from_float_clamped(world_max[1]),
	};
	TerrainLightPatchBounds bounds;
	for (int axis = 0; axis < 3; ++axis) {
		// The fold negates one axis; re-order per axis like the object pass
		// does for model AABBs.
		bounds.aabb_min_fixed[axis] = std::min(a[axis], b[axis]);
		bounds.aabb_max_fixed[axis] = std::max(a[axis], b[axis]);
	}
	return bounds;
}

} // namespace opennova::renderer

namespace opennova::renderer {

// [orig: Light_CollectNearbyZonesByAABB @0x5aa37a — ((d * d + 0x8000) >> 16)
//  per axis, 16.16 squared distance in world^2] — the same metric the object
// pass sorts by (light_scene.cpp); both live in light_scene_internal.h.
using detail::axis_distance_term;
using detail::saturating_add;

size_t LightScene::collect_terrain_pass_rows(
		const TerrainLightPatchBounds *patches,
		size_t patch_count,
		const TerrainLightPassInputs &inputs,
		TerrainLightPatchRows *out) const {
	if (patches == nullptr || out == nullptr || patch_count == 0) {
		return 0;
	}
	for (size_t p = 0; p < patch_count; ++p) {
		out[p].count = 0;
	}
	// The whole light leg is gated off with the pixel-shader terrain path
	// [orig: g_PolyTrnUsePixelShaderPath == 0 @0x6095e4] and by the render
	// mode that skips the per-light loop [orig: dword_319FB84 @0x60983f].
	if (!inputs.pixel_shader_path || inputs.light_pass_disabled) {
		return 0;
	}
	// One pass snapshots the collection inputs in SLOT ORDER — the order the
	// witnessed first-16 cap depends on [orig: Light_CollectNearbyZonesByAABB
	// @0x5aa250 scans the table forward; hidden flag bit 2 skipped @0x5aa2a4].
	struct CompactSlot {
		LightHandle handle;
		const Slot *slot;
	};
	std::vector<CompactSlot> compact;
	compact.reserve(slots_.size());
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Slot &slot = slots_[i];
		if (!slot.live || slot.hidden) {
			continue;
		}
		compact.push_back(CompactSlot{
				LightHandle{static_cast<uint16_t>(i | detail::kHandleFlag),
						slot.generation},
				&slot});
	}
	struct Candidate {
		const CompactSlot *entry;
		uint64_t distance;
	};
	std::array<Candidate, kTerrainLightQueryLimit> candidates{};
	size_t total = 0;
	for (size_t p = 0; p < patch_count; ++p) {
		const TerrainLightPatchBounds &patch = patches[p];
		std::array<int32_t, 3> center{};
		for (int axis = 0; axis < 3; ++axis) {
			center[axis] = static_cast<int32_t>(
					(static_cast<int64_t>(patch.aabb_min_fixed[axis]) +
							patch.aabb_max_fixed[axis]) >> 1);
		}
		size_t count = 0;
		for (const CompactSlot &entry : compact) {
			bool overlaps = true;
			for (int axis = 0; axis < 3; ++axis) {
				if (entry.slot->aabb_min[axis] > patch.aabb_max_fixed[axis] ||
						entry.slot->aabb_max[axis] < patch.aabb_min_fixed[axis]) {
					overlaps = false;
					break;
				}
			}
			if (!overlaps) {
				continue;
			}
			uint64_t distance = 0;
			for (int axis = 0; axis < 3; ++axis) {
				distance = saturating_add(distance, axis_distance_term(
						static_cast<int64_t>(entry.slot->params.position_fixed[axis]) -
						center[axis]));
			}
			candidates[count] = Candidate{&entry, distance};
			++count;
			if (count >= kTerrainLightQueryLimit) {
				break; // the terrain batch's 16 [orig: @0x609658]
			}
		}
		// Retail bubble sort == stable ascending order [orig: @0x5aa3a8].
		std::stable_sort(candidates.begin(),
				candidates.begin() + static_cast<ptrdiff_t>(count),
				[](const Candidate &a, const Candidate &b) {
					return a.distance < b.distance;
				});
		TerrainLightPatchRows &rows = out[p];
		for (size_t i = 0; i < count; ++i) {
			const Slot &slot = *candidates[i].entry->slot;
			const LightSpawnParams &params = slot.params;
			// Light_PassesActiveGroups with both groups cleared
			// [orig: @0x60967c/@0x609685 then @0x609868]: an owned light never
			// matches a zero interior or owner group.
			if (params.owner_entity != 0) {
				continue;
			}
			// LightInstance_IsAliveAndLightsTerrain [orig: @0x609880 — flag
			// 0x400 = the authored terrain disable].
			if (params.disable_terrain) {
				continue;
			}
			TerrainLightRow &row = rows.rows[rows.count];
			for (int axis = 0; axis < 3; ++axis) {
				row.position[axis] =
						static_cast<float>(params.position_fixed[axis]) / 65536.0f;
			}
			const float radius_world =
					static_cast<float>(params.radius_fixed) / 65536.0f;
			// 26214.4 / range on the ps.1.1 pass [orig:
			// Light_SetupTerrainProjectedPassPS @0x5aab76], 32768 / range on
			// the fixed-function one [orig: @0x5aa873].
			row.inv_scale = terrain_project_scale(
					radius_world, inputs.ps_light_pass);
			// Record bytes /256 at spawn [orig: @0x5a8e51] x the live blend
			// (f14) x the modulator ambient scale x the recip factor, then the
			// gen multiply.
			std::array<float, 3> rgb = {
				static_cast<float>(params.rgb[0]) / 256.0f,
				static_cast<float>(params.rgb[1]) / 256.0f,
				static_cast<float>(params.rgb[2]) / 256.0f,
			};
			for (int channel = 0; channel < 3; ++channel) {
				// The ps.1.1 pass uploads the product as c0 unfolded [orig:
				// Light_SetupTerrainProjectedPassPS @0x5aad93..0x5aae8c]; the
				// fixed-function pass adds x 0.66 and, into c4..c6, x 0.5
				// [orig: Light_SetupTerrainProjectedPass @0x5aa9c8..0x5aaab3].
				rgb[channel] = inputs.ps_light_pass
						? terrain_light_ps_constant(rgb[channel], slot.blend,
								  inputs.ambient_scale[channel],
								  inputs.terrain_factor[channel])
						: terrain_light_ambient(rgb[channel], slot.blend,
								  inputs.ambient_scale[channel],
								  inputs.terrain_factor[channel]);
			}
			// The gen multiply follows the colour on both passes (between the
			// 0.66 and the 0.5 on the fixed-function one; plain scalars, so the
			// order does not change the product) [orig: @0x5aade5..0x5aae4c;
			// @0x5aaa05..0x5aaa5f].
			detail::apply_rgb_gen(params, inputs.flicker, rgb);
			if (inputs.ps_light_pass) {
				// The program reads c0 through the ps_1_x constant range.
				for (float &channel : rgb) {
					channel = terrain_light_ps_constant_register(channel);
				}
			}
			row.pixel_rgb = rgb;
			row.handle = candidates[i].entry->handle;
			++rows.count;
		}
		total += rows.count;
	}
	return total;
}

}  // namespace opennova::renderer