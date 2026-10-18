#include <runtime/terrain/texture_preprocess.h>
#include <base/io/bits.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/renderer/device_texture.h>
#include <runtime/renderer/texture_compression.h>
#include <runtime/renderer/texture_load_rules.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace opennova::terrain {

namespace {

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
	if (!detailmap.is_valid() || !io::is_power_of_two(detailmap.width) ||
			!io::is_power_of_two(detailmap.height)) {
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
			// [orig: Texture_GenerateNormalMap @ 0x58c225..0x58c3d9;
			// bump scale 1/32 @0x7DBFAC]
			const double nx = scale *
				(blue_at((x - 1) & x_mask, y) - blue_at((x + 1) & x_mask, y));
			const double ny = scale *
				(blue_at(x, (y - 1) & y_mask) - blue_at(x, (y + 1) & y_mask));
			// Each one-sided cross product contributes unit Z; the two are
			// added before normalization. [orig: Texture_GenerateNormalMap
			// @0x58C070, fld1 @0x58C1FA, fadd st(3) @0x58C305]
			constexpr double nz = 2.0;
			const double inv_length = 1.0 / std::sqrt(nx * nx + ny * ny + nz * nz);

			const size_t dst = (static_cast<size_t>(y) * detailmap.width + x) * 4;
			result.pixels[dst] = encode_coefficient(nx * inv_length);
			result.pixels[dst + 1] = encode_coefficient(static_cast<float>(ny * inv_length));
			result.pixels[dst + 2] = encode_coefficient(nz * inv_length);
			result.pixels[dst + 3] = detailmap.pixels[dst + 3];
		}
	}
	return result;
}

namespace {

// One texel of the heightfield normal atlas: its four taps resolved by its
// quadrant's locks (a locked component wraps within the quadrant, an unlocked
// one across the whole atlas), the slope encoded {slope-x, slope-y, up} with
// the constant alpha.
struct HeightfieldNormalKernel {
	const std::vector<uint16_t> &depth;
	uint32_t width;
	uint32_t height;
	const TerrainQuadrantLocks &locks;

	const TerrainLockCoord &lock_at(uint32_t x, uint32_t y) const {
		const uint32_t quadrant_x = x >= (width >> 1) ? 1u : 0u;
		const uint32_t quadrant_y = y >= (height >> 1) ? 1u : 0u;
		return locks[quadrant_x + quadrant_y * 2u];
	}
	uint32_t resolve_x(uint32_t x, uint32_t y, uint32_t sample_x) const {
		const uint32_t quadrant_width = width >> 1;
		const uint32_t x_offset = (x >= quadrant_width ? 1u : 0u) * quadrant_width;
		return lock_at(x, y).x != 0
			? x_offset + (sample_x & (quadrant_width - 1))
			: sample_x & (width - 1);
	}
	uint32_t resolve_y(uint32_t x, uint32_t y, uint32_t sample_y) const {
		const uint32_t quadrant_height = height >> 1;
		const uint32_t y_offset = (y >= quadrant_height ? 1u : 0u) * quadrant_height;
		return lock_at(x, y).y != 0
			? y_offset + (sample_y & (quadrant_height - 1))
			: sample_y & (height - 1);
	}
	void write(uint32_t x, uint32_t y, uint8_t *pixels) const {
		// One recovered kernel now serves both this texture atlas and the
		// WAC runtime surface-normal lookup, preventing either copy from
		// drifting in scale, axis order, or normalization.
		// [orig: Terrain_GenerateNormalMap @ 0x603210; diff scale 1/256
		// @ 0x7C6950; encode 127.5 @ 0x7D8B48; alpha 0x80 @ 0x6034eb]
		const TerrainSurfaceNormal normal = height_field_normal_from_raw16(
				depth[static_cast<size_t>(y) * width + resolve_x(x, y, x - 1)],
				depth[static_cast<size_t>(y) * width + resolve_x(x, y, x + 1)],
				depth[static_cast<size_t>(resolve_y(x, y, y - 1)) * width + x],
				depth[static_cast<size_t>(resolve_y(x, y, y + 1)) * width + x]);

		const size_t dst = (static_cast<size_t>(y) * width + x) * 4;
		pixels[dst] = encode_coefficient(normal.x);
		pixels[dst + 1] = encode_coefficient(normal.z);
		pixels[dst + 2] = encode_coefficient(normal.up);
		pixels[dst + 3] = 128;
	}
};

} // namespace

Rgba8Image build_heightfield_normal_map(
		const std::vector<uint16_t> &depth,
		uint32_t width,
		uint32_t height,
		const TerrainQuadrantLocks &locks) {
	if (!io::is_power_of_two(width) || !io::is_power_of_two(height) ||
			width < 2 || height < 2 ||
			depth.size() != static_cast<size_t>(width) * height) {
		return {};
	}

	Rgba8Image result{width, height, {}};
	result.pixels.resize(static_cast<size_t>(width) * height * 4);
	const HeightfieldNormalKernel kernel{depth, width, height, locks};
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			kernel.write(x, y, result.pixels.data());
		}
	}
	return result;
}

