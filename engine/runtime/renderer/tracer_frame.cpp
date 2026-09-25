#include <runtime/renderer/tracer_frame.h>

#include <runtime/renderer/render_order.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace opennova::renderer {
namespace {

// The witnessed per-style ribbon tables, built once. Static ramps are ported
// from the .data blocks; the computed ramps run the witnessed build loops
// [orig: CEffectEmitterPool_ResetAndBuildStyles @ 0x5DB3A0].
struct TracerStyleTables {
	// stdred/stdgreen [orig: .data @ 0x8437F0 family].
	std::array<std::uint32_t, 12> stdred_colors = {0x000000, 0xE08080, 0xC08080,
			0xA04040, 0x802020, 0x601010, 0x200000, 0x100000, 0x100000, 0x100000,
			0x080000, 0x000000};
	std::array<std::uint32_t, 12> stdgreen_colors = {0x000000, 0x80A080, 0x808880,
			0x407040, 0x205820, 0x104010, 0x001400, 0x000800, 0x000800, 0x000800,
			0x000400, 0x000000};
	std::array<float, 1> std_sizes = {0.02f};

	// Rocket/AT4 smoke: 112 gray 0xC0C0C0 entries, quadratic alpha fade
	// a = ((255 - 2i)^2) >> 8; linear width growth [orig: @ 0x5db4f8-0x5db5f6].
	std::array<std::uint32_t, 112> smoke_colors{};
	std::array<float, 32> rocket_sizes{};
	std::array<float, 32> at4_sizes{};

	// Grenade: 64 grays, cubic alpha fade a = (192 * (255 - 3i)^3) >> 24
	// [orig: @ 0x5db648].
	std::array<std::uint32_t, 64> grenade_colors{};
	std::array<float, 32> grenade_sizes{};

	// Rapid red/green: the short 6-entry ramps.
	std::array<std::uint32_t, 6> rapidred_colors = {0x000000, 0xE08080, 0xA04040,
			0x200000, 0x100000, 0x000000};
	std::array<std::uint32_t, 6> rapidgreen_colors = {0x000000, 0x80A080, 0x407040,
			0x001400, 0x000800, 0x000000};

	// The NVG laser style (numeric id 8): 0xFF2020 red, alpha ramps UP 6/entry
	// [orig: @ 0x5db6f7-0x5db711].
	std::array<std::uint32_t, 32> nvg_colors{};
	std::array<float, 1> nvg_sizes = {0.01f};

	// Sniper: dim high-alpha ramps, width 0.04 -> 0.1 over 10 [orig: .data
	// @ 0x8458C8].
	std::array<std::uint32_t, 20> sniper_red = {0xFF180000, 0xFF180000, 0xFF180000,
			0xFF180000, 0xF0070000, 0xE0070000, 0xD0060000, 0xC0060000, 0xB0050000,
			0xA0050000, 0x90040000, 0x80040000, 0x70030000, 0x60030000, 0x50020000,
			0x40020000, 0x30010000, 0x20010000, 0x10000000, 0x00000000};
	std::array<std::uint32_t, 20> sniper_green{};
	std::array<float, 10> sniper_sizes = {0.04f, 0.05f, 0.06f, 0.07f, 0.08f,
			0.09f, 0.1f, 0.1f, 0.1f, 0.1f};

	// DF1: 32-entry red/green fades, thin constant width
	// [orig: @ 0x5db3d9-0x5db4d6].
	std::array<std::uint32_t, 32> df1_red{};
	std::array<std::uint32_t, 32> df1_green{};
	std::array<float, 1> df1_sizes = {0.006f};

	std::array<TracerStyleView, 13> styles{};

