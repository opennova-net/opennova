// Mission -> world promotion: a synthetic BMS mission is promoted into a live world +
// AI system, then the AI is ticked to prove the brains/nav are wired to the real mission
// data (entities patrol their authored routes). See engine/runtime/mission/promote.cpp.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <variant>

#include <runtime/mission/promote.h>
#include <runtime/world/ai.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/world.h>

#include "../common/synthetic_mission.h"

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

// The two embedder answers a placed item needs for a vehicle brain: a brain-class
// ai_function row (CHel/cpln or cveh/cbot/ctrn) and the AI-class def attrib
// 0x100000 [orig: Entity_SpawnFromBMSRecord @0x40ED4E; the class inits
// Entity_InitHelicopterAIFromDef @0x4683C0 / Entity_InitVehicleAIFromDef @0x4686C0].
static void brain_class_items(mission::PromoteOptions &options, std::vector<int32_t> types,
                              bool helicopter = false) {
    options.ai_profile_defaults = [types, helicopter](int32_t type) {
        mission::PromoteOptions::AiProfileDefaults d;
        for (int32_t t : types) {
            if (t == type) {
                d.known = true;
                d.helicopter_init = helicopter;
            }
        }
        return d;
    };
    options.item_attributes = [types](int32_t type) {
        for (int32_t t : types)
            if (t == type) return kItemAttribAIData;
        return 0u;
    };
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

using test_mission::item;

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
    // Each child's subType is its slot index; the refNum-less carrier took the
    // lowest free refNum for its group, and every child carries it, the
    // carrier's command group and its Flags.
    // [orig: Entity_SpawnWeaponOverlays @0x40F361..0x40F40E]
    CHECK(plain_child->sub_type == 0 && gun_child->sub_type == 1 &&
          crosshair_child->sub_type == 2);
    CHECK(parent->ref_num == 1);
    CHECK(plain_child->ref_num == 1 && gun_child->ref_num == 1 &&
          crosshair_child->ref_num == 1);
    // The children's records carry that refNum, so they join its group list;
    // the carrier the spawner handed it to does not.
    // [orig: Entity_SpawnWeaponOverlays @0x40F389 / @0x40F4C7;
    //  Entity_SpawnFromBMSRecord @0x40EC23..0x40EC45]
    CHECK(!parent->ref_group_member);
    CHECK(plain_child->ref_group_member && gun_child->ref_group_member &&
          crosshair_child->ref_group_member);
    CHECK(plain_child->group_id == parent->group_id);
    CHECK(gun_child->engine_flags == parent->engine_flags &&
          gun_child->flags == parent->flags);

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
    // The carrier's row destroyed: every child whose def carries EWeap goes with
    // it, as they share its refNum, through the same destroy and without a
    // notify of their own, and the gun's rider is let off. A child without
    // EWeap keeps its pointer to the freed row and holds its last pose while
    // the row is free; the next entity allocated in that row carries it.
    // [orig: Entity_Destroy @0x43E810 — the refNum walk @0x43E9B6..0x43E9CD ->
    //  CStreamingMem_Destroy @0x546F30 (member tests @0x546F8A..0x546FA0,
    //  Entity_Destroy @0x546FA3); Entity_UpdateTransformAndTurret @0x440CBF]
    parent->has_item_def = true;
    parent->item_type = 1;
    plain_child->has_item_def = true;
    gun_child->has_item_def = true;
    gun_child->item_attrib |= kItemAttribEweap;
    crosshair_child->has_item_def = true;
    crosshair_child->item_attrib |= kItemAttribEweap;
    const uint64_t old_parent_spawn_id = parent->registry_spawn_id;
    cw->out.entity_events.clear();
    CHECK(cw->commands.server_remove_and_notify(parent_h));
    int removal_events = 0;
    for (const auto &event : cw->out.entity_events)
        if (const auto *removal = std::get_if<EntityRemoveEvent>(&event)) {
            ++removal_events;
            CHECK(removal->handle == parent_h.packed);
        }
    CHECK(removal_events == 1);
    CHECK(cw->registry.get(gun_h) == nullptr);
    CHECK(cw->registry.get(crosshair_h) == nullptr);
    rider = cw->registry.get(rider_h);
    CHECK(rider != nullptr && !rider->mounted);
    cw->run_logic_tick();
    plain_child = cw->registry.get(plain_h);
    CHECK(plain_child != nullptr && plain_child->position.x == 30.f &&
          plain_child->position.y == 38.f && plain_child->position.z == 6.f);
    Entity replacement_seed{};
    replacement_seed.kind = EntityKind::Item;
    replacement_seed.item_id = 999;
    replacement_seed.position = {50.f, 60.f, 7.f};
    const EntityHandle replacement_h =
            cw->registry.spawn(1, replacement_seed);
    CHECK(replacement_h == parent_h);
    CHECK(cw->registry.get(replacement_h)->registry_spawn_id !=
          old_parent_spawn_id);
    cw->run_logic_tick();
    plain_child = cw->registry.get(plain_h);
    CHECK(plain_child != nullptr && plain_child->position.x == 52.f &&
          plain_child->position.y == 60.f && plain_child->position.z == 8.f);
    CHECK(cw->registry.get(replacement_h) != nullptr);
}

