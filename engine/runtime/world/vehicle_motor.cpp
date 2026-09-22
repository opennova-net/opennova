#include <runtime/world/vehicle_system.h>
#include <runtime/world/vehicle_motor.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/io/bam.h>

#include "vehicle_motor_detail.h"

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/geom.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

namespace opennova::world {

using namespace detail; // the shared platform-solve helpers, unqualified as before

namespace {

// Key-steer ramp: +2.0 deg/tick, capped at 50 deg [orig: @0x48b4e0 — `[137] +=
// 0x16C16C0` while `< 0x238E38C0`]. Constants verbatim.
constexpr int32_t kSteerRampStep = 0x16C16C0;
constexpr int32_t kSteerRampCap = 596523200; // 0x238E38C0

// Gravity on the vertical velocity, 16.16 u/tick per tick — the GROUND
// family constant [orig: @0x48d009 `slideDecay -= 324`; an earlier comment
// cited @0x48d69b, which is watercraft smoke-FX code — corrected by the
// platform-solve review]. The cbik mover uses 250 [orig: @0x4865a6]; the
// watercraft mover uses 167 in its not-afloat drag branch [orig: @0x48EBD9].
constexpr int32_t kGravityStep = 324;
constexpr int32_t kGravityStepBike = 250;

// The slope factor samples the runtime-built 1024-step table, not a continuous
// cosine: the half-bin index rounding and the truncated table entries both
// reach the cos^2 term. [orig: off_849934 = Math_BuildSinTable @0x613050
// output + 0x400; ctan Entity_UpdateTankVehiclePhysics @0x489CE6..0x489D01,
// cveh Entity_UpdateVehiclePhysics @0x48C1C6..0x48C1D7, cbik
// Entity_UpdateLightVehiclePhysics @0x485362..0x48537E]
int32_t slope_cos22(int32_t pitch_bam) {
	int32_t cos22 = 0, sin22 = 0;
	quantized_dir(pitch_bam, cos22, sin22);
	return cos22;
}

// The x87 trig pair (cos22/sin22_of_bam_x87) lives in vehicle_motor_detail.h —
// shared with the contact solves in vehicle_contact_solve.cpp.

} // namespace

void VehicleSystem::tick_health(Entity &veh, const VehicleTraits &traits) {
	// All five movers build the same stagger key with LEA x9 then LEA x4.
	// The decompiler's current_tick[9 * DcbId] is byte-scaled pointer math.
	// [orig: Entity_UpdateVehiclePhysics @0x48AF00, sites @0x48AF24..0x48AF2D;
	// Entity_UpdateAircraftPhysics @0x490310, sites @0x490334..0x490340]
	// The live callback requests its death state before the health cadence.
	// [orig: ground @0x48AFD9..0x48AFF6; aircraft @0x490384..0x4903A1;
	// boat @0x48D53D..0x48D55A]
	if (AiEntity *ai = world_.ai.for_handle(veh.handle)) {
		auto &b = ai->brain;
		const bool air = vehicle_family_uses_direct_air_mover(traits.family);
		// The 0 -> PRETTY stamp sits at the mover HEAD in retail [orig: ground
		// @0x48afac..0x48afb2, aircraft @0x490377..0x49037d], ahead of the
		// occupant/AI-driver block; the authority pass hoists it into
		// vehicle_system.cpp with the drive staging, so this twin only lands
		// for callers that enter the motor directly.
		if (b.f[AiBrain::kCurState] == 0)
			b.f[AiBrain::kCurState] = air ? 14 : 22;
		if (((veh.flags | veh.engine_flags) & kEntityFlagDead) == 0 && veh.health <= 0 &&
				b.f[AiBrain::kCurState] != (air ? 13 : 21) &&
				b.f[AiBrain::kCurState] != (air ? 15 : 23))
			b.set_pend(air ? 13 : 21);
	}
	const bool critical = veh.health > 0 && veh.health <= traits.critical_hp;
	if (world_.ai.is_authority && veh.health > 0 &&
			((world_.logic_tick + 36u * uint32_t(veh.net_id)) & 63u) == 0) {
		if (veh.health > traits.critical_hp) {
			// Strict inequality: a hull at max - regen does not regenerate.
			// [orig: @0x48B015..0x48B03F, @0x48D57C..0x48D5A6;
			// aircraft health read/store @0x4903F9..0x49042D]
			if (traits.non_critical_regen != 0 &&
					veh.health < veh.health_max - traits.non_critical_regen)
				veh.health += traits.non_critical_regen;
		} else {
			// The critical band burns to zero; clients only render this state.
			// [orig: @0x48B06C..0x48B083, @0x48D5FF..0x48D616;
			// aircraft critical-band entry @0x490434]
			veh.health =
					veh.health > traits.critical_drain ? veh.health - traits.critical_drain : 0;
		}
	}
	detail::vehicle_health_effects(world_, veh, traits, critical);
}

VehicleCtrlRegisters vehicle_ctrl_registers(const Entity::VehicleMotorState &state,
		VehicleRenderFamily render_family, const AiEntity *ai) {
	VehicleCtrlRegisters out;
	// Each render callback owns a different subset of the shared register bank.
	// Tank projects fourteen tires from its four wheels and two middle channels.
	// [orig: @0x4929B0 (cveh), @0x449C10 (tank), @0x48F1A0 (chel), @0x48F140 (cpln)]
	switch (render_family) {
		case VehicleRenderFamily::Ground:
			out.mask = VC_STEERING | VC_SPEED | VC_ROTORS | VC_WHEELS | VC_TIRES;
			break;
		case VehicleRenderFamily::Tank:
			out.mask = VC_STEERING | VC_SPEED | VC_ROTORS | VC_WHEELS | VC_TRACKS | VC_TANK_TIRES;
			break;
		case VehicleRenderFamily::Helicopter:
			out.mask = VC_ROTORS | VC_HELO_GEAR;
			break;
		case VehicleRenderFamily::Plane:
			out.mask = VC_STEERING | VC_ROTORS | VC_WHEELS;
			break;
		case VehicleRenderFamily::None:
			break;
	}
	if (render_family == VehicleRenderFamily::Helicopter) {
		out.mask |= VC_HELO_GUN;
		out.gun_pitch = io::bam_sar(state.view_tilt_bam, 16); // [orig: @0x48F1D5]
	}
	if (ai != nullptr && render_family != VehicleRenderFamily::None) {
		// MOVSX of the active BAM high words, unlike unsigned wheel/rotor phases.
		// [orig: HUD_CacheEntityDisplayInfo @0x4A3D90; tank tail @0x449ECF..0x449EE2]
		out.gun_yaw = io::bam_sar(ai->brain.f[AiBrain::kActiveYaw], 16);
		out.gun_pitch = io::bam_sar(ai->brain.f[AiBrain::kActivePitch], 16);
		if (render_family == VehicleRenderFamily::Tank || ai->profile.type == 2)
			out.mask |= VC_VEHICLE_GUN;
		if (render_family == VehicleRenderFamily::Helicopter ||
				(render_family != VehicleRenderFamily::Tank && ai->profile.type == 1))
			out.mask |= VC_HELO_GUN;
	}
	for (size_t i = 0; i < out.tracks.size(); ++i)
		out.tracks[i] = static_cast<int32_t>(static_cast<uint32_t>(state.track_phase[i]) >> 16);

	// entity+0x2B6 is the high word of the vehicle wheel/steer dword. MOVZX
	// makes a negative wheel deflection a 0..0xFFFF cyclic phase; the following
	// retail 0x10000 cap is unreachable for a zero-extended word.
	// [orig: Entity_CacheVehicleHUDStats @0x4929B0;
	//  MOVZX/store @0x4929C0..0x4929D7]
	out.steering = static_cast<int32_t>(static_cast<uint32_t>(state.steer_state) >> 16);

	// Reproduce CDQ/XOR/SUB as unsigned two's-complement arithmetic before the
    // unsigned 0x10000 cap. In particular INT_MIN becomes 0x80000000 (it does
    // not invoke C++ signed-abs UB) and therefore publishes 0x10000.
    // [orig: Entity_CacheVehicleHUDStats @0x4929DC..0x4929F1]
    const uint32_t speed_bits = static_cast<uint32_t>(state.speed);
    const uint32_t sign_mask = 0u - (speed_bits >> 31);
    const uint32_t magnitude =
            (speed_bits ^ sign_mask) - sign_mask;
    out.speed = static_cast<int32_t>(
            std::min(magnitude, uint32_t{0x10000}));

    // The part-animation words [orig: Entity_CacheVehicleHUDStats @0x4929B0 —
    // the rotor angle's +0x466 @0x492ACA..0x492ADE, the wheel phase's +0x2BA
    // @0x4929B4]: the accumulators' high words, zero-extended like the steer
    // word. The tail rotor publishes the same word: ordinal 47 reads the one
    // +0x464 accumulator [orig: @0x492AD7].
    out.rotor = static_cast<int32_t>(
            static_cast<uint32_t>(state.part_spin.angle) >> 16);
    out.tail_rotor = out.rotor;
    out.wheels = static_cast<int32_t>(
            static_cast<uint32_t>(state.wheel_phase) >> 16);
	const auto tire = [](int32_t value) { return std::clamp(value, 0, 0x10000); };
	out.gear = state.gear_phase; // MOVZX LOW word [orig: @0x48F1C8]
	if (render_family == VehicleRenderFamily::Tank) {
		const auto &c = state.wheel_comp;
		const auto mean = [&](int a, int b) {
			return tire(io::bam_sar(io::bam_add(c[a], c[b]), 1));
		};
		// The x87 stack retains the 0.33 constant, not the first mean.
		// [orig: HUD_CacheEntityDebugStats @0x449C10, @0x449D25..0x449D33 /
		// @0x449E25..0x449E3F; flt_7C59B4 = 0.33000001311302185]
		const auto third = [&](int a, int b, int d) {
			const int32_t sum = io::bam_add(io::bam_add(c[a], c[b]), c[d]);
			return tire(int32_t(double(sum) * double(0.33f)));
		};
		out.tires = { tire(c[0]), tire(c[0]), mean(0, 4), third(0, 3, 4), mean(3, 4), tire(c[3]),
			tire(c[3]), tire(c[1]), tire(c[1]), mean(1, 5), third(1, 2, 5), mean(2, 5), tire(c[2]),
			tire(c[2]) };
	} else {
		out.tires = { tire(state.wheel_comp[0]), tire(state.wheel_comp[1]),
			tire(io::bam_sar(io::bam_add(state.wheel_comp[0], state.wheel_comp[2]), 1)),
			tire(io::bam_sar(io::bam_add(state.wheel_comp[1], state.wheel_comp[3]), 1)),
			tire(state.wheel_comp[3]), tire(state.wheel_comp[2]) };
	}
	return out;
}

uint32_t vehicle_trail_magnitude_q16(int32_t signed_speed) noexcept {
	// [orig: Entity_UpdateBoneTrailEffects @0x458C26 IMUL, @0x458C2C SAR]
	const int32_t product = int32_t(uint32_t(signed_speed) * 0xFFFFu);
	const int32_t scaled = product >> 15;
	return uint32_t(io::bam_abs(scaled));
}

// The controlling occupant: the Controller/Driver seat's occupant, stale-validated
// against the occupant's own mount fields (the original walks its mountHandles and
// clears mismatches every tick) [orig: @0x48b8a1-0x48b944 — occupant must satisfy
// `parentEntity == vehicle && parentSlot in {2,5} && attachBoneId == controlBone`;
// our seat model keys the same relation through Seat::occupant + Entity::mount_*,
// the wire-bone divergence is tracked in D-NET-157].
Entity *VehicleSystem::resolve_controller(Entity &veh) {
    World &world = world_;
    Entity *controller = nullptr;
    for (Seat &s : veh.seats) {
        if (!is_vehicle_control_seat(s.type)) continue;
        if (!s.occupant.valid()) continue;
        Entity *occ = world.registry.get(s.occupant);
        if (occ == nullptr || !occ->mounted || occ->mount_target != veh.handle ||
            !is_vehicle_control_seat(occ->mount_type)) {
            s.occupant = EntityHandle{}; // [orig: stale mountHandles slot -> 0xFFFF]
            continue;
        }
        if (controller == nullptr) controller = occ;
    }
    // Stale claimant (for example, a scripted/despawned occupant that bypassed detach):
    // validate the +368 mirror each tick alongside the seat sweep. The stop edge
    // fires for THE claimant only, matching the detach leg; a surviving second control
    // occupant does not inherit the claim (it re-arms only on a fresh attach).
    // [orig: Entity_DetachFromVehicle @0x4356e9..0x43577c]
    if (veh.primary_occupant.valid()) {
        Entity *po = world.registry.get(veh.primary_occupant);
        if (po == nullptr || !po->mounted || po->mount_target != veh.handle) {
            world.vehicles.stop_ground_sound(veh);
            veh.primary_occupant = EntityHandle{};
            world.vehicles.emit_control_stopped(veh);
        }
    }
    return controller;
}

// The pilot's view can turn independently of hull steering. Keep both native
// aliases so LocalPlayer's post-tick reconciliation retains the turn.
int32_t turn_pilot_view(World &world, Entity &pilot, int32_t delta) {
	AiEntity *body = world.ai.for_handle(pilot.handle);
	const int32_t yaw = io::bam_add(
			body != nullptr ? body->heading : bam_heading_from_mission_yaw_deg(pilot.yaw), delta);
	pilot.yaw = static_cast<int16_t>(
			std::lround(normalize_mission_yaw_deg(mission_yaw_deg_from_bam_heading(yaw))));
	if (body != nullptr) {
		body->heading = yaw;
		body->inf.target_heading = io::bam_add(body->inf.target_heading, delta);
	}
	return yaw;
}

// Entity_UpdateVehiclePhysics's shared player-input block. The authority stages
// a remote driver's replicated fields through it; the controlling client stages
// its current local fields instead of replaying delayed compact drive registers.
void stage_player_vehicle_input(
		World &world, Entity &veh, Entity &occ, const VehicleTraits &traits) {
	Entity::VehicleMotorState &m = veh.veh;
	uint32_t move_order = static_cast<uint32_t>(occ.net_move_input) |
			(static_cast<uint32_t>(occ.net_stance_bits) << 8) | occ.local_view_input;
	int dir = static_cast<int>(move_order & 7u);
    bool moving = ((move_order >> 3) & 1u) != 0;
    const int32_t analog_sum = static_cast<int32_t>(occ.net_analog_x) +
                               static_cast<int32_t>(occ.net_analog_y) +
                               static_cast<int32_t>(occ.net_analog_z);
    // The MoveOrder bit-0x10 merge: any analog deflection, or the move bit with
    // a nonzero 8-way direction, latches the free-look/steer-mode bit on the
    // OCCUPANT's own MoveOrder word — the host echoes that byte in the
    // player's 0x0A record, so the latch is wire-visible until the next uplink
    // rewrites it [orig: ground @0x48b847..0x48b897 (`or eax, 10h` /
    // `or [ecx+12Ch], 10h`); boat @0x48DE04..0x48DE7B].
    const bool brake_mode = traits.physics != 0 &&
            (traits.family == VehicleFamily::Ground || traits.family == VehicleFamily::Bike);
    const int free_look_dir = brake_mode && m.handbrake_latched != 0
            ? m.handbrake_direction : dir;
    // The prior brake latch selects +0x3C8 for this merge only; the steering
    // switch below still uses the current input direction.
    // [orig: Entity_UpdateVehiclePhysics @0x48B855..0x48B864; D-VEH-3]
    if (analog_sum != 0 || (moving && free_look_dir != 0)) {
        move_order |= Entity::kMoveOrderFreeLook;
        occ.net_move_input |= static_cast<uint8_t>(Entity::kMoveOrderFreeLook);
    }

	// Read the full-precision LOOK word shared by the body and aircraft mover;
	// the degree mirror is a fallback for unbound bodies.
	// [orig: Entity_UpdateVehiclePhysics @0x48BB0B; D-VEH-3]
	const AiEntity *driver_body = world.ai.for_handle(occ.handle);
	int32_t driver_yaw_bam = driver_body != nullptr ? driver_body->heading
			: bam_heading_from_mission_yaw_deg(occ.yaw);

	// Family split at the command source: the boat player leg reads itemDef
    // waterSpeed (+0x8EC) at EVERY command site — retail cbot defs author no
    // player_speed, so the ground field would command 0 — and consumes MoveOrder
    // bits 6/7 as motion overrides instead of the ground lean flags: bit6 forces
    // dir=1 AND the move bit; bit7 forces cmd=waterSpeed, dir=7, skipping the
    // move/analog split. [orig: bit6 @0x48E005..0x48E00E (edx=edi=1), bit7
    // @0x48E010..0x48E028, cmd reads +0x8EC @0x48E017/@0x48E034/@0x48E088]
    const bool boat = traits.family == VehicleFamily::Watercraft;
    const int32_t cmd_base = boat ? traits.water_speed : traits.player_speed;
    bool boat_hard_turn = false;
    if (boat) {
        if ((move_order & 0x40u) != 0) {
            dir = 1;
            moving = true;
        }
        if ((move_order & 0x80u) != 0) {
            dir = 7;
            boat_hard_turn = true;
        }
    }

    // The previous full-ground/bike brake latch chooses the analog arm even
    // while a movement key is held. The latch itself updates later this tick.
    // [orig: Entity_UpdateVehiclePhysics @0x48B9D2..0x48B9E5; bike @0x484B81]
    const bool braking = brake_mode && m.handbrake_latched != 0;
    if (boat_hard_turn || (moving && !braking)) {
        m.cmd_speed = cmd_base;
    } else {
        int32_t steer_delta =
                (kAnalogSteerScale * static_cast<int32_t>(occ.net_analog_z)) >> 1;
        const int32_t alt =
                (kAnalogSteerScale * static_cast<int32_t>(occ.net_analog_y)) >> 1;
        if (std::abs(alt) > std::abs(steer_delta)) steer_delta = alt;
        if (!braking) dir = 0; // analog control replaces the digital direction
        m.steer_target_bam = io::bam_sub(m.steer_target_bam, steer_delta);
        m.cmd_speed = io::bam_sar(io::bam_sub(0,
                bam_mul_wrap(cmd_base, static_cast<int32_t>(occ.net_analog_x))), 7);
        // The bike clears analog throttle while the old brake latch is set;
        // ground vehicles keep it until the later current-input brake test.
        // [orig: Entity_UpdateLightVehiclePhysics @0x484BA7..0x484BAB]
        if (braking && traits.family == VehicleFamily::Bike) m.cmd_speed = 0;
		// The same pedal turn updates the driver's view unless freelook is
		// latched. The signed three-axis sum above can cancel to zero while
		// the dominant steering axis remains nonzero.
		// [orig: Entity_UpdateVehiclePhysics @0x48B7AA..0x48B7D9;
		// Entity_UpdateWatercraftPhysics @0x48E08D..0x48E0BC]
		if ((move_order & Entity::kMoveOrderFreeLook) == 0)
			driver_yaw_bam = turn_pilot_view(world, occ, io::bam_sub(0, steer_delta));
	}

	if ((move_order & Entity::kMoveOrderCrouch) != 0) m.cmd_speed >>= 1;
    if ((move_order & Entity::kMoveOrderProne) != 0) m.cmd_speed >>= 2;
    if ((move_order & 0x20u) != 0) veh.flags |= 0x80u;
    else veh.flags &= ~0x80u;
    if (!boat && traits.family != VehicleFamily::Tank) {
        // Ground/bike lean flags. The tank input only updates the sprint flag;
        // it preserves 0x20/0x8 regardless of the occupant's lean keys.
        // [orig: ctank @0x489675..0x4896A7; boat @0x48DFE5..0x48E0C0]
        if ((move_order & 0x40u) != 0) veh.flags |= 0x20u;
        else veh.flags &= ~0x20u;
        if ((move_order & 0x80u) != 0) veh.flags |= 0x8u;
        else veh.flags &= ~0x8u;
    }

    if (analog_sum == 0) {
		m.steer_target_bam =
				(move_order & Entity::kMoveOrderFreeLook) != 0 ? m.yaw_bam : driver_yaw_bam;
	}

    // The key-steer ramp cap is a family delta: the boat's own input block
    // caps [137] at 0x1FFFFFE0 (45deg) [orig: @0x48E13A]; the ground/bike
    // template caps at 0x238E38C0 (50deg) [orig: the shared seat-direction
    // template]. The step is 0x16C16C0 everywhere.
    const int32_t ramp_cap = traits.family == VehicleFamily::Watercraft
            ? 0x1FFFFFE0 : kSteerRampCap;
    if (dir != 0) {
        if (m.steer_ramp_bam < ramp_cap)
            m.steer_ramp_bam += kSteerRampStep;
    } else {
        m.steer_ramp_bam = 0;
    }
    // Reverse-gear command: the bike commands -[136] >> 3 (1/8 speed)
    // [orig: @0x484d0f, 0x484d2b, 0x484d50] where the ground core commands
    // -[136] >> 1 (1/2) [orig: @0x48bb89].
    const int reverse_shift = traits.family == VehicleFamily::Bike ? 3 : 1;
    // An active bike launch retains the heading/throttle chosen above.
    // [orig: Entity_UpdateLightVehiclePhysics @ 0x484C9C..0x484CBD]
    if (traits.family == VehicleFamily::Bike &&
            ((veh.flags & kEntityFlagInAir) || m.wheelie_request || m.wheelie_active))
        return;
    switch (dir) {
        case 1:
            m.steer_target_bam = io::bam_add(m.yaw_bam, m.steer_ramp_bam);
            break;
        case 2:
            m.steer_target_bam = io::bam_add(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = 0;
            break;
        case 3:
            m.steer_target_bam = io::bam_add(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = io::bam_sar(io::bam_sub(0, m.cmd_speed), reverse_shift);
            break;
        case 4:
            m.steer_target_bam = m.yaw_bam;
            m.cmd_speed = io::bam_sar(io::bam_sub(0, m.cmd_speed), reverse_shift);
            break;
        case 5:
            m.steer_target_bam = io::bam_sub(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = io::bam_sar(io::bam_sub(0, m.cmd_speed), reverse_shift);
            break;
        case 6:
            m.steer_target_bam = io::bam_sub(m.yaw_bam, m.steer_ramp_bam);
            m.cmd_speed = 0;
            break;
        case 7:
            m.steer_target_bam = io::bam_sub(m.yaw_bam, m.steer_ramp_bam);
            break;
        default:
            break;
    }
}

// The occupant whose input this machine should consume. Retail's gate is
// `(occ->Flags & 0x100) && (occ == g_local_player_entity || is_authority)` — the
// AUTHORITY runs the input block for ANY player occupant, not only its own local
// player. That distinction is invisible on a listen host flying its own
// aircraft, and decisive when a JOINER is the pilot: the host owns the mover, so
// insisting on the host's local player left a remote pilot commanding nothing.
// [orig: Entity_UpdateAircraftPhysics @0x490310 input gate; the ground twin is
//  Entity_UpdateVehiclePhysics @0x48b0ff]
Entity *resolve_piloting_player(World &world, Entity &veh, const VehicleTraits &traits) {
    if (!traits.player_control) return nullptr;
    Entity *occ = world.vehicles.resolve_controller(veh);
    if (occ == nullptr || occ->handle.pool() != 0 || occ->player_class == 0 ||
        !occ->alive || occ->health <= 0)
        return nullptr;
    const bool is_authority = world.ai.is_authority;
    if (!is_authority && occ->handle != world.cached.local_player) return nullptr;
    return occ;
}

static Entity *resolve_local_vehicle_controller(World &world, Entity &veh,
                                                const VehicleTraits &traits) {
    if (!traits.player_control || !world.cached.local_player.valid()) return nullptr;
    Entity *occ = world.vehicles.resolve_controller(veh);
    if (occ == nullptr || occ->handle != world.cached.local_player ||
        occ->handle.pool() != 0 || occ->player_class == 0 ||
        !occ->alive || occ->health <= 0)
        return nullptr;
    return occ;
}

// The ground/tracked contact + suspension solve — the client-executed subset
// of Entity_ProcessTrackedVehiclePhysics [orig: @0x47C1C0] — is declared in
// vehicle_motor_detail.h and defined in vehicle_contact_solve.cpp.

void VehicleSystem::tick_motor(Entity &veh, const VehicleTraits &traits,
		const VehicleDriveCmd *ai_cmd, bool prediction) {
	// [orig: Entity_DispatchPhysicsUpdate @0x48F010]
	if (traits.amphibian && (veh.flags & 0x8000u) != 0) {
		VehicleTraits afloat = traits;
		afloat.family = VehicleFamily::Watercraft;
		tick_watercraft_motor(veh, afloat, ai_cmd);
		return;
	}
	World &world = world_;
	// Selector zero dispatches the cveh row to the simple motor. The ctank and
	// cbike rows call their movers without testing the selector.
	// [orig: Entity_DispatchPhysics_cveh @0x48EFC7; Entity_DispatchPhysics_ctank
	//  @0x48F000..0x48F007; Entity_DispatchPhysics_cbike @0x48EFF0..0x48EFF7]
	if (traits.physics == 0 && traits.family != VehicleFamily::Tank &&
			traits.family != VehicleFamily::Bike) {
		tick_simple_motor(veh, traits, ai_cmd, false);
		return;
	}

	Entity::VehicleMotorState &m = veh.veh;
	if (!m.yaw_seeded) {
		m.yaw_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));
        // Seed the live BAM attitude mirror from the row's authored pose: one
        // entity Pitch/Roll in the original — the same storage the contact
        // solve conforms every grounded tick [orig: entity->Pitch/Roll writes
        // @0x47EC83/@0x47EC75]. (A joiner row arms through ground_client_tick,
        // whose staging already seeded these from the wire.)
        m.air_pitch_bam = static_cast<int32_t>(veh.pitch) * 11930464;
        m.air_roll_bam = static_cast<int32_t>(veh.roll) * 11930464;
        m.yaw_seeded = true;
	}

	if (!m.net_predicted) {
		vehicle_refresh_ground_link(world, veh, traits);
		vehicle_follow_carrier(world, veh);
	}

	// The per-family contact solve owns Z, attitude, the contact byte and
	// the airborne/in-water flags for rows with resolved model boxes on a
	// terrain-backed world; every other row (boxless lib-embedder rows,
	// terrain-less unit worlds) keeps the 5-tap terrain-clamp stand-in below. Retail keys the same
	// split on graphicModel presence [orig: the @0x47C49F bail]. The family routing is the class
	// table's [orig: @0x82ABC0]: cveh/ctrn/catv -> the tracked solve @0x47C1C0; ctan -> the wheeled
	// solve @0x475DE0 (call @0x48a9ef); cbik -> the light solve @0x479600 (call @0x486672).
	enum class ContactSolveKind : uint8_t { None, Tracked, Wheeled, Light };
	ContactSolveKind solve_kind = ContactSolveKind::None;
	{
		if (traits.family == VehicleFamily::Ground && ground_contact_solve_active(traits))
			solve_kind = ContactSolveKind::Tracked;
		else if (traits.family == VehicleFamily::Tank &&
                 ground_contact_solve_active(traits))
            solve_kind = ContactSolveKind::Wheeled;
        else if (traits.family == VehicleFamily::Bike &&
                 light_contact_solve_active(traits))
            solve_kind = ContactSolveKind::Light;
	}
	const bool wheeled_solve = solve_kind != ContactSolveKind::None;

	// Zero health requests wreck state 21. The drive block also rejects Flags bit 1; wreck motion
	// runs through the installed state callback.
	// Witness sites: [orig: @0x48af61]
	tick_health(veh, traits);
	const bool wrecked = veh.health <= 0;

	// ------------------------------------------------------------------ input block
	// [orig: Entity_UpdateVehiclePhysics @0x48af00, the `attrib & 0x40` occupant block
	// @0x48b949-0x48c034; ctan @0x489522..0x489545.] Only this block is role-gated:
	// the authority runs it, while a predicting client (`prediction`) has already
	// swept the seats and staged its local driver or the received registers.
	// Everything after it, including the PlayerControl tail, runs on both roles.
	Entity *occ = traits.player_control && !prediction ? world.vehicles.resolve_controller(veh)
													   : nullptr;
    // A DEAD controller counts as none. The infantry death edge now detaches first;
    // this remains the same-frame safety gate when motor/system ordering varies.
    // [orig: infantry death detach @0x4b9c57..0x4b9c60]
    if (occ != nullptr && (!occ->alive || occ->health <= 0)) occ = nullptr;
    // "Occupant is a player" [orig: `occupant->Flags & 0x100`] — our runtime players
    // are pool-0 organics with a resolved soldier class.
    const bool player_occupant =
            occ != nullptr && occ->handle.pool() == 0 && occ->player_class != 0;

    if (traits.player_control && !prediction) {
        if (occ == nullptr || wrecked || (veh.flags & kEntityFlagDead) != 0) {
			// Without a live controller, steer holds the current heading and command speed decays
			// through the family deceleration clamps. Parked/stuck state is handled by vehicle
			// lifecycle.
			// Witness sites: [orig: @0x48c002]
			m.steer_target_bam = m.yaw_bam;
			m.cmd_speed = 0;
			m.steer_ramp_bam = 0;
            veh.flags &= ~0x80u;
        } else if (player_occupant && !watercraft_driver_submerged(world, *occ)) {
			// A player driver whose eye (CameraOffset.z + Z) sits at or below the
			// water plane drops to the AI leg like the boat twin [orig: `mov
			// eax,[ecx+74h]; add eax,[ecx+0Ch]; cmp eax, Env_WaterHeightFixed;
			// jle loc_48BC12` @0x48B9A0..0x48B9AC].
			stage_player_vehicle_input(world_, veh, *occ, traits);
		} else if (ai_cmd != nullptr && ai_cmd->ai_drive) {
			// An AI controller drives: consume the brain-computed command block
            // (AiSystem::vehicle_ai_drive — the witnessed leg's steer/speed outputs).
            // [orig: the AI-driver leg @0x48bc12-0x48c034 writes aiComp[132]/[136];
            //  the ramp/dir state is the player path's only]
            m.steer_target_bam = ai_cmd->steer_target_bam;
            m.cmd_speed = ai_cmd->cmd_speed;
            m.steer_ramp_bam = 0;
		}
		// A live NON-player controller with no drive command holds the previous
        // steer/speed targets.
        // Occupied: the driverless stuck count rests [orig: `moveTimer = 0` at
        // the occupied entry split, the boat twin @0x48DFA8..0x48DFCD].
        if (occ != nullptr) m.stuck_ticks = 0;
    }
    // The handbrake stop latch and the crashed stop sit OUTSIDE the attrib-0x40
    // authority/occupant block, so a DRIVING CLIENT runs them on its predicted
    // hull too — without them the joiner predicts full drive while the retail
    // host skids or stops (the D-NET-161 snapback on the lean-right key)
    // [orig: cveh @0x48c03a..0x48c095 — `occupant && Flags & 8 &&
    //  itemDef->handBrake` sets byte +0x3CD, else clears it (@0x48c066); while
    //  set, [136] = 0 (@0x48c07a); the crashed byte +0x2EC zeroes [136] too
    //  (@0x48c086..0x48c08f); the cbik twin has the same shape; ctan never
    //  writes +0x3CD]. Flags bit 3 is the player leg's lean-right mirror, so the
    // handbrake IS the lean-right key while driving. (Re-homed 2026-09-10 from
    // inside the player_control gate, jo-c cross-check.) The occupant test reads
    // the +0x170 claimant, not the input block's controller, so it holds on a
    // predicting client [orig: cveh `cmp [esi+170h], edx` @0x48C03C; cbik
    // @0x4851E8].
    if (traits.family != VehicleFamily::Tank) {
        const bool brake_enabled = traits.family == VehicleFamily::Bike
                ? m.wheelie_active == 0 && m.bike_ground_contact_ticks > 0
                : traits.hand_brake != 0;
        if (world.registry.get(veh.primary_occupant) != nullptr && (veh.flags & 0x8u) != 0 &&
                brake_enabled)
            m.handbrake_latched = 1;
        else
            m.handbrake_latched = 0;
        if (m.handbrake_latched != 0) m.cmd_speed = 0;
    }
    // Both bytes arm before steering and acceleration, outside the input-role gate.
    // [orig: Entity_UpdateLightVehiclePhysics @ 0x485233..0x485269;
    // wheelie-active byte store @0x48524C]
    if (traits.family == VehicleFamily::Bike && (veh.flags & 0x20u) && m.speed > 4096)
        m.wheelie_request = m.wheelie_active = 1;
    // The ground core stops a crashed hull outright. The bike and the tank also
    // require +0x2EF, a latch their own contact solves own; the tank solve
    // clears it in its airborne leg, so an airborne crashed tank keeps its
    // command. [orig: cveh @0x48C086..0x48C08F; Entity_UpdateTankVehiclePhysics
    //  @0x489C06..0x489C1C `cmp [esi+2EFh],0; jz; cmp [esi+2ECh],0; jz`;
    //  tank clear @0x478ABC]
    if (m.crashed != 0 &&
            ((traits.family != VehicleFamily::Bike && traits.family != VehicleFamily::Tank) ||
                    m.byte_2ef))
        m.cmd_speed = 0;

    // ------------------------------------------------------------- steering chase
    // Turn rate blends DOWN with speed: eff = (turnRate - minRate) * clamp01(1 -
    // speed/playerSpeed) + minRate, where minRate = turn_rate2 (else turnRate/4)
    // [orig: Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C095..0x48C10F);
    //  the ctan twin @0x489C26..0x489CA2, the cbik twin @0x485272..0x4852C9].
    // A zero playerSpeed pins the fraction at 0 (the `jz -> xor ecx, ecx` arm
    // @0x48C0A9..0x48C0D8 / @0x489C3C..0x489C67 / @0x485283..0x4852B0), so the
    // hull turns at minRate — every ground family alike.
    {
        const int32_t turn_rate = traits.turn_rate;
        int32_t min_rate = io::bam_sar(turn_rate, 2);
        if (traits.turn_rate2 != 0) min_rate = traits.turn_rate2;
		int32_t f = 0;
		if (traits.player_speed != 0) {
			f = 0x10000 -
					static_cast<int32_t>(
							(static_cast<int64_t>(m.speed) * 0x10000) / traits.player_speed);
			if (f < 0) f = 0;
		}
		const int32_t eff = io::bam_add(min_rate,
				static_cast<int32_t>(
						(static_cast<int64_t>(io::bam_sub(turn_rate, min_rate)) * f + 0x8000) >>
						16));
		// Proportional step: 1/64 of the heading error, clamped to the effective rate
        // [orig: Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C111..0x48C12C
        //  `(target - Yaw + 32) >> 6` + the +-clamp)].
        int32_t delta = io::bam_sar(io::bam_add(
                io::bam_sub(m.steer_target_bam, m.yaw_bam), 32), 6);
        if (delta > eff) delta = eff;
        const int32_t neg_eff = io::bam_sub(0, eff);
        if (delta < neg_eff) delta = neg_eff;
        // Smoothed wheel deflection [orig: Entity_UpdateVehiclePhysics @0x48AF00
        // (site @0x48C12E..0x48C14E `+0x2B4 += (4 - 32*delta - +0x2B4) >> 3`)].
        const int32_t steer_error = io::bam_sub(
                io::bam_sub(4, bam_shl_wrap(delta, 5)), m.steer_state);
        m.steer_state = io::bam_add(
                m.steer_state, io::bam_sar(steer_error, 3));
		// Yaw rate = -speed * (wheel >> 2) >> 16, applied while grounded
		// Crashed or settling ground hulls retain the previous yaw rate.
		// [orig: Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C146..0x48C18F:
		//  `test Flags, 0x2000; cmp +0x2EC, 0; cmp +0x2F0, 0` then the imul/shrd
		//  into +0xA4). The ctan steering block @0x489C26..0x489CE0 never writes
		//  +0xA4 — its velocity arm does.]
		if (traits.family != VehicleFamily::Tank &&
				(traits.family == VehicleFamily::Bike || (m.crashed == 0 && m.settle_2f0 == 0)) &&
				(veh.flags & kEntityFlagInAir) == 0) {
			const int32_t neg_speed = io::bam_sub(0, m.speed);
			m.wheel_rate_bam = static_cast<int32_t>(
					(static_cast<int64_t>(neg_speed) * io::bam_sar(m.steer_state, 2) + 0x8000) >>
					16);
		}
	}

	// ------------------------------------------------------------- speed pipeline
	int32_t target_speed;
    {
        // Airborne: the command opposes the current motion (a coast brake)
        // [orig: Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C195..0x48C1C2,
        //  gated `+0x2F2 == 0 && !(Flags & 0x2000)`)].
        // GROUND-FAMILY ONLY: the cbik mover holds throttle off-contact (its
        // +25/tick jump-latch ramp is input-side; the flip simply does not
        // exist there) [orig: cbik speed servo @0x4853ac..0x4853eb].
        int32_t cmd = m.cmd_speed;
		if (traits.family != VehicleFamily::Bike && traits.family != VehicleFamily::Tank &&
				!m.grounded && (veh.flags & kEntityFlagInAir) == 0) {
			if (m.speed < 0) {
				if (cmd < 0)
					cmd = -cmd;
			} else if (cmd > 0) {
				cmd = -cmd;
			}
		}
		// Slope factor: cos^2(pitch) in 22-bit fixed [orig: Entity_UpdateVehiclePhysics
		// @0x48AF00 (site @0x48C1C6..0x48C1FA — off_849934 cos-table sample squared
		// >> 22, times the command >> 22)]. Retail reads the live entity Pitch the
		// contact solve conforms; boxless stand-in rows keep the degree-quantized
		// row pitch (their attitude never advances past the authored pose).
		const int32_t pitch_bam = wheeled_solve
				? m.air_pitch_bam
				: static_cast<int32_t>(static_cast<int64_t>(veh.pitch) * 11930464);
		const int32_t c = slope_cos22(pitch_bam);
        const int32_t c2 = static_cast<int32_t>((static_cast<int64_t>(c) * c) >> 22);
        target_speed = static_cast<int32_t>((static_cast<int64_t>(c2) * cmd) >> 22);

        // Chase 1/32 of the gap per tick, then family clamps [orig:
        // Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C1FC..0x48C215
        // `rawAccel = (target - speed + 16) >> 5`; the clamp tree @0x48C3D0..0x48C46C)].
        const int32_t raw_accel = (target_speed - m.speed + 16) >> 5;
        // The held wheelie below 0x4000 adds to the retained acceleration.
        // [orig: Entity_UpdateLightVehiclePhysics @ 0x4853A1..0x4853DF]
        m.speed_accel = traits.family == VehicleFamily::Bike &&
                m.wheelie_active && m.wheelie_request && m.speed < 16384
                ? io::bam_add(m.speed_accel, 25) : raw_accel;
		m.skid_effects_requested = false;
		if (detail::vehicle_traction_acceleration(world, veh, traits, target_speed)) {
			// The retained contact frame owns the skid acceleration.
		} else if (traits.family == VehicleFamily::Tank) {
			// The tank servo's clamp tree [orig: Entity_UpdateTankVehiclePhysics
			// chase @0x489d79..0x489d84, clamps @0x489d89..0x489e79]: a
			// direction REVERSAL (and the standing
			// start) clamps at ±2·deceleration — where the ground core keeps
			// the raw 1/32 chase — and same-direction drive clamps to
			// ±acceleration (target != 0) or ±deceleration (target == 0). The
			// sharp-steering caps precede these plain caps when a contact
			// direction is retained [orig: @0x489DB1..0x489E97].
			const bool reversal = (target_speed > 0 && m.speed <= 0) ||
					(target_speed < 0 && m.speed >= 0) || (target_speed == 0 && m.speed == 0);
			if (reversal) {
				const int32_t d2 = 2 * traits.deceleration;
				if (m.speed_accel > d2) m.speed_accel = d2;
                if (m.speed_accel < -d2) m.speed_accel = -d2;
			} else if (target_speed != 0) {
				if (m.speed_accel > traits.acceleration)
					m.speed_accel = traits.acceleration;
				if (m.speed_accel < -traits.acceleration) m.speed_accel = -traits.acceleration;
			} else if (const Entity *parent = world.registry.get(veh.mount_target);
					parent == nullptr || (parent->flags & 0x100u) != 0) {
				// NPC-mounted zero-command coast retains the raw servo, as in
				// the ground core. [orig: Entity_UpdateTankVehiclePhysics
				// @0x489EBA..0x489ECB, parent +0x16C]
				if (m.speed_accel > traits.deceleration)
					m.speed_accel = traits.deceleration;
				if (m.speed_accel < -traits.deceleration) m.speed_accel = -traits.deceleration;
			}
		} else if ((!m.grounded || m.settle_2f0 != 0) && traits.family != VehicleFamily::Bike) {
			// Wheels off the ground: coast clamp at half deceleration
			// [orig: Entity_UpdateVehiclePhysics @0x48AF00 (site @0x48C471..0x48C492
			//  `±(+0x8E4 >> 1)`, reached by the `+0x2F2 == 0 || +0x2F0 != 0` gate
			//  @0x48C20E..0x48C228)]. The cbik mover has no
			// airborne clamp — it skips speed INTEGRATION off-contact instead
			// (below) [orig: the contact gate @0x485501..0x485534].
			const int32_t d2 = traits.deceleration >> 1;
			if (m.speed_accel > d2) m.speed_accel = d2;
            if (m.speed_accel < -d2) m.speed_accel = -d2;
		} else {
			// The straight-drive clamp tree [orig: Entity_UpdateVehiclePhysics
			// @0x48AF00 (site @0x48C3D0..0x48C46C)]:
			//   same-direction drive clamps to ±acceleration; a zero target clamps to
			//   ±deceleration; a direction REVERSAL keeps the raw 1/32 chase unclamped.
			const bool reversal = (target_speed > 0 && m.speed <= 0) ||
					(target_speed < 0 && m.speed >= 0) || (target_speed == 0 && m.speed == 0);
			if (!reversal) {
				if (target_speed != 0) {
                    if (m.speed_accel > traits.acceleration) m.speed_accel = traits.acceleration;
                    if (m.speed_accel < -traits.acceleration) m.speed_accel = -traits.acceleration;
				} else if (const Entity *parent = world.registry.get(veh.mount_target);
						parent == nullptr || (parent->flags & 0x100u) != 0) {
					// NPC-mounted zero-command coast retains the raw servo.
					// [orig: cveh @0x48C428..0x48C43F, parent +0x16C]
					if (m.speed_accel > traits.deceleration)
						m.speed_accel = traits.deceleration;
					if (m.speed_accel < -traits.deceleration)
						m.speed_accel = -traits.deceleration;
				}
			}
		}
		// The cbik AND ctan movers integrate speed only in CONTACT [orig: the
		// `!crashed && !(Flags & 0x2000) && BYTE2(aiRef0)` gates
		// @0x485501..0x485534 (cbik) / @0x489f34..0x489f56 with the <48
		// stop snap (ctan)]; the
		// ground core integrates unconditionally [orig: @0x48C2FC..0x48C32A].
		if ((traits.family != VehicleFamily::Bike && traits.family != VehicleFamily::Tank) ||
				(m.grounded && m.crashed == 0 && (veh.flags & kEntityFlagInAir) == 0)) {
			m.speed += m.speed_accel; // [orig: @0x48C302 `currentSpeed += speedAccel`]
			if (target_speed == 0 && std::abs(m.speed) < 48)
				m.speed = 0; // [orig: @0x48C308..0x48C31C]
			if (m.speed_accel == 0) m.speed = target_speed;               // [orig: @0x48C322..0x48C32A]
		}
	}

	if (traits.family == VehicleFamily::Tank)
		track_phase_tick(m.track_phase, m.speed, m.wheel_rate_bam);
	else
		detail::vehicle_wheel_traction_tick(world, veh, traits, target_speed);

	// ------------------------------------------- velocity, gravity, integration
	{
		// Full chassis basis or retained skid frame, with the family recovery
		// tree and airborne velocity hold. The original straight-drive sites
		// remain @0x48ec74 / @0x48ed3b..0x48ed6b, ctan @0x48a5ac..0x48a8b4,
		// and Entity_UpdateVehiclePhysics @0x48AF00 (kong @116468 / @116470,
		// @116425..@116490 and @116804..@116831).
		const bool contact_arm = detail::vehicle_traction_velocity(world, veh, traits, target_speed);
		detail::vehicle_emit_skid_effects(world, veh, traits);
		// Movement trails sample only at the end of the contact arms; the
		// airborne and off-contact arms jump past them.
		// [orig: cveh @0x48CD93..0x48CDFD (off-contact arm @0x48CE02); cbik
		//  @0x486170..0x4861F1 (@0x4861F6); ctan @0x48A60D..0x48A67F (@0x48A684)]
		if (contact_arm)
			detail::vehicle_sample_trails(world, veh, traits, target_speed);
		if (traits.family == VehicleFamily::Bike) {
            // Grounded wheelies retain upward launch velocity.
            // [orig: Entity_UpdateLightVehiclePhysics @ 0x48657E..0x48659D]
            if ((!m.wheelie_active || (veh.flags & kEntityFlagInAir)) && m.slide_z > 0x4000)
                m.slide_z = 0x4000;
            m.slide_z -= kGravityStepBike; // [orig: @0x4865a6 `slideDecay -= 250`]
		} else if (traits.family == VehicleFamily::Tank) {
			// The tank shares the 250 step with the bike — no up-cap
			// [orig: `slideDecay += -250` @0x48a82c in
			// Entity_UpdateTankVehiclePhysics].
			m.slide_z -= kGravityStepBike;
		} else {
			m.slide_z -= kGravityStep; // [orig: @0x48D009 `add [esi+0A0h], -324`]
		}
		// Submerged drag: while the solve-owned in-water flag is up, planar and
		// vertical velocity shed 1/4 per tick [orig: `test Flags,0x8000` then
		// `v -= (v+2)>>2` on all three @0x48d013..0x48d052; identical in the
		// bike mover @0x4865bb..0x4865ed]. The two-HP drown drain
		// (health word +0x11E) is authority-gated — ported below;
		// the zero-health edge clears the attacker [orig: @0x48d05a..0x48d083].
		if ((veh.flags & 0x8000u) != 0) {
			m.vel_x -= (m.vel_x + 2) >> 2;
			m.vel_y -= (m.vel_y + 2) >> 2;
            m.slide_z -= (m.slide_z + 2) >> 2;
			// All ground families lose two health per submerged authority tick.
			// Boats use capsize damage; their afloat flag is normal operation.
			// [orig: cveh @0x48D05A..0x48D083; cbik @0x4865F3..0x48661C;
			// ctan @0x48A87B..0x48A8A4]
			if (world.ai.is_authority && veh.health > 0) {
				veh.health = std::max(0, veh.health - 2);
				if (veh.health == 0)
					veh.last_attacker = {};
			}
		}
		// A crash-settled tank (the +0x2F0 latch) zeroes its planar velocity after
		// the water block — the halving coast alone never reaches zero on a negative
		// LSB (`-1 >> 1 == -1`), so without this the hull creeps forever and the
		// sleep gate never closes [orig: `cmp byte ptr [esi+2F0h],0` @0x48a8ab ->
		// velocityX = velocityY = 0 in Entity_UpdateTankVehiclePhysics].
		if (traits.family == VehicleFamily::Tank && m.settle_2f0 != 0) {
			m.vel_x = 0;
			m.vel_y = 0;
		}
		// The tank alone translates under its recoil and heavy-hit impulse: the
		// unit direction scaled by trunc(amplitude * 28.16f) joins all three
		// velocity words every tick, and the direction clears once the chassis
		// contact byte drops. [orig: Entity_UpdateTankVehiclePhysics
		//  @0x48A8C0..0x48A9C0; writers Entity_InitVehicleSuspensionGeometry
		//  @0x475152..0x4751D4, Entity_SetupInfantryForcePoints @0x475D0E..0x475DB2]
		if (traits.family == VehicleFamily::Tank) {
			const int32_t *dir = m.chassis_impulse_direction;
			const double length = std::sqrt(double(dir[0]) * dir[0] + double(dir[1]) * dir[1] +
					double(dir[2]) * dir[2]);
			if (static_cast<int32_t>(std::min(length, 2147418112.0)) > 0) {
				const int32_t scale = static_cast<int32_t>(
						double(m.chassis_impulse_amplitude) * double(28.16f)); // flt_7C6FA0
				m.vel_x = io::bam_add(m.vel_x, q16_mul_rhu(dir[0], scale));
				m.vel_y = io::bam_add(m.vel_y, q16_mul_rhu(dir[1], scale));
				m.slide_z = io::bam_add(m.slide_z, q16_mul_rhu(dir[2], scale));
			}
			if (!m.chassis_contact_active)
				std::fill_n(m.chassis_impulse_direction, 3, 0);
		}

		const int32_t prev[3] = { to_fixed(veh.position.x), to_fixed(veh.position.y),
			to_fixed(veh.position.z) };
		// The bike integrates through `2 * ftol(v * 0.5)` per axis — the odd LSB
		// of every velocity is dropped each tick (a wire-visible 1-LSB drift on
		// odd values) [orig: Entity_UpdateLightVehiclePhysics `fmul 0.5; ftol;
		// add eax,eax` on X/Y/Z]. Truncation toward zero matches ftol.
		const bool bike = traits.family == VehicleFamily::Bike;
		int32_t px = prev[0] + (bike ? 2 * (m.vel_x / 2) : m.vel_x);
        int32_t py = prev[1] + (bike ? 2 * (m.vel_y / 2) : m.vel_y);
        int32_t pz = prev[2] + (bike ? 2 * (m.slide_z / 2) : m.slide_z);

		// Entity_CheckCollisionState @0x462A30 runs over the authored wheel
		// and spine probes inside each family solve (@0x47CB8C/@0x47D213).
		// Torque severity decay is applied there once [orig: @0x47CC13..0x47CCC1].
		// Ground contact at the witnessed call sites — after Position +=
		// velocity, before the yaw apply [orig: the tracked call @0x48d0b1;
		// the wheeled call @0x48a9ef; the light call @0x486672]. Each family
		// solve rests the hull at wheel height above terrain (pads at
		// box_z_lo + r) and conforms attitude from per-corner lifts; rows
		// outside their activity predicates keep the 5-tap bilinear
		// terrain clamp for an embedder that supplies no authored model probes.
		if (solve_kind == ContactSolveKind::Tracked) {
			ground_contact_solve(world, veh, traits, m, prev[0], prev[1], px, py, pz);
		} else if (solve_kind == ContactSolveKind::Wheeled) {
			wheeled_contact_solve(world, veh, traits, m, prev[0], prev[1], px, py, pz);
		} else if (solve_kind == ContactSolveKind::Light) {
			light_contact_solve(world, veh, traits, m, prev[0], prev[1], px, py, pz);
		} else if (world.tables.terrain != nullptr) {
			const int32_t pos3[3] = { px, py, pz };
			const GroundClearance clearance{};
            const int32_t ground =
                    calc_average_ground_height(*world.tables.terrain, pos3, 0, clearance);
            if (ground != INT32_MIN) {
                if (pz <= ground) {
                    pz = ground;
                    m.slide_z = 0;
                    m.grounded = true;
                } else {
                    // A small clearance still counts as wheel contact (the wheel solver
                    // keeps contact through suspension travel); beyond it = airborne.
                    m.grounded = (pz - ground) <= 0x8000; // 0.5 u suspension margin
                }
            }
		} else {
			m.grounded = true; // no terrain wired (unit worlds): drive on a flat plane
			if (m.slide_z < 0) m.slide_z = 0;
            pz = to_fixed(veh.position.z);
		}

		// Grounded steering applies the wheel yaw rate [orig: @0x48ef60
		// `Yaw += modelPtr0`, gated on ground contact]. The cbik mover ALWAYS
		// applies it, quartered while the airborne/swimming flag is up
		// [orig: @0x486681..0x486697 `Yaw += modelPtr0 >> 2` under Flags 0x2000].
		if (traits.family == VehicleFamily::Bike) {
            // The original's Flags 0x2000 is the suspension solver's current
            // off-contact result. Our portable contact result is m.grounded;
            // veh.flags is not maintained by this stand-in and can be stale.
            const int32_t yaw_step = m.grounded
                    ? m.wheel_rate_bam
                    : io::bam_sar(m.wheel_rate_bam, 2);
            m.yaw_bam = io::bam_add(m.yaw_bam, yaw_step);
		} else if (traits.family == VehicleFamily::Tank && m.settle_2f0 == 0) {
			// Tank yaw is gated by the crash-settled latch and quartered in the
			// airborne arm.
			// Witness sites: [orig: @0x48a9f7, @0x48aa1d]
			const int32_t yaw_step =
					(wheeled_solve ? (veh.flags & kEntityFlagInAir) != 0 : !m.grounded)
					? io::bam_sar(m.wheel_rate_bam, 2)
					: m.wheel_rate_bam;
			m.yaw_bam = io::bam_add(m.yaw_bam, yaw_step);
		} else if (traits.family == VehicleFamily::Ground && m.grounded &&
				(veh.flags & kEntityFlagInAir) == 0 && m.crashed == 0 && m.settle_2f0 == 0) {
			m.yaw_bam = io::bam_add(m.yaw_bam, m.wheel_rate_bam);
		}

		veh.position.x = static_cast<float>(from_fixed(px));
		veh.position.y = static_cast<float>(from_fixed(py));
		veh.position.z = static_cast<float>(from_fixed(pz));
        veh.yaw = static_cast<int16_t>(std::lround(
                normalize_mission_yaw_deg(mission_yaw_deg_from_bam_heading(m.yaw_bam))));
	}

	slew_turret(veh, 0x2108421);

	// Publish the final motor state into the generic, host-owned persistent
	// emitter seam. The claimant gate lives in the sound consumer because the
	// motor still needs to settle an unoccupied PlayerControl vehicle.
	// [orig: Entity_ProcessMovementSoundEffects call @0x48d181..0x48d25c]
	world.vehicles.update_ground_sound(veh, traits, wrecked, m.plat_airborne_ticks > 30);
	world.vehicles.update_traction_sound(veh, traits);
	update_engine_sound(veh, traits);
	// The part-animation accumulators, at the mover's tail — ONLY inside the
	// `itemDef->attrib & 0x40` (PlayerControl) block, in all three full movers
	// [orig: cveh Entity_UpdateVehiclePhysics @0x48AF00 gate @0x48D38B, call
	//  @0x48D42B; ctan Entity_UpdateTankVehiclePhysics @0x488AB0 gate @0x48AD97,
	//  call @0x48AE3D; cbik Entity_UpdateLightVehiclePhysics @0x483FE0 gate
	//  @0x486944, call @0x4869EA]. A non-PlayerControl item never reaches the
	// rotor machine, so it never draws that machine's PRNG roll.
	if (traits.player_control)
		world.vehicles.part_anim_tick(veh, traits);
    if (traits.family == VehicleFamily::Bike)
        m.wheelie_request = 0; // [orig: Entity_UpdateLightVehiclePhysics @ 0x4869F9]
}

namespace {

// bam_of_atan2 moved to vehicle_motor_detail.h (shared with the light solve's
// axle fit); the exact-constant note rides with it.
using detail::bam_of_atan2;

} // namespace

namespace detail {

// The retail Q22 Rz(yaw)*Ry(-pitch)*Rx(roll) builder shared with collision and
// bone transforms (struct in vehicle_motor_detail.h — the contact solves in
// vehicle_contact_solve.cpp consume the same basis). Keeping one
// quantized/sign-correct basis is important: thrust consumes the PREVIOUS
// solve's pitch/roll, then the platform solver writes the attitude for the
// following tick.
VehicleEulerBasis vehicle_euler_basis(int32_t yaw_bam, int32_t pitch_bam,
                                      int32_t roll_bam) {
    VehicleEulerBasis out;
    const int32_t origin[3] = {};
    out.q22 = collision_matrix_from_euler(
            yaw_bam, pitch_bam, roll_bam, origin);
    constexpr double kInvQ22 = io::kInvQ22One;
    out.fwd[0] = double(out.q22.m[0]) * kInvQ22;
    out.fwd[1] = double(out.q22.m[4]) * kInvQ22;
    out.fwd[2] = double(out.q22.m[8]) * kInvQ22;
    out.side[0] = double(out.q22.m[1]) * kInvQ22;
    out.side[1] = double(out.q22.m[5]) * kInvQ22;
    out.side[2] = double(out.q22.m[9]) * kInvQ22;
    out.up[0] = double(out.q22.m[2]) * kInvQ22;
    out.up[1] = double(out.q22.m[6]) * kInvQ22;
    out.up[2] = double(out.q22.m[10]) * kInvQ22;
    return out;
}

} // namespace detail

namespace {

struct VehicleEulerBasisQ16 {
    int32_t fwd_x = 0;
    int32_t fwd_y = 0;
    int32_t fwd_z = 0;
    int32_t up_z = 0;
};

VehicleEulerBasisQ16 vehicle_euler_basis_q16(int32_t yaw_bam,
                                             int32_t pitch_bam,
                                             int32_t roll_bam) {
    const int32_t origin[3] = {};
    const CollisionMatrix basis = collision_matrix_from_euler(
            yaw_bam, pitch_bam, roll_bam, origin);
    VehicleEulerBasisQ16 out;
    // The mover reads the retail-built Q22 matrix columns as Q16 components.
    out.fwd_x = io::bam_sar(basis.m[0], 6);
    out.fwd_y = io::bam_sar(basis.m[4], 6);
    out.fwd_z = io::bam_sar(basis.m[8], 6);
    out.up_z = io::bam_sar(basis.m[10], 6);
    return out;
}

} // namespace

// The per-record vehicle chase (the §5.38e template) applied to a WORLD
// entity's live pose — the interp block every family mover runs before its
// physics. All five movers share the radii, buckets and stale-record coast;
// their crash/settle gates differ:
//   Plain (cbot and both selector-zero movers): snap and step every axis.
//   Ground (cveh): the snap moves heading and Z only while neither crashed nor
//     settled; the per-tick heading step is ungated.
//   Bike (cbik): the snap always moves Z; heading snaps and steps only while
//     unlatched, not wheeling (+0x3DE) and upright at mover entry.
//   Tank (ctan): a crashed, settled or +0x2FC hull widens the snap radius to
//     0xC0000; heading snaps and steps only while unlatched; Z snaps always.
// Every family but Plain steps Z only while airborne and unlatched.
// [orig: cbot @0x48DB6B..0x48DDD4; selector-zero ground @0x46E62E..0x46E85A and
//  boat @0x470129..0x470355; cveh @0x48B563..0x48B7C5 (snap gates
//  @0x48B5FB..0x48B622, Z step @0x48B798..0x48B7B9); cbik @0x484666..0x4848F3
//  (snap @0x4846EB..0x48472D, heading step @0x48487D..0x4848A4, position
//  step @0x4848AE..0x4848E3 with its Z gate @0x4848C2, up.z of the entry
//  row @0x484039); ctan
//  @0x48912B..0x4893A6 (radius @0x48913B..0x48916E, snap heading
//  @0x4891D5..0x489208, heading step @0x48933D..0x48935B, Z step
//  @0x489379..0x48939A)]
void vehicle_client_chase(Entity &veh, VehicleChaseFamily family, int32_t entry_up_z16) {
	Entity::VehicleMotorState &m = veh.veh;
	const bool latched = m.crashed != 0 || m.settle_2f0 != 0;
	bool heading_free = true;
	if (family == VehicleChaseFamily::Tank)
		heading_free = !latched;
	else if (family == VehicleChaseFamily::Bike)
		heading_free = !latched && m.wheelie_active == 0 && entry_up_z16 >= 0;
	const bool z_step = family == VehicleChaseFamily::Plain ||
			((veh.flags & kEntityFlagInAir) != 0 && !latched);
	int32_t px = to_fixed(veh.position.x);
	int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);
    if (m.net_interp_progress == 0) {
        const int64_t dx = int64_t(m.net_smooth_target[0]) - px;
        const int64_t dy = int64_t(m.net_smooth_target[1]) - py;
        const int64_t dz = int64_t(m.net_smooth_target[2]) - pz;
        const double dd = std::sqrt(double(dx) * double(dx) +
                                    double(dy) * double(dy) +
                                    double(dz) * double(dz));
        const int32_t dist = dd >= 2147418112.0 ? INT32_MAX
                                                : static_cast<int32_t>(dd);
        // Snap radius 0x60000 while the received speed says "moving" (>= 293),
        // else 0x20000 [orig: @0x48DB9B..0x48DBAC].
        int32_t snap = m.net_recv_speed >= 293 ? 0x60000 : 0x20000;
        if (family == VehicleChaseFamily::Tank && (latched || m.wreck_2fc != 0))
            snap = 0xC0000;
        if (dist > snap) {
            px = m.net_smooth_target[0];
            py = m.net_smooth_target[1];
            if (family != VehicleChaseFamily::Ground || !latched)
                pz = m.net_smooth_target[2];
            if (family == VehicleChaseFamily::Ground ? !latched : heading_free)
                m.yaw_bam = m.net_smooth_heading;
            m.net_smooth_target[0] = 0;
            m.net_smooth_target[1] = 0;
            m.net_smooth_target[2] = 0;
            m.net_interp_steps = 0;
            m.net_smooth_heading = 0;
        } else if (dist < 0x2000) {
            m.net_smooth_target[0] = 0;
            m.net_smooth_target[1] = 0;
            m.net_smooth_target[2] = 0;
            m.net_interp_steps = 0;
            m.net_smooth_heading = io::bam_add(
                    io::bam_sub(m.net_smooth_heading, m.yaw_bam), 10) / 20;
        } else {
            const int32_t n = vehicle_chase_bucket(dist);
            m.net_interp_steps = static_cast<int16_t>(n);
            m.net_smooth_target[0] =
                    (int32_t(dx) + (n >> 1)) / n;
            m.net_smooth_target[1] =
                    (int32_t(dy) + (n >> 1)) / n;
            m.net_smooth_target[2] =
                    (int32_t(dz) + (n >> 1)) / n;
            m.net_smooth_heading = io::bam_add(
                    io::bam_sub(m.net_smooth_heading, m.yaw_bam), 10) / 20;
        }
    }
    {
        const int16_t progress = m.net_interp_progress;
        if (progress < 20 && heading_free)
            m.yaw_bam = io::bam_add(m.yaw_bam, m.net_smooth_heading);
        if (progress < m.net_interp_steps) {
            px += m.net_smooth_target[0];
            py += m.net_smooth_target[1];
            // On contact, Z is contact-solve owned (the ground solves resettle
            // it every tick), so the ground templates chase record Z only while
            // airborne and neither crashed nor settled; wreck-frozen rows never
            // reach this mover (the sim clears net_predicted).
            if (z_step)
                pz += m.net_smooth_target[2];
        }
        if (progress >= 128) {
            // Stale records: the mirrored command coasts to zero so an
            // abandoned boat predicts to a stop [orig: @0x48DDC0..0x48DDCE].
            m.net_recv_speed -= (m.net_recv_speed + 64) >> 7;
        } else {
            m.net_interp_progress = static_cast<int16_t>(progress + 1);
        }
    }

    veh.position.x = static_cast<float>(from_fixed(px));
    veh.position.y = static_cast<float>(from_fixed(py));
    veh.position.z = static_cast<float>(from_fixed(pz));
}

// Boat platform contact, buoyancy, planing and wreck state. The four pads and three spine probes
// run after integration and before yaw, for authority and prediction. Its second-pass depth buffer
// is also consumed by the grounded branch. See vehicle-client-movers-re.md sections 3 and 12-32.
// Witness sites: [orig: @0x481870, @0x48ECE7, @0x482336, @0x48244D, @0x483494, @0x4834C6,
// @0x462561, @0x482459, @0x45AEA0, @0x4592B0, @0x613310]
namespace detail {

// One bilinear terrain probe force (struct + [orig] cites in
// vehicle_motor_detail.h — shared with the contact solves in
// vehicle_contact_solve.cpp).
int32_t plat_terrain_probe(const World &world, int32_t X, int32_t Y, int32_t Z, int32_t r,
		int32_t soft, int32_t hard, PlatProbeForce &out, bool wheel_probe) {
	if (world.tables.terrain == nullptr)
		return 0;
	const GroundClearance clearance{};
	auto sample = [&](int32_t sx, int32_t sy) {
        const int32_t p3[3] = {sx, sy, Z};
        return calc_average_ground_height(*world.tables.terrain, p3, 0, clearance);
    };
    const int32_t h_xm = sample(X - r, Y), h_xp = sample(X + r, Y);
    const int32_t h_ym = sample(X, Y - r), h_yp = sample(X, Y + r);
    if (h_xm == INT32_MIN) return 0;
    // Height over the 4-sample average [orig: @0x4622A3]; clear by > r skips
    // [orig: @0x4622D2].
    const int32_t rel = Z - (h_yp >> 2) - (h_ym >> 2) - (h_xp >> 2) - (h_xm >> 2);

	const int64_t gx = h_xp - h_xm, gy = h_yp - h_ym, gz = -2 * int64_t(r);
	const double dn = std::sqrt(double(gx) * gx + double(gy) * gy + double(gz) * gz);
	const int32_t norm = dn >= 2147418112.0 ? 2147418112 : int32_t(dn);
	if (norm == 0)
		return 0;
	int64_t fx = gx * r / norm, fy = gy * r / norm;
	int64_t pen = rel + gz * r / norm; // = rel - 2r^2/norm [orig: @0x462360]
	// Optional gap array is written before the broad phase, and clipped to
	// zero for a penetrating point [orig: Entity_CheckCollisionState @0x462A30,
	// @0x462BFB..0x462C5B]. Tank springs consume the latest pass's gaps.
	out.terrain_gap = std::max(0, int32_t(pen));
	if (!wheel_probe && h_xm + r < Z && h_xp + r < Z && h_ym + r < Z && h_yp + r < Z)
		return 0;
	if (pen >= 0) {
		// Grazing case [orig: Entity_ComputeCollisionForces @0x462150 (site
		// @0x4623AF..0x462424); Entity_CheckCollisionState @0x462A30 (site
		// @0x462C51..0x462CD1)]. The product is the two-operand `imul eax, ebx`
		// (@0x4623BF / @0x462C5F and @0x462C77): a 32-bit low dword, so pen * 2r
		// wraps past 2^31 before the `cdq; idiv` — an elevated wheel probe over a
		// slope takes the wrapped quotient, never the wide one.
		const int64_t ax = gx ? bam_mul_wrap(int32_t(pen), int32_t(gz)) / int32_t(gx) : 0;
        const int64_t ay = gy ? bam_mul_wrap(int32_t(pen), int32_t(gz)) / int32_t(gy) : 0;
        bool hit = false;
        if (std::llabs(ax) < std::llabs(fx)) { fx += ax; hit = true; }
        if (std::llabs(ay) < std::llabs(fy)) { fy += ay; hit = true; }
        if (!hit) return 0;
        pen = 0;
	}
	const double dm =
			std::sqrt(double(fx) * fx + double(fy) * fy + double(pen - rel) * double(pen - rel));
	const int32_t mag = dm >= 2147418112.0 ? 2147418112 : int32_t(dm);
	if (mag == 0)
		return 0;
	const int32_t cosr = int32_t((int64_t(rel - pen) << 22) / mag); // [orig: @0x4624A7]
	out.fz -= int32_t(pen); // push-up [orig: @0x4624AD]
    int32_t sev = 0;
    if (cosr < soft) {
        out.fx -= int32_t(fx); out.fy -= int32_t(fy); sev = 3;
    } else if (cosr < ((hard + soft) >> 1)) {
        out.fx -= int32_t(fx >> 2); out.fy -= int32_t(fy >> 2); sev = 2;
    } else if (cosr < hard) {
        out.fx -= int32_t(fx >> 3); out.fy -= int32_t(fy >> 3); sev = 1;
    } // else climbable: vertical only [orig: @0x4624BB..0x462528]
    return sev;
}

} // namespace detail

// q16_mul_rhu / q16_normalize / q16_cross moved to vehicle_motor_detail.h
// (shared with the wheeled/light solves); the witness notes ride with them.

namespace detail {

// The shared 4-normal plane fit, ported in the witnessed fixed/float mix
// (record §4.1 "Port literally"): 16.16-quantized normalized edges, RHU
// fixed-point cross products, the set-1/set-2 duplicated normal, integer
// aggregation with the float `*0.25` round-trip (flt_7C333C), and a final
// quantized normalize per row — NOT clean double math, whose unquantized
// intermediates drift the fitted attitude/Z by LSBs versus retail.
// [orig: fit 0x46C92B../0x46D4C7../0x46D6D8../0x46D8E8 (orientation solver)
//  and 0x46B332../0x46BC76../0x46BE7C../0x46C091 (wheeled solver);
//  aggregation 0x46DC1F..0x46DC69 / 0x46C3B1..0x46C3FB]
void plat_fit_corners(const int32_t c[4][3], PlatFit &out, CollisionMatrix *matrix) {
	auto edge = [&c](int i, int j, int32_t u[3]) {
		const int64_t d[3] = { int64_t(c[i][0]) - c[j][0], int64_t(c[i][1]) - c[j][1],
			int64_t(c[i][2]) - c[j][2] };
		q16_normalize(d, u);
	};
	// Edge sets, verbatim pairing (set 1 == set 2, real shipped duplication —
	// a fourth distinct corner normal is never computed):
	int32_t a[3], b[3], e3[3], f4[3];
    edge(0, 3, a);  // c0 - c3
    edge(3, 2, b);  // c3 - c2
    edge(1, 2, e3); // c1 - c2
    edge(0, 1, f4); // c0 - c1
    int64_t raw[3];
    int32_t n1[3], n2[3], n3[3], n4[3];
    q16_cross(a, b, raw);   q16_normalize(raw, n1);
    q16_cross(a, b, raw);   q16_normalize(raw, n2); // set 2 recomputes set 1
    q16_cross(e3, b, raw);  q16_normalize(raw, n3);
    q16_cross(e3, f4, raw); q16_normalize(raw, n4);
    int64_t up_s[3], fwd_s[3], side_s[3];
    for (int i = 0; i < 3; ++i) {
        up_s[i] = int64_t(n1[i]) + n2[i] + n3[i] + n4[i];
        fwd_s[i] = 2 * int64_t(e3[i]) + 2 * int64_t(a[i]); // e4+e3+a+e2
        side_s[i] = int64_t(f4[i]) + 3 * int64_t(b[i]);    // f4+f3+b+f2
    }
    // `each *= 0.25` through the float constant, ftol'd back [orig:
    // 0x46DC23..0x46DCF0], then the final quantized row normalize.
    for (int i = 0; i < 3; ++i) {
        up_s[i] = static_cast<int64_t>(double(up_s[i]) * 0.25);
        fwd_s[i] = static_cast<int64_t>(double(fwd_s[i]) * 0.25);
        side_s[i] = static_cast<int64_t>(double(side_s[i]) * 0.25);
    }
    int32_t up[3], fwd[3], side[3];
    q16_normalize(up_s, up);
    q16_normalize(fwd_s, fwd);
    q16_normalize(side_s, side);
	if (matrix != nullptr) {
		*matrix = {};
		for (int axis = 0; axis < 3; ++axis) {
			matrix->m[4 * axis] = bam_shl_wrap(fwd[axis], 6);
			matrix->m[4 * axis + 1] = bam_shl_wrap(side[axis], 6);
			matrix->m[4 * axis + 2] = bam_shl_wrap(up[axis], 6);
		}
	}
	out.fwd_z = double(fwd[2]) / 65536.0;
	vehicle_axes_to_euler(fwd, side, up, out.yaw_bam, out.pitch_bam, out.roll_bam);
	// Z = the plain corner average in the ORIENTATION solver [orig: solvedPos.Z
	// @0x46E099..0x46E0B3, `*0.25` via flt_7C333C]; the wheeled solver's
	// leg-C variant averages only the corners with z > 0 (blockers §3) —
	// positive_z_avg below.
	out.z_avg = static_cast<int32_t>(double(int64_t(c[0][2]) + c[1][2] + c[2][2] + c[3][2]) * 0.25);
	int64_t psum = 0;
    int pn = 0;
    for (int k = 0; k < 4; ++k)
        if (c[k][2] > 0) { psum += c[k][2]; ++pn; }
    // All-nonpositive = the witnessed x87 div-by-zero hazard; guard with the
    // plain average (unreachable at real world heights).
    out.positive_z_avg =
            pn > 0 ? static_cast<int32_t>(psum / pn) : out.z_avg;
}

} // namespace detail

void VehicleSystem::watercraft_platform_solve(Entity &veh, const VehicleTraits &traits) {
    World &world = world_;
    Entity::VehicleMotorState &m = veh.veh;
    // No resolved model boxes (lib-only embedders / unresolved graphics):
    // the level-hull + chase-Z stand-in remains for this row.
    if (traits.box_z_hi == traits.box_z_lo || traits.box_y_hi == traits.box_y_lo)
        return;

    const int32_t W = world.env.water_z;
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);

