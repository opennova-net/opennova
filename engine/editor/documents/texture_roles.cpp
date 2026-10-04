#include <editor/documents/texture_roles.h>

#include <iterator>

#include <base/io/strutil.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/object_shader_template.h>
#include <runtime/renderer/texture_load_rules.h>

namespace opennova::editor {

namespace {

using G = TextureRoleGroup;
using L = TextureLoader;
using S = TextureSizeRule;
using R = TextureRoleId;

constexpr uint8_t kStageFormats = kTextureTga | kTextureMdt | kTexturePcx | kTextureDds;
constexpr uint8_t kArchiveFormats = kTextureTga | kTextureMdt | kTexturePcx | kTextureDds;

constexpr const char *kLoaderTokens[] = {"stage", "plain", "normal", "producer", "chunk", "archive", "hud",
                                         "file",  "menu",  "ptl",    "tga",      "pcx",   "pcx8",    "cube"};
static_assert(std::size(kLoaderTokens) == static_cast<size_t>(TextureLoader::kCount), "a token per loader");

// A row built up column by column (the asset kinds' way): each row names only what it sets.
struct Role {
	TextureRoleRow row;
	constexpr Role(R id, const char *token, const char *words, G group, L loader, uint8_t formats) : row() {
		row.id = id;
		row.token = token;
		row.words = words;
		row.group = group;
		row.loader = loader;
		row.formats = formats;
	}
	constexpr Role size(S rule, uint32_t width = 0, uint32_t height = 0) const {
		Role out = *this;
		out.row.size = rule;
		out.row.width = width;
		out.row.height = height;
		return out;
	}
	constexpr Role alpha(const char *words) const {
		Role out = *this;
		out.row.alpha = words;
		return out;
	}
	constexpr Role sampling(const char *words) const {
		Role out = *this;
		out.row.sampling = words;
		return out;
	}
	constexpr Role missing(const char *words) const {
		Role out = *this;
		out.row.missing = words;
		return out;
	}
	constexpr Role witness(const char *words) const {
		Role out = *this;
		out.row.witness = words;
		return out;
	}
	constexpr Role no_alpha() const {
		Role out = *this;
		out.row.reads_alpha = false;
		return out;
	}
};

constexpr TextureRoleRow kRows[] = {
	// --- Models (.3di material rows) ----------------------------------------------------------------
	Role(R::ModelDiffuse, "model_diffuse", "model diffuse", G::Model, L::Stage, kStageFormats)
	        .alpha("what the material makes of it: a cut-out above its reference, a blend, a shine, or unused")
	        .sampling("wrapped, filtered with its mip chain")
	        .missing("the grey checkerboard")
	        .witness("Texture_LoadByNameWithChannel @ 0x58B470; Material_LoadStageTexture @ 0x5B16F0")
	        .row,
	Role(R::ModelDetail, "model_detail", "model detail", G::Model, L::Stage, kStageFormats)
	        .alpha("multiplied into the diffuse's alpha")
	        .sampling("wrapped, on the second UV set")
	        .missing("the detail stage is dropped")
	        .witness("Texture_LoadByNameWithChannel @ 0x58B470 (slot 2)")
	        .row,
	Role(R::ModelFlipFrame, "model_flip_frame", "model flipbook frame", G::Model, L::Stage, kStageFormats)
	        .alpha("as the diffuse's")
	        .sampling("as the diffuse's; the frame a register picks (value modulo the frames)")
	        .missing("the grey checkerboard")
	        .witness("Material_ApplyShaderParameters @ 0x58DC36 (D-RMAT-11)")
	        .row,
	Role(R::ModelPlain, "model_plain", "model plain texture (type 1)", G::Model, L::Plain,
	     kTextureTga | kTextureMdt | kTexturePcx)
	        .alpha("as stored; a name written .PCX in upper case loads white with alpha from blue")
	        .sampling("as the diffuse's")
	        .missing("the grey checkerboard")
	        .witness("Texture_LoadAndRegister @ 0x58B790 (.PCX @ 0x58B8A5..0x58B8F7)")
	        .row,
	Role(R::ModelNormalMap, "model_normal_map", "model normal map (.mdt)", G::Model, L::Normal, kTextureMdt)
	        .size(S::AtMost, 512)
	        .alpha("the coverage of a VS_DOT3DIFF2 material; the normal in RGB, green running down the texture")
	        .sampling("wrapped")
	        .missing("an unfilled texture (the port binds the checkerboard)")
	        .witness("Texture_LoadAsNormalMap @ 0x58C480; the 512 cap, flag 0x1000 @ 0x5B1782")
	        .row,
	Role(R::ModelHeightNormal, "model_height_normal", "model normal map from height (.tga)", G::Model, L::Normal,
	     kTextureTga | kTextureDds)
	        .size(S::PowerOfTwo)
	        .alpha("the height the normal is made from; blue becomes the output alpha")
	        .sampling("wrapped; the conversion wraps neighbours with the side's mask")
	        .missing("the grey checkerboard")
	        .witness("Texture_LoadAsNormalMap @ 0x58C985..0x58CAED (the kernel), the swap @ 0x58C715..0x58C737")
	        .row,
	Role(R::ModelHorizon, "model_horizon", "model horizon volume", G::Model, L::Producer, kTextureTga | kTextureDds)
	        .alpha("the height")
	        .missing("the grey checkerboard")
	        .witness("sub_58A430 @ 0x58A430 from sub_58A580 @ 0x58A5DA")
	        .row,
	Role(R::ModelOcclusion, "model_occlusion", "model occlusion map", G::Model, L::Producer, kTextureTga | kTextureDds)
	        .alpha("the height")
	        .missing("the grey checkerboard")
	        .witness("sub_58A430 @ 0x58A430 from sub_58CE10 @ 0x58CE6A")
	        .row,
	Role(R::ModelChunk, "model_chunk", "model chunk container", G::Model, L::Chunk, 0)
	        .size(S::Unknown)
	        .missing("nothing loads")
	        .witness("chunk loaders @ 0x58F350, @ 0x58F470, @ 0x58F590")
	        .no_alpha()
	        .row,
	// --- Terrain (.trn) -------------------------------------------------------------------------------
	Role(R::TerrainColourMap, "terrain_colour_map", "terrain colour map", G::Terrain, L::Tga, kTextureTga)
	        .size(S::Exact, 1024, 1024)
	        .alpha("premultiplied into the colour for the far terrain; the base pass drops it")
	        .sampling("clamped, filtered, its mips by the nearest level")
	        .missing("the mission aborts: \"Polytrn: Critical file not found - 'colormap'\"")
	        .witness("PolyTrn_InitTextures @ 0x60B389, @ 0x60B3BE; Game_StartMission @ 0x525AD8")
	        .row,
	Role(R::TerrainBlendMap, "terrain_blend_map", "terrain detail blend map", G::Terrain, L::Tga, kTextureTga)
	        .size(S::QuadrantSplit)
	        .alpha("kept; red, green and blue weigh the three splat details")
	        .sampling("clamped")
	        .missing("the mission aborts (\"blendermap\"): the key alone turns the blend on, on any card with pixel shaders")
	        .witness("Terrain_ParseConfigCallback @ 0x60F7D0; PolyTrn_InitTextures @ 0x60B15D..0x60B1A6, @ 0x60B1B8, the "
	                 "split @ 0x60B2C1; sub_520AA0 @ 0x520B4E")
	        .row,
	Role(R::TerrainSplatDetail, "terrain_splat_detail", "terrain splat detail", G::Terrain, L::Stage,
	     kTextureTga | kTextureDds)
	        .alpha("kept")
	        .sampling("wrapped, compressed DXT1 at load")
	        .missing("no splat layer")
	        .witness("PolyTrn_InitTextures @ 0x60ABEF..0x60AC13")
	        .row,
	Role(R::TerrainFarDetail, "terrain_far_detail", "terrain far detail", G::Terrain, L::Tga, kTextureTga)
	        .alpha("blended toward the far levels")
	        .missing("no far blend")
	        .witness("GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270")
	        .row,
	Role(R::TerrainDetailCoefficient, "terrain_detail_coefficient", "terrain detail (coefficient)", G::Terrain,
	     L::Stage, kTextureTga | kTexturePcx | kTextureDds)
	        .size(S::PowerOfTwo)
	        .alpha("unused; blue is the coefficient height")
	        .sampling("wrapped; the conversion wraps neighbours with the side's mask")
	        .missing("the terrain config is rejected when its key is empty")
	        .witness("Texture_GenerateNormalMap @ 0x58C070")
	        .row,
	Role(R::TerrainSecondDetail, "terrain_second_detail", "terrain second detail", G::Terrain, L::Stage,
	     kTextureTga | kTextureDds)
	        .missing("no underwater modulation")
	        .witness("PolyTrn_InitTextures (polytrn_detailmap2)")
	        .row,
	Role(R::TerrainTileAtlas, "terrain_tile_atlas", "terrain tile atlas", G::Terrain, L::Tga, kTextureTga)
	        .size(S::MultipleOf, 64)
	        .alpha("the overlay's blend over the colour map")
	        .sampling("clamped, nearest, compressed DXT5 at load")
	        .missing("no tile overlays")
	        .witness("Terrain_LoadTileSetAtlas @ 0x604A90, cells @ 0x604B7C")
	        .row,
	Role(R::TerrainScorch, "terrain_scorch", "terrain scorch mark", G::Terrain, L::Stage, kStageFormats)
	        .alpha("unused: a modulate-2x of the ground")
	        .sampling("wrapped, filtered")
	        .missing("nothing drawn")
	        .witness("Terrain_LoadScorchTextures @ 0x604CE0")
	        .no_alpha()
	        .row,
	Role(R::TerrainFoliageMap, "terrain_foliage_map", "terrain foliage map", G::Terrain, L::Pcx8, kTexturePcx)
	        .size(S::SquarePowerOfTwoAtMost, 1024)
	        .alpha("none: each palette index is a foliage code, its colour unused")
	        .sampling("looked up by index")
	        .missing("no detail foliage")
	        .witness("Foliage_LoadFoliageMapPCX @ 0x605AD0; Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066D0")
	        .no_alpha()
	        .row,
	Role(R::TerrainCharMap, "terrain_char_map", "terrain char map", G::Terrain, L::Pcx8, kTexturePcx)
	        .size(S::Unknown)
	        .alpha("none: each palette index is data")
	        .witness("the parsed slot @ 0x60F7F6 (its reader NEEDS-RE)")
	        .no_alpha()
	        .row,
	// --- Environment --------------------------------------------------------------------------------
	Role(R::SkyCloud, "sky_cloud", "sky cloud layer", G::Environment, L::Archive, kArchiveFormats)
	        .alpha("the cloud density; a PCX's is its palette brightness, (85 x (r + g + b)) >> 8")
	        .missing("an opaque decode covers the whole dome")
	        .witness("Terrain_InitRenderingResources @ 0x578A97; Texture_LoadFromArchive @ 0x58BC21..0x58BCFB")
	        .row,
	Role(R::WaterWake, "water_wake", "water wake", G::Environment, L::Archive, kArchiveFormats)
	        .alpha("as stored")
	        .witness("WaterRing_LoadResources @ 0x5DDC90")
	        .row,
	Role(R::WeatherDrop, "weather_drop", "rain or snow drop", G::Environment, L::Stage, kStageFormats)
	        .alpha("blended by its alpha")
	        .missing("the weather draws no texture")
	        .witness("WeatherParticle_LoadTextures @ 0x5DE840")
	        .row,
	Role(R::PreviewCube, "preview_cube", "player preview reflection cube", G::Environment, L::Cube, kTextureDds)
	        .witness("PlayerInfo_InitPreviewModel @ 0x56010C (sub_58A690)")
	        .row,
	// --- Effects --------------------------------------------------------------------------------------
	Role(R::ParticleGraphic, "particle_graphic", "particle graphic", G::Effects, L::Ptl, kTextureTga)
	        .size(S::AtlasPage, 1024, 256)
	        .alpha("by the graphic's mode: a blend, cleared for additive, premultiplied, or a bump from blue")
	        .sampling("packed into an atlas page with a 2.5-pixel inset")
	        .missing("never packed: draws nothing (one no page holds hangs the game)")
	        .witness("CParticleTextureEntry_ProbeSizeFromDisk @ 0x5DFAA0; CParticleManager_BuildTextureAtlases @ 0x5E8F19 (the "
	                 "page), @ 0x5E9185, @ 0x5E91BB (the passes); CParticleAtlas_TryPlaceEntry @ 0x5E2C30, @ 0x5E2C57")
	        .row,
	Role(R::ImpactScar, "impact_scar", "impact scar", G::Effects, L::Archive, kArchiveFormats)
	        .alpha("blended by its alpha; bhole1 also alpha-tested")
	        .sampling("clamped")
	        .witness("Scar_LoadTextures @ 0x5CC2E0 (the table @ 0x8413A8)")
	        .row,
	Role(R::TracerSmoke, "tracer_smoke", "tracer smoke", G::Effects, L::Archive, kArchiveFormats)
	        .alpha("its palette brightness")
	        .witness("CEffectEmitterPool_CreateShaders @ 0x5DC8F0")
	        .row,
	Role(R::ShadowDecal, "shadow_decal", "blob shadow decal", G::Effects, L::Stage, kTextureTga | kTextureMdt)
	        .witness("Shadow_DecalLoadTextures @ 0x588040")
	        .row,
	Role(R::SightCard, "sight_card", "weapon sight card", G::Effects, L::Stage, kStageFormats)
	        .alpha("by its blend: alpha, add, multiply, each optionally alpha-tested")
	        .sampling("clamped")
	        .witness("WeaponDef_CreateBlendNamedMaterial @ 0x540180")
	        .row,
	Role(R::ViewEffect, "view_effect", "view effect overlay", G::Effects, L::Stage, kStageFormats)
	        .alpha("as stored")
	        .witness("ViewFx_InitShadersAndTextures @ 0x5CF8E0")
	        .row,
	// --- HUD -------------------------------------------------------------------------------------------
	Role(R::HudColour, "hud_colour", "HUD colour art", G::Hud, L::Hud, kTextureTga | kTexturePcx)
	        .alpha("blended by its alpha (a PCX: white, alpha from blue)")
	        .sampling("one level, no mips")
	        .witness("HUD_LoadAllTextures @ 0x59DDA0; HUD_LoadImageAsTexture @ 0x591550")
	        .row,
	Role(R::HudAlphaOnly, "hud_alpha_only", "HUD alpha-only art", G::Hud, L::Hud, kTextureTga | kTexturePcx)
	        .alpha("the only channel used, tinted by the HUD colour")
	        .sampling("one level, no mips (A8)")
	        .witness("HUD_LoadImageAsTexture @ 0x5916AE..0x5916BE; HUD_LoadAllTextures @ 0x59E248..0x59E2CB")
	        .row,
	Role(R::HudFileArt, "hud_file_art", "HUD map art", G::Hud, L::File, kTextureTga | kTexturePcx)
	        .alpha("as stored")
	        .witness("Texture_LoadFromFile_0 @ 0x58FE00")
	        .row,
	Role(R::HudAttitude, "hud_attitude", "HUD attitude back", G::Hud, L::Stage, kStageFormats)
	        .sampling("clamped, one level")
	        .witness("g_HUDAhzBackTextureName (STAGE flags 0x40001)")
	        .row,
	Role(R::HudMfd, "hud_mfd", "HUD MFD texture", G::Hud, L::Pcx, kTexturePcx)
	        .size(S::PowerOfTwo)
	        .missing("no material is made")
	        .witness("sub_59B120 @ 0x59B19F..0x59B1BD")
	        .no_alpha()
	        .row,
	Role(R::MapOverview, "map_overview", "mission map overview", G::Hud, L::Pcx, kTexturePcx)
	        .witness("Minimap_InitOverviewTexture @ 0x573FA0")
	        .no_alpha()
	        .row,
	Role(R::NetIcon, "net_icon", "network icon strip", G::Hud, L::Tga, kTextureTga)
	        .witness("CNetworkIcons_LoadTextures @ 0x4C2CF0")
	        .row,
	Role(R::TipArt, "tip_art", "tip panel art", G::Hud, L::Stage, kStageFormats)
	        .witness("CTipSystem_Init @ 0x5B6970")
	        .row,
	Role(R::BoardBox, "board_box", "board box art", G::Hud, L::Tga, kTextureTga)
	        .witness("BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0")
	        .row,
	// --- Menus and screens -----------------------------------------------------------------------------
	Role(R::MenuImage, "menu_image", "menu image", G::Menus, L::Menu, kTextureTga | kTexturePcx | kTextureDds | kTexturePng)
	        .alpha("blended by its alpha (a PCX opaque)")
	        .witness("CTextureManager_LoadOrFindTexture @ 0x654980")
	        .row,
	Role(R::MenuFrameStencil, "menu_frame_stencil", "menu frame stencil", G::Menus, L::Menu,
	     kTextureTga | kTexturePcx | kTextureDds | kTexturePng)
	        .alpha("multiplied with the brush's")
	        .witness("CTextureManager_LoadOrFindTexture @ 0x654980 (frames)")
	        .row,
	Role(R::MenuFrameBrush, "menu_frame_brush", "menu frame brush", G::Menus, L::Menu,
	     kTextureTga | kTexturePcx | kTextureDds | kTexturePng)
	        .alpha("multiplied with the stencil's")
	        .witness("CTextureManager_LoadOrFindTexture @ 0x654980 (frames)")
	        .row,
	Role(R::MenuCursor, "menu_cursor", "menu cursor", G::Menus, L::Menu, kTextureTga | kTexturePcx | kTextureDds | kTexturePng)
	        .alpha("blended by its alpha")
	        .witness("CTextureManager_LoadOrFindTexture @ 0x654980")
	        .row,
	Role(R::LoadingScreen, "loading_screen", "mission loading screen", G::Menus, L::Pcx, kTexturePcx)
	        .size(S::Exact, 800, 600)
	        .alpha("none: opaque")
	        .sampling("stretched; the multiplayer text laid out for 800 x 600 art")
	        .missing("loadscrn.pcx in its place")
	        .witness("Render_LoadingScreen @ 0x521D10")
	        .no_alpha()
	        .row,
	Role(R::BootSplash, "boot_splash", "boot splash", G::Menus, L::Pcx, kTexturePcx)
	        .alpha("none: opaque")
	        .sampling("stretched to the desktop")
	        .witness("Game_ShowLoadingScreen @ 0x4A5420")
	        .no_alpha()
	        .row,
	Role(R::SplashCursor, "splash_cursor", "start-mission cursor", G::Menus, L::Tga, kTextureTga)
	        .witness("Game_ShowStartMissionSplash @ 0x520820")
	        .row,
	Role(R::CinematicFade, "cinematic_fade", "cinematic fade image", G::Menus, L::Tga, kTextureTga | kTexturePcx | kTextureDds)
	        .missing("a solid 4 x 4 fill")
	        .witness("CinematicFadeEvent_LoadTexture @ 0x570D00")
	        .row,
};

static_assert(std::size(kRows) == kTextureRoleCount, "every TextureRoleId has exactly one row");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kTextureRoleCount; ++i) {
		const TextureRoleRow &row = kRows[i];
		if (static_cast<size_t>(row.id) != i || !*row.token || !*row.words || !*row.witness) return false;
		for (size_t j = 0; j < i; ++j)
			if (same_text(row.token, kRows[j].token)) return false;
	}
	return true;
}
static_assert(rows_well_formed(), "the roles follow TextureRoleId's order, each with a token of its own, words and a witness");

} // namespace

