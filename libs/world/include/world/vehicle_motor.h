// The host-authoritative ground-vehicle motor — the drive response behind a mounted
// ctrl/drvr player (the retail-join v33 "can't drive" gap).
//
// WITNESS (net-re §5.13 drive-authority chain, 2026-07-04): vehicles have NO wire
// uplink — the client's per-frame C2S 0x0C serializes exactly ONE entity,
// g_local_player_entity [orig: Client_ProcessNetworkFrame @0x42c180 call @0x42c482],
// and the vehicle serialize callback rejects the extended modes 3/4 with -1
// [orig: Entity_SerializeVehicleState @0x460560 mode switch @0x460578/0x460580;
// golden ASH_I5A: 344/344 C2S 0x0C bodies carry the player handle]. Drive is therefore
// HOST-SIDE: the vehicle motor consumes the CONTROLLING occupant's replicated input
// (MoveOrder / heading / analog axes — all landed by the 0x0C apply) when
// `itemDef->attrib & 0x40` (PlayerControl) and this machine is the authority (the
// driver's own client runs the same block as local prediction)
// [orig: Entity_UpdateVehiclePhysics @0x48af00 gate @0x48b0ff:
//  `(occupant->Flags & 0x100) && (occupant == g_local_player_entity || is_authority)`].
//
// This port is the AUTHORITY drive core for the ground family (items.def `physics`
// selector non-zero routes here [orig: Entity_DispatchPhysics_cveh @0x48efc0]):
// input mapping, steering chase, speed pipeline, velocity integration, gravity and a
// terrain ground clamp. Tracked deferrals (D-NET-161): the air/helicopter family
// (`move_function chel` — Super Pumas stay parked), the skid/tire-slip model
// (`tireSlip`/`slip_speed`; the ASH buggy authors slip_speed 0), the pool-1
// vehicle-vs-vehicle collision loop + collision-avoid damping, water
// drag/drowning drain (the plane exists but motor physics does not consume it), the
// AI autopilot/waypoint drive (states 16/18), specialized vehicle sound families
// beyond the ground idle/drive/reverse pass in vehicle_sound.cpp, the
// wheel-contact pitch/roll solver (Entity_ProcessTrackedVehiclePhysics — substituted
// by the bilinear terrain clamp), and the vehicle AI state machine's non-drive states
// [orig: EntityAI_ProcessVehicleStateMachine @0x4583c0].
#ifndef OPENNOVA_WORLD_VEHICLE_MOTOR_H
#define OPENNOVA_WORLD_VEHICLE_MOTOR_H

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "world/entity.h"

namespace opennova::world {

class World;

// Per-item vehicle physics parameters, PRE-SCALED by the items.def parser exactly like
// the original loader [orig: ItemDef_ParsePhysicsProperty @0x49d870]:
// player_speed km/h*293 (16.16 u/tick), turn rates deg/s*192426 (BAM/tick),
// accel/decel token*4. The host's item-traits sweep fills the table from the item db
// (NovaSimulation::resolve_item_traits); tests stamp it directly.
struct VehicleTraits {
    int32_t physics = 0;       // itemDef+0x8DC selector; 0 = never runs the vehicle motor
    int32_t player_speed = 0;  // itemDef+0x8E8
    int32_t acceleration = 0;  // itemDef+0x8E0
    int32_t deceleration = 0;  // itemDef+0x8E4
    int32_t turn_rate = 0;     // itemDef+0x924
    int32_t turn_rate2 = 0;    // itemDef+0x928 (low-speed minimum rate override)
    int32_t torque = 0;        // itemDef+0x91C raw — the collision speed-decay shift count
                               // [orig: parse @0x49dcca; severity handlers @0x47cc13-cc1]
    int32_t unit_type = 0;     // minimap icon class (5..8 helo, 3/4 boat, 12 special,
                               // else ground) [orig: Entity_ClassifyForMinimap @0x50FA70]
    bool player_control = false; // ItemDefAttrib & 0x40 — gates the occupant input block
    // The VEHICLE item's own authored sound binding. This deliberately does not
    // borrow the mounted NPC's AiProfile: pool-1 vehicles need sound even when no
    // AiEntity body exists for them. Profile slots seed soundloop_1..7, then a
    // non-empty item-level soundloop_N overrides the corresponding set name.
    // [orig: ItemDef_ResolveAllResources @0x49e5f0/@0x49e7f0]
    std::string sound_profile;
    std::array<std::string, 7> sound_loops{};
};

// items.def type-id -> traits. World-level like the weapon/ammo tables.
class VehicleTraitsTable {
public:
    void set(int32_t item_id, const VehicleTraits &t) { by_item_[item_id] = t; }
    const VehicleTraits *get(int32_t item_id) const {
        auto it = by_item_.find(item_id);
        return it == by_item_.end() ? nullptr : &it->second;
    }
    bool empty() const { return by_item_.empty(); }
    void clear() { by_item_.clear(); }

private:
    std::unordered_map<int32_t, VehicleTraits> by_item_;
};

// The controlling occupant of a PlayerControl vehicle: the first live, internally
// consistent Controller/Driver seat occupant, with the per-tick stale-slot sweep and the
// +368 claimant validation. [orig: the occupant sweep @0x48b8a1-0x48b944 in
// Entity_UpdateVehiclePhysics @0x48af00]
Entity *resolve_vehicle_controller(World &world, Entity &veh);

// The AI-driver command block, computed by the AI system from the vehicle's brain (the
// witnessed leg lives inside the vehicle physics; our brain state is AiSystem-owned, so
// the math runs there and the motor consumes the result — a structural seam, not a
// behavioral one). [orig: Entity_UpdateVehiclePhysics @0x48af00 — parked stamp
// @0x48c002-0x48c02d, AI-driver leg @0x48bc12-0x48c034]
struct VehicleDriveCmd {
    bool ai_drive = false;        // an AI controller is seated: consume the fields below
    int32_t steer_target_bam = 0; // [orig: aiComp[132] = Yaw + delta + (delta >> 3)]
    int32_t cmd_speed = 0;        // [orig: aiComp[136] = min(brain outSpeed, playerSpeed)]
};

// The two semantic CTRL values published by the retail cveh render path from
// the vehicle's live motor fields. Keeping this projection beside the state
// owner gives rendering and native tests one implementation of the original
// word selection, wrapping absolute value, and saturation rules.
struct VehicleCtrlRegisters {
    int32_t steering = 0;
    int32_t speed = 0;
};

VehicleCtrlRegisters vehicle_ctrl_registers(
        const Entity::VehicleMotorState &state);

// One authority tick of the ground-vehicle motor for `veh` (a pool-1 entity whose
// traits carry a non-zero `physics` selector). Consumes the controlling occupant's
// replicated input (or the AI-driver command when the controller is an NPC), advances
// Entity::position / Entity::yaw and the persistent Entity::veh motor state.
// [orig: Entity_UpdateVehiclePhysics @0x48af00 — the authority drive core;
// block-level cites inline]
void tick_vehicle_motor(World &world, Entity &veh, const VehicleTraits &traits,
                        const VehicleDriveCmd *ai_cmd = nullptr);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_VEHICLE_MOTOR_H
