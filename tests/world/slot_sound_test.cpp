// The infantry slot-sound emissions [orig: the odd/even-tick anim-event sound
// blocks — Entity_UpdateInfantryAI @0x4bf169-0x4bf2b0, Entity_UpdateInfantryPlayerBody
// @0x4b76f1-0x4b78a8 — plus the landing pair @0x4bf87f/@0x4b7f7c and the death
// scream @0x4b9ca3]: tick parity per body, the foot-level Z dip, the
// water/groundEntity-OBJ/surface-3/ground slot pick, the SSAudio foley bits,
// empty-slot no-ops, the "default" profile fallback, and the night scream gate.
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "terrain_query/height_field.h"
#include "terrain_query/surface_tiles.h"
#include "terrain_query/surface_type_map.h"
#include <til/til.h>
#include <til/til_io.h>
#include "world/ai.h"
#include "world/world.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;
namespace slot = opennova::audio;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// Flat terrain at raw16 0 (world z 0), all sectors mapped.
struct Field {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;
    Field() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// Root source whose gait clip carries a caller-set trigger word + capsule bottom.
struct TestSource : IRootMotionSource {
    std::set<int> clips;
    uint32_t events = 0;
    int32_t capsule_bottom = 0;
    bool has_clip(int, int id) const override { return clips.count(id) != 0; }
    int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
    bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
        if (clips.count(id) == 0) return false;
        ++phase;
        out = RootMotionFrame{};
        out.events = events;
        out.capsule_bottom = capsule_bottom;
        return true;
    }
};

// A minimal profile table: "default" + one soldier profile, seeded straight on
// the world (the host parses SndProf.def into the same table).
const char kProfiles[] =
    "begin \"default\"\n"
    "     SSLFootGND     DEF_FOOT_L\n"
    "end\n"
    "begin \"SP_Test\"\n"
    "     sounddeath     T_DEATH\n"
    "     SSNightDead    T_DEATH_K\n"
    "     SSFallDead     T_FALLDEAD\n"
    "     SSFallAlive    T_LAND\n"
    "     SSLFootGND     T_DIRT_L\n"
    "     SSRFootGND     T_DIRT_R\n"
    "     SSLFootSnow    T_SNOW_L\n"
    "     SSRFootSnow    T_SNOW_R\n"
    "     SSLFootOBJ     T_OBJ_L\n"
    "     SSRFootOBJ     T_OBJ_R\n"
    "     SSFootWater    T_WATER\n"
    "     SSAudio1       T_AUD1\n"
    "     SSAudio6       T_AUD6\n"
    "end\n"
    "begin \"SP_TestFemale\"\n"
    "     SSLFootGND     T_FEMALE_DIRT_L\n"
    "end\n";

struct Rig {
    Field field;
    World world;
    AiSystem ai;
    TestSource src;
    AiEntity *e = nullptr;

    explicit Rig(bool local_player = false) {
        world.registry.configure_pool(0, 4);
        world.sound_profiles.parse(kProfiles, sizeof(kProfiles) - 1);
        ai.terrain = &field.field;
        ai.root_motion = &src;
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.health = 100;
        seed.alive = true;
        const EntityHandle h = world.registry.spawn(0, seed);
        e = ai.at(ai.attach(h));
        e->inf.active = true;
        e->inf.is_local_player = local_player;
        e->health = 100;
        e->profile.sound_profile =
            static_cast<int16_t>(world.sound_profiles.index_of("SP_Test"));
        e->pos[2] = fx(0.0);
        src.clips = {anim_state::kIdle, anim_state::kWalkForward};
        e->inf.anim_state = anim_state::kIdle;
        if (local_player) world.cached.local_player = h;
    }

    void run(uint32_t from, uint32_t to_excl, bool authority = true) {
        TickContext ctx;
        ctx.world = &world;
        ctx.is_authority = authority;
        for (uint32_t t = from; t < to_excl; ++t) {
            ctx.logic_tick = t;
            ai.tick(world, ctx);
        }
    }

