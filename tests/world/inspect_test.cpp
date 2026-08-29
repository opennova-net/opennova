// world::inspect (ADR 0042 d5): the entity directory join (registry rows x AI
// cards by handle, unpresented AI diagnostics appended, editable = brain AND
// live registry slot), the per-entity debug card halves, and the
// EntityCommands set_entity_health/position both-store mutators — the engine
// facts the deleted GDScript DebugEntities join and Dictionary getters carried.
#include <runtime/world/ai.h>
#include <runtime/world/inspect.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

EntityHandle spawn_entity(World &world, int pool, int32_t item_id,
        uint16_t net_id, const char *name, const Vec3 &pos) {
    Entity seed;
    seed.item_id = item_id;
    seed.net_id = net_id;
    seed.name = name;
    seed.position = pos;
    seed.health = 100;
    seed.alive = true;
    seed.spawn_origin = spawn_origin_pack(3, 7);
    seed.team = 2;
    return world.registry.spawn(pool, seed);
}

} // namespace

int main() {
    // --- the directory join -------------------------------------------------
    {
        World world;
        world.registry.configure_pool(0, 8);
        world.registry.configure_pool(1, 4);
        AiSystem ai;
        world.ai = &ai;

        const EntityHandle organic =
                spawn_entity(world, 0, 5311, 101, "alpha", Vec3{1, 2, 3});
        const EntityHandle vehicle =
                spawn_entity(world, 1, 1291, 200, "buggy", Vec3{4, 5, 6});
        // A registry row with no def (item_id 0) but a live brain: not
        // presented, appended as an editable diagnostic.
        const EntityHandle defless =
                spawn_entity(world, 0, 0, 102, "ghost", Vec3{7, 8, 9});
        // A brain whose registry slot a scripted remove despawned.
        const EntityHandle vaporized =
                spawn_entity(world, 0, 5311, 103, "gone", Vec3{2, 2, 2});

        const int organic_ai = ai.attach(organic);
        const int defless_ai = ai.attach(defless);
        const int vaporized_ai = ai.attach(vaporized);
        ai.at(organic_ai)->team = 1;
        ai.at(organic_ai)->net_id = 101;
        ai.at(vaporized_ai)->net_id = 103;
        ai.at(vaporized_ai)->pos[0] = 2 << 16;
        ai.at(vaporized_ai)->pos[1] = 2 << 16;
        ai.at(vaporized_ai)->pos[2] = 2 << 16;
        world.registry.despawn(vaporized);

        const auto rows = inspect::entity_directory(world, &ai);
        // organic + vehicle presented; defless + vaporized appended.
        CHECK(rows.size() == 4);
        CHECK(rows[0].index == 0 && rows[3].index == 3);

        // Presented rows join their AI card by handle (net_id match rides the
        // same identity).
        CHECK(rows[0].wire_handle == organic.packed);
        CHECK(rows[0].presented && rows[0].registry_present);
        CHECK(rows[0].ai_index == organic_ai);
        CHECK(rows[0].editable);
        CHECK(rows[0].net_id == 101);
        CHECK(rows[0].item_id == 5311);
        CHECK(rows[0].name == "alpha");
        CHECK(rows[0].team == 1); // the AI half owns the joined row's team
        CHECK(!rows[0].state_name.empty()); // the joined AI half names its state
        CHECK(rows[0].kind == 3 && rows[0].source_index == 7);
        CHECK(std::fabs(rows[0].mission_position.y - 2.0f) < 1e-6f);

        // A vehicle without a brain is presented but not editable.
        CHECK(rows[1].wire_handle == vehicle.packed);
        CHECK(rows[1].ai_index == -1 && !rows[1].editable);
        CHECK(rows[1].team == 2);
        CHECK(rows[1].state_name.empty());

        // The def-less brain-carrying row is appended, editable (registry
        // slot alive), not presented.
        CHECK(rows[2].wire_handle == defless.packed);
        CHECK(rows[2].ai_index == defless_ai);
        CHECK(!rows[2].presented && rows[2].registry_present && rows[2].editable);
        CHECK(rows[2].name == "ghost");

        // The vaporized brain is appended with registry defaults and is NOT
        // editable (no registry slot to mutate).
        CHECK(rows[3].wire_handle == vaporized.packed);
        CHECK(rows[3].ai_index == vaporized_ai);
        CHECK(!rows[3].presented && !rows[3].registry_present && !rows[3].editable);
        CHECK(rows[3].net_id == 103);   // the AI half still reports its scalars
        CHECK(rows[3].item_id == 0 && rows[3].name.empty());
        CHECK(!rows[3].alive);
        CHECK(std::fabs(rows[3].mission_position.x - 2.0f) < 1e-6f);

        // A joiner passes no AI system: the decoded view never mixes the
        // non-authoritative tooling pool in.
        const auto joiner_rows = inspect::entity_directory(world, nullptr);
        CHECK(joiner_rows.size() == 2);
        CHECK(joiner_rows[0].ai_index == -1 && !joiner_rows[0].editable);

        // --- the card halves ------------------------------------------------
        const inspect::EntityCard whole = inspect::build_entity_card(
                world, &ai, organic, [](int32_t) { return std::string("US01.adm"); });
        CHECK(whole.valid && whole.has_world && whole.has_ai);
        CHECK(whole.handle == organic.packed);
        CHECK(whole.ai_index == organic_ai);
        CHECK(whole.world.handle == static_cast<int32_t>(organic.packed));
        CHECK(whole.world.net_id == 101 && whole.world.item_id == 5311);
        CHECK(whole.world.kind == 3 && whole.world.source_index == 7);
        CHECK(whole.world.pool == 0);
        CHECK(whole.world.health == 100 && whole.world.alive);
        CHECK(whole.ai.wire_handle == static_cast<int32_t>(organic.packed));
        CHECK(whole.ai.net_id == 101);
        CHECK(whole.ai.pool == 0);
        CHECK(whole.ai.item_id == 5311 && whole.ai.name == "alpha");
        CHECK(whole.ai.health == 100 && whole.ai.alive);
        CHECK(whole.ai.has_vehicle_block); // live registry row carries it
        CHECK(!whole.ai.infantry && whole.ai.adm_name.empty());

        // The despawned entity keeps a stable AI card shape with typed
        // registry defaults.
        const inspect::EntityCard despawned =
                inspect::build_entity_card(world, &ai, vaporized);
        CHECK(despawned.valid && !despawned.has_world && despawned.has_ai);
        CHECK(despawned.ai.kind == -1 && despawned.ai.source_index == -1);
        CHECK(despawned.ai.pool == -1);
        CHECK(!despawned.ai.alive && despawned.ai.name.empty());
        CHECK(despawned.ai.net_id == 103);
        CHECK(!despawned.ai.has_vehicle_block);

        // A brainless vehicle yields the world half only.
        const inspect::EntityCard veh_card =
                inspect::build_entity_card(world, &ai, vehicle);
        CHECK(veh_card.valid && veh_card.has_world && !veh_card.has_ai);
        CHECK(veh_card.ai_index == -1);
        CHECK(veh_card.world.name == "buggy");

        CHECK(!inspect::build_entity_card(world, &ai, EntityHandle{}).valid);
    }

    // --- the EntityCommands both-store mutators -----------------------------
    {
        World world;
        world.registry.configure_pool(0, 4);
        AiSystem ai;
        world.ai = &ai;
        const EntityHandle h =
                spawn_entity(world, 0, 5311, 44, "target", Vec3{0, 0, 0});
        const int ai_index = ai.attach(h);

        CHECK(world.commands.set_entity_health(h, 37));
        CHECK(world.registry.get(h)->health == 37);
        CHECK(world.registry.get(h)->alive);
        CHECK(ai.at(ai_index)->health == 37);
        CHECK(world.commands.set_entity_health(h, 0));
        CHECK(!world.registry.get(h)->alive);
        CHECK(ai.at(ai_index)->health == 0);

        CHECK(world.commands.set_entity_position(h, Vec3{6.0f, 5.0f, 7.0f}));
        CHECK(std::fabs(world.registry.get(h)->position.x - 6.0f) < 1e-6f);
        CHECK(std::fabs(world.registry.get(h)->position.y - 5.0f) < 1e-6f);
        CHECK(ai.at(ai_index)->pos[0] == 6 << 16);
        CHECK(ai.at(ai_index)->pos[1] == 5 << 16);
        CHECK(ai.at(ai_index)->pos[2] == 7 << 16);

        // Round-trip through the card: both mirrors show the write.
        const inspect::EntityCard card = inspect::build_entity_card(world, &ai, h);
        CHECK(card.ai.ai_health == 0 && card.world.health == 0);
        CHECK(std::fabs(card.ai.mission_position.z - 7.0f) < 1e-6f);
        CHECK(std::fabs(card.world.mission_position.z - 7.0f) < 1e-6f);

        // No registry slot -> refused, nothing written.
        world.registry.despawn(h);
        CHECK(!world.commands.set_entity_health(h, 90));
        CHECK(ai.at(ai_index)->health == 0);
        CHECK(!world.commands.set_entity_position(h, Vec3{1, 1, 1}));
    }

    std::printf("inspect: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
