// Vehicle attach/detach — the authoritative relationship operations behind the
// C2S 0x26/0x27 wire handlers, WAC/BMS mount commands, use-key mounting, and
// mobile-spawn deployment. Handle-keyed and validation-ordered per the originals.
//
// [orig: NapiNPServerMsg_HandleVehicleAttach @0x502390 -> Entity_ProcessVehicleAttach
//  @0x435AA0 -> Entity_AttachToVehicleSlot @0x4946D0 / Entity_AttachToUseGunSlot @0x546B80;
//  NapiNPServerMsg_HandleVehicleDetach @0x4FC980 -> Entity_DetachFromVehicle @0x4355F0.
//  There is NO reply message: the 0x0A compact player record's mounted branch is the
//  confirmation for everyone including the requester.]
#ifndef OPENNOVA_WORLD_VEHICLE_ATTACH_H
#define OPENNOVA_WORLD_VEHICLE_ATTACH_H

#include <cstdint>
#include <string>

#include <runtime/world/entity.h>
#include <runtime/world/vehicle_mount.h>

#include <vector>

namespace opennova::world {

class World;



// One selected vehicle seat, or an armory point returned by the proximity scan.
struct VehicleSeatSelection {
    EntityHandle vehicle;
    int seat_index = -1; // seat index, or the armory_points index in armory mode
    SeatType type = SeatType::None;
};

// The WAC/AI attach-to-seat command ids: 123 accepts sitex (passenger)
// seats only, 124 rejects ctrlx (controller) seats, 125 takes any seat by
// the normal best-seat priority [orig: the Entity_RequestVehicleAttach
// command gates; entity_commands.h].
inline constexpr int kCommandAttachPassengerOnly = 123;
inline constexpr int kCommandAttachSkipController = 124;
inline constexpr int kCommandAttachAnySeat = 125;
// The seat filter an attach command selects; false = not an attach command.
bool seat_selection_mode_for_command(int command_id, SeatSelectionMode &out);

// The witnessed seat priority weights (lower wins): root control/driver
// 0x2000, Gunner 0x20000, root Passenger 0x200000, child Passenger
// 0x2000000 — the table find_best_vehicle_seat walks.
// [orig: Entity_FindBestSeatSlot @0x4351F0]
int32_t seat_priority_weight(SeatType type, bool root_seat);

// The tooling mirror of that selection over a flat seat list (root seats,
// no child walk) for the MCP mission tools and probes: each candidate's
// verdict (an occupied seat is skipped before the command filter; a None
// type or a non-attach command never qualifies) and the pick — the lowest
// weight, the first on a tie; -1 when none. `mode` null = not an attach
// command.
struct SeatCandidate {
    SeatType type = SeatType::None;
    bool occupied = false;
};
enum class SeatVerdict : uint8_t { kEligible = 0, kSkippedOccupied, kSkippedCommand, kSelected };
int predict_seat_selection(const std::vector<SeatCandidate> &seats,
                           const SeatSelectionMode *mode,
                           std::vector<SeatVerdict> &verdicts);

// FindBestSeatSlot's weighted root+child walk. The requested root is considered
// first, followed by live entities whose ground_target is that root. Controller
// and Driver are root-only; lower weights win: root control/driver 0x2000,
// Gunner 0x20000, root Passenger 0x200000, child Passenger 0x2000000. A seat
// occupied by the requester remains eligible.
// [orig: Entity_FindBestSeatSlot @0x4351F0]
bool find_best_vehicle_seat(
        const World &world, EntityHandle root_vehicle, EntityHandle occupant,
        VehicleSeatSelection &out,
        SeatSelectionMode mode = SeatSelectionMode::Any);



// The USE-ITEM mount toggle's weapon-busy gate [orig: Entity_ToggleVehicleMount
// @0x436958-0x436977 — no EquippedSlot passes; currentAction < 2
// (idle/emptyidle) or == 5 (the dry click) passes, as does a pending
// OVERHEATED (nextAction == 11); an in-flight fire/reload/switch swallows the
// toggle].
bool weapon_state_allows_mount_toggle(int32_t current_action, int32_t next_action);


// One floating attach label [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 — the
// selection half; projection and drawing stay host-side]. world_pos carries the witnessed
// +0.1875 u label lift [orig: point.z = boneZ + 12288 @0x5a3585].
struct AttachLabel {
    EntityHandle entity;
    int seat_index = -1;              // seat index; armory-point index when armory
    SeatType type = SeatType::None;   // Passenger/Controller/Driver -> Sit/Control text,
                                      // Gunner -> the weapon attachtextid, ArmoryPoint -> Armory
    bool armory = false;
    bool nearest = false;             // the full-bright highlight; others draw half-bright
    Vec3 world_pos;
    // The USEGUN label text key: the gun entity's primary weapon -> its
    // weapon.def attachtextid; empty for every other seat kind
    // [orig: Entity_GetWeaponSlots slot0 -> def+0x3A0 @0x5a351d].
    std::string attach_text_key;
};

// Optional deterministic work counters for attach-label performance tests and probes.
// The label gather is called every render frame; one gather may visit many proximity
// candidates, but it must not rescan the whole entity registry for every candidate.
struct AttachLabelScanStats {
    uint32_t enemy_occupancy_registry_passes = 0;
};



} // namespace opennova::world

#endif // OPENNOVA_WORLD_VEHICLE_ATTACH_H
