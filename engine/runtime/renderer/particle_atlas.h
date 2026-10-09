#pragma once

// Portable retail particle frame registration and atlas compilation. This is
// the seam between embedder-loaded RGBA images and an embedder texture uploader: callers
// provide raw frames and receive stable entry identities, exact UV placements,
// and fully preprocessed page pixels without depending on Godot.

#include <runtime/renderer/particle_frame.h>
#include <runtime/renderer/texture_dxt.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::renderer {

using ParticleAtlasEntryId = std::size_t;

// The retail flipbook frame-name registrar
// [orig: CParticleDef_ReloadGraphicFrameTextures @0x5e4bb0]. one_based_frame is
// in [1, frame_count] when frame_count is greater than one.
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
// renderer binding's private state.
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

// The side of the atlas page an entry of `type` (its graphic's blend mode,
// formats/particle BlendMode) is packed on: 1024 for blend, additive and
// premult, 256 for bump, mod, mod2x, bumpadd and distort
// [orig: CParticleManager_BuildTextureAtlases @ 0x5e8f19..0x5e8f20].
int particle_atlas_page_side(std::uint8_t type);
// Whether a graphic of width x height fits an empty page of `type`: narrower
// than the page and no taller [orig: CParticleAtlas_TryPlaceEntry @ 0x5e2c30
// (height), @ 0x5e2c57 (width)]. One that does not is never cleared from the
// build's list, and every pass makes a new page for it, so retail's atlas
// build never ends [orig: CParticleManager_BuildTextureAtlases @ 0x5e9185
// (the miss counted), @ 0x5e91bb (another pass while one missed)].
bool particle_atlas_fits(std::uint8_t type, int width, int height);

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

// The creation flags a page is built from pixels with on the full-quality
// branch: 0x100000 and 0x80000, so at most three levels.
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5E82B2..0x5E82EB]
inline constexpr std::uint32_t kParticlePageCreationFlags = 0x180000u;

// The flags under the session's two settings words: a texcompression_level of
// 1 or less adds 0x200 (a DXT5 page, renderer/texture_compression.h) and a
// particle_density of 1 or less 0x10000 (the page halved once before its
// levels, renderer/particle_density.h) (D-RMAT-24).
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5E82B2..0x5E82CC
//  (g_SessionTexCompressionLevel @ 0x24D2070, g_SessionParticleDensity
//  @ 0x24D2058, against edi = 1 @ 0x5E823D)]
std::uint32_t particle_atlas_page_creation_flags(std::int32_t session_texcompression_level,
		std::int32_t session_particle_density);

// The device texture one composed page becomes under its creation flags: the
// page halved as the flags ask (each halving GTexture_Downsample2x2_RGBA8),
// then as many levels as the flags' chain holds (three for both page sides).
// An A8R8G8B8 page (format None) holds rgba_levels: level 0 the halved page,
// every later level D3DX's box filter of the bytes the level before was
// stored as. A DXT page holds dxt_levels: level 0 the halved page through
// D3DX's encoder, every later level the box filter of the level before as its
// blocks decode (renderer::build_dxt_texture_levels).
// [orig: GTexture_CreateFromPixelData_0 @ 0x687717..0x687766 (the format),
//  @ 0x687785 (GTexture_DownsampleToLimits @ 0x687170), @ 0x6877BA..0x687801
//  (the count, over the halved sides), @ 0x6878A0 (level 0,
//  D3DXLoadSurfaceFromMemory), @ 0x6878B5..0x6878BE (D3DXFilterTexture,
//  D3DX_FILTER_BOX, each level from the one before)]
struct ParticleAtlasPageTexture {
	TextureDxtFormat format = TextureDxtFormat::None;
	std::vector<ParticleRgbaImage> rgba_levels;
	std::vector<DxtSurface> dxt_levels;

	// Level 0's side (the page's after its halvings), 0 for an empty texture.
	int side() const noexcept;
	std::size_t level_count() const noexcept;
};

ParticleAtlasPageTexture particle_atlas_page_texture(const ParticleRgbaImage &page,
		std::uint32_t creation_flags, const TextureDxtCaps &caps = kReferenceTextureDxtCaps);

// The last level a page's stage samples: the particle batch draws through a
// GfxShader pass, so the stage is MIN/MAG LINEAR with MIPFILTER POINT, which
// never reads past the last level of the page's chain (`side` the page's as
// packed, before any halving).
// [orig: CParticleBatch_FlushAndBindMaterial @ 0x5E42DC;
//  CGfxDevice_ApplyRenderStates @ 0x67E463..0x67E4A7]
std::uint32_t particle_atlas_page_last_level(int side, std::uint32_t creation_flags);

struct ParticleAtlasBuild {
	// Entries retain registration order, so ParticleAtlasEntryId indexes this
	// vector directly. Pages retain creation order.
	std::vector<ParticleAtlasEntry> entries;
	std::vector<ParticleAtlasPage> pages;
	// Entries left undrawn: an empty image (retail packs only an entry its size probe
	// found) and every one of oversized_entries.
	std::size_t rejected_entries = 0;
	// The graphics no empty page of their type can hold (not narrower than the page's
	// side, or taller), in placement order. Retail's build never finishes over one: it
	// counts the miss (@ 0x5E9185), makes a fresh page and passes again (@ 0x5E91BB)
	// while one missed, and an entry no empty page holds is never cleared, so the game
	// hangs as the effects load [orig: CParticleManager_BuildTextureAtlases @ 0x5E8DB0].
	// The port leaves it undrawn and names it on the engine log, a deliberate
	// divergence (D-PTL-31): no shipped graphic is one, the editor refuses a project
	// carrying one.
	std::vector<ParticleAtlasEntryId> oversized_entries;
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

// A graphic as its atlas page holds it: registered alone and built as the game builds its
// pages, then cut from the page where the build places it (an additive one's alpha cleared, a
// bump's or a distortion's page made a normal map of its blue) [orig:
// CParticleManager_BuildTextureAtlases @ 0x5E8DB0]. `type` its graphic's blend mode (formats/
// particle BlendMode). An empty image where no page of its type holds it (particle_atlas_fits).
ParticleRgbaImage particle_atlas_paged_frame(const ParticleRgbaImage &frame, std::uint8_t type);

}  // namespace opennova::renderer