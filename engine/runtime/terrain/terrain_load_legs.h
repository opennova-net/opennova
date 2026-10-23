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

} // namespace opennova::terrain
