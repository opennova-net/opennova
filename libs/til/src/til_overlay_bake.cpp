#include <til/til_overlay_bake.h>

#include <algorithm>
#include <cmath>

namespace opennova {
namespace {

struct RgbaF {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 0.0f;
};

int wrap_floor(float value, int size) {
	const int floored = static_cast<int>(std::floor(value));
	const int wrapped = floored % size;
	return wrapped < 0 ? wrapped + size : wrapped;
}

float clamp_float(float value, float lo, float hi) {
	return std::max(lo, std::min(value, hi));
}

RgbaF pixel_to_float(const uint8_t *rgba) {
	constexpr float inv = 1.0f / 255.0f;
	return RgbaF{
	    static_cast<float>(rgba[0]) * inv,
	    static_cast<float>(rgba[1]) * inv,
	    static_cast<float>(rgba[2]) * inv,
	    static_cast<float>(rgba[3]) * inv,
	};
}

RgbaF lerp_rgba(RgbaF a, RgbaF b, float t) {
	return RgbaF{
	    a.r + (b.r - a.r) * t,
	    a.g + (b.g - a.g) * t,
	    a.b + (b.b - a.b) * t,
	    a.a + (b.a - a.a) * t,
	};
}

RgbaF sample_bilinear_rgba(const uint8_t *atlas_rgba, int width, int height, TilUv uv) {
	const float sx = clamp_float(uv.u * static_cast<float>(width) - 0.5f, 0.0f, static_cast<float>(width - 1));
	const float sy = clamp_float(uv.v * static_cast<float>(height) - 0.5f, 0.0f, static_cast<float>(height - 1));
	const int x0 = static_cast<int>(std::floor(sx));
	const int y0 = static_cast<int>(std::floor(sy));
	const int x1 = std::min(x0 + 1, width - 1);
	const int y1 = std::min(y0 + 1, height - 1);
	const float tx = sx - static_cast<float>(x0);
	const float ty = sy - static_cast<float>(y0);

	const RgbaF c00 = pixel_to_float(atlas_rgba + 4 * (y0 * width + x0));
	const RgbaF c10 = pixel_to_float(atlas_rgba + 4 * (y0 * width + x1));
	const RgbaF c01 = pixel_to_float(atlas_rgba + 4 * (y1 * width + x0));
	const RgbaF c11 = pixel_to_float(atlas_rgba + 4 * (y1 * width + x1));
	return lerp_rgba(lerp_rgba(c00, c10, tx), lerp_rgba(c01, c11, tx), ty);
}

uint8_t to_byte(float value) {
	return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255));
}

void composite_source_over(uint8_t *dst_rgba, RgbaF src) {
	if (src.a <= 0.0f) {
		return;
	}

	const RgbaF dst = pixel_to_float(dst_rgba);
	const float out_a = src.a + dst.a * (1.0f - src.a);
	RgbaF out;
	out.a = out_a;
	if (out_a > 0.0f) {
		out.r = (src.r * src.a + dst.r * dst.a * (1.0f - src.a)) / out_a;
		out.g = (src.g * src.a + dst.g * dst.a * (1.0f - src.a)) / out_a;
		out.b = (src.b * src.a + dst.b * dst.a * (1.0f - src.a)) / out_a;
	}
	dst_rgba[0] = to_byte(out.r);
	dst_rgba[1] = to_byte(out.g);
	dst_rgba[2] = to_byte(out.b);
	dst_rgba[3] = to_byte(out.a);
}

TilUv interpolate_uv(const TilUvQuad &quad, float x, float z) {
	const TilUv &tl = quad.corners[0];
	const TilUv &tr = quad.corners[1];
	const TilUv &bl = quad.corners[2];
	const TilUv &br = quad.corners[3];
	const TilUv top{tl.u + (tr.u - tl.u) * x, tl.v + (tr.v - tl.v) * x};
	const TilUv bottom{bl.u + (br.u - bl.u) * x, bl.v + (br.v - bl.v) * x};
	return TilUv{top.u + (bottom.u - top.u) * z, top.v + (bottom.v - top.v) * z};
}

} // namespace

bool til_bake_overlay_rgba(const TilFile &til,
                           const uint8_t *atlas_rgba,
                           int atlas_width,
                           int atlas_height,
                           int overlay_width,
                           int overlay_height,
                           std::vector<uint8_t> &out_rgba) {
	out_rgba.clear();
	if (atlas_rgba == nullptr || atlas_width <= 0 || atlas_height <= 0 ||
	    overlay_width <= 0 || overlay_height <= 0) {
		return false;
	}

	out_rgba.assign(static_cast<size_t>(overlay_width) * static_cast<size_t>(overlay_height) * 4u, 0u);

	for (const TilOverlayEntry &entry : til.entries) {
		const TilUvQuad uv_quad =
		    til_build_entry_render_uv_quad(entry.tile_index, entry.flags, atlas_width, atlas_height);
		if (!uv_quad.valid) {
			continue;
		}

		const float origin_x = til_world_x_from_fixed(entry.x_fixed);
		const float origin_z = til_world_z_from_fixed(entry.z_fixed);
		const float max_x = origin_x + static_cast<float>(TIL_CELL_WORLD_UNITS);
		const float max_z = origin_z + static_cast<float>(TIL_CELL_WORLD_UNITS);
		const int ix0 = static_cast<int>(std::floor(origin_x));
		const int iz0 = static_cast<int>(std::floor(origin_z));
		const int ix1 = static_cast<int>(std::ceil(max_x));
		const int iz1 = static_cast<int>(std::ceil(max_z));

		for (int wz_i = iz0; wz_i < iz1; ++wz_i) {
			const float world_z = static_cast<float>(wz_i) + 0.5f;
			const float local_z = (world_z - origin_z) / static_cast<float>(TIL_CELL_WORLD_UNITS);
			if (local_z < 0.0f || local_z > 1.0f) {
				continue;
			}
			const int out_y = wrap_floor(-world_z, overlay_height);
			for (int wx_i = ix0; wx_i < ix1; ++wx_i) {
				const float world_x = static_cast<float>(wx_i) + 0.5f;
				const float local_x = (world_x - origin_x) / static_cast<float>(TIL_CELL_WORLD_UNITS);
				if (local_x < 0.0f || local_x > 1.0f) {
					continue;
				}

				const int out_x = wrap_floor(world_x, overlay_width);
				const TilUv uv = interpolate_uv(uv_quad, local_x, local_z);
				const RgbaF src = sample_bilinear_rgba(atlas_rgba, atlas_width, atlas_height, uv);
				composite_source_over(out_rgba.data() + 4 * (out_y * overlay_width + out_x), src);
			}
		}
	}

	return true;
}

} // namespace opennova
