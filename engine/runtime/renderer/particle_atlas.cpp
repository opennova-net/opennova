#include <renderer/particle_atlas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <utility>

#include <io/strutil.h>

namespace renderer {
namespace {


int atlas_page_size(std::uint8_t type) {
	return type <= 2 ? 1024 : 256;
}

bool atlas_types_compatible(std::uint8_t page_type, std::uint8_t entry_type) {
	if (page_type == 1 || page_type == 2)
		return entry_type == 1 || entry_type == 2;
	return page_type == entry_type;
}

void fill_placement(ParticleAtlasPlacement &placement,
		std::uint32_t page, int side, int x, int y, int width, int height) {
	const float inverse_side = 1.0f / static_cast<float>(side);
	placement.valid = true;
	placement.page = page;
	placement.x = x;
	placement.y = y;
	placement.width = width;
	placement.height = height;
	placement.rect = {
		static_cast<float>(x) * inverse_side,
		static_cast<float>(y) * inverse_side,
		static_cast<float>(x + width) * inverse_side,
		static_cast<float>(y + height) * inverse_side,
	};
	// The material contracts each edge by 2.5 atlas pixels. Cap tiny
	// rectangles at their midpoint so their UV bounds cannot invert.
	placement.inset_u = std::min(2.5f,
			static_cast<float>(width) * 0.5f) * inverse_side;
	placement.inset_v = std::min(2.5f,
			static_cast<float>(height) * 0.5f) * inverse_side;
}

std::uint8_t encode_normal(float value) {
	const int encoded = static_cast<int>((value + 1.0f) * 127.5f);
	return static_cast<std::uint8_t>(std::clamp(encoded, 0, 255));
}

// The atlas conversion reads the blue byte as height, wraps with the
// power-of-two page masks, and retains source alpha. Types 3/6 use 1/8;
// type 7 uses 1/32 and forces output blue to 255.
void convert_page_to_normal_map(ParticleRgbaImage &image, float scale,
		bool force_blue) {
	if (!image.valid())
		return;
	const int width = image.width;
	const int height = image.height;
	const std::vector<std::uint8_t> source = image.rgba;
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
			image.rgba[offset + 0] = encode_normal(nx);
			image.rgba[offset + 1] = encode_normal(ny);
			image.rgba[offset + 2] = force_blue ? 255 : encode_normal(nz);
			// Alpha intentionally remains the pre-conversion byte.
		}
	}
}

struct WorkingPage {
	ParticleAtlasPage page;
	std::vector<int> skyline;
	struct OccupiedRect {
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;
	};
	std::vector<OccupiedRect> occupied;
};

bool rectangles_intersect(const WorkingPage::OccupiedRect &lhs,
		const WorkingPage::OccupiedRect &rhs) {
	return lhs.x < rhs.x + rhs.width && rhs.x < lhs.x + lhs.width &&
			lhs.y < rhs.y + rhs.height && rhs.y < lhs.y + lhs.height;
}

bool intersects_occupied(const WorkingPage &page,
		const WorkingPage::OccupiedRect &candidate) {
	return std::any_of(page.occupied.begin(), page.occupied.end(),
			[&](const WorkingPage::OccupiedRect &occupied) {
				return rectangles_intersect(occupied, candidate);
			});
}

} // namespace

std::string retail_particle_frame_name(std::string_view authored,
		int frame_count, int one_based_frame) {
	if (frame_count <= 1)
		return std::string(authored);
	std::string base = opennova::strutil::to_lower(authored);
	const std::size_t extension = base.find(".tga");
	if (extension != std::string::npos)
		base.resize(extension);
	char suffix[24]{};
	if (one_based_frame < 10)
		std::snprintf(suffix, sizeof(suffix), "_0%d.tga", one_based_frame);
	else
		std::snprintf(suffix, sizeof(suffix), "_%d.tga", one_based_frame);
	return base + suffix;
}