    std::vector<SoundSlotEvent> take() {
        std::vector<SoundSlotEvent> out = world.slot_sounds;
        world.slot_sounds.clear();
        return out;
    }
};

void test_npc_feet_odd_ticks_only() {
    Rig rig;
    rig.src.events = 0x1; // left foot every frame
    rig.run(0, 1);        // tick 0 (even) — NPC consumes on odd ticks
    CHECK(rig.take().empty());
    rig.run(1, 2); // tick 1 (odd)
    auto evs = rig.take();
    CHECK(evs.size() == 1);
    if (!evs.empty()) {
        CHECK(evs[0].slot == slot::kSlotFootLGround);
        CHECK(std::string(evs[0].set_name) == "T_DIRT_L");
    }
    rig.run(2, 3); // tick 2 (even) — nothing again
    CHECK(rig.take().empty());
}

void test_player_feet_even_ticks_only() {
    Rig rig(/*local_player=*/true);
    rig.src.events = 0x2; // right foot
    rig.run(1, 2);        // odd tick — player consumes on EVEN ticks
    CHECK(rig.take().empty());
    rig.run(2, 3); // even tick
    auto evs = rig.take();
    CHECK(evs.size() == 1);
    if (!evs.empty()) CHECK(evs[0].slot == slot::kSlotFootRGround);
}

void test_player_female_profile_selection() {
    Rig rig(/*local_player=*/true);
    Entity *player = rig.world.registry.get(rig.e->handle);
    CHECK(player != nullptr);
    if (player == nullptr) return;
    player->player_class = 5;
    player->minimap_net_id = 0x8407;
    rig.world.character_traits.set(0x8407, true);
    rig.e->profile.sound_profile_female = static_cast<int16_t>(
            rig.world.sound_profiles.index_of("SP_TestFemale"));
    rig.src.events = 0x1;
    rig.run(0, 1);
    auto evs = rig.take();
    CHECK(evs.size() == 1);
    if (!evs.empty())
        CHECK(std::string(evs[0].set_name) == "T_FEMALE_DIRT_L");

    // An unknown packed id stays on the primary profile.
    player->minimap_net_id = 0x1234;
    rig.run(2, 3);
    evs = rig.take();
    CHECK(evs.size() == 1);
    if (!evs.empty()) CHECK(std::string(evs[0].set_name) == "T_DIRT_L");

    // NPC defs already author their sex-specific primary profile. A colliding
    // minimap id cannot redirect a non-player body through the player table.
    player->player_class = 0;
    player->minimap_net_id = 0x8407;
    rig.run(4, 5);
    evs = rig.take();
    CHECK(evs.size() == 1);
    if (!evs.empty()) CHECK(std::string(evs[0].set_name) == "T_DIRT_L");
}

void test_foot_dip_water_and_surface_picks() {
    Rig rig;
    rig.src.events = 0x3; // both feet
    rig.src.capsule_bottom = fx(0.5);
    rig.e->pos[2] = fx(2.0);

    // Ground: dipped z = 1.5 in the emitted position.
    rig.run(1, 2);
    auto evs = rig.take();
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotFootLGround);
        CHECK(evs[1].slot == slot::kSlotFootRGround);
        CHECK(evs[0].pos[2] == fx(1.5));
    }

    // Feet under the water plane -> the single water slot for both feet.
    rig.world.env.water_z = fx(1.75); // dipped 1.5 < 1.75
    rig.run(3, 4);
    evs = rig.take();
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotFootWater);
        CHECK(evs[1].slot == slot::kSlotFootWater);
        CHECK(std::string(evs[0].set_name) == "T_WATER");
    }
    rig.world.env.water_z = 0;

    // Surface type 3 (snow) via a 4x4 charmap of 3s over sector slot 1.
    static const uint8_t snow[16] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    static const int grid[256] = {1}; // cell (0,0) slot 1; rest empty (ocean)
    rig.world.surface_map.data = snow;
    rig.world.surface_map.width = 4;
    rig.world.surface_map.height = 4;
    rig.world.surface_map.sector_grid = grid;
    rig.e->pos[0] = fx(10.0);
    rig.e->pos[1] = -fx(10.0); // mission y negated in the sampler row math
    rig.run(5, 6);
    evs = rig.take();
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotFootLSnow);
        CHECK(evs[1].slot == slot::kSlotFootRSnow);
    }
}

