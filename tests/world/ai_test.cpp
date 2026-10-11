// AI subsystem foundation tests: state enum, struct layout, AIEvent ring, the
// AI_BeginUpdate per-entity movement phase, the infantry state-machine dispatcher + transitions,
// and the byte-exact trivial handler ports. Driven by a manual World + tick.
#include <cstdio>
#include <memory>
#include <cstring>
#include <vector>

#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/body_anim.h>
#include <runtime/world/collision.h>
#include <runtime/world/local_player.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>

#include <array>
#include <unordered_map>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool streq(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

// The vehicle death chain rows (world-wac-ai-re section 19) - own function per the
// main()-frame __chkstk overflow gotcha.
void test_vehicle_death_rows() {
    // ---- the vehicle death chain: rows 21 (DYING) and 23 (GROUND_DEAD) ----
    // [orig: AI_TransitionToDeath_GroundVehicle @0x467b20 / AI_TickState_VehicleDying
    // @0x467cd0 / AI_HandleEvent_VehicleDying @0x457f50 / AI_TransitionToDestroyed_
    // Vehicle @0x467de0 / AI_HandleEvent_ConsumeAll @0x458080; world-wac-ai-re §19]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(1, 4);
        Entity seed;
        seed.team = 1;
        seed.group_id = 3;
        seed.health = 0;
        const EntityHandle h = w.registry.spawn(1, seed);
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = true;
        int idx = sys.attach(h);
        AiEntity &e = *sys.at(idx);
        e.team = 1;
        e.health = 0;
        e.vel_x = 100; e.vel_y = 0; // slow (< 1057): the enter queues the destroy event
        e.brain.f[AiBrain::kCurState] = 21;

        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(21).enter(ctx);
        CHECK(e.brain.f[AiBrain::kAlert] == 2);            // the alert block ran
        CHECK(e.brain.f[AiBrain::kStep] == 16);            // [orig: ai_data[7] = 16]
        CHECK(w.script.relations.group(3).alert == TriggerRelations::kAlertRed);
        bool queued4 = false;
        for (int i = 0; i < sys.events.count(); ++i)
            if (sys.events.at(i).type() == 4) queued4 = true;
        CHECK(queued4);                                    // slow -> destroy event now

        // The dying event handler routes ONLY type 4 -> pending 23.
        AiEventEntry dmg{}; dmg.f[0] = 1;
        AiThinkCtx ctx_dmg{&sys, &e, &w, &dmg};
        sys.row(21).event(ctx_dmg);
        CHECK(e.brain.f[AiBrain::kPendState] != 23);
        AiEventEntry destroy{}; destroy.f[0] = 4;
        AiThinkCtx ctx_dst{&sys, &e, &w, &destroy};
        sys.row(21).event(ctx_dst);
        CHECK(e.brain.f[AiBrain::kPendState] == 23);

        // Enter 23: team cleared (a wreck goes teamless), moveStep 62, corpse timer 0.
		w.registry.get(h)->veh.stuck_ticks = 500;
		sys.row(23).enter(ctx);
		CHECK(e.brain.f[AiBrain::kStep] == 62);
        CHECK(e.team == 0);
        CHECK(w.registry.get(h)->team == 0);
		CHECK(w.registry.get(h)->veh.stuck_ticks == 0); // respawn timer reset

		// The dead row swallows everything: no pending change from any event.
		e.brain.f[AiBrain::kPendState] = 0;
        AiEventEntry late{}; late.f[0] = 3;
        AiThinkCtx ctx_late{&sys, &e, &w, &late};
        sys.row(23).event(ctx_late);
        CHECK(e.brain.f[AiBrain::kPendState] == 0);
    }

    // ---- the dying tick: still-moving clears the work fields; stopping queues 4 ----
    // [orig: AI_TickState_VehicleDying @0x467cd0]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(1, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = 21;
        e.vel_x = 5000; e.vel_y = 0;             // fast
        e.pos[0] = 100000;                        // far from the (0-init) death pose
        e.net_saved_live_pose[0] = 0;
        e.brain.f[AiBrain::kWorkPitch] = 7;
        e.brain.f[AiBrain::kWorkRoll] = 7;
        e.brain.f[AiBrain::kOutSpeed] = 7;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(21).tick(ctx);
        CHECK(sys.events.count() == 0);           // still crashing: no event yet
        CHECK(e.brain.f[AiBrain::kWorkPitch] == 0);
        CHECK(e.brain.f[AiBrain::kWorkRoll] == 0);
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 0);
        e.vel_x = 100;                            // stopped
        sys.row(21).tick(ctx);
        bool queued4 = false;
        for (int i = 0; i < sys.events.count(); ++i)
            if (sys.events.at(i).type() == 4) queued4 = true;
        CHECK(queued4);
    }
}

// A vehicle's own death states replicate its kill: right after the death
// transforms, an in-session authority sends S2C 0x26 with section 0 (queued as
// the host's item-state event); the destroyed enter finds the hull husked and
// sends nothing more. Neither enter sends on a joiner or outside a session.
// [orig: AI_TransitionToDeath_GroundVehicle @0x467B43..0x467B58;
//  AI_TransitionToDestroyed_Vehicle @0x467E40..0x467E55;
//  Server_SendEntityStatePacket @0x509D70]
void test_vehicle_death_states_send_kill_record() {
    const auto kill_records = [](const World &w, EntityHandle h) {
        int count = 0;
        for (const auto &event : w.out.entity_events)
            if (const auto *state = std::get_if<ItemStateEvent>(&event))
                if (state->handle == h.packed && state->section == 0) ++count;
        return count;
    };
    for (int role = 0; role < 3; ++role) {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(1, 4);
        w.rules.logic_authority = role != 1; // role 1: a joiner
        w.rules.mp_session = role != 2;      // role 2: single player
        Entity seed;
        seed.team = 1;
        seed.health = 0;
        seed.has_item_def = true;
        const EntityHandle h = w.registry.spawn(1, seed);
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = w.rules.logic_authority;
        AiEntity &e = *sys.at(sys.attach(h));
        e.team = 1;
        e.vel_x = 100;
        e.brain.f[AiBrain::kCurState] = 21;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(21).enter(ctx);
        CHECK(kill_records(w, h) == (role == 0 ? 1 : 0));
        CHECK((w.registry.get(h)->flags & 6u) == 6u); // the dispatch's Flags |= 6
        sys.row(23).enter(ctx);
        CHECK(kill_records(w, h) == (role == 0 ? 1 : 0));
    }
}

// The sim resolves the launch point at each fire event, with a raw-origin fallback.
// A stand-in for the asset-aware muzzle-pose provider (EntityPoseProvider
// in production): fixed points per handle, so the consumers' plumbing is pinned
// without a rig. [orig: Entity_GetAttachmentWorldPosition @0x4b2670;
//  Entity_ComputeUserpointWorldTransform @0x545c60]
struct FakeMuzzleProvider : IPoseProvider {
    std::unordered_map<uint16_t, std::array<int32_t, 3>> points;
    std::unordered_map<uint16_t, std::array<int32_t, 6>> userpoints;
    std::unordered_map<uint16_t, std::array<int32_t, 3>> rigid_points;
    int rigid_index_seen = 0;
    bool resolve_userpoint_rigid(World &, EntityHandle h, int index, int32_t out[3]) override {
        const auto it = rigid_points.find(h.packed);
        if (it == rigid_points.end()) return false;
        rigid_index_seen = index;
        out[0] = it->second[0]; out[1] = it->second[1]; out[2] = it->second[2];
        return true;
    }
    bool resolve_muzzle_pose(World &, EntityHandle h, int32_t out[3]) override {
        const auto it = points.find(h.packed);
        if (it == points.end()) return false;
        out[0] = it->second[0]; out[1] = it->second[1]; out[2] = it->second[2];
        return true;
    }
    bool resolve_organic_attachment(World &w, EntityHandle h, uint8_t point, int32_t out[3]) override {
        return point != 0 && resolve_muzzle_pose(w, h, out);
    }
    bool resolve_userpoint_transform(World &, EntityHandle h, int, int32_t out[6]) override {
        const auto it = userpoints.find(h.packed);
        if (it == userpoints.end()) return false;
        for (int i = 0; i < 6; ++i) out[i] = it->second[static_cast<size_t>(i)];
        return true;
    }
};

static void test_fire_pass_uses_embedder_fed_muzzle() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    Entity seed{};
    seed.net_id = 900;
    seed.alive = true;
    seed.health = 100;
    EntityHandle h = w->registry.spawn(0, seed);

    w->tables.ammo.entries.resize(2);
    w->tables.ammo.entries[1].valid = true;
    w->tables.ammo.entries[1].velocity = 62; // 1 u/tick; pos is asserted at SPAWN (no tick runs)
    w->tables.ammo.entries[1].max_age_ticks = 100;

    AiSystem sys;
    int idx = sys.attach(h);
    AiEntity &e = *sys.at(idx);
    e.pos[0] = 10 << 16;
    e.pos[1] = 20 << 16;
    e.pos[2] = 5 << 16;
    e.profile.organic.ammo.fill(1);
    e.profile.organic.launch = {1, 2, 3};

    auto near_f = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };

    // No provider -> the raw entity origin (retail's no-model/no-point copy).
    e.inf.last_events = 0x4; // the primary anim-fire event bit
    sys.infantry_fire_pass(e, *w, /*logic_tick=*/1);
    CHECK(w->round_sim.active_count == 1);
    CHECK(near_f(w->round_sim.rounds[0].pos.x, 10.0f));
    CHECK(near_f(w->round_sim.rounds[0].pos.z, 5.0f));

    // The provider resolves the posed launch point -> rounds spawn from it.
    FakeMuzzleProvider provider;
    provider.points[h.packed] = {(10 << 16) + 0x8000, (20 << 16) - 0x4000, (5 << 16) + 0x4000};
    w->pose_provider = &provider;
    e.inf.last_events = 0x4;
    sys.infantry_fire_pass(e, *w, 3);
    CHECK(w->round_sim.active_count == 2);
    CHECK(near_f(w->round_sim.rounds[1].pos.x, 10.5f));
    CHECK(near_f(w->round_sim.rounds[1].pos.y, 19.75f));
    CHECK(near_f(w->round_sim.rounds[1].pos.z, 5.25f));

    // A provider that declines this handle -> the raw origin again (no stamp
    // survives between ticks; retail computes the point at every fire).
    provider.points.clear();
    e.inf.last_events = 0x4;
    sys.infantry_fire_pass(e, *w, 9);
    CHECK(w->round_sim.active_count == 3);
    CHECK(near_f(w->round_sim.rounds[2].pos.z, 5.0f));
    w->pose_provider = nullptr;
}

// D-AI-6: the shared fire-origin helper — the provider's posed launch point
// when it resolves, else the RAW entity origin; both static forms are the raw
// copy. [orig: Entity_GetAttachmentWorldPosition @0x4b2670 — the raw copy
//  @0x4b2767..0x4b278e when index 0 / no graphicModel / no table]
static void test_weapon_fire_origin_fallback_chain() {
    World w;
    w.registry.configure_pool(0, 4);
    AiSystem sys;
    EntityHandle h = EntityHandle::make(0, 1);
    int idx = sys.attach(h);
    AiEntity &e = *sys.at(idx);
    e.pos[0] = 10 << 16;
    e.pos[1] = 20 << 16;
    e.pos[2] = 5 << 16;

    int32_t out[3];
    AiSystem::weapon_fire_origin(e, out);
    CHECK(out[0] == (10 << 16) && out[1] == (20 << 16));
    CHECK(out[2] == (5 << 16)); // no provider -> the raw origin, no lift
    sys.weapon_fire_origin(w, e, out);
    CHECK(out[2] == (5 << 16));

    FakeMuzzleProvider provider;
    provider.points[h.packed] = {(10 << 16) + 7, (20 << 16) - 9, (5 << 16) + 0x8000};
    w.pose_provider = &provider;
    sys.weapon_fire_origin(w, e, out);
    CHECK(out[0] == (10 << 16) + 7 && out[1] == (20 << 16) - 9 && out[2] == (5 << 16) + 0x8000);
    AiSystem::weapon_fire_origin(e, out); // the static form never consults a provider
    CHECK(out[2] == (5 << 16));

    Entity ent{};
    ent.handle = EntityHandle::make(0, 2);
    ent.position = Vec3{10.0f, 20.0f, 5.0f};
    AiSystem::weapon_fire_origin(ent, out);
    CHECK(out[0] == (10 << 16) && out[2] == (5 << 16));
    sys.weapon_fire_origin(w, ent, out); // declined handle -> raw
    CHECK(out[2] == (5 << 16));
    provider.points[ent.handle.packed] = {111, 222, 333};
    sys.weapon_fire_origin(w, ent, out);
    CHECK(out[0] == 111 && out[1] == 222 && out[2] == 333);
    w.pose_provider = nullptr;
}

// D-AI-6a: the LOS endpoints ride the muzzle seam. A 1.2 u ridge band between
// the pair blocks the stampless chest-lift (0.9 u) ray; posing both muzzles at
// 1.5 u clears it; letting the stamps go stale re-blocks. The heightfield is
// uniform across rows so the engine->field row mapping can't skew the band.
static void test_los_endpoints_use_muzzle_stamp() {
    struct BandField {
        enum { kDim = 512 };
        std::vector<uint16_t> heightmap;
        std::vector<int> sector_grid;
        opennova::terrain::TerrainHeightField field;
        BandField() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
            for (int z = 0; z < kDim; ++z)
                for (int x = 8; x <= 12; ++x)
                    heightmap[z * kDim + x] = static_cast<uint16_t>(1.2 * 256.0); // 1.2 u
            field.heightmap = heightmap.data();
            field.dim = kDim;
            field.layout.sector_grid = sector_grid.data();
            field.layout.origin_x = 0;
            field.layout.origin_y = 0;
        }
    };
    static BandField band;

    World w;
    w.registry.configure_pool(0, 4);
    AiSystem sys;
    sys.terrain = &band.field;
    EntityHandle ha = EntityHandle::make(0, 0);
    EntityHandle hb = EntityHandle::make(0, 1);
    int ia = sys.attach(ha);
    AiEntity &a = *sys.at(ia);
    a.pos[0] = 2 << 16;
    a.pos[1] = 100 << 16;
    a.pos[2] = 0;

    Entity target{};
    target.position = Vec3{20.0f, 100.0f, 0.0f};

    target.handle = hb;
    auto clear_between = [&]() {
        int32_t sa[3];
        sys.weapon_fire_origin(w, a, sa);
        int32_t sb[3];
        sys.weapon_fire_origin(w, target, sb);
        return sys.line_of_sight_clear(w, sa, sb, ha, hb);
    };

    w.logic_tick = 10;
    CHECK(!clear_between()); // raw-origin rays (ground level) hit the 1.2 u band

    FakeMuzzleProvider provider;
    provider.points[ha.packed] = {2 << 16, 100 << 16, static_cast<int32_t>(1.5 * 65536.0)};
    provider.points[hb.packed] = {20 << 16, 100 << 16, static_cast<int32_t>(1.5 * 65536.0)};
    w.pose_provider = &provider;
    CHECK(clear_between()); // posed muzzles ride above the band

    w.pose_provider = nullptr; // no provider -> the raw origins again
    CHECK(!clear_between());
}

// The aim/LOS origin [orig: Entity_ComputeWeaponFireOrigin @0x43b4b0]: a
// modeled non-person takes its def TARGET userpoint through the placement
// matrix when the model carries one (def+1350), else its collision-bbox
// center entity+0x1FC through the same matrix; without a model the raw
// position; a person uses its time-phased eye offset.
static void test_aim_origin_takes_the_non_person_leg() {
    World w;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    CollisionWorld collision;
    w.collision = &collision;
    AiSystem sys;

    Entity veh_seed{};
    veh_seed.kind = EntityKind::Item;
    veh_seed.has_item_def = true;
    veh_seed.item_type = 1; // not def type 3: the non-person leg
    veh_seed.alive = true;
    veh_seed.position = Vec3{10.0f, 20.0f, 0.0f};
    veh_seed.yaw = 90;
    veh_seed.bbox_center = Vec3{0.0f, 0.0f, 1.5f};
    const EntityHandle veh = w.registry.spawn(1, veh_seed);

    int32_t out[3];
    // No attached model: the raw position [orig: @0x43b54f].
    sys.weapon_aim_origin(w, *w.registry.get(veh), out);
    CHECK(out[0] == (10 << 16) && out[1] == (20 << 16) && out[2] == 0);

    // A model, no TARGET userpoint: the bbox center through the placement
    // matrix — a vertical offset survives the yaw untouched [orig: @0x43b619].
    collision.assign_entity(veh, collision.add_model(CollisionModel{}));
    sys.weapon_aim_origin(w, *w.registry.get(veh), out);
    CHECK(out[0] == (10 << 16) && out[1] == (20 << 16));
    CHECK(out[2] == static_cast<int32_t>(1.5 * 65536.0));

    // The def TARGET userpoint through the provider's rigid transform
    // [orig: @0x43b5d4..0x43b5f6].
    FakeMuzzleProvider provider;
    provider.rigid_points[veh.packed] = {11 << 16, 22 << 16, 33 << 16};
    w.pose_provider = &provider;
    w.registry.get(veh)->target_userpoint_byte = 2;
    sys.weapon_aim_origin(w, *w.registry.get(veh), out);
    CHECK(out[0] == (11 << 16) && out[1] == (22 << 16) && out[2] == (33 << 16));
    CHECK(provider.rigid_index_seen == 2);

    // Person target points use the actual tick VALUE plus 36 * SSN, never
    // the weapon point or the address of the tick global. [orig: @0x43B4C5]
    Entity person_seed{};
    person_seed.kind = EntityKind::Organic;
    person_seed.has_item_def = true;
    person_seed.item_type = 3;
    person_seed.net_id = 0;
    person_seed.alive = true;
    person_seed.position = Vec3{1.0f, 2.0f, 0.0f};
    person_seed.eye_offset_x = 65536;
    person_seed.eye_offset_y = -65536;
    person_seed.eye_offset_z = 131072;
    const EntityHandle person = w.registry.spawn(0, person_seed);
    Entity &target = *w.registry.get(person);
    provider.points[person.packed] = {3 << 16, 4 << 16, 99 << 16};
    w.logic_tick = 0;
    sys.weapon_aim_origin(w, target, out);
    CHECK(out[0] == 94208 && out[1] == 96256 && out[2] == 131072);
    w.logic_tick = 127;
    sys.weapon_aim_origin(w, target, out);
    CHECK(out[0] == 100352 && out[1] == 102400 && out[2] == 131072);
    w.logic_tick = 128;
    sys.weapon_aim_origin(w, target, out);
    CHECK(out[0] == 77824 && out[1] == 112640 && out[2] == 65536);
    target.net_id = 1;
    w.logic_tick = 0;
    sys.weapon_aim_origin(w, target, out);
    CHECK(out[0] == 96256 && out[1] == 98304 && out[2] == 131072);
    w.logic_tick = 0xFFFFFFFFu; // wrapped phase = 35, same jitter band
    sys.weapon_aim_origin(w, target, out);
    CHECK(out[0] == 96256 && out[1] == 98304 && out[2] == 131072);

    // During the motor, the live fixed-point body/eye leads its registry mirror.
    const int body_index = sys.attach(person);
    AiEntity &body = *sys.at(body_index);
    body.pos[0] = 17; body.pos[1] = 29; body.pos[2] = 41;
    body.inf.eye_offset_x = -3; body.inf.eye_offset_y = -5; body.inf.eye_offset_z = 7;
    target.net_id = 0;
    w.logic_tick = 128;
    sys.weapon_aim_origin(w, target, out);
    CHECK(out[0] == -4080 && out[1] == -2021 && out[2] == 44);
    target.has_item_def = false; // kind alone never selects the person leg
    sys.weapon_aim_origin(w, target, out);
    CHECK(out[0] == 65536 && out[1] == 131072 && out[2] == 0);
    w.pose_provider = nullptr;
}

// The aim source is the rocket launch point, and a person target is aimed at
// its led raw Position: the ComputeWeaponFireOrigin eye point both aim blocks
// compute is never read, and the target's weapon muzzle plays no part either.
// [orig: Entity_GetAttachmentWorldPosition @0x4B2670 (+0x366);
// Entity_UpdateInfantryAI @0x4B9910 (the dead Entity_ComputeWeaponFireOrigin
// calls @0x4BC720 / @0x4BCB23; the aim delta @0x4BC825..0x4BC84E)]
static void test_aim_solution_uses_muzzle_stamp() {
    struct AttackSource : IRootMotionSource {
        bool has_clip(int, int id) const override {
            return id == anim_state::kIdle || id == anim_state::kAttack;
        }
        int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
        bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
            if (!has_clip(0, id)) return false;
            ++phase;
            out = RootMotionFrame{}; // zero displacement: everyone stays put
            return true;
        }
    };
    static AttackSource src;

    World w;
    w.registry.configure_pool(0, 8);
    Entity player_seed;
    player_seed.kind = EntityKind::Organic;
    player_seed.item_id = 1001;
    player_seed.has_item_def = true;
    player_seed.item_type = 3;
    player_seed.team = 2;
    player_seed.health = 100;
    player_seed.net_id = 0x21;
    player_seed.group_id = 2;
    player_seed.position = Vec3{20.0f, 0.0f, 0.0f};
    EntityHandle player_h = w.registry.spawn(0, player_seed);

    Entity npc_seed;
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.group_id = 1;
    npc_seed.position = Vec3{0.0f, 0.0f, 0.0f};
    EntityHandle npc_h = w.registry.spawn(0, npc_seed);

    AiSystem &sys = w.ai;
    sys.is_authority = true;
    sys.root_motion = &src;
    int idx = sys.attach(npc_h);
    AiEntity &npc = *sys.at(idx);
    npc.inf.active = true;
    npc.team = 1;
    npc.net_id = 0x11;
    npc.pos[0] = 0; npc.pos[1] = 0; npc.pos[2] = 0;
    npc.profile.organic.launch[1] = 2;
    npc.slot.f[10] = 0; // zero aim error: the pitch pin is exact
    npc.slot.f[11] = 0;
    npc.slot.f[15] = 60 << 16;
    npc.slot.f[16] = 10 << 16;
    npc.slot.f[17] = 100 << 16;
    npc.slot.f[22] = 62;

    TickContext tctx;
    tctx.world = &w;
    tctx.is_authority = true;
    uint32_t t = 0;
    for (; t < 500 && !npc.inf.aim_valid; ++t) {
        tctx.logic_tick = t;
        w.logic_tick = t; // production's run_logic_tick keeps these in step
        w.update_all_entities(tctx);
    }
    CHECK(npc.inf.aim_valid);
    CHECK(npc.inf.aim_pitch == 0); // raw origin to raw origin: a level shot

    // The provider lifts the NPC's eye 0.5 u: eye 0.5, target origin 0 ->
    // the solution pitches DOWN. The combat FSM oscillates through non-aim
    // anims, so run ticks until the aim re-asserts (a few dozen suffice).
    FakeMuzzleProvider provider;
    provider.points[npc_h.packed] = {0, 0, static_cast<int32_t>(0.5 * 65536.0)};
    w.pose_provider = &provider;
    npc.inf.aim_valid = false;
    for (uint32_t end = t + 500; t < end; ++t) {
        tctx.logic_tick = t;
        w.logic_tick = t;
        w.update_all_entities(tctx);
        if (npc.inf.aim_valid) break;
    }
    CHECK(npc.inf.aim_valid);
    CHECK(npc.inf.aim_pitch < 0);

    // A 1 u target eye changes nothing: the target point stays its raw
    // origin, so the solution still pitches down. An unrelated weapon point at
    // 99 u is ignored as well.
    w.registry.get(player_h)->eye_offset_z = 65536;
    provider.points[player_h.packed] = {20 << 16, 0, 99 << 16};
    npc.inf.aim_valid = false;
    for (uint32_t end = t + 500; t < end; ++t) {
        tctx.logic_tick = t;
        w.logic_tick = t;
        w.update_all_entities(tctx);
        if (npc.inf.aim_valid) break;
    }
    CHECK(npc.inf.aim_valid);
    CHECK(npc.inf.aim_pitch < 0);
    w.pose_provider = nullptr;
}

