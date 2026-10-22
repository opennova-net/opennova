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
//   and the collision model instance.
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

inline const char *body_trace_event_name(BodyTraceEvent event) {
    switch (event) {
        case BodyTraceEvent::kTick: return "tick";
        case BodyTraceEvent::kGroundProbe: return "ground_probe";
        case BodyTraceEvent::kForce: return "force";
        case BodyTraceEvent::kBlast: return "blast";
    }
    return "unknown";
}

} // namespace opennova::world
