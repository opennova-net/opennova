#include <runtime/terrain/terrain_scorch.h>
#include <base/io/hash.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace opennova::terrain {
namespace {

// Bucket cell: the 512-unit routed sector, the coarsest page span.
constexpr int64_t kSectorCellQ16 = INT64_C(512) << 16;
// A page spans at most one cell per axis, so its inclusive Q16 extent touches
// at most two cells per axis.
constexpr std::size_t kMaximumPageCells = 4;

int64_t floor_div(int64_t value, int64_t divisor) noexcept {
	int64_t quotient = value / divisor;
	if (value % divisor != 0 && (value < 0) != (divisor < 0)) --quotient;
	return quotient;
}

uint64_t sector_cell_key(int64_t cell_x, int64_t cell_z) noexcept {
	return (static_cast<uint64_t>(static_cast<uint32_t>(cell_x)) << 32) |
			static_cast<uint64_t>(static_cast<uint32_t>(cell_z));
}

uint64_t mix_entry(uint64_t hash, const TerrainScorchEntry &entry) noexcept;

int wrap(int value, int size) noexcept {
	value %= size;
	return value < 0 ? value + size : value;
}

struct RgbaF {
	float channels[4]{};
};

RgbaF sample_mip(const Rgba8Image &mip, float u, float v) noexcept {
	const float wrapped_u = u - std::floor(u);
	const float wrapped_v = v - std::floor(v);
	const float x = wrapped_u * static_cast<float>(mip.width) - 0.5f;
	const float y = wrapped_v * static_cast<float>(mip.height) - 0.5f;
	const int raw_x0 = static_cast<int>(std::floor(x));
	const int raw_y0 = static_cast<int>(std::floor(y));
	const int x0 = wrap(raw_x0, static_cast<int>(mip.width));
	const int y0 = wrap(raw_y0, static_cast<int>(mip.height));
	const int x1 = wrap(raw_x0 + 1, static_cast<int>(mip.width));
	const int y1 = wrap(raw_y0 + 1, static_cast<int>(mip.height));
	const float tx = x - std::floor(x);
	const float ty = y - std::floor(y);
	RgbaF result;
	for (int channel = 0; channel < 4; ++channel) {
		const auto texel = [&](int px, int py) noexcept {
			const std::size_t offset = 4u *
					(static_cast<std::size_t>(py) * mip.width + px);
			return mip.pixels[offset + static_cast<std::size_t>(channel)] /
					255.0f;
		};
		const float top = texel(x0, y0) +
				(texel(x1, y0) - texel(x0, y0)) * tx;
		const float bottom = texel(x0, y1) +
				(texel(x1, y1) - texel(x0, y1)) * tx;
		result.channels[channel] = top + (bottom - top) * ty;
	}
	return result;
}

RgbaF sample_nearest_level(const TerrainScorchTexture &texture,
		float u, float v, float texels_per_pixel) noexcept {
	std::size_t level = 0;
	if (texels_per_pixel > 1.0f) {
		level = static_cast<std::size_t>(std::clamp(
				static_cast<int>(std::floor(std::log2(texels_per_pixel) + 0.5f)),
				0, static_cast<int>(texture.mips.size()) - 1));
	}
	return sample_mip(texture.mips[level], u, v);
}

uint8_t unorm_byte(float value) noexcept {
	return static_cast<uint8_t>(std::clamp(
			static_cast<int>(std::lround(
					std::clamp(value, 0.0f, 1.0f) * 255.0f)),
			0, 255));
}

uint64_t mix_entry(uint64_t hash, const TerrainScorchEntry &entry) noexcept {
	hash = io::fnv1a64_value(hash, entry.texture_index);
	hash = io::fnv1a64_value(hash, entry.minimum_x_q16);
	hash = io::fnv1a64_value(hash, entry.minimum_z_q16);
	hash = io::fnv1a64_value(hash, entry.maximum_x_q16);
	hash = io::fnv1a64_value(hash, entry.maximum_z_q16);
	return hash;
}

} // namespace

