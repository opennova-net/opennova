#include <runtime/renderer/texture_roles.h>

#include <iterator>

#include <base/io/strutil.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/til/til.h>
#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/object_shader_template.h>
#include <runtime/terrain/terrain_tile_composer.h>

namespace opennova::renderer {

namespace {

using L = TextureLoader;
using S = TextureSizeRule;
using R = TextureRoleId;

constexpr uint8_t kStageFormats = kTextureTga | kTextureMdt | kTexturePcx | kTextureDds;
constexpr uint8_t kArchiveFormats = kTextureTga | kTextureMdt | kTexturePcx | kTextureDds;
constexpr uint8_t kMenuFormats = kTextureTga | kTexturePcx | kTextureDds | kTexturePng;

// A row built up column by column: each row names only what it sets.
struct Role {
	TextureRole row;
	constexpr Role(R id, L loader, uint8_t formats) : row() {
		row.id = id;
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
	constexpr Role no_alpha() const {
		Role out = *this;
		out.row.reads_alpha = false;
		return out;
	}
};

constexpr TextureRole kRoles[] = {
	// --- Models (.3di material rows) ----------------------------------------------------------------
	// [orig: Texture_LoadByNameWithChannel @ 0x58B470; Material_LoadStageTexture @ 0x5B16F0]
	Role(R::ModelDiffuse, L::Stage, kStageFormats).row,
	// [orig: Texture_LoadByNameWithChannel @ 0x58B470 (slot 2)]
	Role(R::ModelDetail, L::Stage, kStageFormats).row,
	// [orig: Material_ApplyShaderParameters @ 0x58DC36] (D-RMAT-11)
	Role(R::ModelFlipFrame, L::Stage, kStageFormats).row,
	// [orig: Texture_LoadAndRegister @ 0x58B790 (.PCX @ 0x58B8A5..0x58B8F7)]
	Role(R::ModelPlain, L::Plain, kTextureTga | kTextureMdt | kTexturePcx).row,
	// [orig: Texture_LoadAsNormalMap @ 0x58C480; the 512 cap, flag 0x1000 @ 0x5B1782]
	Role(R::ModelNormalMap, L::Normal, kTextureMdt).size(S::AtMost, kNormalMapSideCap).row,
	// [orig: Texture_LoadAsNormalMap @ 0x58C985..0x58CAED (the kernel), the swap @ 0x58C715..0x58C737]
	Role(R::ModelHeightNormal, L::Normal, kTextureTga | kTextureDds).size(S::PowerOfTwo).row,
	// [orig: sub_58A430 @ 0x58A430 from sub_58A580 @ 0x58A5DA]
	Role(R::ModelHorizon, L::Producer, kTextureTga | kTextureDds).row,
	// [orig: sub_58A430 @ 0x58A430 from sub_58CE10 @ 0x58CE6A]
	Role(R::ModelOcclusion, L::Producer, kTextureTga | kTextureDds).row,
	// [orig: the chunk loaders @ 0x58F350, @ 0x58F470, @ 0x58F590]
	Role(R::ModelChunk, L::Chunk, 0).size(S::Unknown).no_alpha().row,
	// --- Terrain (.trn) -------------------------------------------------------------------------------
	// [orig: PolyTrn_InitTextures @ 0x60B389, @ 0x60B3BE; Game_StartMission @ 0x525AD8]
	Role(R::TerrainColourMap, L::Tga, kTextureTga)
	        .size(S::Exact, terrain::kTerrainColourMapSide, terrain::kTerrainColourMapSide)
	        .row,
	// [orig: Terrain_ParseConfigCallback @ 0x60F7D0; PolyTrn_InitTextures @ 0x60B15D..0x60B1A6, @ 0x60B1B8,
	// the split @ 0x60B2C1; sub_520AA0 @ 0x520B4E]
	Role(R::TerrainBlendMap, L::Tga, kTextureTga).size(S::QuadrantSplit).row,
	// [orig: PolyTrn_InitTextures @ 0x60ABEF..0x60AC13 (STAGE); by its own name through the TGA reader
	// for the far blend @ 0x60AC76..0x60AD2A]
	Role(R::TerrainSplatDetail, L::Stage, kTextureTga | kTextureDds).row,
	// [orig: GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270]
	Role(R::TerrainFarDetail, L::Tga, kTextureTga).row,
	// [orig: Texture_GenerateNormalMap @ 0x58C070 (by its own name: a .tga through the TGA reader, a .pcx
	// through the PCX reader, @ 0x58C116..0x58C159) from PolyTrn_InitTextures @ 0x60B155; the near
	// texture by STAGE @ 0x60ADA0; its checksum by name @ 0x60AAF4]
	Role(R::TerrainDetailCoefficient, L::Stage, kTextureTga | kTexturePcx | kTextureDds).size(S::PowerOfTwo).row,
	// [orig: PolyTrn_InitTextures (polytrn_detailmap2, STAGE); by its own name through the TGA reader
	// @ 0x60AB4C, @ 0x60AFC9]
	Role(R::TerrainSecondDetail, L::Stage, kTextureTga | kTextureDds).row,
	// [orig: Terrain_LoadTileSetAtlas @ 0x604A90, cells @ 0x604B7C]
	Role(R::TerrainTileAtlas, L::Tga, kTextureTga).size(S::MultipleOf, TIL_ATLAS_TILE_PIXELS).row,
	// [orig: Terrain_LoadScorchTextures @ 0x604CE0]
	Role(R::TerrainScorch, L::Stage, kStageFormats).no_alpha().row,
	// [orig: Foliage_LoadFoliageMapPCX @ 0x605AD0; Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066D0]
	Role(R::TerrainFoliageMap, L::Pcx8, kTexturePcx).size(S::SquarePowerOfTwoAtMost, 1024).no_alpha().row,
	// [orig: sub_605A10 @ 0x605A31 (the 8-bit PCX reader), its side @ 0x605A82..0x605AA0;
	// Terrain_GetSurfaceTypeAtPosition @ 0x606519, @ 0x6065C6]
	Role(R::TerrainCharMap, L::Pcx8, kTexturePcx).size(S::SquarePowerOfTwoAtMost, 1024).no_alpha().row,
	// --- Environment --------------------------------------------------------------------------------
	// The archive loader naming the file twice, so a PCX takes its own palette's luminance as alpha
	// [orig: Terrain_InitRenderingResources @ 0x578A97; Texture_LoadFromArchive @ 0x58BC21..0x58BCFB]
	Role(R::SkyCloud, L::ArchiveSelfAlpha, kArchiveFormats).row,
	// [orig: WaterRing_LoadResources @ 0x5DDC90]
	Role(R::WaterWake, L::Archive, kArchiveFormats).row,
	// [orig: WeatherParticle_LoadTextures @ 0x5DE840]
	Role(R::WeatherDrop, L::Stage, kStageFormats).row,
	// [orig: PlayerInfo_InitPreviewModel @ 0x56010C (sub_58A690)]
	Role(R::PreviewCube, L::Cube, kTextureDds).row,
	// --- Effects --------------------------------------------------------------------------------------
	// [orig: CParticleTextureEntry_ProbeSizeFromDisk @ 0x5DFAA0; CParticleManager_BuildTextureAtlases
	// @ 0x5E8F19 (the page), @ 0x5E9185, @ 0x5E91BB (the passes); CParticleAtlas_TryPlaceEntry
	// @ 0x5E2C30, @ 0x5E2C57]
	Role(R::ParticleGraphic, L::Particle, kTextureTga).size(S::AtlasPage, 1024, 256).row,
	// [orig: Scar_LoadTextures @ 0x5CC2E0 (the table @ 0x8413A8)]
	Role(R::ImpactScar, L::Archive, kArchiveFormats).row,
	// The archive loader naming the file twice [orig: CEffectEmitterPool_CreateShaders @ 0x5DC8F0]
	Role(R::TracerSmoke, L::ArchiveSelfAlpha, kArchiveFormats).row,
	// A character's face animation (.grm): its base texture, the base's .MDT twin and its two eye
	// textures, each name's extension made .TGA (the base's .MDT) and loaded by STAGE. The loader the
	// IDB calls a shadow decal's runs only over the GRM slots (sub_57FC00 over sub_57FCE0's).
	// [orig: Shadow_DecalLoadTextures @ 0x588040 (the base @ 0x5880EA, its .MDT @ 0x588117, the eyes
	// @ 0x58814A, @ 0x588180); FaceAnimConfig_ParseProperty @ 0x5886A0]
	Role(R::FaceTexture, L::Stage, kStageFormats).row,
	// [orig: WeaponDef_CreateBlendNamedMaterial @ 0x540180]
	Role(R::SightCard, L::Stage, kStageFormats).row,
	// [orig: ViewFx_InitShadersAndTextures @ 0x5CF8E0]
	Role(R::ViewEffect, L::Stage, kStageFormats).row,
	// --- HUD -------------------------------------------------------------------------------------------
	// [orig: HUD_LoadAllTextures @ 0x59DDA0; HUD_LoadImageAsTexture @ 0x591550]
	Role(R::HudColour, L::HudColor, kTextureTga | kTexturePcx).row,
	// [orig: HUD_LoadImageAsTexture @ 0x5916AE..0x5916BE; HUD_LoadAllTextures @ 0x59E248..0x59E2CB]
	Role(R::HudAlphaOnly, L::HudAlpha, kTextureTga | kTexturePcx).row,
	// [orig: Texture_LoadFromFile_0 @ 0x58FE00]
	Role(R::HudFileArt, L::File, kTextureTga | kTexturePcx).row,
	// g_HUDAhzBackTextureName, STAGE with flags 0x40001
	Role(R::HudAttitude, L::Stage, kStageFormats).row,
	// [orig: sub_59B120 @ 0x59B19F..0x59B1BD]
	Role(R::HudMfd, L::Pcx, kTexturePcx).size(S::PowerOfTwo).no_alpha().row,
	// [orig: Minimap_InitOverviewTexture @ 0x573FA0]
	Role(R::MapOverview, L::Pcx, kTexturePcx).no_alpha().row,
	// [orig: CNetworkIcons_LoadTextures @ 0x4C2CF0]
	Role(R::NetIcon, L::Tga, kTextureTga).row,
	// [orig: CTipSystem_Init @ 0x5B6970]
	Role(R::TipArt, L::Stage, kStageFormats).row,
	// [orig: BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0]
	Role(R::BoardBox, L::Tga, kTextureTga).row,
	// --- Menus and screens -----------------------------------------------------------------------------
	// [orig: CTextureManager_LoadOrFindTexture @ 0x654980]
	Role(R::MenuImage, L::Menu, kMenuFormats).row,
	// [orig: CTextureManager_LoadOrFindTexture @ 0x654980 (frames)]
	Role(R::MenuFrameStencil, L::Menu, kMenuFormats).row,
	// [orig: CTextureManager_LoadOrFindTexture @ 0x654980 (frames)]
	Role(R::MenuFrameBrush, L::Menu, kMenuFormats).row,
	// [orig: CTextureManager_LoadOrFindTexture @ 0x654980]
	Role(R::MenuCursor, L::Menu, kMenuFormats).row,
	// The multiplayer text laid out for 800 x 600 art [orig: Render_LoadingScreen @ 0x521D10]
	Role(R::LoadingScreen, L::Pcx, kTexturePcx).size(S::Exact, 800, 600).no_alpha().row,
	// [orig: Game_ShowLoadingScreen @ 0x4A5420]
	Role(R::BootSplash, L::Pcx, kTexturePcx).no_alpha().row,
	// [orig: Game_ShowStartMissionSplash @ 0x520820]
	Role(R::SplashCursor, L::Tga, kTextureTga).row,
	// The end-of-round cine's own loader, its two tries (texture_load_rules.h)
	// [orig: CinematicFadeEvent_LoadTexture @ 0x570D00]
	Role(R::CinematicFade, L::CineFade, kTextureTga | kTexturePcx | kTextureDds).row,
};

static_assert(std::size(kRoles) == kTextureRoleCount, "every TextureRoleId has exactly one row");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kTextureRoleCount; ++i)
		if (static_cast<size_t>(kRoles[i].id) != i) return false;
	return true;
}
static_assert(rows_in_order(), "the roles follow TextureRoleId's order");

} // namespace

