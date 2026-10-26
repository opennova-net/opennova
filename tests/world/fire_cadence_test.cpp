// The local fire cadence on the wire (D-NET-NEW-L7-8): how many rounds a held trigger
// fires and the mode byte each S2C 0x0A round event carries.
//
// The rows are the shipped M4AUTO's (fixtures/def/weapon.def: flags auto, ammobucket 1,
// action "fire" delayend 3, action "recoil" delayend 2). The held trigger refires through
// the recoil's deferred re-queue (counter <= 1, the binding active) and the next tick's
// dispatch, which requests fire only while the binding is still active; the mode byte's
// high bits are the slot's own word +0x10, which only a bucketless weapon writes.
// [orig: WeaponAction_Recoil @0x542DD0 (the re-queue @0x542E7F..0x542E9D);
//  Input_ProcessFrame @0x49D520 (Input_FlushDeferredEvents @0x49D591, the dispatch
//  @0x49D5D8); Input_HandleActionBinding_0 @0x4E0420 (Input_IsBindingActive(149)
//  @0x4E07D9); WeaponAction_Fire @0x542B10 (the mode byte @0x542BFC..0x542C11);
//  Weapon_ConsumeAmmo @0x540850 (the slot word @0x540907..0x540913)]
#include <cstdio>
#include <memory>
#include <string>
#include <formats/def/def.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::def;