std::string_view terrain_scorch_texture_name(
		uint8_t texture_index) noexcept {
	switch (texture_index) {
		case 0: return "trscrch1.tga";
		case 1: return "trscrch2.tga";
		case 2: return "trscrch3.tga";
		case 4: return "qburn01.tga";
		default: return {};
	}
}

bool TerrainScorchTexture::is_valid() const noexcept {
	if (mips.empty()) return false;
	uint32_t width = mips.front().width;
	uint32_t height = mips.front().height;
	for (const Rgba8Image &mip : mips) {
		if (!mip.is_valid() || mip.width != width || mip.height != height) {
			return false;
		}
		width = std::max(1u, width >> 1u);
		height = std::max(1u, height >> 1u);
	}
	return true;
}

TerrainScorchTexture build_terrain_scorch_texture(
		const Rgba8Image &base) {
	TerrainScorchTexture result;
	result.mips = build_box_mip_chain_to_4x4(base);
	if (!result.is_valid()) result.mips.clear();
	return result;
}

// Append-only list capped at 4096 records; a full list silently drops the
// record [orig: Terrain_AddScorchRecord @ 0x605c90 — cap @ 0x605c9f, the
// 20-byte {index, min_x, min_z, max_x, max_z} row @ 0x605cc7..0x605cef,
// then the overlapping cached-tile invalidation walk @ 0x605cfc..0x605d5f].
bool TerrainScorchRegistry::append(const TerrainScorchEntry &entry) {
	if (full() || !terrain_scorch_texture_index_valid(entry.texture_index) ||
			entry.minimum_x_q16 >= entry.maximum_x_q16 ||
			entry.minimum_z_q16 >= entry.maximum_z_q16) {
		return false;
	}
	const uint32_t index = static_cast<uint32_t>(entries_.size());
	// Reserve first so the record push below cannot fail after the buckets
	// grew; a bucket allocation failure leaves at most stale copies of an
	// index the walk ignores until it is reused and merges as duplicates.
	entries_.reserve(entries_.size() + 1);
	// Bucket the record into every sector cell its inclusive bounds touch so
	// a page walk in any of those cells finds it; the page test stays the
	// exact inclusive overlap below.
	const int64_t first_x = floor_div(entry.minimum_x_q16, kSectorCellQ16);
	const int64_t last_x = floor_div(entry.maximum_x_q16, kSectorCellQ16);
	const int64_t first_z = floor_div(entry.minimum_z_q16, kSectorCellQ16);
	const int64_t last_z = floor_div(entry.maximum_z_q16, kSectorCellQ16);
	for (int64_t cell_z = first_z; cell_z <= last_z; ++cell_z) {
		for (int64_t cell_x = first_x; cell_x <= last_x; ++cell_x) {
			sector_records_[sector_cell_key(cell_x, cell_z)].push_back(index);
		}
	}
	entries_.push_back(entry);
	++generation_;
	return true;
}

void TerrainScorchRegistry::clear() noexcept {
	entries_.clear();
	sector_records_.clear();
	++generation_;
}

bool TerrainScorchRegistry::overlaps_page(const TerrainScorchEntry &entry,
		const TerrainTilePageKey &page) noexcept {
	return TerrainTileCompositionCache::page_overlaps_q16(page,
			entry.minimum_x_q16, entry.minimum_z_q16,
			entry.maximum_x_q16, entry.maximum_z_q16);
}