	// [orig: Entity_ProcessPlatformPhysics @0x4818B9..0x481A5C]
	if (m.vel_x == 0 && m.vel_y == 0 && m.speed == 0 && m.wheel_rate_bam == 0 &&
			m.air_pitch_rate == 0 && m.air_roll_rate == 0 &&
			((veh.flags | veh.engine_flags) & (kEntityFlagInAir | 0x40u)) == 0 &&
			m.slide_z > -500 && m.slide_z < 0 && veh.saved_live_valid &&
			px == veh.saved_live_pos[0] && py == veh.saved_live_pos[1] &&
			io::bam_abs(io::bam_sub(pz, veh.saved_live_pos[2])) < 500 &&
			std::all_of(
					std::begin(m.plat_acc), std::end(m.plat_acc),
					[](int32_t value) { return value == 0; })) {
		pz = io::bam_sub(pz, m.slide_z);
		m.slide_z >>= 1;
		vehicle_rest_state(world, veh, VehicleFamily::Watercraft);
		veh.position.z = float(from_fixed(pz));
		return;
	}
	vehicle_expire_contact_wake(world, veh);

	// The every-call tuning clamps [orig: @0x481ACC..0x481BA3]:
	const int32_t t_pitch = std::clamp(traits.pitch_lift, 0, 10);
	const int32_t t_pitch_vel = std::clamp(traits.pitch_lift_vel, 0, 10);
    const int32_t t_bob = std::clamp(traits.bob, 0, 10);