// The D-AI-2 turret solver (solve_weapon_fire_transform — Entity_
// ComputeWeaponFireTransform_0 @0x456980) through the state-17 tick's three
// fire legs: mobile solve+fire, WEAPON_SLOW slew hold, and the RC_FIRE
// stationary PITCHLOCKED_MINUS45 leg behind the commanded guard byte.
static void test_sm_turret_fire() {
    auto setup = [](std::unique_ptr<World> &w, AiSystem &sys, AiEntity *&e,
                    EntityHandle &tgt_h) {
        w = std::make_unique<World>();
        w->registry.configure_pool(0, 8);
        w->registry.configure_pool(1, 8);
        w->tables.ammo.entries.resize(2);
        w->tables.ammo.entries[1].valid = true;
        w->tables.ammo.entries[1].velocity = 620;
        w->tables.ammo.entries[1].max_age_ticks = 100;
        Entity shooter{};
        shooter.alive = true;
        shooter.health = 100;
        EntityHandle sh = w->registry.spawn(1, shooter);
        Entity target{};
        target.alive = true;
        target.health = 100;
        target.position = {100.0f, 0.0f, 0.0f};
        tgt_h = w->registry.spawn(0, target);
        sys.is_authority = true;
        int idx = sys.attach(sh);
        e = sys.at(idx);
        e->pos[0] = 0;
        e->pos[1] = 0;
        e->pos[2] = 0;
        e->heading = 0; // facing +X = the bearing to the target
        e->brain.f[AiBrain::kCurState] = kAiGroundCombat;
        e->brain.f[AiBrain::kStep] = 16;
        e->brain.f[AiBrain::kTickAccum] = 16; // one processed tick immediately
        e->brain.f[AiBrain::kTargetSlot] = tgt_h.packed + 1;
        e->brain.f[AiBrain::kAmmoA] = 5;
        e->brain.f[AiBrain::kAccuracy] = 5; // modulus 6-5=1 -> zero scatter
        e->profile.flags96 = 2;             // combat-capable shape
        e->profile.fov_secondary = 0xFF;    // wide-open hull fire gate
        e->profile.fire_interval_a = 1;
        e->profile.fire_a.ammo_index = 1;
        e->profile.fire_a.cone_bam = 0x7FFFFFFF; // limit 63/256 turns
		e->profile.approach_cap = 1000 << 16;
		e->profile.radar_fov_bam = 0x7FFFFFFF;
    };

    // Mobile leg: solve + fire. Origin = pos + 2.0u Z (the empty-bone-list
    // leg); acc 5 -> deterministic direction at the exact bearing (0).
    {
        std::unique_ptr<World> w;
        AiSystem sys;
        AiEntity *e = nullptr;
        EntityHandle tgt_h;
        setup(w, sys, e, tgt_h);
        AiThinkCtx ctx{&sys, e, w.get(), nullptr};
        sys.row(kAiGroundCombat).tick(ctx);
        CHECK(w->out.rounds.count == 1);
        if (w->out.rounds.count == 1) {
            CHECK(w->out.rounds.records[0].origin_z == 0x20000);
            CHECK(w->out.rounds.records[0].dir_yaw == 0); // bearing 0, no scatter
        }
        CHECK(e->brain.f[AiBrain::kLastWeapon] == 1);
        CHECK(e->brain.f[AiBrain::kAmmoA] == 4);
        // The primary cooldown word cleared on fire [orig: §17.6].
        CHECK((static_cast<uint32_t>(e->brain.f[AiBrain::kCooldownPair]) & 0xFFFFu) == 0);
        CHECK((e->brain.bytes()[AiBrain::kBoneFlagByte] & 0x40) != 0);
    }

    // WEAPON_SLOW turret: a 45-degree offset target holds fire and slews the
    // active yaw by the witnessed step; pre-aligning the active yaw fires.
    {
        std::unique_ptr<World> w;
        AiSystem sys;
        AiEntity *e = nullptr;
        EntityHandle tgt_h;
        setup(w, sys, e, tgt_h);
        e->heading = -0x20000000; // target sits 45 deg off the frame
        e->profile.fire_a.flags = 0x1 | 0x2; // WEAPON_TURRET | WEAPON_SLOW
        AiThinkCtx ctx{&sys, e, w.get(), nullptr};
        sys.row(kAiGroundCombat).tick(ctx);
        CHECK(w->out.rounds.count == 0); // slewing, fire held
        // active yaw advanced one slew step toward the staged mirror
        // [orig: @0x456EC3 +-0x2108421].
        CHECK(e->brain.f[AiBrain::kActiveYaw] != 0);
        // Snap the active yaw onto the staged solution -> aligned -> fires.
        e->brain.f[AiBrain::kActiveYaw] = e->brain.f[AiBrain::kStagingBlock + 3];
        e->brain.f[AiBrain::kTickAccum] = 16;
        e->brain.f[AiBrain::kCooldownPair] = 0;
        sys.row(kAiGroundCombat).tick(ctx);
        CHECK(w->out.rounds.count == 1);
    }

    // RC_FIRE stationary + WEAPON_PITCHLOCKED_MINUS45: no target needed, but
    // only behind the commanded guard byte (AI command case 0x15).
    {
        std::unique_ptr<World> w;
        AiSystem sys;
        AiEntity *e = nullptr;
        EntityHandle tgt_h;
        setup(w, sys, e, tgt_h);
        e->brain.f[AiBrain::kTargetSlot] = 0; // the stationary leg clears targets anyway
        e->profile.flags100 = 0x80;           // RC_FIRE
        e->profile.fire_a.flags = 0x10;       // WEAPON_PITCHLOCKED_MINUS45
        AiThinkCtx ctx{&sys, e, w.get(), nullptr};
        sys.row(kAiGroundCombat).tick(ctx);
        CHECK(w->out.rounds.count == 0); // guard byte clear -> weapons hold
        AiEventEntry cmd{};
        cmd.f[0] = 0x15;
        cmd.f[3] = 1;
        CHECK(sys.ai_handle_command(*w, *e, cmd));
        CHECK(e->brain.bytes()[AiBrain::kGuardFireByte] == 1);
        e->brain.f[AiBrain::kTickAccum] = 16;
        e->brain.f[AiBrain::kCooldownPair] = 0x10001;
        sys.row(kAiGroundCombat).tick(ctx);
        CHECK(w->out.rounds.count == 1);
        if (w->out.rounds.count == 1) {
            // pitch = -0x1FFFFFE0 [orig: @0x457061 add 0xE0000020].
            CHECK(w->out.rounds.records[0].dir_pitch == static_cast<int32_t>(0xE0000020u));
        }
    }
}

// Every ready block of the vehicle machine writes its ammo byte into the hull's
// AdmDef byte ahead of its solve, fired or not: the primary's on a shot, the
// secondary's when only it is ready, and a slewing turret's while it holds
// fire. [orig: AIEntity_ProcessWeaponFire processed legs @0x473C9D..0x473CA8 /
// @0x473D87..0x473D92; the weapon call's read @0x47301B]
static void test_sm_fire_stamps_the_hull_adm_byte() {
    struct Rig {
        std::unique_ptr<World> w = std::make_unique<World>();
        AiSystem sys;
        AiEntity *e = nullptr;
        EntityHandle hull;
        Rig() {
            w->registry.configure_pool(0, 8);
            w->registry.configure_pool(1, 8);
            w->tables.ammo.entries.resize(3);
            for (int i = 1; i <= 2; ++i) {
                w->tables.ammo.entries[static_cast<size_t>(i)].valid = true;
                w->tables.ammo.entries[static_cast<size_t>(i)].velocity = 620;
                w->tables.ammo.entries[static_cast<size_t>(i)].max_age_ticks = 100;
            }
            Entity shooter{};
            shooter.alive = true;
            shooter.health = 100;
            hull = w->registry.spawn(1, shooter);
            Entity target{};
            target.alive = true;
            target.health = 100;
            target.position = {100.0f, 0.0f, 0.0f};
            const EntityHandle tgt_h = w->registry.spawn(0, target);
            sys.is_authority = true;
            e = sys.at(sys.attach(hull));
            e->brain.f[AiBrain::kCurState] = kAiGroundCombat;
            e->brain.f[AiBrain::kStep] = 16;
            e->brain.f[AiBrain::kTickAccum] = 16;
            e->brain.f[AiBrain::kTargetSlot] = tgt_h.packed + 1;
            e->brain.f[AiBrain::kAmmoA] = 5;
            e->brain.f[AiBrain::kAmmoB] = 5;
            e->brain.f[AiBrain::kAccuracy] = 5;
            e->profile.flags96 = 2;
            e->profile.fov_secondary = 0xFF;
            e->profile.fire_interval_a = 1;
            e->profile.fire_interval_b = 1;
            e->profile.fire_a.ammo_index = 1;
            e->profile.fire_a.cone_bam = 0x7FFFFFFF;
            e->profile.fire_b.ammo_index = 2;
            e->profile.fire_b.cone_bam = 0x7FFFFFFF;
            e->profile.approach_cap = 1000 << 16;
            e->profile.radar_fov_bam = 0x7FFFFFFF;
        }
        void tick() {
            AiThinkCtx ctx{&sys, e, w.get(), nullptr};
            sys.row(kAiGroundCombat).tick(ctx);
        }
        uint8_t byte() const { return w->registry.get(hull)->equipped_adm_index; }
    };
    {
        Rig r;
        CHECK(r.byte() == kAdmSlotNone);
        r.tick();
        CHECK(r.w->out.rounds.count == 1);
        CHECK(r.byte() == 1);
    }
    {
        Rig r; // only the secondary is ready
        r.e->brain.f[AiBrain::kAmmoA] = 0;
        r.tick();
        CHECK(r.w->out.rounds.count == 1);
        CHECK(r.byte() == 2);
    }
    {
        Rig r; // a slewing turret: ready, stamped, holding fire
        r.e->heading = -0x20000000;
        r.e->profile.fire_a.flags = 0x1 | 0x2;
        r.e->brain.f[AiBrain::kAmmoB] = 0;
        r.tick();
        CHECK(r.w->out.rounds.count == 0);
        CHECK(r.byte() == 1);
    }
}

// Compact attack-animation source for the behavioral regressions below. Retail
// infantry fires from .bad event bit 0x4, so these drive the actual attack path.
struct AttackEventSource final : IRootMotionSource {
    bool has_clip(int, int id) const override {
        return id == anim_state::kIdle || id == anim_state::kAttack ||
               (id >= 67 && id <= 75) || (id >= 173 && id <= 239);
    }
    int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
    bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
        if (!has_clip(0, id)) return false;
        ++phase;
        out = RootMotionFrame{};
        if (id == anim_state::kAttack) out.events = 0x4;
        return true;
    }
};

static void seed_test_rifle_ammo(World &w) {
    w.tables.ammo.entries.resize(2);
    w.tables.ammo.entries[1].velocity = 800;
    w.tables.ammo.entries[1].max_age_ticks = 124;
    w.tables.ammo.entries[1].weight_in_grains = 875;
    w.tables.ammo.entries[1].min_damage = 10;
    w.tables.ammo.entries[1].max_damage = 40;
    w.tables.ammo.entries[1].valid = true;
}

// Exact-symptom feedback loop for a lethal hit at the live -> death-animation edge.
// The root values are the first half-frame samples measured from E_STAND.adm's
// I_idle1.bad and Dt2DeTFC.bad tracks. Retail retains the old clip for the death-edge
// tick, then advances both channels and blends their five numeric root lanes at 0.1.
struct DeathTransitionRootSource final : IRootMotionSource {
    static constexpr int32_t kIdleBottom = 66245;
    static constexpr int32_t kDeathBottom = 60145;
    static constexpr int32_t kDeathDx = -412;
    static constexpr int32_t kDeathDz = -555;
    static constexpr int32_t kFirstBlendDx = -41;
    static constexpr int32_t kFirstBlendBottom = 65635;
    static constexpr int32_t kFirstBlendDz = kFirstBlendBottom - kIdleBottom;

    bool has_clip(int, int id) const override {
        return id == anim_state::kIdle || (id >= 173 && id <= 239);
    }
    int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
    bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
        if (!has_clip(0, id)) return false;
        ++phase;
        out = RootMotionFrame{};
        if (id == anim_state::kIdle) {
            out.capsule_bottom = kIdleBottom;
        } else {
            out.dx = kDeathDx;
            out.dz = kDeathDz;
            out.capsule_bottom = kDeathBottom;
        }
        return true;
    }
};

// The entity update stamps the retail is_in_session fact from the session
// rules: a joiner's brain then takes the in-session death arm (the death tick,
// then the type-20 event), and single player, the in-process listen server,
// stays outside the session.
// [orig: g_NapiNPCtx.is_in_session -- SinglePlayer_StartMission @0x561AF0
//  leaves it clear (read back @0x561E73); EntityAI_ProcessAirStateMachine
//  @0x458273 (the death-event arm)]
static void test_entity_update_stamps_the_session_fact() {
    auto heap = std::make_unique<World>();
    World &w = *heap;
    TickContext ctx{};
    ctx.world = &w;
    ctx.is_authority = false;
    w.rules.mp_session = true;
    w.update_all_entities(ctx);
    CHECK(w.ai.is_in_session);
    AiEntity &e = *w.ai.at(w.ai.attach(EntityHandle::make(0, 0)));
    e.health = 100;
    e.vel_x = 2000;
    e.vel_y = 0;
    e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
    w.ai.process_air_state_machine(e, w, 4);
    CHECK(e.health == 0);
    bool saw_20 = false;
    for (int i = 0; i < w.ai.events.count(); ++i)
        if (w.ai.events.at(i).type() == 20) saw_20 = true;
    CHECK(saw_20);

    w.rules.mp_session = false;
    ctx.is_authority = true;
    w.update_all_entities(ctx);
    CHECK(!w.ai.is_in_session);
}

// A round that hits an NPC is drained into its reaction words inside the same
// entity update, before that update's pool-0 walk: the projectiles and the
// explosion queue run ahead of the organic bodies, whose damage callbacks
// stamp the victim inline.
// [orig: Entity_UpdateAllEntities -- the Weapon_UpdateAllProjectiles call
//  @0x4C223A and the Projectile_ProcessExplosionQueue call @0x4c223f precede
//  the pool-0 walk @0x4C2426; Entity_OnDamageReceived @0x4af859..0x4af878]
static void test_round_hit_reaches_the_same_pass_body_update() {
    auto heap = std::make_unique<World>();
    World &w = *heap;
    w.registry.configure_pool(0, 8);
    seed_test_rifle_ammo(w);

    Entity shooter_seed;
    shooter_seed.team = 1;
    const EntityHandle shooter_h = w.registry.spawn(0, shooter_seed);
    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.has_item_def = true;
    victim_seed.item_type = 3;
    victim_seed.team = 2;
    victim_seed.health = 1000;
    victim_seed.health_max = 1000;
    victim_seed.net_id = 0x21;
    victim_seed.group_id = 2;
    victim_seed.position = {20.0f, 0.0f, 0.0f};
    const EntityHandle victim_h = w.registry.spawn(0, victim_seed);
    AiSystem &ai = w.ai;
    AiEntity &victim = *ai.at(ai.attach(victim_h));
    victim.inf.active = true;
    victim.health = 1000;
    victim.team = 2;
    victim.net_id = 0x21;
    victim.pos[0] = 20 << 16;

    RoundSpawnParams shot;
    shot.owner = shooter_h;
    shot.shooter_handle = shooter_h.packed;
    shot.origin = {0.0f, 0.0f, 0.9f};
    shot.ammo_index = 1;
    CHECK(w.round_sim.spawn(w, shot) >= 0);

    TickContext ctx{};
    ctx.world = &w;
    ctx.is_authority = true;
    bool hit = false;
    for (uint32_t t = 1; t <= 8 && !hit; ++t) {
        ctx.logic_tick = t;
        w.update_all_entities(ctx);
        hit = w.registry.get(victim_h)->health < 1000;
        if (!hit) continue;
        CHECK(w.round_sim.hits.empty());
        CHECK(victim.inf.damage_timer > 0);
    }
    CHECK(hit);
}

static void test_lethal_hit_blends_into_death_animation_without_position_jump() {
    World w;
    w.registry.configure_pool(0, 8);
    seed_test_rifle_ammo(w);

    Entity shooter_seed;
    shooter_seed.team = 1;
    const EntityHandle shooter_h = w.registry.spawn(0, shooter_seed);

    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.has_item_def = true;
    victim_seed.item_type = 3;
    victim_seed.team = 2;
    victim_seed.health = 10;
    victim_seed.health_max = 10;
    victim_seed.net_id = 0x21;
    victim_seed.group_id = 2;
    victim_seed.position = {20.0f, 0.0f, 0.0f};
    // The corpse timer's seed. JO persons author `deathtime 30` (37 of the 39
    // type-person rows in JOX ITEMS.DEF), parse-scaled to 30*62 + 62 ticks
    // [orig: ItemDef_ParseProperty @0x49fa6c-0x49faa0 -> def+0x890; the death
    // edge copies it to entity+0x148 @0x4b9c97]. A def-less rig row leaves it 0,
    // and retail runs the persistence block on the edge tick itself (loc_4B9D55
    // falls into loc_4B9E4D): a zero timer skips the decrement (@0x4b9e70..0x4b9e72),
    // fails the `> 0` keep (@0x4b9f44), and with no respawn tickets, no session and
    // no local-player watcher reaches Entity_Destroy @0x4b9f93 -> jmp loc_4BFC89
    // @0x4b9f9b, which frees the brain before the death clip is ever advanced.
    victim_seed.deathtime_ticks = 30 * 62 + 62;
    const EntityHandle victim_h = w.registry.spawn(0, victim_seed);

    DeathTransitionRootSource root;
    AiSystem &ai = w.ai;
    ai.root_motion = &root;
    const int victim_index = ai.attach(victim_h);
    AiEntity &victim = *ai.at(victim_index);
    victim.inf.active = true;
    victim.inf.anim_state = anim_state::kIdle;
    victim.health = 10;
    victim.team = 2;
    victim.net_id = 0x21;
    victim.pos[0] = 20 << 16;

    TickContext tick{};
    tick.world = &w;
    tick.is_authority = true; // the SP/listen-server authority path the NPC runs on.
    tick.logic_tick = 1;
    w.update_all_entities(tick);

    RoundSpawnParams shot;
    shot.owner = shooter_h;
    shot.shooter_handle = shooter_h.packed;
    shot.origin = {0.0f, 0.0f, w.registry.get(victim_h)->position.z + 0.9f};
    shot.ammo_index = 1;
    CHECK(w.round_sim.spawn(w, shot) >= 0);
    for (int i = 0; i < 4 && w.registry.get(victim_h)->health > 0; ++i)
        w.round_sim.tick(w, nullptr, nullptr);
    CHECK(w.registry.get(victim_h)->health == 0);
    CHECK(!w.round_sim.deaths.empty());

    // A dismemberment piece may attach a brain and move the AI array: re-read
    // the victim's brain.
    AiEntity &corpse = *ai.for_handle(victim_h);
    const int32_t transition_x = corpse.pos[0];
    const int32_t transition_z = corpse.pos[2];
    tick.logic_tick = 2;
    w.update_all_entities(tick);

    CHECK(corpse.inf.anim_state >= 173 && corpse.inf.anim_state <= 239);
    CHECK(corpse.inf.body_clip_state() == anim_state::kIdle);
    CHECK(corpse.inf.clip_phase == 2);
    CHECK(corpse.inf.anim_blend_weight == 1.0f);
    CHECK(corpse.pos[0] == transition_x);
    CHECK(corpse.pos[2] == transition_z);

    const int32_t blend_x = corpse.pos[0];
    const int32_t blend_z = corpse.pos[2];
    tick.logic_tick = 3;
    w.update_all_entities(tick);

    CHECK(corpse.inf.clip_phase == 1);
    CHECK(corpse.inf.anim_blend_weight == 0.1f);
    CHECK(corpse.pos[0] - blend_x == DeathTransitionRootSource::kFirstBlendDx);
    if (corpse.pos[2] - blend_z != DeathTransitionRootSource::kFirstBlendDz) {
        std::printf("first death blend position jump: dz=%d, expected blended dz=%d\n",
                    corpse.pos[2] - blend_z, DeathTransitionRootSource::kFirstBlendDz);
        ++failures;
    }
}

static void configure_test_emplacement_weapon(WeaponTableEntry &weapon) {
    // Every stock emplaced weapon authors its turret window (WPN_EMP50TRI:
    // 180/45/45): an unauthored def keeps zero bounds, which pin both axes.
    // [orig: AdmDef_InitEntryDefaults @0x53FEFF (zeroed def); the fallback
    //  window Entity_GetWeaponTurretLimits @0x540E2C..0x540E58]
    weapon.turret_yaw_range_deg = 180;
    weapon.turret_pitch_max_deg = 45;
    weapon.turret_pitch_min_deg = 45;
    weapon.clipsize = -1;
    weapon.action_fsm.clip_capacity = -1;
    for (int action = 0; action < weapon_action::kCount; ++action)
        weapon.action_fsm.actions[action].id = action;
    weapon.action_fsm.actions[weapon_action::kRecoil].delay_end = 1;
}

static void configure_rifleman(AiEntity &npc, uint16_t net_id, uint8_t team) {
    npc.inf.active = true;
    npc.team = team;
    npc.net_id = net_id;
    npc.health = 100;
    npc.profile.organic.ammo.fill(1);
    npc.profile.organic.launch = {1, 2, 3};
    npc.profile.clip_size = 30;
    npc.inf.magazine = 30;
    npc.slot.f[10] = 0;
    npc.slot.f[11] = 0;
    npc.slot.f[15] = 60 << 16;
    npc.slot.f[16] = 10 << 16;
    npc.slot.f[17] = 100 << 16;
    npc.slot.f[22] = 62;
}

static void test_world_feed_never_engages_same_team() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    seed_test_rifle_ammo(*w);

    Entity ally_seed{};
    ally_seed.kind = EntityKind::Organic;
    ally_seed.has_item_def = true;
    ally_seed.item_type = 3;
    ally_seed.team = 1;
    ally_seed.health = 100;
    ally_seed.net_id = 0x22;
    ally_seed.group_id = 2;
    ally_seed.position = Vec3{20.0f, 0.0f, 0.0f};
    const EntityHandle ally_h = w->registry.spawn(0, ally_seed);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.group_id = 1;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    for (uint32_t tick = 0; tick < 512; ++tick) {
        ctx.logic_tick = tick;
        w->update_all_entities(ctx);
    }

    CHECK(!npc.inf.combat_target.valid());
    CHECK(w->out.rounds.count == 0);
    CHECK(w->registry.get(ally_h)->health == 100);
}

static void test_script_target_policy_reaches_infantry_and_weapons() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    Entity seed{};
    seed.kind = EntityKind::Organic;
    seed.item_id = 1001;
    seed.item_type = 3;
    seed.net_id = 0x11;
    seed.team = 1;
    const EntityHandle npc_h = w->registry.spawn(0, seed);
    seed.team = 2;
    seed.net_id = 0x21;
    seed.group_id = 5;
    seed.position = {5, 0, 0};
    const EntityHandle near_h = w->registry.spawn(0, seed);
    seed.net_id = 0x22;
    seed.group_id = 6;
    seed.position = {12, 0, 0};
    const EntityHandle far_h = w->registry.spawn(0, seed);
    seed.net_id = 0x23;
    seed.group_id = 7;
    seed.team = 1;
    seed.position = {25, 0, 0};
    const EntityHandle ally_h = w->registry.spawn(0, seed);
    AttackEventSource clips;
    AiSystem &ai = w->ai;
    ai.root_motion = &clips;
    ai.attach(npc_h);
    AiEntity &npc = *ai.for_handle(npc_h);
    configure_rifleman(npc, 0x11, 1);
    npc.profile.organic.ammo.fill(0);
    int phase = 0;
    const auto scan = [&] {
        TickContext ctx{};
        ctx.world = w.get();
        ctx.is_authority = true;
        ctx.logic_tick = 28 + 128 * phase++;
        w->logic_tick = ctx.logic_tick;
        w->update_all_entities(ctx);
        return npc.inf.combat_target;
    };
    using S = AiTargetSelector;
    CHECK(scan() == near_h);
    w->commands.set_ssn_target_selector(0x11, S::ExclusiveSsn, 0x23);
    CHECK(scan() == ally_h); // explicit policy admits the friendly candidate
    w->commands.set_ssn_target_selector(0x11, S::ExclusiveGroup, 6);
    CHECK(!scan().valid()); // both exclusive constraints apply
    w->commands.set_ssn_target_selector(0x11, S::ExclusiveSsn, 0);
    CHECK(scan() == far_h);
    w->commands.set_ssn_target_selector(0x11, S::ExclusiveGroup, 0);
    w->commands.set_ssn_target_selector(0x11, S::PreferredSsn, 0x21);
    CHECK(scan() == far_h); // retail shifts mismatched NEGATIVE scores right

    npc.profile.view_fov_bam = INT32_MAX;
    npc.profile.view_dist = 0x640000;
    npc.profile.radar_fov_bam = INT32_MAX;
    npc.profile.approach_cap = 0x640000;
    const int32_t pose[6] = {};
    int32_t metrics[6];
    w->commands.set_ssn_target_selector(0x11, S::ExclusiveGroup, 7);
    CHECK(!ai.weapon_target_metrics(*w, npc, *w->registry.get(near_h), pose, 0, true, metrics));
    CHECK(ai.weapon_target_metrics(*w, npc, *w->registry.get(ally_h), pose, 0, true, metrics));
}

static void test_guided_round_notification_and_ally_alert_scope() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 8);
    Entity seed{};
    seed.item_id = 1001;
    seed.team = 1;
    seed.group_id = 4;
    const auto own = w->registry.spawn(1, seed);
    seed.position = {60, 0, 90}; // outside the 100-unit sphere
    const auto high = w->registry.spawn(1, seed);
    seed.position = {60, 0, 80}; // exactly on the sphere
    const auto edge = w->registry.spawn(1, seed);
    seed.position = {1, 0, 0};
    const auto pool0 = w->registry.spawn(0, seed);
    auto &ai = w->ai;
    for (auto h : {own, high, edge, pool0}) ai.attach(h);
    auto &self = *ai.for_handle(own);
    self.team = 1;
    self.profile.type = 1;
    self.profile.flags96 = 0x10;
    self.brain.f[AiBrain::kCurState] = 7;
    AiEventEntry event{};
    event.f[0] = 12;
    CHECK(ai.ai_handle_command(*w, self, event));
    CHECK(self.brain.f[AiBrain::kPendState] == 10);
    CHECK(self.brain.f[AiBrain::kFireTimer] == 93);
    CHECK(self.brain.f[AiBrain::kAlert] == 2);
    CHECK(w->script.relations.group(4).alert == TriggerRelations::kAlertRed);
    CHECK(ai.for_handle(edge)->brain.f[AiBrain::kAlert] == 2);
    CHECK(ai.for_handle(high)->brain.f[AiBrain::kAlert] == 0);
    CHECK(ai.for_handle(pool0)->brain.f[AiBrain::kAlert] == 0);
    self.brain.f[AiBrain::kCurState] = 14;
    self.brain.f[AiBrain::kFireTimer] = 0;
    CHECK(ai.ai_handle_command(*w, self, event));
    CHECK(self.brain.f[AiBrain::kFireTimer] == 0); // gated, still handled
    event.f[0] = 13;
    CHECK(!ai.ai_handle_command(*w, self, event));
}

