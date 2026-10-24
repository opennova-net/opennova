// The body readout: a debug instrument with no behaviour. It records, per logic tick and per
// watched registry handle, the words a body's motion and death turn on, for the body_trace
// probe (godot/probes/net) and its binding (Simulation::debug_body_trace). Nothing in the
// game reads it, and every recording call leaves the game's words as it found them.
//
// Four producers append, in the order they run:
// - kGroundProbe: the movement resolver's settle-tail ground probe on a watched body
//   (CollisionWorld::resolve, collision_resolve.cpp): the probe's start, the height it
//   returned, the entity it hit (the word that lands in +0x28, ground_target) and whether that
//   entity holds a collision model instance [orig: Entity_MovementCollisionResolver @0x4B2BD0 -
//   the settle tail @0x4B3D6E..0x4B3D9F, the call @0x4B3D95; Entity_RaycastGroundHeightAndObject
//   @0x414320 - the +0x28 store @0x414370].
// - kForce: the collision force applicator on a watched person (apply_collision_force): its
//   hit type, force type, attacker and source point, and the body's velocity triple after it.
// - kBlast: a blast's damage leg on a watched entity (the person leg and the item leg of
//   ExplosionSim's damage): the entry's kz type, the attacker it credits, its centre, and the
//   damage with the health before and after.
// - kTick: once per entity pass, after it (World::update_all_entities), every watched handle's
//   words: Position, the velocity triple (a person's +0x98 / +0x9C / +0xA0, a hull's motor
//   words), both Flags words, Health, the seat (mounted, its carrier, seat and bone), +0x28
//   and the collision model instance; for a row with a brain, the brain's current and pending
//   states and the think countdown (+0x2AC), which a think re-arms from the brain's step.
// - kAiEvent: a timed AI event dispatched to a watched brain (AiEventQueue::process_timed): the
//   event's type word and the brain's state before and after the dispatch's transition, the
//   one place besides a think that commits a pending state [orig: AIEvent_ProcessTimedEntries
//   @0x455DF0 - the dispatch @0x455E4F, the commit @0x455E51..0x455E87].
// - kClassEvent: a watched brain's class event callback run with a nonzero phase (the kill /
//   damage notification 1, the death 4, ...): the phase and the brain's state before and after,
//   whose transition tail commits a pending state in the same call [orig:
//   EntityAI_ProcessGroundStateMachine @0x4583C0 / EntityAI_ProcessAirStateMachine @0x4581B0 - the
//   tails @0x45856E..0x4585B4 / @0x458369..0x4583B0].
// The ring holds the last kCapacity records; a reader polls by seq (since).
#pragma once

#include <runtime/world/entity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::world {

class World;
struct FixedVec3;

enum class BodyTraceEvent : uint8_t {
    kTick = 0,
    kGroundProbe = 1,
    kForce = 2,
    kBlast = 3,
    kAiEvent = 4,
    kClassEvent = 5,
};

struct BodyTraceRecord {
    uint32_t seq = 0;
    uint32_t logic_tick = 0; // World::logic_tick
    BodyTraceEvent event = BodyTraceEvent::kTick;
    uint16_t handle = 0xFFFFu; // the watched entity
    bool present = false;      // kTick: the registry holds it
    // The body's words (kTick; kGroundProbe: the probe's start in pos; kForce: the velocity
    // after the kick in vel). pos is 16.16 world, vel 16.16 per tick.
    int32_t pos[3] = {};
    int32_t vel[3] = {};
    uint32_t flags = 0;
    uint32_t engine_flags = 0;
    int16_t health = 0;
    bool mounted = false;
    uint16_t mount_target = 0xFFFFu;
    int8_t mount_seat = -1;
    uint8_t mount_bone = 0;
    uint16_t ground_target = 0xFFFFu; // +0x28 (kTick), the probe's hit (kGroundProbe)
    int32_t collision_model = -1;     // the instance's model id; -1 none (kTick, kGroundProbe:
                                      // the hit entity's)
    // kGroundProbe: the height the probe returned (16.16).
    int32_t probe_z = 0;
    // kForce / kBlast: the other party (the attacker) and the event's own words.
    uint16_t other = 0xFFFFu;
    uint8_t hit_type = 0;   // kForce: the ammo's secondary_anim byte; kBlast: the entry's kz type
    uint8_t force_type = 0; // kForce: the ammo's kz_physics byte
    int32_t source[3] = {}; // kForce: the source point; kBlast: the entry's centre
    int32_t damage = 0;     // kBlast
    int16_t health_before = 0; // kBlast
    // kTick: the brain's current and pending states (-1 without a brain) and the think
    // countdown (+0x2AC, Entity::spawn_phase). kAiEvent / kClassEvent: the states before
    // (brain_pend) and after (brain_state) the dispatch or the call, the event's type word or
    // the callback's phase in ai_event.
    int16_t brain_state = -1;
    int16_t brain_pend = -1;
    int32_t think_clock = 0;
    int32_t ai_event = 0;
    // kTick, a body with an infantry motor: its eye offset (+0x6C..+0x74, the movement
    // resolver's second test point less Position) and the restamp's source (1 the local body's
    // head anchor, 2 the capsule legs, 0 none yet), its body channel (the requested state, the
    // playing one, the outgoing one, the cross-fade weight in thousandths, the playhead) and
    // the capsule bottom the channel last sampled, its airborne latch, and the last resolve's
    // horizontal correction and the item type of its last contact.
    int32_t eye[3] = {};
    uint8_t eye_source = 0;
    int16_t anim_state = -1;
    int16_t anim_playing = -1;
    int16_t anim_prev = -1;
    int16_t anim_blend_milli = 0;
    int32_t clip_phase = 0;
    int32_t capsule_bottom = 0;
    bool airborne = false;
    int32_t resolve_push[2] = {};
    int32_t contact_item = 0;
};