const char *texture_loader_token(TextureLoader loader) {
	const size_t index = static_cast<size_t>(loader);
	return index < std::size(kLoaderTokens) ? kLoaderTokens[index] : "";
}

const char *texture_role_group_words(TextureRoleGroup group) {
	switch (group) {
	case G::Model: return "Models";
	case G::Terrain: return "Terrain";
	case G::Environment: return "Environment";
	case G::Effects: return "Effects";
	case G::Hud: return "HUD";
	case G::Menus: return "Menus and screens";
	case G::kCount: break;
	}
	return "";
}

const TextureRoleRow &texture_role_row(TextureRoleId id) {
	const size_t index = static_cast<size_t>(id);
	return kRows[index < kTextureRoleCount ? index : 0];
}

bool texture_role_from_token(const std::string &token, TextureRoleId &out) {
	for (const TextureRoleRow &row : kRows)
		if (token == row.token) {
			out = row.id;
			return true;
		}
	return false;
}

bool texture_role_takes(const TextureRoleRow &row, const std::string &extension) {
	const std::string e = strutil::to_lower(extension);
	if (e == ".tga") return (row.formats & kTextureTga) != 0;
	if (e == ".mdt") return (row.formats & kTextureMdt) != 0;
	if (e == ".pcx") return (row.formats & kTexturePcx) != 0;
	if (e == ".dds") return (row.formats & kTextureDds) != 0;
	if (e == ".png") return (row.formats & kTexturePng) != 0;
	return false;
}