static void test_berserk_candidate_is_intentional_team_exception() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);

    Entity candidate_seed{};
    candidate_seed.kind = EntityKind::Organic;
    candidate_seed.item_id = 1001;
    candidate_seed.item_type = 3;
    candidate_seed.team = 1;
    candidate_seed.health = 100;
    candidate_seed.net_id = 0x22;
    candidate_seed.position = Vec3{20.0f, 0.0f, 0.0f};
    const EntityHandle candidate_h = w->registry.spawn(0, candidate_seed);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    const int npc_index = ai.attach(npc_h);
    const int candidate_index = ai.attach(candidate_h);
    AiEntity &npc = *ai.at(npc_index);
    configure_rifleman(npc, 0x11, 1);
    npc.profile.organic.ammo.fill(0);
    AiEntity &candidate = *ai.at(candidate_index);
    configure_rifleman(candidate, 0x22, 1);
    candidate.profile.organic.ammo.fill(0);
    candidate.slot.f[1] |= 0x200;

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 28; // 28 + 36*0x11 is the scanner's 32-tick phase
    w->update_all_entities(ctx);

    // Shared infantry targeting accepts a same-team candidate carrying Berserk.
    // [orig: Entity_FindTargets @0x53a7ea..0x53a824]
    CHECK(npc.inf.combat_target == candidate_h);

    // The exception is symmetric: a Berserk scanner may also select an ordinary
    // same-team candidate.
    candidate.slot.f[1] &= ~0x200;
    npc.slot.f[1] |= 0x200;
    ai.ai_set_target(*w, npc, EntityHandle{});
    npc.inf.damage_timer = 0;
    npc.inf.was_hit = false;
    ctx.logic_tick = 156; // key 768: 32-tick scan, full-range phase
    w->update_all_entities(ctx);
    CHECK(npc.inf.combat_target == candidate_h);
}

static void test_damage_hit_sets_retail_alert_state() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);

    Entity shooter_seed{};
    shooter_seed.kind = EntityKind::Organic;
    shooter_seed.team = 2;
    shooter_seed.health = 100;
    shooter_seed.net_id = 0x21;
    shooter_seed.group_id = 2;
    const EntityHandle shooter_h = w->registry.spawn(0, shooter_seed);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.group_id = 1;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);

    // The sim/AI seam carries the already-processed hit. Retail's damage callback
    // writes these stamps inline before the next infantry update.
    // [orig: OrganicClass_HandleEvent @0x4073db..0x4073ea;
    //  Entity_OnDamageReceived @0x4af85b..0x4af878]
    w->round_sim.hits.push_back(RoundHit{npc_h, shooter_h, 10, 1, 1});

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1; // not a 32-tick perception scan: preserve lastAttacker
    w->update_all_entities(ctx);

    CHECK(npc.slot.bytes()[AiSlot::kAlertByte] == 2);
    CHECK(w->script.relations.group(1).alert == TriggerRelations::kAlertRed);
    CHECK(npc.inf.damage_timer == 10); // hit lands between the 16-tick thinks
    CHECK(npc.inf.was_hit);
    CHECK(npc.inf.last_attacker == shooter_h);

    npc.inf.was_hit = false;
    npc.inf.damage_timer = 0;
    npc.inf.last_attacker = EntityHandle{};
    w->round_sim.hits.push_back(RoundHit{npc_h, npc_h, 1, 1, 2});
    ctx.logic_tick = 2;
    w->update_all_entities(ctx);
    CHECK(!npc.inf.was_hit);
    CHECK(npc.inf.damage_timer == 0);
    CHECK(!npc.inf.last_attacker.valid());

    npc.inf.damage_timer = 24;
    w->round_sim.hits.push_back(RoundHit{npc_h, shooter_h, 1, 1, 3});
    ctx.logic_tick = 3;
    w->update_all_entities(ctx);
    CHECK(npc.inf.damage_timer == 34); // callback adds 10; this is not a think tick
    ctx.logic_tick = 12; // (tick + 36*SSN 0x11) & 15 == 0 but & 63 != 0
    w->update_all_entities(ctx);
    CHECK(npc.inf.damage_timer == 34); // a think, but not the 64-tick decay
    ctx.logic_tick = 28; // (tick + 36*SSN 0x11) & 63 == 0
    w->update_all_entities(ctx);
    // The alert decays once per 64 staggered ticks [orig: Entity_UpdateInfantryAI
    // @0x4B9910 (the key & 0x3F local @0x4BA9D8; the decay @0x4BBE24..0x4BBE38)].
    CHECK(npc.inf.damage_timer == 33);
}

static void test_remote_player_hit_skips_npc_group_alert() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);

    Entity shooter_seed{};
    shooter_seed.kind = EntityKind::Organic;
    shooter_seed.health = 100;
    const EntityHandle shooter_h = w->registry.spawn(0, shooter_seed);

    Entity player_seed{};
    player_seed.kind = EntityKind::Organic;
    player_seed.health = 100;
    player_seed.net_id = 0x41;
    player_seed.group_id = 3;
    player_seed.player_class = 8;
    player_seed.engine_flags = 0x100u;
    const EntityHandle player_h = w->registry.spawn(0, player_seed);

    AiSystem &ai = w->ai;
    ai.is_authority = true;
    AiEntity &player = *ai.at(ai.attach(player_h));
    configure_rifleman(player, 0x41, 1);
    player.inf.is_local_player = false;
    w->round_sim.hits.push_back(RoundHit{player_h, shooter_h, 10, 1, 1});

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1;
    w->update_all_entities(ctx);

    CHECK(player.slot.bytes()[AiSlot::kAlertByte] == 0);
    CHECK(w->script.relations.group(3).alert != TriggerRelations::kAlertRed);
    CHECK(player.inf.was_hit);
    CHECK(player.inf.last_attacker == shooter_h);
}

static void test_mounted_gunner_acquires_and_fires() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity enemy_seed{};
    enemy_seed.kind = EntityKind::Organic;
    enemy_seed.item_id = 1001;
    enemy_seed.has_item_def = true;
    enemy_seed.item_type = 3;
    enemy_seed.team = 2;
    enemy_seed.health = 100;
    enemy_seed.net_id = 0x21;
    enemy_seed.group_id = 2;
    enemy_seed.position = Vec3{20.0f, 0.0f, 0.0f};
    const EntityHandle enemy_h = w->registry.spawn(0, enemy_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90; // engine heading 0: faces the enemy on +X.
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    w->registry.spawn(1, gun);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.group_id = 1;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    npc.profile.organic.ammo.fill(0); // mounted fire must not use the personal rifle slot
    CHECK(w->commands.mount(0x11, 0x31));

    bool acquired = false;
    bool fired = false;
    for (uint32_t tick = 0; tick < 1000 && !fired; ++tick) {
        // Exercise the production phase order: entity AI queues FIRE, the
        // frame's weapon walk consumes it, then the next projectile pass steps
        // the new round.
        // [orig: Game_ProcessMainFrame's Entity_UpdateAllEntities call @0x52674b and
        //  WeaponAction_ProcessAllEntities call @0x526786; Weapon_UpdateAllProjectiles
        //  @0x4ec020]
        w->run_logic_tick(true);
        w->pump_weapon_actions();
        acquired = acquired || npc.inf.combat_target == enemy_h;
        if (!fired && w->out.rounds.count > 0) {
            fired = true;
            // The pump runs after the entity update, so the round it fired
            // has not stepped yet. [orig: Game_ProcessMainFrame -- the
            //  Entity_UpdateAllEntities call @0x52674B (its
            //  Weapon_UpdateAllProjectiles call @0x4C223A) precedes the
            //  WeaponAction_ProcessAllEntities call @0x526786]
            bool unstepped = false;
            for (const LiveRound &round : w->round_sim.rounds)
                if (round.active && round.age_ticks == 0) unstepped = true;
            CHECK(unstepped);
        }
    }
    CHECK(acquired);
    CHECK(fired);
}

// The AI gunner's shot leaves from the gun's point for the barrel its slot's
// clip selected before the shot spent a round (a no-clip gun's 0xFFFF word
// selects barrel 3), and a def carrying a third-person model fires from its
// resolved launch point on that model instead.
// [orig: WeaponAction_Fire @0x542BF7 -> Entity_CalcWeaponFirePosition
//  @0x4DC7E6..0x4DC7F6 -> Entity_ComputeUserpointWorldTransform barrel
//  @0x545D40..0x545D4B, gfx3 leg @0x545D06..0x545D85; consume @0x542C75;
//  the no-clip word WeaponSlot_InitFromEntityDef @0x54670F..0x546713]
static void test_mounted_gunner_fires_from_the_slot_barrel() {
    struct GunPoints : IPoseProvider {
        EntityHandle gun;
        int point = 0;
        const opennova::threedi::Threedi3di3 *model = nullptr;
        bool resolve_userpoint_frame(World &, EntityHandle h, const opennova::threedi::Threedi3di3 *m,
                int index, int32_t out[6], int32_t *) override {
            if (h != gun) return false;
            point = index;
            model = m;
            out[0] = (m != nullptr ? 100 + index : index) << 16;
            out[1] = 0;
            out[2] = 1 << 16;
            out[3] = 0; out[4] = 0; out[5] = 0;
            return true;
        }
    };
    const opennova::threedi::Threedi3di3 gfx3{};
    for (const bool third_person : {false, true}) {
        auto w = std::make_unique<World>();
        w->registry.configure_pool(0, 8);
        w->registry.configure_pool(1, 8);
        seed_test_rifle_ammo(*w);

        Entity enemy_seed{};
        enemy_seed.kind = EntityKind::Organic;
        enemy_seed.item_id = 1001;
        enemy_seed.has_item_def = true;
        enemy_seed.item_type = 3;
        enemy_seed.team = 2;
        enemy_seed.health = 100;
        enemy_seed.net_id = 0x21;
        enemy_seed.group_id = 2;
        enemy_seed.position = Vec3{20.0f, 0.0f, 0.0f};
        w->registry.spawn(0, enemy_seed);

        Entity gun{};
        gun.kind = EntityKind::Item;
        gun.has_item_def = true; // the boarding needs the def (D-NET-422); the fixture,
                                 // built around a def-less gun, drops it below
        gun.team = 1;
        gun.net_id = 0x31;
        gun.yaw = 90;
        gun.item_attrib = kItemAttribEweap;
        gun.primary_weapon.assign(1, 'x');
        for (int barrel = 0; barrel < 4; ++barrel)
            gun.weapon_userpoint_bytes[barrel][0] = static_cast<uint8_t>(10 + barrel);
        Seat seat{};
        seat.type = SeatType::Gunner;
        gun.seats.push_back(seat);
        GunPoints points;
        points.gun = w->registry.spawn(1, gun);
        w->pose_provider = &points;
        w->tables.weapons.entries.resize(2);
        w->tables.weapons.entries[1].name.assign(1, 'x');
        w->tables.weapons.entries[1].ammo_index = 1;
        w->tables.weapons.entries[1].valid = true;
        configure_test_emplacement_weapon(w->tables.weapons.entries[1]);
        if (third_person) {
            w->tables.weapons.entries[1].third_person_model_asset =
                    std::shared_ptr<const opennova::threedi::Threedi3di3>(
                            &gfx3, [](const opennova::threedi::Threedi3di3 *) {});
            w->tables.weapons.entries[1].launch_userpoint = 5;
        }

        Entity npc_seed{};
        npc_seed.kind = EntityKind::Organic;
        npc_seed.team = 1;
        npc_seed.health = 100;
        npc_seed.net_id = 0x11;
        npc_seed.group_id = 1;
        const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

        AttackEventSource clips;
        AiSystem &ai = w->ai;
        ai.is_authority = true;
        ai.root_motion = &clips;
        AiEntity &npc = *ai.at(ai.attach(npc_h));
        configure_rifleman(npc, 0x11, 1);
        npc.profile.organic.ammo.fill(0);
        CHECK(w->commands.mount(0x11, 0x31));
        w->registry.get(points.gun)->has_item_def = false; // the def-less gun past boarding
        CHECK(w->registry.get(points.gun)->primary_weapon_slot.clip == -1);

        bool fired = false;
        for (uint32_t tick = 0; tick < 1000 && !fired; ++tick) {
            w->run_logic_tick(true);
            w->pump_weapon_actions(); // the frame's weapon walk
            fired = w->out.rounds.count > 0;
        }
        CHECK(fired);
        if (fired)
            CHECK(w->out.rounds.records[0].origin_x == (third_person ? 105 : 13) << 16);
        w->pose_provider = nullptr;
    }
}

// The weapon fire POSITION and its quality, leg by leg: no item def copies the
// raw position (3); a person on a UseGun seat of an EWeap parent takes that
// parent's gun point (1); any other person its position plus CameraOffset,
// with no phase and no jitter (1); a non-person without a model the raw
// position (3); a modeled one its def+1351 LOOK point (1), else its position
// raised 0.75 u (2). [orig: Entity_GetWeaponFirePosition @0x43B630: raw
//  @0x43B7B4..0x43B7C9, gun point @0x43B64D..0x43B68A, CameraOffset
//  @0x43B68F..0x43B6B6, no model @0x43B6B7..0x43B6EC, LOOK @0x43B749..0x43B78C,
//  raised @0x43B78D..0x43B7B3]
static void test_weapon_fire_position_legs() {
    struct GunFrame : FakeMuzzleProvider {
        EntityHandle gun;
        int point = 0;
        bool resolve_userpoint_frame(World &, EntityHandle h,
                const opennova::threedi::Threedi3di3 *, int index, int32_t out[6],
                int32_t *) override {
            if (h != gun) return false;
            point = index;
            out[0] = 7 << 16; out[1] = 8 << 16; out[2] = 9 << 16;
            out[3] = 0; out[4] = 0; out[5] = 0;
            return true;
        }
    };
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 4);
    CollisionWorld collision;
    w->collision = &collision;
    GunFrame provider;
    w->pose_provider = &provider;
    AiSystem &sys = w->ai;

    Entity person_seed{};
    person_seed.kind = EntityKind::Organic;
    person_seed.health = 100;
    person_seed.team = 1;
    person_seed.net_id = 0x11;
    const EntityHandle person_h = w->registry.spawn(0, person_seed);
    Entity veh_seed{};
    veh_seed.kind = EntityKind::Item;
    veh_seed.has_item_def = true;
    veh_seed.item_type = 1;
    const EntityHandle veh_h = w->registry.spawn(1, veh_seed);
    const int person_index = sys.attach(person_h);
    const int veh_index = sys.attach(veh_h);
    AiEntity &person = *sys.at(person_index);
    configure_rifleman(person, 0x11, 1);
    person.pos[0] = 1 << 16; person.pos[1] = 2 << 16; person.pos[2] = 3 << 16;
    person.inf.eye_offset_x = 0x100;
    person.inf.eye_offset_y = -0x200;
    person.inf.eye_offset_z = 0x18000;

    int32_t out[3];
    // No item def: the raw position.
    CHECK(sys.weapon_fire_position(*w, person, out) == 3);
    CHECK(out[0] == (1 << 16) && out[1] == (2 << 16) && out[2] == (3 << 16));

    // A person: position plus CameraOffset, the same on either aim phase.
    w->registry.get(person_h)->has_item_def = true;
    w->registry.get(person_h)->item_type = 3;
    for (const uint32_t tick : {0u, 128u}) {
        w->logic_tick = tick;
        CHECK(sys.weapon_fire_position(*w, person, out) == 1);
        CHECK(out[0] == (1 << 16) + 0x100 && out[1] == (2 << 16) - 0x200 &&
              out[2] == (3 << 16) + 0x18000);
    }

    // Seated on a UseGun seat of an EWeap gun: that gun's point, the barrel
    // its no-clip word selects.
    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.team = 1;
    gun.net_id = 0x31;
    gun.has_item_def = true;
    gun.item_attrib = kItemAttribEweap;
    gun.primary_weapon.assign(1, 'x');
    for (int barrel = 0; barrel < 4; ++barrel)
        gun.weapon_userpoint_bytes[barrel][0] = static_cast<uint8_t>(10 + barrel);
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    provider.gun = w->registry.spawn(1, gun);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);
    CHECK(w->commands.mount(0x11, 0x31));
    CHECK(sys.weapon_fire_position(*w, person, out) == 1);
    CHECK(out[0] == (7 << 16) && out[1] == (8 << 16) && out[2] == (9 << 16));
    CHECK(provider.point == 13);
    // Without the EWeap attrib the seat's parent is no gun: the CameraOffset leg.
    w->registry.get(provider.gun)->item_attrib = 0;
    CHECK(sys.weapon_fire_position(*w, person, out) == 1);
    CHECK(out[0] == person.pos[0] + 0x100 && out[2] == person.pos[2] + 0x18000);

    // A non-person without a model: the raw position.
    AiEntity &veh = *sys.at(veh_index);
    veh.pos[0] = 10 << 16; veh.pos[1] = 20 << 16; veh.pos[2] = 30 << 16;
    CHECK(sys.weapon_fire_position(*w, veh, out) == 3);
    CHECK(out[0] == (10 << 16) && out[1] == (20 << 16) && out[2] == (30 << 16));
    // A model without a LOOK point: the position raised 0.75 u.
    collision.assign_entity(veh_h, collision.add_model(CollisionModel{}));
    CHECK(sys.weapon_fire_position(*w, veh, out) == 2);
    CHECK(out[0] == (10 << 16) && out[1] == (20 << 16) && out[2] == (30 << 16) + 0xC000);
    // The def+1351 LOOK point through the placement matrix.
    provider.rigid_points[veh_h.packed] = {11 << 16, 22 << 16, 33 << 16};
    w->registry.get(veh_h)->look_userpoint_byte = 5;
    CHECK(sys.weapon_fire_position(*w, veh, out) == 1);
    CHECK(out[0] == (11 << 16) && out[1] == (22 << 16) && out[2] == (33 << 16));
    CHECK(provider.rigid_index_seen == 5);
    w->pose_provider = nullptr;
    w->collision = nullptr;
}

// A UseGun gunner's threat scan casts its sight rays from the GUN's point,
// not from its eye, and measures range/arc from its own position.
// [orig: Entity_FindTargets @0x53A658..0x53A679 -> Entity_GetWeaponFirePosition
//  @0x43B64D..0x43B68A; metrics frame ctx[0] = entity+4 @0x4B09D0..0x4B09D3]
// In 07TR a cannon gunner's eye sits inside its own hull, which the LOS walk
// does not exclude, so an eye-origin scan never saw a target and no AI tank
// cannon ever fired. A 2 u ridge between the pair stands in for that hull
// here: it blocks every eye-level ray and none from the gun. An NPC on a
// tank turret cannon (IsTurret, one round in the breech) and one on a roof
// .50 (OnTurret, belt-fed) each acquire the enemy vehicle behind it and
// spawn rounds from the gun's point.
static void test_turret_gunners_scan_from_the_gun_point() {
    struct RidgeField {
        enum { kDim = 512 };
        std::vector<uint16_t> heightmap;
        std::vector<int> sector_grid;
        opennova::terrain::TerrainHeightField field;
        RidgeField() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
            for (int z = 0; z < kDim; ++z)
                for (int x = 8; x <= 12; ++x)
                    heightmap[z * kDim + x] = static_cast<uint16_t>(2.0 * 256.0); // 2 u
            field.heightmap = heightmap.data();
            field.dim = kDim;
            field.layout.sector_grid = sector_grid.data();
            field.layout.origin_x = 0;
            field.layout.origin_y = 0;
        }
    };
    static RidgeField ridge;
    struct GunPoint : IPoseProvider {
        EntityHandle gun;
        bool resolve_userpoint_frame(World &, EntityHandle h,
                const opennova::threedi::Threedi3di3 *, int, int32_t out[6],
                int32_t *) override {
            if (h != gun) return false;
            out[0] = 2 << 16; out[1] = 100 << 16; out[2] = 6 << 16; // above the ridge
            out[3] = 0; out[4] = 0; out[5] = 0;
            return true;
        }
    };
    for (const bool cannon : {true, false}) {
        auto w = std::make_unique<World>();
        w->registry.configure_pool(0, 8);
        w->registry.configure_pool(1, 8);
        seed_test_rifle_ammo(*w);

        Entity enemy_seed{};
        enemy_seed.kind = EntityKind::Item;
        enemy_seed.item_id = 1002;
        enemy_seed.has_item_def = true;
        enemy_seed.item_type = 1;
        enemy_seed.team = 2;
        enemy_seed.health = 1000;
        enemy_seed.net_id = 0x21;
        enemy_seed.group_id = 2;
        enemy_seed.position = Vec3{20.0f, 100.0f, 0.0f};
        const EntityHandle enemy_h = w->registry.spawn(1, enemy_seed);

        Entity gun{};
        gun.kind = EntityKind::Item;
        gun.item_id = cannon ? 100166 : 100182;
        gun.has_item_def = true;
        gun.team = 1;
        gun.net_id = 0x31;
        gun.yaw = 90; // engine heading 0: faces the enemy on +X.
        gun.position = Vec3{2.0f, 100.0f, 0.0f};
        gun.item_attrib = kItemAttribEweap;
        gun.item_attrib2 = cannon ? opennova::def::DEF_ITEM_ATTRIB2_ISTURRET
                                  : opennova::def::DEF_ITEM_ATTRIB2_ONTURRET;
        gun.primary_weapon.assign(1, 'x');
        for (int barrel = 0; barrel < 4; ++barrel)
            gun.weapon_userpoint_bytes[barrel][0] = static_cast<uint8_t>(10 + barrel);
        Seat seat{};
        seat.type = SeatType::Gunner;
        gun.seats.push_back(seat);
        GunPoint point;
        point.gun = w->registry.spawn(1, gun);
        w->pose_provider = &point;
        w->tables.weapons.entries.resize(2);
        WeaponTableEntry &weapon = w->tables.weapons.entries[1];
        weapon.name.assign(1, 'x');
        weapon.ammo_index = 1;
        weapon.valid = true;
        configure_test_emplacement_weapon(weapon);
        if (cannon) {
            // WPN_M1TURRET: one round in the breech, 40 carried.
            weapon.clipsize = 1;
            weapon.startrounds = 40;
            weapon.action_fsm.clip_capacity = 1;
        }

        Entity npc_seed{};
        npc_seed.kind = EntityKind::Organic;
        npc_seed.item_id = 1001;
        npc_seed.has_item_def = true;
        npc_seed.item_type = 3;
        npc_seed.team = 1;
        npc_seed.health = 100;
        npc_seed.net_id = 0x11;
        npc_seed.group_id = 1;
        npc_seed.position = Vec3{2.0f, 100.0f, 0.0f};
        const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

        AttackEventSource clips;
        AiSystem &ai = w->ai;
        ai.is_authority = true;
        ai.root_motion = &clips;
        ai.terrain = &ridge.field;
        AiEntity &npc = *ai.at(ai.attach(npc_h));
        configure_rifleman(npc, 0x11, 1);
        npc.pos[0] = 2 << 16;
        npc.pos[1] = 100 << 16;
        npc.profile.organic.ammo.fill(0);
        CHECK(w->commands.mount(0x11, 0x31));
        CHECK(w->registry.get(point.gun)->primary_weapon_slot.clip == (cannon ? 1 : -1));

        // The rig: a ray from anywhere up to 1 u above the gunner is blocked,
        // the gun point's ray is clear.
        int32_t aim[3], from[3];
        ai.weapon_aim_origin(*w, *w->registry.get(enemy_h), aim);
        for (const int32_t lift : {0, 1 << 16}) {
            from[0] = npc.pos[0]; from[1] = npc.pos[1]; from[2] = npc.pos[2] + lift;
            CHECK(!ai.line_of_sight_clear(*w, from, aim, npc_h, enemy_h));
        }
        CHECK(ai.weapon_fire_position(*w, npc, from) == 1);
        CHECK(from[0] == (2 << 16) && from[1] == (100 << 16) && from[2] == (6 << 16));
        CHECK(ai.line_of_sight_clear(*w, from, aim, npc_h, enemy_h));

        bool acquired = false;
        bool fired = false;
        for (uint32_t tick = 0; tick < 1000 && !fired; ++tick) {
            w->run_logic_tick(true);
            w->pump_weapon_actions(); // the frame's weapon walk
            acquired = acquired || npc.inf.combat_target == enemy_h;
            fired = fired || w->out.rounds.count > 0;
        }
        CHECK(acquired);
        CHECK(fired);
        if (fired) {
            CHECK(w->out.rounds.records[0].origin_x == (2 << 16));
            CHECK(w->out.rounds.records[0].origin_z == (6 << 16));
        }
        ai.terrain = nullptr;
        w->pose_provider = nullptr;
    }
}

// The water-crossing edge: a hull that drops below the water plane records ONE
// crossing, not one per frame, and stops recording while it stays under. This is
// the trigger behind the S2C 0x34 fan retail emits at every splash - the last
// message type our host recording was missing against the retail baseline.
// [orig: the water block @0x482BB9..0x482C9D, gated on the 0x8000 latch]
static void test_water_crossing_fires_once_on_entry() {
    World w;
    w.env.water_z = to_fixed(12.0);
    WaterCrossQueue &q = w.out.water_crossings;
    CHECK(q.events.empty());

    // Entering: below the plane with the latch clear -> exactly one record.
    q.add(to_fixed(-834.0), to_fixed(122.0), w.env.water_z, /*airborne=*/true);
    CHECK(q.events.size() == 1);
    CHECK(q.events[0].airborne);
    CHECK(q.events[0].water_z == to_fixed(12.0));

    // The queue is per-tick: the host drains and clears it, so a hull that stays
    // submerged contributes nothing further.
    q.clear();
    CHECK(q.events.empty());

    // The queue refuses to grow without bound if a pathological frame floods it.
    for (int i = 0; i < 100; ++i)
        q.add(0, 0, w.env.water_z, true);
    CHECK(q.events.size() == WaterCrossQueue::kMax);
    std::printf("  [water-cross] queue capped at %zu\n", q.events.size());
}