const TextureRole &texture_role(TextureRoleId id) {
	const size_t index = static_cast<size_t>(id);
	return kRoles[index < kTextureRoleCount ? index : 0];
}

bool texture_role_takes(const TextureRole &role, std::string_view extension) {
	const std::string e = strutil::to_lower(extension);
	if (e == ".tga") return (role.formats & kTextureTga) != 0;
	if (e == ".mdt") return (role.formats & kTextureMdt) != 0;
	if (e == ".pcx") return (role.formats & kTexturePcx) != 0;
	if (e == ".dds") return (role.formats & kTextureDds) != 0;
	if (e == ".png") return (role.formats & kTexturePng) != 0;
	return false;
}

std::vector<std::string> texture_role_extensions(const TextureRole &role) {
	std::vector<std::string> out;
	if (role.formats & kTextureTga) out.push_back(".tga");
	if (role.formats & kTextureMdt) out.push_back(".mdt");
	if (role.formats & kTexturePcx) out.push_back(".pcx");
	if (role.formats & kTextureDds) out.push_back(".dds");
	if (role.formats & kTexturePng) out.push_back(".png");
	return out;
}

// [orig: PolyTrn_InitTextures @ 0x60AAF4, @ 0x60AB4C, @ 0x60AC76..0x60AD2A, @ 0x60AFC9;
// Texture_GenerateNormalMap @ 0x58C116..0x58C159]
bool texture_role_read_by_name(TextureRoleId role) {
	return role == R::TerrainDetailCoefficient || role == R::TerrainSplatDetail || role == R::TerrainSecondDetail;
}