    // ---- §3 probe geometry. Probe springs +0x2D4.. are provably zero for
    // pure boats (blockers §4). q = beam/4.
    const int32_t q = (traits.box_y_hi - traits.box_y_lo) >> 2;
    if (q <= 0) return;
    m.plat_solve_valid = true;
    const int32_t zb = traits.box_z_lo + q;
    // 4 corner probes + 3 midline probes (model space) [orig: @0x481CB4..].
    int32_t rm = std::min(((traits.box_z_hi - traits.box_z_lo) >> 1) - 0x4000,
                          ((traits.box_y_hi - traits.box_y_lo) >> 1) - 0x1000);
    if (rm < 0x2000) rm = 0x2000;
    const int32_t lx = traits.box_x_hi - traits.box_x_lo;
    const int32_t ymid = traits.box_y_lo + ((traits.box_y_hi - traits.box_y_lo) >> 1);
    const int32_t zt = traits.box_z_hi - rm;
    const int32_t probes_model[7][3] = {
        {traits.foot_x_hi - q, traits.foot_y_hi - q, zb}, // p0 bow-A
        {traits.foot_x_hi - q, traits.foot_y_lo + q, zb}, // p1 bow-B
        {traits.foot_x_lo + q, traits.foot_y_lo + q, zb}, // p2 stern-B
        {traits.foot_x_lo + q, traits.foot_y_hi - q, zb}, // p3 stern-A
        {traits.box_x_lo + (lx >> 2), ymid, zt},          // p4
        {traits.box_x_lo + ((3 * lx) >> 2), ymid, zt},    // p5 [(3*lx)>>2 verbatim]
        {traits.box_x_lo + (lx >> 1), ymid, zt},          // p6
    };
    const int32_t radii[7] = {q, q, q, q, rm, rm, rm};
    const int32_t v210 = -(traits.box_z_lo + q); // corner-0 model-Z, negated (§3)
    // Half-extents of the lever rectangle (§3 tail).
    const int32_t hb = (traits.foot_y_hi - q) - (traits.foot_y_lo + q);
    const int32_t hl = (traits.foot_x_hi - q) - (traits.foot_x_lo + q);