namespace {

int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

void set_row(WeaponFsmActionRow &row, const char *name, int32_t ds, int32_t de) {
    std::snprintf(row.name, sizeof(row.name), "%s", name);
    row.delaystart = ds;
    row.delayend = de;
}

float no_clip_seconds(void *, const char *) { return 0.0f; }

// One tick as the game runs it: the deferred fire record the slot's last tick queued (its
// refire_queued, Input_QueueDeferredEvent(149): the recoil window's re-queue or a mid-FIRE
// request's) dispatches at this tick's input frame, where case 0x95's press leg requests fire
// only while the binding is still down; the FSM consumes no re-queue of its own.
// [orig: Input_FlushDeferredEvents @0x497AC0 from Input_ProcessFrame @0x49D591;
//  Input_HandleActionBinding_0 case 0x95 - Input_IsBindingActive(0x95) @0x4E07D9]
void game_tick(const WeaponFsmDef &def, WeaponSlotState &s, WeaponFsmInputs in,
               WeaponFsmEvents &ev) {
    in.fire_pressed = in.fire_pressed || (s.refire_queued && in.fire_held);
    weapon_fsm_tick(def, s, in, ev);
}
int no_clip(void *, const char *) { return 0; }

// The M4AUTO's FSM rows: idle, fire {0, 3}, recoil {0, 2}, reload, empty.
WeaponFsmDef m4auto_def() {
    WeaponFsmActionRow rows[5];
    set_row(rows[0], "idle", 0, -1);
    set_row(rows[1], "fire", 0, 3);
    set_row(rows[2], "recoil", 0, 2);
    set_row(rows[3], "reload", 0, -1);
    set_row(rows[4], "empty", 0, -1);
    WeaponFsmDef def;
    weapon_fsm_bake(rows, 5, no_clip, no_clip_seconds, nullptr, def);
    def.auto_fire = true;
    def.clip_capacity = 30;
    return def;
}

// One press held `hold` ticks (down on tick 0, up on tick `hold`), the FSM alone.
int fsm_shots_for_hold(int hold) {
    const WeaponFsmDef def = m4auto_def();
    WeaponSlotState s;
    s.clip = 30;
    s.reserve = 300;
    int fired = 0;
    for (int t = 0; t < 60; ++t) {
        WeaponFsmInputs in;
        in.is_local = true;
        in.fire_pressed = t == 0;
        in.fire_held = t < hold;
        WeaponFsmEvents ev;
        game_tick(def, s, in, ev);
        if (ev.fired) ++fired;
    }
    return fired;
}

// The cycle is seven ticks: the shot, the fire row's three, the recoil's arbiter and
// its two, the re-queue on the recoil's counter-1 tick (5) and the dispatch on the
// next (6), which fires on the tick after (7) only if the binding is still down at 6.
void test_hold_lengths() {
    CHECK(fsm_shots_for_hold(1) == 1);
    CHECK(fsm_shots_for_hold(5) == 1);  // up before the re-queue: none queued
    CHECK(fsm_shots_for_hold(6) == 1);  // queued at 5, dropped at the dispatch on 6
    CHECK(fsm_shots_for_hold(7) == 2);  // still down at the dispatch: the refire
    CHECK(fsm_shots_for_hold(13) == 2); // the second cycle's dispatch on 13 is up
    CHECK(fsm_shots_for_hold(14) == 3);
    std::printf("PASS a held M4AUTO refires only while the binding is down at the dispatch\n");
}

// An auto weapon's fire press landing inside the FIRE window re-queues every tick
// while the trigger stays down and is dropped at the first dispatch after its release;
// held into the recoil, the recoil's dispatch banks one shot. [orig:
// WeaponSlot_RequestFire @0x53EFA0 (the auto leg's FIRE case, Input_QueueDeferredEvent
// @0x53EFFD)]
void test_mid_fire_press_banks_only_while_held() {
    for (const int release : {4, 8}) {
        const WeaponFsmDef def = m4auto_def();
        WeaponSlotState s;
        s.clip = 30;
        s.reserve = 300;
        int fired = 0;
        for (int t = 0; t < 60; ++t) {
            WeaponFsmInputs in;
            in.is_local = true;
            in.fire_pressed = t == 0 || t == 2;
            in.fire_held = t == 0 || (t >= 2 && t < release);
            WeaponFsmEvents ev;
            game_tick(def, s, in, ev);
            if (ev.fired) ++fired;
        }
        // Released on tick 4 (still inside FIRE): the dispatch on 4 drops it, and the
        // recoil's own re-queue (tick 5) finds the trigger up. Held to tick 8: the
        // recoil's dispatch (tick 5) is down and banks a shot (fired on 7); the next
        // cycle's re-queue (12) finds it up.
        CHECK(fired == (release == 4 ? 1 : 2));
    }
    std::printf("PASS a mid-FIRE press banks a shot only while the trigger stays down\n");
}

// The mode byte reads the slot's own word: a bucket weapon's is never written (0), a
// bucketless weapon's is its clip before the spend.
void test_mode_word() {
    const WeaponFsmDef def = m4auto_def();
    for (const bool bucket : {true, false}) {
        WeaponSlotState s;
        s.clip = 30;
        s.reserve = 300;
        s.shared_clip = bucket;
        WeaponFsmInputs in;
        in.is_local = true;
        in.fire_pressed = true;
        in.fire_held = true;
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        CHECK(ev.fired);
        CHECK(ev.fired_clip_before_consume == (bucket ? 0 : 30));
        CHECK(s.clip == 29); // the loaded rounds still spend (the bucket's, or the word)
    }
    std::printf("PASS the mode byte reads the slot's own word\n");
}

// The authority's own fire through the local pump: the ring records (the S2C 0x0A round
// events) of the shipped M4AUTO, a bucket rifle, carry the mode byte 0x02 on every shot,
// and a six-tick hold appends one round, a seven-tick hold two.
struct Rig {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    LocalPlayer local{world};
    WeaponInventory inv;
    EntityHandle shooter;
    int m4 = 0;
    // The deferred fire record the last pump queued (LocalWeaponPumpIO::fire_deferred).
    bool deferred = false;

