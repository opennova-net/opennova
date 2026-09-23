// The fire-sound legs on the logic clock (world/fire_sound.h): the witnessed
// distance gate + delay formula, the pending-slot pool semantics, the arm
// split's sound routing, and the listener/local-player gates — all through
// RoundSim::spawn and the World drain, the public seam.
#include <cstdio>
#include <cstring>
#include <vector>

#include <runtime/world/fire_sound.h>
#include <runtime/inmatch/client_weapon_replay.h>
#include <runtime/replication/client_state.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

struct Rig {
    World world;
    EntityHandle shooter;

    Rig() {
        world.registry.configure_pool(0, 8);
        Entity s;
        s.kind = EntityKind::Organic;
        s.item_type = 3;
        s.bms_id = 41;
        s.position = {0.0f, 0.0f, 0.0f};
        s.health = 100;
        shooter = world.registry.spawn(0, s);

        world.tables.ammo.entries.resize(1);
        AmmoTableEntry &ammo = world.tables.ammo.entries[0];
        ammo.name = "BALL";
        ammo.valid = true;
        ammo.velocity = 620;
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        ammo.ai_launch_set = "GS_AK47_2";

        // adm 1: fire row authors begin+end sets, recoil row an end set — the
        // JOX shape (gunshots ride soundsetend).
        world.tables.weapons.entries.resize(2);
        WeaponTableEntry &def = world.tables.weapons.entries[1];
        def.valid = true;
        def.name = "WPN_TEST";
        std::strcpy(def.action_fsm.actions[weapon_action::kFire].soundset,
                "GS_BEGIN");
        std::strcpy(def.action_fsm.actions[weapon_action::kFire].soundsetend,
                "GS_END");
        std::strcpy(def.action_fsm.actions[weapon_action::kRecoil].soundsetend,
                "GS_RECOIL");
    }

    int fire(const Vec3 &origin, uint8_t wire_flags = 0,
             bool with_owner = true,
             RoundConsequenceMode mode = RoundConsequenceMode::Authoritative) {
        RoundSpawnParams p;
        if (with_owner) {
            p.owner = shooter;
            p.shooter_handle = shooter.packed;
        }
        p.origin = origin;
        p.ammo_index = 0;
        p.adm_index = 1;
        p.wire_round_flags = wire_flags;
        return world.round_sim.spawn(world, p, mode);
    }

    std::vector<ReadyFireSound> drain() { return world.out.fire_sounds.drain(); }
};

// Without a stamped listener (a dedicated host) no sound leg runs at all
// [orig: the is_mp_session_peer gate @ 0x528e57].
void test_no_listener_no_sounds() {
    Rig r;
    CHECK(r.fire({10.0f, 0.0f, 0.9f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.out.fire_sounds.pending_count() == 0);
}

// Under 30 integer units the ammo-arm sound plays this present, carrying the
// shooter's occlusion identity [orig: the else leg @ 0x528f07].
void test_near_fire_plays_immediately() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({10.0f, 0.0f, 0.0f}) >= 0);
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        CHECK(out[0].set_name == "GS_AK47_2");
        CHECK(out[0].pos.x == 10.0f);
        CHECK(out[0].source_bms_id == 41);
    }
}

// At 330 units the witnessed delay is (62 * 330 / 330) >> 2 = 15 ticks, and
// the play fires at the RECORDED fire-time position [orig: @ 0x528ef2,
// @ 0x52937b].
void test_far_fire_counts_down_on_the_logic_clock() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({330.0f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.out.fire_sounds.pending_count() == 1);
    for (int i = 0; i < 14; ++i) {
        r.world.out.fire_sounds.tick();
        CHECK(r.drain().empty());
    }
    r.world.out.fire_sounds.tick();
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        CHECK(out[0].set_name == "GS_AK47_2");
        CHECK(out[0].pos.x == 330.0f);
        CHECK(out[0].source_bms_id == 41);
    }
    CHECK(r.world.out.fire_sounds.pending_count() == 0);
}