void test_foot_obj_pick() {
    // D-SND-11: a live entity+0x28 groundEntity link picks the OBJ pair. The
    // pass reads LAST tick's link (the sound block precedes the resolve), and
    // with no collision world the terrain-cache fallback clears it each tick —
    // the probe's store is unconditional, null on a miss [orig: the +0x28 store
    // in Entity_RaycastGroundHeightAndObject @ 0x414370].
    Rig rig;
    rig.src.events = 0x3; // both feet
    rig.src.capsule_bottom = fx(0.5);
    rig.e->pos[2] = fx(2.0);
    Entity plat;
    plat.health = 100;
    plat.alive = true;
    const EntityHandle ph = rig.world.registry.spawn(0, plat);

    // Standing on the platform (the link as the probe left it last tick).
    rig.world.registry.get(rig.e->handle)->ground_target = ph;
    rig.run(1, 2);
    auto evs = rig.take();
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotFootLObject);
        CHECK(evs[1].slot == slot::kSlotFootRObject);
        CHECK(std::string(evs[0].set_name) == "T_OBJ_L");
        CHECK(std::string(evs[1].set_name) == "T_OBJ_R");
    }

    // Staleness: the fallback (no collision world) clears the link, so the next
    // pair falls through to ground without any reseed. Ticks 2..3, because the
    // two halves of org1's per-tick work sit on OPPOSITE tick parities: the
    // resolve/fallback that CLEARS the link runs on even ticks
    // [orig: gate kong 155809 under `outYaw.X = tickCounter & 1` kong
    // 155519-155523] while the anim-event sound pass that READS it runs on odd
    // [orig: @0x4bf144-0x4bf156]. Tick 2 clears, tick 3 reads the cleared link.
    // (The old 3..4 window skipped the clearing tick entirely and only worked
    // because we used to run the physics block every tick.)
    rig.run(2, 4);
    evs = rig.take();
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotFootLGround);
        CHECK(evs[1].slot == slot::kSlotFootRGround);
    }

    // Water outranks the link: feet under the plane -> the single water slot.
    rig.world.registry.get(rig.e->handle)->ground_target = ph;
    rig.world.env.water_z = fx(1.75);
    rig.run(5, 6);
    evs = rig.take();
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotFootWater);
        CHECK(evs[1].slot == slot::kSlotFootWater);
    }
    rig.world.env.water_z = 0;

    // The link outranks snow: surface 3 under the feet, link live -> still OBJ.
    static const uint8_t snow[16] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    static const int grid[256] = {1};
    rig.world.surface_map.data = snow;
    rig.world.surface_map.width = 4;
    rig.world.surface_map.height = 4;
    rig.world.surface_map.sector_grid = grid;
    rig.e->pos[0] = fx(10.0);
    rig.e->pos[1] = -fx(10.0);
    rig.world.registry.get(rig.e->handle)->ground_target = ph;
    rig.run(7, 8);
    evs = rig.take();
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotFootLObject);
        CHECK(evs[1].slot == slot::kSlotFootRObject);
    }
}

void test_foley_bits_and_empty_slot_noop() {
    Rig rig;
    rig.src.events = 0x20 | 0x400 | 0x40; // SSAudio1 + SSAudio6 + SSAudio2(empty)
    rig.run(1, 2);
    auto evs = rig.take();
    // SSAudio2 is unauthored in the profile -> no event for bit 0x40.
    CHECK(evs.size() == 2);
    if (evs.size() == 2) {
        CHECK(evs[0].slot == slot::kSlotAudio1);
        CHECK(std::string(evs[0].set_name) == "T_AUD1");
        CHECK(evs[1].slot == slot::kSlotAudio6);
    }
}

