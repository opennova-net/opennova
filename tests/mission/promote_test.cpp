// Mission -> world promotion: a synthetic BMS mission is promoted into a live world +
// AI system, then the AI is ticked to prove the brains/nav are wired to the real mission
// data (entities patrol their authored routes). See engine/runtime/mission/promote.cpp.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <runtime/mission/promote.h>
#include <runtime/world/ai.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static void stamp_fixture_carrier_defs(World &world) {
	// Seat-only fixtures omit items.def; supply the metadata real promotion loads.
	world.registry.for_each([&](const Entity &v) {
		if (!v.seats.empty() && !v.has_item_def) {
			Entity &def = *world.registry.get(v.handle);
			def.has_item_def = true;
			def.item_type = 1;
			def.item_attrib |= kItemAttribPlayerControl;
		}
	});
}

static bms::Entity organic(int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id,
                           int32_t wp_num) {
    bms::Entity e{};
    e.type = bms::ItemType::Organic;
    e.x = x; e.y = y; e.z = z;
    e.yaw = 90;
    e.team = team;
    e.waypoint_id = wp_id;
    e.wp_number = wp_num;
    // PLAIN WORLD UNITS, not 16.16 -- shipped missions author small integers here
    // (00TRg's soldiers carry 10..500). The slot seed shifts these UP to 16.16
    // [orig: slot+60/+64 engagement (<<16)], and the AI profile takes them unscaled.
    e.min_engagement_distance = 50;
    e.max_engagement_distance = 500;
    return e;
}

static bms::Entity marker(int32_t x, int32_t y, int32_t z) {
    bms::Entity e{};
    e.type = bms::ItemType::Marker;
    e.x = x; e.y = y; e.z = z;
    return e;
}

