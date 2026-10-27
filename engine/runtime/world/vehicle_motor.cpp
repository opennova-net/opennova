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
#include <runtime/world/collision_detail.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/geom.h>
#include <runtime/world/local_player.h>
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
		// occupant/AI-driver block; the authority pass runs it in
		// VehicleSystem::update_motor ahead of the mover, so this twin only lands
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
	// [orig: BoneCallback_cveh_PublishControls @0x4929B0;
	//  MOVZX/store @0x4929C0..0x4929D7]
	out.steering = static_cast<int32_t>(static_cast<uint32_t>(state.steer_state) >> 16);

	// Reproduce CDQ/XOR/SUB as unsigned two's-complement arithmetic before the
    // unsigned 0x10000 cap. In particular INT_MIN becomes 0x80000000 (it does
    // not invoke C++ signed-abs UB) and therefore publishes 0x10000.
    // [orig: BoneCallback_cveh_PublishControls @0x4929DC..0x4929F1]
    const uint32_t speed_bits = static_cast<uint32_t>(state.speed);
    const uint32_t sign_mask = 0u - (speed_bits >> 31);
    const uint32_t magnitude =
            (speed_bits ^ sign_mask) - sign_mask;
    out.speed = static_cast<int32_t>(
            std::min(magnitude, uint32_t{0x10000}));

    // The part-animation words [orig: BoneCallback_cveh_PublishControls @0x4929B0 —
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
		// [orig: BoneCallback_tank_PublishControls @0x449C10, @0x449D25..0x449D33 /
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
    // validate the +368 mirror each tick alongside the seat sweep. The mover only
    // nulls the claim here; its tail's leave edge sounds the hull stop this tick. A
    // surviving second control occupant does not inherit the claim (it re-arms only on
    // a fresh attach). [orig: Entity_UpdateTankVehiclePhysics @0x489484..0x4894BE;
    // cveh @0x48b8a1-0x48b944]
    if (veh.primary_occupant.valid()) {
        Entity *po = world.registry.get(veh.primary_occupant);
        if (po == nullptr || !po->mounted || po->mount_target != veh.handle) {
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
			body != nullptr ? body->heading : pilot.heading, delta);
	pilot.heading = yaw;
	if (body != nullptr) {
		body->heading = yaw;
		body->inf.target_heading = io::bam_add(body->inf.target_heading, delta);
	}
	return yaw;
}