// The org1 float block, driven through the real motor entry point rather than by
// poking the queue: a soldier who walks into a river floats on the plane, fans
// exactly ONE splash, and the sound he fans is chosen by whether he was airborne
// when he met the water - not by the fact that he is infantry.
static void test_infantry_floats_and_splashes_once() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->env.water_z = to_fixed(12.0);

    Entity seed{};
    seed.kind = EntityKind::Organic;
    seed.team = 2;
    seed.health = 100;
    seed.net_id = 0x60;
    seed.eye_offset_z = to_fixed(1.8); // a standing body's stamped eye height
    const EntityHandle h = w->registry.spawn(0, seed);

    AiSystem &ai = w->ai;
    ai.is_authority = true;
    AiEntity &npc = *ai.at(ai.attach(h));
    Entity *ent = w->registry.get(h);

    // Shallower than the hysteresis gap: retail does NOT start floating here, so
    // a body wading the very edge of a river neither latches nor splashes. This
    // is the assertion a naive `z < water` port fails.
    npc.pos[2] = w->env.water_z - to_fixed(0.3);
    ai.infantry_water_block(npc, *w, ent, 0, 1);
    CHECK(w->out.water_crossings.events.empty());
    CHECK((ent->flags & kEntityFlagDrowning) == 0);

    // Past the gap, walking (not airborne): one crossing, carrying the wade sound.
    npc.pos[2] = w->env.water_z - to_fixed(1.0);
    const int32_t sank_to = npc.pos[2];
    ai.infantry_water_block(npc, *w, ent, 0, 1);
    CHECK(w->out.water_crossings.events.size() == 1);
    CHECK(!w->out.water_crossings.events[0].airborne);
    CHECK(w->out.water_crossings.events[0].water_z == w->env.water_z);
    CHECK((ent->flags & kEntityFlagDrowning) != 0);
    // He is being lifted toward the surface, not left on the riverbed: the block
    // stores the float target outright; the motor's +0xAC quarter-step tail that
    // follows it is what paces the settle (infantry_org1_parity). [orig:
    // Entity_UpdateInfantryAI store @0x4BFB84, tail @0x4BFC65..0x4BFC86]
    CHECK(npc.pos[2] > sank_to);

    // Still under, still latched: the host drained the queue and nothing refills it.
    w->out.water_crossings.clear();
    for (int i = 0; i < 8; ++i) ai.infantry_water_block(npc, *w, ent, 0, 2 + i);
    CHECK(w->out.water_crossings.events.empty());
    // He sits at the float target, just under the plane within the bob's amplitude.
    const int32_t settled = npc.pos[2] - w->env.water_z;
    CHECK(settled < 0 && settled > -to_fixed(1.0));
    std::printf("  [inf-water] settled %.3f u under the plane\n",
                static_cast<double>(settled) / 65536.0);

    // Out of the water clears the latch, so the next entry can splash again.
    npc.pos[2] = w->env.water_z + to_fixed(0.5);
    ai.infantry_water_block(npc, *w, ent, 0, 20);
    CHECK((ent->flags & kEntityFlagDrowning) == 0);

    // Now the same body arrives from the AIR. Same family, same plane, DIFFERENT
    // sound - the selector retail actually uses.
    ent->flags |= kEntityFlagInAir;
    npc.pos[2] = w->env.water_z - to_fixed(1.0);
    ai.infantry_water_block(npc, *w, ent, 0, 21);
    CHECK(w->out.water_crossings.events.size() == 1);
    CHECK(w->out.water_crossings.events[0].airborne);
    // Landing in water ends the fall: retail clears 0x2000 with the same store
    // that sets the float latch.
    CHECK((ent->flags & kEntityFlagInAir) == 0);
    CHECK(!npc.inf.airborne);
}

static void test_mounted_fire_uses_retail_range_and_spatial_stagger() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity target_seed{};
    target_seed.kind = EntityKind::Organic;
    target_seed.team = 2;
    target_seed.health = 100;
    target_seed.net_id = 0x21;
    target_seed.position = Vec3{1.0f, 0.0f, 10.0f};
    const EntityHandle target_h = w->registry.spawn(0, target_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // the boarding needs the def (D-NET-422); the fixture,
                             // built around a def-less gun, drops it below
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90;
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    w->registry.spawn(1, gun);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AiSystem &ai = w->ai;
    ai.is_authority = true;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    npc.profile.organic.ammo.fill(0);
    npc.slot.f[15] = 6 << 16;
    npc.inf.combat_target = target_h;
    npc.inf.aim_valid = true;
    npc.inf.aim_heading = 0;
    CHECK(w->commands.mount(0x11, 0x31));
    w->registry.get(w->registry.find_by_net_id(0x31))->has_item_def = false; // the def-less gun past boarding

    // Retail halves dz before the 3-D range test: sqrt(1^2 + (10/2)^2) < 6.
    // [orig: Entity_UpdateInfantryAI sar dz,1 @0x4bf515]
    ai.infantry_mounted_fire_pass(npc, *w, 0, 0);
    Entity *gun_live = w->registry.get(w->registry.find_by_net_id(0x31));
    CHECK(gun_live != nullptr);
    CHECK(gun_live->primary_weapon_slot.next == weapon_action::kFire);
    CHECK(w->out.rounds.count == 0);
    // The emplacement authors weapon userpoints (attrib 0x20 + a resolved
    // slot-0 fire byte: its def's in retail, the entity's fields on this
    // def-less fixture gun); the provider returns the posed point and the barrel
    // bone's euler, which the round leaves along.
    // [orig: Entity_CalcWeaponFirePosition parentSlot 3 @0x4dc7e6;
    //  Entity_FireWeaponAndSendPacket copies out[0..2] + out[3]/[4]]
    // The resolver's zero-fill copies the one authored fire byte to every
    // barrel, and a no-clip gun's clip word selects barrel 3.
    // [orig: Entity_ResolveBoneUserpoints @0x545940; the barrel select
    //  @0x545D40..0x545D4B]
    gun_live->item_attrib |= kItemAttribEweap;
    for (auto &barrel : gun_live->weapon_userpoint_bytes) barrel[0] = 1;
    FakeMuzzleProvider provider;
    provider.userpoints[gun_live->handle.packed] =
            {0x12345, -0x23456, 0x34567, 0x40000000, static_cast<int32_t>(0xFF000000u), 0};
    w->pose_provider = &provider;
    // Merely caching an owner as local cannot hand its slot to a local pump:
    // with no LocalPlayer installed the frame's weapon walk pumps it through
    // the AI. The walk reads the frame's own tick, one behind logic_tick.
    w->cached.local_player = npc_h;
    w->logic_tick = 1;
    w->pump_weapon_actions();
    CHECK(w->out.rounds.count == 1);
    CHECK(w->out.rounds.records[0].shooter_handle == npc_h.packed);
    CHECK(w->out.rounds.records[0].origin_x == 0x12345);
    CHECK(w->out.rounds.records[0].origin_y == -0x23456);
    CHECK(w->out.rounds.records[0].origin_z == 0x34567);
    CHECK(w->out.rounds.records[0].dir_yaw == 0x40000000);
    CHECK(w->out.rounds.records[0].dir_pitch == static_cast<int32_t>(0xFF000000u));
    CHECK(gun_live->primary_weapon_slot.current == weapon_action::kFire);
    const int32_t busy_next = gun_live->primary_weapon_slot.next;
    ai.infantry_mounted_fire_pass(npc, *w, 4, 4);
    CHECK(gun_live->primary_weapon_slot.next == busy_next);
    CHECK(w->out.rounds.count == 1);

    // The four-tick gate is followed by a spatial stagger. Whole-unit positions
    // leave bit 0x40 to the stagger key here, so key 64 suppresses the request.
    // [orig: Entity_UpdateInfantryAI @0x4bf4e3..0x4bf4ee]
    w->registry.get(target_h)->position.z = 0.0f;
    const int before = w->out.rounds.count;
    gun_live->primary_weapon_slot = WeaponSlotState{};
    ai.infantry_mounted_fire_pass(npc, *w, 64, 64);
    CHECK(gun_live->primary_weapon_slot.next == weapon_action::kIdle);
    w->logic_tick = 65;
    w->pump_weapon_actions();
    CHECK(w->out.rounds.count == before);

    // An installed LocalPlayer owns the local player's slot: the walk hands it
    // to that pump (here an unarmed one) and the AI never advances it too.
    gun_live->primary_weapon_slot = WeaponSlotState{};
    gun_live->primary_weapon_slot.next = weapon_action::kFire;
    LocalPlayer local(*w);
    w->local_player_state = &local;
    w->logic_tick = 69;
    w->pump_weapon_actions();
    w->local_player_state = nullptr;
    CHECK(w->out.rounds.count == before);
    CHECK(gun_live->primary_weapon_slot.next == weapon_action::kFire);

    // A held target at zero health still draws the request until the next
    // think clears it: retail tests only the target pointer.
    // [orig: Entity_UpdateInfantryAI `mov eax,[edi+0Ch]; test eax,eax; jz`
    //  @0x4BF4CF..0x4BF4D4]
    w->registry.get(target_h)->health = 0;
    gun_live->primary_weapon_slot = WeaponSlotState{};
    ai.infantry_mounted_fire_pass(npc, *w, 0, 0);
    CHECK(gun_live->primary_weapon_slot.next == weapon_action::kFire);
}

// The walk's pool-1 leg pumps an unoccupied EWEAP row's own slot only while
// its heat window is open: the kick decays and a lapsed window closes, and a
// row with an occupant, a non-EWEAP row or a cold slot is left alone.
// [orig: WeaponAction_ProcessAllEntities @0x5426CB..0x54271E -- the occupant
//  test @0x5426E9, the EWEAP bit @0x5426FF, the window @0x54270E;
//  WeaponAction_ProcessFrame -- the kick decay @0x541262..0x54129B, the
//  window close @0x54125F]
static void test_weapon_walk_pumps_hot_unoccupied_guns() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 8);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);
    w->ai.is_authority = true;
    const auto spawn_gun = [&](uint32_t attrib, bool occupied, int32_t window) {
        Entity gun{};
        gun.kind = EntityKind::Item;
        gun.has_item_def = true;
        gun.item_attrib = attrib;
        gun.primary_weapon_slot_adm = 1;
        gun.primary_weapon_slot.kick = 3;
        gun.primary_weapon_slot.heat_window_end_tick = window;
        if (occupied) gun.primary_occupant = EntityHandle::make(0, 3);
        return w->registry.spawn(1, gun);
    };
    const EntityHandle hot = spawn_gun(kItemAttribEweap, false, 50);
    const EntityHandle manned = spawn_gun(kItemAttribEweap, true, 50);
    const EntityHandle plain = spawn_gun(0, false, 50);
    const EntityHandle cold = spawn_gun(kItemAttribEweap, false, 0);
    w->logic_tick = 11; // the frame's own tick is 10, inside the window
    w->pump_weapon_actions();
    CHECK(w->registry.get(hot)->primary_weapon_slot.kick == 2);
    CHECK(w->registry.get(hot)->primary_weapon_slot.heat_window_end_tick == 50);
    CHECK(w->registry.get(manned)->primary_weapon_slot.kick == 3);
    CHECK(w->registry.get(plain)->primary_weapon_slot.kick == 3);
    CHECK(w->registry.get(cold)->primary_weapon_slot.kick == 3);
    // The window lapses: that visit closes it, and the gun drops out of the walk.
    w->logic_tick = 51;
    w->pump_weapon_actions();
    CHECK(w->registry.get(hot)->primary_weapon_slot.heat_window_end_tick == 0);
    CHECK(w->registry.get(hot)->primary_weapon_slot.kick == 1);
    w->logic_tick = 52;
    w->pump_weapon_actions();
    CHECK(w->registry.get(hot)->primary_weapon_slot.kick == 1);
    CHECK(w->out.rounds.count == 0);
}

// The frame's weapon walk visits pool 0 in slot order, each gunner pumping the
// parent slot it borrowed, whatever order the mounts sit in pool 1: the gunner
// in pool-0 slot 0 fires first although its gun is the later pool-1 row.
// [orig: WeaponAction_ProcessAllEntities @0x5426A6..0x5426C9 (pool 0 first, in
//  slot order), then @0x5426CB..0x54271E (pool 1)]
static void test_weapon_walk_visits_pool0_in_slot_order() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    const auto spawn_gun = [&](uint16_t net_id) {
        Entity gun{};
        gun.kind = EntityKind::Item;
        gun.has_item_def = true; // seats need the def (D-NET-422)
        gun.team = 1;
        gun.net_id = net_id;
        gun.yaw = 90;
        gun.primary_weapon.assign(1, 'x');
        Seat seat{};
        seat.type = SeatType::Gunner;
        gun.seats.push_back(seat);
        return w->registry.spawn(1, gun);
    };
    const auto spawn_npc = [&](uint16_t net_id) {
        Entity seed{};
        seed.kind = EntityKind::Organic;
        seed.team = 1;
        seed.health = 100;
        seed.net_id = net_id;
        const EntityHandle handle = w->registry.spawn(0, seed);
        configure_rifleman(*ai.at(ai.attach(handle)), net_id, 1);
        return handle;
    };
    const EntityHandle first_gun = spawn_gun(0x31);   // pool-1 slot 0
    const EntityHandle second_gun = spawn_gun(0x32);  // pool-1 slot 1
    const EntityHandle early = spawn_npc(0x11);       // pool-0 slot 0
    const EntityHandle late = spawn_npc(0x12);        // pool-0 slot 1
    CHECK(w->commands.mount(0x12, 0x31));
    CHECK(w->commands.mount(0x11, 0x32));
    w->registry.get(first_gun)->primary_weapon_slot.next = weapon_action::kFire;
    w->registry.get(second_gun)->primary_weapon_slot.next = weapon_action::kFire;
    w->logic_tick = 1;
    w->pump_weapon_actions();
    CHECK(w->out.rounds.count == 2);
    CHECK(w->out.rounds.records[0].shooter_handle == early.packed);
    CHECK(w->out.rounds.records[1].shooter_handle == late.packed);
}

// T8: past the four-tick cadence and the spatial stagger the mounted request
// copies the parent's AdmDef byte into the rider's; an off-cadence pass leaves
// the rider's byte alone. [orig: Entity_UpdateInfantryAI @0x4BF4F4..0x4BF4FA,
// the cadence @0x4BF4DA, the stagger @0x4BF4E3..0x4BF4EE]
static void test_mounted_request_copies_the_parent_adm_byte() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity target_seed{};
    target_seed.kind = EntityKind::Organic;
    target_seed.team = 2;
    target_seed.health = 100;
    target_seed.net_id = 0x21;
    target_seed.position = Vec3{1.0f, 0.0f, 0.0f};
    const EntityHandle target_h = w->registry.spawn(0, target_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90;
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w->registry.spawn(1, gun);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AiSystem &ai = w->ai;
    ai.is_authority = true;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    npc.slot.f[15] = 6 << 16;
    npc.inf.combat_target = target_h;
    CHECK(w->commands.mount(0x11, 0x31));
    CHECK(w->registry.get(gun_h)->primary_weapon_slot_adm == 1);
    w->registry.get(npc_h)->equipped_adm_index = 7; // the rider's personal byte

    ai.infantry_mounted_fire_pass(npc, *w, 1, 1); // off the four-tick cadence
    CHECK(w->registry.get(npc_h)->equipped_adm_index == 7);
    ai.infantry_mounted_fire_pass(npc, *w, 0, 0);
    CHECK(w->registry.get(npc_h)->equipped_adm_index == 1);
}

// A mounted body still runs the anim-event fire block: a gunner whose seat clip
// raises the primary fire bit fires its own primary ammo on the odd tick,
// beside the dedicated request. [orig: Entity_UpdateInfantryAI odd-tick gate
// @0x4BF156, the 0x4 leg @0x4BF322..0x4BF38C; no seat test before the
// request's mounted-live test @0x4BF4B3]
static void test_mounted_gunner_runs_the_anim_event_fire_block() {
    struct SeatClipFires final : IRootMotionSource {
        bool has_clip(int, int id) const override {
            return id == anim_state::kIdle || (id >= 67 && id <= 75);
        }
        int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
        bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
            if (!has_clip(0, id)) return false;
            ++phase;
            out = RootMotionFrame{};
            if (id >= 67 && id <= 75) out.events = 0x4;
            return true;
        }
    };
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90;
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    w->registry.spawn(1, gun);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    SeatClipFires clips;
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    CHECK(w->commands.mount(0x11, 0x31));

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    for (uint32_t tick = 1; tick <= 6; ++tick) {
        ctx.logic_tick = tick;
        w->update_all_entities(ctx);
    }
    CHECK(w->registry.get(npc_h)->mounted);
    CHECK(npc.inf.body_clip_state() == anim_state::kEmplaced);
    CHECK(w->out.rounds.count > 0);
    if (w->out.rounds.count > 0)
        CHECK(w->out.rounds.records[0].shooter_handle == npc_h.packed);
}

// The dedicated request runs for every mounted-live body, whatever its seat: a
// passenger takes the parent's AdmDef byte past the cadence and the stagger (a
// weaponless parent's is its spawn zero) and, holding no EquippedSlot of the
// parent's, queues nothing. [orig: Entity_UpdateInfantryAI mounted-live test
// @0x4BF4B3, the copy @0x4BF4F4..0x4BF4FA, the EquippedSlot test
// @0x4BF564..0x4BF56C; WeaponSlot_InitFromEntityDef name test @0x5466E1]
static void test_mounted_request_runs_for_a_passenger() {
    for (const bool armed : {true, false}) {
        auto w = std::make_unique<World>();
        w->registry.configure_pool(0, 8);
        w->registry.configure_pool(1, 8);
        seed_test_rifle_ammo(*w);

        Entity target_seed{};
        target_seed.kind = EntityKind::Organic;
        target_seed.team = 2;
        target_seed.health = 100;
        target_seed.net_id = 0x21;
        target_seed.position = Vec3{1.0f, 0.0f, 0.0f};
        const EntityHandle target_h = w->registry.spawn(0, target_seed);

        Entity truck{};
        truck.kind = EntityKind::Item;
        truck.has_item_def = true; // seats need the def (D-NET-422)
        truck.team = 1;
        truck.net_id = 0x31;
        truck.yaw = 90;
        if (armed) truck.primary_weapon.assign(1, 'x');
        Seat seat{};
        seat.type = SeatType::Passenger;
        truck.seats.push_back(seat);
        const EntityHandle truck_h = w->registry.spawn(1, truck);
        w->tables.weapons.entries.resize(2);
        w->tables.weapons.entries[1].name.assign(1, 'x');
        w->tables.weapons.entries[1].ammo_index = 1;
        w->tables.weapons.entries[1].valid = true;
        configure_test_emplacement_weapon(w->tables.weapons.entries[1]);

        Entity npc_seed{};
        npc_seed.kind = EntityKind::Organic;
        npc_seed.team = 1;
        npc_seed.health = 100;
        npc_seed.net_id = 0x12;
        const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

        AiSystem &ai = w->ai;
        ai.is_authority = true;
        AiEntity &npc = *ai.at(ai.attach(npc_h));
        configure_rifleman(npc, 0x12, 1);
        npc.inf.combat_target = target_h;
        CHECK(w->commands.mount(0x12, 0x31));
        CHECK(w->registry.get(npc_h)->mount_type == SeatType::Passenger);
        w->registry.get(npc_h)->equipped_adm_index = 7; // the rider's personal byte

        // key = 4 + 36 * 0x12 = 652: the four-tick cadence and the stagger both
        // pass, and the sixteen-tick think has not yet dropped the target.
        TickContext ctx{};
        ctx.world = w.get();
        ctx.is_authority = true;
        ctx.logic_tick = 4;
        w->update_all_entities(ctx);
        CHECK(w->registry.get(npc_h)->mounted);
        CHECK(w->registry.get(npc_h)->equipped_adm_index == (armed ? 1 : 0));
        CHECK(w->registry.get(truck_h)->primary_weapon_slot.next == weapon_action::kIdle);
    }
}

// The rider copies the parent's live AdmDef byte: the ammo byte the parent's own
// AI fire last wrote there overrides the byte its weapon slot seeded.
// [orig: Entity_UpdateInfantryAI @0x4BF4F4..0x4BF4FA; AIEntity_ProcessWeaponFire
//  @0x472F23; WeaponSlot_InitFromEntityDef @0x546742]
static void test_mounted_request_copies_the_parents_live_byte() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity target_seed{};
    target_seed.kind = EntityKind::Organic;
    target_seed.team = 2;
    target_seed.health = 100;
    target_seed.net_id = 0x21;
    target_seed.position = Vec3{1.0f, 0.0f, 0.0f};
    const EntityHandle target_h = w->registry.spawn(0, target_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90;
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w->registry.spawn(1, gun);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AiSystem &ai = w->ai;
    ai.is_authority = true;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    npc.slot.f[15] = 6 << 16;
    npc.inf.combat_target = target_h;
    CHECK(w->commands.mount(0x11, 0x31));
    CHECK(w->registry.get(gun_h)->primary_weapon_slot_adm == 1);
    w->registry.get(gun_h)->equipped_adm_index = 5; // the gun's own AI fire wrote it
    ai.infantry_mounted_fire_pass(npc, *w, 0, 0);
    CHECK(w->registry.get(npc_h)->equipped_adm_index == 5);
}

static void test_mounted_look_traverses_before_fire_request() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity target_seed{};
    target_seed.kind = EntityKind::Organic;
    target_seed.team = 2;
    target_seed.health = 100;
    target_seed.net_id = 0x21;
    target_seed.position = Vec3{20.0f, 20.0f, 0.0f};
    const EntityHandle target_h = w->registry.spawn(0, target_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90;
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w->registry.spawn(1, gun);
    w->tables.weapons.entries.resize(2);
    w->tables.weapons.entries[1].name.assign(1, 'x');
    w->tables.weapons.entries[1].ammo_index = 1;
    w->tables.weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->tables.weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AiSystem &ai = w->ai;
    ai.is_authority = true;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    npc.inf.combat_target = target_h;
    npc.inf.aim_valid = true;
    npc.inf.aim_heading = 0x20000000;
    npc.inf.aim_pitch = 0;
    npc.heading = 0;
    CHECK(w->commands.mount(0x11, 0x31));

    CHECK(ai.pose_if_mounted(npc, *w));
    CHECK(npc.heading == 0x02000000);
    ai.infantry_mounted_fire_pass(npc, *w, 0, 0);
    CHECK(w->registry.get(gun_h)->primary_weapon_slot.next == weapon_action::kIdle);

    bool queued = false;
    for (uint32_t phase = 4; phase <= 60 && !queued; phase += 4) {
        CHECK(ai.pose_if_mounted(npc, *w));
        ai.infantry_mounted_fire_pass(npc, *w, phase, phase);
        queued = w->registry.get(gun_h)->primary_weapon_slot.next ==
                weapon_action::kFire;
    }
    CHECK(queued);
}

static void test_mounted_gunner_dismounts_into_death_animation() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.net_id = 0x31;
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w->registry.spawn(1, gun);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.deathtime_ticks = 124;
    npc_seed.equipped_adm_index = 7;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    npc.inf.active = true;
    npc.health = 100;
    CHECK(w->commands.mount(0x11, 0x31));

    constexpr int kSelectedDeath = 184;
    w->registry.get(npc_h)->health = 0;
    w->registry.get(npc_h)->death_anim_state = kSelectedDeath;
    npc.health = 0;

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1;
    w->update_all_entities(ctx);

    CHECK(!w->registry.get(npc_h)->mounted);
    CHECK(!w->registry.get(gun_h)->seats[0].occupant.valid());
    // The detach zeroes a non-player's AdmDef byte. [orig: Entity_DetachFromVehicle
    //  @0x43569C]
    CHECK(w->registry.get(npc_h)->equipped_adm_index == 0);
    CHECK(!w->registry.get(npc_h)->use_gun_slot_swapped);
    CHECK(!w->registry.get(gun_h)->primary_weapon_owner.valid());
    CHECK(npc.inf.anim_state == kSelectedDeath);
    CHECK(w->registry.get(npc_h)->death_anim_state == 0);
    CHECK(w->registry.get(npc_h)->corpse_timer == 123);
    CHECK(npc.inf.body_clip_state() == anim_state::kIdle);
    CHECK(npc.inf.clip_phase == 1); // death edge retains this tick's playing channel
    CHECK(npc.inf.anim_blend_weight == 1.0f);
}

static void test_mounted_collision_tail_uses_retail_eight_tick_phase_without_models() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 4);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.has_item_def = true; // the boarding needs the def (D-NET-422); the
                             // test's subject, a def-less parent, drops it below
    gun.net_id = 0x20;
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w->registry.spawn(1, gun);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x10;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    CollisionWorld collision;
    AiSystem &ai = w->ai;
    ai.is_authority = true;
    ai.collision = &collision;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x10, 1);
    CHECK(w->commands.mount(0x10, 0x20));
    w->registry.get(gun_h)->has_item_def = false; // the def-less gun past boarding
    CHECK(collision.instance_count() == 0);

    Entity *npc_live = w->registry.get(npc_h);
    CHECK(npc_live != nullptr);
    npc_live->flags |= kEntityFlagArmoryZone;

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1;
    w->update_all_entities(ctx);
    CHECK((npc_live->flags & kEntityFlagArmoryZone) != 0);

    // key = tick + 36*net_id; net id 0x10 leaves the low three bits unchanged.
    // The mounted tail therefore resolves at tick 8 even with no model instances,
    // clearing the resolver's transient contact flags only on that retail phase.
    // [orig: Entity_UpdateInfantryAI @0x4bf5a5..0x4bf5c3]
    ctx.logic_tick = 8;
    w->update_all_entities(ctx);
    CHECK((npc_live->flags & kEntityFlagArmoryZone) == 0);
}

