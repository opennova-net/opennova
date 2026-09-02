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
// input mapping, steering chase, speed pipeline, velocity integration, gravity,
// submerged drag, and the per-family contact + suspension solves
// (tracked @0x47C1C0 for cveh/ctrn/catv, wheeled @0x475DE0 for ctan/Tank,
// light @0x479600 for cbik/Bike — client subsets: pad probes at wheel height,
// per-corner lifts, the 4-normal/axle attitude fits, the positive-corner or
// mean-wheel rest Z; vehicle-client-movers-re.md §7-§9; boxless/terrain-less
// rows keep the bilinear terrain-clamp stand-in). Tracked deferrals (D-NET-161):
// the air/helicopter AUTHORITY mover (`move_function chel` — Super Pumas stay
// parked), the skid/tire-slip model (`tireSlip`/`slip_speed`; the ASH buggy
// authors slip_speed 0), the pool-1 vehicle-vs-vehicle collision loop +
// collision-avoid damping, the authority drown-drain countdown, the solve's
// contact-direction store feeding a slope-following velocity re-derive
// (@0x47E65D../@0x48cf97.. — the mover keeps its level frame; the tank keeps
// its full-basis drive with the same store deferred), the AI
// autopilot/waypoint drive (states 16/18), specialized vehicle sound families
// beyond the ground idle/drive/reverse pass in vehicle_sound.cpp, and the
// vehicle AI state machine's non-drive states
// [orig: EntityAI_ProcessVehicleStateMachine @0x4583c0].
#ifndef OPENNOVA_WORLD_VEHICLE_MOTOR_H
#define OPENNOVA_WORLD_VEHICLE_MOTOR_H

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <runtime/world/entity.h>