// The movers OR the free-look bit into the occupant's MoveOrder word itself,
// which only the input pack rewrites. A remote occupant's word is its wire
// byte, rewritten by its next uplink. The local occupant's is LocalPlayer::
// move_order, re-derived onto the wire byte every pre-tick, so the bit rides
// it until that player's next pack: every frame on an authority, once per
// send boundary on a joiner.
// [orig: `or [ecx+12Ch], 10h` @0x48B889 / @0x48B897 (ground), @0x490D4D /
//  @0x490D6D (aircraft); Player_PackInputStateToEntity's full store @0x4DF68F
//  inside the holdoff-gated send block (Client_ProcessNetworkFrame @0x42C3DD)]
void merge_occupant_free_look(World &world, Entity &occ) {
	occ.net_move_input |= static_cast<uint8_t>(Entity::kMoveOrderFreeLook);
	if (occ.handle == world.cached.local_player && world.local_player_state != nullptr)
		world.local_player_state->move_order.free_look = true;
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
        merge_occupant_free_look(world, occ);
    }

	// Read the full-precision LOOK word shared by the body and aircraft mover;
	// the degree mirror is a fallback for unbound bodies.
	// [orig: Entity_UpdateVehiclePhysics @0x48BB0B; D-VEH-3]
	const AiEntity *driver_body = world.ai.for_handle(occ.handle);
	int32_t driver_yaw_bam = driver_body != nullptr ? driver_body->heading
			: occ.heading;

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
                io::bam_mul(cmd_base, static_cast<int32_t>(occ.net_analog_x))), 7);
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
// `(occ->Flags & 0x100) && (occ == g_LocalPlayerEntity || is_authority)` — the
// AUTHORITY runs the input block for ANY player occupant, not only its own local
// player. That distinction is invisible on a listen host flying its own
// aircraft, and decisive when a JOINER is the pilot: the host owns the mover, so
// insisting on the host's local player left a remote pilot commanding nothing.
// The block tests the hull, never the occupant's life: an empty seat or the hull's
// Flags 0x10000002 (the dead bit, a script's disable) parks it, and a body that
// dies seated has left its seat at its own death edge before the hull's update
// (the pool-0 walk precedes pool 1). [orig: Entity_UpdateAircraftPhysics
// @0x490310 (the input gate @0x490F00..0x490F0E, the park @0x490F16..0x490F25);
// the ground twin Entity_UpdateVehiclePhysics @0x48AF00 (@0x48B958..0x48B987); the
// death edge's detach Entity_UpdateInfantryAI @0x4B9910 (@0x4B9C57..0x4B9C60)]
Entity *resolve_piloting_player(World &world, Entity &veh, const VehicleTraits &traits) {
    if (!traits.player_control) return nullptr;
    if (vehicle_hull_parked(veh)) return nullptr;
    Entity *occ = world.vehicles.resolve_controller(veh);
    if (occ == nullptr || occ->handle.pool() != 0 || occ->player_class == 0)
        return nullptr;
    const bool is_authority = world.ai.is_authority;
    if (!is_authority && occ->handle != world.cached.local_player) return nullptr;
    return occ;
}

// The predicting client's own driver: the input block runs only for the local
// player's seat, whatever its life (above). [orig: cveh @0x48B958..0x48B96C; cbot
// @0x48DF7F..0x48DFA2]
Entity *resolve_local_vehicle_controller(World &world, Entity &veh,
                                                const VehicleTraits &traits) {
    if (!traits.player_control || !world.cached.local_player.valid()) return nullptr;
    Entity *occ = world.vehicles.resolve_controller(veh);
    if (occ == nullptr || occ->handle != world.cached.local_player ||
        occ->handle.pool() != 0 || occ->player_class == 0)
        return nullptr;
    return occ;
}

// The parked arm the input block takes for an empty seat or a parked hull: steer
// holds the heading, the command and the ramp zero, the lights bit clears.
// [orig: cveh @0x48C002..0x48C02D; cbot @0x48E7EE..0x48E81E]
void vehicle_park_inputs(Entity &veh) {
    Entity::VehicleMotorState &m = veh.veh;
    m.steer_target_bam = m.yaw_bam;
    m.cmd_speed = 0;
    m.steer_ramp_bam = 0;
    veh.flags &= ~0x80u;
}

// The ground/tracked contact + suspension solve — the client-executed subset
// of Entity_ProcessTrackedVehiclePhysics [orig: @0x47C1C0] — is declared in
// vehicle_motor_detail.h and defined in vehicle_contact_solve.cpp.