    // World transform: the pose matrix from {Pos, Yaw, Pitch, Roll} — the
    // boat's live attitude rides air_pitch_bam/air_roll_bam (the shared
    // attitude fields the sim mirrors to the row).
	VehicleEulerBasis basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	const double *fwdv = basis.fwd;
	const double *sidev = basis.side;
	const double *upv = basis.up;
    int32_t probes[7][3];
    place_probes(basis, probes_model, px, py, pz, probes);

    // ---- §4/§6 first force pass + severity response (plat_probe_pass).
    const int32_t soft = cos22_of_bam_x87(traits.max_slope);
    const int32_t hard = cos22_of_bam_x87(traits.slip_slope);
    PlatProbeForce forces[7];
	EntityHandle hit_entity;
	const int32_t sev =
			plat_probe_pass(world, veh, probes, radii, soft, hard, forces, px, py, pz, &hit_entity);
	vehicle_contact_impact(world, veh, traits, sev, hit_entity, px, py, pz);
	if (sev == 1) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x4821E7]
	} else if (sev == 2) {
		m.speed -= m.speed >> ((traits.torque + 1) & 31); // [orig: @0x4822A4]
	} else if (sev == 3) {
		m.speed -= m.speed >> ((traits.torque + 2) & 31); // [orig: @0x4822C9]
		// The shared contact fold applies authority damage, sound and momentum. The quarter-speed
		// cut requires a terrain-only hit beyond the hull. The original yaw-kick arm is
		// unreachable.
		// Witness sites: [orig: @0x482546, @0x4825DD, @0x4825E3, @0x48262D, @0x4826EB]
		if (!hit_entity.valid() && strongest_probe_beyond_hull(forces, probes, 7, px, py))
			m.speed = int32_t(m.speed * 0.25); // [orig: flt_7C333C @0x4826EB]
	}

	// ---- §7 position push + second pass (severity >= 1 only). zc[] mirrors
	// the SHARED force buffer the grounded leg reads [orig: §10-A
	// @0x483D5C..0x483F04]: pass 1 fills it; the sev>=1 second pass re-zeroes
	// and refills it; when §7 is skipped (sev 0, climbable contact) it still
	// holds the PASS-1 forces -- the saved zf[] carries the averaged values.
	int32_t zf[7], zc[7];
    for (int i = 0; i < 7; ++i) { zf[i] = forces[i].fz; zc[i] = forces[i].fz; }
    if (sev >= 1) {
        int64_t dX = 0, dY = 0;
        for (int i = 0; i < 7; ++i) { dX += forces[i].fx; dY += forces[i].fy; }
        for (int i = 0; i < 7; ++i) { probes[i][0] += int32_t(dX); probes[i][1] += int32_t(dY); }
        PlatProbeForce forces2[7];
		const int32_t sev2 =
				plat_probe_pass(world, veh, probes, radii, soft, hard, forces2, px, py, pz);
		for (int i = 0; i < 7; ++i)
			zc[i] = forces2[i].fz;
		if (sev2 != 0) {
			int64_t dX2 = 0, dY2 = 0;
            for (int i = 0; i < 7; ++i) { dX2 += forces2[i].fx; dY2 += forces2[i].fy; }
            for (int i = 0; i < 7; ++i) zf[i] = (forces2[i].fz + zf[i]) >> 1;
            dX = (dX2 + dX) >> 1;
            dY = (dY2 + dY) >> 1;
		}
		int32_t dx = int32_t(dX), dy = int32_t(dY);
		vehicle_contact_mass_share(world, veh, hit_entity, dx, dy, zf, 6);
		px += dx;
		py += dy; // Z-sum always 0 [orig: @0x482A86..0x482A8C]
	}

	// ---- §5 bob/lift parameters.
	const double F = double(q);
    const int32_t amp = std::min(int32_t(F * 0.0625), 352);
    int32_t floatH, liftHi, liftLo, planeSpd, pitchThr;
    if (traits.mass <= 1) { // LIGHT boat [orig: @0x482105]
        floatH = int32_t(0.65 * F);
        liftHi = t_pitch_vel * 100;
        liftLo = 250;
        planeSpd = 10000;
        pitchThr = int32_t(double(t_pitch) * 0.1 * 4096.0);
    } else { // HEAVY [orig: @0x48221B]
        floatH = q;
        liftHi = int32_t(double(t_pitch_vel) * F * 0.004);
        liftLo = liftHi >> 1;
        planeSpd = 20000;
        const double arz = std::abs(sidev[2]) * 65536.0;
        if (arz > double(0x2000)) { // heavily rolled [orig: @0x482259]
            pitchThr = int32_t(double(t_pitch) * 0.1 * 409.6);
            liftHi = 250;
        } else {
            pitchThr = int32_t(double(t_pitch) * 0.1 * 4096.0);
        }
    }
    const int32_t dipExit = int32_t(double(pitchThr) * (1.0 - 0.1 * double(t_bob)));

    // ---- §8 water leg: per-corner submersion, draft, the afloat flag.
    int32_t sub_k[4], cz_k[4];
    for (int k = 0; k < 4; ++k) {
        cz_k[k] = probes[k][2];
        sub_k[k] = W + q - cz_k[k];
    }
    const int32_t avg = (cz_k[0] + cz_k[1] + cz_k[2] + cz_k[3]) >> 2;
    int32_t draft = avg;
	// [orig: Entity_ProcessPlatformPhysics @ 0x481870, callback test @ 0x482B6E]
	if (m.plat_afloat)
		draft = traits.amphibian ? avg - (q >> 1) : int32_t(double(avg) - 0.9 * F);
	// Zero water height is the embedder no-water sentinel. Amphibians and boats retain their
	// separate draft hysteresis.
	if (W == 0 || draft + v210 >= W) {
		m.plat_afloat = false; // [orig: @0x482DB7]
	} else {
		// The first submerged pad emits the local splash/wake and queues the positioned sound at
		// the water plane. Flags 0x8000 latches this edge.
		// Witness sites: [orig: @0x482BB9, @0x482C9D, @0x50a1b0]
		m.plat_afloat = true; // [orig: @0x482CA5]
	}
	const bool trail_was_water = (veh.flags & 0x8000u) != 0;
	veh.flags = m.plat_afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);
	detail::vehicle_trail_water_transition(veh, traits, trail_was_water);
	int water_pad = 0;
	for (int k = 1; k < 4; ++k)
		if (cz_k[k] < cz_k[water_pad])
			water_pad = k;
	detail::vehicle_water_entry(
			world, veh, probes[water_pad][0], probes[water_pad][1], trail_was_water);

	// ---- §9 lever corners around the CURRENT pose + machines. Built through
	// the same Q22 rotate as the probes — the witnessed corner products are
	// round-half-up 16.16 through the pose matrix [orig: @0x482E31..0x48346D],
	// not double-precision basis-row sums.
	int32_t c[4][3];
	{
        const int32_t hb2 = hb >> 1, hl2 = hl >> 1;
        const int32_t corner_model[4][3] = {
            {+hl2, -hb2, 0}, // c0: (-hb/2)*right + (+hl/2)*fwd
            {+hl2, +hb2, 0}, // c1
            {-hl2, -hb2, 0}, // c2
            {-hl2, +hb2, 0}, // c3
        };
        for (int k = 0; k < 4; ++k) {
            int32_t rotated[3];
            basis.q22.rotate_point(corner_model[k], rotated);
            c[k][0] = px + rotated[0];
            c[k][1] = py + rotated[1];
            c[k][2] = pz + rotated[2];
        }
    }
	vehicle_crash_state(world, veh, traits, nullptr, int32_t(upv[2] * 65536.0));
	basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	// Righting can replace the attitude after the first lever-corner build.
	{
		const int32_t hx = hl >> 1, hy = hb >> 1;
		const int32_t local[4][3] = { { hx, -hy, 0 }, { hx, hy, 0 }, { -hx, -hy, 0 },
			{ -hx, hy, 0 } };
		for (int k = 0; k < 4; ++k) {
			int32_t rotated[3];
			basis.q22.rotate_point(local[k], rotated);
			c[k][0] = io::bam_add(px, rotated[0]);
			c[k][1] = io::bam_add(py, rotated[1]);
			c[k][2] = io::bam_add(pz, rotated[2]);
		}
	}

	// Accumulator ramp [orig: @0x483680..0x4836E6].
	for (int k = 0; k < 4; ++k)
		if (zf[k] <= 0 && sub_k[k] <= floatH) m.plat_acc[k] += 250;
    // Bow lift / planing / porpoise [orig: @0x4836EC..0x483891]. The command
    // register is brain[136] (brain+0x220) — the COMMANDED speed. On a remote
    // boat the mirror makes it equal the received register, but the local
    // driver's [136] is the §1.8 current/received average, and the solve reads
    // that averaged command, not the raw wire value.
    const int32_t cmd = m.cmd_speed;
    const double fwd_z_now = fwdv[2] * 65536.0;
    if (m.speed > 0 && liftLo > 0 && liftHi > 0) {
        if (m.plat_afloat && cmd > 0) {
            m.plat_acc[0] = m.plat_acc[1] = 0;
            m.plat_planing = true;
            if (!m.plat_porpoise) {
                if (fwd_z_now > double(pitchThr)) {
                    // dist/293 > (cmd/293)*0.5 -> latch (the per-tick travel
                    // is our speed here; the saved-pose dist is equivalent
                    // between integrations).
                    if (m.speed / 293 > (cmd / 293) / 2) m.plat_porpoise = true;
                } else {
                    const int32_t lift = (m.speed >= planeSpd) ? liftHi : liftLo;
                    c[0][2] += lift;
                    c[1][2] += lift; // bow rises
                }
            } else {
                if (fwd_z_now < double(dipExit)) m.plat_porpoise = false;
                else { c[0][2] -= liftHi; c[1][2] -= liftHi; }
            }
        } else if (cmd <= 0) {
            m.plat_planing = false;
        }
    }
	// Post-machine adjustments [orig: @0x483893..0x48398C].
	if (m.plat_planing) {
		if (m.plat_afloat && m.speed > 0x2000) {
			vehicle_boat_lean(veh, traits, basis.q22.m[9] >> 6);
			m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
		} else {
			const int32_t error = basis.q22.m[9] >> 6;
			m.byte_2ef = 0;
			m.air_roll_rate = io::bam_abs(error) > 240 ? (error < 0 ? -2386239 : 2386092) : 0;
			m.part_spin.angle = 0;
			m.gear_phase = 0;
		}
	} else {
		if (!m.plat_at_rest) {
			for (int k = 0; k < 4; ++k)
				c[k][2] -= 500;
			m.byte_2ef = 0;
			m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
		}
		m.part_spin.angle = 0;
		m.gear_phase = 0;
	}

	// ---- §10 solve select.
	bool any_ground = false;
	for (int k = 0; k < 4; ++k) any_ground |= zf[k] > 0;
    PlatFit fit;
    if (any_ground) {
        // A. Grounded: airborne cleared HERE and only here.
        veh.flags &= ~kEntityFlagInAir; // [orig: @0x483D55]
        for (int k = 0; k < 4; ++k) {
            int32_t lift;
            if (sub_k[k] > floatH) {
                // Deep + grounded: the raw call-#2 force when the probe force
                // exceeds the submersion, else raise to the waterline
                // [orig: @0x483D5C..0x483F04 reads zc_k].
                lift = (zf[k] > sub_k[k]) ? zc[k] : std::abs(sub_k[k] - floatH);
                m.plat_acc[k] = 0;
            } else if (zf[k] != 0) {
                lift = zc[k];
                m.plat_acc[k] = 0;
            } else {
                lift = 250 - m.plat_acc[k];
            }
            c[k][2] += lift;
        }
        m.slide_z = 0; // [orig: @0x483F25]
		vehicle_boat_suspension_fit(world, veh, traits, c, fit, px, py, pz);
		pz = fit.z_avg;
		m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] = m.plat_acc[3] = 0;
	} else {
        // At-rest bob arm [orig: @0x4839C5..0x483A29].
        int32_t floatH_eff = floatH;
        if (m.speed < 100 && m.plat_afloat &&
            std::abs(fwd_z_now) < 5.0 && std::abs(sidev[2] * 65536.0) < 5.0) {
            if (!m.plat_at_rest) {
                m.plat_at_rest = true;
                m.plat_bob_phase = 4.71f; // 3pi/2 [orig: flt_7C6F84]
            }
            m.plat_bob_phase += 0.03488888964056969f; // [orig: flt_7C6EB4]
            const double f =
                std::min(1.0, (std::sin(double(m.plat_bob_phase)) + 1.0) * 0.5);
            const int32_t inc = int32_t(double(amp) * f);
            for (int k = 0; k < 4; ++k) c[k][2] += inc;
            floatH_eff = floatH - inc;
        } else {
            m.plat_at_rest = false;
        }
        bool any_deep = false;
        for (int k = 0; k < 4; ++k) any_deep |= sub_k[k] > floatH_eff;
        if (any_deep) {
            // B. Buoyancy push-up.
            if (m.slide_z < 0) m.slide_z = 0; // [orig: @0x483C15]
            for (int k = 0; k < 4; ++k) {
                int32_t lift;
                if (sub_k[k] >= floatH_eff) {
                    lift = std::abs(sub_k[k] - floatH_eff);
                    m.plat_acc[k] = 0;
                } else {
                    lift = std::min(0, 250 - m.plat_acc[k]);
                }
                c[k][2] += lift;
            }
			vehicle_boat_suspension_fit(world, veh, traits, c, fit, px, py, pz);
			pz = fit.z_avg;
		} else {
			// C. Settled / airborne.
            if (m.plat_afloat) {
                int lowest = 0, second = 1;
                for (int k = 1; k < 4; ++k)
                    if (c[k][2] < c[lowest][2]) lowest = k;
                second = lowest == 0 ? 1 : 0;
                for (int k = 0; k < 4; ++k)
                    if (k != lowest && c[k][2] < c[second][2]) second = k;
                if (cmd <= 0) {
                    m.plat_acc[lowest] = m.plat_acc[second] = 0;
                    if (fwd_z_now < 100.0)
                        m.plat_acc[0] = m.plat_acc[1] = m.plat_acc[2] =
                                m.plat_acc[3] = 0;
                }
                for (int k = 0; k < 4; ++k)
                    c[k][2] += std::min(0, 250 - m.plat_acc[k]);
            } else {
                veh.flags |= kEntityFlagInAir; // [orig: @0x483B98]
            }
            // The wheeled solver's settled/airborne fit: same 4-normal fit;
            // Z = the average of the ABOVE-ground corners, rise-clamped
            // +0x2000/tick (blockers §3).
			vehicle_suspension_fit(world, veh, c, nullptr, fit, px, py, pz);
			if (m.plat_afloat) {
				// The wheeled solver's settled-fit Z: the average of the
				// ABOVE-ZERO corners only, rise-clamped +0x2000/tick
				// (blockers §3).
				int32_t new_z = fit.positive_z_avg;
                if (new_z > pz + 0x2000) new_z = pz + 0x2000;
                pz = new_z;
			}
		}
	}
	// [orig: @0x4839EF..0x483FA9] crashed hulls also adopt solved yaw.
	if (m.crashed)
		m.yaw_bam = fit.yaw_bam;
	m.air_pitch_bam = fit.pitch_bam;
	m.air_roll_bam = fit.roll_bam;
	// Airborne tick counter [orig: @0x483FAF/0x483FC8].
    m.plat_airborne_ticks =
            (veh.flags & kEntityFlagInAir) != 0 ? m.plat_airborne_ticks + 1 : 0;

    veh.position.x = float(from_fixed(px));
    veh.position.y = float(from_fixed(py));
    veh.position.z = float(from_fixed(pz));
}

