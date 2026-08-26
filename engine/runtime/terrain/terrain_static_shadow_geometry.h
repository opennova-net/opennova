#pragma once

// Portable static-shadow caster geometry/material resolution over the native
// 3DI model. This is the policy half the Godot adapter formerly owned: strip
// curation in authored ROBJ order with the loader's relative/absolute index
// conventions, presentation-world (-x,y,z) extraction, PROJSHAD material
// admission (alpha sources plus exact runtime AlphaGen/UV/flipbook inputs),
// per-ROBJ coverage accounting, conservative authored
// bounds, and the FNV geometry key that feeds page content stamps. Texture
// pixels stay device-side behind TerrainStaticShadowTextureProvider.
// [orig: Terrain_CollectAndRenderTileModels @0x60D250 — admission
// @0x60D421..0x60D450, projection/submit @0x60D465..0x60D97D; the loader
// strip walk every renderer pass performs; see docs/terrain/terrain-re.md]

#include <terrain/terrain_static_shadow.h>
#include <terrain/terrain_static_shadow_raster.h>

#include <renderer/material_eval.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::terrain {

enum TerrainStaticShadowUnsupported : uint32_t {
	kTerrainStaticShadowUnsupportedNone = 0,
	kTerrainStaticShadowUnsupportedMissingAlphaTexture = 1u << 0,
	kTerrainStaticShadowUnsupportedFlipbook = 1u << 1,
	kTerrainStaticShadowUnsupportedInvalidMaterial = 1u << 2,
	kTerrainStaticShadowUnsupportedIncompleteGeometry = 1u << 3,
	kTerrainStaticShadowUnsupportedMalformedIndices = 1u << 4,
	kTerrainStaticShadowUnsupportedMissingRequiredUvs = 1u << 5,
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
	bool casts_projected_shadow = true;
	TerrainStaticShadowBlend blend = TerrainStaticShadowBlend::Opaque;
	bool alpha_test_enabled = false;
	bool alpha_test_inverted = false;
	uint8_t alpha_ref = 0;
	bool two_sided = false;
	// The PROJSHAD pass samples diffuse alpha only for material alpha blend or
	// alpha test. AlphaGen itself reaches the pass only through _FFP's
	// material-driven declaration; the file-effect declarations force their
	// own black/opaque source state.
	bool samples_diffuse_alpha = false;
	bool uses_material_alpha = false;
	uint32_t unsupported_issues = kTerrainStaticShadowUnsupportedNone;
	// Immutable POD copy of the authored runtime inputs consumed by the shared
	// material evaluator. The source model may be released after resolution.
	ThreediMaterial runtime_material{};
	// One entry for a static diffuse texture, or authored frame order for an
	// animated diffuse slot. Missing entries remain null and fail only when
	// that frame is selected.
	std::vector<std::shared_ptr<const TerrainStaticShadowAlphaPyramid>>
			diffuse_alpha_frames;
};

// Exact evaluated state for one caster/material at the frame-shared retail
// GetTickCount value and global CTRL-bus snapshot. This is computed once and
// then shared by draw classification and rasterization so stochastic/channel
// evaluation cannot disagree within one page job.
struct TerrainStaticShadowMaterialState {
	::renderer::UvAnimTransform uv{};
	float alpha_scale = 1.0f;
	int32_t diffuse_frame = 0;
	const TerrainStaticShadowAlphaPyramid *alpha_texture = nullptr;
	uint32_t issues = kTerrainStaticShadowUnsupportedNone;
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
	std::array<uint32_t, 2> lod_unsupported_issues{};
	std::array<std::vector<TerrainStaticShadowResolvedSurface>, 2> surfaces;
	std::array<std::vector<TerrainStaticShadowRenderObjectCoverage>, 2>
			coverage;
	std::vector<TerrainStaticShadowResolvedMaterial> materials;
	// Model-local names retained because material parameter bytes are local
	// indices until the retail loader maps them onto the global 96-slot bus.
	std::vector<std::string> control_register_names;
	std::array<float, 3> local_min{};
	std::array<float, 3> local_max{};
	bool has_bounds = false;
	bool bounds_exact = true;
};

// Evaluate the same AlphaGen, complete row-vector UV transform, and diffuse
// animation selector used by ordinary object rendering. The caller supplies
// the frame-shared tick and the already-snapshotted global CTRL values.
// [orig: Render_SubmitEntity @0x5DAD80 tick stamp; batch CTRL snapshot
// @0x5D91AB..0x5D91DE; apply_shader_parameters @0x58DB80]
TerrainStaticShadowMaterialState terrain_static_shadow_evaluate_material(
		const TerrainStaticShadowResolvedGeometry &geometry,
		const TerrainStaticShadowResolvedMaterial &material,
		uint32_t time_ms,
		const ::renderer::ControlRegisterValues &control_values);

// Resolves the full caster geometry/material description from the parsed
// model. graphic feeds the geometry key (lowercased before hashing).
std::shared_ptr<TerrainStaticShadowResolvedGeometry>
resolve_terrain_static_shadow_geometry(const Threedi3di3 &model,
		std::string_view graphic,
		TerrainStaticShadowTextureProvider &textures);

} // namespace opennova::terrain
