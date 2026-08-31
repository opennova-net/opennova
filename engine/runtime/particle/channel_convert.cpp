#include <runtime/particle/channel_convert.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace opennova::particle {
namespace {

std::uint8_t encode_normal(float value) {
	const int encoded = static_cast<int>((value + 1.0f) * 127.5f);
	return static_cast<std::uint8_t>(std::clamp(encoded, 0, 255));
}

} // namespace

// [orig: Texture_GenerateNormalMapFromHeight @ 0x5e2df0]. The blue byte is
// the height field; neighbor reads wrap with the power-of-two masks. The
// gradient sign pair is the witnessed operand order (b(x-1)-b(x+1),
// b(x,y+1)-b(x,y-1)).
void convert_height_to_normal_map(std::uint8_t *rgba, int width, int height,
		float scale, bool force_blue) {
	if (rgba == nullptr || width <= 0 || height <= 0)
		return;
	const std::vector<std::uint8_t> source(rgba,
			rgba + static_cast<std::size_t>(width) *
					static_cast<std::size_t>(height) * 4u);
	auto blue = [&](int x, int y) -> float {
		const int wrapped_x = static_cast<int>(
				static_cast<unsigned int>(x) &
				static_cast<unsigned int>(width - 1));
		const int wrapped_y = static_cast<int>(
				static_cast<unsigned int>(y) &
				static_cast<unsigned int>(height - 1));
		const std::size_t offset = static_cast<std::size_t>(
				(wrapped_y * width + wrapped_x) * 4 + 2);
		return static_cast<float>(source[offset]);
	};
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			float nx = (blue(x - 1, y) - blue(x + 1, y)) * scale;
			float ny = (blue(x, y + 1) - blue(x, y - 1)) * scale;
			float nz = 2.0f;
			const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (length > 0.0f) {
				nx /= length;
				ny /= length;
				nz /= length;
			}
			const std::size_t offset = static_cast<std::size_t>(
					(y * width + x) * 4);
			rgba[offset + 0] = encode_normal(nx);
			rgba[offset + 1] = encode_normal(ny);
			rgba[offset + 2] = force_blue ? 255 : encode_normal(nz);
			// Alpha intentionally remains the pre-conversion byte.
		}
	}
}

void clear_alpha_channel(std::uint8_t *rgba, int width, int height) {
	if (rgba == nullptr || width <= 0 || height <= 0)
		return;
	const std::size_t pixels = static_cast<std::size_t>(width) *
			static_cast<std::size_t>(height);
	for (std::size_t i = 0; i < pixels; ++i)
		rgba[i * 4 + 3] = 0;
}

} // namespace opennova::particle
