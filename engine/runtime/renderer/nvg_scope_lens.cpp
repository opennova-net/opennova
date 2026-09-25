#include "nvg_scope_lens.h"

#include <base/io/bam.h>

namespace opennova::renderer {

namespace {

// One BAM table stop per 0x4000000 of angle: 64 segments, 65 stops.
// [orig: `add edi, 4000000h` @0x5d22fa; `add esi, 14000000h` over five stops
//  @0x5d0e05; `add edx, 14000000h` @0x5d26d7]
constexpr std::uint32_t kStopStep = 0x4000000u;
constexpr int kStops = 65;

double table_cos(std::uint32_t angle) {
	return io::bam_table_cos(io::bam_table_index(angle));
}

double table_sin(std::uint32_t angle) {
	return io::bam_table_sin(io::bam_table_index(angle));
}

} // namespace

std::array<NvgLensStrip, kNvgPolarPasses> nvg_polar_unwrap_passes() {
	std::array<NvgLensStrip, kNvgPolarPasses> out;
	// [orig: terrain_scene_render `mov esi, 2200000h` @0x5d0a75, `add esi,
	//  800000h` @0x5d0e8c -- an eighth of a stop per pass]
	std::uint32_t base = 0x2200000u;
	for (int p = 0; p < kNvgPolarPasses; ++p, base += 0x800000u) {
		// [orig: `255 / (p + 1)` @0x5d0a9f..0x5d0aaf, `shl eax, 18h; or eax,
		//  3F3F3Fh` @0x5d0aef..0x5d0af2]
		const std::uint32_t color =
				(static_cast<std::uint32_t>(255 / (p + 1)) << 24) | 0x3F3F3Fu;
		// [orig: row 0 `0.48 - 0.01 p` (flt_7DC61C, flt_7C56A8) @0x5d0aa9..0x5d0ab3;
		//  row 16 `0.5 - 0.005 p` (flt_7D4B28) @0x5d0abb..0x5d0ac9]
		const double inner = static_cast<double>(0.48f) - static_cast<double>(0.01f) * p;
		const double outer = 0.5 - static_cast<double>(0.005f) * p;
		NvgLensStrip &strip = out[static_cast<std::size_t>(p)];
		strip.reserve(kNvgLensStripVertices);
		for (int k = 0; k < kStops; ++k) {
			// Stop k reads `(base + (k - 1) * 0x4000000) >> 22` of the wrapped
			// angle. [orig: `lea edi, [esi-4000000h]` @0x5d0b01 and its four
			// siblings through @0x5d0d26]
			const std::uint32_t angle =
					base + static_cast<std::uint32_t>(k - 1) * kStopStep;
			const double c = table_cos(angle);
			const double s = table_sin(angle);
			const auto place = [&](float y, double radius) {
				NvgLensVertex v;
				v.x = static_cast<float>(k);
				v.y = y;
				v.argb = color;
				v.u0 = static_cast<float>(c * radius + 0.5);
				v.v0 = static_cast<float>(0.5 - s * radius);
				v.u1 = v.u0;
				v.v1 = v.v0;
				return v;
			};
			strip.push_back(place(0.0f, inner));
			strip.push_back(place(static_cast<float>(kNvgPolarHeight), outer));
		}
	}
	return out;
}

NvgScopeLens build_nvg_scope_lens(std::int32_t x0, std::int32_t y0, std::int32_t x1,
		std::int32_t y1, int scene_side) {
	NvgScopeLens out;
	// [orig: draw_minimap_compass_border `(x0 + x1) >> 1` @0x5d1d63,
	//  `((h) >> 3) + ((h) >> 1)` @0x5d1d7a, `(y0 + y1) >> 1` @0x5d1d8b]
	const std::int32_t cx = (x0 + x1) >> 1;
	const std::int32_t height = y1 - y0;
	const std::int32_t ring = (height >> 3) + (height >> 1);
	const std::int32_t cy = (y1 + y0) >> 1;
	out.center_x = static_cast<float>(cx);
	out.center_y = static_cast<float>(cy);
	out.ring_size = ring;
	const double r = static_cast<double>(ring);
	// The band radii, stored as floats. [orig: @0x5d1dc0 (x 0.0), @0x5d1e0e
	//  (x 0.5), @0x5d1e4a (flt_7D7C08), @0x5d1e5a (flt_7DC63C), @0x5d1e66
	//  (flt_7DC624)]
	const std::array<float, kNvgLensBands + 1> radii = {
		static_cast<float>(r * 0.0),
		static_cast<float>(r * 0.5),
		static_cast<float>(r * static_cast<double>(0.7f)),
		static_cast<float>(r * static_cast<double>(0.705f)),
		static_cast<float>(r * static_cast<double>(0.71f)),
	};
	// The texture radii and the shrink's step counts. [orig: @0x5d1e7a..0x5d1eb5;
	//  @0x5d1def..0x5d1e1f]
	const std::array<float, kNvgLensBands + 1> texture = {0.0f, 0.324f, 0.475f, 0.4775f, 0.48f};
	const std::array<std::int32_t, kNvgLensBands + 1> steps = {0, 8, 26, 32, 36};
	const double side = static_cast<double>(scene_side > 0 ? scene_side : 1);
	for (int pass = 0; pass < kNvgLensPasses; ++pass) {
		// The pixel-shader arm's glow pass q counts from pass 1; pass 0 is the
		// tint. [orig: @0x5d200c..0x5d2023]
		const int q = pass == 0 ? 0 : pass - 1;
		const std::uint32_t color = pass == 0 ? kNvgLensTintColor : kNvgLensGlowColor;
		// [orig: `0.5 / dword_2BDFA94`, negated unless pass & 1, @0x5d2076..0x5d2090;
		//  `0.5 / dword_2BDFA98`, negated unless pass & 2, plus 0.5,
		//  @0x5d2094..0x5d20c4; `q * 0.00018` (flt_7DC628) @0x5d20b6]
		const float du = static_cast<float>((pass & 1) ? 0.5 / side : -(0.5 / side));
		const float dv = static_cast<float>(((pass & 2) ? 0.5 / side : -(0.5 / side)) + 0.5);
		const float shrink = static_cast<float>(static_cast<double>(q) *
				static_cast<double>(0.00018f));
		for (int band = 0; band < kNvgLensBands; ++band) {
			const std::int32_t inner_steps = steps[static_cast<std::size_t>(band)];
			const double inner_texture = static_cast<double>(texture[static_cast<std::size_t>(band)]) -
					static_cast<double>(shrink) * inner_steps;
			const double outer_texture = static_cast<double>(texture[static_cast<std::size_t>(band + 1)]) -
					static_cast<double>(shrink) * steps[static_cast<std::size_t>(band + 1)];
			// q 2 adds and q 3 subtracts the band's inner step count << 17 of BAM
			// on both vertices. [orig: @0x5d21af..0x5d21ce; @0x5d226d..0x5d228c]
			std::uint32_t rotation = 0u;
			if (q == 2) {
				rotation = static_cast<std::uint32_t>(inner_steps) << 17;
			} else if (q == 3) {
				rotation = 0u - (static_cast<std::uint32_t>(inner_steps) << 17);
			}
			NvgLensStrip &strip = out.passes[static_cast<std::size_t>(pass)]
					[static_cast<std::size_t>(band)];
			strip.reserve(kNvgLensStripVertices);
			for (int k = 0; k < kStops; ++k) {
				const std::uint32_t angle = static_cast<std::uint32_t>(k) * kStopStep;
				// Both reads round to the nearest stop. [orig: `add ..., 200000h;
				//  shr ..., 16h` @0x5d2179..0x5d217f and @0x5d21d6..0x5d21e3]
				const std::uint32_t position = angle + 0x200000u;
				const std::uint32_t lookup = angle + rotation + 0x200000u;
				const double pc = table_cos(position);
				const double ps = table_sin(position);
				const double tc = table_cos(lookup);
				const double ts = table_sin(lookup);
				const auto place = [&](float radius, double texture_radius, bool clear_alpha) {
					NvgLensVertex v;
					v.x = static_cast<float>(pc * static_cast<double>(radius) + cx);
					v.y = static_cast<float>(cy - ps * static_cast<double>(radius));
					v.u0 = static_cast<float>(tc * texture_radius + static_cast<double>(du) + 0.5);
					v.v0 = static_cast<float>(static_cast<double>(dv) - texture_radius * ts);
					v.argb = color;
					// The glow passes zero the alpha byte on the two inner bands'
					// vertices, inert under their ONE/ONE blend. [orig: @0x5d2224..0x5d2239;
					//  @0x5d22db..0x5d22f3]
					if (clear_alpha)
						v.argb &= 0x00FFFFFFu;
					return v;
				};
				strip.push_back(place(radii[static_cast<std::size_t>(band)], inner_texture,
						pass > 0 && band < 2));
				strip.push_back(place(radii[static_cast<std::size_t>(band + 1)], outer_texture,
						pass > 0 && band + 1 < 2));
			}
		}
	}
	// The ring, 0.71 x ring to 1.5 x ring. The inner vertex multiplies the
	// extended sine/cosine, the outer re-reads their float stores.
	// [orig: var_94 @0x5d1e66, var_8C @0x5d1e76; the stops @0x5d23be..0x5d2770 --
	//  `fst [var_E0]` @0x5d23f7 / `fld [var_E0]` @0x5d2448]
	const double ring_inner = static_cast<double>(radii[kNvgLensBands]);
	const double ring_outer = static_cast<double>(static_cast<float>(r * 1.5));
	out.ring.reserve(kNvgLensStripVertices);
	for (int k = 0; k < kStops; ++k) {
		const std::uint32_t angle = 0x200000u + static_cast<std::uint32_t>(k) * kStopStep;
		const double c = table_cos(angle);
		const double s = table_sin(angle);
		const double cf = static_cast<double>(static_cast<float>(c));
		const double sf = static_cast<double>(static_cast<float>(s));
		// [orig: `fild k; fmul flt_7C3DCC` (1/64) @0x5d2429..0x5d242d]
		const float u = static_cast<float>(k) * 0.015625f;
		NvgLensVertex inner;
		inner.x = static_cast<float>(c * ring_inner + cx);
		inner.y = static_cast<float>(cy - ring_inner * s);
		inner.argb = kNvgLensRingInnerColor;
		inner.u0 = u;
		inner.v0 = 0.0f;
		inner.u1 = u;
		inner.v1 = 0.0f;
		NvgLensVertex outer;
		outer.x = static_cast<float>(cf * ring_outer + cx);
		outer.y = static_cast<float>(cy - ring_outer * sf);
		outer.argb = kNvgLensRingOuterColor;
		outer.u0 = u;
		outer.v0 = 1.0f;
		outer.u1 = u;
		outer.v1 = 0.0f;
		out.ring.push_back(inner);
		out.ring.push_back(outer);
	}
	return out;
}

std::int32_t nvg_scoped_scene_fov_q16(float selected_h_over_w, std::int32_t zoom) {
	// A malformed zero/negative zoom keeps unit magnification, as the frame's
	// own optical fov does (world/player_view.cpp player_view_fov_h_deg).
	if (zoom < 1)
		zoom = 1;
	// [orig: sub_5D2990 `fild zoom; fld1; fdivrp` then `fstp [var_20]`
	//  @0x5d29ed..0x5d29fd; `sub_58A920() * flt_7DC640 * var_20 * dbl_7C3618`
	//  @0x5d2a0a..0x5d2a19; `_ftol2_sse` @0x5d2a1f]
	const float reciprocal = static_cast<float>(1.0 / static_cast<double>(zoom));
	const double fov = static_cast<double>(selected_h_over_w) * 10485760.0 *
			static_cast<double>(reciprocal) * 0.5;
	return static_cast<std::int32_t>(fov);
}

std::int32_t nvg_sighted_scene_fov_q16(std::int32_t zoom) {
	if (zoom < 1)
		zoom = 1;
	// [orig: Math_BuildScaledFixedPointToFloatMatrix `fild zoom; fld1; fdivrp`
	//  @0x5d2ac1..0x5d2ac7 (the reciprocal stays on the stack),
	//  `fmul flt_7DBD20` @0x5d2ac9, `_ftol2_sse` @0x5d2acf]
	const double fov = (1.0 / static_cast<double>(zoom)) * 5242880.0;
	return static_cast<std::int32_t>(fov);
}

} // namespace opennova::renderer
