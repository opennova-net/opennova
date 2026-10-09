// The ammo table's shared lookups (runtime/world/ammo_table.h): the entity-face
// material cost a round pays to go on through a face. Five materials pass, at
// the table's cost; every other material costs nothing (the round sim reads
// that as an absorbing face).
// [orig: table @0x82D034; Entity_ClampKineticEnergy @0x4E9070..0x4E9200]
#include <cstdint>
#include <cstdio>

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
    if (failures == 0) {
        std::printf("ammo_table_test: OK\n");
        return 0;
    }
    std::printf("ammo_table_test: %d FAILED\n", failures);
    return 1;
}