// The >= 30 gate is on the TRUNCATED integer distance: 29.9 plays now, 30
// queues one tick ((62 * 30 / 330) >> 2 = 1) [orig: @ 0x528ed4].
void test_distance_gate_truncates_to_units() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({29.9f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().size() == 1);
    CHECK(r.fire({30.0f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.out.fire_sounds.pending_count() == 1);
    r.world.out.fire_sounds.tick();
    CHECK(r.drain().size() == 1);
}

// The local player's own fire keeps its action-slot presentation — no seed.
void test_local_player_fire_is_filtered() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    r.world.cached.local_player = r.shooter;
    CHECK(r.fire({10.0f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.out.fire_sounds.pending_count() == 0);
}

// The adm-indexed arm plays every authored action-row set (fire + recoil,
// begin + end) IMMEDIATELY at the shooter's position — never the fire origin,
// never delay-gated [orig: ActionSlot_ExecuteAction @ 0x4020ef + the one-shot
// end shim @ 0x401100, both at entity+4; rows @ 0x42f777/@ 0x42f785].
void test_adm_arm_plays_action_rows_at_the_shooter() {
    Rig r;
    // Listener 500 u away: an ammo-arm sound would queue; the action rows may not.
    r.world.out.fire_sounds.set_listener({500.0f, 0.0f, 0.0f});
    CHECK(r.fire({0.0f, 0.0f, 0.9f}, round_event_flag::kAdmIndexed) >= 0);
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 3);
    CHECK(r.world.out.fire_sounds.pending_count() == 0);
    if (out.size() == 3) {
        CHECK(out[0].set_name == "GS_BEGIN");
        CHECK(out[1].set_name == "GS_END");
        CHECK(out[2].set_name == "GS_RECOIL");
        for (const ReadyFireSound &s : out) {
            CHECK(s.pos.x == 0.0f);
            CHECK(s.pos.z == 0.0f); // the entity position, not the 0.9 eye origin
            CHECK(s.source_bms_id == 41);
        }
    }
}

// Bit 0 is tested first: set means the ammo-def arm even when bit 1 rides
// along [orig: @ 0x42f521 before @ 0x42f6ce].
void test_alt_fire_bit_selects_the_ammo_arm() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({10.0f, 0.0f, 0.0f},
                  round_event_flag::kAltFire | round_event_flag::kAdmIndexed) >= 0);
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) CHECK(out[0].set_name == "GS_AK47_2");
}

// An adm-arm round with no local shooter entity (a joiner's decoded remote
// fire) plays at the wire row's position when supplied, else at the origin.
void test_entityless_adm_arm_uses_the_supplied_shooter_pos() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    RoundSpawnParams p;
    p.origin = {5.0f, 0.0f, 1.7f};
    p.ammo_index = 0;
    p.adm_index = 1;
    p.wire_round_flags = round_event_flag::kAdmIndexed;
    p.shooter_pos = {5.0f, 0.0f, 0.2f};
    p.shooter_pos_valid = true;
    CHECK(r.world.round_sim.spawn(r.world, p,
                  RoundConsequenceMode::VisualOnly) >= 0);
    std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 3);
    if (!out.empty()) CHECK(out[0].pos.z == 0.2f);

    p.shooter_pos_valid = false;
    CHECK(r.world.round_sim.spawn(r.world, p,
                  RoundConsequenceMode::VisualOnly) >= 0);
    out = r.drain();
    CHECK(out.size() == 3);
    if (!out.empty()) CHECK(out[0].pos.z == 1.7f);
}

// A full 128-slot pool drops further delayed sounds, exactly like the retail
// allocator's failed scan [orig: @ 0x527c47].
void test_full_pending_pool_drops() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    for (int i = 0; i < FireSoundQueue::kSlotCount + 5; ++i)
        r.world.out.fire_sounds.play_with_distance_delay(
                "GS_FAR", {330.0f, 0.0f, 0.0f}, 0);
    CHECK(r.world.out.fire_sounds.pending_count() == FireSoundQueue::kSlotCount);
    for (int i = 0; i < 15; ++i) r.world.out.fire_sounds.tick();
    CHECK(r.drain().size() == FireSoundQueue::kSlotCount);
}

// run_logic_tick advances the countdown once per tick from the World seam.
void test_world_tick_advances_the_countdown() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({330.0f, 0.0f, 0.0f}) >= 0);
    for (int i = 0; i < 15; ++i) r.world.run_logic_tick();
    CHECK(r.drain().size() == 1);
}

} // namespace


