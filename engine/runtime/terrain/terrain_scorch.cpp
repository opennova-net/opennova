#include <terrain/terrain_scorch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace opennova::terrain {
namespace {

constexpr uint64_t kFnvOffset = UINT64_C(1469598103934665603);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr int kMaximumAnisotropy = 16;
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

uint64_t mix_byte(uint64_t hash, uint8_t value) noexcept {
	return (hash ^ value) * kFnvPrime;
}

template <typename T>
uint64_t mix_value(uint64_t hash, const T &value) noexcept {
	const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		hash = mix_byte(hash, bytes[index]);
	}
	return hash;
}

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

RgbaF lerp(RgbaF a, RgbaF b, float t) noexcept {
	RgbaF result;
	for (int channel = 0; channel < 4; ++channel) {
		result.channels[channel] = a.channels[channel] +
				(b.channels[channel] - a.channels[channel]) * t;
	}
	return result;
}

RgbaF sample_trilinear(const TerrainScorchTexture &texture,
		float u, float v, float lod) noexcept {
	lod = std::clamp(lod, 0.0f,
			static_cast<float>(texture.mips.size() - 1));
	const std::size_t lower = static_cast<std::size_t>(std::floor(lod));
	const std::size_t upper = std::min(lower + 1, texture.mips.size() - 1);
	return lerp(sample_mip(texture.mips[lower], u, v),
			sample_mip(texture.mips[upper], u, v),
			lod - static_cast<float>(lower));
}

RgbaF sample_anisotropic(const TerrainScorchTexture &texture,
		float u, float v, float du_dx, float dv_dy) noexcept {
	const float rho_x = std::abs(du_dx) * texture.mips[0].width;
	const float rho_y = std::abs(dv_dy) * texture.mips[0].height;
	const float major = std::max({1.0f, rho_x, rho_y});
	const float minor = std::max(1.0f, std::min(rho_x, rho_y));
	const float ratio = std::clamp(major / minor, 1.0f,
			static_cast<float>(kMaximumAnisotropy));
	const int taps = std::clamp(static_cast<int>(std::ceil(ratio)),
			1, kMaximumAnisotropy);
	const float lod = std::log2(std::max(1.0f, major / taps));
	RgbaF result;
	const bool along_u = rho_x >= rho_y;
	for (int tap = 0; tap < taps; ++tap) {
		const float offset =
				(static_cast<float>(tap) + 0.5f) / taps - 0.5f;
		const float sample_u = u + (along_u ? offset * du_dx : 0.0f);
		const float sample_v = v + (along_u ? 0.0f : offset * dv_dy);
		const RgbaF value = sample_trilinear(texture, sample_u, sample_v, lod);
		for (int channel = 0; channel < 4; ++channel) {
			result.channels[channel] += value.channels[channel];
		}
	}
	for (float &channel : result.channels) channel /= taps;
	return result;
}

uint8_t unorm_byte(float value) noexcept {
	return static_cast<uint8_t>(std::clamp(
			static_cast<int>(std::lround(
					std::clamp(value, 0.0f, 1.0f) * 255.0f)),
			0, 255));
}

uint64_t mix_entry(uint64_t hash, const TerrainScorchEntry &entry) noexcept {
	hash = mix_value(hash, entry.texture_index);
	hash = mix_value(hash, entry.minimum_x_q16);
	hash = mix_value(hash, entry.minimum_z_q16);
	hash = mix_value(hash, entry.maximum_x_q16);
	hash = mix_value(hash, entry.maximum_z_q16);
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
	content_stamp = kFnvOffset;
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
	content_stamp = mix_value(content_stamp, count);
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
	const float inverse_q16 = 1.0f / 65536.0f;
	const float world_origin_x = static_cast<float>(
			job.target.page.sector_origin_x + job.target.page.page_local_x);
	const float world_origin_z = static_cast<float>(
			job.target.page.sector_origin_z + job.target.page.page_local_z);
	const float world_per_texel = static_cast<float>(job.layout.world_span) /
			job.layout.texture_dimension;
	const int dimension = job.layout.texture_dimension;

	for (const TerrainScorchEntry &entry : plan.entries) {
		if (!terrain_scorch_texture_index_valid(entry.texture_index) ||
				entry.texture_index >= textures.size()) {
			return false;
		}
		const TerrainScorchTexture &texture = textures[entry.texture_index];
		if (!texture.is_valid()) return false;
		const float minimum_x = entry.minimum_x_q16 * inverse_q16;
		const float minimum_z = entry.minimum_z_q16 * inverse_q16;
		const float maximum_x = entry.maximum_x_q16 * inverse_q16;
		const float maximum_z = entry.maximum_z_q16 * inverse_q16;
		const float width = maximum_x - minimum_x;
		const float height = maximum_z - minimum_z;
		if (!(width > 0.0f) || !(height > 0.0f)) return false;
		const int x0 = std::clamp(static_cast<int>(std::floor(
				(minimum_x - world_origin_x) / world_per_texel)), 0, dimension);
		const int y0 = std::clamp(static_cast<int>(std::floor(
				(minimum_z - world_origin_z) / world_per_texel)), 0, dimension);
		const int x1 = std::clamp(static_cast<int>(std::ceil(
				(maximum_x - world_origin_x) / world_per_texel)), 0, dimension);
		const int y1 = std::clamp(static_cast<int>(std::ceil(
				(maximum_z - world_origin_z) / world_per_texel)), 0, dimension);
		const float du_dx = world_per_texel / width;
		const float dv_dy = world_per_texel / height;
		for (int y = y0; y < y1; ++y) {
			const float world_z = world_origin_z +
					(static_cast<float>(y) + 0.5f) * world_per_texel;
			if (world_z < minimum_z || world_z >= maximum_z) continue;
			const float v = (world_z - minimum_z) / height;
			for (int x = x0; x < x1; ++x) {
				const float world_x = world_origin_x +
						(static_cast<float>(x) + 0.5f) * world_per_texel;
				if (world_x < minimum_x || world_x >= maximum_x) continue;
				const float u = (world_x - minimum_x) / width;
				const RgbaF source = sample_anisotropic(
						texture, u, v, du_dx, dv_dy);
				const std::size_t offset = 4u *
						(static_cast<std::size_t>(y) * page.width + x);
				for (int channel = 0; channel < 3; ++channel) {
					const float destination =
							page.pixels[offset + channel] / 255.0f;
					page.pixels[offset + channel] = unorm_byte(
							2.0f * source.channels[channel] * destination);
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