// A joiner still runs each vehicle's retail physics callback for presentation
// evaluation, but its compact-record copy is wire-posed rather than motor-integrated.
// Sound therefore reads the current replicated speed/claimant state without advancing
// the vehicle transform. [orig: Entity_UpdateVehiclePhysics @0x48af00; movement-sound
// call @0x48d181..0x48d1c4]
static void test_joiner_evaluates_vehicle_idle_without_integrating_motor() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 4);

    static constexpr char kProfile[] =
            "begin \"SP_JoinerTruck\"\r\n"
            "  Soundloop_1 V_TRUCK_ILP .8 1.2\r\n"
            "end\r\n";
    CHECK(w->tables.sound_profiles.parse(kProfile, sizeof(kProfile) - 1) == 1);

    Entity npc{};
    npc.kind = EntityKind::Organic;
    npc.health = 100;
    npc.alive = true;
    const EntityHandle npc_h = w->registry.spawn(0, npc);

    Entity vehicle{};
    vehicle.kind = EntityKind::Item;
    vehicle.item_id = 1291;
    vehicle.health = 3000;
    vehicle.health_max = 3000;
    vehicle.alive = true;
    vehicle.position = Vec3{100.0f, 200.0f, 10.0f};
    vehicle.yaw = 37;
    vehicle.veh.speed = 0;
    vehicle.primary_occupant = npc_h; // NPC claimant: the PlayerControl loop gate
    const EntityHandle vehicle_h = w->registry.spawn(1, vehicle);
    Entity *npc_live = w->registry.get(npc_h);
    CHECK(npc_live != nullptr);
    npc_live->mounted = true;
    npc_live->mount_target = vehicle_h;

    VehicleTraits traits{};
    traits.physics = 1;
    traits.player_speed = 94 * 293;
    traits.player_control = true;
    traits.sound_profile = "SP_JoinerTruck";
    w->vehicles.traits.set(vehicle.item_id, traits);

    AiSystem &ai = w->ai;

    const Vec3 before = w->registry.get(vehicle_h)->position;
    const int16_t yaw_before = w->registry.get(vehicle_h)->yaw;
    w->run_logic_tick(false);

    const Entity *after = w->registry.get(vehicle_h);
    CHECK(after != nullptr);
    CHECK(after->position.x == before.x);
    CHECK(after->position.y == before.y);
    CHECK(after->position.z == before.z);
    CHECK(after->yaw == yaw_before);

    CHECK(w->out.sound_emitters.size() == 1);
    if (w->out.sound_emitters.size() == 1) {
        const SoundEmitterEvent &idle = w->out.sound_emitters[0];
        CHECK(idle.source_spawn_id == after->registry_spawn_id);
        CHECK(idle.source_handle == vehicle_h.packed);
        CHECK(idle.pos.x == before.x);
        CHECK(idle.pos.y == before.y);
        CHECK(idle.pos.z == before.z);
        CHECK(idle.lane == 0);
        CHECK(idle.slot == 0);
        CHECK(idle.lifetime_ticks == 30);
        CHECK(idle.pitch_q16 == 0x10000);
        CHECK(idle.volume_q8_8 == 0xFFFF);
        CHECK(idle.set_name == "V_TRUCK_ILP");
    }

    // Client presentation validates the full mount relationship instead of
    // trusting a non-null replicated handle forever. A stale claimant may keep
    // moving a residual lane's source anchor during its original lifetime, but
    // it must not refresh the idle registration indefinitely.
    w->out.sound_emitters.clear();
    npc_live->mounted = false;
    w->run_logic_tick(false);
    CHECK(w->out.sound_emitters.size() == 1);
    if (w->out.sound_emitters.size() == 1) {
        CHECK(w->out.sound_emitters[0].source_only);
    }
    for (int i = 0; i < 30; ++i) {
        w->out.sound_emitters.clear();
        w->run_logic_tick(false);
    }
    CHECK(w->out.sound_emitters.empty());
}

// Exercise the distinct aircraft combat legs through the dispatch table. The
// pose provider makes muzzle selection and full pitch/roll observable without
// relying on a proprietary model fixture.
static void test_aircraft_combat_states() {
	auto owned = std::make_unique<World>();
	World &w = *owned;
	AiSystem &sys = w.ai;
	w.registry.configure_pool(0, 8);
	w.registry.configure_pool(1, 8);
	std::vector<uint16_t> heights(64 * 64, 0);
	std::vector<int> sectors(256, 1);
	opennova::terrain::TerrainHeightField terrain;
	terrain.heightmap = heights.data();
	terrain.dim = 64;
	terrain.layout.sector_grid = sectors.data();
	w.tables.terrain = &terrain;
	w.tables.ammo.entries.resize(2);
	w.tables.ammo.entries[1].valid = true;
	w.tables.ammo.entries[1].velocity = 620;
	w.tables.ammo.entries[1].max_age_ticks = 100;
	Entity hull;
	hull.health = 500;
	hull.alive = true;
	hull.position = { 0, 0, 10 };
	hull.team = 1;
	const EntityHandle sh = w.registry.spawn(1, hull);
	Entity victim;
	victim.health = 100;
	victim.alive = true;
	victim.position = { 100, 0, 10 };
	victim.team = 2;
	const EntityHandle th = w.registry.spawn(0, victim);
	AiEntity &ai = *sys.at(sys.attach(sh));
	sys.is_authority = true;
	auto &b = ai.brain;
	ai.health = 500;
	ai.pos[2] = 10 << 16;
	ai.team = 1;
	ai.profile.type = 1;
	ai.profile.field220 = 6000;
	ai.profile.patrol_climb = 3000;
	ai.profile.field216 = 20 << 16;
	ai.profile.patrol_altitude = 12 << 16;
	ai.profile.min_agl = 5 << 16;
	ai.profile.min_chase = 20 << 16;
	ai.profile.max_chase = 60 << 16;
	ai.profile.approach_cap = 1000 << 16;
	ai.profile.radar_fov_bam = INT32_MAX;
	ai.profile.fov_secondary = 0x7f;
	ai.profile.fire_a.ammo_index = 1;
	ai.profile.fire_a.cone_bam = INT32_MAX;
	ai.profile.fire_b = ai.profile.fire_a;
	ai.profile.fire_interval_a = ai.profile.fire_interval_b = 1;
	b.f[AiBrain::kSpeedA] = 2000;
	b.f[AiBrain::kSpeedB] = 1000;
	b.f[AiBrain::kFallback] = 7;
	b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState] = 8;
	AiThinkCtx ctx{ &sys, &ai, &w, nullptr };
	sys.row(8).enter(ctx);
	CHECK(ai.aircraft_controller == 0x10000 && b.f[45] == 1 && b.f[AiBrain::kStep] == 1);
	sys.ai_set_target(w, ai, th);
	b.f[AiBrain::kAmmoA] = 4;
	b.f[AiBrain::kAccuracy] = 4;
	ai.profile.flags100 = 0x40; // locked burst
	b.f[AiBrain::kTickAccum] = 15;
	const uint32_t random_before = sys.prng_a;
	sys.row(8).tick(ctx);
	CHECK(w.out.rounds.count == 1);
	CHECK(b.f[AiBrain::kAmmoA] == 3 && b.f[AiBrain::kLastWeapon] == 1);
	CHECK(b.f[AiBrain::kBurstWindow] == 1 && sys.prng_a == random_before);
	CHECK((b.bytes()[AiBrain::kBoneFlagByte] & 0x40) != 0);
	// A moving, rolling hull continues the saved six-component burst without
	// solving again or consuming scatter randomness.
	ai.pos[0] = 4 << 16;
	ai.pos[2] = 15 << 16;
	ai.heading = 12345;
	ai.pitch = 23456;
	ai.roll = 34567;
	const int32_t saved_z = b.f[AiBrain::kSavedDeltaA + 2];
	sys.row(8).tick(ctx);
	CHECK(w.out.rounds.count == 2 && b.f[AiBrain::kAmmoA] == 2);
	if (w.out.rounds.count == 2) {
		CHECK(w.out.rounds.records[1].origin_z == ai.pos[2] + saved_z);
		CHECK(w.out.rounds.records[1].dir_yaw == ai.heading + b.f[AiBrain::kSavedDeltaA + 3]);
		CHECK(w.out.rounds.records[1].dir_pitch == ai.pitch + b.f[AiBrain::kSavedDeltaA + 4]);
	}
	CHECK(sys.prng_a == random_before);
	// Secondary processed fire omits the primary fire bit and perfect-aim
	// scatter; the secondary continuation site still draws its two values.
	ai.pos[0] = 0;
	ai.pos[2] = 10 << 16;
	ai.heading = ai.pitch = ai.roll = 0;
	ai.profile.flags100 = 0;
	b.f[AiBrain::kBurstWindow] = 0;
	b.f[AiBrain::kAmmoA] = 0;
	b.f[AiBrain::kAmmoB] = 3;
	b.f[AiBrain::kTickAccum] = 15;
	sys.row(8).tick(ctx);
	CHECK(w.out.rounds.count == 3 && b.f[AiBrain::kLastWeapon] == 2);
	CHECK((b.bytes()[AiBrain::kBoneFlagByte] & 0x40) == 0 && sys.prng_a == random_before);
	sys.row(8).tick(ctx);
	CHECK(w.out.rounds.count == 4 && b.f[AiBrain::kAmmoB] == 1 && sys.prng_a != random_before);
	// Stationary weapons are commanded separately from the AI movement mode.
	ai.profile.flags100 = 0x80;
	b.bytes()[AiBrain::kGuardFireByte] = 0;
	b.f[AiBrain::kCombatTimer] = 620;
	b.f[AiBrain::kTickAccum] = 0;
	sys.row(8).tick(ctx);
	CHECK(b.f[AiBrain::kPendState] == 7 && w.out.rounds.count == 4);
	// Damage source selects the directional evasion controller and survives
	// the full event dispatch as a handle, rather than a damage quantity.
	ai.profile.flags100 = 0;
	ai.profile.flags96 = 0;
	sys.ai_set_target(w, ai, EntityHandle{});
	AiEventEntry damage{};
	damage.f[0] = 1;
	damage.f[1] = sys.index_of(ai) << 16;
	damage.f[3] = th.packed + 1;
	ctx.event = &damage;
	sys.row(8).event(ctx);
	ctx.event = nullptr;
	CHECK(b.f[AiBrain::kDamageInfo] == th.packed + 1 && b.f[AiBrain::kPendState] == 10);
	sys.apply_transition(ai, w);
	CHECK(b.f[AiBrain::kCurState] == 10 && ai.aircraft_controller == 1);
	CHECK(b.f[AiBrain::kTargetSlot] == th.packed + 1 && b.f[AiBrain::kWorkHeading] == 1073741760);
	ai.aircraft_phase = 180;
	sys.row(10).tick(ctx);
	CHECK(ai.aircraft_phase == 196 && b.f[AiBrain::kWorkHeading] == 0);
	ai.aircraft_phase = 372;
	sys.row(10).tick(ctx);
	CHECK(ai.aircraft_controller == 3 && ai.aircraft_result == 0 && b.f[AiBrain::kPendState] == 8);
	ai.profile.subtype = 2;
	b.f[45] = 0;
	sys.row(8).enter(ctx);
	CHECK(ai.aircraft_controller == 0x10005 && b.f[45] == -1);
	b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState] = 10;
	sys.row(10).enter(ctx);
	CHECK(b.f[AiBrain::kPendState] == 8);
}

// The aircraft combat tick's ready block writes its ammo byte into the hull's
// AdmDef byte ahead of its solve, as the ground machine does.
// [orig: AI_TickState_AircraftCombat processed legs @0x4724A4..0x4724AF /
//  @0x47258E..0x472599]
static void test_aircraft_fire_stamps_the_hull_adm_byte() {
	auto owned = std::make_unique<World>();
	World &w = *owned;
	AiSystem &sys = w.ai;
	w.registry.configure_pool(0, 8);
	w.registry.configure_pool(1, 8);
	std::vector<uint16_t> heights(64 * 64, 0);
	std::vector<int> sectors(256, 1);
	opennova::terrain::TerrainHeightField terrain;
	terrain.heightmap = heights.data();
	terrain.dim = 64;
	terrain.layout.sector_grid = sectors.data();
	w.tables.terrain = &terrain;
	w.tables.ammo.entries.resize(3);
	for (size_t i = 1; i <= 2; ++i) {
		w.tables.ammo.entries[i].valid = true;
		w.tables.ammo.entries[i].velocity = 620;
		w.tables.ammo.entries[i].max_age_ticks = 100;
	}
	Entity hull;
	hull.health = 500;
	hull.alive = true;
	hull.position = { 0, 0, 10 };
	hull.team = 1;
	const EntityHandle sh = w.registry.spawn(1, hull);
	Entity victim;
	victim.health = 100;
	victim.alive = true;
	victim.position = { 100, 0, 10 };
	victim.team = 2;
	const EntityHandle th = w.registry.spawn(0, victim);
	AiEntity &ai = *sys.at(sys.attach(sh));
	sys.is_authority = true;
	auto &b = ai.brain;
	ai.health = 500;
	ai.pos[2] = 10 << 16;
	ai.team = 1;
	ai.profile.type = 1;
	ai.profile.field220 = 6000;
	ai.profile.patrol_climb = 3000;
	ai.profile.field216 = 20 << 16;
	ai.profile.patrol_altitude = 12 << 16;
	ai.profile.min_agl = 5 << 16;
	ai.profile.min_chase = 20 << 16;
	ai.profile.max_chase = 60 << 16;
	ai.profile.approach_cap = 1000 << 16;
	ai.profile.radar_fov_bam = INT32_MAX;
	ai.profile.fov_secondary = 0x7f;
	ai.profile.fire_a.ammo_index = 1;
	ai.profile.fire_a.cone_bam = INT32_MAX;
	ai.profile.fire_b = ai.profile.fire_a;
	ai.profile.fire_b.ammo_index = 2;
	ai.profile.fire_interval_a = ai.profile.fire_interval_b = 1;
	b.f[AiBrain::kSpeedA] = 2000;
	b.f[AiBrain::kSpeedB] = 1000;
	b.f[AiBrain::kFallback] = 7;
	b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState] = 8;
	AiThinkCtx ctx{ &sys, &ai, &w, nullptr };
	sys.row(8).enter(ctx);
	sys.ai_set_target(w, ai, th);
	b.f[AiBrain::kAmmoA] = 0;
	b.f[AiBrain::kAmmoB] = 3;
	b.f[AiBrain::kAccuracy] = 4;
	b.f[AiBrain::kTickAccum] = 15;
	CHECK(w.registry.get(sh)->equipped_adm_index == kAdmSlotNone);
	sys.row(8).tick(ctx);
	CHECK(w.out.rounds.count == 1 && b.f[AiBrain::kLastWeapon] == 2);
	CHECK(w.registry.get(sh)->equipped_adm_index == 2);
}

static void test_vehicle_weapon_pose_and_target_cleanup() {
	struct Pose : IPoseProvider {
		int point = 0, pivots = 0, poses = 0;
		bool resolve_userpoint_rigid(World &, EntityHandle, int index, int32_t out[3]) override {
			point = index;
			out[0] = index << 16;
			out[1] = 0;
			out[2] = 10 << 16;
			return true;
		}
		bool resolve_userpoint_pivot(World &, EntityHandle, int index, int32_t out[3]) override {
			point = index;
			++pivots;
			out[0] = out[1] = 0;
			out[2] = 10 << 16;
			return true;
		}
		bool resolve_userpoint_transform(
				World &, EntityHandle, int index, int32_t out[6]) override {
			point = index;
			++poses;
			out[0] = 8 << 16;
			out[1] = 9 << 16;
			out[2] = 10 << 16;
			out[3] = out[4] = out[5] = 0;
			return true;
		}
	} pose;
	auto owned = std::make_unique<World>();
	World &w = *owned;
	auto &sys = w.ai;
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(1, 4);
	w.pose_provider = &pose;
	Entity hull;
	hull.health = 500;
	hull.alive = true;
	hull.position = { 0, 0, 10 };
	const EntityHandle sh = w.registry.spawn(1, hull);
	Entity target;
	target.health = 100;
	target.alive = true;
	target.position = { 100, 0, 10 };
	const EntityHandle th = w.registry.spawn(0, target);
	sys.attach(sh);
	sys.attach(th);
	AiEntity &ai = *sys.for_handle(sh);
	auto &b = ai.brain;
	ai.pos[2] = 10 << 16;
	ai.pitch = 0x10000000;
	ai.roll = 0x20000000;
	ai.profile.fire_a.flags = 0x10;
	b.f[AiBrain::kBoneCountA] = 3;
	b.f[56] = 2;
	b.f[57] = 3;
	b.f[58] = 4;
	b.f[AiBrain::kAmmoA] = 5;
	int32_t out[6];
	CHECK(sys.solve_weapon_fire_transform(w, ai, nullptr, ai.profile.fire_a, 0, false, out));
	CHECK(pose.point == 4 && out[0] == (4 << 16) && out[2] == (10 << 16));
	CHECK(out[4] == ai.pitch - 536870880 && out[5] == ai.roll);
	CHECK(b.bytes()[AiBrain::kBoneFlagByte] == 0x82 && pose.poses == 0);
	ai.profile.fire_a.flags = 0x11;
	b.f[AiBrain::kAmmoA] = -1;
	CHECK(sys.solve_weapon_fire_transform(w, ai, nullptr, ai.profile.fire_a, 0, false, out));
	CHECK(pose.point == 2 && pose.pivots == 1 && pose.poses == 1 && out[0] == (8 << 16));
	CHECK(b.f[AiBrain::kBoneRoundRobin] == -2);
	CHECK(sys.solve_weapon_fire_transform(w, ai, nullptr, ai.profile.fire_a, 0, false, out));
	CHECK(pose.point == 4 && b.f[AiBrain::kBoneRoundRobin] == -3);
	// Aim straight up from a pitched hull. A yaw-only solver rejects this as
	// outside the narrow cone; the full local-frame solve is aligned.
	b.f[AiBrain::kBoneCountA] = 0;
	ai.profile.fire_a.flags = 0;
	ai.profile.fire_a.cone_bam = 0;
	ai.profile.radar_fov_bam = INT32_MAX;
	ai.profile.approach_cap = 1000 << 16;
	ai.pitch = 0x40000000;
	ai.roll = 0;
	w.registry.get(th)->position = { 0, 0, 100 };
	CHECK(sys.solve_weapon_fire_transform(
			w, ai, w.registry.get(th), ai.profile.fire_a, 0, true, out));
	CHECK(std::abs(int64_t(out[4]) - 0x40000000) < 8192);
	// Death cleanup clears priority/damage pointers and the dying brain's
	// target, while retaining the original cross-brain SetAITarget quirk.
	AiEntity &other = *sys.for_handle(th);
	sys.ai_set_target(w, ai, th);
	sys.ai_set_target(w, other, sh);
	other.brain.f[AiBrain::kPriorityTarget] = sh.packed + 1;
	other.brain.f[AiBrain::kDamageInfo] = sh.packed + 1;
	sys.clear_entity_references(w, ai.handle);
	CHECK(b.f[AiBrain::kTargetSlot] == 0 && w.registry.get(th)->ai_target_refcount == 0);
	CHECK(other.brain.f[AiBrain::kTargetSlot] == sh.packed + 1);
	CHECK(other.brain.f[AiBrain::kPriorityTarget] == 0 && other.brain.f[AiBrain::kDamageInfo] == 0);
	// Re-selecting an unchanged target does not modify a zero reference count.
	w.registry.get(sh)->ai_target_refcount = 0;
	sys.ai_set_target(w, other, sh);
	CHECK(w.registry.get(sh)->ai_target_refcount == 0);
}

// The ground/boat/train brain machine differs from the air machine in three gates
// and the spawn channel word [orig: EntityAI_ProcessGroundStateMachine @0x4583c0
// vs EntityAI_ProcessAirStateMachine @0x4581b0]:
//  - the alert edge pends GROUND_EVADE (18) unless cur == GROUND_PRETTY (22)
//    (@0x458442..0x458448; the air machine pends 10 unless 14 @0x458239..0x45823b);
//  - a client ticks only cur 21/23 (@0x458545..0x45854d; air 13/15);
//  - a client commits pend 16 or 21..23 (@0x458579..0x458586; air 7 or 13..15);
//  - the spawn AIEvent's channel word is bx == 0 (@0x45851a, xor ebx,ebx @0x4583cd;
//    the air machine stores 9 @0x458312).
// Both machines take the alert edge only for a hull whose entity+0x170 occupant is
// present and not a Player (@0x45841A..0x458433; air @0x45820D..0x458226).
static void test_vehicle_class_state_machine_gates() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	w.registry.configure_pool(0, 2);
	w.registry.configure_pool(1, 1);
	Entity hull_seed;
	hull_seed.kind = EntityKind::Item;
	const EntityHandle hull = w.registry.spawn(1, hull_seed);
	Entity crew_seed;
	crew_seed.kind = EntityKind::Organic;
	const EntityHandle crew = w.registry.spawn(0, crew_seed);
	auto sys_heap = std::make_unique<AiSystem>();
	AiSystem &sys = *sys_heap;
	const int idx = sys.attach(hull);
	AiEntity &e = *sys.at(idx);
	e.health = 100;

	// An empty hull only records the alert: no evade pend, prev = alert.
	sys.is_authority = true;
	e.brain.f[AiBrain::kCurState] = e.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;
	e.brain.f[AiBrain::kPrevAlert] = 0;
	e.brain.f[AiBrain::kAlert] = 1;
	sys.process_ground_state_machine(e, w, 2);
	CHECK(e.brain.f[AiBrain::kPrevAlert] == 1);
	CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundFollowWp);
	CHECK(e.brain.f[AiBrain::kPendState] == kAiGroundFollowWp);
	// A Player claimant (Flags 0x100) gates the edge the same way.
	w.registry.get(crew)->flags |= kEntityFlagPlayer;
	w.registry.get(hull)->primary_occupant = crew;
	e.brain.f[AiBrain::kPrevAlert] = 0;
	sys.process_air_state_machine(e, w, 2);
	CHECK(e.brain.f[AiBrain::kPrevAlert] == 1);
	CHECK(e.brain.f[AiBrain::kPendState] == kAiGroundFollowWp);
	w.registry.get(crew)->flags &= ~kEntityFlagPlayer; // an NPC crew from here on

	// Authority alert edge: GROUND_EVADE from a non-22 state, committed at once ...
	e.brain.f[AiBrain::kCurState] = e.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;
	e.brain.f[AiBrain::kPrevAlert] = 0;
	e.brain.f[AiBrain::kAlert] = 1;
	sys.process_ground_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kPrevAlert] == 2);
	CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundEvade);
	// ... and never out of GROUND_PRETTY.
	e.brain.f[AiBrain::kCurState] = e.brain.f[AiBrain::kPendState] = kAiGroundPretty;
	e.brain.f[AiBrain::kPrevAlert] = 0;
	e.brain.f[AiBrain::kAlert] = 1;
	sys.process_ground_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kPrevAlert] == 2);
	CHECK(e.brain.f[AiBrain::kPendState] == kAiGroundPretty);
	// The air machine, given the same edge from GROUND_PRETTY, pends HELO_EVADE.
	e.brain.f[AiBrain::kPrevAlert] = 0;
	e.brain.f[AiBrain::kAlert] = 1;
	sys.is_authority = false; // hold the commit so the pend is observable
	sys.process_air_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kPendState] == kAiGroundPretty); // no alert leg on a client
	sys.is_authority = true;
	e.brain.f[AiBrain::kPrevAlert] = 0;
	e.brain.f[AiBrain::kAlert] = 1;
	e.brain.f[AiBrain::kCurState] = e.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;
	sys.process_air_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kCurState] == kAiHeloEvade);

	// Client tick gate: GROUND_DYING (21) ticks under the vehicle machine — the
	// dying tick queues the destroy event for a stopped hull — and not under the
	// air machine.
	sys.is_authority = false;
	e.brain.f[AiBrain::kPrevAlert] = e.brain.f[AiBrain::kAlert];
	e.brain.f[AiBrain::kCurState] = e.brain.f[AiBrain::kPendState] = 21;
	e.vel_x = 0;
	e.vel_y = 0;
	const int queued_before = sys.events.count();
	sys.process_air_state_machine(e, w, 0);
	CHECK(sys.events.count() == queued_before);
	sys.process_ground_state_machine(e, w, 0);
	CHECK(sys.events.count() == queued_before + 1);

	// Client commit gate: 17 holds, 16 and 22 commit; the air machine holds 16.
	e.brain.f[AiBrain::kCurState] = kAiGroundFormation;
	e.brain.f[AiBrain::kPendState] = kAiGroundCombat;
	sys.process_ground_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundFormation);
	e.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;
	sys.process_ground_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundFollowWp);
	e.brain.f[AiBrain::kPendState] = kAiGroundPretty;
	sys.process_ground_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundPretty);
	e.brain.f[AiBrain::kCurState] = kAiGroundFormation;
	e.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;
	sys.process_air_state_machine(e, w, 0);
	CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundFormation);

	// Spawn event channel word: 0 for the vehicle machine, 9 for the air one.
	sys.is_authority = true;
	e.brain.f[AiBrain::kPrevAlert] = e.brain.f[AiBrain::kAlert];
	e.brain.f[AiBrain::kCurState] = e.brain.f[AiBrain::kPendState] = kAiGroundFollowWp;
	const int n0 = sys.events.count();
	sys.process_ground_state_machine(e, w, 1);
	CHECK(sys.events.count() == n0 + 1);
	CHECK(sys.events.at(n0).f[0] == 1);
	CHECK((sys.events.at(n0).f[1] & 0xffff) == 0);
	CHECK((sys.events.at(n0).f[1] >> 16) == idx);
	sys.process_air_state_machine(e, w, 1);
	CHECK(sys.events.count() == n0 + 2);
	CHECK((sys.events.at(n0 + 1).f[1] & 0xffff) == 9);
}