void test_replica_borrows_the_carrier_slot_for_received_actions() {
    namespace im = opennova::inmatch;
    namespace rp = opennova::replication;
    Rig r;
    r.world.logic_tick = 100;
    r.world.rules.mp_session = true;
    r.world.rules.projectile_authority = false;
    r.world.out.fire_sounds.set_listener({});
    r.world.tables.weapons.entries.resize(3);
    auto &weapon = r.world.tables.weapons.entries[2];
    weapon.valid = true; weapon.name = "MOUNT";
    weapon.clipsize = -1;
    weapon.action_fsm.clip_capacity = -1;
    weapon.action_fsm.heat_per_shot = 100;
    weapon.action_fsm.heat_decay_per_tick = 10;
    std::strcpy(weapon.action_fsm.soundhead, "MOUNT_HEAD");
    std::strcpy(weapon.action_fsm.soundtrailoff, "MOUNT_TAIL");
    std::strcpy(weapon.action_fsm.soundfireloop, "MOUNT_LOOP");
    std::strcpy(r.world.tables.weapons.entries[1].action_fsm.soundhead, "WIRE_HEAD");
    r.world.registry.configure_pool(1, 2);
    Entity gun;
    gun.has_item_def = true; gun.item_id = 80; gun.item_type = 2;
    gun.item_attrib = kItemAttribEweap; gun.primary_weapon = "MOUNT";
    Seat seat; seat.type = SeatType::Gunner; seat.bone_index = 7; gun.seats.push_back(seat);
    gun.weapon_userpoint_bytes[2][1] = 1; // only the third magazine-selected muzzle
    const auto handle = r.world.registry.spawn(1, gun);
    rp::ClientState state;
    rp::ClientEntityState peer;
    peer.handle = 3; peer.type_id = 11; peer.cls = opennova::EntityClass::Player;
    peer.carrier_handle = handle.packed; peer.mount_bone = 7;
    peer.x = 12 * 65536; peer.y = 34 * 65536; peer.z = 5 * 65536;
    state.entities.push_back(peer);
    im::sync_replica_weapon_slots(state, r.world, 0xFFFF);
    auto *mount = r.world.registry.get(handle);
    CHECK(state.entities[0].weapon_slot_handle == handle.packed);
    CHECK(mount->primary_weapon_owner.packed == 3);
    RoundSourceState source;
    im::replica_weapon_action_source(state.entities[0], r.world, 0x22, source);
    CHECK(source.action_allowed && source.mounted_action && source.action_slot == &mount->primary_weapon_slot);
    RoundSpawnParams p;
    p.adm_index = 1; p.ammo_index = 0; p.shooter_handle = 3; p.wire_round_flags = 0x22;
    p.shooter_pos = {12, 34, 5}; p.shooter_pos_valid = true; p.source_state = &source;
    CHECK(r.world.round_sim.spawn(r.world, p, RoundConsequenceMode::VisualOnly) >= 0);
    auto ready = r.drain();
    CHECK(ready.size() == 4);
    if (ready.size() == 4) {
        CHECK(ready[0].set_name == "GS_BEGIN" && ready[1].set_name == "GS_END");
        CHECK(ready[2].set_name == "MOUNT_HEAD" && ready[3].set_name == "GS_RECOIL");
        CHECK(ready[2].pos.x == 12 && ready[2].pos.z == 5);
    }
    CHECK(mount->primary_weapon_slot.kick == 66 && mount->primary_weapon_slot.counter == 0);
    CHECK(mount->primary_weapon_slot.current == weapon_action::kIdle &&
            mount->primary_weapon_slot.next == weapon_action::kIdle &&
            mount->primary_weapon_slot.heat_window_end_tick == 111);
    // No world organic exists at wire handle 3. The replica's borrowed slot
    // still pumps once, and the world's weapon walk (a joiner walks its
    // replica slots instead) must not clear its owner.
    r.world.pump_weapon_actions();
    CHECK(mount->primary_weapon_owner.packed == 3);
    im::tick_replica_weapon_slots(state, r.world);
    ready = r.drain();
    CHECK(ready.size() == 1 && ready[0].set_name == "MOUNT_TAIL");
    CHECK(mount->primary_weapon_slot.kick == 0 && r.world.out.sound_emitters.empty());
    im::tick_replica_weapon_slots(state, r.world);
    CHECK(r.drain().empty());

    // A missing muzzle only skips action replay; it still admits the round.
    source = {};
    im::replica_weapon_action_source(state.entities[0], r.world, 2, source);
    CHECK(!source.action_allowed);
    CHECK(r.world.round_sim.spawn(r.world, p, RoundConsequenceMode::VisualOnly) >= 0);
    CHECK(r.drain().empty());

    // The designated-G route selects the hull's slot without changing the
    // wire-selected action rows or the child model's muzzle gate.
    Entity hull = gun;
    hull.item_type = 1;
    hull.item_attrib |= 0x40u;
    const auto parent_handle = r.world.registry.spawn(1, hull);
    auto *parent = r.world.registry.get(parent_handle);
    mount->emplacement_attachment_flags = 2;
    mount->ground_target = mount->emplacement_parent = parent_handle;
    mount->emplacement_parent_spawn_id = parent->registry_spawn_id;
    state.entities[0].seat_type = 2;
    im::sync_replica_weapon_slots(state, r.world, 0xFFFF);
    CHECK(state.entities[0].weapon_slot_handle == parent_handle.packed);
    parent->primary_weapon_slot.counter = 7;
    source = {};
    im::replica_weapon_action_source(state.entities[0], r.world, 0x22, source);
    CHECK(source.action_slot == &parent->primary_weapon_slot);
    CHECK(r.world.round_sim.spawn(r.world, p, RoundConsequenceMode::VisualOnly) >= 0);
    ready = r.drain();
    CHECK(ready.size() == 4 && ready[2].set_name == "MOUNT_HEAD");
    CHECK(parent->primary_weapon_slot.counter == 7 && parent->primary_weapon_slot.kick == 73);
    im::tick_replica_weapon_slots(state, r.world);
    ready = r.drain();
    CHECK(ready.size() == 1 && ready[0].set_name == "MOUNT_TAIL");
    CHECK(parent->primary_weapon_slot.counter == 6);

    // Dismount restores a null remote slot. Each on-foot event starts a
    // temporary context, so it plays the wire definition's head every time.
    state.entities[0].mount_bone = 0;
    im::sync_replica_weapon_slots(state, r.world, 0xFFFF);
    CHECK(state.entities[0].weapon_slot_handle == 0xFFFF);
    for (int i = 0; i < 2; ++i) {
        source = {};
        im::replica_weapon_action_source(state.entities[0], r.world, 2, source);
        CHECK(!source.mounted_action && source.action_slot == nullptr);
        CHECK(r.world.round_sim.spawn(r.world, p, RoundConsequenceMode::VisualOnly) >= 0);
        ready = r.drain();
        CHECK(ready.size() == 4 && ready[2].set_name == "WIRE_HEAD");
    }
}