// The page walk keeps retail's insertion order: PolyTrn_RenderTile runs the
// permanent list front to back and draws every record overlapping the tile
// [orig: PolyTrn_RenderTile @ 0x60DF39..0x60E0AF]. Candidates come from the
// sector cells the page's inclusive extent touches, merged ascending by
// record index (each cell list is ascending, a record may sit in several).
bool TerrainScorchRegistry::collect(const TerrainTilePageKey &page,
		std::vector<TerrainScorchEntry> *entries,
		uint64_t &content_stamp, uint32_t &count) const {
	const int span = TerrainTileCompositionCache::page_world_span(
			page.page_lod_level);
	if (span == 0) return false;
	content_stamp = io::kFnv1a64Offset;
	count = 0;
	const int64_t page_minimum_x =
			(static_cast<int64_t>(page.sector_origin_x) + page.page_local_x) << 16;
	const int64_t page_minimum_z =
			(static_cast<int64_t>(page.sector_origin_z) + page.page_local_z) << 16;
	const int64_t page_maximum_x = page_minimum_x +
			(static_cast<int64_t>(span) << 16);
	const int64_t page_maximum_z = page_minimum_z +
			(static_cast<int64_t>(span) << 16);
	const int64_t first_x = floor_div(page_minimum_x, kSectorCellQ16);
	const int64_t last_x = floor_div(page_maximum_x, kSectorCellQ16);
	const int64_t first_z = floor_div(page_minimum_z, kSectorCellQ16);
	const int64_t last_z = floor_div(page_maximum_z, kSectorCellQ16);

	std::array<const std::vector<uint32_t> *, kMaximumPageCells> cells{};
	std::array<std::size_t, kMaximumPageCells> cursors{};
	std::size_t cell_count = 0;
	bool cells_overflowed = false;
	for (int64_t cell_z = first_z; cell_z <= last_z && !cells_overflowed;
			++cell_z) {
		for (int64_t cell_x = first_x; cell_x <= last_x; ++cell_x) {
			const auto bucket = sector_records_.find(
					sector_cell_key(cell_x, cell_z));
			if (bucket == sector_records_.end() || bucket->second.empty()) {
				continue;
			}
			if (cell_count == cells.size()) {
				cells_overflowed = true;
				break;
			}
			cells[cell_count++] = &bucket->second;
		}
	}

	const auto emit = [&](const TerrainScorchEntry &entry) {
		if (!overlaps_page(entry, page)) return;
		if (entries != nullptr) entries->push_back(entry);
		content_stamp = mix_entry(content_stamp, entry);
		++count;
	};
	if (cells_overflowed) {
		// A page wider than one sector cell is not a routed page; keep the
		// result exact with the plain list walk rather than a partial merge.
		for (const TerrainScorchEntry &entry : entries_) emit(entry);
	} else {
		for (;;) {
			std::size_t best_cell = cells.size();
			uint32_t best_index = 0;
			for (std::size_t cell = 0; cell < cell_count; ++cell) {
				if (cursors[cell] >= cells[cell]->size()) continue;
				const uint32_t index = (*cells[cell])[cursors[cell]];
				if (best_cell == cells.size() || index < best_index) {
					best_cell = cell;
					best_index = index;
				}
			}
			if (best_cell == cells.size()) break;
			for (std::size_t cell = 0; cell < cell_count; ++cell) {
				while (cursors[cell] < cells[cell]->size() &&
						(*cells[cell])[cursors[cell]] == best_index) {
					++cursors[cell];
				}
			}
			if (best_index >= entries_.size()) continue;
			emit(entries_[best_index]);
		}
	}
	content_stamp = io::fnv1a64_value(content_stamp, count);
	return true;
}

TerrainScorchPageStamp TerrainScorchRegistry::stamp(
		const TerrainTilePageKey &page) const {
	TerrainScorchPageStamp result;
	result.valid = collect(page, nullptr, result.content_stamp,
			result.entry_count);
	if (!result.valid) result = TerrainScorchPageStamp{};
	return result;
}

TerrainScorchPagePlan TerrainScorchRegistry::plan(
		const TerrainTilePageKey &page) const {
	TerrainScorchPagePlan result;
	uint32_t count = 0;
	result.valid = collect(page, &result.entries, result.content_stamp, count);
	if (!result.valid) result = TerrainScorchPagePlan{};
	return result;
}

