#include "renderer/tracer_frame.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace renderer {
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

		auto view = [](const std::uint32_t *colors, int color_count,
							const float *sizes, int size_count,
							std::uint32_t base, bool additive) {
			TracerStyleView v;
			v.colors = colors;
			v.color_count = color_count;
			v.sizes = sizes;
			v.size_count = size_count;
			v.base_argb = base;
			v.additive = additive;
			return v;
		};
		// index 0 = the fallback (stdred — the CEffectChannel_Init default case
		// @ 0x5db1c8).
		styles[0] = view(stdred_colors.data(), 12, std_sizes.data(), 1, 0, true);
		styles[1] = styles[0];
		styles[2] = view(stdgreen_colors.data(), 12, std_sizes.data(), 1, 0, true);
		styles[3] = view(smoke_colors.data(), 112, rocket_sizes.data(), 32,
				0xC0C0C0u, false);
		styles[4] = view(smoke_colors.data(), 112, at4_sizes.data(), 32,
				0xC0C0C0u, false);
		styles[5] = view(grenade_colors.data(), 64, grenade_sizes.data(), 32,
				0xC0C0C0u, false);
		styles[6] = view(rapidred_colors.data(), 6, std_sizes.data(), 1, 0, true);
		styles[7] = view(rapidgreen_colors.data(), 6, std_sizes.data(), 1, 0, true);
		styles[8] = view(nvg_colors.data(), 32, nvg_sizes.data(), 1, 0xC04040u, true);
		styles[9] = view(sniper_red.data(), 20, sniper_sizes.data(), 10, 0, true);
		styles[10] = view(sniper_green.data(), 20, sniper_sizes.data(), 10, 0, true);
		styles[11] = view(df1_red.data(), 32, df1_sizes.data(), 1, 0, true);
		styles[12] = view(df1_green.data(), 32, df1_sizes.data(), 1, 0, true);
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

inline float length_squared(const TracerVec3 &v) {
	return v.x * v.x + v.y * v.y + v.z * v.z;
}

// Push one vertex: position +/- right * half_w with the style color.
void push_vertex(std::vector<float> &sink, const TracerVec3 &pos, float r,
		float g, float b, float a) {
	sink.push_back(pos.x);
	sink.push_back(pos.y);
	sink.push_back(pos.z);
	sink.push_back(r);
	sink.push_back(g);
	sink.push_back(b);
	sink.push_back(a);
}

void append_channel_ribbon(std::vector<float> &sink, const TracerChannelInput &c,
		const TracerStyleView &style, const TracerVec3 &camera) {
	if (c.count < 2 || c.points == nullptr) {
		return;
	}
	constexpr float kByteToUnit = 1.0f / 255.0f;
	TracerVec3 last_right{};
	bool first = true;
	for (int pi = 0; pi < c.count - 1; ++pi) {
		const float *row = c.points + static_cast<std::size_t>(pi) * 4;
		const TracerVec3 p{row[0], row[1], row[2]};
		const float jitter = row[3];
		const TracerVec3 nxt{row[4], row[5], row[6]};
		// Ramp index [orig: (count - i) + age - 1, clamped per table @ 0x5db8a0].
		const int idx = c.count - pi + c.age - 1;
		const int color_idx = std::clamp(idx, 0, style.color_count - 1);
		const int size_idx = std::clamp(idx, 0, style.size_count - 1);
		// The oldest pair takes the style base color [orig: i == 0 -> desc+0x14].
		const std::uint32_t argb = pi == 0
				? style.base_argb
				: style.colors[static_cast<std::size_t>(color_idx)];
		const TracerVec3 dir = sub(nxt, p);
		TracerVec3 right = last_right;
		if (length_squared(dir) > 0.000001f) {
			const TracerVec3 raw = cross(dir, sub(camera, p));
			const float len_sq = length_squared(raw);
			if (len_sq > 0.000001f) {
				const float inv = 1.0f / std::sqrt(len_sq);
				right = {raw.x * inv, raw.y * inv, raw.z * inv};
			}
			// else: duplicate points / collinear camera keep the last facing.
		}
		last_right = right;
		const float dist = std::sqrt(length_squared(sub(p, camera)));
		const float half_w = std::max(
				style.sizes[static_cast<std::size_t>(size_idx)] * jitter,
				dist * kTracerMinWidthPerDist);
		// Additive styles render ONE:ONE in retail (alpha byte unused — the std
		// ramps author alpha 0), so alpha rides at 1 under a SRCALPHA:ONE add;
		// smoke styles carry their authored alpha fade.
		const float r = static_cast<float>((argb >> 16) & 0xFFu) * kByteToUnit;
		const float g = static_cast<float>((argb >> 8) & 0xFFu) * kByteToUnit;
		const float b = static_cast<float>(argb & 0xFFu) * kByteToUnit;
		const float a = style.additive
				? 1.0f
				: static_cast<float>((argb >> 24) & 0xFFu) * kByteToUnit;
		const TracerVec3 va{p.x + right.x * half_w, p.y + right.y * half_w,
				p.z + right.z * half_w};
		const TracerVec3 vb{p.x - right.x * half_w, p.y - right.y * half_w,
				p.z - right.z * half_w};
		if (first && !sink.empty()) {
			// Degenerate join from the previous channel's last vertex.
			const std::size_t prev = sink.size() - 7;
			for (std::size_t k = 0; k < 7; ++k) {
				sink.push_back(sink[prev + k]);
			}
			push_vertex(sink, va, r, g, b, a);
		}
		first = false;
		push_vertex(sink, va, r, g, b, a);
		push_vertex(sink, vb, r, g, b, a);
	}
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
		const TracerVec3 &camera, TracerRibbonFrame &out) {
	out.clear();
	if (channels == nullptr) {
		return;
	}
	for (std::size_t i = 0; i < count; ++i) {
		const TracerChannelInput &c = channels[i];
		if (c.count <= 0) {
			continue;
		}
		++out.channels;
		const TracerStyleView &style = tracer_style(c.style_id);
		append_channel_ribbon(style.additive ? out.additive : out.alpha, c, style,
				camera);
	}
}

} // namespace renderer