void test_weapon_level_sound_publication() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0, 0, 0});
    WeaponFsmDef def;
    std::strcpy(def.soundhead, "MINI_HEAD");
    std::strcpy(def.soundfireloop, "MINI_LOOP");
    std::strcpy(def.soundtrailoff, "MINI_TAIL");
    auto *owner = r.world.registry.get(r.shooter);
    WeaponFsmEvents events;
    events.head_started = true;
    weapon_sound_publish(r.world, *owner, def, events);
    auto sounds = r.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) CHECK(sounds[0].set_name == "MINI_HEAD");
    CHECK(r.world.out.sound_emitters.empty());
    events = {};
    events.fireloop_lifetime_ticks = 19;
    weapon_sound_publish(r.world, *owner, def, events);
    CHECK(r.drain().empty());
    auto loops = r.world.out.sound_emitters.drain();
    CHECK(loops.size() == 1);
    if (!loops.empty()) {
        CHECK(loops[0].set_name == "MINI_LOOP");
        CHECK(loops[0].lifetime_ticks == 19);
        CHECK(loops[0].volume_q8_8 == 0xFFFF);
        CHECK(loops[0].source_handle == r.shooter.packed);
    }
    events.fireloop_lifetime_ticks = -57;
    weapon_sound_publish(r.world, *owner, def, events);
    loops = r.world.out.sound_emitters.drain();
    CHECK(loops.size() == 1 && loops[0].lifetime_ticks == -57);
    events.fireloop_lifetime_ticks = 19;
    r.world.registry.configure_pool(1, 1);
    Entity mount;
    owner->mount_target = r.world.registry.spawn(1, mount);
    owner->mount_type = SeatType::Gunner;
    weapon_sound_publish(r.world, *owner, def, events);
    loops = r.world.out.sound_emitters.drain();
    CHECK(loops.size() == 1 && loops[0].source_handle == owner->mount_target.packed);
    events = {};
    events.trailoff_started = true;
    weapon_sound_publish(r.world, *owner, def, events);
    sounds = r.drain();
    CHECK(sounds.size() == 1 && sounds[0].set_name == "MINI_TAIL");
}