bool refresh_heightfield_normal_map(
		Rgba8Image &normals,
		const std::vector<uint16_t> &depth,
		uint32_t width,
		uint32_t height,
		const TerrainQuadrantLocks &locks,
		const terrain::CoordsRect &rect,
		std::vector<terrain::CoordsRect> *written) {
	if (written != nullptr) written->clear();
	if (!io::is_power_of_two(width) || !io::is_power_of_two(height) ||
			width < 2 || height < 2 ||
			depth.size() != static_cast<size_t>(width) * height ||
			normals.width != width || normals.height != height ||
			normals.pixels.size() != static_cast<size_t>(width) * height * 4) {
		return false;
	}
	if (rect.w <= 0 || rect.h <= 0) return true;
	if (rect.x < 0 || rect.z < 0 || static_cast<int64_t>(rect.x) + rect.w > width ||
			static_cast<int64_t>(rect.z) + rect.h > height) {
		return false;
	}
	const int64_t x0 = rect.x, z0 = rect.z;
	const int64_t x1 = static_cast<int64_t>(rect.x) + rect.w, z1 = static_cast<int64_t>(rect.z) + rect.h;
	const auto column_in = [&](uint32_t v) { return v >= x0 && v < x1; };
	const auto row_in = [&](uint32_t v) { return v >= z0 && v < z1; };
	const HeightfieldNormalKernel kernel{depth, width, height, locks};
	// The boxes of the texels written, [x0, x1) x [z0, z1) each inside one quadrant: a texel joins a box of its
	// quadrant it touches (or is inside), else opens one; touching boxes merge at the end.
	struct Box {
		uint32_t quadrant, x0, z0, x1, z1;
	};
	std::vector<Box> boxes;
	const auto write = [&](uint32_t x, uint32_t y) {
		kernel.write(x, y, normals.pixels.data());
		if (written == nullptr) return;
		const uint32_t quadrant = (x >= (width >> 1) ? 1u : 0u) + (y >= (height >> 1) ? 2u : 0u);
		for (Box &box : boxes) {
			if (box.quadrant != quadrant || x + 1 < box.x0 || x > box.x1 || y + 1 < box.z0 || y > box.z1) continue;
			box.x0 = std::min(box.x0, x);
			box.z0 = std::min(box.z0, y);
			box.x1 = std::max(box.x1, x + 1);
			box.z1 = std::max(box.z1, y + 1);
			return;
		}
		boxes.push_back(Box{quadrant, x, y, x + 1, y + 1});
	};
	// The rect's rows: a texel whose left or right tap is one of its columns.
	for (uint32_t y = static_cast<uint32_t>(z0); y < static_cast<uint32_t>(z1); ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			if (column_in(kernel.resolve_x(x, y, x - 1)) || column_in(kernel.resolve_x(x, y, x + 1))) {
				write(x, y);
			}
		}
	}
	// The rect's columns: a texel whose upper or lower tap is one of its rows.
	for (uint32_t x = static_cast<uint32_t>(x0); x < static_cast<uint32_t>(x1); ++x) {
		for (uint32_t y = 0; y < height; ++y) {
			if (row_in(kernel.resolve_y(x, y, y - 1)) || row_in(kernel.resolve_y(x, y, y + 1))) {
				write(x, y);
			}
		}
	}
	if (written != nullptr) {
		// Boxes of a quadrant that touch merge, until none do.
		for (bool merged = true; merged;) {
			merged = false;
			for (size_t i = 0; i < boxes.size() && !merged; ++i)
				for (size_t j = i + 1; j < boxes.size() && !merged; ++j) {
					Box &a = boxes[i];
					const Box &b = boxes[j];
					if (a.quadrant != b.quadrant || b.x0 > a.x1 || a.x0 > b.x1 || b.z0 > a.z1 || a.z0 > b.z1) continue;
					a.x0 = std::min(a.x0, b.x0);
					a.z0 = std::min(a.z0, b.z0);
					a.x1 = std::max(a.x1, b.x1);
					a.z1 = std::max(a.z1, b.z1);
					boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(j));
					merged = true;
				}
		}
		for (const Box &box : boxes)
			written->push_back(terrain::CoordsRect{static_cast<int32_t>(box.x0), static_cast<int32_t>(box.z0),
					static_cast<int32_t>(box.x1 - box.x0), static_cast<int32_t>(box.z1 - box.z0)});
	}
	return true;
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
			!io::is_power_of_two(base.width) || !io::is_power_of_two(base.height)) {
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

std::vector<renderer::DxtSurface> build_detail_layer_levels(
		const Rgba8Image &base,
		const Rgba8Image *far_detail,
		int32_t session_texcompression_level,
		int32_t terrain_texdetail) {
	// The session's compression word, the terrain detail's halving word and 8.
	// [orig: PolyTrn_InitTextures @ 0x60AB95 (g_TerrainDetailTextureFlags),
	//  @ 0x60AB9B..0x60ABBC (0x400100 / 0x400200), @ 0x60ABA0 (| 8), @ 0x60ABE5]
	const uint32_t kDetailLayerFlags =
			renderer::terrain_detail_layer_compression_flags(session_texcompression_level) | 8u |
			renderer::terrain_texdetail_flags(terrain_texdetail);
	const renderer::TextureDxtFormat format = renderer::select_texture_dxt_format(
			kDetailLayerFlags, renderer::kReferenceTextureDxtCaps);
	std::vector<renderer::DxtSurface> levels;
	if (!base.is_valid() || format == renderer::TextureDxtFormat::None) return levels;
	// Either creator halves its pixels first as the word asks, the far with the
	// base [orig: GTexture_DownsampleToLimits @ 0x687170, from
	// GTexture_CreateFromPixelData_0 @ 0x687785 and from
	// GTexture_CreateFromPixelDataWithAlphaBlend @ 0x68735B (the base) and
	// @ 0x68736D (the far)].
	const auto halved = [kDetailLayerFlags](const Rgba8Image &image) {
		Rgba8Image out = image;
		renderer::halve_rgba_times(out.pixels, out.width, out.height,
				renderer::pixel_texture_halvings(out.width, out.height, kDetailLayerFlags));
		return out;
	};
	if (far_detail != nullptr) {
		for (const Rgba8Image &level : build_paired_detail_mip_chain(halved(base), halved(*far_detail))) {
			levels.push_back(renderer::encode_dxt_surface(
					renderer::decode_rgba8(level.pixels.data(), level.width, level.height),
					level.width, level.height, format));
		}
		return levels;
	}
	// The plain creator: its halvings, count, level-0 load and box chain
	// (renderer::pixel_device_texture_levels) [orig: GTexture_CreateFromPixelData_0 @ 0x6876C0].
	renderer::pixel_device_texture_levels(base.pixels.data(), base.width, base.height, kDetailLayerFlags,
			renderer::kReferenceTextureDxtCaps, 0, renderer::kReferenceMaxTextureSide, &levels);
	return levels;
}

// [orig: GTexture_CreateFromPixelDataWithAlphaBlend @0x687270 — the custom mip builder stops
//  after the 4x4 level, each level a GTexture_Downsample2x2_RGBA8 @0x687000 box reduction]
std::vector<Rgba8Image> build_box_mip_chain_to_4x4(
		const Rgba8Image &base) {
	if (!base.is_valid() || !io::is_power_of_two(base.width) ||
			!io::is_power_of_two(base.height)) {
		return {};
	}
	std::vector<Rgba8Image> result;
	Rgba8Image level = base;
	for (uint32_t minimum_dimension = std::min(base.width, base.height);
			minimum_dimension > 2; minimum_dimension >>= 1) {
		result.push_back(level);
		if ((minimum_dimension >> 1) > 2) {
			level = downsample_2x2(level);
		}
	}
	return result;
}

std::vector<uint8_t> build_depthspin_rg8(const std::vector<uint16_t> &depth, float water_height_wu) {
	constexpr int kSourcePx = 1024;
	constexpr int kMaskPx = 256;
	constexpr int kStep = 4;
	constexpr int kHalfStep = 2;
	if (depth.size() != static_cast<size_t>(kSourcePx) * kSourcePx || !std::isfinite(water_height_wu))
		return {};
	const int water_int = std::clamp(static_cast<int>(std::floor(water_height_wu)), 0, 254);
	if (water_int <= 0) return {};
	std::vector<uint8_t> out(static_cast<size_t>(kMaskPx) * kMaskPx * 2);
	const uint16_t *height = depth.data();
	// [orig: PolyTrn_InitTextures @ 0x60BA30..0x60BAF3 — the four movzx taps at +0, +4, +0x1000,
	// +0x1004 bytes, `shl ebx, 0Eh; or ebx, 0FFFFFFh`, unrolled four texels to 0x20 source bytes
	// @ 0x60BAF6; 0x2000 bytes (four source rows) a texel row @ 0x60BB05]
	for (int z = 0; z < kMaskPx; ++z) {
		const int source_z = z * kStep;
		for (int x = 0; x < kMaskPx; ++x) {
			const int source_x = x * kStep;
			const uint32_t sum = height[source_z * kSourcePx + source_x] +
					height[source_z * kSourcePx + source_x + kHalfStep] +
					height[(source_z + kHalfStep) * kSourcePx + source_x] +
					height[(source_z + kHalfStep) * kSourcePx + source_x + kHalfStep];
			uint8_t *pixel = out.data() + (static_cast<size_t>(z) * kMaskPx + x) * 2;
			pixel[0] = static_cast<uint8_t>(sum >> 10);
			pixel[1] = static_cast<uint8_t>(water_int);
		}
	}
	return out;
}

} // namespace opennova::terrain