namespace {

bool watercraft_has_platform_geometry(const VehicleTraits &traits) {
    return traits.box_z_hi != traits.box_z_lo &&
           ((traits.box_y_hi - traits.box_y_lo) >> 2) > 0;
}

// Establish only the platform water latch for a newly promoted client hull.
// The full solve's water decision depends on the four transformed corner
// heights but not on its later collision/bob mutations, so this gives the first
// mover tick the state a preceding retail platform frame would have supplied
// without double-running gravity, accumulators, or the attitude fit.
void watercraft_seed_platform_latch(World &world, Entity &veh,
                                    const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    const int32_t q = (traits.box_y_hi - traits.box_y_lo) >> 2;
    if (q <= 0) return;
    const int32_t zb = traits.box_z_lo + q;
    const int32_t corners[4][3] = {
        {traits.foot_x_hi - q, traits.foot_y_hi - q, zb},
        {traits.foot_x_hi - q, traits.foot_y_lo + q, zb},
        {traits.foot_x_lo + q, traits.foot_y_lo + q, zb},
        {traits.foot_x_lo + q, traits.foot_y_hi - q, zb},
    };
    const int32_t pos[3] = {to_fixed(veh.position.x),
                            to_fixed(veh.position.y),
                            to_fixed(veh.position.z)};
    const int32_t origin[3] = {};
    const CollisionMatrix basis = collision_matrix_from_euler(
            m.yaw_bam, m.air_pitch_bam, m.air_roll_bam, origin);
    int32_t corner_z[4];
    for (int i = 0; i < 4; ++i) {
        int32_t rotated[3];
        basis.rotate_point(corners[i], rotated);
        corner_z[i] = pos[2] + rotated[2];
    }
    const int32_t avg =
            (corner_z[0] + corner_z[1] + corner_z[2] + corner_z[3]) >> 2;
    const int32_t v210 = -(traits.box_z_lo + q);
    // The platform solve's afloat decision is hysteretic: once afloat, the
    // boat-form draft is 0.9*beam-quarter below the corner average. A promoted
    // client row has lost that prior latch, so reconstruct the basin it would
    // already occupy instead of evaluating the dry-entry arm at the waterline.
    const int32_t afloat_draft =
            static_cast<int32_t>(double(avg) - 0.9 * double(q));
    m.plat_afloat = world.env.water_z != 0 &&
            afloat_draft + v210 < world.env.water_z;
    m.plat_solve_valid = true;
	const bool trail_was_water = (veh.flags & 0x8000u) != 0;
	veh.flags = m.plat_afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);
	detail::vehicle_trail_water_transition(veh, traits, trail_was_water);
}