void test_delayed_interface_shares_slots_and_suppresses_repeats() {
    FireSoundQueue q;
    q.set_listener({});
    q.play_throttled_interface("FLAG_VXSV_P", {3, 4, 5}, 62, 3720);
    q.play_throttled_interface("FLAG_VXSV_P", {6, 7, 8}, 62, 3720);
    CHECK(q.pending_count() == 1);
    for (int i = 0; i < 61; ++i) q.tick();
    CHECK(q.drain().empty() && q.pending_count() == 1);
    q.tick();
    auto ready = q.drain();
    CHECK(ready.size() == 1 && ready[0].interface_set &&
            ready[0].set_name == "FLAG_VXSV_P" && ready[0].pos.z == 5);
    for (int i = 62; i < 3719; ++i) q.tick();
    q.play_throttled_interface("FLAG_VXSV_P", {}, 62, 3720);
    CHECK(q.pending_count() == 0);
    q.tick();
    q.play_throttled_interface("FLAG_VXSV_P", {}, 0, 3720);
    CHECK(q.pending_count() == 1);
    q.tick(); // zero delay seeds one
    CHECK(q.drain().size() == 1);
    q.clear();
    for (int i = 0; i < 32; ++i)
        q.play_throttled_interface(("SET" + std::to_string(i)).c_str(), {}, 1, 100);
    q.play_throttled_interface("EXTRA", {}, 1, 100);
    CHECK(q.pending_count() == 32); // suppression table fills before the sound pool
    q.clear();
    for (int i = 0; i < FireSoundQueue::kSlotCount; ++i)
        q.play_with_distance_delay("BALL", {330, 0, 0}, 0);
    q.play_throttled_interface("RESERVED", {}, 1, 100);
    CHECK(q.pending_count() == FireSoundQueue::kSlotCount);
    for (int i = 0; i < 15; ++i) q.tick();
    q.drain();
    q.play_throttled_interface("RESERVED", {}, 1, 100);
    CHECK(q.pending_count() == 0); // failed allocation kept its suppression reservation
    for (int i = 15; i < 100; ++i) q.tick();
    q.play_throttled_interface("RESERVED", {}, 1, 100);
    CHECK(q.pending_count() == 1);
    FireSoundQueue dedicated;
    dedicated.play_throttled_interface("SILENT", {}, 1, 1);
    dedicated.tick();
    CHECK(dedicated.drain().empty());
}

