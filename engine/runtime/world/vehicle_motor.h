// Vehicle motors for authority and prediction. The controlling occupant supplies replicated input;
// the wire carries no separate client vehicle uplink. Full and selector-zero ground/water paths
// share their original dispatch, while aircraft uses vehicle_motor_air.cpp. Contact, traction,
// chassis, lifecycle, sound and trails are implemented in their named sibling modules. See
// docs/world/vehicle-client-movers-re.md sections 11-32.
// Witness sites: [orig: @0x42c180, @0x42c482, @0x460560, @0x460578, @0x48af00, @0x48b0ff,
// @0x48efc0, @0x47C1C0, @0x475DE0, @0x479600, @0x47E65D, @0x48cf97, @0x4583c0]
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <runtime/world/entity.h>

namespace opennova::world {

class World;
struct AiEntity;

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

// One of the two cbot-only, afloat wake slots retained from items.def. Unlike
// the general item particle slot, these are speed-controlled persistent groups
// and deliberately stay in the portable vehicle traits.
struct VehicleTrailDefinition {
	std::string effect;
	std::string secondary_effect;
	std::string userpoint;
	uint16_t mask = 0;
};

// Rigid model userpoints selected by the authored particlefxs mask.
// [orig: ItemDef_GetBoneMaskByName @0x49EA40; Entity_SpawnBoneEffectsAtMask @0x458750]
struct VehicleEffectPoint {
	int32_t position[3] = {};
	int32_t direction[3] = {};
};

struct VehicleEffectEvent {
	// Spawn: one transient effect at `position` along `direction`.
	// The three zone-group kinds carry the helicopter focal-wind pool's one
	// persistent surface-effect group per zone (`force_zone` names the zone):
	// Ensure creates it at the hit, Trigger re-spawns its children at a later
	// hit, Release retires it before the surface effect changes.
	// [orig: WeatherParticle_UpdateAllEmitters @0x5CB100, slot words +3/+4
	//  @0x5CB407..0x5CB49B; sub_5F6C10 @0x5F6C10]
	enum class Kind : uint8_t { Spawn, EnsureZoneGroup, TriggerZoneGroup, ReleaseZoneGroup };
	std::string effect;
	Vec3 position;
	Vec3 direction;
	uint32_t source_tick = 0;
	uint16_t force_zone = 0;
	Kind kind = Kind::Spawn;
};

// Selected independently of the mover by items.def render_function.
// [orig: renderer callback rows @0x82CFD0..0x82D00F]
enum class VehicleRenderFamily : uint8_t { None, Ground, Tank, Helicopter, Plane };

// The brain-machine class, selected by items.def ai_function through the class
// event-callback table (fn1 of each 24-byte row): CHel and cpln rows run the
// air-class machine, cveh/cbot/ctrn rows the vehicle-class one. Unset = the
// ai_function names no vehicle class (the tick then falls back to the mover
// family). [orig: g_EntityClassEventCallbackTable @0x813000, rows @0x8132a0
// CHel / @0x813378 cveh / @0x813390 cbot / @0x8133a8 cpln / @0x8133c0 ctrn;
// EntityDef_InitAllCallbacks @0x4a5aae -> Entity_LookupRenderCallbacks @0x407dc0]
enum class VehicleBrainClass : uint8_t { Unset, Air, Ground };

enum VehicleControlMask : uint16_t {
	VC_STEERING = 1,
	VC_SPEED = 2,
	VC_ROTORS = 4,
	VC_WHEELS = 8,
	VC_TIRES = 16,
	VC_TRACKS = 32,
	VC_VEHICLE_GUN = 64,
	VC_HELO_GUN = 128,
	VC_HELO_GEAR = 256,
	VC_TANK_TIRES = 512,
};

struct VehicleTraits {
	VehicleRenderFamily render_family = VehicleRenderFamily::Ground;
	VehicleBrainClass brain_class = VehicleBrainClass::Unset; // ai_function class (see enum)
	int32_t physics = 0; // itemDef+0x8DC ground-dispatch selector; direct air ignores it
	int32_t player_speed = 0;  // itemDef+0x8E8
	int32_t slip_speed = 0; // +0x8F0, simple mover lateral-slip threshold
	int32_t acceleration = 0; // itemDef+0x8E0
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
	int32_t tire_slip = 5; // itemDef+0x940: five ticks per recovery unit
	int32_t hand_brake = 0; // itemDef+0x944 raw — arms the byte-973 stop latch
							// [orig: @0x48c03a `occupant && Flags & 8 && handBrake`]
	// The AI crew clamp pair [orig: minAI +0x8D8 @0x48bc51, criticalHp +0x180
    // @0x48bc7d]: an undercrewed AI hull that has left its spawn anchor bleeds
    // to criticalHp (AiSystem::apply_min_ai_crew_clamp).
    int32_t min_ai = 0;        // itemDef+0x8D8 raw — the crew count threshold
    int32_t critical_hp = 0;   // itemDef+0x180 i16 raw — the clamp ceiling
	// The family movers' authority health machine [orig: the every-64th-tick
	// block of Entity_UpdateAircraftPhysics @0x4903F4..0x4904A7]: above
	// criticalHp the hull regens nonCriticalRegen up to healthMax - regen; at or
	// below it the hull burns criticalDrain per 64 ticks.
	int32_t critical_drain = 0; // itemDef+0x182 i16 raw
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
	std::string skid_effect, skid_snow_effect, skid_userpoint;
	std::vector<VehicleEffectPoint> skid_points;
	std::vector<VehicleEffectPoint> flare_points; // first 16 FLARE-prefix model points
	VehicleTrailDefinition trails[4];
	VehicleEffectPoint trail_points[16];
	uint8_t trail_point_count = 0;
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
	uint16_t mask = 0;
	std::array<int32_t, 2> tracks{};
	int32_t gun_yaw = 0, gun_pitch = 0;
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
	int32_t gear = 0;
	// VEHICLE_TIRE00..05: front L/R, midpoint L/R, rear R/L.
	// [orig: Entity_CacheVehicleHUDStats @0x4929F6..0x492AC5]
	std::array<int32_t, 14> tires{};
};

VehicleCtrlRegisters vehicle_ctrl_registers(const Entity::VehicleMotorState &state,
		VehicleRenderFamily render_family = VehicleRenderFamily::Ground,
		const AiEntity *ai = nullptr);

// abs((0xFFFF * signed_speed) >> 15), kept as Q16 control magnitude. The
// arithmetic shift occurs before absolute value, including its one-unit
// forward/reverse asymmetry.
uint32_t vehicle_trail_magnitude_q16(int32_t signed_speed) noexcept;

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





} // namespace opennova::world