namespace opennova::world {

class World;

// Per-item vehicle physics parameters, PRE-SCALED by the items.def parser exactly like
// the original loader [orig: ItemDef_ParsePhysicsProperty @0x49d870]:
// player_speed km/h*293 (16.16 u/tick), turn rates deg/s*192426 (BAM/tick),
// accel/decel token*4. The host's item-traits sweep fills the table from the item db
// (Simulation::resolve_item_traits); tests stamp it directly.
// The items.def move_function family tag, the per-class mover selector (the
// update-callback table keys on it [orig: the [tag,flags,callback] rows
// @0x82ABC0; net-re §5.38e]). Ground covers cveh/ctan/ctrn/catv;
// Bike covers cbik.
enum class VehicleFamily : uint8_t {
    Ground = 0,
    Watercraft, // cbot -> Entity_UpdateWatercraftPhysics @0x48D480
    Helicopter, // chel/CHel -> Entity_UpdateAircraftPhysics @0x490310
    Plane,      // cpln
    Bike,       // cbik -> Entity_UpdateLightVehiclePhysics @0x483FE0. Shares the
                // ground template; the witnessed family deltas gate on this tag
                // inside the core (gravity 250, vZ up-cap, airborne
                // throttle/integration, yaw always-applied >>2 in water)
                // [cbik grill 2026-07-31], and the contact solve is the light
                // variant [orig: Entity_ProcessLightVehiclePhysics @0x479600,
                // call @0x486672].
    Tank,       // ctan -> Entity_UpdateTankVehiclePhysics @0x488AB0 (its own
                // class-table row @0x82ABC0). The ground template with the
                // witnessed tank deltas (gravity 250, contact-gated speed
                // integration with the ±2·deceleration reversal clamps,
                // full-basis drive velocity, yaw applied unless parked with the
                // airborne quarter-rate), and the contact solve is the wheeled
                // variant [orig: Entity_ProcessWheeledVehiclePhysics @0x475DE0,
                // call @0x48a9ef; dispatcher push 0 @0x48f004 = no
                // water support].
};

// CHel/cpln occupy direct rows in g_EntityClassPhysicsTable. Unlike the
// cveh/cbot/etc. dispatcher thunks, their family mover does not test the
// items.def ground `physics` selector before running.
constexpr bool vehicle_family_uses_direct_air_mover(VehicleFamily family) {
    return family == VehicleFamily::Helicopter ||
           family == VehicleFamily::Plane;
}

struct VehicleTraits {
    int32_t physics = 0;       // itemDef+0x8DC ground-dispatch selector; direct air ignores it
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
    VehicleFamily family = VehicleFamily::Ground; // move_function tag (§5.38e movers)
    bool amphibian = false;    // move_function catv: the generic dispatcher passes
                               // hasWaterLevel=2 into the ground mover, arming the
                               // contact solve's pad water-support forces; cveh/ctrn
                               // pass 0 [orig: @0x48f010 vs @0x48efce/@0x48f06e]
    int32_t water_speed = 0;   // itemDef+0x8EC waterSpeed — the cbot family's max
                               // drive speed (the same slot the ground family
                               // reads as playerSpeed) [orig: @0x48E835]
    int32_t climb_speed = 0;   // itemDef+0x920 — air vertical clamp [+cs, -2cs]
    int32_t turn_roll = 0;     // itemDef+0x90C raw — air roll-rate cap (*192426)
    int32_t speed_pitch = 0;   // itemDef+0x910 raw — air pitch-rate cap (*192426)
    int32_t max_slope = 0;     // itemDef+0x8F4 BAM (deg token * 11930464) — the
                               // platform slope-soft threshold (cos22 at use)
    int32_t slip_slope = 0;    // itemDef+0x8F8 BAM — the slope-hard threshold
    // The platform-solve tuning block (all raw tokens; vehicle-client-movers-re.md §3):
    int32_t mass = 0;          // itemDef+0x908 — weight class (<=1 light) + momentum
    int32_t lean = 0;          // itemDef+0x92C — planing roll-lean machine @0x45AEA0
    int32_t lean_velocity = 0; // itemDef+0x930
    int32_t pitch_lift = 0;    // itemDef+0x934 ("pitch") — bow-lift threshold scale
    int32_t pitch_lift_vel = 0;// itemDef+0x938 ("pitch_velocity") — lift amount scale
    int32_t bob = 0;           // itemDef+0x93C — porpoise exit fold
    int32_t flip = 0;          // itemDef+0x948 — ground movers' tip threshold (*0.01)
    int32_t hand_brake = 0;    // itemDef+0x944 raw — arms the byte-973 stop latch
                               // [orig: @0x48c03a `occupant && Flags & 8 && handBrake`]
    // The AI crew clamp pair [orig: minAI +0x8D8 @0x48bc51, criticalHp +0x180
    // @0x48bc7d]: an undercrewed AI hull that has left its spawn anchor bleeds
    // to criticalHp (AiSystem::apply_min_ai_crew_clamp).
    int32_t min_ai = 0;        // itemDef+0x8D8 raw — the crew count threshold
    int32_t critical_hp = 0;   // itemDef+0x180 i16 raw — the clamp ceiling
    // The aircraft mover's authority health machine [orig: the every-64th-tick
    // block of Entity_UpdateAircraftPhysics @0x4903F4..0x4904A7]: above
    // criticalHp the hull regens nonCriticalRegen up to healthMax - regen; at or
    // below it the hull burns criticalDrain per 64 ticks.
    int32_t critical_drain = 0;     // itemDef+0x182 i16 raw
    int32_t non_critical_regen = 0; // itemDef+0x184 i16 raw
    // The suspension spring block (world/vehicle_suspension.cpp; raw tokens)
    // [orig: spring +0x8FC, spring_comp +0x900, shock +0x904 —
    //  ItemDef_ParsePhysicsProperty @0x49db5c/@0x49dbd4/@0x49dc10]. The def's
    // top_heavy (+0x918) is parsed for parity but DEAD in retail (the parser,
    // the allocator and the debug item editor are its only readers) — nothing
    // here carries it.
    int32_t spring = 0;        // the per-wheel spring constant k
    int32_t spring_comp = 0;   // travel percentage: (100 - spring_comp) scales 0xFFFF
    int32_t shock = 0;         // the landing damp (11 - shock)/11; the oscillator
                               // clamps THIS field to [0,10] in place, as retail
                               // clamps the def's (@0x45D18F..0x45D1A2)
    // Platform probe geometry from the model bound boxes (16.16 model space;
    // modelData [0x28..0x3C] + the [0x40..0x4C] footprint). Provenance
    // witnessed 2026-08-12: box Z = the CMDL header bbox Z pair, box X/Y =
    // the lower-half type-1 BVOL fold, footprint = the bottom-eighth fold
    // with the q+0x2000 clamps [orig: Threedi_BuildCollisionModelFromChunks
    // @ 0x5b3bf0 tail @ 0x5b4455..0x5b45db]; filled from
    // threedi_3di3_collision_probe_boxes at collision resolve:
    int32_t box_z_lo = 0, box_z_hi = 0; // keel/deck Z pair
    int32_t box_x_lo = 0, box_x_hi = 0; // length pair
    int32_t box_y_lo = 0, box_y_hi = 0; // beam pair
    int32_t foot_x_lo = 0, foot_x_hi = 0; // footprint length pair
    int32_t foot_y_lo = 0, foot_y_hi = 0; // footprint beam pair
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
    VehicleTraits *get_mutable(int32_t item_id) {
        auto it = by_item_.find(item_id);
        return it == by_item_.end() ? nullptr : &it->second;
    }
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
    // The part-animation words: the rotor angle accumulator's high word for
    // HELO_ROTOR and HELO_TAILROTOR, the wheel phase's for VEHICLE_WHEELS
    // [orig: Entity_CacheVehicleHUDStats @0x4929B0 — +0x466 @0x492ACA..
    //  0x492ADE, +0x2BA @0x4929B4]. Same MOVZX idiom as `steering`.
    // HELO_TAILROTOR (ordinal 47) reads the SAME +0x464 accumulator as
    // HELO_ROTOR (46) [orig: the shared HIWORD(+0x464) store @0x492AD7].
    int32_t rotor = 0;
    int32_t tail_rotor = 0;
    int32_t wheels = 0;
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

// The JOINER-side watercraft mover (net-re §5.38e, D-NET-196): the client-executed
// subset of the cbot family function — per-record chase plus local-driver input
// or remote register mirroring, steer/thrust/drag/keel prediction, contact drags,
// and X/Y/yaw integration [orig: Entity_UpdateWatercraftPhysics @0x48D480].
// The local driver reconciles longitudinal command with the received register
// while retaining local steer. Z, pitch/roll, and the
// afloat/airborne latches come from the platform solve below. Consumes the staged
// VehicleMotorState net_* cluster; the sim runs it once per world tick on a
// non-authority world for staged pool-1 Watercraft entities.
// The boat platform solve — buoyancy, hull attitude, and the airborne/afloat
// flags, run every tick after integration exactly where the retail caller sits
// [orig: Entity_ProcessPlatformPhysics @0x481870, called @0x48ECE7; client
// subset — the authority damage/latch legs, entity-entity collision, the
// planing lean machine @0x45AEA0, and the wreck-tumble path are cited
// deferrals]. Writes veh.veh.air_pitch_bam/air_roll_bam (the shared attitude
// fields the sim mirrors to the presented row) and position Z.
void watercraft_platform_solve(World &world, Entity &veh,
                               const VehicleTraits &traits);

// The carrier pose in the deck-ride's units (16.16 position / BAM32
// attitude): predicted vehicles serve the exact motor registers, everything
// else converts the presented pose. ONE reader shared by the mover-entry
// savedLivePose stamp and the embedder's carrier provider, so stamped and
// live values can never diverge in representation.
void carrier_pose_fixed(const Entity &e, int32_t pos[3], int32_t &yaw,
                        int32_t &pitch, int32_t &roll);

// Stamp the mover-entry savedLivePose from the same reader [orig: the
// per-mover prologue stamps of +0x80..+0x94].
void stamp_saved_live_pose(Entity &e);

void watercraft_client_tick(World &world, Entity &veh, const VehicleTraits &traits);

// One AUTHORITY tick of the watercraft motor (the host-side cbot mover): the
// occupant/AI/parked input staging behind the witnessed gate, capsize damage,
// then the same steer/thrust/drag/contact/integration core the client subset
// runs. The AI command block comes from AiSystem::watercraft_ai_drive.
// [orig: Entity_UpdateWatercraftPhysics @0x48D480 — the authority path behind
// the @0x48DF8C..0x48DFA2 input gate; witnessed 2026-08-06]
void tick_watercraft_motor(World &world, Entity &veh, const VehicleTraits &traits,
                           const VehicleDriveCmd *ai_cmd = nullptr);

// The GROUND/Bike prediction leg: shared chase + local-driver input or mirrored
// remote registers driving tick_vehicle_motor's core with the input block bypassed;
// the Bike family selects its witnessed gravity/contact/yaw deltas in that core.
void ground_client_tick(World &world, Entity &veh, const VehicleTraits &traits);

// The AIR-family prediction leg (CHel + cpln — one mover, the plane callback is
// a thunk): the client subset of Entity_UpdateAircraftPhysics @0x490310 —
// three-register mirror, tilt-command attitude model, altitude-hold servo on the
// record-seeded target Z (no gravity constant), airborne aero / grounded sheds,
// and the terrain-clamp stand-in for the unported 0x47EF10 contact solve.
void aircraft_client_tick(World &world, Entity &veh, const VehicleTraits &traits);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_VEHICLE_MOTOR_H
