#include <terrain/texture_preprocess.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace opennova::terrain {

namespace {

bool is_power_of_two(uint32_t value) noexcept {
	return value != 0 && (value & (value - 1)) == 0;
}

uint8_t encode_coefficient(double value) noexcept {
	const int encoded = static_cast<int>((value + 1.0) * 127.5);
	return static_cast<uint8_t>(std::clamp(encoded, 0, 255));
}

Rgba8Image downsample_2x2(const Rgba8Image &source) {
	Rgba8Image result;
	result.width = source.width >> 1;
	result.height = source.height >> 1;
	result.pixels.resize(static_cast<size_t>(result.width) * result.height * 4);

	for (uint32_t y = 0; y < result.height; ++y) {
		for (uint32_t x = 0; x < result.width; ++x) {
			const size_t dst = (static_cast<size_t>(y) * result.width + x) * 4;
			const size_t tl = (static_cast<size_t>(y * 2) * source.width + x * 2) * 4;
			const size_t tr = tl + 4;
			const size_t bl = tl + static_cast<size_t>(source.width) * 4;
			const size_t br = bl + 4;
			for (size_t channel = 0; channel < 4; ++channel) {
				const uint32_t sum = source.pixels[tl + channel] +
					source.pixels[tr + channel] + source.pixels[bl + channel] +
					source.pixels[br + channel];
				result.pixels[dst + channel] = static_cast<uint8_t>(sum >> 2);
			}
		}
	}
	return result;
}

} // namespace

bool Rgba8Image::is_valid() const noexcept {
	return width > 0 && height > 0 &&
		pixels.size() == static_cast<size_t>(width) * height * 4;
}

Rgba8Image build_detail_coefficient_map(const Rgba8Image &detailmap) {
	if (!detailmap.is_valid() || !is_power_of_two(detailmap.width) ||
			!is_power_of_two(detailmap.height)) {
		return {};
	}

	Rgba8Image result{detailmap.width, detailmap.height, {}};
	result.pixels.resize(detailmap.pixels.size());
	const uint32_t x_mask = detailmap.width - 1;
	const uint32_t y_mask = detailmap.height - 1;
	constexpr double scale = 1.0 / 32.0;

	for (uint32_t y = 0; y < detailmap.height; ++y) {
		for (uint32_t x = 0; x < detailmap.width; ++x) {
			auto blue_at = [&](uint32_t sample_x, uint32_t sample_y) {
				const size_t offset =
					(static_cast<size_t>(sample_y) * detailmap.width + sample_x) * 4;
				return static_cast<double>(detailmap.pixels[offset + 2]);
			};

			// The source is packed A8R8G8B8 in retail, so its low byte is the
			// authored blue channel. Both axes wrap by dimension-1 masks.
			// [orig: Texture_GenerateNormalMap @ 0x58c225..0x58c3d9]
			const double nx = scale *
				(blue_at((x - 1) & x_mask, y) - blue_at((x + 1) & x_mask, y));
			const double ny = scale *
				(blue_at(x, (y - 1) & y_mask) - blue_at(x, (y + 1) & y_mask));
			// Retail normalizes the paired one-sided diffs against the fld1
			// unit Z kept on the FPU stack, not 2.0.
			// [orig: Texture_GenerateNormalMap @ 0x58c1fa (fld1), 0x58c26d..
			// 0x58c2b0 (paired diffs x bumpScale = 1/32 @ 0x7DBFAC)]
			constexpr double nz = 1.0;
			const double inv_length = 1.0 / std::sqrt(nx * nx + ny * ny + nz * nz);

			const size_t dst = (static_cast<size_t>(y) * detailmap.width + x) * 4;
			result.pixels[dst] = encode_coefficient(nx * inv_length);
			result.pixels[dst + 1] = encode_coefficient(ny * inv_length);
			result.pixels[dst + 2] = encode_coefficient(nz * inv_length);
			result.pixels[dst + 3] = detailmap.pixels[dst + 3];
		}
	}
	return result;
}

