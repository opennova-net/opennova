#pragma once

// Portable retail particle frame registration and atlas compilation. This is
// the seam between embedder-loaded RGBA images and an embedder texture uploader: callers
// provide raw frames and receive stable entry identities, exact UV placements,
// and fully preprocessed page pixels without depending on Godot.

#include <renderer/particle_frame.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace renderer {

using ParticleAtlasEntryId = std::size_t;

// CParticleDef_ReloadGraphicFrameTextures @ 0x5e4bb0. one_based_frame is in
// [1, frame_count] when frame_count is greater than one.
std::string retail_particle_frame_name(std::string_view authored,
		int frame_count, int one_based_frame);

struct ParticleRgbaImage {
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> rgba;

	bool valid() const noexcept;
};

// Value result for the witnessed skyline allocator. The counters make its
// otherwise invisible control-flow quirks contractible without exposing a
// renderer adapter's private state.
struct ParticleAtlasAllocation {
	bool valid = false;
	int x = 0;
	int y = 0;
	std::size_t candidate_starts = 0;
	std::size_t rejected_starts = 0;
};

// Exact CParticleAtlas_TryPlaceEntry @ 0x5e2be0 behavior. skyline must contain
// exactly side columns. On failure it is unchanged; on success the selected
// run is raised by height.
ParticleAtlasAllocation allocate_retail_particle_atlas_rect(
		std::vector<int> &skyline, int side, int width, int height);

struct ParticleAtlasPlacement {
	bool valid = false;
	std::uint32_t page = 0;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	ParticleUvRect rect{};
	float inset_u = 0.0f;
	float inset_v = 0.0f;
};

struct ParticleAtlasEntry {
	ParticleAtlasEntryId id = 0;
	std::string name;
	std::uint8_t type = 0;
	int width = 0;
	int height = 0;
	ParticleAtlasPlacement placement{};
};

struct ParticleAtlasPage {
	// The first entry that created a type-1/type-2 compatible page owns this
	// value even when the other compatible type is later placed on it.
	std::uint8_t type = 0;
	ParticleRgbaImage image;
};

struct ParticleAtlasBuild {
	// Entries retain registration order, so ParticleAtlasEntryId indexes this
	// vector directly. Pages retain creation order.
	std::vector<ParticleAtlasEntry> entries;
	std::vector<ParticleAtlasPage> pages;
	std::size_t rejected_entries = 0;
};

// Deep module: registration owns the case-insensitive (name,type) catalog and
// build owns stable ordering, witnessed skyline proposals with overlap-safe page
// fallback, source preprocessing, page composition, whole-page normal
// conversion, and UV derivation.
//
// Registration is first-win. Re-registering an equivalent name and exact type
// returns the original id and does not replace its spelling or pixels. build()
// is value-returning and repeatable; embedder upload state remains outside.
class ParticleAtlasBuilder {
public:
	ParticleAtlasEntryId register_frame(std::string name, std::uint8_t type,
			ParticleRgbaImage image);

	std::size_t entry_count() const noexcept;
	ParticleAtlasBuild build() const;

private:
	struct RegisteredFrame {
		std::string name;
		std::string folded_name;
		std::uint8_t type = 0;
		ParticleRgbaImage image;
	};

	std::vector<RegisteredFrame> frames_;
};

} // namespace renderer