void test_flag_sound_variants() {
    World world;
    opennova::lwf::File bank;
    for (const char *name : {"FLAG_DO_P", "FLAG_DO_OT", "FLAG_DO_T", "FLAG_WIN_P",
            "FLAG_WIN_T", "FLAG_VXDO_P", "FLAG_VXDO_T", "FLAG_VXDO_OT",
            "FLAG_VWIN_P", "FLAG_VWIN_T", "FLAG_VWIN_OT", "FLAG_PU_P",
            "FLAG_PU_T", "FLAG_PU_OT", "FLAG_VXPU_P", "FLAG_VXPU_T",
            "FLAG_VXPU_OT", "FLAG_SV_P", "FLAG_SV_T", "FLAG_SV_OT",
            "FLAG_VXSV_P", "FLAG_VXSV_T", "FLAG_VXSV_OT"}) {
        opennova::lwf::Multi set;
        set.name = name; bank.multis.push_back(set);
    }
    opennova::audio::SoundSetIndex sets;
    sets.add_bank(0, bank);
    world.tables.sound_sets = &sets;
    world.out.fire_sounds.set_listener({});
    Entity local, actor;
    local.handle = EntityHandle::make(0, 1); local.team = 1;
    actor.handle = EntityHandle::make(0, 2); actor.team = 1;
    struct Case { uint8_t event; uint32_t game; bool own; uint8_t team; const char *cue; const char *voice; };
    const Case cases[] = {
        {19, 65540, true, 1, "FLAG_DO_P", "FLAG_VXDO_P"},
        {19, 65540, false, 1, "FLAG_WIN_T", "FLAG_VXDO_T"},
        {19, 65540, false, 2, "FLAG_DO_OT", "FLAG_VXDO_OT"},
        {19, 65544, true, 1, "FLAG_WIN_P", "FLAG_VWIN_P"},
        {19, 65544, false, 1, "FLAG_WIN_T", "FLAG_VWIN_T"},
        {19, 65544, false, 2, "FLAG_DO_OT", "FLAG_VWIN_OT"},
        {19, 8, true, 1, "FLAG_WIN_P", "FLAG_VWIN_P"},
        {19, 8, false, 1, "FLAG_DO_OT", "FLAG_VWIN_OT"},
        {19, 65538, true, 1, "", ""},
        {19, 65538, false, 1, "FLAG_DO_T", ""},
        {19, 65538, false, 2, "", ""},
        {20, 65540, true, 1, "FLAG_PU_P", "FLAG_VXPU_P"},
        {20, 65540, false, 1, "FLAG_PU_T", "FLAG_VXPU_T"},
        {20, 65540, false, 2, "FLAG_PU_OT", "FLAG_VXPU_OT"},
        {20, 65544, true, 1, "FLAG_PU_P", ""},
        {20, 8, false, 1, "FLAG_PU_OT", ""},
        {21, 65544, true, 1, "FLAG_SV_P", "FLAG_VXSV_P"},
        {21, 65544, false, 1, "FLAG_SV_T", "FLAG_VXSV_T"},
        {21, 8, false, 1, "FLAG_SV_OT", "FLAG_VXSV_OT"},
    };
    for (const auto &c : cases) {
        world.out.fire_sounds.clear(); world.out.script_sounds.clear();
        actor.team = c.team;
        play_flag_event_sound(world, c.event, c.own ? local : actor, local, c.game, -123, 45);
        auto ready = world.out.fire_sounds.drain();
        if (c.own) {
            CHECK(ready.empty());
            CHECK(world.out.script_sounds.size() == (*c.cue ? 1u : 0u));
            if (!world.out.script_sounds.empty())
                CHECK(world.out.script_sounds[0].name == c.cue &&
                        world.out.script_sounds[0].kind == ScriptSoundEvent::Kind::Interface);
        } else {
            CHECK(world.out.script_sounds.empty());
            CHECK(ready.size() == (*c.cue ? 1u : 0u));
            if (!ready.empty()) CHECK(ready[0].set_name == c.cue && !ready[0].interface_set &&
                    ready[0].pos.x == -123 && ready[0].pos.y == 45 && ready[0].pos.z == 0);
        }
        CHECK(world.out.fire_sounds.pending_count() == (*c.voice ? 1 : 0));
        for (int i = 0; i < 62; ++i) world.out.fire_sounds.tick();
        ready = world.out.fire_sounds.drain();
        CHECK(ready.size() == (*c.voice ? 1u : 0u));
        if (!ready.empty()) CHECK(ready[0].interface_set && ready[0].set_name == c.voice);
    }
    world.out.fire_sounds.clear();
    world.tables.sound_sets = nullptr;
    play_flag_event_sound(world, 21, actor, local, 65544, 0, 0);
    CHECK(world.out.fire_sounds.pending_count() == 0 && world.out.fire_sounds.drain().empty());
    world.script.waypoints.current = 4;
    play_flag_event_sound(world, 19, local, local, 8, 0, 0);
    CHECK(world.script.waypoints.current == 4); // unsupported mode retains selection
}

int main() {
    test_delayed_interface_shares_slots_and_suppresses_repeats();
    test_flag_sound_variants();
    test_no_listener_no_sounds();
    test_weapon_level_sound_publication();
    test_replica_borrows_the_carrier_slot_for_received_actions();
    test_near_fire_plays_immediately();
    test_far_fire_counts_down_on_the_logic_clock();
    test_distance_gate_truncates_to_units();
    test_local_player_fire_is_filtered();
    test_adm_arm_plays_action_rows_at_the_shooter();
    test_alt_fire_bit_selects_the_ammo_arm();
    test_entityless_adm_arm_uses_the_supplied_shooter_pos();
    test_full_pending_pool_drops();
    test_world_tick_advances_the_countdown();
    if (failures == 0) std::printf("fire_sound_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
