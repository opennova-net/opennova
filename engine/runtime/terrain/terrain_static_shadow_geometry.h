#pragma once

// Portable static-shadow caster geometry/material resolution over the native
// 3DI model. This is the policy half the Godot adapter formerly owned: strip
// curation in authored ROBJ order with the loader's relative/absolute index
// conventions, presentation-world (-x,y,z) extraction, PROJSHAD material
// admission (alpha sources, AlphaGen, dynamic-UV rejection, TEX_TEAM
// team-frame selection), per-ROBJ coverage accounting, conservative authored
// bounds, and the FNV geometry key that feeds page content stamps. Texture
// pixels stay device-side behind TerrainStaticShadowTextureProvider.
// [orig: Terrain_CollectAndRenderTileModels @0x60D250 — admission
// @0x60D421..0x60D450, projection/submit @0x60D465..0x60D97D; the loader
// strip walk every renderer pass performs; see docs/terrain/terrain-re.md]

#include <terrain/terrain_static_shadow.h>
#include <terrain/terrain_static_shadow_raster.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Threedi3di3;

namespace opennova::terrain {

// Bit values are frozen: they participate in diagnostics and attribution
// reported through the adapter and must not be renumbered.
enum TerrainStaticShadowUnsupported : uint32_t {
	kTerrainStaticShadowUnsupportedNone = 0,
	kTerrainStaticShadowUnsupportedMissingAlphaTexture = 1u << 0,
	kTerrainStaticShadowUnsupportedDynamicAlpha = 1u << 1,
	kTerrainStaticShadowUnsupportedDynamicUv = 1u << 2,
	kTerrainStaticShadowUnsupportedFlipbook = 1u << 3,
	kTerrainStaticShadowUnsupportedSkinnedLod = 1u << 4,
	kTerrainStaticShadowUnsupportedInvalidMaterial = 1u << 5,
	kTerrainStaticShadowUnsupportedIncompleteGeometry = 1u << 6,
	kTerrainStaticShadowUnsupportedMalformedIndices = 1u << 7,
	kTerrainStaticShadowUnsupportedMissingRequiredUvs = 1u << 8,
};

// Names for each set issue bit, in bit order. Stable spellings — they are the
// diagnostic vocabulary the capture tooling records.
std::vector<const char *> terrain_static_shadow_unsupported_reason_names(
		uint32_t issues);

// Alpha mips decoded by the device (Godot Image) and handed over as bytes.
// The mip views alias storage; a pyramid is immutable once built.
struct TerrainStaticShadowAlphaPyramid {
	std::vector<std::vector<uint8_t>> storage;
	std::vector<TerrainStaticShadowAlphaMipView> mips;
};

class TerrainStaticShadowTextureProvider {
public:
	virtual ~TerrainStaticShadowTextureProvider() = default;
	// Returns the alpha pyramid for a texture name, or null when the texture
	// cannot be loaded/decoded. Names are the authored 3DI texture names.
	virtual std::shared_ptr<const TerrainStaticShadowAlphaPyramid> load_alpha(
			std::string_view texture_name) = 0;
};

struct TerrainStaticShadowResolvedMaterial {
	TerrainStaticShadowBlend blend = TerrainStaticShadowBlend::Opaque;
	bool alpha_test_enabled = false;
	bool alpha_test_inverted = false;
	uint8_t alpha_ref = 0;
	float alpha_scale = 1.0f;
	bool two_sided = false;
	bool exact = true;
	uint32_t unsupported_issues = kTerrainStaticShadowUnsupportedNone;
	std::shared_ptr<const TerrainStaticShadowAlphaPyramid> alpha_texture;
	std::vector<std::shared_ptr<const TerrainStaticShadowAlphaPyramid>>
			team_alpha_frames;
};

struct TerrainStaticShadowResolvedSurface {
	uint16_t render_object = 0;
	// Presentation-world ROBJ offset (-abs.x, abs.y, abs.z).
	std::array<float, 3> render_object_offset{};
	// Unindexed triangle soup in strip order: indices are 0..n sequential.
	std::vector<std::array<float, 3>> vertices;
	std::vector<std::array<float, 2>> uvs;
	std::vector<int32_t> indices;
	int material_index = -1;
};

struct TerrainStaticShadowResolvedGeometry {
	// FNV over materials + per-LOD surface content; feeds page content
	// stamps. Zero is reserved (normalized to 1).
	uint64_t key = 0;
	std::array<uint16_t, 2> render_object_counts{};
	std::array<bool, 2> lod_exact{{true, true}};
	std::array<uint32_t, 2> lod_unsupported_issues{};
	std::array<std::vector<TerrainStaticShadowResolvedSurface>, 2> surfaces;
	std::array<std::vector<TerrainStaticShadowRenderObjectCoverage>, 2>
			coverage;
	std::vector<TerrainStaticShadowResolvedMaterial> materials;
	std::array<float, 3> local_min{};
	std::array<float, 3> local_max{};
	bool has_bounds = false;
	bool bounds_exact = true;
};

// The TEX_TEAM frame selection: team % frame_count into the material's frame
// pyramids, or the static alpha when the material is not team-animated.
// Null means the selected frame has no usable alpha.
// [orig: Avatar camo ctrl consumers select frame = value % frame_count,
// apply_shader_parameters @0x58DC36..0x58DC42]
const TerrainStaticShadowAlphaPyramid *terrain_static_shadow_selected_alpha(
		const TerrainStaticShadowResolvedMaterial &material, int team);

// The material's admission issues for a caster of the given team (adds the
// flipbook issue when the selected team frame is unusable).
uint32_t terrain_static_shadow_caster_material_issues(
		const TerrainStaticShadowResolvedMaterial &material, int team);

// Resolves the full caster geometry/material description from the parsed
// model. graphic feeds the geometry key (lowercased before hashing).
std::shared_ptr<TerrainStaticShadowResolvedGeometry>
resolve_terrain_static_shadow_geometry(const Threedi3di3 &model,
		std::string_view graphic,
		TerrainStaticShadowTextureProvider &textures);

} // namespace opennova::terrain
