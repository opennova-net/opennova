#include <runtime/renderer/render_slot_shadow.h>

#include <runtime/renderer/light_runtime.h>
#include <runtime/world/collision.h>

#include <algorithm>
#include <cmath>

namespace opennova::renderer {

std::array<float, 3> slot_projection_direction(
		const std::array<float, 3> &sun_surface_to_light) {
	// [orig: render_shadow_pass @ 0x5d7bdc..0x5d7c30 — `if (y < 0.25)
	// y = 0.25`, then negate x/y/z into RenderSlot_DefaultLightDir*].
	const float y = std::max(sun_surface_to_light[1], 0.25f);
	return {-sun_surface_to_light[0], -y, -sun_surface_to_light[2]};
}

int slot_lod_for_radius(float bound_radius_units) {
	// (bound_radius_fixed >> 15) + 1 = 2 * radius + 1, clamped [6, 20]
	// [orig: RenderSlot_AllocSlot @ 0x5d5773..0x5d578a].
	const int lod = static_cast<int>(bound_radius_units * 2.0f) + 1;
	return std::clamp(lod, 6, 20);
}

int slot_lod_for_blob(float width_units, float length_units) {
	// max(w, l) + 7, clamped [2, 20] [orig: @ 0x5d572a..0x5d5767].
	const int lod =
			static_cast<int>(std::max(width_units, length_units)) + 7;
	return std::clamp(lod, 2, 20);
}

int grazing_slot_lod(int base_lod, float dir_y) {
	// (0.5 + |0.5 / dir_y|) * base_lod, clamped [6, 20]
	// [orig: RenderSlot_UpdateEntityLight @ 0x5d6d5c..0x5d6dac,
	// flt_7C3B94 = 0.5].
	if (dir_y == 0.0f) {
		return 20;
	}
	const float scale = 0.5f + std::fabs(0.5f / dir_y);
	const int lod = static_cast<int>(scale * static_cast<float>(base_lod));
	return std::clamp(lod, 6, 20);
}

float silhouette_half_extent(float bound_radius_units) {
	// radius * 1.25 clamped to radius + 0.75
	// [orig: RenderSlot_RenderEntityAndChildren @ 0x5d783e..0x5d7871 —
	// 0.000019073486 = 1.25/65536, 0.000015258789 = 1/65536].
	return std::min(bound_radius_units * 1.25f, bound_radius_units + 0.75f);
}

SlotCaptureViewAxes slot_capture_view_axes(const std::array<float, 3> &direction) {
	// The slot view frame is the shared retail look-at
	// [orig: build_direction_look_at_matrix @ 0x612c90 via
	// setup_shadow_cascade_matrices @ 0x58d31e], its right row mapped
	// through the render<->presentation reflection (the header derives it).
	const DirectionLookAt<float> frame = direction_look_at(direction);
	SlotCaptureViewAxes axes;
	axes.x = {-frame.right[0], -frame.right[1], -frame.right[2]};
	axes.y = frame.up;
	axes.z = {-frame.forward[0], -frame.forward[1], -frame.forward[2]};
	axes.degenerate = frame.degenerate;
	return axes;
}

int slot_texture_size(int texture_order, int shadow_detail) {
	// [orig: RenderSlot_InitTextureChain @ 0x5d5320 — base 256/512/1024 by the
	// shadow-detail option, halving after every second slot, 32 px floor].
	int size = 256;
	if (shadow_detail >= 2) {
		size = 512;
	}
	if (shadow_detail >= 4) {
		size *= 2;
	}
	for (int i = 0; i < kSlotTextureCount; ++i) {
		if (i == texture_order) {
			return size;
		}
		if ((i & 1) != 0 && size > 32) {
			size >>= 1;
		}
	}
	return 32;
}

uint32_t slot_refresh_mask(int shadow_detail) {
	// [orig: RenderSlot_RenderEntityAndChildren @ 0x5d76d9..0x5d76fe].
	if (shadow_detail < 2) {
		return 7;
	}
	if (shadow_detail < 3) {
		return 3;
	}
	return shadow_detail < 4 ? 1 : 0;
}

uint32_t slot_refresh_mask_for(int shadow_detail,
		bool is_local_player_or_parent) {
	// [orig: @ 0x5d7713..0x5d7734 — the local player's slot (or its parent
	// vehicle's) takes mask 0 (every frame) from detail 3 up].
	if (is_local_player_or_parent && shadow_detail >= 3) {
		return 0;
	}
	return slot_refresh_mask(shadow_detail);
}

bool local_first_person_drape_skipped(bool first_person, bool prone,
		int shadow_detail) {
	// [orig: RenderSlot_DrawAllDrapes @ 0x5d6e70..0x5d6e90 — the first-person
	// local player draws no own drape while prone-latched or below detail 2].
	return first_person && (prone || shadow_detail < 2);
}

bool slot_refresh_due(int slot_index, uint32_t frame, uint32_t mask,
		bool dirty) {
	// [orig: @ 0x5d771b..0x5d7748 — `(frame & mask) == (handle & mask)` or
	// the slot dirty bit].
	if (dirty) {
		return true;
	}
	return (frame & mask) == (static_cast<uint32_t>(slot_index) & mask);
}

float drape_fade(float camera_distance_units) {
	// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5d30..0x5d5d53 — 0 below 40 u
	// (0x280000), (d - 40) / 40 beyond (flt_7DC668 = 1/2621440)].
	if (camera_distance_units < kDrapeFadeStartUnits) {
		return 0.0f;
	}
	return std::min((camera_distance_units - kDrapeFadeStartUnits) /
					(kDrapeFadeEndUnits - kDrapeFadeStartUnits),
			1.0f);
}

bool drape_culled(float camera_distance_units) {
	// [orig: @ 0x5d5d3b..0x5d5d40 — >= 80 u (0x500000) skips the draw].
	return camera_distance_units >= kDrapeFadeEndUnits;
}

std::array<float, 3> drape_shadow_term(const std::array<float, 3> &sun_rgb,
		const std::array<float, 3> &sky_rgb, float dir_y) {
	// q_c = sun_c*|y| / (sun_c*|y| + sky_c)
	// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5f63..0x5d6008 — fabs of the slot
	// direction vertical, Env_LightBlock / Env_SkyBlock bytes; the byte
	// scale cancels in the ratio].
	const float ay = std::fabs(dir_y);
	std::array<float, 3> q{};
	for (int c = 0; c < 3; ++c) {
		const float sun = sun_rgb[c] * ay;
		const float denom = sun + sky_rgb[c];
		q[c] = denom > 0.0f ? sun / denom : 0.0f;
	}
	return q;
}

std::array<float, 3> drape_sun_ambient(const std::array<float, 3> &sun_rgb,
		const std::array<float, 3> &sky_rgb, float dir_y, float fade) {
	// ambient_c = 1 - (1 - fade) * q_c [orig: @ 0x5d5fb0..0x5d6008].
	const std::array<float, 3> q = drape_shadow_term(sun_rgb, sky_rgb, dir_y);
	std::array<float, 3> ambient{};
	for (int c = 0; c < 3; ++c) {
		ambient[c] = 1.0f - (1.0f - fade) * q[c];
	}
	return ambient;
}

std::array<float, 3> drape_silhouette_factor(
		const std::array<float, 3> &capture_rgb,
		const std::array<float, 3> &shadow_term, float fade,
		float depth_clip) {
	// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5ca0: stage 0 ADD
	// (silhouette RT, DIFFUSE), stage 1 ADD (shadowztex, CURRENT), both
	// saturating before DESTCOLOR/ZERO multiplies the framebuffer].
	std::array<float, 3> factor{};
	for (int c = 0; c < 3; ++c) {
		const float ambient = 1.0f - (1.0f - fade) * shadow_term[c];
		factor[c] = std::clamp(
				capture_rgb[c] + ambient + depth_clip, 0.0f, 1.0f);
	}
	return factor;
}

static float ntsc_luminance(const std::array<float, 3> &rgb) {
	// [orig: flt_7D4B34 = 0.3, flt_7D8B88 = 0.6, flt_7C69F4 = 0.1].
	return 0.3f * rgb[0] + 0.6f * rgb[1] + 0.1f * rgb[2];
}

std::array<float, 3> drape_attached_light_scale(
		const std::array<float, 3> &rgb, float fade) {
	// (c + lum) * 0.5 * -2 * (1 - fade) = -(c + lum) * (1 - fade)
	// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5e89..0x5d5f14, flt_7D4B24 = -2.0].
	const float lum = ntsc_luminance(rgb);
	std::array<float, 3> out{};
	for (int c = 0; c < 3; ++c) {
		out[c] = -(rgb[c] + lum) * (1.0f - fade);
	}
	return out;
}

SlotLightPick pick_dominant_light(const std::array<float, 3> &entity_pos,
		const std::array<float, 3> &default_direction,
		const SlotPointLight *lights, size_t light_count, bool interior) {
	SlotLightPick pick;
	pick.direction = default_direction;
	pick.attached_handle = 0;
	// Threshold 0.1, zeroed for interior-parented entities
	// [orig: RenderSlot_UpdateEntityLight @ 0x5d6ab7/0x5d6ae4].
	float best = interior ? 0.0f : 0.1f;
	for (size_t i = 0; i < light_count; ++i) {
		const SlotPointLight &light = lights[i];
		const float dx = entity_pos[0] - light.position[0];
		const float dy = entity_pos[1] - light.position[1];
		const float dz = entity_pos[2] - light.position[2];
		const float dist_sq = dx * dx + dy * dy + dz * dz;
		// lum / (dist^2 * quadratic + constant)
		// [orig: @ 0x5d6bce..0x5d6c43].
		const float denom =
				dist_sq * light.attenuation[2] + light.attenuation[0];
		if (denom <= 0.0f) {
			continue;
		}
		const float weighted = ntsc_luminance(light.color) / denom;
		if (weighted <= best) {
			continue;
		}
		const float len = std::sqrt(dist_sq);
		if (len <= 0.0f) {
			continue;
		}
		best = weighted;
		pick.direction = {dx / len, dy / len, dz / len};
		pick.attached_handle = light.handle;
	}
	return pick;
}

SlotDrapeLight drape_attached_light(const SlotPointLight &light, float fade) {
	SlotDrapeLight out;
	out.position = light.position;
	out.range = light.range;
	// Light_FillD3DPointLight's diffuse is the params colour with the D3D
	// 1.5x (renderer::point_light_color's D3D leg; the RgbGen multiply
	// commutes) [orig: Light_FillD3DPointLight @ 0x5aa4a3..0x5aa4de], then
	// the drape's -(c + lum) * (1 - fade) rescale [orig:
	// RenderSlot_DrawSilhouetteDrape @ 0x5d5e89..0x5d5f0d].
	const std::array<float, 3> d3d =
			point_light_color(light.color, 1.0f, {1.0f, 1.0f, 1.0f}, true);
	out.diffuse = drape_attached_light_scale(d3d, fade);
	// atten0 = 1, atten1 = 0, atten2 = 15 / range^2
	// [orig: Light_FillD3DPointLight @ 0x5aa53e..0x5aa553].
	out.quadratic = light.attenuation[2];
	return out;
}

std::array<float, 3> drape_attached_light_color(const SlotDrapeLight &light,
		const std::array<float, 3> &patch_point) {
	// Material ambient 1 x D3DRS_AMBIENT white, plus material diffuse 1 x
	// light 4 on the (0, 1, 0) patch normal, saturated
	// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5e58 (AMBIENT 0xFFFFFF),
	// @ 0x5d5f33..0x5d5f4c (material); normals @ 0x5d52b1..0x5d52c3].
	std::array<float, 3> out{1.0f, 1.0f, 1.0f};
	if (light.range <= 0.0f) {
		return out;
	}
	const float dx = light.position[0] - patch_point[0];
	const float dy = light.position[1] - patch_point[1];
	const float dz = light.position[2] - patch_point[2];
	const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (d <= 0.0f || d > light.range) {
		return out;
	}
	const float attenuation = 1.0f / (1.0f + light.quadratic * d * d);
	const float n_dot_l = std::max(0.0f, dy / d);
	for (int c = 0; c < 3; ++c) {
		out[c] = std::clamp(1.0f + light.diffuse[c] * attenuation * n_dot_l,
				0.0f, 1.0f);
	}
	return out;
}

std::array<float, 3> slot_march_start_offset(bool flags_zero, int32_t heading_bam,
		int32_t pitch_bam, int32_t roll_bam,
		const std::array<int32_t, 3> &bbox_center_q16) {
	// `cmp dword [edi+24h], 0; jz` [orig: RenderSlot_UpdateEntityLight
	// @ 0x5d6ce7..0x5d6ceb]: a set Flags bit keeps the entity position.
	if (!flags_zero) {
		return {0.0f, 0.0f, 0.0f};
	}
	// The Euler matrix transform of entity+0x1FC, its result loaded as the
	// march start [orig: @ 0x5d6cfb..0x5d6d20, the load @ 0x5d6d25..0x5d6d2d;
	// Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40,
	// Math_FixedPointTransformPoint22 @ 0x615810]; relative to the position
	// the translation drops out.
	const int32_t origin[3] = {0, 0, 0};
	const world::CollisionMatrix matrix =
			world::collision_matrix_from_euler(heading_bam, pitch_bam, roll_bam, origin);
	const int32_t centre[3] = {bbox_center_q16[0], bbox_center_q16[1], bbox_center_q16[2]};
	int32_t rotated[3] = {0, 0, 0};
	matrix.rotate_point(centre, rotated);
	// Mission (X, Y, Z-up) -> presentation (X, Z, -Y).
	return {static_cast<float>(rotated[0]) / 65536.0f,
		static_cast<float>(rotated[2]) / 65536.0f,
		-static_cast<float>(rotated[1]) / 65536.0f};
}

bool slot_entity_flags_zero(uint32_t flags, uint32_t engine_flags, int item_type) {
	// `or dword [esi+24h], 400h` for ItemDefType 1
	// [orig: Entity_InitFromModel @ 0x40e208..0x40e20a].
	const uint32_t vehicle_reflectable = item_type == 1 ? 0x400u : 0u;
	return (flags | engine_flags | vehicle_reflectable) == 0;
}

std::array<float, 2> march_shadow_anchor(const std::array<float, 3> &start,
		const std::array<float, 3> &direction,
		const std::function<float(float, float)> &terrain_height,
		int max_steps) {
	// [orig: RenderSlot_UpdateEntityLight @ 0x5d6c86..0x5d6d67 — planar step
	// normalized to unit length (flt_7C32BC = 65536 fold); the vertical step
	// keeps its true rate and is SUBSTITUTED by -0.5 u only when it does not
	// descend (`test eax, eax; jl` @ 0x5d6cd7..0x5d6cdd, the -32768 store
	// @ 0x5d6cdf is reached for a non-negative step only — re-witnessed
	// 2026-08-22; the earlier "clamped to at least 0.5 u of drop" reading
	// steepened every shallow sun)].
	float x = start[0];
	float z = start[2];
	float y = start[1];
	const float planar_len = std::sqrt(
			direction[0] * direction[0] + direction[2] * direction[2]);
	if (!(planar_len > 1.0e-6f) || !terrain_height) {
		return {x, z};
	}
	const float step_x = direction[0] / planar_len;
	const float step_z = direction[2] / planar_len;
	float step_y = direction[1] / planar_len;
	if (step_y >= 0.0f) {
		step_y = -0.5f;
	}
	for (int i = 0; i < max_steps && terrain_height(x, z) < y; ++i) {
		x += step_x;
		z += step_z;
		y += step_y;
	}
	return {x, z};
}

float slot_march_start_height(float caster_height, float terrain_height,
		float contact_tolerance) {
	const float gap = caster_height - terrain_height;
	if (gap > 0.0f && gap <= std::max(contact_tolerance, 0.0f)) {
		return terrain_height;
	}
	return caster_height;
}

SlotPatch slot_patch_bounds(float anchor_x, float anchor_north, int lod) {
	// The patch origin is the anchor backed off half a lod west and pushed
	// half a lod north, rounded to the lod band's grid; the (lod + 1)^2 grid
	// then runs east and south at 1 u, so the patch is the lod x lod square
	// [origin_x, origin_x + lod] x [origin_north - lod, origin_north]
	// [orig: RenderSlot_RebuildPatchVertexBuffer @ 0x5d5130 — raw origin
	//  `slot[15] - lod << 15`, `slot[16] + lod << 15` @ 0x5d5142/@ 0x5d5149;
	//  `(v + 0x8000) & 0xFFFF0000` below lod 10 @ 0x5d515e, `(v + 0x10000) &
	//  0xFFFE0000` for 10..15 @ 0x5d517d, `(v + 0x20000) & 0xFFFC0000` from 16
	//  @ 0x5d5197; rows `x += 0x10000` @ 0x5d52f7, columns `north - col <<
	//  16` @ 0x5d527a, `resolution = lod + 1` vertices each way].
	const float grid = lod >= 16 ? 4.0f : (lod >= 10 ? 2.0f : 1.0f);
	const float half = static_cast<float>(lod) * 0.5f;
	const float origin_x =
			std::floor((anchor_x - half) / grid + 0.5f) * grid;
	const float origin_north =
			std::floor((anchor_north + half) / grid + 0.5f) * grid;
	SlotPatch patch;
	patch.min_x = origin_x;
	patch.max_x = origin_x + static_cast<float>(lod);
	patch.max_north = origin_north;
	patch.min_north = origin_north - static_cast<float>(lod);
	return patch;
}

float slot_patch_lift(int lod) {
	// (lod + 1) * 0.004 [orig: RenderSlot_RebuildPatchVertexBuffer
	// @ 0x5d5201..0x5d5212 — fild slot+0x28, fmul flt_7DB864 — added to
	// every height sample @ 0x5d529b].
	return static_cast<float>(lod + 1) * kSlotPatchLiftStep;
}

void slot_patch_vertices(const SlotPatch &patch, int lod,
		const std::function<float(float, float)> &terrain_height,
		std::vector<std::array<float, 3>> &out) {
	// The outer loop steps east (x += 0x10000 @ 0x5d52f7), the inner one south
	// (north - col << 16 @ 0x5d5269..0x5d5274); every vertex adds the lift to
	// its height sample (@ 0x5d527a..0x5d529f).
	const int resolution = lod + 1;
	const float lift = slot_patch_lift(lod);
	out.clear();
	out.reserve(static_cast<size_t>(resolution) * static_cast<size_t>(resolution));
	for (int i = 0; i < resolution; ++i) {
		const float x = patch.min_x + static_cast<float>(i);
		for (int j = 0; j < resolution; ++j) {
			const float z = -(patch.max_north - static_cast<float>(j));
			const float height = terrain_height ? terrain_height(x, z) : 0.0f;
			out.push_back({x, height + lift, z});
		}
	}
}

void slot_patch_indices(int lod, std::vector<uint16_t> &out) {
	// [orig: init_shadow_decal_index_buffer @ 0x5d5453..0x5d54f4 — edi = row
	// * resolution, ebx = (row + 1) * resolution, six indices per cell].
	const int resolution = lod + 1;
	out.clear();
	out.reserve(static_cast<size_t>(lod) * static_cast<size_t>(lod) * 6u);
	for (int i = 0; i < lod; ++i) {
		const int row = i * resolution;
		const int next = (i + 1) * resolution;
		for (int j = 0; j < lod; ++j) {
			const uint16_t a = static_cast<uint16_t>(row + j);
			const uint16_t b = static_cast<uint16_t>(row + j + 1);
			const uint16_t c = static_cast<uint16_t>(next + j);
			const uint16_t d = static_cast<uint16_t>(next + j + 1);
			out.insert(out.end(), {a, d, c, a, b, d});
		}
	}
}

std::array<uint32_t, kShadowZTexWidth * kShadowZTexHeight> shadowztex_pixels() {
	// [orig: shadow_system_init_resources @ 0x5d6260..0x5d62a7 — 4 rows of
	//  32 ARGB texels: row 3 white, rows 0..2 white below column 16, the one
	//  gray texel at column 16 (0xFF808080), black beyond].
	std::array<uint32_t, kShadowZTexWidth * kShadowZTexHeight> px{};
	for (int row = 0; row < kShadowZTexHeight; ++row) {
		for (int col = 0; col < kShadowZTexWidth; ++col) {
			uint32_t argb = 0xFF000000u;
			if (row == kShadowZTexHeight - 1 || col < 16) {
				argb = 0xFFFFFFFFu;
			} else if (col == 16) {
				argb = 0xFF808080u;
			}
			px[static_cast<size_t>(row * kShadowZTexWidth + col)] = argb;
		}
	}
	return px;
}

SlotDepthClip slot_depth_clip(const std::array<float, 3> &slot_direction,
		float half_size, bool person, const std::array<float, 3> &entity_pos) {
	// [orig: RenderSlot_DrawSilhouetteDrape @ 0x5d5d66..0x5d5de1 — two copies
	//  of the slot direction; the second's VERTICAL x 4.0 (flt_7C44B8) for
	//  itemdef type 3 @ 0x5d5d7e..0x5d5d91; light_pos = slot pos - dir1
	//  @ 0x5d5d95..0x5d5dd4; build_shadow_cascade_uv_matrices @ 0x58cf10 —
	//  k = 0.5 / half_size @ 0x58cf2f, the look-ats normalize their direction
	//  (build_direction_look_at_matrix @ 0x612c90), the detail u row is the
	//  dir2 look-at's forward column scaled k with + 0.5 @ 0x58d1cc..0x58d204,
	//  the detail v row is the primary's ALREADY k-scaled depth column times
	//  0.333 k with + 0.5 @ 0x58d222..0x58d249].
	SlotDepthClip clip;
	const float k = half_size > 1.0e-6f ? 0.5f / half_size : 0.0f;
	std::array<float, 3> lp = {entity_pos[0] - slot_direction[0],
			entity_pos[1] - slot_direction[1], entity_pos[2] - slot_direction[2]};
	auto normalized = [](std::array<float, 3> v) {
		const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (len > 1.0e-6f) {
			v[0] /= len;
			v[1] /= len;
			v[2] /= len;
		}
		return v;
	};
	const std::array<float, 3> f1 = normalized(slot_direction);
	const std::array<float, 3> f2 = normalized({slot_direction[0],
			slot_direction[1] * (person ? kPersonClipSteepening : 1.0f),
			slot_direction[2]});
	const float v_scale = 0.333f * k * k;
	clip.u_offset = 0.5f;
	clip.v_offset = 0.5f;
	for (int i = 0; i < 3; ++i) {
		clip.u_axis[i] = k * f2[i];
		clip.v_axis[i] = v_scale * f1[i];
		clip.u_offset -= clip.u_axis[i] * lp[i];
		clip.v_offset -= clip.v_axis[i] * lp[i];
	}
	return clip;
}

static bool slot_excluded(const SlotCandidateState &state) {
	// [orig: RenderSlot_SortAndAssign @ 0x5d6581..0x5d6627 —
	// hidden (Flags & 1), seat-parented, or standing on a vehicle; the
	// silhouette-render leg re-checks the same predicates
	// @ 0x5d774e..0x5d77b3].
	return state.hidden || state.seat_parented || state.on_vehicle;
}

int32_t slot_priority_score(const std::array<float, 2> &camera_pos2d,
		const std::array<float, 2> &view_dir2d,
		const SlotCandidateState &state) {
	// [orig: RenderSlot_SortAndAssign @ 0x5d6535..0x5d6871].
	if (slot_excluded(state)) {
		return kSlotScoreExcluded;
	}
	const float dx = state.pos2d[0] - camera_pos2d[0];
	const float dz = state.pos2d[1] - camera_pos2d[1];
	const float dist = std::sqrt(dx * dx + dz * dz);
	// base = dist_fixed * 0.25 (flt_7C333C); beyond 0x500000 the record is
	// dropped [orig: @ 0x5d668f..0x5d66b4] — base > 0x500000 IS dist > 320,
	// the slot-bind horizon.
	const float base = dist * 65536.0f * 0.25f;
	if (dist > kSlotBindMaxDistance) {
		return kSlotScoreExcluded;
	}
	// weight = 1.5 - dot(view_unit, to_entity_unit) (98304 Q16); a
	// degenerate view or offset drops the alignment term
	// [orig: @ 0x5d671b..0x5d6851].
	float dot = 0.0f;
	const float view_len = std::sqrt(
			view_dir2d[0] * view_dir2d[0] + view_dir2d[1] * view_dir2d[1]);
	if (view_len > 1.0e-6f && dist > 1.0e-6f) {
		dot = (view_dir2d[0] * dx + view_dir2d[1] * dz) / (view_len * dist);
	}
	float score = base * (1.5f - dot);
	if (state.is_local_player_or_parent) {
		// [orig: @ 0x5d6864..0x5d6868 — priority >> 1].
		score *= 0.5f;
	}
	// The horizon bounds base at 0x500000 and the weight (1.5 - dot) at
	// 2.5, so the score never reaches the exclusion sentinel.
	return static_cast<int32_t>(score);
}

bool RenderSlotPlan::register_entity(uint64_t id) {
	// [orig: RenderSlot_AllocSlot @ 0x5d5690 — a registered entity keeps
	// its index for life; retail appends at the high-water count]. The
	// lowest-free reuse is the device fold the header describes.
	int free_index = -1;
	for (int i = 0; i < kSlotRecordCount; ++i) {
		const Record &record = records_[static_cast<size_t>(i)];
		if (record.live && record.id == id) {
			return true;
		}
		if (!record.live && free_index < 0) {
			free_index = i;
		}
	}
	if (free_index < 0) {
		// [orig: @ 0x5d56d6 — a full table refuses].
		return false;
	}
	Record &record = records_[static_cast<size_t>(free_index)];
	record = Record{};
	record.id = id;
	record.live = true;
	++live_count_;
	return true;
}

void RenderSlotPlan::release_entity(uint64_t id) {
	for (Record &record : records_) {
		if (!record.live || record.id != id) {
			continue;
		}
		if (record.patch_index >= 0) {
			patch_used_[static_cast<size_t>(record.patch_index)] = false;
		}
		record = Record{};  // the index stays put for the next registration
		--live_count_;
		return;
	}
}

std::vector<SlotAssignment> RenderSlotPlan::assign(
		const std::array<float, 2> &camera_pos2d,
		const std::array<float, 2> &view_dir2d,
		const std::function<SlotCandidateState(uint64_t)> &state_for) {
	struct Scored {
		size_t record;
		size_t out;
		int32_t score;
		SlotCandidateState state;
	};
	std::vector<Scored> scored;
	scored.reserve(live_count_);
	for (size_t i = 0; i < records_.size(); ++i) {
		if (!records_[i].live) {
			continue;
		}
		Scored entry;
		entry.record = i;
		entry.out = scored.size();
		entry.state = state_for(records_[i].id);
		entry.score = slot_priority_score(camera_pos2d, view_dir2d,
				entry.state);
		scored.push_back(entry);
	}
	// One row per live record in table order; the exclusion classification
	// reads only the state [orig: RenderSlot_DrawAllDrapes @ 0x5d6e54..].
	std::vector<SlotAssignment> out(scored.size());
	for (const Scored &entry : scored) {
		SlotAssignment &assignment = out[entry.out];
		assignment.id = records_[entry.record].id;
		assignment.record_index = static_cast<int>(entry.record);
		assignment.excluded = slot_excluded(entry.state);
	}
	// Ascending bubble sort in retail; stable ascending here
	// [orig: @ 0x5d6890..0x5d68d9].
	std::stable_sort(scored.begin(), scored.end(),
			[](const Scored &a, const Scored &b) { return a.score < b.score; });

	// Pass 1: release everything excluded or past the 24-patch horizon
	// [orig: @ 0x5d68ed..0x5d693c].
	for (size_t rank = 0; rank < scored.size(); ++rank) {
		Record &record = records_[scored[rank].record];
		if (scored[rank].score == kSlotScoreExcluded ||
				rank >= static_cast<size_t>(kSlotPatchCount)) {
			if (record.bound) {
				if (record.patch_index >= 0) {
					patch_used_[static_cast<size_t>(record.patch_index)] =
							false;
				}
				record.bound = false;
				record.patch_index = -1;
				record.capture_order = -1;
			}
		}
	}
	// Pass 2: bind the leading 24 and hand the first 12 their capture RTs
	// [orig: @ 0x5d6944..0x5d69ef]; a bound slot with a live silhouette RT
	// drapes it [orig: RenderSlot_DrawAllDrapes @ 0x5d6e54..0x5d6ec4].
	int capture_count = 0;
	for (size_t rank = 0;
			rank < scored.size() &&
			rank < static_cast<size_t>(kSlotPatchCount);
			++rank) {
		if (scored[rank].score == kSlotScoreExcluded) {
			continue;
		}
		Record &record = records_[scored[rank].record];
		SlotAssignment &assignment = out[scored[rank].out];
		if (!record.bound) {
			record.bound = true;
			for (int k = 0; k < kSlotPatchCount; ++k) {
				if (!patch_used_[static_cast<size_t>(k)]) {
					patch_used_[static_cast<size_t>(k)] = true;
					record.patch_index = k;
					break;
				}
			}
		}
		assignment.bound = true;
		if (scored[rank].state.dynamic) {
			if (capture_count < kSlotCaptureCount) {
				const int order = capture_count++;
				// Dirty only when the RT index changed
				// [orig: @ 0x5d69c8..0x5d69d9].
				assignment.capture_dirty = record.capture_order != order;
				record.capture_order = order;
				assignment.capture_order = order;
			} else {
				record.capture_order = -1;
			}
		} else {
			record.capture_order = -1;
		}
		if (!assignment.excluded) {
			assignment.draws_silhouette =
					scored[rank].state.dynamic && assignment.capture_order >= 0;
		}
	}
	return out;
}

}  // namespace opennova::renderer