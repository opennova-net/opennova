// world::inspect (ADR 0042 d5): the entity directory join (registry rows x AI
// cards by handle, unpresented AI diagnostics appended, editable = brain AND
// live registry slot), the per-entity debug card halves, and the
// EntityCommands set_entity_health/position both-store mutators — the engine
// facts the deleted GDScript DebugEntities join and Dictionary getters carried.
#include <runtime/world/ai.h>
#include <runtime/world/inspect.h>
#include <runtime/world/muzzle_pose.h>
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

// Fixed-point muzzle stub so the report's engaged-only resolve is countable.
struct TestMuzzleProvider : IMuzzlePoseProvider {
    int resolves = 0;
    bool resolve_muzzle_pose(World &, EntityHandle, int32_t out[3]) override {
        ++resolves;
        out[0] = 5 << 16;
        out[1] = 6 << 16;
        out[2] = 7 << 16;
        return true;
    }
};

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

    // --- the per-entity ItemDefAttrib override + the item name / health_max
    //     inspection fields -------------------------------------------------
    {
        World world;
        world.registry.configure_pool(0, 4);
        AiSystem ai;
        world.ai = &ai;
        world.item_names.set(5311, "Rifleman");
        const EntityHandle h =
                spawn_entity(world, 0, 5311, 45, "rifle", Vec3{0, 0, 0});
        world.registry.get(h)->health_max = 120;
        ai.attach(h);

        // The override lands both words and re-derives the per-entity stamps.
        CHECK(world.commands.set_entity_item_attrib(
                h, kItemAttribNoDismember | kItemAttribChangeTeam, 0x2000u));
        const Entity *e = world.registry.get(h);
        CHECK(e->item_attrib == (kItemAttribNoDismember | kItemAttribChangeTeam));
        CHECK(e->item_attrib2 == 0x2000u);
        CHECK(e->is_capture_trigger);
        CHECK(!e->is_spawn_point && !e->leave_corpse && !e->is_ai_capable);
        CHECK(world.commands.set_entity_item_attrib(
                h, kItemAttribAIData | kItemAttribSpawnPoint | kItemAttribLeaveCorpse, 0u));
        e = world.registry.get(h);
        CHECK(e->is_ai_capable && e->is_spawn_point && e->leave_corpse);
        CHECK(!e->is_capture_trigger && e->item_attrib2 == 0u);

        // The card and the directory row carry the new fields.
        const inspect::EntityCard card = inspect::build_entity_card(world, &ai, h);
        CHECK(card.has_world);
        CHECK(card.world.item_attrib ==
                static_cast<int64_t>(kItemAttribAIData | kItemAttribSpawnPoint | kItemAttribLeaveCorpse));
        CHECK(card.world.health_max == 120);
        CHECK(card.world.item_name == "Rifleman");
        const auto rows = inspect::entity_directory(world, &ai);
        CHECK(rows.size() == 1 && rows[0].item_name == "Rifleman");

        // No registry slot -> refused.
        world.registry.despawn(h);
        CHECK(!world.commands.set_entity_item_attrib(h, 0u, 0u));
    }

    // --- the AI debug join (ai_debug_report) + the AiDetail brain block -----
    {
        World world;
        world.registry.configure_pool(0, 8);
        AiSystem ai;
        world.ai = &ai;
        TestMuzzleProvider muzzle;
        world.muzzle_pose_provider = &muzzle;

        // Channel 1: a 3-node route (node coords in 16.16).
        ai.nav.channels.resize(2);
        ai.nav.channels[1].loopflag = 1;
        ai.nav.channels[1].count = 3;
        ai.nav.channels[1].entries[0] = 0;
        ai.nav.channels[1].entries[1] = 1;
        ai.nav.channels[1].entries[2] = 2;
        for (int k = 0; k < 3; ++k) {
            NavEntry node;
            node.f[0] = 0x8000; // arrival radius
            node.f[1] = (10 + k) << 16;
            node.f[2] = (20 + k) << 16;
            node.f[3] = 2 << 16;
            node.wait_ticks = k * 62;
            ai.nav.nodes.push_back(node);
        }

        const EntityHandle router =
                spawn_entity(world, 0, 5311, 301, "router", Vec3{10, 20, 2});
        const EntityHandle shooter =
                spawn_entity(world, 0, 5311, 302, "shooter", Vec3{1, 1, 0});
        const EntityHandle idle =
                spawn_entity(world, 0, 5311, 303, "idler", Vec3{3, 3, 0});
        world.registry.get(router)->group_id = 5;

        // A routed SM brain targeting the idler (packed+1 rebase).
        const int router_ai = ai.attach(router);
        AiEntity *ra = ai.at(router_ai);
        ra->pos[0] = 10 << 16;
        ra->pos[1] = 20 << 16;
        ra->brain.f[AiBrain::kCurState] = 16; // GROUND_FOLLOWWP
        ra->brain.f[AiBrain::kWpChannel] = 1;
        ra->brain.f[AiBrain::kWpNode] = 2;
        ra->brain.f[AiBrain::kWpDistance] = 12;
        ra->brain.f[AiBrain::kTargetSlot] = idle.packed + 1;
        ra->brain.f[AiBrain::kPriorityTarget] = idle.packed + 1;
        ra->brain.f[AiBrain::kCombatTimer] = 40;
        ra->brain.f[AiBrain::kFireDelay] = 9;
        ra->brain.f[AiBrain::kRetargetTimer] = 48;
        ra->brain.f[AiBrain::kCooldownPair] =
                static_cast<int32_t>((7u << 16) | 3u);
        ra->brain.f[AiBrain::kSpeedA] = 100;
        ra->brain.f[AiBrain::kSpeedB] = 60;
        ra->brain.f[AiBrain::kWorkPosX] = 11 << 16;
        ra->brain.f[AiBrain::kWorkPosY] = 21 << 16;
        ra->brain.f[AiBrain::kWorkPosZ] = 2 << 16;
        ra->brain.f[AiBrain::kActiveYaw] = 0x1000;
        ra->brain.f[AiBrain::kActivePitch] = 0x2000;
        ra->profile.type = 2;
        ra->profile.class_priority[1] = 90;
        ra->profile.fov_primary = 96;
        ra->profile.range_primary = 250;
        ra->profile.approach_cap = 30 << 16;
        ra->slot.f[1] = 0x200; // berserk bit
        ra->slot.f[AiSlot::kSightRange] = 80 << 16;
        ra->slot.f[AiSlot::kAttackRange] = 50 << 16;

        // An engaged infantry brain: yellow alert, aim solution live.
        const int shooter_ai = ai.attach(shooter);
        AiEntity *sa = ai.at(shooter_ai);
        sa->inf.active = true;
        sa->inf.move_mode = 7;
        sa->inf.combat_target = router;
        sa->inf.aim_valid = true;
        sa->inf.aim_heading = 0x4000;
        sa->inf.aim_pitch = -0x800;
        sa->inf.damage_timer = 6;
        sa->inf.same_target_ticks = 4;
        sa->inf.combat_move_timer = 11;
        sa->slot.bytes()[AiSlot::kAlertByte] = 1;

        // A quiet brain: no target -> the muzzle resolve must skip it.
        const int idle_ai = ai.attach(idle);
        ai.at(idle_ai)->pos[0] = 3 << 16;
        ai.at(idle_ai)->pos[1] = 3 << 16;

        // Group + counters state.
        world.relations.group(5).alert = TriggerRelations::kAlertRed;
        world.relations.group(5).initial_count = 4;
        world.relations.group(5).live_count = 3;
        ai.unported_calls = 2;
        ai.find_target_calls = 8;
        ai.scheduler.budget = 128;

        const inspect::AiDebugReport report =
                inspect::ai_debug_report(world, ai);
        CHECK(report.rows.size() == 3);

        const inspect::AiOverlayRow &rr = report.rows[0];
        CHECK(rr.ai_index == router_ai && rr.handle == router.packed);
        CHECK(rr.name == "router" && rr.group_id == 5);
        CHECK(rr.alive && !rr.infantry);
        CHECK(rr.pos[0] == 10 << 16 && rr.pos[1] == 20 << 16);
        CHECK(rr.state == 16 && rr.state_name == "GROUND_FOLLOWWP");
        CHECK(rr.wp_channel == 1 && rr.wp_node == 2 && rr.wp_distance == 12);
        CHECK(rr.target_valid && rr.target_handle == idle.packed);
        CHECK(rr.target_name == "idler");
        CHECK(rr.target_pos[0] == 3 << 16); // the target's AI 16.16 mirror
        CHECK(rr.sight_range_q16 == 80 << 16 && rr.attack_range_q16 == 50 << 16);
        CHECK(rr.combat_timer == 40 && rr.fire_delay == 9);
        CHECK(rr.muzzle_valid && rr.muzzle[0] == 5 << 16);

        const inspect::AiOverlayRow &sr = report.rows[1];
        CHECK(sr.infantry && sr.move_mode == 7);
        CHECK(sr.alert == 1);
        CHECK(sr.target_valid && sr.target_handle == router.packed);
        CHECK(sr.target_name == "router");
        CHECK(sr.target_pos[0] == 10 << 16); // the target's AI 16.16 mirror
        CHECK(sr.aim_valid && sr.aim_heading == 0x4000 && sr.aim_pitch == -0x800);
        CHECK(sr.damage_timer == 6 && sr.combat_move_timer == 11);

        // Engaged-only muzzle resolve: router + shooter, never the idler.
        CHECK(muzzle.resolves == 2);
        CHECK(!report.rows[2].target_valid && !report.rows[2].muzzle_valid);

        // The route: channel 1 with its resolved nodes and one follower.
        CHECK(report.channels.size() == 1);
        const inspect::AiNavChannelRow &ch = report.channels[0];
        CHECK(ch.index == 1 && ch.loopflag == 1);
        CHECK(ch.nodes.size() == 3);
        CHECK(ch.nodes[1].pos[0] == 11 << 16 && ch.nodes[1].pos[1] == 21 << 16);
        CHECK(ch.nodes[1].radius_q16 == 0x8000 && ch.nodes[2].wait_ticks == 124);
        CHECK(ch.followers == 1);

        // Groups: only the seeded one.
        CHECK(report.groups.size() == 1);
        CHECK(report.groups[0].id == 5);
        CHECK(report.groups[0].alert == TriggerRelations::kAlertRed);
        CHECK(report.groups[0].initial_count == 4);
        CHECK(report.groups[0].live_count == 3);

        CHECK(report.counters.brain_count == 3);
        CHECK(report.counters.scheduler_budget == 128);
        CHECK(report.counters.unported_calls == 2);
        CHECK(report.counters.find_target_calls == 8);
        CHECK(report.counters.event_count == 0);

        // The deep card carries the new brain/profile/slot/infantry fields.
        const inspect::EntityCard card =
                inspect::build_entity_card(world, &ai, router);
        CHECK(card.ai.target_valid &&
                card.ai.target_handle == static_cast<int32_t>(idle.packed));
        CHECK(card.ai.target_name == "idler");
        CHECK(card.ai.priority_target_valid &&
                card.ai.priority_target_handle == static_cast<int32_t>(idle.packed));
        CHECK(card.ai.combat_timer == 40 && card.ai.fire_delay == 9);
        CHECK(card.ai.retarget_timer == 48);
        CHECK(card.ai.cooldown_a == 3 && card.ai.cooldown_b == 7);
        CHECK(card.ai.speed_a == 100 && card.ai.speed_b == 60);
        CHECK(std::fabs(card.ai.work_pos.x - 11.0f) < 1e-6f);
        CHECK(std::fabs(card.ai.work_pos.y - 21.0f) < 1e-6f);
        CHECK(card.ai.turret_yaw == 0x1000 && card.ai.turret_pitch == 0x2000);
        CHECK(card.ai.profile_type == 2); // filled for every brain now
        CHECK(card.ai.profile_class_priority[1] == 90);
        CHECK(card.ai.profile_fov_primary == 96);
        CHECK(card.ai.profile_range_primary == 250);
        CHECK(card.ai.profile_approach_cap == 30 << 16);
        CHECK(card.ai.slot_control_bits == 0x200);
        const inspect::EntityCard shooter_card =
                inspect::build_entity_card(world, &ai, shooter);
        CHECK(shooter_card.ai.aim_valid && shooter_card.ai.aim_heading == 0x4000);
        CHECK(shooter_card.ai.damage_timer == 6);
        CHECK(shooter_card.ai.same_target_ticks == 4);
        CHECK(shooter_card.ai.combat_move_timer == 11);
    }

    // --- the report's row cap (followers still count the whole pool) --------
    {
        World world;
        world.registry.configure_pool(0, 4);
        AiSystem ai;
        world.ai = &ai;
        ai.nav.channels.resize(2);
        ai.nav.channels[1].count = 1;
        ai.nav.channels[1].entries[0] = 0;
        ai.nav.nodes.push_back(NavEntry{});
        for (int i = 0; i < 260; ++i) {
            const int idx = ai.attach(EntityHandle::make(0, i));
            ai.at(idx)->brain.f[AiBrain::kWpChannel] = 1;
        }
        const inspect::AiDebugReport report =
                inspect::ai_debug_report(world, ai);
        CHECK(static_cast<int>(report.rows.size()) ==
                inspect::AiDebugReport::kMaxRows);
        CHECK(report.channels.size() == 1 && report.channels[0].followers == 260);
        CHECK(report.counters.brain_count == 260);
    }

    std::printf("inspect: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