// [orig: HUD_LoadAllTextures @ 0x59E248..0x59E26A; WeaponDefs_ParseLineCallback @ 0x544966, @ 0x5449A6,
// @ 0x544A0B, @ 0x544A52; CTextureManager_LoadOrFindTexture @ 0x654980]
bool def_texture_field_role(std::string_view field, TextureRoleId &out) {
	if (field == "hudicon" || field == "hudclipgfx_texture" || field == "hudrndgfx_texture" || field == "crosshair" ||
	    field == "crosshair_secondary" || field == "commanders_x" || field == "hud_loadout_select" ||
	    field == "hud_image") {
		out = R::HudAlphaOnly;
		return true;
	}
	if (field == "loadout_menu_icon") {
		out = R::MenuImage;
		return true;
	}
	if (field == "texture") {
		out = R::SightCard;
		return true;
	}
	return false;
}

uint8_t texture_row_material_flags(const std::string &shader, uint8_t material_flags, uint8_t row_type, uint8_t slot) {
	constexpr uint8_t kTestBits = uint8_t(threedi::THREEDI_MATERIAL_FLAG_ALPHA_TEST | threedi::THREEDI_MATERIAL_FLAG_ALPHA_INVERT);
	if ((material_flags & threedi::THREEDI_MATERIAL_FLAG_ALPHA_TEST) == 0) return material_flags;
	const ObjectShaderPipelineDescriptor pipeline =
			describe_object_shader_pipeline(build_object_shader_key(classify_object_material(shader, material_flags, 0, 0, 0)));
	// The row's runtime type as the dispatcher reads it [orig: Material_LoadStageTexture @ 0x5B1737].
	const uint8_t runtime = material_texture_runtime_type(row_type);
	const bool diffuse = (runtime == 0 || runtime == 2 || runtime == 8) && slot != 2;
	const bool normal = runtime == 4 || runtime == 5;
	bool tested = false;
	switch (object_coverage_source(pipeline.technique)) {
	case ObjectCoverageSource::DiffuseAlpha: tested = diffuse; break;
	case ObjectCoverageSource::NormalAlpha: tested = normal; break;
	case ObjectCoverageSource::VertexDiffuseAlpha:
	case ObjectCoverageSource::ReflectAlpha:
	case ObjectCoverageSource::Zero: break;
	}
	return tested ? material_flags : uint8_t(material_flags & ~kTestBits);
}