	TracerStyleTables() {
		for (int i = 0; i < 112; ++i) {
			const std::uint32_t v = static_cast<std::uint32_t>(255 - 2 * i);
			smoke_colors[static_cast<std::size_t>(i)] =
					(((v * v) >> 8) << 24) | 0xC0C0C0u;
		}
		for (int i = 0; i < 32; ++i) {
			rocket_sizes[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.0625f;
			at4_sizes[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.03125f;
			grenade_sizes[static_cast<std::size_t>(i)] =
					static_cast<float>(i) * 0.0078125f;
		}
		for (int i = 0; i < 64; ++i) {
			const std::int64_t s = 255 - 3 * i;
			const std::uint32_t a =
					static_cast<std::uint32_t>((192 * s * s * s) >> 24);
			grenade_colors[static_cast<std::size_t>(i)] = (a << 24) | 0xC0C0C0u;
		}
		for (int i = 0; i < 32; ++i) {
			nvg_colors[static_cast<std::size_t>(i)] =
					(static_cast<std::uint32_t>(6 * i) << 24) | 0xFF2020u;
		}
		for (int i = 0; i < 20; ++i) {
			const std::uint32_t c = sniper_red[static_cast<std::size_t>(i)];
			sniper_green[static_cast<std::size_t>(i)] =
					(c & 0xFF000000u) | (((c >> 16) & 0xFFu) << 8);
		}
		df1_red[0] = 0;
		df1_green[0] = 0;
		for (int i = 1; i < 32; ++i) {
			const std::uint32_t hi = static_cast<std::uint32_t>((32 - i) * 8) >> 1;
			const std::uint32_t lo = static_cast<std::uint32_t>((32 - i) * 8) >> 2;
			df1_red[static_cast<std::size_t>(i)] = (hi << 16) | (lo << 8) | lo;
			df1_green[static_cast<std::size_t>(i)] = (lo << 16) | (hi << 8) | lo;
		}

		struct Flags {
			bool fog_black;
			bool cross_section;
			float wave_u;
			float wave_v;
			float wave_t;
			bool distortion;
			TracerShader shader;
		};
		auto view = [](const std::uint32_t *colors, int color_count,
							const float *sizes, int size_count, std::uint32_t base,
							const Flags &f) {
			TracerStyleView v;
			v.colors = colors;
			v.color_count = color_count;
			v.sizes = sizes;
			v.size_count = size_count;
			v.base_argb = base;
			v.fog_black = f.fog_black;
			v.cross_section = f.cross_section;
			v.wave_u = f.wave_u;
			v.wave_v = f.wave_v;
			v.wave_t = f.wave_t;
			v.distortion = f.distortion;
			v.shader = f.shader;
			return v;
		};
		// The block words +0 / +4 / +0x81C..0x824 / +0x828 and the channel's
		// normal-pass material: the static blocks read verbatim off .data
		// (std/rapid/sniper), the built ones written by the style build
		// [orig: rocket @ 0x2BF4A40 (B=1, wave 1/1/1, dist 1), at4 @ 0x2BF4210
		// (B=1, 2/1.5/1, dist 1), grenade @ 0x2BF39E0 (B=1, 8/4/1), NVG
		// @ 0x2BF31B0 (A=1, B=1, 2/2/0.1), DF1 @ 0x2BF2980/0x2BF2150 (A=1);
		// the materials per CEffectChannel_Init @ 0x5DB156..0x5DB1D6].
		const Flags stock{true, false, 0.0f, 0.0f, 0.0f, false, TracerShader::Stock};
		const Flags sniper{true, true, 0.0f, 0.0f, 0.0f, true, TracerShader::Stock};
		const Flags rocket{false, true, 1.0f, 1.0f, 1.0f, true, TracerShader::Smoke};
		const Flags at4{false, true, 2.0f, 1.5f, 1.0f, true, TracerShader::Smoke};
		const Flags grenade{false, true, 8.0f, 4.0f, 1.0f, false, TracerShader::Smoke};
		const Flags nvg{true, true, 2.0f, 2.0f, 0.1f, false, TracerShader::NvgLaser};
		// index 0 = the fallback (stdred — the CEffectChannel_Init default case
		// @ 0x5db1c8).
		styles[0] = view(stdred_colors.data(), 12, std_sizes.data(), 1, 0, stock);
		styles[1] = styles[0];
		styles[2] = view(stdgreen_colors.data(), 12, std_sizes.data(), 1, 0, stock);
		styles[3] = view(smoke_colors.data(), 112, rocket_sizes.data(), 32,
				0xC0C0C0u, rocket);
		styles[4] = view(smoke_colors.data(), 112, at4_sizes.data(), 32,
				0xC0C0C0u, at4);
		styles[5] = view(grenade_colors.data(), 64, grenade_sizes.data(), 32,
				0xC0C0C0u, grenade);
		styles[6] = view(rapidred_colors.data(), 6, std_sizes.data(), 1, 0, stock);
		styles[7] = view(rapidgreen_colors.data(), 6, std_sizes.data(), 1, 0, stock);
		styles[8] = view(nvg_colors.data(), 32, nvg_sizes.data(), 1, 0xC04040u, nvg);
		styles[9] = view(sniper_red.data(), 20, sniper_sizes.data(), 10, 0, sniper);
		styles[10] = view(sniper_green.data(), 20, sniper_sizes.data(), 10, 0, sniper);
		styles[11] = view(df1_red.data(), 32, df1_sizes.data(), 1, 0, stock);
		styles[12] = view(df1_green.data(), 32, df1_sizes.data(), 1, 0, stock);
	}
};

const TracerStyleTables &tables() {
	static const TracerStyleTables t;
	return t;
}

inline TracerVec3 sub(const TracerVec3 &a, const TracerVec3 &b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline TracerVec3 cross(const TracerVec3 &a, const TracerVec3 &b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float dot(const TracerVec3 &a, const TracerVec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline TracerVec3 scale(const TracerVec3 &v, float s) {
	return {v.x * s, v.y * s, v.z * s};
}

// A zero-length vector stays zero: every normalize in the ribbon build falls
// through to a zero vector [orig: the fucomp/jnp pairs @ 0x5DC24F/0x5DC27C,
// @ 0x5DC2B0/0x5DC2DB, @ 0x5DC3A1/0x5DC3BA].
inline TracerVec3 normalize_or_zero(const TracerVec3 &v) {
	const float len = std::sqrt(dot(v, v));
	if (len == 0.0f) {
		return {0.0f, 0.0f, 0.0f};
	}
	return scale(v, 1.0f / len);
}

// The distortion pass's vertex alpha: 255 - ((255 - a)^3 >> 16), added back
// over the RGB [orig: @ 0x5DBD0B..0x5DBD46].
std::uint32_t distortion_color(std::uint32_t argb) {
	const std::int32_t inv = 255 - static_cast<std::int32_t>(argb >> 24);
	const std::int32_t alpha = 255 - ((inv * inv * inv) >> 16);
	return (static_cast<std::uint32_t>(alpha) << 24) + (argb & 0xFFFFFFu);
}

TracerVertex vertex_at(const TracerVec3 &p, std::uint32_t argb) {
	TracerVertex v;
	v.x = p.x;
	v.y = p.y;
	v.z = p.z;
	v.argb = argb;
	return v;
}

// One point run through the witnessed build. `age` indexes the ramp; the
// distortion pass widens the ribbon and re-derives the vertex alpha.
void append_run(const float *points, int count, int age, const TracerStyleView &style,
		const TracerView &view, TracerPass pass, TracerShader shader, bool fog_black,
		TracerRibbonFrame &out) {
	const std::uint32_t first_vertex = static_cast<std::uint32_t>(out.vertices.size());
	const std::uint32_t first_index = static_cast<std::uint32_t>(out.indices.size());
	const bool distortion = pass == TracerPass::Distortion;
	// The minimum screen width: 1/_11 x 1.831e-8 per 16.16 distance unit
	// [orig: fld1 / fdiv mat / fmul flt_7DC69C @ 0x5DC0D6..0x5DC0F7].
	const float min_width_per_q16 =
			(1.0f / view.projection_x_scale) * 1.831054774e-8f;
	const TracerVec3 forward = view.forward;
	// GetTickCount is read as a signed dword [orig: @ 0x5DC104; fild @ 0x5DC4F8].
	const double tick = static_cast<double>(static_cast<std::int32_t>(view.tick_ms));
	for (int i = 0; i < count - 1; ++i) {
		const float *row = points + static_cast<std::size_t>(i) * 4;
		const TracerVec3 p{row[0], row[1], row[2]};
		const float w = row[3];
		const TracerVec3 next{row[4], row[5], row[6]};
		// The eye ray and the segment, both unit [orig: @ 0x5DC192..0x5DC2F9].
		const TracerVec3 a_hat = normalize_or_zero(sub(p, view.camera));
		const TracerVec3 d_hat = normalize_or_zero(sub(next, p));
		// The ribbon's side vector: the eye-ray x segment cross product with its
		// component along the view direction removed, so the ribbon plane stays
		// parallel to the screen [orig: @ 0x5DC2F9..0x5DC3CE]. Retail crosses
		// eye x segment in its Y-negated upload frame (a mirror of this one), so
		// here the operands swap.
		const TracerVec3 c = cross(d_hat, a_hat);
		const TracerVec3 r_hat =
				normalize_or_zero(sub(c, scale(forward, dot(c, forward))));
		// Ramp index (count - i) + age - 1, the colour and size tables clamped
		// separately [orig: @ 0x5DC3D0..0x5DC41F].
		const int idx = count - i + age - 1;
		const int color_idx = std::clamp(idx, 0, style.color_count - 1);
		const int size_idx = std::clamp(idx, 0, style.size_count - 1);
		// Half-width: the size curve times the point's jitter against the
		// distance-proportional minimum; the 16.16 eye distance saturates at
		// 2147418112 and truncates [orig: @ 0x5DC423..0x5DC4DB].
		const TracerVec3 to_eye = sub(view.camera, p);
		const double q16 = 65536.0;
		const double dist_q16 = std::sqrt(
				static_cast<double>(to_eye.x) * q16 * static_cast<double>(to_eye.x) * q16 +
				static_cast<double>(to_eye.y) * q16 * static_cast<double>(to_eye.y) * q16 +
				static_cast<double>(to_eye.z) * q16 * static_cast<double>(to_eye.z) * q16);
		const std::int32_t dist_int =
				static_cast<std::int32_t>(std::min(dist_q16, 2147418112.0));
		const float min_width = static_cast<float>(dist_int) * min_width_per_q16;
		const float size_w = style.sizes[static_cast<std::size_t>(size_idx)] * w;
		const float width = size_w < min_width ? min_width : size_w;
		TracerVec3 rs = scale(r_hat, width);
		// The oldest point takes the style base colour [orig: @ 0x5DC4DB..0x5DC4E8].
		std::uint32_t argb = i == 0 ? style.base_argb
									: style.colors[static_cast<std::size_t>(color_idx)];
		std::uint32_t outer_argb = style.base_argb;
		if (distortion) {
			// The distortion ribbon is 1.2x wider and its alpha is re-derived; the
			// cross-section's outer pair keeps the raw base colour
			// [orig: flt_7D8FB4 @ 0x5DBD0B; @ 0x5DBE1E].
			rs = scale(rs, 1.2f);
			argb = distortion_color(argb);
		}
		if (!style.cross_section) {
			// Two vertices, P + rs then P - rs, no texture coordinates
			// [orig: @ 0x5DC798..0x5DC7E5].
			out.vertices.push_back(vertex_at({p.x + rs.x, p.y + rs.y, p.z + rs.z}, argb));
			out.vertices.push_back(vertex_at({p.x - rs.x, p.y - rs.y, p.z - rs.z}, argb));
			continue;
		}
		// The four-vertex cross-section [orig: @ 0x5DC4F6..0x5DC796]: the outer
		// pair at +-rs in the base colour, the inner pair at +-0.4 rs in the ramp
		// colour, two scrolling texture layers.
		const float phase = static_cast<float>(
				tick * static_cast<double>(style.wave_t) * static_cast<double>(0.0004f));
		const float u_offset = w * 0.1f + phase;
		const float u_step = style.wave_u * 0.3f;
		const float u1_step = style.wave_u * 0.11f;
		const float u_i = static_cast<float>(i) * u_step;
		const float a = 0.2f * size_w * style.wave_v;
		const float b = 0.2f * w * style.wave_v;
		// The texture wobble: 0.1 x cos^4 of the segment against the eye ray,
		// signed away from the eye [orig: @ 0x5DC558..0x5DC58D].
		const float cosine = dot(d_hat, a_hat);
		const float c4 = cosine * cosine * cosine * cosine;
		const float v_offset = 0.1f * (cosine > 0.0f ? -c4 : c4);
		const float u_outer = u_i + u_offset;
		const float u_inner = v_offset + u_offset + u_i;
		const float u1 = static_cast<float>(i) * u1_step + phase;
		TracerVertex v0 = vertex_at({p.x + rs.x, p.y + rs.y, p.z + rs.z}, outer_argb);
		v0.u0 = u_outer;
		v0.v0 = -a;
		v0.u1 = u1;
		v0.v1 = -b;
		TracerVertex v1 = vertex_at(
				{p.x + 0.4f * rs.x, p.y + 0.4f * rs.y, p.z + 0.4f * rs.z}, argb);
		v1.u0 = u_inner;
		v1.v0 = -0.4f * a;
		v1.u1 = u1;
		v1.v1 = -0.4f * b;
		TracerVertex v2 = vertex_at(
				{p.x - 0.4f * rs.x, p.y - 0.4f * rs.y, p.z - 0.4f * rs.z}, argb);
		v2.u0 = u_inner;
		v2.v0 = 0.4f * a;
		v2.u1 = u1;
		v2.v1 = 0.4f * b;
		TracerVertex v3 = vertex_at({p.x - rs.x, p.y - rs.y, p.z - rs.z}, outer_argb);
		v3.u0 = u_outer;
		v3.v0 = a;
		v3.u1 = u1;
		v3.v1 = b;
		out.vertices.push_back(v0);
		out.vertices.push_back(v1);
		out.vertices.push_back(v2);
		out.vertices.push_back(v3);
		if (i < count - 2) {
			// Six triangles bridge this cross-section to the next
			// [orig: @ 0x5DC6FF..0x5DC793].
			const std::uint32_t base = first_vertex + static_cast<std::uint32_t>(4 * i);
			const std::uint32_t tri[18] = {base, base + 1, base + 4, base + 1, base + 5,
					base + 4, base + 1, base + 2, base + 5, base + 2, base + 6, base + 5,
					base + 2, base + 3, base + 6, base + 3, base + 7, base + 6};
			out.indices.insert(out.indices.end(), tri, tri + 18);
		}
	}
	const std::uint32_t vertex_count =
			static_cast<std::uint32_t>(out.vertices.size()) - first_vertex;
	if (style.cross_section) {
		// The indexed list draws from eight vertices up; a single cross-section
		// (four collinear vertices, no indices) is the retail strip fallback and
		// covers no area [orig: @ 0x5DC893..0x5DC8CE].
		if (vertex_count < 8) {
			out.vertices.resize(first_vertex);
			out.indices.resize(first_index);
			return;
		}
	} else {
		// The strip draws from four vertices up [orig: @ 0x5DC8CB..0x5DC8D8];
		// expand it to its triangle list.
		if (vertex_count < 4) {
			out.vertices.resize(first_vertex);
			return;
		}
		for (std::uint32_t k = 0; k + 2 < vertex_count; ++k) {
			const std::uint32_t v = first_vertex + k;
			out.indices.push_back(v);
			out.indices.push_back(v + 1);
			out.indices.push_back(v + 2);
		}
	}
	TracerDraw draw;
	draw.shader = shader;
	draw.fog_black = fog_black;
	draw.first_vertex = first_vertex;
	draw.vertex_count = vertex_count;
	draw.first_index = first_index;
	draw.index_count = static_cast<std::uint32_t>(out.indices.size()) - first_index;
	out.draws.push_back(draw);
}

} // namespace

const TracerStyleView &tracer_style(int style_id) {
	const TracerStyleTables &t = tables();
	if (style_id >= 1 && style_id <= 12) {
		return t.styles[static_cast<std::size_t>(style_id)];
	}
	return t.styles[0];
}

void compile_tracer_ribbons(const TracerChannelInput *channels, std::size_t count,
		const TracerView &view, TracerPass pass, TracerRibbonFrame &out) {
	out.clear();
	if (channels == nullptr) {
		return;
	}
	for (std::size_t i = 0; i < count; ++i) {
		const TracerChannelInput &c = channels[i];
		// A channel draws only with at least one point [orig: `test ebx, ebx /
		// jle` @ 0x5DC0D8..0x5DC0FE].
		if (c.count <= 0 || c.points == nullptr) {
			continue;
		}
		const TracerStyleView &style = tracer_style(c.style_id);
		if (pass == TracerPass::Distortion) {
			// Only the +0x828 styles, through the distortion material, scene fog
			// [orig: CEffectEmitterPool_RenderDistortionPass @ 0x5DCB58;
			// SetFogAndBlendMode(0) @ 0x5DC0A6].
			if (!style.distortion) {
				continue;
			}
			++out.channels;
			append_run(c.points, c.count, c.age, style, view, pass,
					TracerShader::Distortion, false, out);
			continue;
		}
		++out.channels;
		append_run(c.points, c.count, c.age, style, view, pass, style.shader,
				style.fog_black, out);
	}
}

int tracer_rung(bool camera_above_water) {
	return camera_above_water ? kRungTracerCameraSide : kRungTracerFarSide;
}

void append_tracer_beam(const float *points, int count, int style_id,
		const TracerView &view, TracerRibbonFrame &out) {
	if (points == nullptr || count <= 0) {
		return;
	}
	const TracerStyleView &style = tracer_style(style_id);
	// A run shorter than the ramp is anchored to the ramp's end
	// [orig: sub eax, ebp @ 0x5DCC34].
	const int age = count < style.color_count ? style.color_count - count : 0;
	++out.channels;
	append_run(points, count, age, style, view, TracerPass::Main, style.shader,
			style.fog_black, out);
}

}  // namespace opennova::renderer