int32_t texture_role_arg(TextureRoleId role, int32_t flags) {
	return kTextureRoleArg + int32_t(role) + (flags & (kTextureArgGates | kTextureArgTileSet));
}

bool texture_arg_role(int32_t loader_arg, TextureRoleId &role) {
	if (loader_arg < kTextureRoleArg) return false;
	const int32_t index = (loader_arg & 0xFFFF) - kTextureRoleArg;
	if (index < 0 || size_t(index) >= kTextureRoleCount) return false;
	role = static_cast<TextureRoleId>(index);
	return true;
}

bool texture_role_renderer_loader(TextureRoleId role, renderer::TextureLoader &out, TextureLoader loader, int alpha_mode) {
	using RL = renderer::TextureLoader;
	if (loader == L::kCount) {
		if (size_t(role) >= kTextureRoleCount) return false;
		loader = texture_role_row(role).loader;
		// The end-of-round cine's images go through their own loader, its two tries (texture_load_rules.h).
		if (role == R::CinematicFade) {
			out = RL::CineFade;
			return true;
		}
	}
	switch (loader) {
	case L::Stage: out = RL::Stage; return true;
	case L::Plain: out = RL::Plain; return true;
	// The archive loader naming the file twice for the sky maps and the tracer smoke, a PCX taking its
	// own palette's luminance as alpha; once (an empty alpha name) for the others, a PCX opaque.
	case L::Archive: out = role == R::SkyCloud || role == R::TracerSmoke ? RL::ArchiveSelfAlpha : RL::Archive; return true;
	case L::File: out = RL::File; return true;
	case L::Menu: out = RL::Menu; return true;
	case L::Ptl: out = RL::Particle; return true;
	case L::Tga: out = RL::Tga; return true;
	case L::Pcx: out = RL::Pcx; return true;
	// The HUD loader in the caller's mode: the role's, alpha only for its alpha-only art.
	case L::Hud: out = (alpha_mode >= 0 ? alpha_mode == 1 : role == R::HudAlphaOnly) ? RL::HudAlpha : RL::HudColor; return true;
	case L::Normal:
	case L::Producer:
	case L::Chunk:
	case L::Pcx8:
	case L::Cube:
	case L::kCount: break;
	}
	return false;
}
uint8_t texture_row_material_flags(const std::string &shader, uint8_t material_flags, uint8_t row_type, uint8_t slot) {
	constexpr uint8_t kTestBits = uint8_t(threedi::THREEDI_MATERIAL_FLAG_ALPHA_TEST | threedi::THREEDI_MATERIAL_FLAG_ALPHA_INVERT);
	if ((material_flags & threedi::THREEDI_MATERIAL_FLAG_ALPHA_TEST) == 0) return material_flags;
	const renderer::ObjectShaderPipelineDescriptor pipeline = renderer::describe_object_shader_pipeline(
			renderer::build_object_shader_key(renderer::classify_object_material(shader, material_flags, 0, 0, 0)));
	// The row's runtime type as the dispatcher reads it [orig: Material_LoadStageTexture @ 0x5B1737].
	const uint8_t runtime = renderer::material_texture_runtime_type(row_type);
	const bool diffuse = (runtime == 0 || runtime == 2 || runtime == 8) && slot != 2;
	const bool normal = runtime == 4 || runtime == 5;
	bool tested = false;
	switch (renderer::object_coverage_source(pipeline.technique)) {
	case renderer::ObjectCoverageSource::DiffuseAlpha: tested = diffuse; break;
	case renderer::ObjectCoverageSource::NormalAlpha: tested = normal; break;
	case renderer::ObjectCoverageSource::VertexDiffuseAlpha:
	case renderer::ObjectCoverageSource::ReflectAlpha:
	case renderer::ObjectCoverageSource::Zero: break;
	}
	return tested ? material_flags : uint8_t(material_flags & ~kTestBits);
}