void test_default_profile_fallback() {
    Rig rig;
    rig.e->profile.sound_profile = -1; // unresolved -> the "default" profile
    rig.src.events = 0x1;
    rig.run(1, 2);
    auto evs = rig.take();
    CHECK(evs.size() == 1);
    if (!evs.empty()) CHECK(std::string(evs[0].set_name) == "DEF_FOOT_L");
}

void test_landing_pair_alive_and_dead() {
    Rig rig;
    rig.e->inf.airborne = true;
    rig.e->pos[0] = fx(10.0);
    rig.e->pos[1] = fx(10.0);
    rig.e->pos[2] = fx(0.0); // feet at ground -> clearance <= 0 resolves the landing
    rig.e->inf.vel[2] = -1000;
    // An EVEN tick: the landing edge (and its sound) is part of org1's
    // gravity/resolver/edge block, which runs on even ticks only
    // [orig: gate kong 155809 under `outYaw.X = tickCounter & 1` kong
    // 155519-155523]. The dead-body case below already used tick 2 and was
    // unaffected, which is what pointed at the phase rather than the policy.
    rig.run(2, 3);
    auto evs = rig.take();
    bool saw_land = false;
    for (const auto &ev : evs)
        if (ev.slot == slot::kSlotFallAlive && std::string(ev.set_name) == "T_LAND")
            saw_land = true;
    CHECK(saw_land);
    CHECK(!rig.e->inf.airborne);

    // Dead body landing -> SSFallDead.
    Rig rig2;
    rig2.e->inf.anim_state = anim_state::kDeathFire; // an already-posed corpse (0x82 family)
    rig2.src.clips.insert(anim_state::kDeathFire);
    rig2.e->health = 0;
    if (Entity *ent = rig2.world.registry.get(rig2.e->handle)) ent->health = 0;
    rig2.run(1, 2); // settle the death edge bookkeeping
    rig2.take();
    rig2.e->inf.airborne = true;
    rig2.e->pos[0] = fx(10.0);
    rig2.e->pos[1] = fx(10.0);
    rig2.e->pos[2] = fx(0.0);
    rig2.run(2, 3);
    auto evs2 = rig2.take();
    bool saw_dead = false;
    for (const auto &ev : evs2)
        if (ev.slot == slot::kSlotFallDead && std::string(ev.set_name) == "T_FALLDEAD")
            saw_dead = true;
    CHECK(saw_dead);
}

void test_death_scream_day_and_night() {
    Rig rig;
    if (Entity *ent = rig.world.registry.get(rig.e->handle)) ent->health = 0;
    rig.run(1, 2);
    auto evs = rig.take();
    bool saw = false;
    for (const auto &ev : evs)
        if (ev.slot == slot::kSlotDeath && std::string(ev.set_name) == "T_DEATH") saw = true;
    CHECK(saw);

    Rig rig2;
    rig2.world.mission_attrib_flags = 0x100000; // EnableNVG = the night gate
    if (Entity *ent = rig2.world.registry.get(rig2.e->handle)) ent->health = 0;
    rig2.run(1, 2);
    auto evs2 = rig2.take();
    bool saw_night = false;
    for (const auto &ev : evs2)
        if (ev.slot == slot::kSlotNightDeath && std::string(ev.set_name) == "T_DEATH_K")
            saw_night = true;
    CHECK(saw_night);
}