// A dead PlayerControl hull with vehicle respawns off is removed by the dead tick
// [orig: AI_TickState_VehicleDead @0x467ede -> Server_RemoveEntityAndNotify
//  @0x50A270 -> Entity_Destroy @0x43e810, which zeroes the AI component @0x43e995
//  and nulls entity+100 @0x43e99d]. The registry hands the next pool-1 spawn the
// same slot, so that entity must start brainless, and the freed AI slot is what
// the next attach reuses [orig: Entity_InitVehicleAI's owner scan @0x460204..0x460222].
static void test_dead_vehicle_despawn_frees_brain() {
	auto owned = std::make_unique<World>();
	World &w = *owned;
	w.registry.configure_pool(1, 2);
	w.rules.vehicle_respawns = false;
	w.ai.is_authority = true;
	Entity hull;
	hull.kind = EntityKind::Item;
	hull.item_id = 1291;
	hull.item_attrib = kItemAttribPlayerControl;
	hull.health = 0;
	hull.alive = false;
	hull.position = { 10, 20, 30 };
	const EntityHandle h = w.registry.spawn(1, hull);
	// A cveh class row: its event callback is the vehicle machine.
	VehicleTraits traits;
	traits.brain_class = VehicleBrainClass::Ground;
	w.vehicles.traits.set(hull.item_id, traits);
	const int idx = w.ai.attach(h);
	AiEntity &ai = *w.ai.at(idx);
	ai.brain.f[AiBrain::kCurState] = ai.brain.f[AiBrain::kPendState] = kAiGroundDead;
	ai.brain.f[AiBrain::kPrevAlert] = ai.brain.f[AiBrain::kAlert];
	TickContext ctx{};
	ctx.world = &w;
	ctx.is_authority = true;
	w.update_all_entities(ctx);
	CHECK(w.registry.get(h) == nullptr);
	CHECK(w.out.entity_events.size() == 1);
	CHECK(w.ai.for_handle(h) == nullptr);
	CHECK(w.ai.count() == 1 && w.ai.at(0)->brain.f[AiBrain::kOwner] == 0);
	// The next pool-1 spawn lands in the freed registry slot, brainless; ticking
	// again runs no dead tick on it (no second removal, the row survives).
	Entity device;
	device.kind = EntityKind::Item;
	device.item_id = 100042;
	device.health = 100;
	device.alive = true;
	const EntityHandle h2 = w.registry.spawn(1, device);
	CHECK(h2.packed == h.packed);
	CHECK(w.ai.for_handle(h2) == nullptr);
	w.update_all_entities(ctx);
	CHECK(w.registry.get(h2) != nullptr);
	CHECK(w.out.entity_events.size() == 1);
	// A fresh brain reuses the freed AI slot instead of growing the array.
	CHECK(w.ai.attach(h2) == 0);
	CHECK(w.ai.count() == 1 && w.ai.for_handle(h2) == w.ai.at(0));
	CHECK(w.ai.at(0)->brain.f[AiBrain::kOwner] == 1 && w.ai.at(0)->handle == h2);
}

namespace {
// The flat-field rig the aircraft brain pins share: a 500 hp hull at the origin
// heading east, a live victim wherever the pin needs it, one ammo row.
struct AircraftRig {
	std::unique_ptr<World> owned = std::make_unique<World>();
	World &w = *owned;
	AiSystem &sys = w.ai;
	std::vector<uint16_t> heights = std::vector<uint16_t>(64 * 64, 0);
	std::vector<int> sectors = std::vector<int>(256, 1);
	opennova::terrain::TerrainHeightField terrain;
	EntityHandle hull_h, victim_h;
	AiEntity *ai = nullptr;
	explicit AircraftRig(Vec3 victim_pos) {
		w.registry.configure_pool(0, 8);
		w.registry.configure_pool(1, 8);
		terrain.heightmap = heights.data();
		terrain.dim = 64;
		terrain.layout.sector_grid = sectors.data();
		w.tables.terrain = &terrain;
		w.tables.ammo.entries.resize(2);
		w.tables.ammo.entries[1].valid = true;
		w.tables.ammo.entries[1].velocity = 620;
		w.tables.ammo.entries[1].max_age_ticks = 100;
		Entity hull;
		hull.health = 500;
		hull.alive = true;
		hull.position = { 0, 0, 10 };
		hull.team = 1;
		hull_h = w.registry.spawn(1, hull);
		Entity victim;
		victim.health = 100;
		victim.alive = true;
		victim.position = victim_pos;
		victim.team = 2;
		victim_h = w.registry.spawn(0, victim);
		ai = sys.at(sys.attach(hull_h));
		sys.is_authority = true;
		ai->health = 500;
		ai->pos[2] = 10 << 16;
		ai->team = 1;
		ai->profile.type = 1;
		ai->profile.field220 = 6000;
		ai->profile.patrol_climb = 3000;
		ai->profile.field216 = 20 << 16;
		ai->profile.patrol_altitude = 12 << 16;
		ai->profile.min_agl = 5 << 16;
		ai->profile.min_speed = 100;
		ai->profile.min_chase = 20 << 16;
		ai->profile.max_chase = 60 << 16;
		ai->profile.approach_cap = 1000 << 16;
		ai->profile.radar_fov_bam = INT32_MAX;
		ai->profile.fov_secondary = 0x7f;
		ai->profile.fire_a.ammo_index = 1;
		ai->profile.fire_a.cone_bam = INT32_MAX;
		ai->profile.fire_b = ai->profile.fire_a;
		ai->profile.fire_interval_a = ai->profile.fire_interval_b = 1;
		ai->brain.f[AiBrain::kSpeedA] = 2000;
		ai->brain.f[AiBrain::kSpeedB] = 1000;
		ai->brain.f[AiBrain::kFallback] = 7;
	}
};
} // namespace

// The processed-tick fire arc [orig: AI_TickState_AircraftCombat @0x471710]:
// the limit is the profile's secondary FOV byte read SIGNED (movsx @0x471736),
// OR 1 at the head, OR 2 at the gate, sar 1, compared UNSIGNED (ja @0x472481)
// against the folded bearing delta. Out of arc jumps straight to the epilogue
// (@0x472df5) without touching the bone byte or last_weapon; a byte >= 0x80
// yields a negative limit that admits every bearing AT THE GATE. The gate is not
// the only cone: the primary/secondary solve behind it validates the target in
// the hull frame (Entity_ValidateWeaponTarget @0x53a400 -> the arc word
// max(|yaw|,|pitch|) + 5*min>>4 @0x53a522..0x53a548, compared UNSIGNED against
// the radar/heat FOV words @0x53a55e/@0x53a582), then folds the solved relative
// yaw the same way and compares it (ja @0x456d4c) against
// (block+8 | 0x2000000) >> 25 (sar @0x456d47) -- at most 63/256 of a turn for
// any non-negative cone. A target dead astern (delta 128, |yaw| = 0x80000000)
// therefore never solves however wide the gate is, and an in-arc tick with no
// block solved takes LABEL_209 (@0x4729ed..0x4729f4): bone byte and last_weapon
// cleared. (The earlier form of this test expected the primary to fire astern
// under the negative gate limit; that premise ignored the solve's own cone.)
static void test_aircraft_fire_arc_gate() {
	AircraftRig r({ -100, 0, 10 }); // dead astern: folded delta 128
	AiEntity &ai = *r.ai;
	auto &b = ai.brain;
	b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState] = 8;
	AiThinkCtx ctx{ &r.sys, &ai, &r.w, nullptr };
	r.sys.row(8).enter(ctx);
	r.sys.ai_set_target(r.w, ai, r.victim_h);
	b.f[AiBrain::kAmmoA] = 4;
	b.f[AiBrain::kAmmoB] = 0;
	b.f[AiBrain::kAccuracy] = 4;
	ai.profile.flags100 = 0;
	// (0x7f | 3) >> 1 = 63 < 128: out of arc, state untouched, no shot.
	b.bytes()[AiBrain::kBoneFlagByte] = 0x40;
	b.f[AiBrain::kLastWeapon] = 2;
	b.f[AiBrain::kTickAccum] = 15;
	r.sys.row(8).tick(ctx);
	CHECK(r.w.out.rounds.count == 0);
	CHECK(b.bytes()[AiBrain::kBoneFlagByte] == 0x40);
	CHECK(b.f[AiBrain::kLastWeapon] == 2);
	// (int8(0x80) | 3) >> 1 = -63, unsigned: the gate admits every bearing, but
	// the astern target fails the solve (the 0x7fffffff radar FOV word cannot
	// admit |yaw| = 0x80000000, and no non-negative cone admits a fold of 128):
	// in arc, nothing solved -> LABEL_209 clears the bone byte and last_weapon.
	ai.profile.fov_secondary = 0x80;
	b.f[AiBrain::kTickAccum] = 15;
	r.sys.row(8).tick(ctx);
	CHECK(r.w.out.rounds.count == 0);
	CHECK(b.bytes()[AiBrain::kBoneFlagByte] == 0);
	CHECK(b.f[AiBrain::kLastWeapon] == 0);
	// The same target 45 deg off the bow (folded delta 32) sits inside the
	// solve's 63 cone, so the gate alone decides: (0x10 | 3) >> 1 = 9 < 32 is
	// out of arc (state untouched again) ...
	r.w.registry.get(r.victim_h)->position = { 100, 100, 10 };
	b.bytes()[AiBrain::kBoneFlagByte] = 0x40;
	b.f[AiBrain::kLastWeapon] = 2;
	ai.profile.fov_secondary = 0x10;
	b.f[AiBrain::kTickAccum] = 15;
	r.sys.row(8).tick(ctx);
	CHECK(r.w.out.rounds.count == 0);
	CHECK(b.bytes()[AiBrain::kBoneFlagByte] == 0x40);
	CHECK(b.f[AiBrain::kLastWeapon] == 2);
	// ... and the negative limit admits it: the primary fires and becomes the
	// weapon on record.
	ai.profile.fov_secondary = 0x80;
	b.f[AiBrain::kTickAccum] = 15;
	r.sys.row(8).tick(ctx);
	CHECK(r.w.out.rounds.count == 1);
	CHECK(b.f[AiBrain::kLastWeapon] == 1);
	CHECK((b.bytes()[AiBrain::kBoneFlagByte] & 0x40) != 0);
}

// The two 0x1000x combat movers. IDB names are swapped-looking: AI_CalcGroundVehicleTarget
// @0x4613A0 is the HELICOPTER mover (0x10000), AI_CalcHelicopterTarget @0x461870 the
// PLANE mover (0x10005).
static void test_aircraft_combat_mover_pins() {
	// Helicopter, target 40 u dead astern at 15 u: the >= min_chase arm with the
	// bearing delta > 0x40 skips the fire-point check (combat timer untouched)
	// but still writes [127] = [45] << 14 (LABEL_35 @0x4616c7); the work Z
	// ([51] + target Z = 15 u) exceeds the 12.5 u helicopter ceiling
	// (819200 @0x46172c) and re-targets to ground + [51] + 50 u (@0x46173b);
	// the out-speed is [49] >> 2 = 500, inside [min_speed, [49]].
	{
		AircraftRig r({ -40, 0, 15 });
		AiEntity &ai = *r.ai;
		auto &b = ai.brain;
		ai.aircraft_controller = 0x10000;
		r.sys.ai_set_target(r.w, ai, r.victim_h);
		b.f[45] = 1;
		b.f[AiBrain::kStep] = 1;
		b.f[AiBrain::kCombatTimer] = 100;
		b.f[AiBrain::kNoTargetIdle] = 0;
		b.f[AiBrain::kTargetRef] = 0;
		CHECK(r.sys.aircraft_movement(ai, r.w) == 0);
		CHECK(b.f[AiBrain::kTargetRef] == 16384);
		CHECK(b.f[AiBrain::kCombatTimer] == 100);
		CHECK(b.f[AiBrain::kWorkPosZ] == 3276800);
		CHECK(b.f[AiBrain::kOutSpeed] == 500);
		CHECK(b.f[AiBrain::kWorkPosX] == 0 && b.f[AiBrain::kWorkPosY] == 0);
		CHECK(b.f[138] == 6000);
		// The plane keeps a 15 u work Z: its ceiling is 50 u (3276800 @0x461b5c),
		// and the plane mover never writes [127] (no store in @0x461870..0x461c1a).
		ai.aircraft_controller = 0x10005;
		b.f[AiBrain::kTargetRef] = 777;
		b.f[AiBrain::kCombatTimer] = 100;
		CHECK(r.sys.aircraft_movement(ai, r.w) == 0);
		CHECK(b.f[AiBrain::kWorkPosZ] == 15 << 16);
		CHECK(b.f[AiBrain::kTargetRef] == 777);
	}
	// Plane with a target while no_target_idle is set: the retreat write does not
	// return early — the common tail still zeroes work X/Y and clamps the speed
	// into [min_speed, [49]] (@0x4619a4..0x461a07 falling through to
	// @0x461b3f..0x461bb3). The helicopter returns before the tail (@0x4615a1).
	{
		AircraftRig r({ 40, 0, 10 });
		AiEntity &ai = *r.ai;
		auto &b = ai.brain;
		r.sys.ai_set_target(r.w, ai, r.victim_h);
		b.f[AiBrain::kNoTargetIdle] = 1;
		b.f[AiBrain::kStep] = 1;
		ai.aircraft_controller = 0x10005;
		b.f[AiBrain::kWorkPosX] = b.f[AiBrain::kWorkPosY] = 12345;
		b.f[AiBrain::kOutSpeed] = 0;
		CHECK(r.sys.aircraft_movement(ai, r.w) == 0);
		CHECK(b.f[AiBrain::kWorkPosX] == 0 && b.f[AiBrain::kWorkPosY] == 0);
		CHECK(b.f[AiBrain::kOutSpeed] == 2000);
		CHECK(b.f[AiBrain::kWorkPosZ] == 20 << 16);
		CHECK(b.f[138] == 6000);
		ai.aircraft_controller = 0x10000;
		b.f[AiBrain::kWorkPosX] = b.f[AiBrain::kWorkPosY] = 12345;
		CHECK(r.sys.aircraft_movement(ai, r.w) == 0);
		CHECK(b.f[AiBrain::kWorkPosX] == 12345 && b.f[AiBrain::kWorkPosY] == 12345);
	}
}

// The pool-1 visit's think countdown (entity+684, Entity::spawn_phase): the
// class event callback runs only while the pre-decrement word is <= 0, the
// machine re-arms it to brain[7] after the update and zeroes it on a committed
// transition, and every visit subtracts one at its tail — a brain thinks once
// every brain[7] visits, on the visit after any transition, on clients too.
// [orig: Entity_UpdatePool1Slot @0x4B8DD0 gate @0x4B8E1B / decrement @0x4B8EA0;
//  EntityAI_ProcessGroundStateMachine @0x458568 / @0x4585B4;
//  EntityAI_ProcessAirStateMachine @0x458363 / @0x4583B0]
void test_vehicle_brain_think_countdown() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 77;
    seed.health = 100;
    const EntityHandle h = w.registry.spawn(1, seed);
    // A cveh class row: its event callback is the vehicle machine.
    VehicleTraits traits;
    traits.brain_class = VehicleBrainClass::Ground;
    w.vehicles.traits.set(seed.item_id, traits);
    AiSystem &sys = w.ai;
    sys.is_authority = true;
    // The think visit also refreshes the entity's own blink/indoors state
    // (Entity_BuildProximityList @0x4B3DC0 = refresh_blink, the call @0x4B8E25);
    // a silent visit leaves it alone.
    CollisionWorld cw;
    sys.collision = &cw;
    AiEntity &e = *sys.at(sys.attach(h));
    e.health = 100;
    e.brain.f[AiBrain::kCurState] = kAiGroundPretty;  // row 22: a live tick returns at once
    e.brain.f[AiBrain::kPendState] = kAiGroundPretty;
    e.brain.f[AiBrain::kStep] = 16;                   // the class init's step
    Entity &ent = *w.registry.get(h);
    ent.spawn_phase = 3;                              // the class init's 0..15 stagger
    ent.flags |= kEntityFlagIndoors;
    TickContext ctx{};
    ctx.world = &w;
    ctx.is_authority = true;
    const int32_t tick0 = e.brain.f[AiBrain::kTick];

    // Three visits with a positive countdown: no think, the word counts 3 -> 0.
    for (int i = 0; i < 3; ++i) w.update_all_entities(ctx);
    CHECK(e.brain.f[AiBrain::kTick] == tick0);
    CHECK(ent.spawn_phase == 0);
    CHECK((ent.flags & kEntityFlagIndoors) != 0); // no blink refresh on a silent visit
    // The visit that reads 0 thinks, re-arms to brain[7] and then counts down.
    w.update_all_entities(ctx);
    CHECK(e.brain.f[AiBrain::kTick] == tick0 + 1);
    CHECK(ent.spawn_phase == 15);
    CHECK((ent.flags & kEntityFlagIndoors) == 0); // the think visit's refresh_blink
    // Fifteen more visits stay silent (15 .. 1 -> 0); the sixteenth thinks:
    // the period is brain[7] visits.
    for (int i = 0; i < 15; ++i) w.update_all_entities(ctx);
    CHECK(e.brain.f[AiBrain::kTick] == tick0 + 1);
    CHECK(ent.spawn_phase == 0);
    w.update_all_entities(ctx);
    CHECK(e.brain.f[AiBrain::kTick] == tick0 + 2);
    CHECK(ent.spawn_phase == 15);

    // A committed transition zeroes the word, so the next visit thinks at once
    // (the enter's own step, 16 for row 19, then re-arms it).
    e.brain.f[AiBrain::kPendState] = kAiGroundFormation;
    sys.apply_transition(e, w);
    CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundFormation);
    CHECK(ent.spawn_phase == 0);
    w.update_all_entities(ctx);
    CHECK(e.brain.f[AiBrain::kTick] == tick0 + 3);
    CHECK(e.brain.f[AiBrain::kStep] == 16);
    CHECK(ent.spawn_phase == 15);

    // A client re-arms the word after its (gated) update just the same.
    sys.is_authority = false;
    ctx.is_authority = false;
    ent.spawn_phase = 0;
    w.update_all_entities(ctx);
    CHECK(e.brain.f[AiBrain::kTick] == tick0 + 4);
    CHECK(ent.spawn_phase == 15);
    sys.is_authority = true;
    ctx.is_authority = true;

    sys.collision = nullptr;

    // A brain on a pool-0 row never thinks: the pool-0 walk runs only each
    // row's +0x1C4 update, never its +0x1C8 class callback, so neither the
    // think nor the countdown runs for it.
    // [orig: Entity_UpdateAllEntities @0x4C2460..0x4C2474 (the pool-0 walk's
    //  only call is `call eax` on +0x1C4); the +0x1C8 calls are the pool-1
    //  visit's @0x4B8E3C and the pool-2/3 walks' @0x4C22B3 / @0x4C2378]
    Entity organic;
    organic.kind = EntityKind::Organic;
    organic.health = 100;
    const EntityHandle oh = w.registry.spawn(0, organic);
    AiEntity &o = *sys.at(sys.attach(oh));
    o.health = 100;
    o.brain.f[AiBrain::kCurState] = kAiGroundPretty;
    o.brain.f[AiBrain::kPendState] = kAiGroundPretty;
    o.brain.f[AiBrain::kStep] = 16;
    const int32_t otick0 = o.brain.f[AiBrain::kTick];
    for (int i = 0; i < 3; ++i) w.update_all_entities(ctx);
    CHECK(o.brain.f[AiBrain::kTick] == otick0);
    CHECK(w.registry.get(oh)->spawn_phase == 0);

    // A pool-1 brain whose class row names no brain machine (no traits row at
    // all, or an ai_function other than CHel/cveh/cbot/cpln/ctrn) thinks
    // nothing on its think visits: its row's event callback is another
    // class's. [orig: g_EntityClassEventCallbackTable @0x813000 -- only the
    //  CHel @0x8132a0 / cveh @0x813378 / cbot @0x813390 / cpln @0x8133a8 /
    //  ctrn @0x8133c0 rows run a state machine; Entity_UpdatePool1Slot `call
    //  eax` @0x4B8E3C]
    Entity plain;
    plain.kind = EntityKind::Item;
    plain.item_id = 78; // no traits row
    plain.health = 100;
    const EntityHandle ph = w.registry.spawn(1, plain);
    const int pidx = sys.attach(ph);
    sys.at(pidx)->health = 100;
    sys.at(pidx)->brain.f[AiBrain::kCurState] = kAiGroundPretty;
    sys.at(pidx)->brain.f[AiBrain::kPendState] = kAiGroundPretty;
    sys.at(pidx)->brain.f[AiBrain::kStep] = 16;
    const int32_t ptick0 = sys.at(pidx)->brain.f[AiBrain::kTick];
    for (int i = 0; i < 3; ++i) w.update_all_entities(ctx);
    CHECK(sys.at(pidx)->brain.f[AiBrain::kTick] == ptick0);
    CHECK(w.registry.get(ph)->spawn_phase == -3); // the visit still steps the clock
}