std::vector<std::string> texture_role_extensions(const TextureRoleRow &row) {
	std::vector<std::string> out;
	if (row.formats & kTextureTga) out.push_back(".tga");
	if (row.formats & kTextureMdt) out.push_back(".mdt");
	if (row.formats & kTexturePcx) out.push_back(".pcx");
	if (row.formats & kTextureDds) out.push_back(".dds");
	if (row.formats & kTexturePng) out.push_back(".png");
	return out;
}

const char *texture_size_rule_token(TextureSizeRule rule) {
	switch (rule) {
	case S::Any: return "any";
	case S::PowerOfTwo: return "power_of_two";
	case S::Exact: return "exact";
	case S::SquarePowerOfTwoAtMost: return "square_power_of_two_at_most";
	case S::MultipleOf: return "multiple_of";
	case S::AtMost: return "at_most";
	case S::QuadrantSplit: return "quadrant_split";
	case S::AtlasPage: return "atlas_page";
	case S::Unknown: return "unknown";
	}
	return "any";
}

io::JsonValue texture_role_json(const TextureRoleRow &row) {
	using io::json_number;
	using io::json_string;
	io::JsonValue out = io::JsonValue::make_object();
	out.set("role", json_string(row.token));
	out.set("words", json_string(row.words));
	out.set("group", json_string(texture_role_group_words(row.group)));
	out.set("loader", json_string(texture_loader_token(row.loader)));
	io::JsonValue formats = io::JsonValue::make_array();
	for (const std::string &extension : texture_role_extensions(row)) formats.push(json_string(extension));
	out.set("formats", std::move(formats));
	io::JsonValue size = io::JsonValue::make_object();
	size.set("rule", json_string(texture_size_rule_token(row.size)));
	size.set("words", json_string(texture_size_words(row)));
	if (row.width) size.set("width", json_number(double(row.width)));
	if (row.height) size.set("height", json_number(double(row.height)));
	out.set("size", std::move(size));
	out.set("alpha", json_string(row.alpha));
	out.set("reads_alpha", io::JsonValue::make_bool(row.reads_alpha));
	out.set("sampling", json_string(row.sampling));
	out.set("missing", json_string(row.missing));
	out.set("witness", json_string(row.witness));
	return out;
}

std::string texture_size_words(const TextureRoleRow &row) {
	switch (row.size) {
	case S::Any: return "any size";
	case S::PowerOfTwo: return "both sides powers of two";
	case S::Exact: return std::to_string(row.width) + " x " + std::to_string(row.height);
	case S::SquarePowerOfTwoAtMost: return "square, a power of two, at most " + std::to_string(row.width);
	case S::MultipleOf: return std::to_string(row.width) + "-pixel cells";
	case S::AtMost: return "at most " + std::to_string(row.width) + " a side (halved to fit)";
	case S::QuadrantSplit: return "at least as tall as it is wide (split in quadrants at its width)";
	case S::AtlasPage:
		return "narrower than its atlas page and no taller: " + std::to_string(row.width) + ", or " + std::to_string(row.height) +
		       " for a bump, mod, mod2x, bumpadd or distort graphic";
	case S::Unknown: return "unknown (NEEDS-RE)";
	}
	return "";
}

} // namespace opennova::editor