static bms::Entity item(int32_t type_id, int32_t x, int32_t y, int32_t z) {
    bms::Entity e{};
    e.type = bms::ItemType::Item;
    e.type_id = type_id;
    e.x = x; e.y = y; e.z = z;
    return e;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
// Every pool loop hands Entity_SpawnFromBMSRecord `Pool_GetEntry(pool, i)` for
// record i, so a record's pool slot IS its index within its pool section and a
// record never slides into an earlier hole [orig: Mission_LoadBMSFile @0x40F4E0
//  — pool 1 @0x40f9bb..0x40f9c6, pool 0 @0x40fb0d..0x40fb19]. (Promote resets
// the pools first, so the overwrite arm of spawn_at is not stageable here; the
// discriminating case arrives with the spawn FILTER — world-wac-ai-re §31.4.)
static void test_bms_record_index_is_pool_slot() {
    bms::File m{};
    m.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
    m.items[0].id = 21;
    m.items.push_back(item(/*type_id=*/164, 30 << 16, 20 << 16, 3 << 16));
    m.items[1].id = 22;
    m.organics.push_back(organic(1 << 16, 1 << 16, 0, 1, 0, 0));
    m.organics[0].id = 31;

    auto w = std::make_unique<World>();
    AiSystem *ai = &w->ai;
    const mission::PromoteResult r = mission::promote_mission(m, *w, {});
    CHECK(r.spawned == 3);
    CHECK(r.dropped == 0);
    const EntityHandle h21 = w->registry.find_by_net_id(21);
    const EntityHandle h22 = w->registry.find_by_net_id(22);
    const EntityHandle h31 = w->registry.find_by_net_id(31);
    CHECK(h21.valid() && h21.pool() == 1 && h21.slot() == 0);
    CHECK(h22.valid() && h22.pool() == 1 && h22.slot() == 1);
    CHECK(h31.valid() && h31.pool() == 0 && h31.slot() == 0);
}

static void test_emplacement_attachments() {
    bms::File cm{};
    cm.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
    cm.items[0].id = 11;

    mission::PromoteOptions co{};
    mission::ItemSeatSpec carrier{};
    carrier.type_id = 164;
    mission::ItemEmplacementAttachmentSpec plain{};
    plain.child_type_id = 166;
    plain.kind = mission::EmplacementAttachmentKind::Standard;
    plain.stored_slot = 1;
    plain.anchor.type = SeatType::Gunner;
    plain.anchor.bone_index = 4;
    plain.anchor.seat_local = {2.f, 0.f, 1.f};
    plain.anchor_found = true;
    carrier.emplacement_attachments.push_back(plain);
    mission::ItemEmplacementAttachmentSpec gun{};
    gun.child_type_id = 183;
    gun.kind = mission::EmplacementAttachmentKind::G;
    gun.stored_slot = 2;
    gun.attachment_flags = 2;
    gun.anchor.type = SeatType::Gunner;
    gun.angle_count = 4;
    gun.down_limit_bam = 70 * 11930464;
    carrier.emplacement_attachments.push_back(gun);
    mission::ItemEmplacementAttachmentSpec crosshair{};
    crosshair.child_type_id = 182;
    crosshair.kind = mission::EmplacementAttachmentKind::C;
    crosshair.stored_slot = 3;
    crosshair.attachment_flags = 1;
    crosshair.anchor.type = SeatType::Gunner;
    carrier.emplacement_attachments.push_back(crosshair);
    co.item_seat_specs.push_back(carrier);

    mission::ItemSeatSpec gun_item{};
    gun_item.type_id = 183;
    Seat usegun{};
    usegun.type = SeatType::Gunner;
    gun_item.seats.push_back(usegun);
    gun_item.primary_weapon = "WPN_HELOGUN";
    co.item_seat_specs.push_back(gun_item);

    auto cw = std::make_unique<World>();
    AiSystem *cai = &cw->ai;
    const mission::PromoteResult cr =
            mission::promote_mission(cm, *cw, co);
    CHECK(cr.spawned == 4);
    CHECK(cw->registry.live_count() == 4);
    const EntityHandle parent_h = cw->registry.find_by_net_id(11);
    EntityHandle plain_h{}, gun_h{}, crosshair_h{};
    cw->registry.for_each([&](const Entity &e) {
        if (e.item_id == 166) plain_h = e.handle;
        if (e.item_id == 183) gun_h = e.handle;
        if (e.item_id == 182) crosshair_h = e.handle;
    });
    Entity *parent = cw->registry.get(parent_h);
    Entity *plain_child = cw->registry.get(plain_h);
    Entity *gun_child = cw->registry.get(gun_h);
    Entity *crosshair_child = cw->registry.get(crosshair_h);
    CHECK(parent != nullptr && plain_child != nullptr);
    CHECK(gun_child != nullptr && crosshair_child != nullptr);
    if (parent == nullptr || plain_child == nullptr ||
        gun_child == nullptr || crosshair_child == nullptr)
        return;
    CHECK(plain_child->emplacement_parent == parent_h);
    CHECK(plain_child->ground_target == parent_h);
    CHECK(plain_child->emplacement_parent_spawn_id == parent->registry_spawn_id);
    CHECK(plain_child->emplacement_pose_metadata_resolved);
    CHECK(plain_child->emplacement_kind == 0);
    CHECK(gun_child->emplacement_pose_metadata_resolved);
    CHECK(gun_child->emplacement_kind == 1);
    CHECK(gun_child->ground_target == parent_h);
    CHECK(gun_child->emplacement_attachment_flags == 2);
    CHECK(gun_child->emplacement_angle_count == 4);
    CHECK(gun_child->emplacement_down_limit_bam == 70 * 11930464);
    CHECK(gun_child->seats.size() == 1);
    CHECK(gun_child->primary_weapon == "WPN_HELOGUN");
    CHECK(crosshair_child->emplacement_kind == 2);
    CHECK(crosshair_child->emplacement_pose_metadata_resolved);
    CHECK(crosshair_child->ground_target == parent_h);
    CHECK(crosshair_child->emplacement_attachment_flags == 1);
    CHECK(plain_child->position.x == 12.f && plain_child->position.y == 20.f &&
          plain_child->position.z == 4.f);
    CHECK(gun_child->position.x == 10.f && gun_child->position.y == 20.f &&
          gun_child->position.z == 3.f);

    parent->position = {30.f, 40.f, 5.f};
    parent->yaw = 90;
    cw->run_logic_tick();
    plain_child = cw->registry.get(plain_h);
    CHECK(plain_child->position.x == 30.f && plain_child->position.y == 38.f &&
          plain_child->position.z == 6.f);
    Entity rider_seed{};
    rider_seed.kind = EntityKind::Organic;
    rider_seed.health = 100;
    const EntityHandle rider_h = cw->registry.spawn(0, rider_seed);
    Entity *rider = cw->registry.get(rider_h);
    rider->mounted = true;
    rider->mount_target = gun_h;
    rider->mount_seat = 0;
    rider->mount_type = SeatType::Gunner;
    gun_child->seats[0].occupant = rider_h;
    const uint64_t old_parent_spawn_id = parent->registry_spawn_id;
    cw->registry.despawn(parent_h);
    Entity replacement_seed{};
    replacement_seed.kind = EntityKind::Item;
    replacement_seed.item_id = 999;
    const EntityHandle replacement_h =
            cw->registry.spawn(1, replacement_seed);
    CHECK(replacement_h == parent_h);
    CHECK(cw->registry.get(replacement_h)->registry_spawn_id !=
          old_parent_spawn_id);
    cw->run_logic_tick();
    CHECK(cw->registry.get(plain_h) == nullptr);
    CHECK(cw->registry.get(gun_h) == nullptr);
    CHECK(cw->registry.get(crosshair_h) == nullptr);
    rider = cw->registry.get(rider_h);
    CHECK(rider != nullptr && !rider->mounted);
    CHECK(cw->registry.get(replacement_h) != nullptr);
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static void test_emplacement_parent_death_cascades() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(0, 4);
    world->registry.configure_pool(1, 8);

    Entity parent_seed{};
    parent_seed.kind = EntityKind::Item;
    parent_seed.item_id = 164;
    const EntityHandle parent_h = world->registry.spawn(1, parent_seed);
    Entity *parent = world->registry.get(parent_h);
    CHECK(parent != nullptr);
    if (parent == nullptr) return;

    Entity child_seed{};
    child_seed.kind = EntityKind::Item;
    child_seed.item_id = 166;
    child_seed.emplacement_parent = parent_h;
    child_seed.emplacement_parent_spawn_id = parent->registry_spawn_id;
    const EntityHandle child_h = world->registry.spawn(1, child_seed);
    Entity *child = world->registry.get(child_h);
    CHECK(child != nullptr);
    if (child == nullptr) return;

    Entity grandchild_seed{};
    grandchild_seed.kind = EntityKind::Item;
    grandchild_seed.item_id = 183;
    grandchild_seed.emplacement_parent = child_h;
    grandchild_seed.emplacement_parent_spawn_id = child->registry_spawn_id;
    const EntityHandle grandchild_h = world->registry.spawn(1, grandchild_seed);
    Entity *grandchild = world->registry.get(grandchild_h);
    CHECK(grandchild != nullptr);
    if (grandchild == nullptr) return;

    Entity rider_seed{};
    rider_seed.kind = EntityKind::Organic;
    const EntityHandle rider_h = world->registry.spawn(0, rider_seed);
    Entity *rider = world->registry.get(rider_h);
    CHECK(rider != nullptr);
    if (rider == nullptr) return;
    Seat usegun{};
    usegun.type = SeatType::Gunner;
    usegun.occupant = rider_h;
    grandchild->seats.push_back(usegun);
    rider->mounted = true;
    rider->mount_target = grandchild_h;
    rider->mount_seat = 0;
    rider->mount_type = SeatType::Gunner;

    // The ordinary item-death path keeps the carrier entity resident as a husk.
    // Its implicit attachment ownership must still end immediately.
    parent->health = 0;
    destruction_notify_item_damage(*world, *parent, 2);
    CHECK(world->registry.get(parent_h) != nullptr);
    CHECK(!parent->alive);

    world->run_logic_tick();
    CHECK(world->registry.get(parent_h) != nullptr);
    CHECK(world->registry.get(child_h) == nullptr);
    CHECK(world->registry.get(grandchild_h) == nullptr);
    rider = world->registry.get(rider_h);
    CHECK(rider != nullptr && !rider->mounted);
    CHECK(rider != nullptr && !rider->mount_target.valid());
}

static void test_unresolved_emplacement_preserves_streamed_pose() {
    auto world = std::make_unique<World>();
    world->registry.configure_pool(1, 8);

    Entity parent_seed{};
    parent_seed.kind = EntityKind::Item;
    parent_seed.position = {30.f, 40.f, 5.f};
    const EntityHandle parent_h = world->registry.spawn(1, parent_seed);
    Entity *parent = world->registry.get(parent_h);
    CHECK(parent != nullptr);
    if (parent == nullptr) return;

    // A stock streamed addeweap row gives the child an exact absolute pose and
    // a carrier identity, but not necessarily a uniquely recoverable authored
    // userpoint. Until that metadata resolves, the world must retain the wire
    // pose instead of collapsing the child onto the carrier root.
    Entity child_seed{};
    child_seed.kind = EntityKind::Item;
    child_seed.position = {17.f, 23.f, 9.f};
    child_seed.yaw = 37;
    child_seed.pitch = -4;
    child_seed.roll = 6;
    child_seed.emplacement_parent = parent_h;
    child_seed.emplacement_parent_spawn_id = parent->registry_spawn_id;
    const EntityHandle child_h = world->registry.spawn(1, child_seed);
    CHECK(child_h.valid());

    world->run_logic_tick();
    const Entity *child = world->registry.get(child_h);
    CHECK(child != nullptr);
    if (child == nullptr) return;
    CHECK(!child->emplacement_pose_metadata_resolved);
    CHECK(child->position.x == 17.f && child->position.y == 23.f &&
          child->position.z == 9.f);
    CHECK(child->yaw == 37 && child->pitch == -4 && child->roll == 6);
}

// D-HUD-20: the authored display name rides name_index through the embedder's
// [PeopleNames] resolver with the retail 15-char copy, and the friendly-tag
// gather applies the witnessed pass gates (team, player flag, aliveness).
// [orig: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a;
//  HUD_DrawFriendlyTagsPass @0x5a4480]
static void test_friendly_tag_names_and_gather() {
    bms::File m{};
    m.organics.push_back(organic(10 << 16, 10 << 16, 0, /*team=*/1, 0, 0));
    m.organics[0].id = 21;
    m.organics[0].name_index = 5;
    m.organics.push_back(organic(20 << 16, 10 << 16, 0, /*team=*/1, 0, 0));
    m.organics[1].id = 22; // name_index 0 -> unnamed, the draw-time fallback
    m.organics.push_back(organic(30 << 16, 10 << 16, 0, /*team=*/2, 0, 0));
    m.organics[2].id = 23; // enemy team: never gathered

    mission::PromoteOptions o{};
    o.people_name_resolver = [](int32_t idx) {
        return idx == 5 ? std::string("123456789012345XYZ") : std::string();
    };
    auto w = std::make_unique<World>();
    AiSystem *ai = &w->ai;
    mission::promote_mission(m, *w, o);

    Entity *named = w->registry.get(w->registry.find_by_net_id(21));
    Entity *unnamed = w->registry.get(w->registry.find_by_net_id(22));
    Entity *enemy = w->registry.get(w->registry.find_by_net_id(23));
    CHECK(named != nullptr && unnamed != nullptr && enemy != nullptr);
    if (named == nullptr || unnamed == nullptr || enemy == nullptr) return;
    CHECK(named->display_name == "123456789012345"); // the strncpy(_, _, 15)
    CHECK(unnamed->display_name.empty());
    // The production item-traits sweep stamps has_item_def; mirror it here.
    named->has_item_def = true;
    unnamed->has_item_def = true;
    enemy->has_item_def = true;

    Entity viewer{};
    viewer.team = 1; // a synthetic local player outside the registry
    // The pass-level gate: `g_GameType || death screen` [orig: @0x5a44e8].
    FriendlyTagPassContext ctx;
    ctx.game_type = 0x30020u;
    std::vector<FriendlyTagSource> tags;
    collect_friendly_tags(*w, viewer, tags);
    CHECK(tags.empty()); // game type 0 and no death screen draws nothing
    collect_friendly_tags(*w, viewer, tags, ctx);
    CHECK(tags.size() == 2); // both team-1 organics, never the enemy
    bool saw_named = false;
    bool saw_unnamed = false;
    for (const FriendlyTagSource &t : tags) {
        if (t.net_id == 21) {
            saw_named = true;
            CHECK(t.name == "123456789012345");
            CHECK(t.health_ratio_fp16 == 0x10000);
        }
        if (t.net_id == 22) {
            saw_unnamed = true;
            CHECK(t.name.empty()); // the compiler resolves '^' + table[id % 36]
        }
        CHECK(!t.player);
        CHECK(!t.dead && !t.has_slot);
    }
    CHECK(saw_named && saw_unnamed);

    // A dead entity STAYS labelled, carrying the dead latch the bad tier's
    // downed legs read [orig: `Flags & 2` @0x5a3c1c; the entry bails only
    // test Flags & 1 @0x5a39eb]; a CARRIED one drops.
    named->alive = false;
    tags.clear();
    collect_friendly_tags(*w, viewer, tags, ctx);
    CHECK(tags.size() == 2);
    for (const FriendlyTagSource &t : tags)
        CHECK(t.dead == (t.net_id == 21));
    named->alive = true;
    named->flags |= kEntityFlagCarried;
    tags.clear();
    collect_friendly_tags(*w, viewer, tags, ctx);
    CHECK(tags.size() == 1 && tags[0].net_id == 22);
    named->flags &= ~kEntityFlagCarried;

    // The enemy is labelled only while the death screen is up
    // [orig: the team gate's death-screen arm @0x5a44df].
    FriendlyTagPassContext death_ctx = ctx;
    death_ctx.death_screen = true;
    tags.clear();
    collect_friendly_tags(*w, viewer, tags, death_ctx);
    CHECK(tags.size() == 3);

    // Player-controlled entities ride the slot walk, not the pool walk
    // [orig: the Flags & 0x100 skip @0x5a44bc]: without a slot owner they
    // are never visited; with one they carry the slot's downed facts.
    unnamed->flags |= kEntityFlagPlayer;
    tags.clear();
    collect_friendly_tags(*w, viewer, tags, ctx);
    CHECK(tags.size() == 1 && tags[0].net_id == 21);
    const PlayerSlotLookup slots = [&](EntityHandle h, PlayerSlotFacts &f) {
        if (!(h == unnamed->handle)) return false;
        f.revive_seconds = 87;
        f.medic_request = true;
        return true;
    };
    ctx.slot_lookup = &slots;
    tags.clear();
    collect_friendly_tags(*w, viewer, tags, ctx);
    CHECK(tags.size() == 2);
    for (const FriendlyTagSource &t : tags) {
        if (t.net_id != 22) continue;
        CHECK(t.player && t.has_slot && t.revive_seconds == 87 && t.medic_request);
    }
    ctx.slot_lookup = nullptr;
    unnamed->flags &= ~kEntityFlagPlayer;
}

// A placed vehicle with no ai_textfile still gets a profile: retail's AI init
// falls back to the def's default_aip (vehicle family) or "helo1" (both
// families' last arm), so a bare-placed Blackhawk's brain is type HELO and its
// rotor twin runs [orig: Entity_InitHelicopterAIFromDef @0x4683C0 the helo1
// arm @0x4684c9; Entity_InitVehicleAIFromDef @0x4686C0 the def+0x8B8 arm
// @0x4687c1 then helo1 @0x4687d3; Entity_UpdateHeloRotorSpin @0x48FA70 gates on
// profile+0x10 == 1 @0x48fa98].
static void test_nameless_vehicle_takes_the_retail_default_profile() {
    int failures = 0;
    static constexpr int32_t kHeloType = 2010;  // Dblkhwk1, ai_function chel
    static constexpr int32_t kTruckType = 1237; // a cveh row whose def authors default_aip
    static constexpr int32_t kBoatType = 1500;  // a cbot row with no default_aip
    static constexpr int32_t kCrateType = 9;    // no AI class row at all

    mission::PromoteOptions opts;
    opts.ai_profile_defaults = [](int32_t type_id) {
        mission::PromoteOptions::AiProfileDefaults d;
        if (type_id == kHeloType) {
            d.known = true; d.helicopter_init = true; d.default_aip = "H_Ignored";
        } else if (type_id == kTruckType) {
            d.known = true; d.default_aip = "D_5ton";
        } else if (type_id == kBoatType) {
            d.known = true;
        }
        return d;
    };
    const auto entity = [](int32_t type_id, const char *name2) {
        bms::Entity e{};
        e.type = bms::ItemType::Item;
        e.type_id = type_id;
        if (name2 != nullptr) std::memcpy(e.name2, name2, std::strlen(name2));
        return e;
    };
    // The name resolution, arm by arm.
    CHECK(mission::ai_profile_name_for(entity(kHeloType, nullptr), true, opts.ai_profile_defaults) == "helo1");
    CHECK(mission::ai_profile_name_for(entity(kTruckType, nullptr), true, opts.ai_profile_defaults) == "d_5ton");
    CHECK(mission::ai_profile_name_for(entity(kBoatType, nullptr), true, opts.ai_profile_defaults) == "helo1");
    CHECK(mission::ai_profile_name_for(entity(kCrateType, nullptr), true, opts.ai_profile_defaults).empty());
    CHECK(mission::ai_profile_name_for(entity(kHeloType, "H_BHawk "), true, opts.ai_profile_defaults) == "h_bhawk");
    // Only a placed ITEM takes the fallback; an organic with no name keeps none.
    CHECK(mission::ai_profile_name_for(entity(kHeloType, nullptr), false, opts.ai_profile_defaults).empty());
    // No embedder answer (tests, no item db): the ai_textfile alone, as before.
    CHECK(mission::ai_profile_name_for(entity(kHeloType, nullptr), true, {}).empty());

    // The promote seeds the brain from the fallback row.
    bms::File m{};
    m.items.push_back(entity(kHeloType, nullptr));
    mission::ItemSeatSpec spec;
    spec.type_id = kHeloType;
    Seat ctrl;
    ctrl.type = SeatType::Controller;
    spec.seats.push_back(ctrl);
    opts.item_seat_specs.push_back(spec);
    mission::PromoteOptions::AiProfileRow helo1;
    helo1.profile = "helo1";
    helo1.data.type = 1;
	helo1.data.default_state = kAiHeloLand;
	helo1.data.helo_patrol_speed = 3000;
	helo1.data.helo_combat_speed = 5000;
	helo1.data.helo_patrol_climb = 4000;
	helo1.data.helo_combat_climb = 6000;
	helo1.data.helo_patrol_altitude = 20 << 16;
	helo1.data.helo_combat_altitude = 40 << 16;
	helo1.data.min_agl = 10 << 16;
	helo1.data.min_speed = 700;
	helo1.data.min_chase = 30 << 16;
	helo1.data.max_chase = 80 << 16;
	helo1.data.radar_dist = 500 << 16;
	helo1.data.view_dist = 300 << 16;
	helo1.data.radar_fov_bam = 0x40000000;
	helo1.data.view_fov_bam = 0x20000000;
	helo1.data.evade_flags = 0x10;
	helo1.data.combat_flags = 0x40;
	helo1.data.use_waypoint_z = 1;
	helo1.data.hunt_flags = 1;
	helo1.data.drive_skill = 3;
	helo1.data.primary.weapon = "50cal";
	helo1.data.primary.ammo = 60;
	helo1.data.primary.flags = 1;
	helo1.data.primary.facing_bam = 12345;
	helo1.data.primary.pitch_bam = 67890;
	helo1.data.aim_skill = 4;
	opts.ai_profiles.push_back(helo1);
	World world;
    AiSystem &ai = world.ai;
    ai.is_authority = true;
    const mission::PromoteResult r = mission::promote_mission(m, world, opts);
    CHECK(r.brains == 1);
    CHECK(ai.count() == 1);
    CHECK(ai.at(0) != nullptr && ai.at(0)->profile.type == 1);
	const AiEntity &air = *ai.at(0);
	CHECK(air.brain.cur_state() == kAiHeloLand);
	CHECK(air.brain.f[AiBrain::kSpeedA] == 5000 && air.brain.f[AiBrain::kSpeedB] == 3000);
	CHECK(air.profile.patrol_altitude == (20 << 16) && air.profile.field216 == (40 << 16));
	CHECK(air.profile.patrol_climb == 4000 && air.profile.field220 == 6000);
	CHECK(air.profile.min_agl == (10 << 16) && air.profile.min_speed == 700);
	CHECK(air.profile.min_chase == (30 << 16) && air.profile.max_chase == (80 << 16));
	CHECK(air.profile.range_primary == 500 && air.profile.range_secondary == 300);
	CHECK(air.profile.fov_primary == 0x40 && air.profile.fov_secondary == 0x20);
	CHECK(air.profile.view_fov_bam == 0x20000000 && air.profile.radar_fov_bam == 0x40000000);
	CHECK(air.profile.flags96 == 0x10 && air.profile.flags100 == 0x40);
	CHECK(air.profile.flight_flags == 1 && air.brain.f[AiBrain::kUseWaypointZones] == 1);
	CHECK(air.brain.f[AiBrain::kDriveSkill] == 3 && air.brain.f[AiBrain::kAccuracy] == 4);
	CHECK(air.brain.f[AiBrain::kAmmoA] == 60 && air.profile.fire_a.ammo_name == "50cal");
	CHECK(air.brain.f[AiBrain::kActiveYaw] == 12345 &&
			air.brain.f[AiBrain::kStagingBlock + 3] == 12345);
	CHECK(air.brain.f[AiBrain::kElevationBias] == 67890);
	CHECK(air.brain.f[51] >= 0 && air.brain.f[51] < (20 << 16));
	if (failures)
		std::exit(1);
}

int main() {
    // A synthetic mission: 3 markers forming a path, 1 looping waypoint record (channel 0),
    // 2 organics on that route (teams 1/2), 1 building.
    bms::File m{};
    m.markers.push_back(marker(100 << 16, 0, 0));
    m.markers.push_back(marker(200 << 16, 0, 0));
    m.markers.push_back(marker(300 << 16, 0, 0));

    bms::WaypointRecord wr{};
    wr.flags = bms::WaypointFlags::None; // loops
    wr.marker_count = 3;
    wr.waypoint_numbers = {0, 1, 2};
    // The .bms waypoint block is positional (slot == authored list id, slot 0 never
    // authored); this route is list 1, so it lives at slot 1. List 3 sits above an
    // UNAUTHORED list 2 — the 00TRg shape that exposed the shifted channel table
    // (an order for list 3 must read slot 3, not the empty slot 2).
    bms::WaypointRecord wr3{};
    wr3.flags = bms::WaypointFlags::None;
    wr3.marker_count = 2;
    wr3.waypoint_numbers = {2, 0};
    m.waypoint_records.resize(4);
    m.waypoint_records[1] = wr;
    m.waypoint_records[3] = wr3;

    // waypoint_id is 1-based (channel 0 = the AI "no route" sentinel); these patrol channel 1.
    m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1, /*wp_num=*/0));
    m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1, /*wp_num=*/0));
    // Organic 1 authors an ai_textfile whose .aip speeds the embedder resolved —
    // its brain seeds the profile speeds at the witnessed x65536/225 scale while
    // organic 0 keeps the default_speed stand-in.
    std::memcpy(m.organics[1].name2, "d_zode", 7);
    m.organics[0].bmsi_attributes =
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Blind) |
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Guarding) |
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Berserk) |
            static_cast<uint32_t>(bms::BmsiAttributeFlags::Coward);

    bms::Entity bldg{};
    bldg.type = bms::ItemType::Building;
    bldg.x = 999 << 16;
    m.buildings.push_back(bldg);

    // The SSN every trigger/action references is AUTHORED in the record (the editor
    // assigns it); promotion copies it verbatim. [orig: Entity_SpawnFromBMSRecord
    // @0x40e9f0 -> entity+124 = record dword @+8]
    m.organics[0].id = 1;
    m.organics[1].id = 2;
    m.buildings[0].id = 3;
    m.markers[0].id = 10;
    m.markers[1].id = 11;
    m.markers[2].id = 12;

    World world;
    AiSystem &ai = world.ai;
    ai.is_authority = true;

    mission::PromoteOptions opts;
    opts.arrival_radius = 1000;
    opts.default_speed = 20;
    mission::PromoteOptions::AiProfileRow zode;
    zode.profile = "d_zode";
    // ASYMMETRIC on purpose (h_ah6b_z.aip-shaped): the witnessed seeding is
    // CROSSED — brain[49]=kSpeedA <- +0xC4 combat, brain[50]=kSpeedB <- +0xC0
    // patrol — and a symmetric pair cannot detect a swapped wiring.
    zode.data.patrol_speed = 70;
    zode.data.combat_speed = 150;
    // Class-walk data (D-AI-1): distinct priorities pin the +40..+52 sort —
    // GROUND type loads the sort keys, walk order priority-descending
    // [orig: AIProfile_LoadOrFind @0x45fd80 qsort + reversed store].
    zode.data.type = 2;
    zode.data.priority_air = 10;
    zode.data.priority_ground = 200;
    zode.data.priority_organics = 100;
    zode.data.priority_decorations = 0;
    opts.ai_profiles.push_back(zode);
    mission::PromoteResult r = mission::promote_mission(m, world, opts);

    // ---- promotion populated entities + brains + nav ----
    CHECK(r.nav_nodes == 3);
    CHECK(r.nav_channels == 4); // channel table size == the slot-indexed record table
    CHECK(r.brains == 2);   // the 2 organics
    CHECK(r.spawned == 6);  // 1 building + 3 markers (pool 3, as the original spawns them) + 2 organics
    CHECK(r.dropped == 0);
    CHECK(ai.count() == 2);

    // nav table built from markers + the waypoint record.
    CHECK(ai.nav.nodes.size() == 3);
    CHECK(ai.nav.nodes[0].f[1] == (100 << 16)); // marker 0 X
    CHECK(ai.nav.nodes[0].f[0] == 1000);        // arrival radius (payload0)
    CHECK(ai.nav.channel(0) != nullptr);
    CHECK(ai.nav.channel(0)->count == 0);       // slot 0 is never authored = "no route"
    const NavChannel *ch = ai.nav.channel(1);   // channel == slot == the authored list id
    CHECK(ch != nullptr);
    CHECK(ch->count == 3);
    CHECK(ch->loopflag == 0);             // WaypointFlags::None -> loops
    CHECK(ch->entries[2] == 2);
    // The gap shape: list 2 unauthored (empty), list 3 present at its own slot.
    CHECK(ai.nav.channel(2) != nullptr && ai.nav.channel(2)->count == 0);
    CHECK(ai.nav.channel(3) != nullptr && ai.nav.channel(3)->count == 2);
    CHECK(ai.nav.channel(3)->entries[0] == 2);

    // organic 0 is in GROUND_FOLLOWWP with its route + spawn transform.
    AiEntity *e0 = ai.at(0);
    // Engage-range UNITS, both conventions pinned together so they cannot drift apart
    // again: the AI PROFILE takes the BMS value unscaled (world units, i16), while the
    // SLOT copy is the same value shifted to 16.16. A previous uncited `>> 16` on the
    // profile side zeroed both ranges for every shipped mission, and ai_score_target
    // rejects every candidate when the range is 0.
    CHECK(e0->profile.range_primary == 500);
    CHECK(e0->profile.range_secondary == 50);
    CHECK(e0->slot.f[15] == (500 << 16));
    CHECK(e0->slot.f[16] == (50 << 16));
    CHECK(e0 != nullptr);
    CHECK(e0->brain.f[AiBrain::kCurState] == 16); // GROUND_FOLLOWWP (patrol_on_spawn)
    CHECK(e0->brain.f[AiBrain::kWpType] == 1);
    CHECK(e0->brain.f[AiBrain::kWpChannel] == 1); // 1-based channel
    CHECK(e0->brain.f[AiBrain::kWpNode] == 0);
    CHECK(e0->brain.f[AiBrain::kSpeedB] == 20);
    CHECK(e0->team == 1);

    // Organic 1's ai_textfile resolved a profile: the CROSSED seeding —
    // kSpeedA <- combat 150 -> 150*65536/225 = 43690, kSpeedB <- patrol 70 ->
    // 70*65536/225 = 20388 [orig: Entity_InitVehicleAIFromDef @0x4688C7/@0x4688D3].
    AiEntity *e1 = ai.at(1);
    CHECK(e1 != nullptr);
    CHECK(e1->brain.f[AiBrain::kSpeedB] == 20388);
    CHECK(e1->brain.f[AiBrain::kSpeedA] == 43690);
    // The class-walk seed (D-AI-1): priorities copied verbatim; the +40..+52
    // order sorts priority-descending for the keyed GROUND type — 200 ground,
    // 100 organics, 10 air, 0 decorations [orig: AIProfile_LoadOrFind @0x45fd80].
    CHECK(e1->profile.type == 2);
    CHECK(e1->profile.class_priority[0] == 10);
    CHECK(e1->profile.class_priority[1] == 200);
    CHECK(e1->profile.class_priority[2] == 100);
    CHECK(e1->profile.class_priority[3] == 0);
    CHECK(e1->profile.slot_class[0] == 1); // ground first
    CHECK(e1->profile.slot_class[1] == 2); // organics
    CHECK(e1->profile.slot_class[2] == 0); // air
    CHECK(e1->profile.slot_class[3] == 3); // decorations last
    // Organic 0 resolved no profile: retail's memset-0 record — zero priorities,
    // tie order {3,2,1,0} (insertion-stable ascending, stored reversed).
    CHECK(e0->profile.class_priority[1] == 0);
    CHECK(e0->profile.slot_class[0] == 3);
    CHECK(e0->pos[0] == 0);               // spawned at origin
    CHECK(e0->net_id == 1);               // the AUTHORED record id, copied verbatim
    CHECK((e0->slot.f[1] & 0x209) == 0x209);
    CHECK(e0->see_all);
    CHECK((world.registry.get(world.registry.find_by_net_id(1))->engine_flags &
           0x40u) != 0);
    CHECK((world.registry.get(world.registry.find_by_net_id(1))->flags &
           0x40u) != 0);

    // entities carry their authored net ids; the registry resolves them (faithful
    // find_by_net_id over pools 0..3, markers included).
    CHECK(world.registry.find_by_net_id(1).valid());
    CHECK(world.registry.find_by_net_id(2).valid());  // second organic
    CHECK(world.registry.find_by_net_id(3).valid());  // building
    CHECK(world.registry.find_by_net_id(11).valid()); // marker 1 (pool 3)

    // ---- area-trigger zones are promoted into the registry's area table (array order) ----
    {
        bms::File ma{};
        bms::AreaTrigger zone{};
        zone.id = 5;                  // designer zone id (1..99); registered at array index 0
        zone.x_min = -10 << 16; zone.x_max = 10 << 16;
        zone.y_min = -10 << 16; zone.y_max = 10 << 16;
        zone.flags = 0;               // Z unbounded
        ma.area_triggers.push_back(zone);
        ma.organics.push_back(organic(0, 0, 0, 1, 0, 0));         // SSN 1, inside the zone
        ma.organics.push_back(organic(100 << 16, 0, 0, 1, 0, 0)); // SSN 2, outside
        ma.organics[0].id = 1;
        ma.organics[1].id = 2;
        World wz;
        AiSystem &aiz = wz.ai;
        mission::promote_mission(ma, wz);
        CHECK(wz.registry.area(0) != nullptr);  // the zone populated the table (was empty before)
        CHECK(wz.commands.ssn_in_area(1, 0));    // organic at origin is inside zone 0
        CHECK(!wz.commands.ssn_in_area(2, 0));   // organic at x=100 is outside
    }

    // ---- marker entity+0 radius overrides survive mission promotion ----
    // The 6006 hill proximity pass reads the marker entity's Q16 dword at +0,
    // not the +350 u16 used by numbered capture entities. Waypoint (6005) and
    // location (2044) markers share the same authored/default radius writer.
    // [orig: Entity_SpawnFromBMSRecord @0x40F05A..0x40F173 and
    // @0x40F213..0x40F227; Server_UpdateCaptureZoneProximity
    // @0x5089E8..0x508A68]
    {
        bms::File radii{};
        radii.markers.push_back(marker(0, 0, 0));
        radii.markers.back().type_id = 6006;
        radii.markers.back().wp_distance = 25;
        radii.markers.push_back(marker(100 << 16, 0, 0));
        radii.markers.back().type_id = 6005;
        radii.markers.push_back(marker(200 << 16, 0, 0));
        radii.markers.back().type_id = 2044;

        World radius_world;
        AiSystem &radius_ai = radius_world.ai;
        mission::promote_mission(radii, radius_world);
        std::array<float, 3> promoted_radii{};
        radius_world.registry.for_each([&](const Entity &entity) {
            if (entity.handle.pool() != 3) return;
            if (entity.item_id == 6006) promoted_radii[0] = entity.bound_radius;
            if (entity.item_id == 6005) promoted_radii[1] = entity.bound_radius;
            if (entity.item_id == 2044) promoted_radii[2] = entity.bound_radius;
        });
        CHECK(promoted_radii[0] == 25.0f);
        CHECK(promoted_radii[1] == 0.5f);
        CHECK(promoted_radii[2] == 0.5f);
    }

    // ---- the player waypoint track: the BLUE-flagged route + the marker fields ----
    // [orig: NetPacket_WriteWorldStateLoad0x0F @0x502e41 picks the first flags&2
    //  channel; Entity_SpawnFromBMSRecord @0x40f0aa seeds radius/name/link/chain]
    {
        bms::File wm{};
        wm.markers.push_back(marker(100 << 16, 0, 0));
        wm.markers.push_back(marker(200 << 16, 0, 0));
        wm.markers.push_back(marker(300 << 16, 0, 0));
        wm.markers[0].wp_distance = 25;             // authored radius -> 25<<16
        wm.markers[0].ttool_index = 4;              // STRWPNAME004
        wm.markers[1].wp_adv_trigger = 3;           // completes when event 3 fires
        wm.markers[1].bmsi_attributes = 1u << 22;   // chain-back
        // List 1: an AI patrol route (unflagged) — must NOT become the track.
        bms::WaypointRecord ai_route{};
        ai_route.flags = bms::WaypointFlags::None;
        ai_route.marker_count = 1;
        ai_route.waypoint_numbers = {2};
        // List 2: the blue player route. Slot-indexed like the .bms block.
        bms::WaypointRecord blue{};
        blue.flags = bms::WaypointFlags::BlueTeam;
        blue.marker_count = 2;
        blue.waypoint_numbers = {0, 1};
        wm.waypoint_records.resize(3);
        wm.waypoint_records[1] = ai_route;
        wm.waypoint_records[2] = blue;

        World ww;
        AiSystem &wai = ww.ai;
        mission::promote_mission(wm, ww);
        CHECK(ww.script.waypoints.entries.size() == 2);    // the blue route only
        CHECK(ww.script.waypoints.show);                    // visible by default
        CHECK(ww.script.waypoints.current == -1);           // no selection until the tick
        CHECK(ww.script.waypoints.entries[0].x == (100 << 16));
        CHECK(ww.script.waypoints.entries[0].radius == (25 << 16)); // authored wp_distance
        CHECK(ww.script.waypoints.entries[0].name_id == 4);
        CHECK(ww.script.waypoints.entries[1].radius == 0x8000);     // default 0.5 u
        CHECK(ww.script.waypoints.entries[1].linked_event == 3);
        CHECK(ww.script.waypoints.entries[1].chain_back);
        // The raw route-flags word rides the nav channel (bit1 = the blue mark).
        CHECK((wai.nav.channel(2)->loopflag &
               static_cast<int32_t>(bms::WaypointFlags::BlueTeam)) != 0);
    }

    // a non-routed entity option: with patrol_on_spawn=false the brain stays in state 0.
    {
        World w2;
        AiSystem &ai2 = w2.ai;
        mission::PromoteOptions o2;
        o2.patrol_on_spawn = false;
        mission::promote_mission(m, w2, o2);
        CHECK(ai2.at(0)->brain.f[AiBrain::kCurState] == 0); // faithful init, no auto-patrol
    }

    // ---- command 125: BMS wp_number is a target entity serial, not a path node ----
    // IDA proof: Entity_SpawnFromBMSRecord @0x40F02F copies record byte 0x4F to slot+148
    // and record dword 0x30 to slot+152; Entity_UpdateInfantryAI @0x4B9910 resolves
    // slot+152 against entity+124 and then uses Entity_FindBestSeatSlot @0x4351F0.
    // Boarders spawn ON FOOT and attach through the infantry think's board leg
    // (infantry_board.cpp) — there is no load-time mount shortcut, matching retail.
    // Drive the think for a few 16-tick boundaries to let the order land.
    auto run_ai = [](AiSystem &a, World &aw, int n) {
        TickContext c;
        c.world = &aw;
        c.is_authority = true;
        for (int t = 0; t < n; ++t) {
            c.logic_tick = static_cast<uint32_t>(t);
            a.tick(aw, c);
        }
    };
    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        // Authored AT the seat point (seat_local {1,2,3} off the vehicle at x=10):
        // arrival is immediate, the first think boards.
        cm.organics.push_back(organic(11 << 16, 2 << 16, 3 << 16, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat s{};
        s.type = SeatType::Passenger;
        s.seat_local = {1.f, 2.f, 3.f};
        seats.seats.push_back(s);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);

		const EntityHandle vh = cw.registry.find_by_net_id(11);
        const EntityHandle oh = cw.registry.find_by_net_id(1);
        Entity *veh = cw.registry.get(vh);
        Entity *occ = cw.registry.get(oh);
        CHECK(veh != nullptr);
        CHECK(occ != nullptr);
        CHECK(occ != nullptr && !occ->mounted); // promote stores the ORDER only
        run_ai(cai, cw, 46);
        CHECK(veh != nullptr && veh->seats.size() == 1);
        CHECK(occ->mounted);
        CHECK(occ->mount_target == vh);
        CHECK(veh->seats[0].occupant == oh);
        CHECK(occ->position.x == 11.f && occ->position.y == 2.f && occ->position.z == 3.f);
        CHECK(cai.at(0)->slot.f[37] == 125);
        CHECK(cai.at(0)->slot.f[38] == 11);
        CHECK(cai.at(0)->pos[0] == to_fixed(11.0));
        CHECK(cai.at(0)->pos[1] == to_fixed(2.0));
        CHECK(cai.at(0)->pos[2] == to_fixed(3.0));
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 100 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(0, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat s{};
        s.type = SeatType::Passenger;
        seats.seats.push_back(s);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);

		Entity *veh = cw.registry.get(cw.registry.find_by_net_id(11));
        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(veh != nullptr);
        CHECK(occ != nullptr);
        run_ai(cai, cw, 46); // 100u away, no gait clips: the order engages, no attach
        CHECK(occ != nullptr && !occ->mounted);
        CHECK(veh != nullptr && !veh->seats.empty());
        CHECK(veh != nullptr && !veh->seats[0].occupant.valid());
        CHECK(cai.at(0)->inf.move_mode == 3);       // the board walk is ORDERED
        CHECK(cai.at(0)->inf.at_final_oneshot);     // final-approach gait flag
        CHECK(cai.at(0)->pos[0] == 0);              // but no clips -> no displacement
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1420, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(11 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;
        cm.organics.push_back(organic(12 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[1].id = 2;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1420;
        Seat s0{};
        s0.type = SeatType::Passenger;
        Seat s1{};
        s1.type = SeatType::Passenger;
        s1.seat_local = {1.f, 0.f, 0.f};
        seats.seats.push_back(s0);
        seats.seats.push_back(s1);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);

		Entity *veh = cw.registry.get(cw.registry.find_by_net_id(11));
        Entity *o0 = cw.registry.get(cw.registry.find_by_net_id(1));
        Entity *o1 = cw.registry.get(cw.registry.find_by_net_id(2));
        CHECK(veh != nullptr);
        CHECK(o0 != nullptr && o1 != nullptr);
        run_ai(cai, cw, 46);
        CHECK(veh != nullptr && veh->seats.size() == 2);
        CHECK(o0 != nullptr && o0->mounted);
        CHECK(o1 != nullptr && o1->mounted);
        CHECK(veh->seats[0].occupant.valid());
        CHECK(veh->seats[1].occupant.valid());
        CHECK(veh->seats[0].occupant != veh->seats[1].occupant);
    }

    // ---- command 123/124/125 seat restrictions: sitex-only / no-ctrlx / any ----
    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        // Authored at the PASSENGER seat point (x=14): 123 must ignore the nearer
        // driver seat and take sitex.
        cm.organics.push_back(organic(14 << 16, 0, 0, 1, /*wp_id=*/123, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat passenger{};
        passenger.type = SeatType::Passenger;
        passenger.seat_local = {4.f, 0.f, 0.f};
        Seat driver{};
        driver.type = SeatType::Driver;
        driver.seat_local = {1.f, 0.f, 0.f};
        seats.seats.push_back(passenger);
        seats.seats.push_back(driver);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);
		run_ai(cai, cw, 46);

        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr);
        CHECK(occ != nullptr && occ->mounted);
        CHECK(occ != nullptr && occ->mount_type == SeatType::Passenger);
        CHECK(occ != nullptr && occ->position.x == 14.f);
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        // Authored at the DRIVER seat point (x=15): 124 rejects ctrlx but keeps
        // drvrx eligible.
        cm.organics.push_back(organic(15 << 16, 0, 0, 1, /*wp_id=*/124, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat controller{};
        controller.type = SeatType::Controller;
        controller.seat_local = {1.f, 0.f, 0.f};
        Seat driver{};
        driver.type = SeatType::Driver;
        driver.seat_local = {5.f, 0.f, 0.f};
        seats.seats.push_back(controller);
        seats.seats.push_back(driver);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);
		run_ai(cai, cw, 46);

        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr);
        CHECK(occ != nullptr && occ->mounted);
        CHECK(occ != nullptr && occ->mount_type == SeatType::Driver);
        CHECK(occ != nullptr && occ->position.x == 15.f);
    }

    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 10 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(10 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat passenger{};
        passenger.type = SeatType::Passenger;
        passenger.seat_local = {5.f, 0.f, 0.f};
        Seat driver{};
        driver.type = SeatType::Driver;
        driver.seat_local = {1.f, 0.f, 0.f};
        seats.seats.push_back(passenger);
        seats.seats.push_back(driver);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);
		run_ai(cai, cw, 46); // driver seat point 1u away: inside the 2u arrival ring

        Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr);
        CHECK(occ != nullptr && occ->mounted);
        CHECK(occ != nullptr && occ->mount_type == SeatType::Driver);
        CHECK(occ != nullptr && occ->position.x == 11.f);
    }

    // ---- items.def addeweap*: spawn every child and carry it on the parent frame ----
    test_emplacement_attachments();
    test_bms_record_index_is_pool_slot();
    test_friendly_tag_names_and_gather();
    test_emplacement_parent_death_cascades();
    test_unresolved_emplacement_preserves_streamed_pose();
    test_nameless_vehicle_takes_the_retail_default_profile();

    // ---- organics are routed through the INFANTRY motor with seeded slots ----
    // [orig: g_EntityClassPhysicsTable "org1" -> Entity_UpdateInfantryAI @0x4b9910;
    //  slot map from Entity_SpawnFromBMSRecord @0x40e9f0]
    CHECK(e0->inf.active);
    CHECK(e0->slot.f[35] == 1);   // has-route flag (slot+140)
    CHECK(e0->slot.f[37] == 1);   // channel = waypoint_id (slot+148)
    CHECK(e0->slot.f[38] == 0);   // start node = wp_number (slot+152)
    CHECK(e0->inf.body_heading == e0->heading); // spawns facing its authored heading

    // ---- end-to-end: the soldier WALKS its route on anim root motion ----
    // Synthetic clip source: gaits move 0.25u forward per tick, idles don't move.
    struct TestSource : IRootMotionSource {
        int32_t step;
        explicit TestSource(int32_t s) : step(s) {}
        static bool gait(int id) {
            return id == anim_state::kWalkForward || id == anim_state::kRunForward ||
                   id == anim_state::kJogForward;
        }
        bool has_clip(int /*adm_id*/, int id) const override {
            return gait(id) || id == anim_state::kIdle || id == anim_state::kStop;
        }
        int32_t clip_length_ticks(int, int, int /*variant*/) const override { return -1; }
        bool advance(int /*adm_id*/, int id, int32_t &phase, RootMotionFrame &out) override {
            if (!has_clip(0, id)) return false;
            ++phase;
            out = RootMotionFrame{};
            if (gait(id)) out.dx = step;
            return true;
        }
    };
    {
        TestSource src(0x4000); // 0.25u/tick forward
        ai.root_motion = &src;
        for (auto &n : ai.nav.nodes) n.f[0] = 5 << 16; // robust arrival window for the walk
        ai.nav.nodes[1].wait_ticks = 62;               // 1s authored hold at marker 1

        TickContext ctx;
        ctx.world = &world;
        ctx.is_authority = true;

        int max_node = 0;
        bool held = false, walked = false;
        for (int t = 0; t < 2600; ++t) {
            ctx.logic_tick = static_cast<uint32_t>(t); // the cadence gates key off this
            ai.tick(world, ctx);
            if (e0->slot.f[38] > max_node) max_node = e0->slot.f[38];
            if (e0->inf.wait_cooldown > 0) held = true;
            if (e0->inf.anim_state == anim_state::kWalkForward) walked = true;
        }
        CHECK(walked);                       // unalerted patrol uses the walk gait
        CHECK(max_node >= 2);                // arrived at markers and advanced the route
        CHECK(e0->pos[0] > (100 << 16));     // physically walked past marker 0
        CHECK(e0->pos[0] < (300 << 16));     // still on the route, not teleported
        CHECK(held);                         // honored marker 1's movetimer hold
        CHECK(!ai.relmat_calls.empty());     // arrival recorded the relation-matrix marks
        ai.root_motion = nullptr;
    }

    // ---- end-to-end: a command-123 boarder WALKS to the seat point and attaches ----
    // The whole chain on anim root motion: promote stores the order, the think
    // resolves the SSN and orders the walk, the motor covers ~20u, arrival runs
    // the filtered attach. [orig: Entity_UpdateInfantryAI @0x4b9910 board leg ->
    // Entity_FindBestSeatSlot @0x4351f0 -> Entity_RequestVehicleAttach @0x4364a0]
    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 30 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(10 << 16, 0, 0, 1, /*wp_id=*/123, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat s{};
        s.type = SeatType::Passenger;
        s.seat_local = {2.f, 0.f, 0.f}; // seat point at (32, 0, 0)
        seats.seats.push_back(s);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);
		Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr && !occ->mounted); // spawns ON FOOT — no load-time shortcut

        TestSource src(0x4000); // 0.25u/tick forward
        cai.root_motion = &src;
        TickContext c;
        c.world = &cw;
        c.is_authority = true;
        bool walked = false;
        for (int t = 0; t < 2600 && occ != nullptr && !occ->mounted; ++t) {
            c.logic_tick = static_cast<uint32_t>(t);
            cai.tick(cw, c);
            if (cai.at(0)->inf.anim_state == anim_state::kWalkForward) walked = true;
        }
        CHECK(walked);                                    // covered the ground on foot
        CHECK(occ != nullptr && occ->mounted);            // arrived and attached
        CHECK(occ != nullptr && occ->mount_type == SeatType::Passenger);
        CHECK(occ != nullptr && occ->position.x == 32.f); // posed at the seat point
        cai.root_motion = nullptr;
    }

    // ---- the blocked latch: a hull-stalled boarder still attaches ----
    // Interior seat points (helo cabins) leave the walker pressed against the
    // hull outside the 2u seat ring. Retail arms pad_368[1] from the collision
    // push and widens the ring to bound+1u; our latch arms on a stalled think.
    // Model the hull stall by cutting root motion once the walker is inside the
    // widened ring but outside the seat ring. [orig: @0x4b9910 push block +
    // the board leg's `pad_368[1] ? *target + 0x10000 : 0x20000` ring pick]
    {
        bms::File cm{};
        cm.items.push_back(item(/*type_id=*/1294, 30 << 16, 0, 0));
        cm.items[0].id = 11;
        cm.organics.push_back(organic(10 << 16, 0, 0, 1, /*wp_id=*/125, /*wp_num=*/11));
        cm.organics[0].id = 1;

        mission::PromoteOptions co{};
        mission::ItemSeatSpec seats{};
        seats.type_id = 1294;
        Seat s{};
        s.type = SeatType::Passenger;
        s.seat_local = {2.f, 0.f, 0.f}; // seat point at (32, 0, 0)
        seats.seats.push_back(s);
        co.item_seat_specs.push_back(seats);

        World cw;
        AiSystem &cai = cw.ai;
        mission::promote_mission(cm, cw, co);
		stamp_fixture_carrier_defs(cw);
		Entity *occ = cw.registry.get(cw.registry.find_by_net_id(1));
        CHECK(occ != nullptr && !occ->mounted);

        TestSource walk_src(0x4000);
        TestSource hull_src(0);     // "pressed against the hull": clips, no motion
        cai.root_motion = &walk_src;
        TickContext c;
        c.world = &cw;
        c.is_authority = true;
        bool cut = false;
        for (int t = 0; t < 2600 && occ != nullptr && !occ->mounted; ++t) {
            // Cut displacement once inside the widened 4u ring but still outside
            // the 2u seat ring (seat at x=32 -> cut past x=29).
            if (!cut && cai.at(0)->pos[0] > (29 << 16)) {
                cai.root_motion = &hull_src;
				// Inject the collision resolver's witnessed facing-push latch;
				// zero root motion alone no longer impersonates a hull contact.
				cai.at(0)->inf.board_blocked = true;
				cw.registry.get(cw.registry.find_by_net_id(11))->bound_radius = 3.0f;
				cut = true;
            }
            c.logic_tick = static_cast<uint32_t>(t);
            cai.tick(cw, c);
        }
        CHECK(cut);                              // the stall actually happened
        CHECK(occ != nullptr && occ->mounted);   // the latch widened the ring and boarded
        CHECK(occ != nullptr && occ->mount_type == SeatType::Passenger);
        cai.root_motion = nullptr;
    }

    if (failures == 0) std::printf("promote: all tests passed\n");
    return failures ? 1 : 0;
}
