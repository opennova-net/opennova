#pragma once

// The two legs of the game's terrain load at a mission start (D-NET-NEW-L2-46).
//
// The connect leg runs at the head of the start, between a joiner's sync-tail wait (its S2C 0x11)
// and its game-start wait (whose head queues the C2S 0x0A), at the same point of a host's and a
// single player's start: it parses the .trn and the .env lines the terrain reads (the mission's
// tile set named there, .TGA and .TSD), loads the height data, builds its mip chain, and loads the
// char map and the foliage map. The mission textures stage, past the objects, the first loadout
// and the render arm's pumps, loads the texture slots (the colour, detail and blend maps) and the
// tile set's atlas, on every arm: a dedicated host's reduced one calls it too.
// [orig: Game_LoadTerrainDuringConnect @0x520710 (the joiner arm's call @0x524DF2) -
//  Terrain_LoadEnvironmentConfig @0x610940 (the call @0x52073B; the tile set's names
//  @0x6109CE..0x610A1C), Terrain_Init @0x60FBE0 (the call @0x52076F) -> PolyTrn_LoadTerrainConfig
//  @0x60E3D0 (the call @0x60FD2D): Terrain_ConfigureAndLoadLodStorage @0x603CC0 (the call
//  @0x60E62F), Terrain_LoadCharmapPCX @0x605A10 (the call @0x60E668), Foliage_LoadFoliageMapPCX
//  @0x605AD0 (the call @0x60E66D); Render_InitMissionTextures @0x587120 (Game_StartMission's call
//  @0x525A6E) - PolyTrn_InitTextures @0x60AAA0 (the full arm's call @0x58715E, the reduced arm's
//  @0x58727A) -> Terrain_LoadTileSetAtlas @0x604A90 (the call @0x60C5BE)]

#include <cstdint>

namespace opennova::terrain {

// What a terrain load reads, by what it is to the game.
enum class TerrainLoadFile : uint8_t {
	Configuration, // the .trn and the .env lines the terrain's parser reads; the tile set's name
	HeightData,    // the .cpt
	CharMap,       // the surface char map (PCX)
	FoliageMap,    // the foliage map (PCX)
	TextureSlot,   // a colour, detail or blend map
	TileSetAtlas,  // the tile set's atlas (.TGA)
};

enum class TerrainLoadLeg : uint8_t {
	Connect,
	MissionTextures,
};

// The leg of the mission start that loads `file`.
constexpr TerrainLoadLeg terrain_load_leg(TerrainLoadFile file) noexcept {
	return file == TerrainLoadFile::TextureSlot || file == TerrainLoadFile::TileSetAtlas
			? TerrainLoadLeg::MissionTextures
			: TerrainLoadLeg::Connect;
}

// Whether a mission start loads the foliage's definitions (their models and :fd textures), which
// load with the render subsystems ahead of the mission textures. A dedicated host's start (in
// session, the authority, no session peer) jumps over the render subsystems and loads none
// (D-NET-NEW-LF-3). The definitions' load first creates the foliage's vertex shaders, the detail
// tier's wind-sway shader and the MODEL tier's GridPlacementVS, so a device backend creates its
// MODEL masks' shader (D-NET-NEW-LF-13) and its tiers' shaders and materials (D-NET-NEW-LF-19)
// there, never in the first frame that draws foliage.
// [orig: Game_StartMission @0x524360 - the dedicated flag @0x524952..0x524964, its test @0x5259B4
//  skipping Render_InitRenderingSubsystems (the call @0x5259D1) and Render_InitTerrainSubsystems
//  (the call @0x5259FD); Render_InitTerrainSubsystems @0x587000 -> Render_InitLodCacheAndFoliage
//  @0x604DA0 (the call @0x587004) -> Foliage_LoadDefAssets @0x601260 (the call @0x604DBC) ->
//  Terrain_CreateFoliageVertexShaders @0x5FF630 (the call @0x601278; the wind-sway shader created
//  @0x5FF6FF, stored to g_FoliageWindSwayVS @0x5FF711; GridPlacementVS's text @0x7DE3A0, read
//  @0x5FF699, created @0x5FF76D)]
constexpr bool loads_foliage_definitions(bool dedicated_host) noexcept {
	return !dedicated_host;
}

// Whether a mission start builds the terrain's LOD cache: the device geometry its patches draw
// from, each LOD level's static vertex and index buffers. It builds in the same render subsystems'
// terrain leg as the foliage's definitions, so a dedicated host's start jumps over it too; its
// mission textures stage still loads the terrain's textures (PolyTrn_InitTextures runs on both
// arms) (D-NET-NEW-L3-32).
// [orig: Game_StartMission @0x524360 - the dedicated test @0x5259B4 skipping
//  Render_InitTerrainSubsystems (the call @0x5259FD); Render_InitTerrainSubsystems @0x587000 ->
//  Render_InitLodCacheAndFoliage @0x604DA0 (the call @0x587004) -> LODTileCache_SetDetailAndRebuild
//  @0x602750 (the call @0x604DAA): each present LOD level (TerrainLodTable_HasLevel @0x6029C0, the
//  call @0x6027A1) builds its buffers (LODTileCache_InitLevelBuffers @0x602310, the call @0x6027B0:
//  GStaticIB_Init @0x6842D0, the call @0x6023A8; GStaticVB_Init @0x683F70, the call @0x6023BB)]
constexpr bool builds_lod_cache(bool dedicated_host) noexcept {
	return !dedicated_host;
}

// Whether a mission start makes the terrain tile cache's page targets: the render targets its
// pages compose into, one per page slot and the extra one. They build in the render subsystems'
// init, which a dedicated host's start jumps over, so it makes none and composes no page. The
// cache's sources are the mission textures stage's and load on both arms: the colour map's
// quadrants, the scorch textures and the tile set's atlas all come from PolyTrn_InitTextures
// (D-NET-NEW-L3-33).
// [orig: Game_StartMission @0x524360 - the dedicated test (the cmp @0x5259B2, the jz @0x5259B4)
//  skipping Render_InitRenderingSubsystems (the call @0x5259D1); Render_InitRenderingSubsystems
//  @0x586EE0 -> Terrain_CreateTileCacheTargets @0x604DD0 (the call @0x586F01): a target per page
//  slot (the loop @0x604DD3..0x604E3C, each made by GTexRT_Create @0x680370, the call @0x604DE8)
//  and the extra target (the call @0x604E46);
//  PolyTrn_InitTextures @0x60AAA0 (Render_InitMissionTextures' full arm's call @0x58715E, the
//  reduced arm's @0x58727A) - the colour map's quadrants (Terrain_SplitTileIntoQuadrants @0x604E60,
//  the calls @0x60B510 / @0x60B964), Terrain_LoadScorchTextures @0x604CE0 (the call @0x60C5B4),
//  Terrain_LoadTileSetAtlas @0x604A90 (the call @0x60C5BE)]
constexpr bool builds_tile_cache_targets(bool dedicated_host) noexcept {
	return !dedicated_host;
}

} // namespace opennova::terrain
