#pragma once

// A D3D triangle strip as the triangle list it draws. Triangle t of a strip
// is (v[t], v[t + 1], v[t + 2]) with the first two swapped on odd t, so every
// triangle keeps the strip's facing (D3DPT_TRIANGLESTRIP's winding rule); a
// triangle with two equal indices, the stitch joining two runs inside one
// strip, draws nothing and is dropped. The terrain tiles' strip LODs and the
// water strip batches unroll through it.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// Appends the list of `count` strip indices to `out`, each offset by `base`.
inline void triangle_strip_to_list(const std::uint16_t *strip, std::size_t count,
		std::uint32_t base, std::vector<std::uint32_t> &out) {
	for (std::size_t i = 2; i < count; ++i) {
		const std::uint16_t a = strip[i - 2];
		const std::uint16_t b = strip[i - 1];
		const std::uint16_t c = strip[i];
		if (a == b || b == c || a == c) {
			continue;
		}
		if ((i & 1) != 0) {
			out.push_back(base + b);
			out.push_back(base + a);
		} else {
			out.push_back(base + a);
			out.push_back(base + b);
		}
		out.push_back(base + c);
	}
}

inline void triangle_strip_to_list(const std::vector<std::uint16_t> &strip, std::uint32_t base,
		std::vector<std::uint32_t> &out) {
	triangle_strip_to_list(strip.data(), strip.size(), base, out);
}

} // namespace opennova::renderer
