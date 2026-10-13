// The genx row's class callback, Entity_UpdateParentTransform: every store of
// the def's own +0x1C4 puts it on a genx def's entity (the creation's item
// init, the AINODEPATH 0 command), a death that installs its callback and the
// AINODEPATH 1 command take it off, and each visit (every pool-1 tick, a
// pool-2 row's cohort) saves the pose, follows the ground entity, falls again
// on a dead one and registers the ambient loop on the batch's last tick.
// [orig: g_EntityClassPhysicsTable row 'genx' @0x82AC10 ->
//  Entity_UpdateParentTransform @0x4A88B0; Entity_SpawnFromBMSRecord
//  @0x40EC7A; Entity_UpdatePool1Slot @0x4B8E53; Entity_UpdateAllEntities
//  @0x4C22E7; Entity_SpawnDeathPieces @0x49394B; Entity_ApplyCommand
//  @0x43B235 / @0x43B24E]
#include <formats/def/def.h>
#include <runtime/mission/item_traits.h>
#include <runtime/world/destruction.h>
#include <runtime/world/match.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

#include "item_pool_step.h"

using namespace opennova;
using namespace opennova::world;
using namespace opennova::def;
using test_world::step_item_pool;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// ITEMS.DEF 101995 as shipped, a genx decoration in its shape, and a def on
// the null row.
const char kItemsDef[] =
    "begin \"Communications dish #2 S & D\"\r\n"
    "  id 101995\r\n"
    "  type object\r\n"
    "  graphic Comdish2\r\n"
    "  husk Comd2X\r\n"
    "  hp 225\r\n"
    "  ai_function gnrc\r\n"
    "  move_function genx\r\n"
    "  soundloop_1 LP_COMDISH\r\n"
    "end\r\n"
    "\r\n"
    "begin \"Genx decoration\"\r\n"
    "  id 101996\r\n"
    "  type decoration\r\n"
    "  graphic deco\r\n"
    "  hp 1000\r\n"
    "  ai_function gnrc\r\n"
    "  move_function genx\r\n"
    "end\r\n"
    "\r\n"
    "begin \"Plain object\"\r\n"
    "  id 101997\r\n"
    "  type object\r\n"
    "  graphic plain\r\n"
    "  hp 225\r\n"
    "  ai_function gnrc\r\n"
    "end\r\n";

constexpr uint16_t kDish = 1995;
constexpr uint16_t kDeco = 1996;
constexpr uint16_t kPlain = 1997;

EntityHandle spawn(World &w, int pool, uint16_t item_id) {
    Entity e;
    e.kind = EntityKind::Item;
    e.item_id = item_id;
    e.health = 100;
    e.alive = true;
    return w.registry.spawn(pool, e);
}

void loop_every_region(World &w, uint16_t item_id) {
    ItemDeathTraits *traits = w.tables.item_death_traits.get_mutable(item_id);
    if (traits == nullptr) return;
    for (auto &loop : traits->regional_loops) loop = "LP_COMDISH";
}

} // namespace

