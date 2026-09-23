// The global hit record and the script kills that read it [orig: hitRecord
// @0xB7C620 via Projectile_GetHitRecord @0x4E7000]. The BMS kills clear only
// the damage word (and the owner on a KillSingle pool-0 row or any KillGroup
// row), so a person's class callback still runs the round legs of the last
// recorded round; WAC killSSN zeroes the record first. Items read the record's
// section, and a brained item's machine its owner word.
// [orig: Entity_KillByNetId @0x43DBD0; Entity_KillAllByNetId @0x43C8E0;
//  Entity_ResetWeaponState @0x4F1E40; Entity_HandleDamageTrigger @0x407310]
#include <cstdio>
#include <memory>
#include <variant>

#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/angle.h>
#include <runtime/world/destruction.h>
#include <runtime/world/infantry.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct KillRig {
    std::unique_ptr<World> w = std::make_unique<World>();
    EntityHandle shooter;
    EntityHandle npc;

    KillRig() {
        w->registry.configure_pool(0, 8);
        w->registry.configure_pool(1, 8);
        w->ai.is_authority = true;
        AmmoTableEntry ammo;
        ammo.name = "BULLET";
        ammo.valid = true;
        w->tables.ammo.entries.push_back(ammo);
        Entity s;
        s.kind = EntityKind::Organic;
        s.item_type = 3;
        s.health = 100;
        s.net_id = 50;
        s.team = 1;
        shooter = w->registry.spawn(0, s);
        Entity n;
        n.kind = EntityKind::Organic;
        n.item_type = 3;
        n.has_item_def = true;
        n.item_type_index = 3;
        n.health = 100;
        n.health_max = 100;
        n.alive = true;
        n.net_id = 60;
        n.team = 2;
        n.group_id = 4;
        n.yaw = 90; // engine heading 0: a +X round arrives from behind
        npc = w->registry.spawn(0, n);
        w->ai.at(w->ai.attach(npc))->inf.active = true;
    }

    Entity &victim() { return *w->registry.get(npc); }

    // The last round the damage pass recorded: it struck `section`, flying +X.
    void record_round(int32_t section) {
        HitRecord &r = w->round_sim.hit_record;
        r = HitRecord{};
        r.has_round = true;
        r.round_pos = Vec3{-5.0f, 0.0f, 1.0f};
        r.round_vel_q16 = FixedVec3{10 << 16, 0, 0};
        r.round_ammo_index = 0;
        r.round_owner = shooter;
        r.owner = shooter;
        r.damage = 40;
        r.section = section;
        r.target = npc;
    }

    int pieces() const {
        int n = 0;
        w->registry.for_each_in_pool(0, [&](const Entity &e) {
            if (e.dismemberment_piece) ++n;
        });
        return n;
    }
};

// The rear quadrant a +X round gives a heading-0 body.
int rear_quadrant() {
    return death_quadrant_from_round(bam_heading_from_mission_yaw_deg(90.0), 10.0f, 0.0f);
}

} // namespace

// A KillSingle clears the record's damage and (pool 0) owner words and then
// runs the org callback's round legs for the round still recorded: the clip
// from its section and quadrant, the torso roll, the damage reaction toward
// its owner and, on the authority, the dismemberment cut.
// [orig: Entity_KillByNetId +0x44 @0x43DC22, +0x30 @0x43DC27, the callback
//  @0x43DC2A..0x43DC31; Entity_HandleDamageTrigger @0x40740D..0x4076C9]
static void test_kill_single_runs_the_recorded_round_legs() {
    KillRig rig;
    rig.record_round(3);
    CHECK(rig.w->commands.kill_ssn(rig.npc));
    const HitRecord &r = rig.w->round_sim.hit_record;
    CHECK(r.has_round && r.section == 3 && r.round_owner == rig.shooter);
    CHECK(r.damage == 0 && !r.owner.valid());
    CHECK(rear_quadrant() == 2);
    CHECK(rig.victim().death_anim_state ==
          compute_death_anim_state(3, 2, death_cause::kBullet));
    CHECK(rig.w->ai.for_handle(rig.npc)->roll == -0x05B05B00);
    CHECK(rig.w->round_sim.hits.size() == 1);
    if (rig.w->round_sim.hits.size() == 1) {
        CHECK(rig.w->round_sim.hits[0].victim == rig.npc);
        CHECK(rig.w->round_sim.hits[0].shooter == rig.shooter);
    }
    CHECK(rig.pieces() == 1);
    CHECK((rig.victim().section_mask & (1u << 3)) != 0);
}