bool ParticleRgbaImage::valid() const noexcept {
	if (width <= 0 || height <= 0)
		return false;
	const std::size_t w = static_cast<std::size_t>(width);
	const std::size_t h = static_cast<std::size_t>(height);
	if (w > std::numeric_limits<std::size_t>::max() / h)
		return false;
	const std::size_t pixels = w * h;
	if (pixels > std::numeric_limits<std::size_t>::max() / 4)
		return false;
	return rgba.size() == pixels * 4;
}

ParticleAtlasAllocation allocate_retail_particle_atlas_rect(
		std::vector<int> &skyline, int side, int width, int height) {
	ParticleAtlasAllocation result;
	if (side <= 0 || static_cast<int>(skyline.size()) != side ||
			width <= 0 || height <= 0) {
		return result;
	}
	const int max_y = side - height;
	// Both comparisons are witnessed as strict. In particular width == side
	// cannot be placed even on an empty page.
	if (max_y < 0 || side - width <= 0)
		return result;

	int best = -1;
	int x = 0;
	// This accumulator belongs to the whole call, not one candidate start.
	int min_seen = side;
	const int limit = side - width;
	while (x < limit) {
		++result.candidate_starts;
		int scan = x;
		const int end = x + width;
		bool rejected = false;
		while (scan < end) {
			min_seen = std::min(min_seen,
					skyline[static_cast<std::size_t>(scan)]);
			if (min_seen > max_y) {
				// The common loop increment below advances once more, so the
				// start immediately following scan is never probed.
				x = scan + 1;
				++result.rejected_starts;
				rejected = true;
				break;
			}
			++scan;
		}
		if (!rejected) {
			if (best == -1)
				best = x;
			if (skyline[static_cast<std::size_t>(x)] <
					skyline[static_cast<std::size_t>(best)] &&
					skyline[static_cast<std::size_t>(best)] < max_y) {
				best = x;
			}
		}
		++x;
	}
	if (best < 0)
		return result;

	const int base = skyline[static_cast<std::size_t>(best)];
	for (int column = best; column < best + width; ++column)
		skyline[static_cast<std::size_t>(column)] = base + height;
	result.valid = true;
	result.x = best;
	result.y = base;
	return result;
}

ParticleAtlasEntryId ParticleAtlasBuilder::register_frame(std::string name,
		std::uint8_t type, ParticleRgbaImage image) {
	const std::string folded_name = opennova::strutil::to_lower(name);
	for (ParticleAtlasEntryId id = 0; id < frames_.size(); ++id) {
		const RegisteredFrame &registered = frames_[id];
		if (registered.type == type && registered.folded_name == folded_name)
			return id;
	}
	const ParticleAtlasEntryId id = frames_.size();
	frames_.push_back({std::move(name), folded_name, type, std::move(image)});
	return id;
}

std::size_t ParticleAtlasBuilder::entry_count() const noexcept {
	return frames_.size();
}

