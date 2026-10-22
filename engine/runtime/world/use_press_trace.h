// The USE readout: a debug instrument with no behaviour. It records what each USE-ITEM press
// met on its way to the wire, for the use_press_trace probe (godot/probes/net) and its
// binding (Simulation::debug_use_press_trace). Nothing in the game reads it.
//
// Three producers append, in the order they run:
// - kPress: each run of the held row's case 0xB1 (LocalPlayer::take_use_item_press, once per
//   frame a USE key is down). It records the arm the press took, the local body's zone and
//   queued-mount Flags bits, its mount, and the active slot's current / next action.
// - kToggleGate: the release's mount toggle gate (LocalPlayer::mount_toggle_allowed). It
//   records the gate's answer and the slot's actions as the gate read them.
// - kToggleScan: a joiner's seat scan and request (inmatch::JoinerRole::queue_mount_toggle).
//   It records find_mount_toggle_candidate's answer and pick, and how the request ended.
//   For a queued 0x26 it also records the runtime's framed-0x26 count at queue time
//   (inmatch::ClientRuntime::attach_requests_sent), so a later readout of that count shows
//   whether the request left.
// Every record also carries the watched entity's scan inputs (UsePressTrace::watch, one
// registry handle the reader names): whether it exists, its Flags words, whether it holds
// a row in the pool-1 dynamic table and that row's radius, and whether the local body's
// candidate slice holds it. The scan reads all of them from host-sent state and reports
// nothing for an entity it rejects.
#pragma once