Rgba8Image build_heightfield_normal_map(
		const std::vector<uint16_t> &depth,
		uint32_t width,
		uint32_t height,
		const TerrainQuadrantLocks &locks) {
	if (!is_power_of_two(width) || !is_power_of_two(height) ||
			width < 2 || height < 2 ||
			depth.size() != static_cast<size_t>(width) * height) {
		return {};
	}

	Rgba8Image result{width, height, {}};
	result.pixels.resize(static_cast<size_t>(width) * height * 4);
	const uint32_t x_mask = width - 1;
	const uint32_t y_mask = height - 1;
	const uint32_t quadrant_width = width >> 1;
	const uint32_t quadrant_height = height >> 1;
	const uint32_t quadrant_x_mask = quadrant_width - 1;
	const uint32_t quadrant_y_mask = quadrant_height - 1;
	constexpr double height_scale = 1.0 / 256.0;

	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			const uint32_t quadrant_x = x >= quadrant_width ? 1u : 0u;
			const uint32_t quadrant_y = y >= quadrant_height ? 1u : 0u;
			const TerrainLockCoord &lock = locks[quadrant_x + quadrant_y * 2u];
			const uint32_t x_offset = quadrant_x * quadrant_width;
			const uint32_t y_offset = quadrant_y * quadrant_height;

			const auto resolve_x = [&](uint32_t sample_x) {
				return lock.x != 0
					? x_offset + (sample_x & quadrant_x_mask)
					: sample_x & x_mask;
			};
			const auto resolve_y = [&](uint32_t sample_y) {
				return lock.y != 0
					? y_offset + (sample_y & quadrant_y_mask)
					: sample_y & y_mask;
			};
			const auto height_at = [&](uint32_t sample_x, uint32_t sample_y) {
				return static_cast<double>(
					depth[static_cast<size_t>(sample_y) * width + sample_x]) *
					height_scale;
			};
			const double nx =
				height_at(resolve_x(x - 1), y) -
				height_at(resolve_x(x + 1), y);
			const double ny =
				height_at(x, resolve_y(y - 1)) -
				height_at(x, resolve_y(y + 1));
			// Retail's third component is the fld1 unit Z (@ 0x603248), giving
			// twice the slope response of a nz=2 normalization.
			// [orig: Terrain_GenerateNormalMap @ 0x603210; diff scale 1/256
			// @ 0x7C6950; encode 127.5 @ 0x7D8B48; alpha 0x80 @ 0x6034eb]
			constexpr double nz = 1.0;
			const double inverse_length =
				1.0 / std::sqrt(nx * nx + ny * ny + nz * nz);

			const size_t dst = (static_cast<size_t>(y) * width + x) * 4;
			result.pixels[dst] = encode_coefficient(nx * inverse_length);
			result.pixels[dst + 1] = encode_coefficient(ny * inverse_length);
			result.pixels[dst + 2] = encode_coefficient(nz * inverse_length);
			result.pixels[dst + 3] = 128;
		}
	}
	return result;
}

Rgba8Image normalize_detail_blend_map(const Rgba8Image &blendmap) {
	if (!blendmap.is_valid()) {
		return {};
	}

	Rgba8Image result = blendmap;
	for (size_t offset = 0; offset < result.pixels.size(); offset += 4) {
		const uint32_t sum = static_cast<uint32_t>(blendmap.pixels[offset]) +
			blendmap.pixels[offset + 1] + blendmap.pixels[offset + 2];
		if (sum == 0) {
			result.pixels[offset] = 255;
			result.pixels[offset + 1] = 0;
			result.pixels[offset + 2] = 0;
			continue;
		}

		// Retail first truncates 0xffff/sum, then truncates each scaled
		// channel again after >>8. This is intentionally not float normalize.
		// [orig: PolyTrn_InitTextures @ 0x60b216..0x60b24d]
		const uint32_t coefficient = 0xffffu / sum;
		for (size_t channel = 0; channel < 3; ++channel) {
			result.pixels[offset + channel] = static_cast<uint8_t>(
				(coefficient * blendmap.pixels[offset + channel]) >> 8);
		}
	}
	return result;
}

std::vector<Rgba8Image> build_paired_detail_mip_chain(
		const Rgba8Image &base,
		const Rgba8Image &far_detail) {
	if (!base.is_valid() || !far_detail.is_valid() ||
			base.width != far_detail.width || base.height != far_detail.height ||
			!is_power_of_two(base.width) || !is_power_of_two(base.height)) {
		return {};
	}

	uint32_t level_count = 0;
	for (uint32_t minimum_dimension = std::min(base.width, base.height);
			minimum_dimension > 2; minimum_dimension >>= 1) {
		++level_count;
	}
	if (level_count == 0) {
		return {};
	}

	std::vector<Rgba8Image> result;
	result.reserve(level_count);
	Rgba8Image base_level = base;
	Rgba8Image far_level = far_detail;
	for (uint32_t level = 0; level < level_count; ++level) {
		const uint32_t far_weight = std::min(256u, (320u * level) / level_count);
		const uint32_t base_weight = 256u - far_weight;
		Rgba8Image output = base_level;
		for (size_t offset = 0; offset < output.pixels.size(); offset += 4) {
			for (size_t channel = 0; channel < 3; ++channel) {
				output.pixels[offset + channel] = static_cast<uint8_t>(
					(far_weight * far_level.pixels[offset + channel] +
						base_weight * base_level.pixels[offset + channel]) >> 8);
			}
			// Alpha remains the independently downsampled base alpha.
			output.pixels[offset + 3] = base_level.pixels[offset + 3];
		}
		result.push_back(std::move(output));

		if (level + 1 < level_count) {
			base_level = downsample_2x2(base_level);
			far_level = downsample_2x2(far_level);
		}
	}
	return result;
}

} // namespace opennova::terrain