ParticleAtlasBuild ParticleAtlasBuilder::build() const {
	ParticleAtlasBuild result;
	result.entries.resize(frames_.size());
	for (ParticleAtlasEntryId id = 0; id < frames_.size(); ++id) {
		const RegisteredFrame &frame = frames_[id];
		ParticleAtlasEntry &entry = result.entries[id];
		entry.id = id;
		entry.name = frame.name;
		entry.type = frame.type;
		entry.width = frame.image.width;
		entry.height = frame.image.height;
	}

	std::vector<ParticleAtlasEntryId> placement_order(frames_.size());
	std::iota(placement_order.begin(), placement_order.end(), 0);
	// [orig: CParticleManager_BuildTextureAtlases @ 0x5e8db0] uses a stable
	// width-descending order; equal-width entries retain registrar order.
	std::stable_sort(placement_order.begin(), placement_order.end(),
			[&](ParticleAtlasEntryId lhs, ParticleAtlasEntryId rhs) {
				return frames_[lhs].image.width > frames_[rhs].image.width;
			});

	std::vector<WorkingPage> working_pages;
	for (ParticleAtlasEntryId id : placement_order) {
		const RegisteredFrame &frame = frames_[id];
		ParticleAtlasEntry &entry = result.entries[id];
		if (!frame.image.valid()) {
			++result.rejected_entries;
			continue;
		}

		for (std::size_t page_index = 0;
				page_index < working_pages.size(); ++page_index) {
			WorkingPage &working = working_pages[page_index];
			if (!atlas_types_compatible(working.page.type, frame.type))
				continue;
			// Keep the witnessed allocator untouched, but treat its skyline as a
			// proposal. Its persistent minimum can select a rectangle below an
			// occupied column and then lower that column. The builder must not
			// publish an overlapping placement or forget occupied height.
			std::vector<int> candidate_skyline = working.skyline;
			const ParticleAtlasAllocation allocation =
					allocate_retail_particle_atlas_rect(candidate_skyline,
							working.page.image.width,
							frame.image.width, frame.image.height);
			if (!allocation.valid)
				continue;
			const WorkingPage::OccupiedRect candidate{
				allocation.x, allocation.y,
				frame.image.width, frame.image.height,
			};
			if (intersects_occupied(working, candidate))
				continue;
			for (int column = allocation.x;
					column < allocation.x + frame.image.width; ++column) {
				const std::size_t index = static_cast<std::size_t>(column);
				candidate_skyline[index] = std::max(
						candidate_skyline[index], working.skyline[index]);
			}
			working.skyline = std::move(candidate_skyline);
			working.occupied.push_back(candidate);
			fill_placement(entry.placement,
					static_cast<std::uint32_t>(page_index),
					working.page.image.width, allocation.x, allocation.y,
					frame.image.width, frame.image.height);
			break;
		}
		if (entry.placement.valid)
			continue;

		WorkingPage working;
		working.page.type = frame.type;
		const int side = atlas_page_size(frame.type);
		working.page.image.width = side;
		working.page.image.height = side;
		working.skyline.assign(static_cast<std::size_t>(side), 0);
		const ParticleAtlasAllocation allocation =
				allocate_retail_particle_atlas_rect(working.skyline,
						side, frame.image.width, frame.image.height);
		if (!allocation.valid) {
			++result.rejected_entries;
			continue;
		}
		const std::uint32_t page_index =
				static_cast<std::uint32_t>(working_pages.size());
		fill_placement(entry.placement, page_index, side,
				allocation.x, allocation.y,
				frame.image.width, frame.image.height);
		working.occupied.push_back({
			allocation.x, allocation.y,
			frame.image.width, frame.image.height,
		});
		working_pages.push_back(std::move(working));
	}

	for (WorkingPage &working : working_pages) {
		const std::size_t bytes = static_cast<std::size_t>(
				working.page.image.width * working.page.image.height * 4);
		working.page.image.rgba.assign(bytes, 0);
	}
	for (ParticleAtlasEntryId id : placement_order) {
		const RegisteredFrame &frame = frames_[id];
		const ParticleAtlasPlacement &placement = result.entries[id].placement;
		if (!placement.valid)
			continue;
		ParticleRgbaImage &page =
				working_pages[placement.page].page.image;
		for (int y = 0; y < frame.image.height; ++y) {
			for (int x = 0; x < frame.image.width; ++x) {
				const std::size_t source = static_cast<std::size_t>(
						(y * frame.image.width + x) * 4);
				const std::size_t destination = static_cast<std::size_t>(
						((placement.y + y) * page.width + placement.x + x) * 4);
				page.rgba[destination + 0] = frame.image.rgba[source + 0];
				page.rgba[destination + 1] = frame.image.rgba[source + 1];
				page.rgba[destination + 2] = frame.image.rgba[source + 2];
				// Type 1 alpha is cleared per source before the stable-order
				// blit. A type 2 source on the same compatible page retains it.
				page.rgba[destination + 3] = frame.type == 1
						? 0 : frame.image.rgba[source + 3];
			}
		}
	}

	result.pages.reserve(working_pages.size());
	for (WorkingPage &working : working_pages) {
		if (working.page.type == 3 || working.page.type == 6) {
			convert_page_to_normal_map(working.page.image, 0.125f, false);
		} else if (working.page.type == 7) {
			convert_page_to_normal_map(working.page.image, 0.03125f, true);
		}
		result.pages.push_back(std::move(working.page));
	}
	return result;
}

} // namespace renderer