bool compose_terrain_scorches(
		const TerrainTileCompositionJob &job,
		const TerrainScorchPagePlan &plan,
		const std::array<TerrainScorchTexture,
				kTerrainScorchTextureSlots> &textures,
		Rgba8Image &page) noexcept {
	if (!plan.valid || !page.is_valid() ||
			job.layout.texture_dimension <= 0 || job.layout.world_span <= 0 ||
			page.width != static_cast<uint32_t>(job.layout.texture_dimension) ||
			page.height != static_cast<uint32_t>(job.layout.texture_dimension)) {
		return false;
	}
	const int dimension = job.layout.texture_dimension;
	const int64_t origin_x_q16 = static_cast<int64_t>(
			job.target.page.sector_origin_x + job.target.page.page_local_x) << 16;
	const int64_t origin_z_q16 = static_cast<int64_t>(
			job.target.page.sector_origin_z + job.target.page.page_local_z) << 16;
	const double pixels_per_q16 = static_cast<double>(dimension) /
			static_cast<double>(static_cast<int64_t>(job.layout.world_span) << 16);
	const auto first_covered = [dimension](double edge) {
		return std::clamp(static_cast<int>(std::ceil(edge)), 0, dimension);
	};

	for (const TerrainScorchEntry &entry : plan.entries) {
		if (!terrain_scorch_texture_index_valid(entry.texture_index) ||
				entry.texture_index >= textures.size()) {
			return false;
		}
		const TerrainScorchTexture &texture = textures[entry.texture_index];
		if (!texture.is_valid()) return false;
		// The record's quad: vertex 0 (UV 0,0) at (minimum X, maximum Z),
		// vertex 3 (UV 1,1) at (maximum X, minimum Z) — the page Z axis runs
		// against the record's mission-plane Y, so V grows toward minimum Z.
		// [orig: PolyTrn_RenderTile scorch positions @ 0x60DFD1..0x60E02A,
		// UV (0,0)-(1,1) @ 0x60E06B..0x60E08E]
		const double x0 = static_cast<double>(entry.minimum_x_q16 - origin_x_q16) *
				pixels_per_q16;
		const double x1 = static_cast<double>(entry.maximum_x_q16 - origin_x_q16) *
				pixels_per_q16;
		const double z_v0 = static_cast<double>(entry.maximum_z_q16 - origin_z_q16) *
				pixels_per_q16;
		const double z_v1 = static_cast<double>(entry.minimum_z_q16 - origin_z_q16) *
				pixels_per_q16;
		const double width = x1 - x0;
		const double height = z_v0 - z_v1;
		if (!(width > 0.0) || !(height > 0.0)) return false;
		// Scorch textures load with flags 0 (WRAP, LINEAR, MIPFILTER POINT):
		// bilinear on the level nearest the pixel footprint.
		// [orig: Terrain_LoadScorchTextures @ 0x604CE0 via
		// Texture_LoadByNameWithChannel flags @ 0x58B728; flag decode
		// apply_texture_stages @ 0x68084C..0x680870]
		const float texels_per_pixel = static_cast<float>(std::max(
				texture.mips.front().width / width,
				texture.mips.front().height / height));
		const int x_begin = first_covered(x0);
		const int x_end = first_covered(x1);
		const int y_begin = first_covered(z_v1);
		const int y_end = first_covered(z_v0);
		for (int y = y_begin; y < y_end; ++y) {
			const float v = static_cast<float>((z_v0 - y) / height);
			for (int x = x_begin; x < x_end; ++x) {
				const float u = static_cast<float>((x - x0) / width);
				const RgbaF source = sample_nearest_level(
						texture, u, v, texels_per_pixel);
				const std::size_t offset = 4u *
						(static_cast<std::size_t>(y) * page.width + x);
				for (int channel = 0; channel < 3; ++channel) {
					// Stage colour MODULATE2X(TEXTURE, 0xFF808080) = tex *
					// 256/255 saturated; DESTCOLOR/SRCCOLOR doubles it into
					// the target. [orig: PolyTrn_RenderTile diffuse
					// @ 0x60E033..0x60E04C; scorch mode 0x300628 @ 0x604CEE]
					const float stage = std::clamp(
							source.channels[channel] * (256.0f / 255.0f),
							0.0f, 1.0f);
					const float destination =
							page.pixels[offset + channel] / 255.0f;
					page.pixels[offset + channel] = unorm_byte(
							2.0f * stage * destination);
				}
				// The scorch loop runs inside the same
				// COLORWRITEENABLE=7 window as the .til overlays, so its
				// blend never lands on the page alpha; the DOT3/static
				// passes own that channel exclusively.
				// [orig: PolyTrn_RenderTile SetRenderState(0xA8, 7)
				// @ 0x60DD6B..0x60DD73; 0xF restore @ 0x60E0EA..0x60E0F2]
			}
		}
	}
	return true;
}

} // namespace opennova::terrain