// The inmatch headless host boots World without the shell's model-box
// resolution (VehicleTraits.box_z_* is stamped only by the binding's
// simulation_assets resolve), so its watercraft have no platform
// geometry and the solver cannot produce an afloat latch. This water-plane
// stand-in covers exactly that path; resolved hulls always consume the
// prior platform solve.
void watercraft_refresh_fallback_afloat(World &world, Entity &veh, const VehicleTraits &traits) {
	Entity::VehicleMotorState &m = veh.veh;
	bool afloat = false;
	if (world.env.water_z != 0) {
        if (world.tables.terrain == nullptr) {
            afloat = true;
        } else {
            const int32_t pos[3] = {to_fixed(veh.position.x),
                                    to_fixed(veh.position.y),
                                    to_fixed(veh.position.z)};
            const GroundClearance clearance{};
            const int32_t ground = calc_average_ground_height(
                    *world.tables.terrain, pos, 0, clearance);
            afloat = ground == INT32_MIN || ground < world.env.water_z;
        }
    }
    m.plat_afloat = afloat;
	const bool trail_was_water = (veh.flags & 0x8000u) != 0;
	veh.flags = afloat ? (veh.flags | 0x8000u) : (veh.flags & ~0x8000u);
	detail::vehicle_trail_water_transition(veh, traits, trail_was_water);
}

} // namespace

