// The presenting-peer gate of the fire-sound legs lives INSIDE the plays, not
// around the action rows: a host with no stamped listener (dedicated) readies
// nothing on either arm while the adm arm's kick/heat replay still runs; and
// the immediate plays carry the source entity's own-channel key where the
// delayed slot plays with none. Sibling of tests/world/fire_sound_test.
// [orig: Sound_Play3DPositional @0x527CB3 is_mp_session_peer;
//  Sound_PlayWithDistanceAttenuation @0x528E57, its near leg @0x528F07;
//  Sound_TickPendingSlots NULL entity @0x52937B]
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

    int fire(const Vec3 &origin, uint8_t wire_flags = 0) {
        RoundSpawnParams p;
        p.owner = shooter;
        p.shooter_handle = shooter.packed;
        p.origin = origin;
        p.ammo_index = 0;
        p.adm_index = 1;
        p.wire_round_flags = wire_flags;
        return world.round_sim.spawn(world, p, RoundConsequenceMode::Authoritative);
    }

    std::vector<ReadyFireSound> drain() { return world.out.fire_sounds.drain(); }
};

// The adm arm's action rows play through Entity_PlaySound3D_FullVolume, whose
// play tests is_mp_session_peer itself: no listener, no ready row, while the
// rows still ran [orig: @ 0x527CB3].
void test_listener_less_host_readies_nothing_on_the_adm_arm() {
    Rig r;
    CHECK(r.fire({0.0f, 0.0f, 0.9f}, round_event_flag::kAdmIndexed) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.out.fire_sounds.pending_count() == 0);

    FireSoundQueue q;
    q.play_immediate("GS_BEGIN", {1.0f, 2.0f, 3.0f}, 41);
    CHECK(q.drain().empty());
    q.set_listener({0.0f, 0.0f, 0.0f});
    q.play_immediate("GS_BEGIN", {1.0f, 2.0f, 3.0f}, 41);
    CHECK(q.drain().size() == 1);
}

// The immediate plays forward the shooter as the open's own-channel key; the
// delayed slot stores no entity and its tick plays with NULL, keeping only the
// occlusion source [orig: near leg @ 0x528F07; the NULL entity @ 0x52937B].
void test_immediate_rows_carry_the_source_key_and_delayed_rows_none() {
    Rig r;
    r.world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({0.0f, 0.0f, 0.9f}, round_event_flag::kAdmIndexed) >= 0);
    std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 3);
    const uint32_t key = opennova::audio::oneshot_sound_id(r.shooter.packed, 41);
    CHECK(key != 0);
    for (const ReadyFireSound &s : out) CHECK(s.sound_id == key);

    CHECK(r.fire({10.0f, 0.0f, 0.0f}) >= 0);
    out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) CHECK(out[0].sound_id == key);

    CHECK(r.fire({330.0f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().empty());
    for (int i = 0; i < 15; ++i) r.world.out.fire_sounds.tick();
    out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        CHECK(out[0].sound_id == 0);
        CHECK(out[0].source_bms_id == 41);
    }
}

} // namespace

int main() {
    test_listener_less_host_readies_nothing_on_the_adm_arm();
    test_immediate_rows_carry_the_source_key_and_delayed_rows_none();
    if (failures) return 1;
    std::printf("fire_sound_peer_gate_test: OK\n");
    return 0;
}
