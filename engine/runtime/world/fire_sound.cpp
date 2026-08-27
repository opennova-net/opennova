// The fire-sound legs on the logic clock — witness map in world/fire_sound.h.
#include <runtime/world/fire_sound.h>

#include <runtime/world/ammo_table.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

#include <cmath>

namespace opennova::world {

void FireSoundQueue::push_ready(const char *set_name, const Vec3 &pos,
                                int32_t source_bms_id) {
    if (set_name == nullptr || set_name[0] == '\0') return;
    if (ready_.size() >= kMaxReady) return;
    ReadyFireSound sound;
    sound.set_name = set_name;
    sound.pos = pos;
    sound.source_bms_id = source_bms_id;
    ready_.push_back(std::move(sound));
}

void FireSoundQueue::play_immediate(const char *set_name, const Vec3 &pos,
                                    int32_t source_bms_id) {
    push_ready(set_name, pos, source_bms_id);
}

void FireSoundQueue::play_with_distance_delay(const char *set_name,
                                              const Vec3 &pos,
                                              int32_t source_bms_id) {
    if (set_name == nullptr || set_name[0] == '\0') return;
    // The peer gate lives INSIDE the witnessed function: a host with no
    // stamped listener (dedicated) plays nothing [orig: the
    // is_mp_session_peer test at the head of Sound_PlayWithDistanceAttenuation
    // @ 0x528e57].
    if (!listener_valid_) return;
    // Truncating distance in integer units — retail loads the 16.16 deltas as
    // floats, sqrts, truncates the fixed value and shifts the fraction off
    // [orig: @ 0x528e7c..0x528ec0 (ftol then >> 16)].
    const double dx = static_cast<double>(pos.x - listener_.x);
    const double dy = static_cast<double>(pos.y - listener_.y);
    const double dz = static_cast<double>(pos.z - listener_.z);
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int32_t dist_units =
            dist >= 2147483647.0 ? 2147483647 : static_cast<int32_t>(dist);
    // The set's max-range gate ran here in retail (soundDef+72 @ 0x528ec6);
    // our audio bank culls at play time instead — the tracked D-AI-8 delta.
    if (dist_units >= kMinDelayDistUnits) {
        // [orig: the delay formula @ 0x528ef2 — integer division, then >> 2,
        //  the witnessed quarter-compression of physical travel time]
        int32_t countdown = (62 * dist_units / kSoundSpeedUnits) >> 2;
        if (countdown == 0) countdown = 1; // [orig: @ 0x527c94]
        for (PendingFireSound &slot : slots_) {
            if (slot.active) continue;
            slot.active = true;
            slot.set_name = set_name;
            slot.pos = pos;
            slot.countdown = countdown;
            slot.source_bms_id = source_bms_id;
            return;
        }
        // Full pool: the retail allocator returns without a slot — the sound
        // is dropped [orig: @ 0x527c47].
        return;
    }
    push_ready(set_name, pos, source_bms_id);
}

void FireSoundQueue::tick() {
    for (PendingFireSound &slot : slots_) {
        if (!slot.active) continue;
        if (--slot.countdown > 0) continue;
        // Plays at the RECORDED fire-time position [orig: @ 0x52937b].
        push_ready(slot.set_name.c_str(), slot.pos, slot.source_bms_id);
        slot.active = false;
        slot.set_name.clear();
    }
}

std::vector<ReadyFireSound> FireSoundQueue::drain() {
    std::vector<ReadyFireSound> out;
    out.swap(ready_);
    return out;
}

void FireSoundQueue::clear() {
    for (PendingFireSound &slot : slots_) {
        slot.active = false;
        slot.set_name.clear();
    }
    ready_.clear();
    // The listener survives a mission restart on purpose: retail's listener
    // global is camera state, not per-mission state.
}

int FireSoundQueue::pending_count() const {
    int count = 0;
    for (const PendingFireSound &slot : slots_) {
        if (slot.active) ++count;
    }
    return count;
}

void fire_sound_on_spawn(World &world, const RoundSpawnParams &params) {
    FireSoundQueue &queue = world.fire_sounds;
    // A host with no stamped listener presents nothing — the dedicated-server
    // gate [orig: is_mp_session_peer @ 0x528e57].
    if (!queue.listener_valid()) return;
    // The local player's own fire keeps its action-slot presentation; the
    // shell self-filtered exactly this case before the move.
    // [orig: ActionSlot_ExecuteActionTick @ 0x541A70 routing]
    if (world.cached.local_player.valid() &&
            params.owner == world.cached.local_player)
        return;
    const Entity *shooter = world.registry.get(params.owner);
    const int32_t source_bms_id = shooter != nullptr ? shooter->bms_id : 0;
    const bool adm_arm =
            (params.wire_round_flags & round_event_flag::kAltFire) == 0 &&
            (params.wire_round_flags & round_event_flag::kAdmIndexed) != 0;
    if (adm_arm) {
        // The adm-indexed arm plays no ammo-def sound. It executes the
        // addressed def's FIRE and RECOIL action rows; each row plays its
        // soundset at begin AND its soundsetend (the receive path stamps the
        // one-shot mode so the end shim runs immediately), all positional at
        // the SHOOTER'S entity position with no distance delay — JOX fire
        // rows author the audible gunshot in soundsetend.
        // [orig: ActionSlot_ExecuteAction @ 0x4020a0 — soundset @ 0x4020ef,
        //  the +90==64 one-shot stamp gate @ 0x4020ff into
        //  ActionSlot_PlayEndSoundAndDupes @ 0x401100 (both at entity+4);
        //  rows +684/+688 @ 0x42f777/@ 0x42f785 and @ 0x42f98f/@ 0x42f9d0]
        const WeaponTableEntry *def = world.weapons.by_index(params.adm_index);
        if (def == nullptr) return;
        // The shooter's entity position; a decoded wire shooter with no local
        // entity supplies its row position through the spawn params, and the
        // wire fire origin (the eye) is the last resort.
        const Vec3 pos = shooter != nullptr ? shooter->position
                : params.shooter_pos_valid ? params.shooter_pos
                                           : params.origin;
        constexpr int32_t kRows[2] = {weapon_action::kFire,
                weapon_action::kRecoil};
        for (const int32_t row_id : kRows) {
            const WeaponFsmAction &row = def->action_fsm.actions[row_id];
            queue.play_immediate(row.soundset, pos, source_bms_id);
            queue.play_immediate(row.soundsetend, pos, source_bms_id);
        }
        return;
    }
    // The ammo-def arm (wire bit 0) and host/AI-originated fire (zero flags)
    // both play ammoDef+64 at the fire origin through the distance gate.
    // [orig: WeaponSlot_FireAndSpawnEffects @ 0x53f440 ->
    //  Sound_PlayWithDistanceAttenuation @ 0x528e40; the wire ammo arm
    //  @ 0x42f5dc]
    const AmmoTableEntry *ammo = world.ammo.by_index(params.ammo_index);
    if (ammo == nullptr || ammo->ai_launch_set.empty()) return;
    queue.play_with_distance_delay(ammo->ai_launch_set.c_str(), params.origin,
            source_bms_id);
}

} // namespace opennova::world