void test_local_player_death_scream_composite() {
    // The prefix map + composite builder [orig: Entity_GetBodyModelPrefix
    // @0x5280F0; SoundProfile_FindByEntityAndType @0x528180 "%s_%s" over the
    // {suffix, type} table @0x82F548].
    char buf[24];
    CHECK(std::string(slot::compose_entity_sound_set(
                  0, slot::kEntitySoundDeath, buf, sizeof(buf))) == "BM1_DEATH");
    CHECK(std::string(slot::compose_entity_sound_set(
                  1, slot::kEntitySoundDeathNight, buf, sizeof(buf))) == "BM1_DEATH_K");
    CHECK(std::string(slot::compose_entity_sound_set(
                  7, slot::kEntitySoundDeath, buf, sizeof(buf))) == "BF1_DEATH");
    CHECK(std::string(slot::compose_entity_sound_set(
                  9, slot::kEntitySoundDeath, buf, sizeof(buf))) == "BM6_DEATH");
    CHECK(std::string(slot::compose_entity_sound_set(
                  11, slot::kEntitySoundDeathNight, buf, sizeof(buf))) == "RF1_DEATH_K");
    CHECK(std::string(slot::compose_entity_sound_set(3, 99, buf, sizeof(buf))).empty());

    // Day: the local player's death edge emits the body-model composite — not
    // the T_DEATH the profile authors in slot 7 (the org2 leg never reads the
    // profile slots) [orig: @0x4b4c4a-0x4b4c6a].
    Rig rig(true);
    if (Entity *ent = rig.world.registry.get(rig.e->handle)) {
        ent->health = 0;
        ent->anim_slot = 2; // BM2
    }
    rig.run(1, 2);
    auto evs = rig.take();
    bool saw = false;
    for (const auto &ev : evs)
        if (ev.slot == slot::kSlotDeath && std::string(ev.set_name) == "BM2_DEATH")
            saw = true;
    CHECK(saw);

    // Night: the _K composite through the same EnableNVG gate; the default
    // anim-slot 0 resolves BM1 [orig: the null/zero -> type-1 default @0x5280F8].
    Rig rig2(true);
    rig2.world.mission_attrib_flags = 0x100000;
    if (Entity *ent = rig2.world.registry.get(rig2.e->handle)) ent->health = 0;
    rig2.run(1, 2);
    auto evs2 = rig2.take();
    bool saw_night = false;
    for (const auto &ev : evs2)
        if (ev.slot == slot::kSlotNightDeath && std::string(ev.set_name) == "BM1_DEATH_K")
            saw_night = true;
    CHECK(saw_night);
}

void test_surface_sampler_defaults() {
    using opennova::terrain::SurfaceTypeMap;
    using opennova::terrain::surface_type_at_fixed;
    SurfaceTypeMap m;
    // No charmap -> 1 [orig: @0x606519].
    CHECK(surface_type_at_fixed(m, fx(10), fx(10)) == 1);
    static const uint8_t raster[4] = {5, 6, 7, 8};
    static const int grid[256] = {1};
    m.data = raster;
    m.width = 2;
    m.height = 2;
    m.sector_grid = grid;
    // Cell (0,0) -> slot 1 -> quadrant 0; fine (10,10)>>9 -> raster[0].
    CHECK(surface_type_at_fixed(m, fx(10.0), -fx(10.0)) == 5);
    // An unmapped cell -> the ocean default 7 [orig: @0x606573].
    CHECK(surface_type_at_fixed(m, fx(600.0), -fx(10.0)) == 7);
}