int main() {
    // ---- struct layout (byte-exact strides) ----
    CHECK(sizeof(AiBrain) == 812);
    CHECK(sizeof(AiSlot) == 172);

    // ---- the PLAYPARTANIM sweep seams (shared with the preview binding) ----
    // Rate [orig: @0x43B1A9..0x43B1F9]: zero seconds -> the x87
    // integer-indefinite INT_MIN (min-1 does NOT fire); a normal time
    // truncates toward zero; a tiny nonzero result promotes to 1.
    CHECK(part_anim_rate_from_seconds(0.0) ==
          static_cast<int32_t>(0x80000000));
    CHECK(part_anim_rate_from_seconds(1.0) ==
          static_cast<int32_t>((0.016 / 1.0) * 65536.0));
    CHECK(part_anim_rate_from_seconds(1.0e9) == 1);
    // Step [orig: @0x456740..0x4567A9]: landing EXACTLY on 0x10000 stays
    // active; only strict overshoot clamps and finishes; direction 0 freezes;
    // reverse clamps at a strictly negative result.
    {
        int32_t phase = 0x10000 - 4;
        CHECK(!part_anim_step(phase, 1, 4) && phase == 0x10000);
        CHECK(part_anim_step(phase, 1, 4) && phase == 0x10000);
        CHECK(!part_anim_step(phase, 0, 4) && phase == 0x10000);
        phase = 4;
        CHECK(!part_anim_step(phase, -1, 4) && phase == 0);
        CHECK(part_anim_step(phase, -1, 4) && phase == 0);
    }

    // ---- state name table (Entity_LookupAIStateName) ----
    CHECK(streq(ai_state_name(kAiGroundFollowWp), "GROUND_FOLLOWWP"));
    CHECK(streq(ai_state_name(kAiGroundDead), "GROUND_DEAD"));
    CHECK(streq(ai_state_name(kAiHeloCombat), "HELO_COMBAT"));
    CHECK(streq(ai_state_name(13), "?")); // transitional gap
    CHECK(streq(ai_state_name(21), "?"));

    // ---- AI_BeginUpdate per-entity movement phase ----
    {
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kStep] = 64;
        e.brain.f[AiBrain::kSpeedA] = 7;
        e.heading = 123;

        // Before phase expiry: accumulates and proceeds; working fields copied.
        e.aircraft_phase = 0;
        CHECK(sys.begin_update(e) == true);
        CHECK(e.aircraft_phase == 64);          // += step
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 7);  // [128] = [49]
        CHECK(e.brain.f[132] == 123);               // = heading

        // Expired phase, profile flag clear -> forced pending state 8.
        e.aircraft_phase = 500;
        e.brain.f[AiBrain::kPendState] = 0;
        e.profile.flags100 = 0;
        CHECK(sys.begin_update(e) == false);
        CHECK(e.aircraft_phase == 500);         // unchanged
        CHECK(e.brain.f[AiBrain::kPendState] == 8);

        // Expired phase, profile flag set -> forced pending = fallback.
        e.aircraft_phase = 500;
        e.profile.flags100 = 2;
        e.brain.f[AiBrain::kFallback] = 17;
        CHECK(sys.begin_update(e) == false);
        CHECK(e.brain.f[AiBrain::kPendState] == 17);
    }

    // ---- handle lookup index ----
    {
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        EntityHandle h0 = EntityHandle::make(0, 4);
        EntityHandle h1 = EntityHandle::make(2, 9);
        EntityHandle h2 = EntityHandle::make(3, 7);
        CHECK(sys.for_handle(h0) == nullptr);
        int i0 = sys.attach(h0);
        int i1 = sys.attach(h1);
        CHECK(sys.for_handle(h0) == sys.at(i0));
        CHECK(sys.for_handle(h1) == sys.at(i1));
        CHECK(sys.for_handle(EntityHandle{}) == nullptr);

        sys.capture_spawn_baseline();
        sys.attach(h2);
        CHECK(sys.for_handle(h2) != nullptr);
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        sys.on_load(w);
        CHECK(sys.for_handle(h0) == sys.at(i0));
        CHECK(sys.for_handle(h1) == sys.at(i1));
        CHECK(sys.for_handle(h2) == nullptr);
    }

    // ---- state-machine dispatcher transition (authority) ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // 16
        e.brain.f[AiBrain::kPendState] = kAiGroundFormation; // 19 (enter = full_reset_to_idle)
        e.brain.f[AiBrain::kStep] = 999;
        int32_t tick_before = e.brain.f[AiBrain::kTick];

        sys.process_air_state_machine(e, w, 0);

        CHECK(e.brain.f[AiBrain::kTick] == tick_before + 1); // ++ each update
        CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundFormation); // committed
        CHECK(e.brain.f[AiBrain::kStep] == 16); // enter[19]=AI_FullResetToIdle set step 16
    }

    // ---- reset-to-patrol enter handler (byte-exact: step=64, alert cleared) ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kAlert] = 5;
        e.brain.f[AiBrain::kPrevAlert] = 5;
        e.brain.f[AiBrain::kCurState] = 0;
        e.brain.f[AiBrain::kPendState] = kAiHeloPretty; // 14, enter = AI_ResetToPatrol
        sys.apply_transition(e, w);
        CHECK(e.brain.f[AiBrain::kCurState] == 14);
        CHECK(e.brain.f[AiBrain::kStep] == 64);
        CHECK(e.brain.f[AiBrain::kAlert] == 0);
        CHECK(e.brain.f[AiBrain::kPrevAlert] == 0);
    }

    // ---- AIEvent ring: timer decrement, expiry dispatch, transition, compaction ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = 13; // event handler = AI_HandleAlertEvent
        e.brain.f[AiBrain::kPendState] = 13;

        // Event that fires after 2 frames (timer 0.03 > 0.016 once, expires twice).
        AiEventEntry ev{};
        ev.f[0] = 4;                 // type 4 (alert)
        ev.f[1] = 9 | (idx << 16);   // channel 9 | entity index
        ev.set_timer(0.03f);
        sys.events.queue(ev);
        CHECK(sys.events.count() == 1);

        sys.events.process_timed(sys, w);   // frame 1: 0.03 -> 0.014, not expired
        CHECK(sys.events.count() == 1);
        CHECK(e.brain.f[AiBrain::kCurState] == 13); // not fired yet

        sys.events.process_timed(sys, w);   // frame 2: 0.014 -> <0, expires
        CHECK(sys.events.count() == 0);     // compacted out
        // AI_HandleAlertEvent set pending=15, then the ring applied the transition.
        CHECK(e.brain.f[AiBrain::kCurState] == 15);
    }

    // ---- ChangeAI alert brain leg: AIEvent {6, level} through the command
    // dispatch [orig: Entity_ApplyCommand queue @0x43ac59/@0x43acc4/@0x43ad34
    // -> AI_HandleCommand case 6 @0x4657a6..0x465816] ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = true;
        const auto alert_event = [](int idx, int32_t level) {
            AiEventEntry ev{};
            ev.f[0] = 6;
            ev.f[1] = 9 | (idx << 16);
            ev.set_timer(0.0f);
            ev.f[3] = level;
            return ev;
        };
        // Type-2 (ground) brain at FOLLOWWP: a change to red pushes pend 18
        // and stores 2 in both alert words.
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.profile.type = 2;
        e.profile.flags96 = 0;
        e.brain.f[AiBrain::kCurState] = 16;
        e.brain.f[AiBrain::kPendState] = 16;
        sys.events.queue(alert_event(idx, 2));
        sys.events.process_timed(sys, w);
        CHECK(e.brain.f[AiBrain::kAlert] == 2);
        CHECK(e.brain.f[AiBrain::kPrevAlert] == 2);
        CHECK(e.brain.f[AiBrain::kCurState] == 18); // GROUND_EVADE push
        // The forced-2 quirk: a GREEN command onto a differing brain still
        // stores 2 in both words [orig: mov eax, ebx @0x4657cd runs
        // unconditionally inside the changed branch].
        e.brain.f[AiBrain::kCurState] = 16;
        e.brain.f[AiBrain::kPendState] = 16;
        sys.events.queue(alert_event(idx, 0));
        sys.events.process_timed(sys, w);
        CHECK(e.brain.f[AiBrain::kAlert] == 2);
        CHECK(e.brain.f[AiBrain::kPrevAlert] == 2);
        // Unchanged level: the same-value rewrite is a no-op (no pend push,
        // the clamped param stores as-is).
        e.brain.f[AiBrain::kCurState] = 16;
        e.brain.f[AiBrain::kPendState] = 16;
        sys.events.queue(alert_event(idx, 2));
        sys.events.process_timed(sys, w);
        CHECK(e.brain.f[AiBrain::kCurState] == 16);
        CHECK(e.brain.f[AiBrain::kAlert] == 2);
        // The flags96 bit-1 gate suppresses the state push but not the store.
        int idx2 = sys.attach(EntityHandle::make(0, 1));
        AiEntity &cap = *sys.at(idx2);
        cap.profile.type = 2;
        cap.profile.flags96 = 2;
        cap.brain.f[AiBrain::kCurState] = 16;
        cap.brain.f[AiBrain::kPendState] = 16;
        sys.events.queue(alert_event(idx2, 1));
        sys.events.process_timed(sys, w);
        CHECK(cap.brain.f[AiBrain::kCurState] == 16);
        CHECK(cap.brain.f[AiBrain::kAlert] == 2); // forced-2 on change
        // The state-22 exclusion: a type-2 brain already at GROUND_PRETTY
        // keeps its state. (State 22's own event column is unported, so the
        // exclusion is pinned through the dispatch-capable state 17 by
        // pre-seeding pend.)
        int idx3 = sys.attach(EntityHandle::make(0, 2));
        AiEntity &excl = *sys.at(idx3);
        excl.profile.type = 2;
        excl.profile.flags96 = 0;
        excl.brain.f[AiBrain::kCurState] = 17;
        excl.brain.f[AiBrain::kPendState] = 17;
        sys.events.queue(alert_event(idx3, 1));
        sys.events.process_timed(sys, w);
        CHECK(excl.brain.f[AiBrain::kCurState] == 18); // 17 != 22 -> push runs
        // An out-of-range level clamps to 0..2 before the compare.
        int idx4 = sys.attach(EntityHandle::make(0, 3));
        AiEntity &cl = *sys.at(idx4);
        cl.profile.type = 2;
        cl.brain.f[AiBrain::kCurState] = 16;
        cl.brain.f[AiBrain::kPendState] = 16;
        cl.brain.f[AiBrain::kAlert] = 2;
        cl.brain.f[AiBrain::kPrevAlert] = 2;
        sys.events.queue(alert_event(idx4, 9)); // clamps to 2 = unchanged
        sys.events.process_timed(sys, w);
        CHECK(cl.brain.f[AiBrain::kCurState] == 16);
        CHECK(cl.brain.f[AiBrain::kAlert] == 2);
    }

    // ---- a HELO_LAND tick over an absent entity ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiHeloLand; // 6
        e.brain.f[AiBrain::kPendState] = kAiHeloLand;
        sys.is_authority = true;
        sys.process_air_state_machine(e, w, 0);
		CHECK(e.brain.f[AiBrain::kCurState] == kAiHeloLand); // landing safely handles an absent entity
	}

	// ---- body-anim slot names; the class machines select none ----
    {
        // body_anim_adm_key maps slots to the AI .adm key namespace.
        CHECK(streq(body_anim_adm_key(kBodyAnimIdle), "anim_idle"));
        CHECK(streq(body_anim_adm_key(kBodyAnimWalkForward), "anim_walk_forward"));
        CHECK(streq(body_anim_adm_key(kBodyAnimRunForward), "anim_run_forward"));
        CHECK(streq(body_anim_adm_key(-1), ""));

        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 8);
        Entity seed;
        seed.alive = true;
        seed.health = 100;
        EntityHandle h = w.registry.spawn(0, seed);
        CHECK(h != EntityHandle{});

        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = true;
        int idx = sys.attach(h);
        AiEntity &e = *sys.at(idx);
        // State 20's row is all no-ops. Moving or stopped, alert or not, the
        // dispatcher's update leg writes no body-anim slot [orig:
        // EntityAI_ProcessAirStateMachine @0x4581B0, the event-0 leg ends in
        // the +0x2AC re-arm @0x458363 and the commit].
        e.brain.f[AiBrain::kCurState] = kAiGroundReturnToBase;  // 20
        e.brain.f[AiBrain::kPendState] = kAiGroundReturnToBase;
        for (int32_t speed : {10, 0}) {
            for (int32_t alert : {0, 2}) {
                e.brain.f[AiBrain::kOutSpeed] = speed;
                e.brain.f[AiBrain::kAlert] = alert;
                sys.process_air_state_machine(e, w, 0);
                CHECK(w.registry.get(h)->body_anim_slot == -1);
            }
        }
    }

    // ======================= P1: GROUND_FOLLOWWP movement =======================

    // ---- waypoint solver: type 3 (literal coord), byte-exact dist + bearing ----
    {
        NavNodeTable nav; // unused for type 3
        AiBrain b;
        // Target straight along +dx (pos+4): atan2(dz=0, dx=100) = 0 -> bearing 0.
        b.f[AiBrain::kWpType] = 3;
        b.f[AiBrain::kWpCoordX] = 100;
        b.f[AiBrain::kWpCoordY] = 0;
        b.f[AiBrain::kWpCoordZ] = 0;
        b.f[AiBrain::kWpCoordSrc] = 777;
        int32_t pos[3] = {0, 0, 0};
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpDistance] == 100);  // base=|dx|=100, cross term 0
        CHECK(b.f[AiBrain::kWpBearing] == 0);     // atan2(0,+) == 0  (verifies arg order)
        CHECK(b.f[AiBrain::kWpExtra] == 0);
        CHECK(b.f[AiBrain::kWpNodeVal] == 777);   // = kWpCoordSrc

        // Target along +dz (pos+8): atan2(dz=100, dx=0) = +pi/2 -> large positive bearing.
        b.f[AiBrain::kWpCoordX] = 0;
        b.f[AiBrain::kWpCoordY] = 100;
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpDistance] == 100);
        CHECK(b.f[AiBrain::kWpBearing] > 1000000000); // ~2^30 quarter turn
    }

    // ---- waypoint solver: type 1 (nav node) resolves a pool-3 entry ----
    {
        NavNodeTable nav;
        nav.channels.resize(2);              // channel id 1 (0 is the navMeshId==0 sentinel)
        nav.channels[1].count = 2;
        nav.channels[1].entries[0] = 0;      // -> nodes[0]
        nav.channels[1].entries[1] = 1;
        nav.nodes.resize(2);
        nav.nodes[0] = NavEntry{{7, 100, 0, 0, 9}}; // {payload0, x, y, z, payload4}
        AiBrain b;
        b.f[AiBrain::kWpType] = 1;
        b.f[AiBrain::kWpChannel] = 1;
        b.f[AiBrain::kWpNode] = 0;
        int32_t pos[3] = {0, 0, 0};
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpResolved] == 0);    // pool-3 index stored (deviation: idx not ptr)
        CHECK(b.f[AiBrain::kWpDistance] == 100);
        CHECK(b.f[AiBrain::kWpBearing] == 0);     // atan2(0,100)=0
        CHECK(b.f[AiBrain::kWpNodeVal] == 7);     // navEntry[0]
        CHECK(b.f[AiBrain::kWpExtra] == 9);       // navEntry[4]
    }

    // ---- waypoint solver: -1 sentinels + type-2 no-op ----
    {
        NavNodeTable nav;
        nav.channels.resize(2);
        nav.channels[1].count = 0;          // present but empty
        int32_t pos[3] = {0, 0, 0};
        AiBrain b;
        b.f[AiBrain::kWpType] = 1;
        b.f[AiBrain::kWpChannel] = 0;       // navMeshId == 0 -> -1
        CHECK(ai_waypoint_update_target(b, pos, nav) == -1);
        b.f[AiBrain::kWpChannel] = 1;       // channel present but count==0 -> -1
        CHECK(ai_waypoint_update_target(b, pos, nav) == -1);
        b.f[AiBrain::kWpType] = 5;          // unknown type -> -1
        CHECK(ai_waypoint_update_target(b, pos, nav) == -1);
        b.f[AiBrain::kWpType] = 2;          // type 2 -> 0 (no-op)
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
    }

    // ---- path follower: arrival advances the node + records relmat + outputs ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.nav.channels.resize(2);
        sys.nav.channels[1].count = 3;
        sys.nav.channels[1].loopflag = 0;
        sys.nav.channels[1].entries[0] = 10;
        sys.nav.channels[1].entries[1] = 11;
        sys.nav.channels[1].entries[2] = 12;
        sys.nav.nodes.resize(13);
        sys.nav.nodes[10] = NavEntry{{1000, 500, 600, 0, 0}}; // payload0=1000 (arrival radius)
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.relmat_id = 4;
        e.net_id = 9;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // 16 -> moveSpeed = kSpeedB
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 1;
        e.brain.f[AiBrain::kWpNode] = 0;
        e.brain.f[AiBrain::kSpeedB] = 20;
        e.brain.f[AiBrain::kStep] = 64;

        sys.update_waypoint_movement(e, w);

        // dist=600 (|dz|=600 base) < nodeVal=1000 -> advance.
        CHECK(e.brain.f[AiBrain::kWpNode] == 1);
        CHECK(e.brain.f[AiBrain::kStoredKeyTime] == 1000);
        CHECK(sys.relmat_calls.size() == 2);
        CHECK(sys.relmat_calls[0].which == 1);                    // SetBitB first
        CHECK(sys.relmat_calls[0].key == 4);                      // group / SetBitB key
        CHECK(sys.relmat_calls[1].which == 0);                    // SetBitA second
        CHECK(sys.relmat_calls[1].key == 9);                      // SSN / SetBitA key
        CHECK(sys.relmat_calls[0].channel == 1 && sys.relmat_calls[0].node == 0);
        CHECK(w.script.relations.group_visited(4, 1, 0));
        CHECK(w.script.relations.single_visited(9, 1, 0));
        // working transform from the resolved node; out-speed halved (timeDelta<step*speed).
        CHECK(e.brain.f[AiBrain::kWorkPosX] == 500);
        CHECK(e.brain.f[AiBrain::kWorkPosY] == 600);
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 10);               // 20 >> 1
    }

    // ---- path follower: loop-wrap vs one-shot terminate at path end ----
    {
        for (int loopflag = 0; loopflag <= 1; ++loopflag) {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            sys.nav.channels.resize(2);
            sys.nav.channels[1].count = 3;
            sys.nav.channels[1].loopflag = loopflag;
            sys.nav.channels[1].entries[2] = 12;
            sys.nav.nodes.resize(13);
            sys.nav.nodes[12] = NavEntry{{2000, 700, 800, 0, 0}};
            int idx = sys.attach(EntityHandle::make(0, 0));
            AiEntity &e = *sys.at(idx);
            e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
            e.brain.f[AiBrain::kWpType] = 1;
            e.brain.f[AiBrain::kWpChannel] = 1;
            e.brain.f[AiBrain::kWpNode] = 2; // last node -> ++ hits count(3)
            e.brain.f[AiBrain::kSpeedB] = 20;
            e.brain.f[AiBrain::kStep] = 64;

            sys.update_waypoint_movement(e, w);

            if (loopflag & 1) {
                // one-shot: terminate, clear waypoint type, clamp node, freeze.
                CHECK(e.brain.f[AiBrain::kWpType] == 0);
                CHECK(e.brain.f[AiBrain::kWpNode] == 2);   // count-1
                CHECK(e.brain.f[AiBrain::kOutSpeed] == 0);
            } else {
                // loop: wrap to 0, keep moving (out-speed set from the resolved node).
                CHECK(e.brain.f[AiBrain::kWpNode] == 0);
                CHECK(e.brain.f[AiBrain::kWpType] == 1);
                CHECK(e.brain.f[AiBrain::kWorkPosX] == 700);
            }
        }
    }

    // ---- state-16 tick: alive + no target -> walks the path ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.nav.nodes.resize(1);
        sys.nav.nodes[0] = NavEntry{{0, 100, 0, 0, 0}};
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kWpType] = 3;       // literal coord toward (100,0,0)
        e.brain.f[AiBrain::kWpCoordX] = 100;
        e.brain.f[AiBrain::kWpResolved] = 0;   // resolves nodes[0]
        e.brain.f[AiBrain::kSpeedB] = 20;
        e.brain.f[AiBrain::kStep] = 64;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(sys.find_target_calls == 1);          // alive path attempted acquisition
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 10); // mover ran (20 >> 1)
        CHECK(e.brain.f[AiBrain::kWorkPosX] == 100);
    }

    // ---- state-16 tick: death queues crash (3) vs still (4) by speed ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.health = 0;
        e.vel_x = 2000; e.vel_y = 0;  // |v| = 2000 >= 1057 -> crash death (3)
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(sys.events.count() == 1);
        CHECK(sys.events.at(0).type() == 3);
        CHECK(sys.events.at(0).entity_index() == idx);

        e.vel_x = 100; e.vel_y = 0;   // |v| = 100 < 1057 -> still death (4)
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(sys.events.count() == 2);
        CHECK(sys.events.at(1).type() == 4);
    }

    // ---- path follower: state-17 uses 16*speed threshold + kSpeedA; un-halved output ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.nav.channels.resize(2);
        sys.nav.channels[1].count = 3;
        sys.nav.channels[1].entries[0] = 10;
        sys.nav.nodes.resize(11);
        sys.nav.nodes[10] = NavEntry{{1000, 2000, 0, 0, 0}}; // nodeVal=1000, X=2000 -> dist 2000
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundCombat; // 17 -> moveSpeed = kSpeedA
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 1;
        e.brain.f[AiBrain::kWpNode] = 0;
        e.brain.f[AiBrain::kSpeedA] = 20;
        e.brain.f[AiBrain::kSpeedB] = 999; // must NOT be used in state 17
        e.brain.f[AiBrain::kStep] = 64;
        sys.update_waypoint_movement(e, w);
        // dist=2000 >= nodeVal=1000 -> no advance. timeDelta=1000. state17 threshold 16*20=320;
        // 1000 >= 320 -> NOT halved -> speed stays 20 (proves kSpeedA source + 16x threshold).
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 20);
        CHECK(e.brain.f[AiBrain::kWpNode] == 0); // never advanced
    }

    // ---- contrast: state-16 with the SAME timeDelta halves (threshold speed*step=1280) ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.nav.channels.resize(2);
        sys.nav.channels[1].count = 3;
        sys.nav.channels[1].entries[0] = 10;
        sys.nav.nodes.resize(11);
        sys.nav.nodes[10] = NavEntry{{1000, 2000, 0, 0, 0}};
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // 16 -> moveSpeed = kSpeedB
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 1;
        e.brain.f[AiBrain::kSpeedB] = 20;
        e.brain.f[AiBrain::kStep] = 64;
        sys.update_waypoint_movement(e, w);
        // state16 threshold 20*64=1280; timeDelta 1000 < 1280 -> halve -> 10.
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 10);
    }

    // ---- waypoint solver: distance approximation tail (5*cross>>16) + |dy| term ----
    {
        NavNodeTable nav;
        AiBrain b;
        b.f[AiBrain::kWpType] = 3;
        b.f[AiBrain::kWpCoordX] = 20000; // dx
        b.f[AiBrain::kWpCoordY] = 20001; // dz (max axis -> base)
        b.f[AiBrain::kWpCoordZ] = 20000; // dy (enters the cross sum)
        int32_t pos[3] = {0, 0, 0};
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        // base=|dz|=20001; cross=|dx|+|dy|=40000; term=(5*40000)>>16=200000>>16=3; dist=20004.
        CHECK(b.f[AiBrain::kWpDistance] == 20004);
    }

    // ---- path follower: unresolvable waypoint -> freeze at current transform ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.pos[0] = 11; e.pos[1] = 22; e.pos[2] = 33;
        e.heading = 44; e.pitch = 55; e.roll = 66;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 0; // navMeshId 0 -> solver returns -1 -> freeze
        e.brain.f[AiBrain::kSpeedB] = 20;
        sys.update_waypoint_movement(e, w);
        CHECK(e.brain.f[AiBrain::kWorkPosX] == 11);
        CHECK(e.brain.f[AiBrain::kWorkPosY] == 22);
        CHECK(e.brain.f[AiBrain::kWorkPosZ] == 33);
        CHECK(e.brain.f[AiBrain::kWorkHeading] == 44);
        CHECK(e.brain.f[AiBrain::kWorkPitch] == 55);
        CHECK(e.brain.f[AiBrain::kWorkRoll] == 66);
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 0);
    }

    // ---- waypoint solver: bearing quadrant + sign + truncation-toward-zero ----
    {
        NavNodeTable nav;
        AiBrain b;
        int32_t pos[3] = {0, 0, 0};
        b.f[AiBrain::kWpType] = 3;
        b.f[AiBrain::kWpCoordX] = 100; // dx
        b.f[AiBrain::kWpCoordY] = 100; // dz -> atan2(100,100)=+pi/4 ~ 2^29 = 536870912
        b.f[AiBrain::kWpCoordZ] = 0;
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpBearing] > 536000000 && b.f[AiBrain::kWpBearing] < 537000000);
        // negative dz -> -pi/4: pins the sign + chop-toward-zero (same magnitude, negative).
        b.f[AiBrain::kWpCoordY] = -100;
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpBearing] < -536000000 && b.f[AiBrain::kWpBearing] > -537000000);
    }

    // ---- state-16 tick: can-fire + fire timer decrements by step ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.nav.nodes.resize(1);
        sys.nav.nodes[0] = NavEntry{{0, 0, 0, 0, 0}};
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kWpType] = 3;
        e.brain.f[AiBrain::kFireTimer] = 50;
        e.brain.f[AiBrain::kStep] = 64;
        e.profile.flags96 = 0x10; // can-fire
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
		CHECK(e.brain.f[AiBrain::kFireTimer] == 50 - 64); // -= kStep
	}

    // ======================= P2: GROUND combat + targeting =======================

    // ---- PRNG: the rotate-LCG (dword_31BFBB8 / PRNG_Next16) is byte-exact + independent ----
    {
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.prng_a = 1;
        // s = rotl(1+rotl(1,11),4)^1 = rotl(0x801,4)^1 = 0x8010^1 = 0x8011 = 32785.
        CHECK(static_cast<uint32_t>(sys.prng_step_a()) == 0x8011u);
        CHECK(sys.prng_a == 0x8011u);             // state advanced
        CHECK((0x8011u & 0xFFFFu) % 62 == 49);    // the %62 jitter the engagement adds

        // PRNG_Next16 is the same algorithm over a separate stream (dword_31BFBB0).
        auto prng_world_heap = std::make_unique<World>();
        World &prng_world = *prng_world_heap;
        prng_world.prng16_state = 1;
        CHECK(prng_world.next_prng16() == 0x8011u);
        CHECK(sys.prng_a == 0x8011u);             // stepping 16 did NOT touch stream a (independent)
    }

    // ---- ai_score_target: byte-exact FOV/range/stealth/priority scoring ----
    {
        // Centered (angle 0), close (dist 50), fully visible: primary FOV path.
        // primary_fov=0x41 (=65), gate (65|2)>>1=33; angle 0<33 -> angle_score=((65)<<16)/65=0x10000.
        // range=0x640000/50=0x20000; stealth=0x10000; priority=0x10000 -> score 0x20000=131072.
        CHECK(ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 0, 0) == 131072);

        // Outside both FOV gates -> -1 (gate-fail sentinel, distinct from an in-gate score of 0).
        CHECK(ai_score_target(50, 50, 0x41, 0x41, 100, 100, 100, 100, 0, 0) == -1);

        // Beyond range -> -1 even when perfectly centered.
        CHECK(ai_score_target(0, 200, 0x41, 0x41, 100, 100, 100, 100, 0, 0) == -1);

        // Secondary FOV path (fails primary angle, passes secondary): scores > 0, < the centered max.
        int sec = ai_score_target(35, 50, 0x41, 0x51, 100, 100, 100, 100, 0, 0);
        CHECK(sec > 0 && sec < 131072);

        // Stealth: visibility >= 16 zeroes the score (in-gate 0, NOT gate-fail -1); partial scales it.
        CHECK(ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 16, 0) == 0);
        int dim = ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 8, 0);
        CHECK(dim > 0 && dim < 131072);                            // stealth 0xFF00 < 0x10000

        // Priority flag (&0x4000) applies the 6.0x weight (393216/0x10000).
        CHECK(ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 0, 0x4000) == 131072 * 6);
    }

    // ---- acquire_target: best-of, team filter, LOS, priority bypass ----
    {
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.heading = 0;
        e.team = 1;
        e.profile.fov_primary = 0x40;     // -> 0x41
        e.profile.fov_secondary = 0x40;
        e.profile.range_primary = 1000;
        e.profile.range_secondary = 1000;

        // Far enemy (dist large but in range) and near enemy (better range_score); both centered.
        AiCandidate far{};
        far.handle = EntityHandle::make(1, 5);
        far.team = 2; far.pos[0] = 500 << 16; far.health = 100;
        far.range_primary = 1000; far.range_secondary = 1000;
        far.relmat_id = 0x11; far.net_id = 0x111;
        AiCandidate near{};
        near.handle = EntityHandle::make(1, 6);
        near.team = 2; near.pos[0] = 100 << 16; near.health = 100;
        near.range_primary = 1000; near.range_secondary = 1000;
        near.relmat_id = 0x22; near.net_id = 0x222; near.has_brain = true;

        AiTarget out{};
        CHECK(sys.acquire_target_from(e, {far, near}, out) == true);
        CHECK(sys.find_target_calls == 1);
        CHECK(out.net_id == 0x222);          // nearer -> higher range_score -> chosen
        CHECK(out.relmat_id == 0x22);
        CHECK(out.has_brain == true);

        // Same-team candidate is filtered out -> no target.
        AiCandidate same{};
        same.handle = EntityHandle::make(1, 7);
        same.team = 1;          // same team as e
        same.pos[0] = 100 << 16; same.health = 100;
        same.range_primary = 1000; same.range_secondary = 1000;
        AiTarget none{};
        CHECK(sys.acquire_target_from(e, {same}, none) == false);

        // LOS blocked on the only enemy -> no target.
        AiCandidate blocked = near;
        blocked.los_blocked = true;
        CHECK(sys.acquire_target_from(e, {blocked}, none) == false);

        // Priority target (in-gate) bypasses scoring and returns immediately on LOS.
        AiCandidate prio = near;
        prio.is_priority = true; prio.net_id = 0x333; prio.relmat_id = 0x33;
        prio.pos[0] = 900 << 16; // would score worse than a closer one, but priority wins
        AiCandidate closer = near;
        closer.net_id = 0x444; closer.pos[0] = 50 << 16;
        AiTarget pout{};
        CHECK(sys.acquire_target_from(e, {closer, prio}, pout) == true);
        CHECK(pout.net_id == 0x333);         // priority bypass beats the closer non-priority

        // [grill fix: priority bypass @0x467350 is reached only PAST the FOV/range gate]
        // A priority target OUTSIDE the engage range is skipped; an in-range non-priority enemy wins.
        AiCandidate prio_far{};
        prio_far.handle = EntityHandle::make(1, 20);
        prio_far.team = 2; prio_far.pos[0] = 5000 << 16; prio_far.health = 100;
        prio_far.range_primary = 1000; prio_far.range_secondary = 1000;
        prio_far.is_priority = true; prio_far.net_id = 0x888;  // dist 5000 > range 1000 -> gate-fail
        AiCandidate inrange{};
        inrange.handle = EntityHandle::make(1, 21);
        inrange.team = 2; inrange.pos[0] = 100 << 16; inrange.health = 100;
        inrange.range_primary = 1000; inrange.range_secondary = 1000;
        inrange.net_id = 0x999;
        AiTarget gout{};
        CHECK(sys.acquire_target_from(e, {prio_far, inrange}, gout) == true);
        CHECK(gout.net_id == 0x999);         // out-of-gate priority skipped; in-range enemy chosen
    }

    // ---- acquire_target: the class-driven pool walk (D-AI-1, §16.2) ----
    {
        const auto make_world = [](World &w) {
            w.registry.configure_pool(0, 8);
            w.registry.configure_pool(1, 8);
            w.registry.configure_pool(2, 8);
            const auto spawn = [&w](int pool, uint16_t net_id, uint32_t flags = 0) {
                Entity c;
                c.team = 2;
                c.health = 100;
                c.position = Vec3{100.0f, 0.0f, 0.0f};
                c.net_id = net_id;
                c.radar_sig = 1000;
                c.heat_sig = 1000;
                c.engine_flags = flags;
                return w.registry.spawn(pool, c);
            };
            spawn(0, 0x10);                      // organic
            spawn(0, 0x11, kEntityFlagPlayer);   // player-flagged organic
            spawn(1, 0x20);                      // ground vehicle (no brain)
            spawn(2, 0x30);                      // building
        };
        const auto make_scanner = [](AiSystem &sys, EntityHandle h) -> AiEntity & {
            AiEntity &e = *sys.at(sys.attach(h));
            e.team = 1;
            e.heading = 0;
            e.profile.fov_primary = 0x40;
            e.profile.fov_secondary = 0x40;
            e.profile.range_primary = 1000;
            e.profile.range_secondary = 1000;
            return e;
        };

        // All-zero priorities (retail's memset-0 / unresolved-.aip profile) -> no scan.
        // [orig: every case gates on the +80+4*class word @0x466ff3/0x467012/0x46702d/0x467048]
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == false);
        }

        // priority_ground alone: pool 1 scanned (class 1); pool-0 organics and pool-2
        // buildings unseen. [orig: case 1 -> pool 1 only @0x467018-0x467024]
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            e.profile.class_priority[1] = 100;
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == true);
            CHECK(out.net_id == 0x20);
        }

        // priority_organics alone: pool 0, the Player-flagged candidate excluded
        // [orig: case 2 -> pool 0, else-leg `test ebx,100h` reject @0x467165].
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            e.profile.class_priority[2] = 100;
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == true);
            CHECK(out.net_id == 0x10);
        }

        // priority_air alone: pool 1 (unbrained accepted), then pool 0 PLAYERS only
        // [orig: case 0 two-leg walk @0x466ff9-0x467005 + @0x4673ac-0x4673be]. With the
        // vehicle removed, only the player-flagged organic remains visible.
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            e.profile.class_priority[0] = 100;
            // Kill the pool-1 vehicle so the pool-0 player leg decides.
            w.registry.get(EntityHandle::make(1, 0))->health = 0;
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == true);
            CHECK(out.net_id == 0x11);
            // The local cheat word's 0x800 bit excludes the LOCAL player from that leg
            // [orig: `test dword_24C1930,800h` @0x467141].
            w.cached.local_player = EntityHandle::make(0, 1);
            w.rules.ai_rules_skip_local_player = true;
            AiTarget out2{};
            CHECK(sys.acquire_target(w, e, out2) == false);
        }

        // priority_decorations alone: pool 2 buildings become targetable
        // [orig: case 3 -> pool 2 @0x46704e].
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            e.profile.class_priority[3] = 100;
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == true);
            CHECK(out.net_id == 0x30);
        }

        // Zero-signature candidate: the per-candidate caps reject it (an unauthored
        // radarsig/heatsig makes the entity undetectable, exactly like retail)
        // [orig: dist > uint16 +422/+420 @0x46723e/@0x467277].
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            e.profile.class_priority[1] = 100;
            w.registry.get(EntityHandle::make(1, 0))->radar_sig = 0;
            w.registry.get(EntityHandle::make(1, 0))->heat_sig = 0;
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == false);
        }

        // The round-end latch nulls acquisition [orig: g_SpawnSuccessGate @0x24C1928].
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            e.profile.class_priority[1] = 100;
            w.process_round_end(0);
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == false);
        }

        // The brain+148 priority feed: the marked candidate wins via the LOS-only
        // bypass over a better-scoring nearer enemy [orig: brain+148 read @0x467350].
        {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            make_world(w);
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            e.profile.class_priority[1] = 100;
            Entity far_vehicle;
            far_vehicle.team = 2;
            far_vehicle.health = 100;
            far_vehicle.position = Vec3{900.0f, 0.0f, 0.0f};
            far_vehicle.net_id = 0x21;
            far_vehicle.radar_sig = 1000;
            far_vehicle.heat_sig = 1000;
            const EntityHandle prio_h = w.registry.spawn(1, far_vehicle);
            e.brain.f[AiBrain::kPriorityTarget] = static_cast<int32_t>(prio_h.packed) + 1;
            AiTarget out{};
            CHECK(sys.acquire_target(w, e, out) == true);
            CHECK(out.net_id == 0x21); // priority beats the nearer 0x20
        }

        // LOS runs LAST, only for a would-be best: the lazy probe is consulted at
        // most once per improving candidate, never for gate-failed ones
        // [orig: Entity_CheckMutualLineOfSight only @0x467363/@0x46738b].
        {
            auto sys_heap = std::make_unique<AiSystem>();
            AiSystem &sys = *sys_heap;
            AiEntity &e = make_scanner(sys, EntityHandle::make(0, 7));
            AiCandidate a{};
            a.handle = EntityHandle::make(1, 1);
            a.team = 2; a.pos[0] = 500 << 16; a.health = 100;
            a.range_primary = 1000; a.range_secondary = 1000; a.net_id = 0x51;
            AiCandidate b = a;
            b.handle = EntityHandle::make(1, 2);
            b.pos[0] = 100 << 16; b.net_id = 0x52;
            AiCandidate gated = a; // out of range -> gate-fails before any LOS
            gated.handle = EntityHandle::make(1, 3);
            gated.pos[0] = 5000 << 16; gated.net_id = 0x53;
            int los_calls = 0;
            const AiSystem::LosBlockedFn count_fn = [](void *ctx, const AiCandidate &) {
                ++*static_cast<int *>(ctx);
                return false;
            };
            AiTarget out{};
            CHECK(sys.acquire_target_from(e, {a, b, gated}, out, count_fn, &los_calls) == true);
            CHECK(out.net_id == 0x52);
            CHECK(los_calls == 2); // a (first best), b (improves) — never the gated one
        }
    }

    // ---- death event on a non-authority in-session client zeroes health before the death tick ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = false;
        sys.is_in_session = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.health = 100;                                    // still "alive" coming in
        e.vel_x = 2000; e.vel_y = 0;                       // crash speed (>= 1057)
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // tick = h_ground_followwp_tick
        sys.process_air_state_machine(e, w, 4);
        CHECK(e.health == 0);                              // zeroed before the tick [orig @0x45827f]
        bool saw_death = false, saw_20 = false;            // death tick (3) + SM's type-20 event
        for (int i = 0; i < sys.events.count(); ++i) {
            if (sys.events.at(i).type() == 3) saw_death = true;
            if (sys.events.at(i).type() == 20) saw_20 = true;
        }
        CHECK(saw_death);                                  // death PATH ran (not the alive path)
        CHECK(saw_20);
    }

    // ---- engage_target: 8 relation ops in order, target set, fire-delay jitter, pending 17 ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        w.prng16_state = 1;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.relmat_id = 0x1234;
        e.net_id = 0x9999;
        e.profile.field104 = 0;
        AiTarget t{0x55, 0x66, /*has_brain=*/false};

        sys.engage_target(w, e, t);

        CHECK(e.brain.f[AiBrain::kPendState] == 17);   // GROUND_COMBAT
        CHECK(e.brain.f[AiBrain::kCombatTimer] == 0);
        // branch B: always jitter via PRNG_Next16; field104=0 -> delay = 0 + 49.
        CHECK(e.brain.f[AiBrain::kFireDelay] == 49);
        CHECK(sys.target_set_calls.size() == 1 && sys.target_set_calls[0] == 0x66);
        CHECK(sys.rel_ops.size() == 8);
        CHECK(sys.rel_ops[0].op == kRelEventSpecial && sys.rel_ops[0].a == 0x1234 && sys.rel_ops[0].b == 0x55);
        CHECK(sys.rel_ops[1].op == kRelSharedMem    && sys.rel_ops[1].a == 0x9999 && sys.rel_ops[1].b == 0x55);
        CHECK(sys.rel_ops[2].op == kRelProximity    && sys.rel_ops[2].a == 0x1234 && sys.rel_ops[2].b == 0x66);
        CHECK(sys.rel_ops[3].op == kRelEnemy        && sys.rel_ops[3].a == 0x9999 && sys.rel_ops[3].b == 0x66);
        CHECK(sys.rel_ops[4].op == kRelAllied       && sys.rel_ops[4].a == 0x1234 && sys.rel_ops[4].b == 0x55);
        CHECK(sys.rel_ops[5].op == kRel452B30       && sys.rel_ops[5].a == 0x9999 && sys.rel_ops[5].b == 0x55);
        CHECK(sys.rel_ops[6].op == kRelDamaged      && sys.rel_ops[6].a == 0x1234 && sys.rel_ops[6].b == 0x66);
        CHECK(sys.rel_ops[7].op == kRelSpotted      && sys.rel_ops[7].a == 0x9999 && sys.rel_ops[7].b == 0x66);
    }

    // ---- engage_target: branch A (a brained target) guards jitter by base-delay; sign-extends relmat ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.prng_a = 1;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.relmat_id = 0x8000;             // high bit set -> (int16) sign-extends to -32768
        e.profile.field104 = 0;           // base delay 0 -> branch A skips jitter entirely
        AiTarget t{0x55, 0x66, /*has_brain=*/true};
        sys.engage_target(w, e, t);
        CHECK(e.brain.f[AiBrain::kFireDelay] == 0);     // no jitter when base_delay == 0
        CHECK(sys.prng_a == 1);                          // stream A untouched (jitter skipped)
        CHECK(sys.rel_ops[0].a == -32768);               // movsx of 0x8000

        // Now with a base delay: branch A jitters via stream A (49) and advances it.
        auto sys2_heap = std::make_unique<AiSystem>();
        AiSystem &sys2 = *sys2_heap;
        sys2.prng_a = 1;
        int i2 = sys2.attach(EntityHandle::make(0, 0));
        AiEntity &e2 = *sys2.at(i2);
        e2.profile.field104 = 100;
        AiTarget t2{1, 2, /*has_brain=*/true};
        sys2.engage_target(w, e2, t2);
        CHECK(e2.brain.f[AiBrain::kFireDelay] == 149);   // 100 + 49
        CHECK(sys2.prng_a == 0x8011u);                   // stream A advanced
    }

    // ---- state-16 tick: a visible enemy -> engage (pending 17) instead of walking ----
    // The feed now scans the registry (D-AI-1): spawn a real pool-1 enemy.
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 16);
        Entity self_seed;
        self_seed.team = 1;
        self_seed.health = 100;
        EntityHandle self_h = w.registry.spawn(0, self_seed);
        Entity enemy_seed;
        enemy_seed.team = 2;
        enemy_seed.health = 100;
        enemy_seed.position = Vec3{100.0f, 0.0f, 0.0f};
        enemy_seed.net_id = 0x77; // single key (SSN)
        enemy_seed.group_id = 3;
        enemy_seed.radar_sig = 1000; // per-candidate engage caps (def+376/+378)
        enemy_seed.heat_sig = 1000;
        EntityHandle enemy_h = w.registry.spawn(1, enemy_seed);
        CHECK(enemy_h.valid());

        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(self_h);
        AiEntity &e = *sys.at(idx);
        e.team = 1;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.profile.fov_primary = 0x40;
        e.profile.fov_secondary = 0x40;
        e.profile.range_primary = 1000;
        e.profile.range_secondary = 1000;
        // The class walk gates the feed (D-AI-1): a ground priority admits the
        // pool-1 non-helo enemy [orig: profile+84 -> case 1 @0x467012].
        e.profile.class_priority[1] = 100;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 17);     // engaged
        CHECK(sys.rel_ops.size() == 8);
        CHECK(sys.target_set_calls.size() == 1 && sys.target_set_calls[0] == 0x77);
        CHECK(e.brain.f[AiBrain::kOutSpeed] != 10);      // mover did NOT run (no walk)
        // D-AI-3 closed: the engage APPLIED the sees+targeted quads (keys: group/SSN).
        const Entity *self_e = w.registry.get(self_h);
        CHECK(w.script.relations.single_single(TriggerRelations::kSees, self_e->net_id, 0x77));
        CHECK(w.script.relations.single_single(TriggerRelations::kTargeted, self_e->net_id, 0x77));
        // Entity_SetAITarget maintained the target's +530 refcount.
        CHECK(w.registry.get(enemy_h)->ai_target_refcount == 1);
    }

    // ---- state-18 patrol tick: arrival clears the goal; otherwise fallback vs engage ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        // The arrival proximity is the def turn-rate word (itemDef+0x924), read
        // live from the entity's def [orig: @0x457DCB..0x457DCE].
        w.registry.configure_pool(1, 1);
        Entity hull;
        hull.item_id = 1294;
        const EntityHandle hull_h = w.registry.spawn(1, hull);
        VehicleTraits hull_traits;
        hull_traits.turn_rate = 100;
        w.vehicles.traits.set(hull.item_id, hull_traits);
        int idx = sys.attach(hull_h);
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundEvade; // 18
        e.brain.f[AiBrain::kWorkHeading] = 1000;        // brain[132] target heading
        e.heading = 1050;                                // within [900,1100] -> arrived
        e.patrol_goal = 1;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.patrol_goal == 0);                       // cleared on arrival
        CHECK(e.brain.f[AiBrain::kPendState] == 0);      // no transition yet (goal-clear branch)

        // Goal still set but heading out of range -> goal stays, no transition.
        e.patrol_goal = 1;
        e.heading = 2000;
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.patrol_goal == 1);
        CHECK(e.brain.f[AiBrain::kPendState] == 0);

        // No goal + fallback flag -> pending = fallback state (brain[6]).
        e.patrol_goal = 0;
        e.profile.flags100 = 2;
        e.brain.f[AiBrain::kFallback] = 22;
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 22);

        // No goal + no fallback flag -> pending = 17 (GROUND_COMBAT).
        e.patrol_goal = 0;
        e.profile.flags100 = 0;
        e.brain.f[AiBrain::kPendState] = 0;
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 17);
    }

    // ---- state-18 patrol tick: death queues an event; fire timer decrements ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundEvade;
        e.health = 0;
        e.vel_x = 2000; e.vel_y = 0;          // >= 1057 -> crash death (3)
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(sys.events.count() == 1 && sys.events.at(0).type() == 3);

        e.health = 100;
        e.profile.flags96 = 0x10;             // can-fire
        e.brain.f[AiBrain::kFireTimer] = 50;
        e.brain.f[AiBrain::kStep] = 64;
        e.patrol_goal = 1;                    // stay in the goal branch (no transition)
        e.brain.f[AiBrain::kWorkHeading] = 0; e.heading = 1000; // no def: prox 0, not arrived
        sys.row(kAiGroundEvade).tick(ctx);
		CHECK(e.brain.f[AiBrain::kFireTimer] == 50 - 64); // -= step
	}

    // ---- combat event handler (states 16/17/18 event): damage/death/destroy transitions ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);

        // type 1 (damage): pending = 18 + brain[39] = event[3], when not dead/suppressed.
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kPendState] = 0;
        e.profile.flags96 = 0; // not suppressed
        AiEventEntry dmg{};
        dmg.f[0] = 1; dmg.f[3] = 0xDEAD;
        AiThinkCtx ctx{&sys, &e, &w, &dmg};
        sys.row(kAiGroundFollowWp).event(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 18);
        CHECK(e.brain.f[AiBrain::kDamageInfo] == 0xDEAD);

        // type 1 but already transitioning to 21 -> no 18 (still stashes damage info).
        e.brain.f[AiBrain::kPendState] = 21;
        e.brain.f[AiBrain::kDamageInfo] = 0;
        sys.row(kAiGroundCombat).event(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 21);  // unchanged
        CHECK(e.brain.f[AiBrain::kDamageInfo] == 0xDEAD);

        // type 1 but suppressed (flags96 & 2) -> no 18.
        e.brain.f[AiBrain::kPendState] = 0;
        e.profile.flags96 = 2;
        sys.row(kAiGroundEvade).event(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 0);

        // type 3 (death) -> 21; type 4 (destroy) -> 23.
        AiEventEntry death{}; death.f[0] = 3;
        AiThinkCtx ctx3{&sys, &e, &w, &death};
        sys.row(kAiGroundFollowWp).event(ctx3);
        CHECK(e.brain.f[AiBrain::kPendState] == 21);
        AiEventEntry destroy{}; destroy.f[0] = 4;
        AiThinkCtx ctx4{&sys, &e, &w, &destroy};
        sys.row(kAiGroundCombat).event(ctx4);
        CHECK(e.brain.f[AiBrain::kPendState] == 23);
    }

    // ---- a brain moves only through its row's physics callback: the decided
    // speed, target and heading of a brain with no vehicle mover are never
    // integrated [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53, the +0x1C4
    // call; g_EntityClassPhysicsTable @0x82ABC8 has no generic brain mover] ----
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        int idx = w.ai.attach(EntityHandle::make(1, 0));
        AiEntity &e = *w.ai.at(idx);
        e.pos[0] = 0; e.pos[1] = 0; e.pos[2] = 7 << 16;
        e.brain.f[AiBrain::kOutSpeed] = 3;
        e.brain.f[AiBrain::kWorkPosX] = 100 << 16;
        e.brain.f[AiBrain::kWorkPosY] = 0;
        e.brain.f[AiBrain::kWorkHeading] = 12345;
        const int32_t heading = e.heading;
        TickContext ctx{};
        ctx.world = &w;
        ctx.is_authority = true;
        w.update_all_entities(ctx);
        CHECK(e.pos[0] == 0);
        CHECK(e.pos[1] == 0);
        CHECK(e.pos[2] == (7 << 16));
        CHECK(e.heading == heading);
    }

    // ---- slice-1 exit: an NPC rifleman kills the player through the authoritative
    // round path (perception -> attack anim -> .bad fire events -> ring + RoundSim ->
    // damage -> death), on the listen-server tick shape (AI tick + round tick). ----
    // [witness: world-wac-ai-re §17; net-re §5.60]
    {
        // A root-motion double whose attack clip carries the .bad fire trigger (bit 0x4)
        // every frame; idle carries none. [orig: g_AnimEventTriggerBits @0xA2ED08]
        struct FiringSource : IRootMotionSource {
            bool has_clip(int, int id) const override {
                return id == anim_state::kIdle || id == anim_state::kAttack;
            }
            int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
            bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
                if (!has_clip(0, id)) return false;
                ++phase;
                out = RootMotionFrame{};
                if (id == anim_state::kAttack) out.events = 0x4; // trigger-pull frames
                return true;
            }
        };
        static FiringSource fire_src;

        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        // One rifle round in the ammo table (index 0 is the null entry by convention;
        // use index 1). Velocity 800 u/s, heavy enough to kill in a few hits.
        w.tables.ammo.entries.resize(2);
        w.tables.ammo.entries[1].name = "AMMO_TEST_556";
        w.tables.ammo.entries[1].velocity = 800;
        w.tables.ammo.entries[1].max_age_ticks = 124;
        w.tables.ammo.entries[1].weight_in_grains = 875; // damage = speed_scaled (clamped 1219)
        w.tables.ammo.entries[1].min_damage = 10;
        w.tables.ammo.entries[1].max_damage = 40;
        w.tables.ammo.entries[1].valid = true;

        // The player: pool 0, team 2, 20 u east of the NPC, at ground height 0.
        Entity player_seed;
        player_seed.kind = EntityKind::Organic;
        player_seed.item_id = 1001;
    player_seed.has_item_def = true;
        player_seed.item_type = 3;
        player_seed.team = 2;
        player_seed.health = 100;
        player_seed.net_id = 0x21;
        player_seed.group_id = 2;
        player_seed.position = Vec3{20.0f, 0.0f, 0.0f};
        EntityHandle player_h = w.registry.spawn(0, player_seed);
        CHECK(player_h.valid());

        // The NPC rifleman: pool 0, team 1, armed via the organic ammo seed (§33.35).
        Entity npc_seed;
        npc_seed.kind = EntityKind::Organic;
        npc_seed.team = 1;
        npc_seed.health = 100;
        npc_seed.net_id = 0x11;
        npc_seed.group_id = 1;
        npc_seed.position = Vec3{0.0f, 0.0f, 0.0f};
        EntityHandle npc_h = w.registry.spawn(0, npc_seed);
        CHECK(npc_h.valid());
        // Headless: no models, so the rig's posed launch point is emulated at
        // chest height (a production world resolves it through
        // EntityPoseProvider). The NPC aims at the player's raw Position, never
        // its eye point [orig: Entity_UpdateInfantryAI @0x4B9910 (the dead
        // Entity_ComputeWeaponFireOrigin calls @0x4BC720 / @0x4BCB23)], while
        // the unposed-person fallback sphere sits 0.9 u above that origin: only
        // a steep shot crosses it, so the player stands in a pit 40 u below.
        FakeMuzzleProvider provider;
        w.registry.get(player_h)->position.z = -40.0f;
        w.registry.get(player_h)->eye_offset_z = static_cast<int32_t>(0.9 * 65536.0);
        provider.points[npc_h.packed] = {0, 0, static_cast<int32_t>(0.9 * 65536.0)};
        w.pose_provider = &provider;

        AiSystem &sys = w.ai;
        sys.is_authority = true;
        sys.root_motion = &fire_src;
        int idx = sys.attach(npc_h);
        AiEntity &npc = *sys.at(idx);
        npc.inf.active = true;
        npc.team = 1;
        npc.net_id = 0x11;
        npc.pos[0] = 0; npc.pos[1] = 0; npc.pos[2] = 0;
        npc.profile.organic.ammo.fill(1); // -> w.tables.ammo[1]
        npc.profile.organic.launch = {1, 2, 3};
        npc.profile.clip_size = 30;
        npc.inf.magazine = 30;
        npc.slot.f[10] = 0;                    // perfect accuracy (w_accuracy 100)
        npc.slot.f[11] = 0;
        npc.slot.f[15] = 60 << 16;             // attack range 60 u
        npc.slot.f[16] = 10 << 16;             // approach range
        npc.slot.f[17] = 100 << 16;            // sight range 100 u
        npc.slot.f[22] = 62;                   // cooldown base (~1 s)

        TickContext tctx;
        tctx.world = &w;
        tctx.is_authority = true;
        bool acquired = false, fired = false, killed = false;
        for (uint32_t t = 0; t < 2000 && !killed; ++t) {
            tctx.logic_tick = t;
            w.update_all_entities(tctx);
            if (npc.inf.combat_target == player_h) acquired = true;
            if (w.out.rounds.count > 0) fired = true;
            for (const RoundDeath &d : w.round_sim.deaths)
                if (d.victim == player_h && d.killer == npc_h) killed = true;
        }
        CHECK(acquired);                        // the 32-tick perception found the player
        CHECK(fired);                           // rounds entered the ring (the 0x0A fan-out)
        CHECK(w.round_sim.deaths.size() >= 1);  // the damage pass detected the death
        CHECK(killed);                          // ...credited NPC -> player
        CHECK(w.registry.get(player_h)->health <= 0);
        // The engagement left the witnessed side effects: the sees+targeted quads
        // (group/SSN keys) and the shooter's priority mark from firing.
        CHECK(w.script.relations.single_single(TriggerRelations::kSees, 0x11, 0x21));
        CHECK(w.script.relations.single_single(TriggerRelations::kTargeted, 0x11, 0x21));
        CHECK(w.script.relations.group_group(TriggerRelations::kSees, 1, 2));
        CHECK((w.registry.get(npc_h)->engine_flags & 0x4000u) != 0);
        // The kill staged the bullet death-anim selection on the victim at damage
        // time: torso group (the bone stand-in, D-AI-9) = 184..187 by quadrant, and
        // the victim's group went alert red. [orig: OrganicClass_HandleEvent
        // @0x407478/@0x4073ea; world-wac-ai-re §19]
        const int sel = w.registry.get(player_h)->death_anim_state;
        CHECK(sel >= 184 && sel <= 187);
        CHECK(w.script.relations.group(2).alert == TriggerRelations::kAlertRed);
    }

    test_vehicle_death_rows();
    test_vehicle_death_states_send_kill_record();
    test_vehicle_brain_think_countdown();
    test_fire_pass_uses_embedder_fed_muzzle();
    test_weapon_fire_origin_fallback_chain();
    test_los_endpoints_use_muzzle_stamp();
    test_aim_solution_uses_muzzle_stamp();
    test_aim_origin_takes_the_non_person_leg();
    test_world_feed_never_engages_same_team();
    test_berserk_candidate_is_intentional_team_exception();
    test_script_target_policy_reaches_infantry_and_weapons();
    test_guided_round_notification_and_ally_alert_scope();
    test_damage_hit_sets_retail_alert_state();
    test_remote_player_hit_skips_npc_group_alert();
    test_mounted_gunner_acquires_and_fires();
    test_mounted_gunner_fires_from_the_slot_barrel();
    test_weapon_fire_position_legs();
    test_turret_gunners_scan_from_the_gun_point();
    test_water_crossing_fires_once_on_entry();
    test_infantry_floats_and_splashes_once();
    test_mounted_fire_uses_retail_range_and_spatial_stagger();
    test_weapon_walk_visits_pool0_in_slot_order();
    test_weapon_walk_pumps_hot_unoccupied_guns();
    test_mounted_request_copies_the_parent_adm_byte();
    test_mounted_gunner_runs_the_anim_event_fire_block();
    test_mounted_request_runs_for_a_passenger();
    test_mounted_request_copies_the_parents_live_byte();
    test_sm_fire_stamps_the_hull_adm_byte();
    test_aircraft_fire_stamps_the_hull_adm_byte();
    test_mounted_look_traverses_before_fire_request();
    test_mounted_gunner_dismounts_into_death_animation();
    test_mounted_collision_tail_uses_retail_eight_tick_phase_without_models();
    test_joiner_evaluates_vehicle_idle_without_integrating_motor();
    test_lethal_hit_blends_into_death_animation_without_position_jump();
    test_round_hit_reaches_the_same_pass_body_update();
    test_entity_update_stamps_the_session_fact();
    test_sm_turret_fire();
	test_aircraft_combat_states();
	test_vehicle_weapon_pose_and_target_cleanup();
	test_vehicle_class_state_machine_gates();
	test_dead_vehicle_despawn_frees_brain();
	test_aircraft_fire_arc_gate();
	test_aircraft_combat_mover_pins();

	if (failures == 0)
		std::printf("ai: all tests passed\n");
	return failures ? 1 : 0;
}
