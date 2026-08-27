// The fire-sound presentation legs on the logic clock (S12a, ADR 0028): the
// propagation-delay gate and the pending-sound slot pool, seeded inline at
// round spawn — the original presents fire sounds inside the game tick, not
// per render frame. The presenting shell stamps the listener and drains the
// ready one-shots; a host that never stamps a listener (dedicated) runs none
// of this, the witnessed peer gate.
// [orig: Sound_PlayWithDistanceAttenuation @ 0x528e40 (gate + delay formula),
//  EffectSlot_AllocateAndInit @ 0x527c30 (the 128 x 24-B slot pool
//  @ 0x24DF678..0x24E0278 {flags|1, soundDef, pos[3], countdown}),
//  Sound_TickPendingSlots @ 0x529310 (per-tick countdown, play on zero)]
#ifndef OPENNOVA_WORLD_FIRE_SOUND_H
#define OPENNOVA_WORLD_FIRE_SOUND_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <runtime/world/geom.h>

namespace opennova::world {

class World;

struct RoundSpawnParams; // world/round_sim.h

// The wire round-event arm bits, mirrored from the npwire decoder constants
// (the adapter static_asserts the pairing; world never links the net stack).
// Bit 0 is tested FIRST: set -> the ammo-def arm. Only with bit 0 clear does
// bit 1 select the adm-indexed arm. [orig: NetPacket_DeserializeRoundEvent
// @ 0x42f270 — bit 0 @ 0x42f521, bit 1 @ 0x42f6ce]
namespace round_event_flag {
inline constexpr uint8_t kAltFire = 0x01;
inline constexpr uint8_t kAdmIndexed = 0x02;
} // namespace round_event_flag

// One pending propagation-delayed fire sound — the 24-B retail slot. The
// flag-2 slots of the original pool are the dialog-trigger variant
// [orig: @ 0x52932e], not ported here; sound-set duplicate repeats
// (ActionDef dupes through the same pool @ 0x401148/@ 0x40118b) are a tracked
// deferral — the JOX weapon.def authors none.
struct PendingFireSound {
    bool active = false;    // [orig: flags|1 @ 0x527c5e]
    std::string set_name;   // retail stores the resolved soundDef pointer
    Vec3 pos{};             // the fire-time play position, mission units
    int32_t countdown = 0;  // ticks until play [orig: slot+4 @ 0x527c8e]
    // Occlusion source identity, carried through the delay. Retail's delayed
    // play passes a NULL entity [orig: @ 0x52937b]; our audio bank keys
    // occlusion by BMS id, so the shipped port keeps the source through the
    // queue (pre-existing presentation behavior, unchanged by the move).
    int32_t source_bms_id = 0;
};

// One ready-to-play positional fire one-shot, drained by the presenting host.
// The set's max-range cull runs at PLAY time in our audio bank vs fire time
// in retail (soundDef+72 @ 0x528ec6) — the tracked D-AI-8 delta.
struct ReadyFireSound {
    std::string set_name;
    Vec3 pos{};
    int32_t source_bms_id = 0;
};

class FireSoundQueue {
public:
    // [orig: (0x24E0278 - 0x24DF678) / 24 slots]
    static constexpr int kSlotCount = 128;
    // [orig: the dist >= 0x1E gate @ 0x528ed4, integer units]
    static constexpr int32_t kMinDelayDistUnits = 30;
    // [orig: g_SoundSpeedFixed @ 0x24D6660 = 330.0 16.16; the divisor is its
    //  HIGH word (SHIWORD) — the integer 330]
    static constexpr int32_t kSoundSpeedUnits = 330;
    // The ready list is our transport to the presenting shell (retail plays
    // inline). Bounded like the retail slot pool so a stamped-but-undrained
    // embedder cannot grow it without limit; appends past the cap drop like a
    // full retail pool drops an allocation [orig: @ 0x527c47].
    static constexpr size_t kMaxReady = 128;

    // The presenting shell stamps the listener (the camera position, mission
    // frame) each frame before the tick batch [orig: listener_pos
    // @ 0x24D6630]. Never stamped on a dedicated host — every leg then stays
    // off, the witnessed is_mp_session_peer gate [orig: @ 0x528e57].
    void set_listener(const Vec3 &pos) {
        listener_ = pos;
        listener_valid_ = true;
    }
    bool listener_valid() const { return listener_valid_; }
    const Vec3 &listener() const { return listener_; }

    // The distance-gated play [orig: Sound_PlayWithDistanceAttenuation
    // @ 0x528e40 minus the set-range gate — D-AI-8]: truncate the listener
    // distance to integer units; >= 30 units queues a free pending slot with
    // countdown (62 * dist / 330) >> 2 (integer division; a full pool drops
    // the sound [orig: @ 0x527c47]; a zero countdown seeds 1 [orig:
    // @ 0x527c94]); nearer plays this present.
    void play_with_distance_delay(const char *set_name, const Vec3 &pos,
                                  int32_t source_bms_id);

    // Immediate positional play — the action-row legs
    // [orig: Entity_PlaySound3D_FullVolume @ 0x528e20].
    void play_immediate(const char *set_name, const Vec3 &pos,
                        int32_t source_bms_id);

    // The per-tick countdown [orig: Sound_TickPendingSlots @ 0x529310:
    // countdown-- reaching zero plays at the RECORDED position]. Runs at the
    // head of World::run_logic_tick — retail drains after the client network
    // frame and before Server_TickUpdate / Entity_UpdateAllEntities
    // [orig: @ 0x526697 in Game_ProcessMainFrame @ 0x5263f0], so client-
    // received seeds decrement the same tick they arrive; our host processes
    // wire fire on the same pre-tick boundary, projecting those one countdown
    // earlier than retail's post-drain server tick.
    void tick();

    // Drained by the presenting host once per present.
    std::vector<ReadyFireSound> drain();

    void clear();

    int pending_count() const;
    size_t ready_count() const { return ready_.size(); }

private:
    void push_ready(const char *set_name, const Vec3 &pos,
                    int32_t source_bms_id);

    std::array<PendingFireSound, kSlotCount> slots_{};
    std::vector<ReadyFireSound> ready_;
    Vec3 listener_{};
    bool listener_valid_ = false;
};

// The per-spawn fire-sound dispatch, called beside the FireEvent record — the
// inline-presentation moment of the original. The local player's own fire
// keeps its action-slot presentation (the shell self-filter, applied here at
// the seed). [orig: the host/AI inline leg WeaponSlot_FireAndSpawnEffects
// @ 0x53f440; the wire receive arms NetPacket_DeserializeRoundEvent @ 0x42f270
// — ammo arm sound @ 0x42f5dc, adm arm action rows @ 0x42f777/@ 0x42f785 and
// @ 0x42f98f/@ 0x42f9d0]
void fire_sound_on_spawn(World &world, const RoundSpawnParams &params);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_FIRE_SOUND_H