static void watercraft_motor_core(World &world, Entity &veh,
                                  const VehicleTraits &traits);

// Live full-precision pose shared by all carrier-delta readers.
// [orig: Entity_UpdateWatercraftPhysics @0x48D480, carrier @0x48D6DA..0x48DACD;
// Entity_UpdateAircraftPhysics @0x490310, carrier @0x4905BC..0x49095B]
void carrier_pose_fixed(const Entity &e, int32_t pos[3], int32_t &yaw,
                        int32_t &pitch, int32_t &roll) {
    pos[0] = to_fixed(e.position.x);
    pos[1] = to_fixed(e.position.y);
    pos[2] = to_fixed(e.position.z);
	if (e.veh.yaw_seeded) {
		yaw = e.veh.yaw_bam;
		pitch = e.veh.air_pitch_bam;
		roll = e.veh.air_roll_bam;
	} else {
		yaw = bam_heading_from_mission_yaw_deg(e.yaw);
		pitch = static_cast<int32_t>(std::llround(static_cast<double>(e.pitch) / kDegreesPerBam));
		roll = static_cast<int32_t>(
            std::llround(static_cast<double>(e.roll) / kDegreesPerBam));
	}
}

void stamp_saved_live_pose(Entity &e) {
    carrier_pose_fixed(e, e.saved_live_pos, e.saved_live_yaw,
                       e.saved_live_pitch, e.saved_live_roll);
    e.saved_live_valid = true;
}

void VehicleSystem::watercraft_client_tick(Entity &veh, const VehicleTraits &traits) {
	if (traits.physics == 0) {
		tick_simple_motor(veh, traits, nullptr, true);
		return;
	}
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	if (!m.net_predicted) return;
	update_engine_sound(veh, traits);
	if (!m.yaw_seeded) {
		m.yaw_bam = bam_heading_from_mission_yaw_deg(veh.yaw);
		m.yaw_seeded = true;
	}

	vehicle_refresh_ground_link(world, veh, traits);
	vehicle_follow_carrier(world, veh);

	// ---- 1. Per-record chase (the §5.38e vehicle template) on the world pose.
	vehicle_client_chase(veh, VehicleChaseFamily::Plain, 0);
	if (!watercraft_has_platform_geometry(traits)) {
		watercraft_refresh_fallback_afloat(world, veh, traits);
	} else if (!m.plat_solve_valid) {
		// An exact-handle wire-materialized joiner hull has no earlier local frame, but the mover
		// consumes the PREVIOUS platform solve's afloat/attitude state. Seed
		// that state before its first thrust/drag pass instead of treating a
		// resolved floating hull as landed for one frame.
		watercraft_seed_platform_latch(world, veh, traits);
	}

	// Remote prediction mirrors the received command registers; local-driver reconciliation
	// retains the authored steering input.
	// Witness sites: [orig: @0x48DDD4, @0x48DDF4, @0x490C9E]
	if (Entity *local_driver = resolve_local_vehicle_controller(world, veh, traits)) {
		stage_player_vehicle_input(world_, veh, *local_driver, traits);
		// Retail reconciles only longitudinal command on the controlling
		// client; its steer target stays owned by current local LOOK/input.
		m.cmd_speed = io::bam_sar(
                io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
	} else {
		m.cmd_speed = m.net_recv_speed;
		m.steer_target_bam = m.net_recv_steer_bam;
	}

	watercraft_motor_core(world, veh, traits);
}

// Blocks 12..19 of the cbot mover — the steer integrator through the yaw apply.
// ONE sequence in the original, executed by every machine that runs the function
// past the input gate: the authority (with the occupant/AI/parked staging done),
// the driver's client (local prediction), and remote clients (register mirror).
// Extracted verbatim from the 2026-07-31 client-subset port; the authority tick
// below stages its inputs and runs the same core.
// [orig: Entity_UpdateWatercraftPhysics @0x48D480, blocks @0x48E82C..0x48ED76]
static void watercraft_motor_core(World &world, Entity &veh,
                                  const VehicleTraits &traits) {
    Entity::VehicleMotorState &m = veh.veh;
    int32_t px = to_fixed(veh.position.x);
    int32_t py = to_fixed(veh.position.y);
    int32_t pz = to_fixed(veh.position.z);

    // ---- 3. Steer/rudder integrator [orig: @0x48E82C..0x48E926].
    {
        const int32_t turn_rate = traits.turn_rate;
        // Retail reads ONLY itemDef waterSpeed here; a zero pins the speed
        // fraction at 0 so the effective turn rate stays at min_rate — no
        // player_speed fallback exists [orig: the jz to the f=0 arm @0x48E844].
        const int32_t water_spd = traits.water_speed;
        int32_t min_rate = io::bam_sar(turn_rate, 2);
        if (traits.turn_rate2 != 0) min_rate = traits.turn_rate2;
        int32_t f = 0;
        if (water_spd != 0) {
            f = 0x10000 - static_cast<int32_t>(
                    (static_cast<int64_t>(m.speed) * 0x10000) / water_spd);
        }
        if (f < 0) f = 0;
        // No upper clamp — a reversing hull over-rotates, witnessed absent.
        const int32_t eff = io::bam_add(min_rate, static_cast<int32_t>(
                (static_cast<int64_t>(io::bam_sub(turn_rate, min_rate)) * f +
                        0x8000) >> 16));
        int32_t delta = io::bam_sar(io::bam_add(
                io::bam_sub(m.steer_target_bam, m.yaw_bam), 32), 6);
        if (delta > eff) delta = eff;
        const int32_t neg_eff = io::bam_sub(0, eff);
        if (delta < neg_eff) delta = neg_eff;
        const int32_t steer_error = io::bam_sub(
                io::bam_sub(4, bam_shl_wrap(delta, 5)), m.steer_state);
        m.steer_state = io::bam_add(
                m.steer_state, io::bam_sar(steer_error, 3));
        // Airborne, non-afloat hulls retain their existing yaw rate; grounded
        // or floating hulls recompute from the rudder state.
        // [orig: @0x48E8E3..0x48E920]
        if ((veh.flags & kEntityFlagInAir) == 0 || m.plat_afloat) {
            const int32_t neg_speed = io::bam_sub(0, m.speed);
            m.wheel_rate_bam = static_cast<int32_t>(
                    (static_cast<int64_t>(neg_speed) *
                             io::bam_sar(m.steer_state, 2) + 0x8000) >> 16);
        }
    }

    // ---- 4. Thrust [orig: @0x48E926..0x48EA12].
    int32_t vertical_thrust = 0;
    {
        const int32_t cmd = m.cmd_speed;
        if (cmd == 0) {
            if (io::bam_abs(m.vel_x) < 384) m.vel_x = 0;
            if (io::bam_abs(m.vel_y) < 384) m.vel_y = 0;
        } else {
            const int32_t a = io::bam_abs(cmd);
            int32_t acc = io::bam_add(
                    traits.acceleration,
                    io::bam_add(io::bam_sar(a, 8), io::bam_sar(a, 7)));
            const int32_t accel = cmd >= 0 ? std::min(acc, cmd)
                                           : std::max(io::bam_sub(0, acc), cmd);
            // Retail consumes the full Euler matrix from the PREVIOUS platform
            // solve. Roll contributes the capsize up[2] gate; pitch tilts the
            // forward thrust into X/Y/Z. The new solve runs later in this tick,
            // after position integration [orig: @0x48E972..0x48E9FF].
            const VehicleEulerBasisQ16 basis = vehicle_euler_basis_q16(
                    m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
            if (basis.up_z > 0) {
                m.vel_x += static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_x +
                                0x8000) >> 16);
                m.vel_y += static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_y +
                                0x8000) >> 16);
                vertical_thrust = static_cast<int32_t>(
                        (static_cast<int64_t>(accel) * basis.fwd_z +
                                0x8000) >> 16);
                // The wheel phase is this block's inline tail — a capsized
                // (up.z <= 0) or unpowered hull advances no phase [orig:
                // `mov edx,[ebp+220h]; shl edx,0Dh; add [esi+2B8h],edx`
                // @0x48E9F0..0x48E9F9, inside the `cmp var_98(up.z), 0; jle`
                // gate @0x48E9A1..0x48E9A6 and the `cmd == 0` skip @0x48E92E].
                m.wheel_phase = watercraft_wheel_phase_step(m.wheel_phase, cmd);
            }
        }
    }

    // ---- 5. Drag / slip / keel [orig: @0x48EA14..0x48EBB3, FPU-reconstructed].
    {
        m.vel_x -= m.vel_x >> 6;
        m.vel_y -= m.vel_y >> 6;
        const int32_t vx = m.vel_x, vy = m.vel_y;
        const int32_t vel_heading = bam_of_atan2(double(vy), double(vx));
        const int32_t slip = io::bam_sub(m.yaw_bam, vel_heading);
        const int32_t s22 = sin22_of_bam_x87(slip);
        const int32_t c22 = cos22_of_bam_x87(slip);
        const double dm = std::sqrt(double(vx) * double(vx) +
                                    double(vy) * double(vy));
        int32_t mag = dm >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(dm);
        if (mag > 0x10000) mag = 0x10000; // 1.0 u/tick planar clamp
        const int32_t lateral = static_cast<int32_t>(
                (static_cast<int64_t>(s22) * mag) >> 22); // no rounding bias
        const int32_t along = static_cast<int32_t>(
                (static_cast<int64_t>(c22) * mag) >> 22);
        m.speed = along; // currentSpeed — signed, negative in reverse
        // Keel: bleed 1/32 of the cross-track speed along the beam axis. The
        // beam constant is the raw 0x3FFFFFC0 (+90 deg minus 0x40 under the
        // pi=0x7FFF8000 convention) — port verbatim, do not "fix" it.
        const int32_t beam = io::bam_add(m.yaw_bam, 0x3FFFFFC0);
        const int32_t bs22 = sin22_of_bam_x87(beam);
        const int32_t bc22 = cos22_of_bam_x87(beam);
        m.vel_x += static_cast<int32_t>(
                (static_cast<int64_t>(lateral >> 5) * bc22) >> 22);
        m.vel_y += static_cast<int32_t>(
                (static_cast<int64_t>(lateral >> 5) * bs22) >> 22);
        if (along > m.cmd_speed) { // overspeed shed
            m.vel_x -= m.vel_x >> 6;
            m.vel_y -= m.vel_y >> 6;
        }
    }

    // ---- 6. Contact drags [orig: @0x48EBB5..0x48ECA6]. This tick consumes
    // the prior platform solve's authoritative afloat latch [orig: test
    // Flags 0x8000 @0x48EBB5]. NOT afloat (landed AND airborne): planar
    // sheds + yaw-rate shed + gravity slideDecay -= 167 [orig:
    // @0x48EBD3..0x48EC13, the -167 @0x48EBD9]; afloat + at-rest: the heave
    // counterweight slideDecay -= 8350 [orig: @0x48EBBE..0x48EBC7]; afloat +
    // moving: no vertical write. A BOXLESS row (the solve early-returns)
    // keeps the terrain-derived stand-in and no gravity (Z stays
    // chase-owned there).
    const bool solve_active =
            traits.box_z_hi != traits.box_z_lo && traits.box_y_hi != traits.box_y_lo;
    {
        bool afloat;
        if (solve_active) {
            afloat = m.plat_afloat;
        } else {
            afloat = false;
            int32_t ground_here = INT32_MIN;
            if (world.tables.terrain != nullptr) {
                const int32_t pos3[3] = {px, py, pz};
                const GroundClearance clearance{};
                ground_here =
                        calc_average_ground_height(*world.tables.terrain, pos3, 0, clearance);
            }
            if (world.env.water_z != 0 && ground_here != INT32_MIN)
                afloat = ground_here < world.env.water_z;
            else if (world.env.water_z != 0 && world.tables.terrain == nullptr)
                afloat = true; // headless/no-terrain world with water: float
        }
        if (!afloat) {
            m.vel_x -= (m.vel_x + 4) >> 3;
            m.vel_y -= (m.vel_y + 4) >> 3;
            m.wheel_rate_bam = io::bam_sub(
                    m.wheel_rate_bam,
                    io::bam_sar(io::bam_add(m.wheel_rate_bam, 2), 2));
            if (solve_active) m.slide_z -= 167; // [orig: @0x48EBD9]
        } else if (solve_active && m.plat_at_rest) {
            m.slide_z -= 8350; // [orig: @0x48EBC7]
        }
        // Shore look-ahead: the witnessed single bilinear sample at the NEXT
        // position [orig: Terrain_SampleHeightBilinear(pos + vel)
        // @0x48EC19..0x48EC2D] — radius 0 with default clearance collapses
        // calc_average_ground_height to exactly that center-only column.
        if (world.tables.terrain != nullptr && world.env.water_z != 0) {
            const int32_t ahead3[3] = {px + m.vel_x, py + m.vel_y, pz};
            const GroundClearance clearance{};
            const int32_t ground =
                    calc_average_ground_height(*world.tables.terrain, ahead3, 0, clearance);
            if (ground != INT32_MIN && ground >= world.env.water_z) {
                m.vel_x -= (m.vel_x + 4) >> 3;
                m.vel_y -= (m.vel_y + 4) >> 3;
                m.wheel_rate_bam = io::bam_sub(
                        m.wheel_rate_bam,
                        io::bam_sar(io::bam_add(m.wheel_rate_bam, 2), 2));
                if (vertical_thrust > 0 && ground - world.env.water_z > 30583 &&
                        !veh.ground_target.valid()) {
                    m.vel_x = 0;
                    m.vel_y = 0;
                    m.wheel_rate_bam = 0;
                }
            }
        }
    }

    // ---- 7. Integration -> solve -> yaw, the witnessed order
    // [orig: Position += velocity (X/Y/Z all UNCONDITIONAL)
    // @0x48ECA8..0x48ECC6; the wheel-rate self-decay @0x48ECC9..0x48ECE1;
    // the platform call @0x48ECE7; Yaw += modelPtr0 AFTER it @0x48ECF2]. A
    // boxless row keeps its chase-owned Z (no slide integration).
    px += m.vel_x;
    py += m.vel_y;
    if (solve_active) pz += m.slide_z; // [orig: add [esi+0Ch] @0x48ECC6]
    {
        const int32_t r = m.wheel_rate_bam;
        m.wheel_rate_bam = io::bam_sub(
                io::bam_sub(r, io::bam_sar(io::bam_add(r, 16), 5)),
                io::bam_sar(r, 31)); // decay toward zero
    }

    veh.position.x = static_cast<float>(from_fixed(px));
    veh.position.y = static_cast<float>(from_fixed(py));
    veh.position.z = static_cast<float>(from_fixed(pz));

    // The platform solve runs every tick after integration, exactly where the
    // retail caller sits [orig: call @0x48ECE7 — after Position += velocity,
    // before the yaw apply]. It owns Z + Pitch/Roll + the afloat/airborne
    // flags from here (the chase-staged Z above is its seed, matching the
    // template's airborne-only Z-step gate). Yaw applies only AFTER this call.
    world.vehicles.watercraft_platform_solve(veh, traits);
    m.yaw_bam = io::bam_add(m.yaw_bam, m.wheel_rate_bam);
    veh.yaw = static_cast<int16_t>(std::lround(
            mission_yaw_deg_from_bam_heading(m.yaw_bam)));
	detail::vehicle_sample_trails(world, veh, traits, m.cmd_speed);
	// No rotor machine here: Entity_UpdateWatercraftPhysics @0x48D480..0x48EF74
	// calls neither @0x4928B0 nor @0x48FA70, and its wheel phase is the thrust
	// block's inline add, ported above inside the up.z gate.
}