void test_surface_sampler_placed_tile_override() {
    using opennova::terrain::SurfaceTileEntry;
    using opennova::terrain::SurfaceTypeMap;
    using opennova::terrain::surface_type_at_fixed;
    // D-SND-15: a position covered by a placed tile returns the tileset .TSD
    // table's entry for the tile index — 0 (TSD_NULL) included; retail never
    // falls back to the charmap under a tile, and shipped JO carries no .TSD
    // so its placed tiles all read 0. [orig: Terrain_GetSurfaceTypeAtPosition
    // @ 0x606510, walk @ 0x6065ca-0x606601, table read @ 0x60660c]
    static const uint8_t raster[4] = {5, 6, 7, 8};
    static const int grid[256] = {1};
    SurfaceTypeMap m;
    m.data = raster;
    m.width = 2;
    m.height = 2;
    m.sector_grid = grid;
    // One tile at mission (16..32, 16..32): x_fixed = 16u, z_fixed stores the
    // NEGATED mission y base [orig: entry +4].
    static const SurfaceTileEntry tiles[3] = {
            {fx(16.0), -fx(16.0), 12},
            {fx(16.0), -fx(16.0), 30}, // later duplicate: first containing wins
            {fx(592.0), -fx(16.0), 12}, // covers the unmapped sector cell below
    };
    m.tiles = tiles;
    m.tile_count = 3;
    static uint8_t table[256];
    table[12] = 3;  // TSD_SNOW
    table[30] = 9;  // TSD_MUD (must lose to the first entry)
    m.tile_surface = table;
    // Inside the tile square (inclusive edges): the table entry wins.
    CHECK(surface_type_at_fixed(m, fx(20.0), fx(20.0)) == 3);
    CHECK(surface_type_at_fixed(m, fx(16.0), fx(16.0)) == 3);
    CHECK(surface_type_at_fixed(m, fx(32.0), fx(32.0)) == 3);
    // Outside the square: the charmap class.
    CHECK(surface_type_at_fixed(m, fx(33.0), fx(20.0)) == raster[0]);
    // No .TSD table (retail memset default): a covered position reads 0, not
    // the charmap.
    m.tile_surface = nullptr;
    CHECK(surface_type_at_fixed(m, fx(20.0), fx(20.0)) == 0);
    // The unmapped-cell early return precedes the walk: (600, 20) sits inside
    // the third tile, yet the unmapped sector still answers 7 [orig: @0x606573].
    m.tile_surface = table;
    CHECK(surface_type_at_fixed(m, fx(600.0), fx(20.0)) == 7);
}

void test_surface_tile_resolvers() {
    // The engine-side D-SND-15 resolvers (terrain_query/surface_tiles.h):
    // the .TSD probe derives `<tilestrip base>.tsd` and fills the table; an
    // absent file leaves retail's zeroed default.
    opennova::terrain::SurfaceTileFileSource files;
    files.has_file = [](const std::string &n) { return n == "trntile10.tsd"; };
    files.read_file = [](const std::string &n, std::vector<uint8_t> &out) {
        if (n != "trntile10.tsd") return false;
        static const char text[] = "INDEX_12 TSD_SNOW\r\n";
        out.assign(text, text + sizeof(text) - 1);
        return true;
    };
    uint8_t table[256];
    opennova::terrain::resolve_tileset_surface_table(files, "trntile10.tga", table);
    CHECK(table[12] == 3); // TSD_SNOW
    CHECK(table[0] == 0);
    opennova::terrain::resolve_tileset_surface_table(files, "other.tga", table);
    CHECK(table[12] == 0); // no .TSD -> the memset default

    // The til0 fold keeps exactly the three walk fields per entry.
    opennova::TilFile til;
    til.entries.push_back(opennova::make_til_overlay_entry(1, 2, 12, 0));
    std::vector<uint8_t> bytes;
    std::string err;
    CHECK(opennova::save_til(til, bytes, err));
    const auto tiles = opennova::terrain::surface_tiles_from_til_bytes(bytes);
    CHECK(tiles.size() == 1);
    if (tiles.size() == 1) {
        CHECK(tiles[0].x_fixed == til.entries[0].x_fixed);
        CHECK(tiles[0].z_fixed == til.entries[0].z_fixed);
        CHECK(tiles[0].tile_index == 12);
    }
    CHECK(opennova::terrain::surface_tiles_from_til_bytes({}).empty());
}

} // namespace

int main() {
    test_npc_feet_odd_ticks_only();
    test_player_feet_even_ticks_only();
    test_player_female_profile_selection();
    test_foot_dip_water_and_surface_picks();
    test_foot_obj_pick();
    test_foley_bits_and_empty_slot_noop();
    test_default_profile_fallback();
    test_landing_pair_alive_and_dead();
    test_death_scream_day_and_night();
    test_local_player_death_scream_composite();
    test_surface_sampler_defaults();
    test_surface_sampler_placed_tile_override();
    test_surface_tile_resolvers();
    if (failures == 0) std::printf("slot_sound_test OK\n");
    return failures == 0 ? 0 : 1;
}
