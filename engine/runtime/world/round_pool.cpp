// The round pool's slot walks: its one allocator, and a client's S2C 0x59
// found leg over every slot. The pool is 512 rounds of 780 bytes, each with its
// activity byte; a release clears that byte, the life word and its effect
// handles (the ring record, the glow) and keeps every other word, so a freed
// round reads as it last stood until the allocator's cursor comes back to its
// slot.
// [orig: g_RoundPool @0xB7E1A8, the activity bytes g_RoundPoolActive @0xB7DFA0;
//  CEntityManager_AllocateSlot @0x4EAAD0; Projectile_ReleaseEffects @0x4E8280
//  (@0x4E8317 / @0x4E831E, the handles @0x4E82F2 / @0x4E8310);
//  Entity_SpawnOrUpdateFromSlotPacket @0x546770]

#include <runtime/world/round_sim.h>

#include <runtime/world/tracer_trails.h>
#include <runtime/world/world.h>

#include <cstddef>
#include <cstdint>

namespace opennova::world {

uint16_t RoundSim::allocate_round_id(uint16_t sequence_word) {
    spawn_allocated_ = true;
    if (sequence_word != 0xFFFFu) return last_allocated_round_id_ = sequence_word; // @0x4EAB12
    round_id_counter_ = (round_id_counter_ + 1u) & 0x1FFu;           // @0x4EAAF5..0x4EAB04
    return last_allocated_round_id_ = static_cast<uint16_t>(
            (uint32_t(local_slot_id) << 9) | round_id_counter_);
}

int RoundSim::allocate_slot(World &world, uint16_t sequence_word) {
    const uint16_t id = allocate_round_id(sequence_word);
    // The release a held slot takes: the trail's last point and its kill, then
    // the round's own release [orig: Projectile_ReleaseEffects @0x4E8280 -
    // @0x4E8292..0x4E82AC, the ring and the activity byte @0x4E82E3..0x4E8317].
    const auto release = [this, &world](LiveRound &held) {
        if (held.trail_slot >= 0) {
            trails.append(held.trail_slot,
                    tracer_trail_anchor(trails.channels[static_cast<size_t>(held.trail_slot)],
                            held.pos, held.yaw_bam, held.pitch_bam, held.roll_bam,
                            world.entity_update_counter));
            trails.request_kill(held.trail_slot);
        }
        retire_round(held);
    };
    const auto live = [this](int slot) {
        return rounds[static_cast<size_t>(slot)].active;
    };
    // The slot after the cursor; an occupied one is released unless its
    // Priority bit holds it, which moves the walk on, until a free slot comes
    // up; a walk back to the start takes the slot after it whatever it holds.
    // [orig: @0x4EAADB..0x4EAB37, the walk @0x4EAB42..0x4EAB85, the start test
    //  @0x4EAB89, the wrap @0x4EAB8E..0x4EAB9C]
    const int start = slot_cursor_;
    int slot = (slot_cursor_ + 1) & 0x1FF;
    slot_cursor_ = slot;
    bool take = false;
    if (!live(slot)) {
        take = start != slot;
    } else {
        while (start != slot) {
            if (rounds[static_cast<size_t>(slot)].priority) {
                slot = (slot + 1) & 0x1FF;
                slot_cursor_ = slot;
            } else {
                release(rounds[static_cast<size_t>(slot)]);
                slot = slot_cursor_;
            }
            if (!live(slot)) {
                take = start != slot;
                break;
            }
        }
    }
    if (!take) {
        slot = (slot + 1) & 0x1FF;
        slot_cursor_ = slot;
    }
    // A slot still held is released, then zeroed and stamped [orig: @0x4EABA8..0x4EABB2,
    //  the memset @0x4EABC8, the id @0x4EABD5].
    LiveRound &round = rounds[static_cast<size_t>(slot)];
    if (round.active) release(round);
    round = LiveRound{};
    round.shot_seq = id;
    return slot;
}

DeployedRoundCopy RoundSim::match_deployed_item(const DeployedRoundQuery &query) {
    DeployedRoundCopy copy;
    for (LiveRound &round : rounds) {
        // The round's +0x170 is its shooter as this client resolved it: a
        // decoded round's wire shooter, this client's own body for its own fire
        // [orig: @0x546830..0x546840; RoundData_SpawnRound @0x4EC670].
        const bool friendly = round.item_type_id == query.friendly_item;
        const bool enemy = round.item_type_id == query.enemy_item &&
                round.shooter_handle == query.owner_handle;
        if (!friendly && !enemy) continue;
        copy.matched = true;
        round.parent = query.parent;          // @0x546857
        round.parent_spawn_id = query.parent_spawn_id;
        round.mass_q16 = 0;                   // +0x2A8 @0x54685A
        if (round.has_item_def) {
            // The copy carries the round's item, team, Flags and +0x114 word as
            // they stand ahead of this match's own clears; the model init over
            // it ORs the matrix-built bit for a null +452.
            copy.copied = true;
            copy.item_id = round.item_type_id;
            copy.team = round.team;
            copy.noage = round.noage;
            copy.flags = (round.indoors_flags ? kEntityFlagIndoors : 0u) |
                    (round.update_nulled ? kEntityFlagMatrixBuilt : 0u);
        }
        round.noage = false;                       // @0x546883
        round.max_age_ticks = round.age_ticks + 1; // +0x2AC = 1 @0x546892
    }
    return copy;
}

} // namespace opennova::world