// WAC killSSN zeroes the whole record before its callback: no round legs.
// [orig: Entity_ResetWeaponState memset @0x4F1E8F..0x4F1E99, the callback
//  @0x4F1EC7..0x4F1ED2; the round test @0x40740D]
static void test_wac_kill_ssn_zeroes_the_record() {
    KillRig rig;
    rig.record_round(3);
    CHECK(rig.w->commands.wac_kill_ssn(rig.npc));
    const HitRecord &r = rig.w->round_sim.hit_record;
    CHECK(!r.has_round && r.section == 0 && !r.round_owner.valid());
    CHECK(rig.victim().death_anim_state == 0);
    CHECK(rig.w->ai.for_handle(rig.npc)->roll == 0);
    CHECK(rig.w->round_sim.hits.empty());
    CHECK(rig.pieces() == 0);
}

// A KillGroup row clears the record's damage and owner, keeps the victim's own
// attacker, and still runs the recorded round's legs; section 0 (a terrain
// stop's record) selects the clip but cuts nothing.
// [orig: Entity_KillAllByNetId @0x43C980 / @0x43C983, the callback
//  @0x43C986..0x43C98F; the section gate @0x4075B4]
static void test_kill_group_runs_the_round_and_keeps_the_attacker() {
    KillRig rig;
    rig.victim().last_attacker = rig.shooter;
    rig.record_round(0);
    CHECK(rig.w->commands.kill_group(4) == 1);
    const HitRecord &r = rig.w->round_sim.hit_record;
    CHECK(r.has_round && r.damage == 0 && !r.owner.valid());
    CHECK(rig.victim().last_attacker == rig.shooter);
    CHECK(rig.victim().death_anim_state ==
          compute_death_anim_state(0, 2, death_cause::kBullet));
    CHECK(rig.w->round_sim.hits.size() == 1);
    CHECK(rig.pieces() == 0);
}

// An item's callback reads the record's section, and a brained item's machine
// queues the record's owner word, which a KillSingle item row keeps.
// [orig: Entity_KillByNetId pool 1 +0x30 only @0x43DC68, the callback
//  @0x43DC6C..0x43DC72; the gnrc row's callback (row @0x8130C0) reads
//  hitRecord[14] @0x407073 for its state send @0x40708E;
//  EntityAI_ProcessInfantryStateMachine `mov ecx,[eax+44h]` @0x45831E]
static void test_kill_single_hands_items_the_record() {
    KillRig rig;
    rig.w->rules.mp_session = true;
    rig.w->rules.logic_authority = true;
    ItemDeathTraits traits;
    traits.death_class = ItemDeathClass::kGnrc;
    rig.w->tables.item_death_traits.set(900, traits);
    Entity tank;
    tank.kind = EntityKind::Item;
    tank.item_id = 900;
    tank.has_item_def = true;
    tank.item_type_index = 5;
    tank.net_id = 70;
    tank.engine_flags = kEntityFlagDead; // killed earlier, not yet husked
    const EntityHandle item = rig.w->registry.spawn(1, tank);
    rig.record_round(7);
    CHECK(rig.w->commands.kill_ssn(item));
    CHECK(rig.w->round_sim.hit_record.owner == rig.shooter);
    int sections = 0;
    for (const auto &event : rig.w->out.entity_events)
        if (const auto *state = std::get_if<ItemStateEvent>(&event))
            if (state->handle == item.packed && state->section == 7) ++sections;
    CHECK(sections == 1);

    Entity hull;
    hull.kind = EntityKind::Item;
    hull.item_id = 901;
    hull.has_item_def = true;
    hull.item_type_index = 6;
    hull.is_ai_capable = true;
    hull.health = 400;
    hull.alive = true;
    hull.net_id = 71;
    const EntityHandle brained = rig.w->registry.spawn(1, hull);
    rig.w->ai.attach(brained);
    // An ai_function CHel row: its class event is the air machine. Without a
    // brain-class row the class event runs no machine.
    // [orig: g_EntityClassEventCallbackTable CHel @0x8132A0 ->
    //  EntityAI_ProcessInfantryStateMachine @0x4581B0]
    VehicleTraits air;
    air.brain_class = VehicleBrainClass::Air;
    rig.w->vehicles.traits.set(901, air);
    CHECK(rig.w->commands.kill_ssn(brained));
    CHECK(rig.w->ai.events.count() == 1);
    if (rig.w->ai.events.count() == 1) {
        CHECK(rig.w->ai.events.at(0).type() == 1);
        CHECK(rig.w->ai.events.at(0).f[3] == int32_t(rig.shooter.packed) + 1);
    }
}

int main() {
    test_kill_single_runs_the_recorded_round_legs();
    test_wac_kill_ssn_zeroes_the_record();
    test_kill_group_runs_the_round_and_keeps_the_attacker();
    test_kill_single_hands_items_the_record();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("hit_record: all passed\n");
    return 0;
}
