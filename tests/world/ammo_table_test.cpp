// The ammo table's shared lookups (runtime/world/ammo_table.h): the surface rows
// of the impact tag table, and the entity-face material cost a round pays to go
// on through a face. Five materials pass, at
// the table's cost; every other material costs nothing (the round sim reads
// that as an absorbing face).
// [orig: table @0x82D034; Entity_ClampKineticEnergy @0x4E9070..0x4E9200]
#include <cstdint>
#include <cstdio>
#include <string>

#include <runtime/world/ammo_table.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    // A face byte b plays tag row b + 4: flesh, glass, cloth, water, foliage.
    CHECK(material_energy_cost(19) == 10 * 65536);
    CHECK(material_energy_cost(15) == 10 * 65536);
    CHECK(material_energy_cost(16) == 4 * 65536);
    CHECK(material_energy_cost(7) == 4 * 65536);
    CHECK(material_energy_cost(17) == 8 * 65536);
    int passing = 0;
    for (int material = 0; material < 256; ++material)
        passing += material_energy_cost(static_cast<uint8_t>(material)) != 0 ? 1 : 0;
    CHECK(passing == 5);

    // A surface class plays row class + 4: a bullet face's byte past the table the
    // obj row, the terrain's class past it the dirt row.
    // [orig: Weapon_RaycastAndSpawnImpact @0x4e8867; AmmoDef_ProcessImpactEffect
    //  @0x40a1bf; Terrain_GetSurfaceTypeAtPosition @0x606510 result + 4]
    CHECK(kSurfaceImpactTagOffset == 4);
    CHECK(surface_impact_effect_tag(0) == 4 && surface_impact_effect_tag(19) == 23);
    CHECK(surface_impact_effect_tag(23) == 27 && surface_impact_effect_tag(24) == 4);
    CHECK(surface_impact_effect_tag(-1) == 4 && surface_impact_effect_tag(255) == 4);
    CHECK(terrain_impact_effect_tag(1) == 5 && terrain_impact_effect_tag(7) == kWaterImpactEffectTag);
    CHECK(terrain_impact_effect_tag(24) == 5 && terrain_impact_effect_tag(-1) == 5);
    CHECK(std::string(kImpactEffectTagNames[surface_impact_effect_tag(19)]) == "flesh");
    if (failures == 0) {
        std::printf("ammo_table_test: OK\n");
        return 0;
    }
    std::printf("ammo_table_test: %d FAILED\n", failures);
    return 1;
}