#include <runtime/world/entity.h>
#include <runtime/world/local_player_view.h> // UseItemArm
#include <runtime/world/weapon_fsm.h>        // WeaponSlotState

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::world {

enum class UsePressEvent : uint8_t {
    kPress = 0,
    kToggleGate = 1,
    kToggleScan = 2,
};

// How a joiner's seat scan and request ended (kToggleScan).
enum class UseToggleOutcome : uint8_t {
    kNone = 0,      // not a scan record
    kNoPlayer,      // no local body
    kSeatbelt,      // mounted with the WAC seatbelt set
    kDetachQueued,  // no candidate while mounted: the 0x27 queued
    kDetachRefused, // no candidate while mounted: queue_vehicle_detach refused
    kNoCandidate,   // no candidate on foot
    kBadSeat,       // the candidate's seat does not resolve on its carrier
    kAttachQueued,  // the 0x26 queued
    kAttachRefused, // queue_vehicle_attach refused
};

// The Flags bits the press's arm and the toggle's queued-mount arm key on: the type-11
// volume touch, the type-6 volume touch and the queued spawn-marker mount.
inline constexpr uint32_t kUsePressFlagMask =
        kEntityFlagVehicleLoadoutZone | kEntityFlagArmoryZone | kEntityFlagQueuedMount;

struct UsePressRecord {
    uint32_t seq = 0;
    uint32_t logic_tick = 0; // World::logic_tick, the scripted device's clock
    UsePressEvent event = UsePressEvent::kPress;
    UseItemArm arm = UseItemArm::kMountChain; // kPress
    bool has_player = false;
    // The local body's Flags dword masked by kUsePressFlagMask, in our two words
    // (Entity::flags, Entity::engine_flags).
    uint32_t flags = 0;
    uint32_t engine_flags = 0;
    bool mounted = false;
    bool has_slot = false; // an active weapon slot, its actions below
    int32_t slot_current = 0;
    int32_t slot_next = 0;
    bool allowed = false; // kToggleGate
    UseToggleOutcome outcome = UseToggleOutcome::kNone; // kToggleScan
    bool candidate = false;
    uint16_t vehicle = 0xFFFFu;
    int16_t seat_index = -1;
    uint8_t bone = 0;
    uint32_t attach_sent_before = 0;
    // The watched entity (UsePressTrace::watch; 0xFFFF none). Its Flags words whole: bit 0
    // keeps it out of the pool-1 table, bit 1 is the dead bit.
    // [orig: Entity_BuildProximityLists_Pool01 @0x4B9340 - the Flags & 1 skip @0x4B9382,
    //  the radius copy @0x4B9389; Entity_BuildProximityListsFromPools @0x4B8EB0 - the slice
    //  arena stores @0x4B8FD6]
    uint16_t watch = 0xFFFFu;
    bool watch_present = false;
    uint32_t watch_flags = 0;
    uint32_t watch_engine_flags = 0;
    bool watch_in_table = false;
    int32_t watch_table_radius_q16 = 0;
    bool watch_in_slice = false;
    int32_t slice_count = -1; // the local body's candidate slice; -1 no slice
};

// The last kCapacity records; seq counts every record appended.
class UsePressTrace {
public:
    static constexpr std::size_t kCapacity = 64;
    // The registry handle whose scan inputs every record carries (0xFFFF none).
    uint16_t watch = 0xFFFFu;

    void append(UsePressRecord record) {
        record.seq = next_seq_++;
        ring_[record.seq % kCapacity] = record;
    }
    uint32_t next_seq() const { return next_seq_; }
    uint32_t oldest_seq() const {
        return next_seq_ > kCapacity ? next_seq_ - static_cast<uint32_t>(kCapacity) : 0u;
    }
    // The held records with seq >= from, oldest first.
    std::vector<UsePressRecord> since(uint32_t from) const {
        std::vector<UsePressRecord> out;
        for (uint32_t seq = from > oldest_seq() ? from : oldest_seq(); seq < next_seq_; ++seq)
            out.push_back(ring_[seq % kCapacity]);
        return out;
    }

private:
    std::array<UsePressRecord, kCapacity> ring_{};
    uint32_t next_seq_ = 0;
};

// The record's body words: the local body (null: none) and its active slot (null: none).
inline void use_press_fill_body(UsePressRecord &record, const Entity *player,
        const WeaponSlotState *slot) {
    record.has_player = player != nullptr;
    if (player != nullptr) {
        record.flags = player->flags & kUsePressFlagMask;
        record.engine_flags = player->engine_flags & kUsePressFlagMask;
        record.mounted = player->mounted;
    }
    record.has_slot = slot != nullptr;
    if (slot != nullptr) {
        record.slot_current = slot->current;
        record.slot_next = slot->next;
    }
}

inline const char *use_press_event_name(UsePressEvent event) {
    switch (event) {
        case UsePressEvent::kPress: return "press";
        case UsePressEvent::kToggleGate: return "toggle_gate";
        case UsePressEvent::kToggleScan: return "toggle_scan";
    }
    return "unknown";
}

inline const char *use_item_arm_name(UseItemArm arm) {
    switch (arm) {
        case UseItemArm::kMountChain: return "mount_chain";
        case UseItemArm::kArmory: return "armory";
        case UseItemArm::kArmoryRefused: return "armory_refused";
        case UseItemArm::kVehicleLoadout: return "vehicle_loadout";
        case UseItemArm::kDropped: return "dropped";
    }
    return "unknown";
}

inline const char *use_toggle_outcome_name(UseToggleOutcome outcome) {
    switch (outcome) {
        case UseToggleOutcome::kNone: return "none";
        case UseToggleOutcome::kNoPlayer: return "no_player";
        case UseToggleOutcome::kSeatbelt: return "seatbelt";
        case UseToggleOutcome::kDetachQueued: return "detach_queued";
        case UseToggleOutcome::kDetachRefused: return "detach_refused";
        case UseToggleOutcome::kNoCandidate: return "no_candidate";
        case UseToggleOutcome::kBadSeat: return "bad_seat";
        case UseToggleOutcome::kAttachQueued: return "attach_queued";
        case UseToggleOutcome::kAttachRefused: return "attach_refused";
    }
    return "unknown";
}

} // namespace opennova::world