// The watched handles and the last kCapacity records; seq counts every record appended.
class BodyTrace {
public:
    static constexpr std::size_t kCapacity = 512;
    static constexpr std::size_t kWatchSlots = 8;

    // The watched registry handles (0xFFFF a free slot).
    std::array<uint16_t, kWatchSlots> watch = filled_watch();

    bool watches(EntityHandle h) const {
        if (!h.valid()) return false;
        for (const uint16_t w : watch)
            if (w == h.packed) return true;
        return false;
    }
    bool any_watch() const {
        for (const uint16_t w : watch)
            if (w != 0xFFFFu) return true;
        return false;
    }
    void clear_watch() { watch = filled_watch(); }

    // The ring lives on the heap, sized at the first record, so a World that records nothing
    // (and a test World on the stack) carries no ring.
    void append(BodyTraceRecord record) {
        if (ring_.empty()) ring_.resize(kCapacity);
        record.seq = next_seq_++;
        ring_[record.seq % kCapacity] = record;
    }
    uint32_t next_seq() const { return next_seq_; }
    uint32_t oldest_seq() const {
        return next_seq_ > kCapacity ? next_seq_ - static_cast<uint32_t>(kCapacity) : 0u;
    }
    // The held records with seq >= from, oldest first.
    std::vector<BodyTraceRecord> since(uint32_t from) const {
        std::vector<BodyTraceRecord> out;
        for (uint32_t seq = from > oldest_seq() ? from : oldest_seq(); seq < next_seq_; ++seq)
            out.push_back(ring_[seq % kCapacity]);
        return out;
    }

private:
    static std::array<uint16_t, kWatchSlots> filled_watch() {
        std::array<uint16_t, kWatchSlots> out{};
        out.fill(0xFFFFu);
        return out;
    }
    std::vector<BodyTraceRecord> ring_;
    uint32_t next_seq_ = 0;
};

// The recording calls. Each reads the world, appends only for a watched handle, and changes
// nothing; the ones placed inside an expression return their argument unchanged.

// kTick for every watched handle (World::update_all_entities' tail).
void body_trace_tick(World &world);

// kGroundProbe: the settle tail's probe on `body` from `start`, which returned `ground_z` and
// hit `hit`. Returns hit.
EntityHandle body_trace_ground_probe(World &world, EntityHandle body, const int32_t start[3],
        int32_t ground_z, EntityHandle hit);

// kForce: the force applicator's kick on `target`, the velocity triple after it.
void body_trace_force(World &world, const Entity &target, uint8_t hit_type, uint8_t force_type,
        const FixedVec3 &source, EntityHandle attacker, const int32_t vel[3]);

// kBlast: a blast's damage leg on `target` (health_before ahead of the leg, the target's
// Health after it).
void body_trace_blast(World &world, const Entity &target, int32_t kz_type, EntityHandle attacker,
        const int32_t centre[3], int32_t damage, int32_t health_before);

// kAiEvent: a timed event of type `type` dispatched to the brain of `entity`, its state
// `state_before` ahead of the dispatch and `state_after` past its transition.
void body_trace_ai_event(World &world, EntityHandle entity, int32_t type, int32_t state_before,
        int32_t state_after);

// kClassEvent: the class event callback run on the brain of `entity` with `phase` (nonzero),
// its state `state_before` ahead of the call and `state_after` past it.
void body_trace_class_event(World &world, EntityHandle entity, int32_t phase,
        int32_t state_before, int32_t state_after);

inline const char *body_trace_event_name(BodyTraceEvent event) {
    switch (event) {
        case BodyTraceEvent::kTick: return "tick";
        case BodyTraceEvent::kGroundProbe: return "ground_probe";
        case BodyTraceEvent::kForce: return "force";
        case BodyTraceEvent::kBlast: return "blast";
        case BodyTraceEvent::kAiEvent: return "ai_event";
        case BodyTraceEvent::kClassEvent: return "class_event";
    }
    return "unknown";
}

} // namespace opennova::world