// The loader's addeweap pass walks pool 1 and then pool 2, each over the rows
// its count held before the pass: a building carrier's slot spawns its child
// into pool 1 (subType = its slot index), and a child's own slots are never
// walked, so no grandchild spawns. [orig: Mission_LoadBMSFile
//  @0x40FD49..0x40FD96; Entity_SpawnWeaponOverlays @0x40F300]
static void test_attachment_pass_walks_the_loaded_rows() {
    bms::File m{};
    m.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
    m.items[0].id = 11;
    bms::Entity bunker = item(/*type_id=*/700, 40 << 16, 50 << 16, 0);
    bunker.type = bms::ItemType::Building;
    bunker.id = 12;
    m.buildings.push_back(bunker);

    mission::PromoteOptions o{};
    mission::ItemSeatSpec tank{};
    tank.type_id = 164;
    mission::ItemEmplacementAttachmentSpec turret{};
    turret.child_type_id = 166;
    turret.stored_slot = 1;
    tank.emplacement_attachments.push_back(turret);
    mission::ItemSeatSpec nested{};
    nested.type_id = 166;
    mission::ItemEmplacementAttachmentSpec never{};
    never.child_type_id = 183;
    never.stored_slot = 1;
    nested.emplacement_attachments.push_back(never);
    mission::ItemSeatSpec building{};
    building.type_id = 700;
    mission::ItemEmplacementAttachmentSpec roof_gun{};
    roof_gun.child_type_id = 182;
    roof_gun.stored_slot = 2;
    building.emplacement_attachments.push_back(roof_gun);
    o.item_seat_specs = {tank, nested, building};

    auto w = std::make_unique<World>();
    mission::promote_mission(m, *w, o);
    int turrets = 0, grandchildren = 0;
    const Entity *roof = nullptr;
    w->registry.for_each([&](const Entity &e) {
        if (e.item_id == 166) ++turrets;
        if (e.item_id == 183) ++grandchildren;
        if (e.item_id == 182) roof = &e;
    });
    CHECK(turrets == 1);
    CHECK(grandchildren == 0);
    CHECK(roof != nullptr);
    if (roof == nullptr) return;
    const EntityHandle bunker_h = w->registry.find_by_net_id(12);
    CHECK(roof->handle.pool() == 1 && roof->emplacement_parent == bunker_h &&
          bunker_h.pool() == 2);
    CHECK(roof->sub_type == 1);
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static void test_emplacement_parent_death_keeps_children() {
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

    // The ordinary item-death path keeps the carrier entity resident as a husk,
    // and its attachments stay with it: no death leg in retail walks a dead
    // carrier's children (the ewep class update hides them on a dead
    // PlayerControl hull), and the rider keeps its seat.
    // [orig: Entity_UpdateTransformAndTurret @0x440CBF..0x440CE1;
    //  AI_TransitionToDeath_GroundVehicle @0x467B20 (only the Parent-gated
    //  list @0x467B90..0x467BCC)]
    parent->health = 0;
    destruction_notify_item_damage(*world, *parent, 2);
    CHECK(world->registry.get(parent_h) != nullptr);
    CHECK(!parent->alive);

    world->run_logic_tick();
    CHECK(world->registry.get(parent_h) != nullptr);
    CHECK(world->registry.get(child_h) != nullptr);
    CHECK(world->registry.get(grandchild_h) != nullptr);
    rider = world->registry.get(rider_h);
    CHECK(rider != nullptr && rider->mounted && rider->mount_target == grandchild_h);
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
    // Every row here is an AI-class def (attrib 0x100000); the crate row has no
    // brain class, so it still takes no brain.
    opts.item_attributes = [](int32_t) { return kItemAttribAIData; };
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
	// The .aip default_state (HELO_LAND here) is parsed but has NO reader in retail:
	// the allocator's three state words are the memset's zero read back
	// [orig: Entity_InitVehicleAI @0x460200 — `mov ebx,[esi+18h]` @0x46024b after the
	//  memset @0x460246, stores @0x46028b..0x460291]; the first aircraft mover tick
	//  promotes 0 -> 14 [orig: @0x490377..0x49037d] (teammate_spawn_test pins that leg).
	CHECK(air.brain.cur_state() == 0);
	CHECK(air.brain.f[AiBrain::kPendState] == 0 && air.brain.f[AiBrain::kFallback] == 0);
	// The allocator's constant block, in the witnessed order after the profile copy
	// [orig: @0x4602b8 sweep -196608, @0x4602c2 burst 0, @0x460345 f[137] 25.0 u,
	//  @0x460352 f[179] = entity Yaw, @0x460358 f[199] 0, @0x46035e..0x460371 f[200] =
	//  PRNG_Next16_C % 0x80000, @0x460377 f[201] 0].
	CHECK(air.brain.f[AiBrain::kSweepPhase] == -196608);
	CHECK(air.brain.f[AiBrain::kBurstWindow] == 0);
	CHECK(air.brain.f[137] == 1638400);
	CHECK(air.brain.f[179] == air.heading);
	CHECK(air.brain.f[199] == 0 && air.brain.f[201] == 0);
	{
		// Exactly ONE PRNG C draw per placed vehicle: f[200] is the fresh stream's
		// first value and the promoted world's next draw is the fresh stream's second.
		World fresh;
		CHECK(air.brain.f[200] == static_cast<int32_t>(fresh.next_prng16_c()) % 0x80000);
		CHECK(world.next_prng16_c() == fresh.next_prng16_c());
	}
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

// Every AI-class record gets its slot from the record, vehicles included: the
// respawn budget slot[18] = 62 * spawns (the mission start turns it into the
// spawn count with a /62) and the authored route slot[35/37/38] that the dead
// state's respawn restarts.
// [orig: Entity_SpawnFromBMSRecord @0x40ED4E..0x40F054 (slot+0x48 @0x40EFE4..0x40EFF4,
//  +0x8C/+0x94/+0x98 @0x40F02F..0x40F054); Game_StartMission @0x526079..0x52608F;
//  AI_TickState_VehicleDead @0x468005..0x46801D]
static void test_vehicle_records_seed_the_ai_slot() {
    int failures = 0;
    static constexpr int32_t kTruckType = 1237;
    bms::File m{};
    bms::Entity truck{};
    truck.type = bms::ItemType::Item;
    truck.type_id = kTruckType;
    truck.spawns = 3;
    truck.waypoint_id = 4;
    truck.wp_number = 2;
    m.items.push_back(truck);
    mission::PromoteOptions opts;
    mission::ItemSeatSpec spec;
    spec.type_id = kTruckType;
    Seat ctrl;
    ctrl.type = SeatType::Controller;
    spec.seats.push_back(ctrl);
    opts.item_seat_specs.push_back(spec);
    brain_class_items(opts, {kTruckType});
    World world;
    world.ai.is_authority = true;
    const mission::PromoteResult r = mission::promote_mission(m, world, opts);
    CHECK(r.brains == 1);
    if (world.ai.at(0) == nullptr) std::exit(1);
    const AiEntity &ai = *world.ai.at(0);
    CHECK(ai.slot.f[18] == 62 * 3);
    CHECK(ai.slot.f[35] == 1 && ai.slot.f[37] == 4 && ai.slot.f[38] == 2);
    // No .aip loaded: the loader still sorts the zeroed record's four keys
    // through the CRT qsort's shortsort [orig: AIProfile_LoadOrFind @0x45FECA].
    CHECK(ai.profile.slot_class[0] == 0 && ai.profile.slot_class[1] == 3 &&
          ai.profile.slot_class[2] == 2 && ai.profile.slot_class[3] == 1);
    VehicleTraits t;
    t.player_control = true;
    t.physics = 1;
    world.vehicles.traits.set(kTruckType, t);
    world.vehicles.initialize_mission_vehicles();
    CHECK(world.ai.at(0)->inf.wait_cooldown == 3);
    // The wreck's respawn restarts the authored route from those slot words,
    // over wherever the brain's route had got to.
    // [orig: AI_TickState_VehicleDead @0x468005..0x46801D]
    AiEntity &vai = *world.ai.at(0);
    Entity &veh = *world.registry.get(vai.handle);
    ItemDeathTraits death;
    death.static_death = true;
    world.tables.item_death_traits.set(kTruckType, death);
    veh.item_attrib |= kItemAttribPlayerControl;
    veh.health = 0;
    veh.alive = false;
    veh.flags = veh.engine_flags = kEntityFlagHusk | kEntityFlagDead;
    vai.brain.f[AiBrain::kCurState] = 23;
    vai.brain.f[AiBrain::kWpType] = 0;
    vai.brain.f[AiBrain::kWpChannel] = 9;
    vai.brain.f[AiBrain::kWpNode] = 7;
    for (int i = 0; i < 16; ++i)
        world.vehicles.tick_dead(veh, vai);
    CHECK(vai.inf.wait_cooldown == 2); // this tick respawned
    CHECK(vai.brain.f[AiBrain::kWpType] == 1);
    CHECK(vai.brain.f[AiBrain::kWpChannel] == 4 && vai.brain.f[AiBrain::kWpNode] == 2);
    if (failures)
        std::exit(1);
}

static void test_script_spatial_tables_are_promoted_and_replaced() {
    bms::File mission{};
    bms::AreaTrigger area{};
    area.id = 37; area.x_min = area.y_min = -(2 << 16);
    area.x_max = area.y_max = 2 << 16; area.z_max = 1 << 16;
    mission.area_triggers.push_back(area); // no constrain-Z bit
    bms::BoundingBox box{};
    box.type = 5; box.ref_id = 9;
    box.min_x = box.min_y = box.min_z = 2 << 16;
    box.max_x = box.max_y = box.max_z = -(2 << 16); // load normalizes
    mission.bounding_boxes.push_back(box);
    auto w = std::make_unique<World>();
    mission::promote_mission(mission, *w, {});
    const Area *promoted = w->registry.area(0);
    CHECK(promoted != nullptr && promoted->zone_id == 37);
    CHECK(promoted->bounds.max.z == bms::AreaTrigger::kUnboundedZMax);
    CHECK(promoted->script_bounds.max.z == 1);
    CHECK(w->registry.location_at({0, 0, 0}) == 9);
    CHECK(w->registry.location_at({2, 0, 0}) == 0);
    w->registry.intern_group("old_mission");
    mission::promote_mission(bms::File{}, *w, {});
    CHECK(w->registry.area(0) == nullptr);
    CHECK(w->registry.location_at({0, 0, 0}) == 0);
    CHECK(w->registry.script_group_index("old_mission") == -1);
    CHECK(w->registry.script_group_index("humans") == 1);
}

static void test_bms_admission_preserves_holes_and_signed_thresholds() {
    bms::File file;
    file.items.resize(9);
    for (size_t i = 0; i < file.items.size(); ++i) {
        file.items[i].type_id = 200 + static_cast<int32_t>(i);
        file.items[i].id = 100 + static_cast<int32_t>(i);
        file.items[i].team = 1;
    }
    file.items[1].bmsi_attributes = 0x10;
    file.items[1].no_less_than = 8;
    file.items[2].bmsi_attributes = 0x20;
    file.items[2].no_more_than = 8;
    file.items[3].bmsi_attributes = 0x40;
    file.items[4].bmsi_attributes = 0x80;
    file.items[5].team = 3;
    file.items[7].bmsi_attributes = 0x10;
    file.items[7].no_less_than = 255; // signed -1
    file.items[8].bmsi_attributes = 0x20;
    file.items[8].no_more_than = 255; // signed -1
    file.organics.resize(2);
    file.organics[0].type_id = 5305;
    file.organics[0].id = 201;
    file.organics[1].type_id = 5001;
    file.organics[1].id = 202;
    struct Case {
        bool session;
        int32_t limit;
        uint8_t teams;
        uint32_t game_type;
        std::array<bool, 9> admitted;
    };
    for (const auto &c : {
            Case{false, 8, 2, 0, {true,false,true,true,false,false,false,false,true}},
            Case{true, 8, 2, 0x10000, {true,true,true,false,true,false,false,true,false}},
            Case{true, 7, 4, 0x10001, {true,false,true,false,true,true,true,true,false}},
            Case{true, 9, 4, 0x10008, {true,true,false,false,true,true,true,true,false}},
            Case{true, 8, 4, 0x10004, {true,true,true,false,true,true,false,true,false}}}) {
        auto world = std::make_unique<World>();
        world->rules.mp_session = c.session;
        mission::PromoteOptions options;
        options.player_limit = c.limit;
        options.team_count = c.teams;
        options.game_type = c.game_type;
        options.item_attributes = [](int32_t type) { return type == 206 ? 0x10000u : 0u; };
        const auto result = mission::promote_mission(file, *world, options);
        // Organics: offline both records spawn; in a session the rejected
        // teammate leaves pool 0's used count at 1, so the accepted 5001 at
        // record index 1 sits outside the window and is never live.
        int accepted = c.session ? 0 : 2;
        for (size_t i = 0; i < c.admitted.size(); ++i) {
            const Entity *row = world->registry.get(EntityHandle::make(1, static_cast<int>(i)));
            CHECK((row != nullptr) == c.admitted[i]);
            if (row) {
                CHECK(row->net_id == 100 + i);
                ++accepted;
            }
        }
        CHECK(result.spawned == accepted);
        CHECK(result.dropped == static_cast<int>(file.items.size() + file.organics.size()) - accepted);
        const Entity *teammate = world->registry.get(EntityHandle::make(0, 0));
        CHECK((teammate != nullptr) == !c.session);
        if (teammate) {
            CHECK(teammate->item_id == 5000);
            CHECK(teammate->player_class == 1);
        }
        const Entity *next = world->registry.get(EntityHandle::make(0, 1));
        CHECK((next != nullptr) == !c.session);
        if (next) CHECK(next->net_id == 202);
        CHECK(file.organics[0].type_id == 5305); // the document remains authored
    }
    auto world = std::make_unique<World>();
    world->rules.teammates_disabled = true;
    mission::promote_mission(file, *world);
    CHECK(world->registry.get(EntityHandle::make(0, 0)) == nullptr);
}

// Pool 0's used count is the accepted count, not the record count: with one
// rejected organic in the middle, the record at the last index falls outside
// the window while the accepted record between the hole and it stays live.
// [orig: Mission_LoadBMSFile @0x40fb0d..0x40fb34]
static void test_bms_pool0_used_window_is_the_accepted_count() {
    bms::File file;
    file.organics.resize(4);
    for (size_t i = 0; i < file.organics.size(); ++i) {
        file.organics[i].type_id = 5001;
        file.organics[i].id = 301 + static_cast<int32_t>(i);
        file.organics[i].team = 1;
    }
    file.organics[1].bmsi_attributes = 0x40; // single-player only: rejected in a session
    auto world = std::make_unique<World>();
    world->rules.mp_session = true;
    mission::PromoteOptions options;
    options.player_limit = 8;
    const auto result = mission::promote_mission(file, *world, options);
    CHECK(result.spawned == 2 && result.dropped == 2 && result.brains == 2);
    CHECK(world->registry.get(EntityHandle::make(0, 0)) != nullptr);
    CHECK(world->registry.get(EntityHandle::make(0, 1)) == nullptr);
    const Entity *third = world->registry.get(EntityHandle::make(0, 2));
    CHECK(third != nullptr && third->net_id == 303);
    CHECK(world->registry.get(EntityHandle::make(0, 3)) == nullptr);
    CHECK(world->registry.by_net_id(304) == nullptr);
}

static void test_bms_admission_zeros_rejected_marker_projections() {
    bms::File file;
    file.markers.resize(2);
    file.markers[0].type_id = 2043;
    file.markers[0].bmsi_attributes = 0x80;
    file.markers[0].x = 100;
    file.markers[1].type_id = 2043;
    file.markers[1].x = 200;
    auto world = std::make_unique<World>();
    const auto result = mission::promote_mission(file, *world);
    CHECK(result.spawned == 1 && result.dropped == 1);
    CHECK(world->registry.get(EntityHandle::make(3, 0)) == nullptr);
    CHECK(world->registry.get(EntityHandle::make(3, 1)) != nullptr);
    CHECK(world->tables.map_grid_origin_present && world->tables.map_grid_origin_x == 200);
    CHECK(world->ai.nav.nodes.size() == 2);
    CHECK(world->ai.nav.nodes[0].f[0] == 0 && world->ai.nav.nodes[0].f[1] == 0);
    CHECK(world->ai.nav.nodes[1].f[1] == 200);
    CHECK(world->registry.get(EntityHandle::make(3, 1))->class_think_ticks == 0);
}

// The BMS AI-attribute fold inside the AI branch (the def carries 0x100000):
// every authored bit lands on its AiSlot[1] behavior bit or entity Flags bit,
// and 0x2000000 sets entity+0x2C bit 0x80 outside the branch. A record whose
// def is not AI-class takes only that outside bit.
// [orig: Entity_SpawnFromBMSRecord @0x40ED3B..0x40ED44, the AI gate @0x40ED4E,
//  the fold @0x40ED92..0x40EE94]
static void test_bms_ai_attribute_fold() {
    int failures = 0;
    bms::File m{};
    bms::Entity soldier = organic(0, 0, 0, /*team=*/1, /*wp_id=*/0, /*wp_num=*/0);
    soldier.id = 1;
    soldier.bmsi_attributes = 0x1u | 0x2u | 0x100u | 0x200u | 0x400u | 0x800u | 0x1000u |
            0x2000u | 0x4000u | 0x8000u | 0x10000u | 0x40000u | 0x80000u | 0x100000u |
            0x2000000u;
    m.organics.push_back(soldier);
    bms::Entity heli = item(1307, 0, 0, 0);
    heli.id = 2;
    heli.bmsi_attributes = 0x20000u; // EngineRunning
    m.items.push_back(heli);
    bms::Entity crate = item(900, 10 << 16, 0, 0);
    crate.id = 3;
    crate.bmsi_attributes = 0x2u | 0x4000u | 0x2000000u;
    m.items.push_back(crate);
    mission::PromoteOptions opts;
    opts.item_attributes = [](int32_t type) { return type == 1307 ? kItemAttribAIData : 0u; };
    World world;
    mission::promote_mission(m, world, opts);

    const Entity *s = world.registry.get(world.registry.find_by_net_id(1));
    const AiEntity *sai = s != nullptr ? world.ai.for_handle(s->handle) : nullptr;
    CHECK(s != nullptr && sai != nullptr);
    if (s == nullptr || sai == nullptr) std::exit(1);
    CHECK(static_cast<uint32_t>(sai->slot.f[1]) ==
          (0x1u | 0x2000u | 0x10000u | 0x100u | 0x200u | 0x400u | 0x800u | 0x8000u | 0x8u |
           0x80000u | 0x100000u | 0x200000u));
    CHECK((s->flags & kEntityFlagMounted) != 0 && (s->engine_flags & kEntityFlagMounted) != 0);
    CHECK((s->flags & kEntityFlagAiClimb) != 0 && (s->engine_flags & kEntityFlagAiClimb) != 0);
    CHECK((s->cause_flags & 0x80u) != 0);

    const Entity *h = world.registry.get(world.registry.find_by_net_id(2));
    CHECK(h != nullptr);
    if (h != nullptr) {
        CHECK((h->flags & 0x80u) != 0);
        CHECK(h->veh.speed == 0x10000);
        CHECK(h->veh.part_spin.speed != 0);
    }

    const Entity *c = world.registry.get(world.registry.find_by_net_id(3));
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK(((c->flags | c->engine_flags) & (kEntityFlagMounted | kEntityFlagAiClimb)) == 0);
        CHECK((c->cause_flags & 0x80u) != 0);
    }
    if (failures)
        std::exit(1);
}

// A placed item takes a vehicle brain exactly when its def carries the AI-class
// attrib AND its ai_function row is a brain class; authoring a control seat is
// not the test. [orig: Entity_SpawnFromBMSRecord @0x40ED4E; the pool-1 class init
// Entity_InitAllFromModels @0x40E5B8..0x40E5D8]
static void test_item_brain_follows_class_and_attrib() {
    int failures = 0;
    bms::File m{};
    m.items.push_back(item(1237, 0, 0, 0));        // cveh row + attrib: a brain, no seats
    m.items.push_back(item(1300, 20 << 16, 0, 0)); // a control seat, no brain row
    m.items.push_back(item(1301, 40 << 16, 0, 0)); // a brain row without the attrib
    mission::PromoteOptions opts;
    mission::ItemSeatSpec spec;
    spec.type_id = 1300;
    Seat ctrl;
    ctrl.type = SeatType::Controller;
    spec.seats.push_back(ctrl);
    opts.item_seat_specs.push_back(spec);
    opts.ai_profile_defaults = [](int32_t type) {
        mission::PromoteOptions::AiProfileDefaults d;
        d.known = type == 1237 || type == 1301;
        return d;
    };
    opts.item_attributes = [](int32_t type) { return type == 1237 ? kItemAttribAIData : 0u; };
    World world;
    const mission::PromoteResult r = mission::promote_mission(m, world, opts);
    CHECK(r.brains == 1);
    CHECK(world.ai.for_handle(EntityHandle::make(1, 0)) != nullptr);
    CHECK(world.ai.for_handle(EntityHandle::make(1, 1)) == nullptr);
    CHECK(world.ai.for_handle(EntityHandle::make(1, 2)) == nullptr);
    if (failures)
        std::exit(1);
}

int main() {
    test_item_brain_follows_class_and_attrib();
    test_bms_ai_attribute_fold();
    test_bms_admission_preserves_holes_and_signed_thresholds();
    test_bms_pool0_used_window_is_the_accepted_count();
    test_bms_admission_zeros_rejected_marker_projections();
    test_script_spatial_tables_are_promoted_and_replaced();
    // A synthetic mission: 3 markers forming a path, 1 looping waypoint record (channel 0),
    // 2 organics on that route (teams 1/2), 1 building.
    bms::File m{};
    m.markers.push_back(marker(100 << 16, 0, 0));
    m.markers.push_back(marker(200 << 16, 0, 0));
    m.markers.push_back(marker(300 << 16, 0, 0));
    // Route nodes are "waypoint" markers (items.def 106005): only that type (and
    // 6006/2044) carries the wp_distance arrival radius [orig:
    // Entity_SpawnFromBMSRecord `cmp dword ptr [edi],1775h` @0x40F05A].
    for (bms::Entity &mk : m.markers) mk.type_id = 6005;

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
    // Organic 1 authors an ai_textfile whose .aip the embedder resolved: its
    // profile fields and class walk seed; an organic runs no vehicle class init,
    // so neither organic takes brain speed words.
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
    m.organics[0].group_id = 5;
    m.buildings[0].id = 3;
    m.markers[0].id = 10;
    m.markers[1].id = 11;
    m.markers[2].id = 12;

    World world;
    AiSystem &ai = world.ai;
    ai.is_authority = true;

    mission::PromoteOptions opts;
    opts.arrival_radius = 1000;
    mission::PromoteOptions::AiProfileRow zode;
    zode.profile = "d_zode";
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

    // organic 0 carries its route + spawn transform; its brain starts in state 0.
    AiEntity *e0 = ai.at(0);
    // The record's engagement distances are SLOT words, shifted to 16.16; an
    // unresolved .aip leaves the profile's range words at the memset-0 record's
    // zero [orig: AIProfile_LoadOrFind @0x45fd80; Entity_SpawnFromBMSRecord slot
    // fills @0x40ED61..0x40F054].
    CHECK(e0->profile.range_primary == 0);
    CHECK(e0->profile.range_secondary == 0);
    CHECK(e0->slot.f[15] == (500 << 16));
    CHECK(e0->slot.f[16] == (50 << 16));
    CHECK(e0 != nullptr);
    // State 0 at spawn: retail organics carry no vehicle brain at all (the AiSlot drives
    // the infantry motor), and the former patrol_on_spawn 16 seed had no witness.
    CHECK(e0->brain.f[AiBrain::kCurState] == 0);
    CHECK(e0->brain.f[AiBrain::kPendState] == 0 && e0->brain.f[AiBrain::kFallback] == 0);
    CHECK(e0->brain.f[AiBrain::kWpType] == 1);
    CHECK(e0->brain.f[AiBrain::kWpChannel] == 1); // 1-based channel
    CHECK(e0->brain.f[AiBrain::kWpNode] == 0);
    CHECK(e0->brain.f[AiBrain::kSpeedB] == 0); // no class init, no stand-in speed
    CHECK(e0->team == 1);

    // Organic 1's ai_textfile resolved a profile; the speed words stay a vehicle
    // class init's (ai_brain_rows pins the crossed +0xC4/+0xC0 seeding).
    AiEntity *e1 = ai.at(1);
    CHECK(e1 != nullptr);
    CHECK(e1->brain.f[AiBrain::kSpeedA] == 0 && e1->brain.f[AiBrain::kSpeedB] == 0);
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
    // Organic 0 resolved no profile and owns no vehicle brain, so nothing sorts
    // its walk: zero priorities, the untouched default order. (A profile-less
    // VEHICLE brain sorts the zeroed record's keys to {0,3,2,1}: ai_brain_rows.)
    CHECK(e0->profile.class_priority[1] == 0);
    CHECK(e0->profile.slot_class[0] == 3);
    CHECK(e0->pos[0] == 0);               // spawned at origin
    CHECK(e0->net_id == 1);               // the AUTHORED record id, copied verbatim
    // The relation group key is the record's command group, not its SSN
    // [orig: Entity_SpawnFromBMSRecord @0x40EBB3..0x40EBB7 -> entity+0x11C].
    CHECK(e0->relmat_id == 5);
    CHECK((e0->slot.f[1] & 0x209) == 0x209);
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

    // ---- command 125: BMS wp_number is a target entity serial, not a path node ----
    // IDA proof: Entity_SpawnFromBMSRecord @0x40F02F copies record byte 0x4F to slot+148
    // and record dword 0x30 to slot+152; Entity_UpdateInfantryAI @0x4B9910 resolves
    // slot+152 against entity+124 and then uses Entity_FindBestSeatSlot @0x4351F0.
    // Boarders spawn ON FOOT and attach through the infantry think's board leg
    // (infantry_board.cpp) — there is no load-time mount shortcut, matching retail.
    // Drive the think for a few 16-tick boundaries to let the order land.
    auto run_ai = [](AiSystem &, World &aw, int n) {
        TickContext c;
        c.world = &aw;
        c.is_authority = true;
        for (int t = 0; t < n; ++t) {
            c.logic_tick = static_cast<uint32_t>(t);
            aw.update_all_entities(c);
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
    test_attachment_pass_walks_the_loaded_rows();
    test_bms_record_index_is_pool_slot();
    test_friendly_tag_names_and_gather();
    test_emplacement_parent_death_keeps_children();
    test_unresolved_emplacement_preserves_streamed_pose();
    test_nameless_vehicle_takes_the_retail_default_profile();
    test_vehicle_records_seed_the_ai_slot();

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
            world.update_all_entities(ctx);
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
            cw.update_all_entities(c);
            if (cai.at(0)->inf.anim_state == anim_state::kWalkForward) walked = true;
        }
        CHECK(walked);                                    // covered the ground on foot
        CHECK(occ != nullptr && occ->mounted);            // arrived and attached
        CHECK(occ != nullptr && occ->mount_type == SeatType::Passenger);
        // The pass that boards ends on foot: the org1 motor keys its seat block
        // on the mounted-live local its head took, so the next pass poses the
        // seat. [orig: Entity_UpdateInfantryAI @0x4B9960..0x4B9985, the seat
        //  block's test @0x4BE8F0]
        CHECK(occ != nullptr && occ->position.x != 32.f);
        c.logic_tick += 1;
        cw.update_all_entities(c);
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
				cai.at(0)->inf.path_state = 1;
				cw.registry.get(cw.registry.find_by_net_id(11))->bound_radius = 3.0f;
				cut = true;
            }
            c.logic_tick = static_cast<uint32_t>(t);
            cw.update_all_entities(c);
        }
        CHECK(cut);                              // the stall actually happened
        CHECK(occ != nullptr && occ->mounted);   // the latch widened the ring and boarded
        CHECK(occ != nullptr && occ->mount_type == SeatType::Passenger);
        cai.root_motion = nullptr;
    }

    if (failures == 0) std::printf("promote: all tests passed\n");
    return failures ? 1 : 0;
}
