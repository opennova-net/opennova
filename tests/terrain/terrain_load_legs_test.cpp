// The two legs of a mission start's terrain load (runtime/terrain/terrain_load_legs.h,
// D-NET-NEW-L2-46): the connect leg, ahead of a joiner's C2S 0x0A, reads the configuration (the
// tile set's name in it), the height data and the two maps; the texture slots and the tile set's
// atlas wait for the mission textures stage.
#include <runtime/terrain/terrain_load_legs.h>

#include <cstdio>

using opennova::terrain::TerrainLoadFile;
using opennova::terrain::TerrainLoadLeg;
using opennova::terrain::loads_foliage_definitions;
using opennova::terrain::terrain_load_leg;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

// Terrain_LoadEnvironmentConfig and Terrain_Init's PolyTrn_LoadTerrainConfig at the connect leg.
static_assert(terrain_load_leg(TerrainLoadFile::Configuration) == TerrainLoadLeg::Connect);
static_assert(terrain_load_leg(TerrainLoadFile::HeightData) == TerrainLoadLeg::Connect);
// PolyTrn_InitTextures at the mission textures stage.
static_assert(terrain_load_leg(TerrainLoadFile::TextureSlot) == TerrainLoadLeg::MissionTextures);

int main() {
	// The connect leg: the .trn / .env configuration, the height data, the char and foliage maps.
	CHECK(terrain_load_leg(TerrainLoadFile::Configuration) == TerrainLoadLeg::Connect);
	CHECK(terrain_load_leg(TerrainLoadFile::HeightData) == TerrainLoadLeg::Connect);
	CHECK(terrain_load_leg(TerrainLoadFile::CharMap) == TerrainLoadLeg::Connect);
	CHECK(terrain_load_leg(TerrainLoadFile::FoliageMap) == TerrainLoadLeg::Connect);
	// The mission textures stage: the colour, detail and blend maps and the tile set's atlas (the
	// atlas's name stands from the connect leg; its pixels load here).
	CHECK(terrain_load_leg(TerrainLoadFile::TextureSlot) == TerrainLoadLeg::MissionTextures);
	CHECK(terrain_load_leg(TerrainLoadFile::TileSetAtlas) == TerrainLoadLeg::MissionTextures);
	// The foliage's definitions load with the render subsystems, which a dedicated host's start
	// jumps over (D-NET-NEW-LF-3).
	CHECK(loads_foliage_definitions(false));
	CHECK(!loads_foliage_definitions(true));
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("terrain_load_legs: ok\n");
	return 0;
}