int main() {
    DefItemsFile file = {};
    CHECK(def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItemsDef),
                                 sizeof(kItemsDef) - 1, &file) == 0);
    auto heap = std::make_unique<World>();
    World &w = *heap;
    for (int pool = 0; pool <= 3; ++pool) w.registry.configure_pool(pool, 8);
    w.weather.tod_fixed24 = uint32_t(12 * 65536) << 8;
    const EntityHandle dish = spawn(w, 1, kDish);
    const EntityHandle deco = spawn(w, 2, kDeco);
    const EntityHandle plain = spawn(w, 1, kPlain);
    Entity clone_seed;
    clone_seed.kind = EntityKind::Item;
    clone_seed.item_id = kDish;
    clone_seed.item_section_piece = true;
    clone_seed.section_clone = true;
    const EntityHandle clone = w.registry.spawn(1, clone_seed);
    mission::resolve_item_traits(w, file, {});
    loop_every_region(w, kDish);
    loop_every_region(w, kDeco);

    // The creation stores the def's own callback: the genx rows ride, the
    // null row's and the section clone's pinned one do not.
    CHECK(w.registry.get(dish)->parent_follow_update);
    CHECK(w.registry.get(dish)->drop_motion == DropMotion::Ride);
    CHECK(w.registry.get(deco)->drop_motion == DropMotion::Ride);
    CHECK(!w.registry.get(plain)->parent_follow_update);
    CHECK(w.registry.get(plain)->drop_motion == DropMotion::None);
    CHECK(w.registry.get(clone)->parent_follow_update);
    CHECK(w.registry.get(clone)->drop_motion == DropMotion::None);
    // A resweep leaves the callback the ride itself installed.
    w.registry.get(dish)->drop_motion = DropMotion::Fall;
    mission::resolve_item_traits(w, file, {});
    CHECK(w.registry.get(dish)->drop_motion == DropMotion::Fall);
    w.registry.get(dish)->drop_motion = DropMotion::Ride;

    // Each pool-1 visit registers the loop on the batch's last tick alone,
    // for a live, unhusked row.
    w.rules.last_tick_of_batch = true;
    w.logic_tick = 1;
    step_item_pool(w, 1);
    CHECK(w.out.sound_emitters.size() == 1);
    if (w.out.sound_emitters.size() == 1) {
        CHECK(w.out.sound_emitters[0].source_handle == dish.packed);
        CHECK(w.out.sound_emitters[0].set_name == "LP_COMDISH");
    }
    w.out.sound_emitters.clear();
    w.rules.last_tick_of_batch = false;
    step_item_pool(w, 1);
    CHECK(w.out.sound_emitters.empty());
    w.rules.last_tick_of_batch = true;
    w.registry.get(dish)->engine_flags |= kEntityFlagHusk;
    step_item_pool(w, 1);
    CHECK(w.out.sound_emitters.empty());
    w.registry.get(dish)->engine_flags &= ~kEntityFlagHusk;
    // A non-authority visit (a joiner's replica row) runs the same callback.
    {
        TickContext joiner;
        joiner.world = &w;
        joiner.is_authority = false;
        joiner.logic_tick = w.logic_tick;
        w.out.sound_emitters.clear();
        w.update_pool1_slot(*w.registry.get(dish), joiner);
        CHECK(w.out.sound_emitters.size() == 1);
        if (w.out.sound_emitters.size() == 1)
            CHECK(w.out.sound_emitters[0].source_handle == dish.packed);
    }

    // A pool-2 row runs it on its slot's cohort, one tick in eight.
    w.out.sound_emitters.clear();
    w.logic_tick = deco.slot() + 1;
    step_item_pool(w, 2);
    CHECK(w.out.sound_emitters.empty());
    w.logic_tick = deco.slot() + 8;
    step_item_pool(w, 2);
    CHECK(w.out.sound_emitters.size() == 1);
    if (w.out.sound_emitters.size() == 1)
        CHECK(w.out.sound_emitters[0].source_handle == deco.packed);
    w.out.sound_emitters.clear();

    // The ride follows its ground entity's delta and falls again, 0x4000 a
    // tick down with no ground link, when that entity is dead.
    Entity ground_seed;
    ground_seed.kind = EntityKind::Item;
    ground_seed.position = {10, 10, 0};
    const EntityHandle ground = w.registry.spawn(1, ground_seed);
    Entity &rider = *w.registry.get(dish);
    rider.position = {10, 12, 0};
    rider.ground_target = ground;
    stamp_saved_live_pose(*w.registry.get(ground));
    w.registry.get(ground)->position.x = 11;
    update_dropped_object(w, rider);
    CHECK(rider.position.x == 11 && rider.position.y == 12);
    CHECK(rider.drop_motion == DropMotion::Ride);
    stamp_saved_live_pose(*w.registry.get(ground));
    w.registry.get(ground)->engine_flags |= kEntityFlagDead;
    update_dropped_object(w, rider);
    CHECK(rider.drop_motion == DropMotion::Fall);
    CHECK(!rider.ground_target.valid());
    CHECK(rider.veh.slide_z == -0x4000);

    // The AINODEPATH command's two stores: nullsub_28, then the def's own.
    rider.group_id = 3;
    CHECK(w.commands.apply_group_ai_command(3, EntityCommands::kAiNodePath, 1, 0, 0) == 1);
    CHECK(rider.drop_motion == DropMotion::None && rider.motor_suspended);
    CHECK(w.commands.apply_group_ai_command(3, EntityCommands::kAiNodePath, 0, 0, 0) == 1);
    CHECK(rider.drop_motion == DropMotion::Ride && !rider.motor_suspended);

    // A death the piece spawn refuses keeps the callback; one it accepts
    // installs its own in its place.
    ItemDeathTraits *traits = w.tables.item_death_traits.get_mutable(kDish);
    CHECK(traits != nullptr && traits->has_husk);
    rider.engine_flags |= kEntityFlagHusk;
    spawn_death_pieces(w, rider);
    CHECK(rider.drop_motion == DropMotion::Ride);
    rider.engine_flags &= ~kEntityFlagHusk;
    spawn_death_pieces(w, rider);
    CHECK(rider.death_motion == DeathMotionMode::Generic);
    CHECK(rider.drop_motion == DropMotion::None);

    // The boat rows' callback stores its +0x1C4 even where the piece spawn
    // refused the death (here: already husked). [orig: Entity_ProcessBuildingDeath
    //  @0x494420 - the store @0x49446F]
    rider.drop_motion = DropMotion::Ride;
    rider.death_motion = DeathMotionMode::None;
    rider.engine_flags |= kEntityFlagHusk;
    traits->unit_type = 5;
    traits->husk_model_loaded = true;
    entity_update_death_transforms(w, rider, /*silent=*/true);
    CHECK(rider.death_motion == DeathMotionMode::Static);
    CHECK(rider.drop_motion == DropMotion::None);

    def_free_items(&file);
    if (failures != 0) {
        std::printf("genx_parent_follow: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("genx_parent_follow: ok\n");
    return 0;
}