    explicit Rig(bool bucket) {
        world.registry.configure_pool(0, 4);
        world.registry.configure_pool(1, 4);
        WeaponTable &t = world.tables.weapons;
        t.ammo_class_names.emplace_back("");
        t.ammo_class_caps.push_back(0);
        t.ammo_class_names.emplace_back("CLASS_556MM");
        t.ammo_class_caps.push_back(1000);
        WeaponTableEntry null_e;
        null_e.name = "null";
        null_e.valid = true;
        t.entries.push_back(null_e);
        WeaponTableEntry e;
        e.name = "WPN_M4AUTO";
        e.valid = true;
        e.category = 3;
        e.rank = 0;
        e.clipsize = 30;
        e.startrounds = 300;
        e.maxclips = 10;
        e.ammo_class = "CLASS_556MM";
        e.ammo_class_id = 1;
        e.ammo_class_count = 1;
        e.ammo_bucket = bucket ? 1 : 0;
        e.ammo_index = 1;
        e.flags = static_cast<int32_t>(DEF_WEAPON_FLAG_AUTO);
        t.entries.push_back(e);
        m4 = 1;
        world.tables.ammo.entries.resize(2);
        auto &ammo = world.tables.ammo.entries[1];
        ammo.valid = true;
        ammo.velocity = 900;
        ammo.max_age_ticks = 200;

        Entity p;
        p.kind = EntityKind::Organic;
        p.item_type = 3;
        p.item_id = 1;
        p.alive = true;
        p.health = 100;
        p.flags = kEntityFlagPlayer;
        p.team = 1;
        p.position = {10, 20, 3};
        p.equipped_adm_index = static_cast<uint8_t>(m4);
        shooter = world.registry.spawn(0, p);
        world.cached.local_player = shooter;
        world.local_player_state = &local;
        world.ai.attach(shooter);
        auto &b = *world.ai.for_handle(shooter);
        b.inf.active = true;
        b.inf.is_local_player = true;

        inv.reset(t);
        weapon_inventory_load_from_display(t, {"WPN_M4AUTO"}, inv, 8, false);
        inv.equipped_combo = 3 * 65;
        if (bucket) inv.shared_clips[1] = 30;
        else inv.slot(3 * 65)->clip = 30;
        inv.pools[1] = 270;

        WeaponInstallData data;
        data.name = "WPN_M4AUTO";
        data.clipsize = 30;
        data.flags = static_cast<int32_t>(DEF_WEAPON_FLAG_AUTO);
        data.rows.resize(3);
        set_row(data.rows[0], "idle", 0, -1);
        set_row(data.rows[1], "fire", 0, 3);
        set_row(data.rows[2], "recoil", 0, 2);
        local_weapon_install(world, local.weapon, data, false, false, &inv, local.view);
    }

    // One update: the binding's state and press, the deferred fire record's dispatch at
    // the input frame (its press leg while the binding holds), then the pump.
    void tick(bool held, bool pressed) {
        local_weapon_set_input(local.weapon, local.view, held, pressed, false);
        if (deferred) (void)local.fire_record(held, false);
        LocalWeaponPumpIO io;
        io.view = &local.view;
        io.inventory = &inv;
        io.is_authority = true;
        io.self_wire_handle = shooter.packed;
        ++world.logic_tick;
        local_weapon_pump_tick(world, local.weapon, io);
        deferred = io.fire_deferred;
    }
};

void test_authority_rounds() {
    for (const bool bucket : {true, false}) {
        for (const int hold : {6, 7}) {
            Rig r(bucket);
            r.tick(false, false); // settle the install
            const int before = r.world.out.rounds.count;
            for (int t = 0; t < 40; ++t) r.tick(t < hold, t == 0);
            const int rounds = r.world.out.rounds.count - before;
            CHECK(rounds == (hold == 6 ? 1 : 2));
            for (int i = 0; i < rounds; ++i) {
                const RoundEvent &ev = r.world.out.rounds.records[static_cast<size_t>(before + i)];
                // A bucket rifle: 0x02 every shot. A bucketless one: its clip before the
                // spend (30, then 29), low two bits << 4, | 2.
                const uint8_t expected = bucket
                        ? uint8_t(0x02)
                        : uint8_t((((30 - i) & 3) << 4) | 0x02);
                CHECK(ev.mode_flags == expected);
            }
        }
    }
    std::printf("PASS the authority's rounds: one per six-tick hold, the bucket's mode byte 0x02\n");
}

} // namespace

// The fire case's leg choice (world::weapon_fire_record_leg), the one home the game's records and
// the weapon timing compiler share: the binding up runs the release leg; held, the press leg, or a
// PowerThrow def's windup start from the keyboard alone.
// [orig: Input_HandleActionBinding_0 @0x4E0420 - @0x4E07D9, @0x4E07E9, @0x4E08DC, @0x4E08FD,
//  @0x4E090B]
void test_fire_record_leg() {
    CHECK(weapon_fire_record_leg(false, 0, true) == FireRecordLeg::kRelease);
    CHECK(weapon_fire_record_leg(false, weapon_flag::kPowerThrow, true) == FireRecordLeg::kRelease);
    CHECK(weapon_fire_record_leg(true, weapon_flag::kAuto, false) == FireRecordLeg::kPress);
    CHECK(weapon_fire_record_leg(true, weapon_flag::kPowerThrow, true) == FireRecordLeg::kThrowStart);
    CHECK(weapon_fire_record_leg(true, weapon_flag::kPowerThrow, false) == FireRecordLeg::kNone);
}

int main() {
    test_fire_record_leg();
    test_hold_lengths();
    test_mid_fire_press_banks_only_while_held();
    test_mode_word();
    test_authority_rounds();
    if (failures == 0) std::printf("fire_cadence_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