void VehicleSystem::tick_motor(Entity &veh, const VehicleTraits &traits, bool prediction) {
	// [orig: Entity_DispatchPhysicsUpdate @0x48F010]
	if (traits.amphibian && (veh.flags & 0x8000u) != 0) {
		VehicleTraits afloat = traits;
		afloat.family = VehicleFamily::Watercraft;
		tick_watercraft_motor(veh, afloat);
		return;
	}
	World &world = world_;
	// Selector zero dispatches the cveh row to the simple motor. The ctank and
	// cbike rows call their movers without testing the selector.
	// [orig: Entity_DispatchPhysics_cveh @0x48EFC0 (@0x48EFC7); Entity_DispatchPhysics_ctank
	//  @0x48F000..0x48F007; Entity_DispatchPhysics_cbike @0x48EFF0..0x48EFF7]
	if (traits.physics == 0 && traits.family != VehicleFamily::Tank &&
			traits.family != VehicleFamily::Bike) {
		tick_simple_motor(veh, traits, false);
		return;
	}

	Entity::VehicleMotorState &m = veh.veh;
	if (!m.yaw_seeded) {
		// An unmoved row's Yaw/Pitch/Roll are its placement angles in the spawn
		// form, which the mover's entry copy reads as they stand.
		// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6;
		//  Entity_UpdateVehiclePhysics @0x48AF7A..0x48AF8C]
		m.yaw_bam = veh.heading;
        // Seed the live BAM attitude mirror from the row's authored pose: one
        // entity Pitch/Roll in the original — the same storage the contact
        // solve conforms every grounded tick [orig: entity->Pitch/Roll writes
        // @0x47EC83/@0x47EC75]. (A joiner row arms through ground_client_tick,
        // whose staging already seeded these from the wire.)
        m.air_pitch_bam = veh.pitch_bam;
        m.air_roll_bam = veh.roll_bam;
        m.yaw_seeded = true;
	}
	// The mover prologue copies the entry pose to savedLivePose (+0x80..+0x94)
	// ahead of every bail, on every role and on Game_StartMission's direct
	// call. A predicting client stamps at its own entry, before the chase.
	// [orig: ctan @0x488B24..0x488B50; cveh @0x48AF74..0x48AFA0; cbik
	//  @0x484054..0x484080; Game_StartMission `call ecx` @0x526010]
	if (!prediction)
		stamp_saved_live_pose(veh);

	// A hull with no brain (+0x64) jumps from the stamp to the matrix-build tail
	// on every role: no ground probe, carrier ride, health cadence, input, motion
	// or sound. Only the class init brains a row (an AIData def whose ai_function
	// row is a brain class), on the authority and the client alike.
	// [orig: cveh `mov edi, [esi+64h]` @0x48AF1A, `jz` @0x48AFA6 -> the tail
	//  @0x48D43E..0x48D451; ctan @0x488ACA, @0x488B56 -> @0x48AEB6; cbik @0x483FFA,
	//  @0x484086 -> @0x486A04]
	if (world.ai.for_handle(veh.handle) == nullptr) {
		veh.engine_flags |= kEntityFlagMatrixBuilt;
		return;
	}

	if (!m.net_predicted) {
		vehicle_refresh_ground_link(world, veh, traits);
		vehicle_follow_carrier(world, veh);
	}

	// The per-family contact solve owns Z, attitude, the contact byte and the
	// airborne/in-water flags. Each runs its sleep leg and its wake expiry, then
	// returns at once for a hull whose graphic model (+0x30) is null, writing
	// none of those words: a model-less hull keeps what its placement (or a
	// last solve) left and moves by its own velocity words, gravity included.
	// A model whose collision block holds no box extents would fault the solve,
	// which reads the block unchecked (no stock model lacks one); ours keeps the
	// terrain-clamp stand-in below for it (a lib embedder's rows, a unit rig).
	// The family routing is the physics table's [orig: @0x82abc8]:
	// cveh/ctrn/catv -> the tracked solve @0x47C1C0; ctank -> the wheeled solve
	// @0x475DE0 (call @0x48a9ef); cbike -> the light solve @0x479600 (call
	// @0x486672).
	enum class ContactSolveKind : uint8_t { None, Tracked, Wheeled, Light };
	ContactSolveKind solve_kind = ContactSolveKind::None;
	{
		if (traits.family == VehicleFamily::Ground &&
				(!veh.has_graphic_model || ground_contact_solve_active(traits)))
			solve_kind = ContactSolveKind::Tracked;
		else if (traits.family == VehicleFamily::Tank &&
				(!veh.has_graphic_model || ground_contact_solve_active(traits)))
            solve_kind = ContactSolveKind::Wheeled;
        else if (traits.family == VehicleFamily::Bike &&
				(!veh.has_graphic_model || light_contact_solve_active(traits)))
            solve_kind = ContactSolveKind::Light;
	}
	const bool wheeled_solve = solve_kind != ContactSolveKind::None;

	// Zero health requests wreck state 21. The drive block also rejects Flags bit 1; wreck motion
	// runs through the installed state callback.
	// Witness sites: [orig: @0x48af61]
	tick_health(veh, traits);
	const bool wrecked = veh.health <= 0;

	// A DEAD hull (one that keeps this live mover: no husk, or a husk whose model
	// did not load) skips the input block, the physics and the sound fold to the
	// matrix-build tail, as the boat twin does; the dead bit reads both flag halves
	// (one Flags dword in retail). [orig: cveh `test byte ptr [esi+24h], 2; jnz`
	// @0x48B810..0x48B814 -> the tail @0x48D43C..0x48D451; ctan @0x4893E9 -> @0x48AEB4;
	// cbik @0x484936 -> @0x486A02]
	if (((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0) {
		veh.engine_flags |= kEntityFlagMatrixBuilt;
		return;
	}

	// ------------------------------------------------------------------ input block
	// [orig: Entity_UpdateVehiclePhysics @0x48af00, the `attrib & 0x40` occupant block
	// @0x48b949-0x48c034; ctan @0x489522..0x489545.] Only this block is role-gated:
	// the authority runs it, while a predicting client (`prediction`) has already
	// swept the seats and staged its local driver or the received registers.
	// Everything after it, including the PlayerControl tail, runs on both roles.
	Entity *occ = traits.player_control && !prediction ? world.vehicles.resolve_controller(veh)
													   : nullptr;
    // The block reads the hull, never the occupant's life: a body that dies seated
    // has left the seat at its own death edge, which the pool-0 walk runs ahead of
    // this pool-1 update. [orig: Entity_UpdateInfantryAI @0x4B9910 (@0x4B9C57..0x4B9C60)]
    // "Occupant is a player" [orig: `occupant->Flags & 0x100`] — our runtime players
    // are pool-0 organics with a resolved soldier class.
    const bool player_occupant =
            occ != nullptr && occ->handle.pool() == 0 && occ->player_class != 0;

    if (traits.player_control && !prediction) {
        // Zero health alone does not park: the driver's leg runs until the dying
        // state's death transforms raise the dead bit. Every arm runs here, after the
        // mover's head (the health cadence, the ground probe, the carrier ride), the
        // parked arm's stuck count and the AI leg's crew cap, waypoint refresh and
        // avoid walk included.
        // [orig: cveh @0x48B972..0x48B987; ctan @0x48954B..0x489560;
        //  cbik @0x484A98..0x484AAD]
        AiEntity *brain = world.ai.for_handle(veh.handle);
        VehicleDriveCmd cmd;
        if (occ == nullptr || vehicle_hull_parked(veh)) {
			// The parked arm: steer holds the heading, the command and the ramp zero, the
			// stuck escalation counts, the lights bit clears and the brain takes state 22
			// (vehicle_ai_drive's parked arm).
			// [orig: cveh @0x48C002..0x48C02D; ctan @0x489BCE..0x489BF9; cbik
			//  @0x4851AE..0x4851D9]
			vehicle_park_inputs(veh);
			world.ai.vehicle_ai_drive(world, veh, occ, traits, cmd);
        } else if (player_occupant && !watercraft_driver_submerged(world, *occ)) {
			// A player driver (the occupant's Flags 0x100, cveh @0x48B993) whose eye
			// (CameraOffset.z + Z) sits at or below the water plane drops to the AI leg
			// like the boat twin [orig: `mov eax,[ecx+74h]; add eax,[ecx+0Ch]; cmp eax,
			// g_EnvWaterHeightFixed; jle loc_48BC12` @0x48B9A0..0x48B9AC; ctan
			// @0x489579..0x489585 -> @0x4897DB; cbik @0x484AC6..0x484AD2 -> @0x484DB8].
			// The player arm parks the brain at state 22, so the route never advances
			// under a human driver [orig: cveh @0x48B9CB; ctan @0x4895A6; cbik @0x484AED].
			if (brain != nullptr) brain->brain.f[AiBrain::kCurState] = 22;
			stage_player_vehicle_input(world_, veh, *occ, traits);
		} else {
			// The AI leg writes the command registers in place; it writes the ramp only
			// in its boarders' hold.
            // [orig: the AI-driver leg cveh @0x48BC12..0x48C000; ctan @0x4897DB..0x489BCC;
            //  cbik @0x484DB8..0x4851AC]
            world.ai.vehicle_ai_drive(world, veh, occ, traits, cmd);
            if (cmd.ai_drive) {
                m.steer_target_bam = cmd.steer_target_bam;
                m.cmd_speed = cmd.cmd_speed;
            }
		}
        // Occupied and not parked: the driverless stuck count rests; a parked
        // hull (Flags 0x10000002) keeps counting under its occupant [orig:
        // `moveTimer = 0` after the parked test — cveh @0x48B98D, ctan @0x489566,
        // cbik @0x484AB3; the boat twin @0x48DFA8..0x48DFCD].
        if (occ != nullptr && !vehicle_hull_parked(veh)) m.stuck_ticks = 0;
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
		// contact solve conforms, the placement's spawn form until a solve writes
		// it (air_pitch_bam, seeded at the first tick above); the boxless stand-in
		// rows keep the row pitch, their attitude never advancing past the
		// authored pose [orig: Entity_SpawnFromBMSRecord @0x40EB69..0x40EB86].
		const int32_t pitch_bam = wheeled_solve
				? m.air_pitch_bam
				: veh.pitch_bam;
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
			if (static_cast<int32_t>(std::min(length, detail::kFtolClamp)) > 0) {
				const int32_t scale = static_cast<int32_t>(
						double(m.chassis_impulse_amplitude) * double(28.16f)); // flt_7C6FA0
				m.vel_x = io::bam_add(m.vel_x, io::mul_q16_rhu(dir[0], scale));
				m.vel_y = io::bam_add(m.vel_y, io::mul_q16_rhu(dir[1], scale));
				m.slide_z = io::bam_add(m.slide_z, io::mul_q16_rhu(dir[2], scale));
			}
			if (!m.chassis_contact_active)
				std::fill_n(m.chassis_impulse_direction, 3, 0);
		}

		const int32_t prev[3] = { veh.position.x.q16, veh.position.y.q16,
			veh.position.z.q16 };
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
		// box_z_lo + r) and conforms attitude from per-corner lifts; a model with
		// no box extents keeps the 5-tap bilinear terrain clamp (a model-less
		// hull's solve returns after its prologue: no stand-in).
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
            pz = veh.position.z.q16;
		}

		// Grounded steering applies the wheel yaw rate [orig: @0x48ef60
		// `Yaw += modelPtr0`, gated on ground contact]. The cbik mover ALWAYS
		// applies it, quartered while the airborne/swimming flag is up
		// [orig: @0x486681..0x486697 `Yaw += modelPtr0 >> 2` under Flags 0x2000].
		if (traits.family == VehicleFamily::Bike) {
            // The quarter keys on the light solve's Flags 0x2000, not on the
            // contact byte: that byte stays clear for the landing ticks before
            // the rear-contact run passes 1, while Flags is already down. Only
            // the boxless terrain-clamp stand-in, which keeps no flags, reads
            // m.grounded. [orig: `test [esi+24h],2000h` @0x48667A..0x486697]
            const bool in_air = solve_kind == ContactSolveKind::Light
                    ? (veh.flags & kEntityFlagInAir) != 0
                    : !m.grounded;
            const int32_t yaw_step = in_air
                    ? io::bam_sar(m.wheel_rate_bam, 2)
                    : m.wheel_rate_bam;
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

		veh.position.x.q16 = px;
		veh.position.y.q16 = py;
		veh.position.z.q16 = pz;
        veh.heading = m.yaw_bam;
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
	// The tank's tread cue follows the PlayerControl block.
	// [orig: Entity_UpdateTankVehiclePhysics @0x48AE45..0x48AEA1]
	if (traits.family == VehicleFamily::Tank)
		world.vehicles.update_tread_sound(veh, traits);
    if (traits.family == VehicleFamily::Bike)
        m.wheelie_request = 0; // [orig: Entity_UpdateLightVehiclePhysics @ 0x4869F9]
	// Every mover ends by rebuilding the entity matrix and setting the matrix bit
	// (Flags 0x20000, homed on engine_flags); the dead-hull bail jumps to this
	// same tail. The load stream carries it on
	// every live vehicle. [orig: cveh Entity_UpdateVehiclePhysics @0x48D449,
	// OR @0x48D451 (dead bail @0x48B810); ctan Entity_UpdateTankVehiclePhysics
	// @0x48AEC9 (bail @0x4893E9); cbik Entity_UpdateLightVehiclePhysics
	// @0x486A17 (bail @0x484936)]
	veh.engine_flags |= kEntityFlagMatrixBuilt;
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
    const int32_t origin[3] = {};
    return vehicle_basis_from_matrix(collision_matrix_from_euler(
            yaw_bam, pitch_bam, roll_bam, origin));
}

VehicleEulerBasis vehicle_basis_from_matrix(const CollisionMatrix &q22) {
    VehicleEulerBasis out;
    out.q22 = q22;
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

CollisionMatrix vehicle_solve_matrix(const Entity &veh) {
	const auto &m = veh.veh;
	const int32_t origin[3] = {};
	return collision_matrix_from_euler_scaled(
			m.yaw_bam, m.air_pitch_bam, m.air_roll_bam, origin, veh.uniform_scale_q16);
}

CollisionMatrix vehicle_occupant_matrix(const CollisionMatrix &head) {
	CollisionMatrix out = head;
	for (int row = 0; row < 3; ++row) {
		const int64_t extract[3] = { head.m[row] >> 6, head.m[4 + row] >> 6, head.m[8 + row] >> 6 };
		int32_t unit[3];
		fixed_normalize(extract, unit);
		for (int k = 0; k < 3; ++k)
			out.m[4 * k + row] = bam_shl_wrap(unit[k], 6);
	}
	return out;
}

CollisionMatrix vehicle_matrix_rows(const CollisionMatrix &head) {
	CollisionMatrix out = head;
	for (int row = 0; row < 3; ++row)
		for (int k = 0; k < 3; ++k)
			out.m[4 * k + row] = bam_shl_wrap(head.m[4 * k + row] >> 6, 6);
	return out;
}

} // namespace detail

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
	int32_t px = veh.position.x.q16;
	int32_t py = veh.position.y.q16;
    int32_t pz = veh.position.z.q16;
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

    veh.position.x.q16 = px;
    veh.position.y.q16 = py;
    veh.position.z.q16 = pz;
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
		const int64_t ax = gx ? io::bam_mul(int32_t(pen), int32_t(gz)) / int32_t(gx) : 0;
        const int64_t ay = gy ? io::bam_mul(int32_t(pen), int32_t(gz)) / int32_t(gy) : 0;
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

// fixed_normalize / fixed_cross_wide live beside FixedVec3 (geom.h; the product is io::mul_q16_rhu),
// shared with the wheeled/light solves; the witness notes ride with them.

namespace detail {

// Corner k is the position plus two products, each rounded half up on its own, of a half extent
// (ftol of the extent times -0.5f or 0.5f, truncated toward zero) and a row's extract word: row 1
// (side) with the beam's half, row 0 (forward) with the length's. c0 (+f, -s), c1 (+f, +s),
// c2 (-f, -s), c3 (-f, +s).
// [orig: Entity_ProcessPlatformPhysics @0x481870 - the halves flt_7C59B0 (-0.5f) @0x482E3D and
//  flt_7C3B94 (0.5f) @0x482EF6; c0 @0x482E57..0x482FE0, c1 @0x482FF7..0x48316B, c2
//  @0x483176..0x4832F1, c3 @0x4832FC..0x48346D (`add eax, 8000h; adc edx, 0; shrd eax, edx, 10h`
//  per product)]
void vehicle_platform_corners(const CollisionMatrix &raw, int32_t beam, int32_t length,
		int32_t px, int32_t py, int32_t pz, int32_t out[4][3]) {
	const int32_t forward_half[4] = { length / 2, length / 2, -length / 2, -length / 2 };
	const int32_t side_half[4] = { -beam / 2, beam / 2, -beam / 2, beam / 2 };
	const int32_t position[3] = { px, py, pz };
	for (int k = 0; k < 4; ++k)
		for (int i = 0; i < 3; ++i)
			out[k][i] = io::bam_add(position[i],
					io::bam_add(io::mul_q16_rhu(side_half[k], raw.m[4 * i + 1] >> 6),
							io::mul_q16_rhu(forward_half[k], raw.m[4 * i] >> 6)));
}

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
		fixed_normalize(d, u);
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
    fixed_cross_wide(a, b, raw);   fixed_normalize(raw, n1);
    fixed_cross_wide(a, b, raw);   fixed_normalize(raw, n2); // set 2 recomputes set 1
    fixed_cross_wide(e3, b, raw);  fixed_normalize(raw, n3);
    fixed_cross_wide(e3, f4, raw); fixed_normalize(raw, n4);
    // Set 1's edge is normalized a second time after its normal, and the forward sum reads that
    // copy beside set 2's once-normalized one [orig: Entity_ProcessWheeledVehicleSuspension
    // @0x46B140 - the second normalize @0x46BB96..0x46BBE6, the sum's read @0x46C29D;
    // Entity_ComputeSuspensionOrientation @0x46C8E0 @0x46CB4C..0x46CBA9 / @0x46DAFF;
    // Entity_ComputeSuspensionAndOrientation @0x4698A0 @0x46A453..0x46A469 / @0x46AB10;
    // Vehicle_ComputeAveragedOrientation @0x459A50 @0x459DB9..0x459E05 / @0x45A42B].
    int32_t a2[3];
    {
        const int64_t once[3] = { a[0], a[1], a[2] };
        fixed_normalize(once, a2);
    }
    int64_t up_s[3], fwd_s[3], side_s[3];
    for (int i = 0; i < 3; ++i) {
        up_s[i] = int64_t(n1[i]) + n2[i] + n3[i] + n4[i];
        fwd_s[i] = 2 * int64_t(e3[i]) + int64_t(a2[i]) + int64_t(a[i]); // e4+e3+a2+e2
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
    fixed_normalize(up_s, up);
    fixed_normalize(fwd_s, fwd);
    fixed_normalize(side_s, side);
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

// Live full-precision pose shared by all carrier-delta readers.
// [orig: Entity_UpdateWatercraftPhysics @0x48D480, carrier @0x48D6DA..0x48DACD;
// Entity_UpdateAircraftPhysics @0x490310, carrier @0x4905BC..0x49095B]
void carrier_pose_fixed(const Entity &e, int32_t pos[3], int32_t &yaw,
                        int32_t &pitch, int32_t &roll) {
    pos[0] = e.position.x.q16;
    pos[1] = e.position.y.q16;
    pos[2] = e.position.z.q16;
	if (e.veh.yaw_seeded) {
		yaw = e.veh.yaw_bam;
		pitch = e.veh.air_pitch_bam;
		roll = e.veh.air_roll_bam;
	} else {
		// An unmoved row: its placement angles in the spawn form.
		// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6]
		yaw = e.heading;
		pitch = e.pitch_bam;
		roll = e.roll_bam;
	}
}

void stamp_saved_live_pose(Entity &e) {
    carrier_pose_fixed(e, e.saved_live_pos, e.saved_live_yaw,
                       e.saved_live_pitch, e.saved_live_roll);
    e.saved_live_valid = true;
}

// Ground-family prediction runs the shared chase, stages local-driver input or remote commands,
// and advances the same motor/contact state with authority health writes disabled.
void VehicleSystem::ground_client_tick(Entity &veh, const VehicleTraits &traits) {
	// The same class-table split as the authority tick_motor: the ctank and
	// cbike rows ignore the selector [orig: Entity_DispatchPhysics_ctank
	// @0x48F000..0x48F007, Entity_DispatchPhysics_cbike @0x48EFF0..0x48EFF7].
	if (traits.physics == 0 && traits.family != VehicleFamily::Tank &&
			traits.family != VehicleFamily::Bike) {
		tick_simple_motor(veh, traits, true);
		return;
	}
	// [orig: Entity_DispatchPhysicsUpdate @0x48F010]
	if (traits.amphibian && (veh.flags & 0x8000u) != 0) {
		VehicleTraits afloat = traits;
		afloat.family = VehicleFamily::Watercraft;
		watercraft_client_tick(veh, afloat);
		// The boat mover's fold call (@0x48EEB4) lies inside its brainless bail
		// (@0x48D4EF -> @0x48EF50).
		if (world_.ai.for_handle(veh.handle) != nullptr)
			update_ground_sound(veh, afloat, veh.health <= 0, false);
		return;
	}
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	if (!m.net_predicted) return;
    if (!m.yaw_seeded) {
        // [orig: the movers' entry copy reads the row's Yaw, unmoved = the spawn
        //  form, Entity_UpdateVehiclePhysics @0x48AF7A; Entity_SpawnFromBMSRecord
        //  @0x40EB42..0x40EB66]
        m.yaw_bam = veh.heading;
        m.yaw_seeded = true;
    }
	// The prologue's savedLivePose stamp precedes the chase.
	// [orig: ctan @0x488B24..0x488B50, chase @0x48912B; cveh
	//  @0x48AF74..0x48AFA0; cbik @0x484054..0x484080]
	stamp_saved_live_pose(veh);
	// The brainless bail precedes the chase: a client row the class init never
	// brained keeps its wire pose. [orig: ctan @0x488B56 -> @0x48AEB6, chase
	//  @0x48912B; cveh @0x48AFA6 -> @0x48D43E; cbik @0x484086 -> @0x486A04]
	if (world.ai.for_handle(veh.handle) == nullptr) {
		veh.engine_flags |= kEntityFlagMatrixBuilt;
		return;
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
		// The driving client parks a parked hull as the authority does
		// [orig: cveh @0x48B980..0x48B987 -> @0x48C002].
		if (vehicle_hull_parked(veh)) {
			vehicle_park_inputs(veh);
		} else {
			stage_player_vehicle_input(world_, veh, *local_driver, traits);
			// The controlling client halves its staged command against the
			// received register every tick — the same longitudinal-only
			// reconciliation as the boat; steer stays owned by current local
			// LOOK/input [orig: ground @0x48bbef; bike @0x484dad —
			// [136] = ([136] + [177]) >> 1].
			m.cmd_speed = io::bam_sar(
                    io::bam_add(m.cmd_speed, m.net_recv_speed), 1);
		}
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
    world.vehicles.tick_motor(veh, traits, /*prediction=*/true);
}

} // namespace opennova::world