// The technique's rules (render-material-re.md, "What the diffuse's alpha means by the material" and
// the 2026-08-22 technique audit): a flag-bit-0 material cuts out by the alpha the technique tests
// (object_coverage_source, texture_row_material_flags); FF_*_AB blends by Diffuse1.a; the Phong
// effects read Diffuse1.a as specular brightness (_psPhong.fx), _psPhong2.fx as brightness and the
// PhongMap channels' weight (ObjectSpecularSource); the _MT stage multiplies the diffuse's alpha by
// the detail's [orig: _FFP.fx TECHNIQUE_NORMAL, TSSAlpha(1, Modulate, Texture, Current)]; a normal
// row's .tga is a height [orig: Texture_LoadAsNormalMap @ 0x58C985..0x58CAED, the height in A].
TextureAlphaMeaning texture_row_alpha_meaning(const std::string &shader, uint8_t material_flags, uint8_t row_type,
		uint8_t slot, const std::string &name) {
	const uint8_t runtime = material_texture_runtime_type(row_type);
	const bool normal = runtime == 4 || runtime == 5;
	if (normal && strutil::to_lower(name).size() >= 4 && strutil::to_lower(name).substr(name.size() - 4) == ".tga")
		return TextureAlphaMeaning::Height;
	if ((material_flags & threedi::THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0) return TextureAlphaMeaning::CutOut;
	const bool diffuse = (runtime == 0 || runtime == 2 || runtime == 8) && slot != 2 && slot != 3 && slot != 4;
	if ((runtime == 0 || runtime == 2 || runtime == 8) && slot == 2) return TextureAlphaMeaning::Detail;
	if (!diffuse) return TextureAlphaMeaning::Unused;
	const ObjectShaderPipelineDescriptor pipeline =
			describe_object_shader_pipeline(build_object_shader_key(classify_object_material(shader, material_flags, 0, 0, 0)));
	switch (pipeline.specular_source) {
	case ObjectSpecularSource::AnalyticPow8DiffuseAlpha: return TextureAlphaMeaning::Specular;
	case ObjectSpecularSource::PhongMapLookupDiffuseAlpha: return TextureAlphaMeaning::PhongMapWeight;
	case ObjectSpecularSource::None:
	case ObjectSpecularSource::AnalyticPow16: break;
	}
	if (pipeline.blend == ObjectBlendMode::AlphaBlend &&
	    object_coverage_source(pipeline.technique) == ObjectCoverageSource::DiffuseAlpha)
		return TextureAlphaMeaning::Blend;
	return TextureAlphaMeaning::Unused;
}

} // namespace opennova::renderer