// Authority watercraft entry: stage occupant, AI or parked commands, advance the shared boat
// motor, then apply the authority capsize drain.
// Witness sites: [orig: @0x48DF7F, @0x48DFA2, @0x48D51F, @0x48D6DA, @0x48DACD, @0x48DE04,
// @0x48DE7B, @0x48DFD3, @0x48DFDF, @0x48ECF5, @0x48D480]
void VehicleSystem::tick_watercraft_motor(Entity &veh, const VehicleTraits &traits, const VehicleDriveCmd *ai_cmd) {
    World &world = world_;
	if (traits.physics == 0) {
		tick_simple_motor(veh, traits, ai_cmd, false);
		return;
	}
	Entity::VehicleMotorState &m = veh.veh;
	if (!m.yaw_seeded) {
		m.yaw_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));
        m.air_pitch_bam = static_cast<int32_t>(veh.pitch) * 11930464;
        m.air_roll_bam = static_cast<int32_t>(veh.roll) * 11930464;
        m.yaw_seeded = true;
	}
	vehicle_refresh_ground_link(world, veh, traits);
	vehicle_follow_carrier(world, veh);
	// The platform solve owns Z/attitude/afloat from the first tick; an authored
	// hull starts with no prior solve frame, so seed the latch exactly like the
	// client path does before its first thrust pass.
	if (!watercraft_has_platform_geometry(traits)) {
		watercraft_refresh_fallback_afloat(world, veh, traits);
	} else if (!m.plat_solve_valid) {
		watercraft_seed_platform_latch(world, veh, traits);
	}

	tick_health(veh, traits);
	update_engine_sound(veh, traits);

	// A DEAD hull skips everything to the matrix-build tail — no input, no
	// integration [orig: `test Flags, 2 -> jnz 0x48EF4B` @0x48DDFA]. Retail has
	// ONE flags word; our death chain latches the dead bit on engine_flags
	// (destruction.cpp), so read the established combined view.
	if (((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0)
		return;

	// Capsize damage, authority-only: past ~100 deg of roll OR pitch the hull
	// drains 200 health per tick to zero [orig: @0x48DE84..0x48DECD —
	// |Roll|/|Pitch| > 0x471C7180, Health -= 200, floor 0 and clear attacker].
	if (world.ai.is_authority && veh.health > 0 &&
			(io::bam_abs(m.air_roll_bam) > 0x471C7180 ||
					io::bam_abs(m.air_pitch_bam) > 0x471C7180)) {
		veh.health -= 200;
		if (veh.health <= 0) {
			veh.health = 0;
			veh.last_attacker = {};
		}
	}

	// Sound-lane classification only. The boat ENTRY split is occupant-NULL or
	// the dead flag — the witness records no health term (a 0-hp hull that never
	// took the kill edge, e.g. the capsize drain floor, keeps driving); the
	// ground family's mode-21 health check is its own witness and stays there.
	const bool wrecked = veh.health <= 0;

	// ------------------------------------------------------------------ input block
    // [orig: the gate @0x48DF7F..0x48DFA2; occupant resolve + class split
    // @0x48DFA8..0x48DFCD — no attrib 0x40 means the whole block is skipped and
    // the core runs on the persisted registers]
    if (traits.player_control) {
        Entity *occ = world.vehicles.resolve_controller(veh);
        if (occ != nullptr && (!occ->alive || occ->health <= 0)) occ = nullptr;
        const bool player_occupant =
                occ != nullptr && occ->handle.pool() == 0 && occ->player_class != 0;
        if (occ == nullptr) {
            // Parked/no controller: hold heading, zero command and ramp, lights
            // off. The state-22 stamp and the stuck escalation live with the
            // brain in watercraft_ai_drive. [orig: @0x48E7EE..0x48E81E]
            m.steer_target_bam = m.yaw_bam;
            m.cmd_speed = 0;
            m.steer_ramp_bam = 0;
            veh.flags &= ~0x80u;
        } else if (player_occupant && !watercraft_driver_submerged(world, *occ)) {
            // The human-driver leg: 8-way keys + analog through waterSpeed, the
            // boat bit6/bit7 overrides, the 45-deg ramp cap, walk/creep halving,
            // lights [orig: @0x48DFE5..0x48E20F]. A player whose head is under
            // the water plane routes to the AI leg instead [orig: the
            // submerged-driver cut @0x48DFD3..0x48DFDF].
            m.stuck_ticks = 0; // [orig: `moveTimer = 0` at the entry split @0x48DFA8..0x48DFCD]
			stage_player_vehicle_input(world_, veh, *occ, traits);
		} else if (ai_cmd != nullptr && ai_cmd->ai_drive) {
			// The AI-driver leg's outputs (AiSystem::watercraft_ai_drive)
            // [orig: @0x48E247..0x48E756 writes aiComp[132]/[136]].
            m.stuck_ticks = 0;
            m.steer_target_bam = ai_cmd->steer_target_bam;
            m.cmd_speed = ai_cmd->cmd_speed;
            m.steer_ramp_bam = 0;
		}
		// A live non-player controller with no drive command holds the previous
        // targets.
    }

    watercraft_motor_core(world, veh, traits);

    // Movement-sound presentation, same per-tick site as the ground core's tail
    // [orig: the cbot movement-sound call in step 18 @0x48ED76..].
    world.vehicles.update_ground_sound(veh, traits, wrecked, /*collided=*/false);
    // The part-animation tick is the core's (@0x48E9F0..0x48E9F9 runs once
    // per mover pass); a second call here would double the wheel phase.
}

// Ground-family prediction runs the shared chase, stages local-driver input or remote commands,
// and advances the same motor/contact state with authority health writes disabled.
void VehicleSystem::ground_client_tick(Entity &veh, const VehicleTraits &traits) {
	// The same class-table split as the authority tick_motor: the ctank and
	// cbike rows ignore the selector [orig: Entity_DispatchPhysics_ctank
	// @0x48F000..0x48F007, Entity_DispatchPhysics_cbike @0x48EFF0..0x48EFF7].
	if (traits.physics == 0 && traits.family != VehicleFamily::Tank &&
			traits.family != VehicleFamily::Bike) {
		tick_simple_motor(veh, traits, nullptr, true);
		return;
	}
	// [orig: Entity_DispatchPhysicsUpdate @0x48F010]
	if (traits.amphibian && (veh.flags & 0x8000u) != 0) {
		VehicleTraits afloat = traits;
		afloat.family = VehicleFamily::Watercraft;
		watercraft_client_tick(veh, afloat);
		update_ground_sound(veh, afloat, veh.health <= 0, false);
		return;
	}
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	if (!m.net_predicted) return;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(veh.yaw);
        m.yaw_seeded = true;
    }
	// The bike's chase reads the up.z of the mover-entry matrix, built before
	// the carrier follow [orig: Entity_UpdateLightVehiclePhysics @0x484039].
	const int32_t entry_up_z16 =
			vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam).q22.m[10] >> 6;
	vehicle_refresh_ground_link(world, veh, traits);
	vehicle_follow_carrier(world, veh);
	vehicle_client_chase(veh,
			traits.family == VehicleFamily::Tank     ? VehicleChaseFamily::Tank
					: traits.family == VehicleFamily::Bike ? VehicleChaseFamily::Bike
														   : VehicleChaseFamily::Ground,
			entry_up_z16);
	// Register mirror / local-driver gate (see the watercraft mirror note).
	if (Entity *local_driver =
                resolve_local_vehicle_controller(world, veh, traits)) {
		stage_player_vehicle_input(world_, veh, *local_driver, traits);
		// The controlling client halves its staged command against the
		// received register every tick — the same longitudinal-only
		// reconciliation as the boat; steer stays owned by current local
		// LOOK/input [orig: ground @0x48bbef; bike @0x484dad —
		// [136] = ([136] + [177]) >> 1].
		m.cmd_speed = io::bam_sar(
                io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
    } else {
        m.cmd_speed = m.net_recv_speed;
        m.steer_target_bam = m.net_recv_steer_bam;
    }
    // Only the input block is role-gated; the core runs with the real row so
    // the PlayerControl tail (claimant sound gates, engine start/stop, part
    // spin) runs for every hull the client predicts.
    // [orig: ctan input gate @0x489522..0x489545 vs the ungated tail gates
    //  @0x48AAC4..0x48AADA and @0x48AD94..0x48AE3D; cveh @0x48B949..0x48B96C vs
    //  @0x48D38B..0x48D42B; cbik @0x486941..0x4869EA]
    world.vehicles.tick_motor(veh, traits, nullptr, /*prediction=*/true);
}

} // namespace opennova::world
