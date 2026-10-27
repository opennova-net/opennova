#include "vehicle_motor_detail.h"
#include <runtime/world/vehicle_system.h>
// The ground-vehicle suspension spring leg: crash tests, sink growth, the
// spring-energy loop, the crash latch and the tick tail.
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 — the extend loop
//  @0x47db70..0x47dbd1, the crash tests @0x47d745..0x47d7a8 + @0x47e793..
//  0x47e7ee, the airborne loop @0x47E283..0x47E344, the grounded loop
//  @0x47E960..0x47EC1F, the tail @0x47eeee; Entity_ProcessWheeledVehicleSuspension
//  @0x46B140 — the seed @0x46b1a6..0x46b213; Entity_RespawnVehicle @0x45FF40;
//  Suspension_CompressWheelQuadratic @0x45CFB0; Suspension_OscillateWheelFast
//  @0x45D110]

#include <runtime/world/vehicle_suspension.h>

#include <runtime/world/ground_conform.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

// The bike's retained wheelie byte (+0x3DE) is cleared by the FALLEN-bike arm of
// Entity_UpdateVehicleChassisOrientation @0x468A50 (the `mov [esi+3DEh], bl`
// @0x468BB1 under `[ebp+8] > 250 && [ebp+14h] > 250 || [ebp+38h] > 0`, ported in
// vehicle_contact_solve.cpp's light solve), NOT by the crash seed
// @0x468B28..0x468B3B, which writes only +0x2EC/+0x2EF/+0x2EE and ejects.
namespace opennova::world {

namespace {

// The oscillator kernel clamps the def's shock IN PLACE. Rows the traits
// table knows clamp the shared entry exactly as retail clamps the shared def;
// a row handed loose traits (tests, lib embedders) clamps the loose copy.
int32_t &shock_field(World &world, const Entity &veh, const VehicleTraits &traits,
                     int32_t &loose) {
	if (VehicleTraits *entry = world.vehicles.traits.get_mutable(veh.item_id))
		if (entry->shock == traits.shock) return entry->shock;
	loose = traits.shock;
	return loose;
}

ConformOscillator load_osc(const Entity::VehicleMotorState::WheelOsc &w) {
	ConformOscillator o;
	o.amplitude = w.amplitude;
	o.extension = w.extension;
	o.energy = w.energy;
	o.phase = w.phase;
	return o;
}

void store_osc(Entity::VehicleMotorState::WheelOsc &w, const ConformOscillator &o) {
	w.amplitude = o.amplitude;
	w.extension = o.extension;
	w.energy = o.energy;
	w.phase = o.phase;
}

// The free-fall catch-up [orig: @0x47e9c2..0x47ea2c grounded / @0x47e2ed..
//  0x47e32c airborne]: the corner target drops by the sink beyond one growth
//  step, and a drop past -5000 while falling marks the hard landing.
void catch_up(Entity::VehicleMotorState &m, int k, int32_t growth, bool grounded,
              const bool contact[4], int32_t corner_adj[4], bool tank = false) {
	int32_t c = m.plat_acc[k] - growth;
	if (grounded && tank) {
		// The tank's grounded twin skips the WHOLE block — the pair clear
		// included — while the sink is within one growth step [orig:
		// Entity_ProcessWheeledVehiclePhysics @0x475DE0 (site @0x478DC0..0x478DC9
		// `mov eax,[eax]; sub eax, 0FAh; test eax, eax; jle loc_478E27`; the pair
		// tests it skips @0x478DDF..0x478E0E)].
		if (c <= 0) return;
	} else if (grounded) {
		if (c < 0) c = 0; // [orig: jns / xor @0x47e9dc..0x47e9de]
	} else if (c <= 0) {
		return; // [orig: test/jle @0x47e307..0x47e309]
	}
	c = -c;
	corner_adj[k] += c;
	if (grounded) {
		// A same-side pad pair in contact clears the marker instead [orig: @0x47e9f6..0x47ea12].
		if ((contact[0] && contact[3]) || (contact[1] && contact[2])) {
			m.landing_2ee = 0;
			return;
		}
	}
	if (c < kHardLandingCatchupBelow && m.slide_z < 0) m.landing_2ee = 1;
}

} // namespace

void vehicle_suspension_respawn(Entity::VehicleMotorState &m) {
	// [orig: Entity_RespawnVehicle @0x45FF40 — +0x2F0 @0x45ffeb, +0x2EC
	//  @0x45fff1, +0x2EE @0x45fff7, +0x2F2 @0x45fffd, +0x2FC @0x46000c, +0x2F8
	//  @0x460012, +0x2ED @0x460018, +0x2F1 = 1 @0x46001e]
	m.trails = {};
	m.movement_effects_disabled = false; // +0x44C reset @0x460003
	m.settle_2f0 = 0;
	m.crashed = 0;
	m.landing_2ee = 0;
	m.grounded = false; // +0x2F2 @0x45fffd
	m.wreck_2fc = 0;
	m.airborne_stamp_2f8 = 0;
	m.crash_request = 0;
	m.fresh_2f1 = 1;
}

int32_t vehicle_flip_threshold_q16(const VehicleTraits &traits) {
	// [orig: ftol(def->flip(+0x948) × flt_7C56A8 (0.01) × flt_7C32BC (65536.0))
	//  @0x47d72e..0x47d745: `fild [ecx+948h]` @0x47D727, the two fmuls @0x47D732 /
	//  @0x47D73A, _ftol2_sse @0x47D740]: the products at the FPU's length, so flip 50
	//  gives 32767 where a float product rounds up to 32768 (D-NET-515).
	return static_cast<int32_t>(static_cast<double>(traits.flip) * static_cast<double>(0.01f) *
			static_cast<double>(65536.0f));
}

void VehicleSystem::suspension_crash_tests(Entity &veh, const VehicleTraits &traits, int32_t up_z16, SuspensionFamily family) {
    World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	const bool airborne = (veh.flags & kEntityFlagInAir) != 0;
	const bool bit = (veh.flags & kEntityFlagSuspensionCrashed) != 0;
	if (m.crashed == 0) {
		// (a) |up.z| under the flip bound — the ABSOLUTE value [orig: `cdq;
		//  xor eax, edx; sub eax, edx` @0x47d722..0x47d726 / @0x477748..
		//  0x477753] — or, tracked only, the replicated bit [orig: `test al,
		//  10h` @0x47d749, no twin in the tank's @0x477760..0x477776], while
		// airborne [orig: tracked @0x47d745..0x47d763; tank @0x477760..0x477776].
		const bool tipped = std::abs(up_z16) < vehicle_flip_threshold_q16(traits);
		const bool bit_alt = family == SuspensionFamily::Tracked && bit;
		if ((tipped || bit_alt) && airborne) {
			m.crash_request = 1;
			m.fresh_2f1 = 0;
		}
		// (b) the authority's hard fall vs the client's replicated bit in the
		// air [orig: tracked @0x47d771..0x47d7a8; tank @0x47778d..0x4777bf].
		const bool fall = world.rules.logic_authority
				? std::abs(m.slide_z) > kCrashFallVzAbove
				: (airborne && bit);
		if (fall) {
			m.crash_request = 1;
			m.fresh_2f1 = 0;
		}
	}
}

void VehicleSystem::suspension_client_crash_window(Entity &veh, SuspensionFamily family) {
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	const bool airborne = (veh.flags & kEntityFlagInAir) != 0;
	// (c) the client crash window [orig: tracked @0x47e793..0x47e7ee; tank
	//  @0x478b6c..0x478bd6]: client-only, a fresh-spawned row that is not
	//  crashed (the tank also requires !settle_2f0 @0x478b8a; the tracked
	//  gate @0x47e793..0x47e7a8 reads only +0x2F1 and +0x2EC) stamps the tick
	//  its in-air flag was last seen and requests for the next ten ticks; past
	//  them the stamp clears and the row counts as respawned. The tank runs it
	//  at its grounded entry, after the sink growth, so the in-air flag is the
	//  previous tick's.
	const bool settle_term = family == SuspensionFamily::Tank && m.settle_2f0 != 0;
	if (!world.rules.logic_authority && m.fresh_2f1 == 0 && m.crashed == 0 &&
	    !settle_term) {
		if (airborne && m.airborne_stamp_2f8 == 0) m.airborne_stamp_2f8 = world.logic_tick;
		// Signed tick age [orig: `sub eax, [esi+2F8h]; cmp eax, 0Ah; jge` @0x478BB5..0x478BBE].
		if (int32_t(world.logic_tick - m.airborne_stamp_2f8) <
				int32_t(kClientCrashWindowTicks)) {
			m.crash_request = 1;
		} else {
			m.crash_request = 0;
			m.airborne_stamp_2f8 = 0;
			m.fresh_2f1 = 1;
		}
	}
}

void vehicle_suspension_bike_crash_test(Entity &veh, const int32_t copy[6]) {
	// [orig: Entity_ProcessLightVehiclePhysics @0x47B32D..0x47B375]
	Entity::VehicleMotorState &m = veh.veh;
	if (copy[0] != 0 || copy[1] != 0 || m.wheelie_active == 0)
		return;
	if ((copy[2] != 0 || copy[3] != 0 || copy[5] != 0) && m.crashed == 0)
		m.crash_request = 1;
}

void vehicle_suspension_grow_sinks(Entity &veh, const bool contact[4], int wheels,
                                   int32_t growth, bool latch_gated, bool pre_gate_skip) {
	Entity::VehicleMotorState &m = veh.veh;
	if (pre_gate_skip) return;
	if (latch_gated && (m.crash_request != 0 || m.crashed != 0)) return;
	for (int k = 0; k < wheels; ++k)
		if (!contact[k]) m.plat_acc[k] += growth;
}

void VehicleSystem::suspension_grounded_loop(Entity &veh, const VehicleTraits &traits, int wheels, int32_t depth[4], const bool contact[4], int32_t growth, int32_t corner_adj[4]) {
    World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	if (traits.spring == 0) return; // [orig: @0x47e973 — no suspension def'd]
	// minDepth over the wheel pads [orig: var_26C @0x47e8f0-region, 100000 init].
	int32_t min_depth = 100000;
	for (int k = 0; k < wheels; ++k) min_depth = std::min(min_depth, depth[k]);
	const int32_t travel = conform_travel_from_def(traits.spring_comp);
	const int32_t thr = traits.spring_comp <= 10 ? kImpulseThresholdSoft
	                                             : kImpulseThresholdHard; // [orig: @0x47ea48..0x47ea5b]
	int32_t loose_shock = 0;
	int32_t &shock = shock_field(world, veh, traits, loose_shock);
	for (int k = 0; k < wheels; ++k) {
		// The catch-up runs only for a pad off the ground with no request
		// pending; a crashed row skips the wheel entirely
		// [orig: @0x47e9a1..0x47e9bc].
		if (!contact[k] && m.crash_request == 0) {
			if (m.crashed != 0) continue;
			catch_up(m, k, growth, /*grounded=*/true, contact, corner_adj);
		}
		if (m.crashed != 0) continue; // [orig: @0x47ea33..0x47ea3a]
		ConformOscillator osc = load_osc(m.wheel_osc[k]);
		// The landing IMPULSE [orig: @0x47ea40..0x47eab2]: a sink past the
		// threshold on a pad that just found contact.
		if (m.plat_acc[k] > thr && contact[k]) {
			const int32_t a = std::abs(m.plat_acc[k]);
			const int32_t sq = io::bam_mul(io::bam_mul(traits.mass, a), a);
			osc.energy += conform_half_energy(sq);
			if (osc.energy < 0) osc.energy = 0x1000000; // [orig: @0x47ea97..0x47ea99]
			// `fild [ebx]; fmul flt_7C6F18` (1.25f) at the FPU's length [orig: @0x47EA9F..0x47EAA7].
			m.spring_energy += static_cast<int32_t>(
					static_cast<double>(osc.energy) * static_cast<double>(1.25f));
		} else if (m.spring_energy <= 0) {
			// The settle term [orig: @0x47eab4..0x47eb21]: the pad's excess
			// penetration over the shallowest pad, less WHEEL 0's amplitude
			// (the witnessed unindexed `[esi+0x304]` read), capped at the step.
			int32_t e = depth[k] - min_depth;
			if (e > m.wheel_osc[0].amplitude) {
				e -= m.wheel_osc[0].amplitude;
				if (e > 0) {
					if (e > kSpringStepCap) e = kSpringStepCap;
					const int32_t sq = io::bam_mul(io::bam_mul(2 * traits.spring, e), e);
					osc.energy += conform_half_energy(sq);
				}
			}
		}
		// Compress on stored energy, else free-decay on a live amplitude
		// [orig: @0x47eb23..0x47ebc3].
		int32_t delta = 0;
		if (osc.energy > 0) {
			const int32_t q = (2 * osc.energy) / (2 * traits.spring); // idiv @0x47eb37
			int32_t step = static_cast<int32_t>(std::sqrt(static_cast<double>(q)));
			if (step <= 0) osc.energy = 0; // [orig: @0x47eb48..0x47eb4c]
			if (step > kSpringStepCap) step = kSpringStepCap;
			delta = conform_spring_compress(osc, m.wheel_comp[k], m.spring_energy,
			                                step, travel, traits.spring);
		} else if (osc.amplitude != 0 && m.plat_acc[0] < kOscillateSinkCeiling &&
		           m.plat_acc[1] < kOscillateSinkCeiling &&
		           m.plat_acc[2] < kOscillateSinkCeiling &&
		           m.plat_acc[3] < kOscillateSinkCeiling) {
			delta = conform_spring_oscillate(osc, m.wheel_comp[k], m.spring_energy,
			                                 shock, traits.spring, m.slide_z);
		}
		depth[k] -= delta; // [orig: @0x47ebc3]
		store_osc(m.wheel_osc[k], osc);
	}
}

void VehicleSystem::suspension_airborne_loop(Entity &veh, const VehicleTraits &traits, int wheels, int32_t growth, int32_t corner_adj[4], bool latch_gated) {
    World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	if (latch_gated && m.settle_2f0 != 0) return; // [orig: @0x47e283..0x47e28a]
	const int32_t travel = conform_travel_from_def(traits.spring_comp);
	int32_t loose_shock = 0;
	int32_t &shock = shock_field(world, veh, traits, loose_shock);
	const bool no_contact[4] = {false, false, false, false};
	for (int k = 0; k < wheels; ++k) {
		ConformOscillator osc = load_osc(m.wheel_osc[k]);
		// [orig: tracked @0x47e2c1 / @0x47e2d3; aircraft Suspension_CompressWheelQuadratic
		//  @0x45CFB0 with dword_815180 >> 4 (the call @0x480E48) / Suspension_OscillateWheelFast
		//  @0x45D110 (the call @0x480E5A)]
		if (osc.energy > 0) {
			(void)conform_spring_compress(osc, m.wheel_comp[k], m.spring_energy,
			                              kSpringStepCap, travel, traits.spring);
		} else if (osc.amplitude != 0) {
			(void)conform_spring_oscillate(osc, m.wheel_comp[k], m.spring_energy,
			                               shock, traits.spring, m.slide_z);
		}
		store_osc(m.wheel_osc[k], osc);
		// The aircraft's catch-up has no latch gate [orig: @0x480E62..0x480E90].
		if (!latch_gated || (m.crashed == 0 && m.crash_request == 0))
			catch_up(m, k, growth, /*grounded=*/false, no_contact, corner_adj);
	}
}

void VehicleSystem::suspension_air_grounded_loop(Entity &veh, const VehicleTraits &traits,
		int32_t depth[4], const bool contact[4], int32_t growth, int32_t corner_adj[4]) {
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	// No suspension def'd: the corners keep the raw depths [orig: `cmp [ebp+8FCh], 0; jnz`
	//  @0x4811A5..0x4811AC -> the lift @0x481425..0x481427].
	if (traits.spring == 0) return;
	// minDepth over the four pads, 100000 init [orig: @0x481129..0x481183].
	int32_t min_depth = 100000;
	for (int k = 0; k < 4; ++k) min_depth = std::min(min_depth, depth[k]);
	const int32_t travel = conform_travel_from_def(traits.spring_comp);
	const int32_t thr = traits.spring_comp <= 10 ? kImpulseThresholdSoft
	                                             : kImpulseThresholdHard; // [orig: @0x481268..0x481281]
	int32_t loose_shock = 0;
	int32_t &shock = shock_field(world, veh, traits, loose_shock);
	for (int k = 0; k < 4; ++k) {
		// A pad off the ground drops its corner by its sink beyond one growth step, unless
		// the hull is crashed and still moving; a diagonal pair in contact clears the
		// hard-landing marker, else a drop past -5000 while falling raises it. The
		// entity+0x60 override freeze is never set here.
		// [orig: @0x4811D2..0x481259: `cmp [esi+2ECh], 0; jz` / `cmp [esi+29Ch], 20h; jge`
		//  @0x4811F7..0x481207, the excess test @0x481209, the corner add @0x48121C, the pair
		//  test @0x481223..0x48123D, the marker @0x481248..0x481259]
		if (!contact[k] && (m.crashed == 0 || m.speed < 32) && m.plat_acc[k] - growth > 0) {
			const int32_t c = growth - m.plat_acc[k];
			corner_adj[k] += c;
			if ((contact[0] && contact[3]) || (contact[1] && contact[2]))
				m.landing_2ee = 0;
			else if (c < kHardLandingCatchupBelow && m.slide_z < 0)
				m.landing_2ee = 1;
		}
		ConformOscillator osc = load_osc(m.wheel_osc[k]);
		// The landing IMPULSE [orig: @0x481260..0x4812CB], else the settle term against
		// WHEEL 0's amplitude (the unindexed `[esi+304h]` read @0x4812EA), capped at the
		// step [orig: @0x4812DA..0x481343].
		if (m.plat_acc[k] > thr && contact[k]) {
			const int32_t a = std::abs(m.plat_acc[k]);
			const int32_t sq = io::bam_mul(io::bam_mul(traits.mass, a), a);
			osc.energy += conform_half_energy(sq);
			if (osc.energy < 0) osc.energy = 0x1000000; // [orig: @0x4812B6..0x4812B8]
			// `fild [ebx]; fmul flt_7C6F18` (1.25f) at the FPU's length [orig: @0x4812BE..0x4812CB].
			m.spring_energy += static_cast<int32_t>(
					static_cast<double>(osc.energy) * static_cast<double>(1.25f));
		} else if (m.spring_energy <= 0) {
			int32_t e = depth[k] - min_depth;
			if (e > m.wheel_osc[0].amplitude) {
				e -= m.wheel_osc[0].amplitude;
				if (e > 0) {
					if (e > kSpringStepCap) e = kSpringStepCap;
					const int32_t sq = io::bam_mul(io::bam_mul(2 * traits.spring, e), e);
					osc.energy += conform_half_energy(sq);
				}
			}
		}
		// Compress on stored energy, else free-decay on a live amplitude while every sink
		// is under 2000; the wheel absorbs the step [orig: @0x481349..0x4813E5, `d_k -= delta`
		// @0x4813E5; Suspension_CompressWheelQuadratic @0x45CFB0 (the calls @0x48138A /
		// @0x48139E), Suspension_OscillateWheelFast @0x45D110 (the call @0x4813DD)].
		int32_t delta = 0;
		if (osc.energy > 0) {
			const int32_t q = (2 * osc.energy) / (2 * traits.spring); // idiv @0x48135B
			int32_t step = static_cast<int32_t>(std::sqrt(static_cast<double>(q)));
			if (step <= 0) osc.energy = 0; // [orig: @0x48136C..0x48136E]
			if (step > kSpringStepCap) step = kSpringStepCap;
			delta = conform_spring_compress(osc, m.wheel_comp[k], m.spring_energy,
			                                step, travel, traits.spring);
		} else if (osc.amplitude != 0 && m.plat_acc[0] < kOscillateSinkCeiling &&
		           m.plat_acc[1] < kOscillateSinkCeiling &&
		           m.plat_acc[2] < kOscillateSinkCeiling &&
		           m.plat_acc[3] < kOscillateSinkCeiling) {
			delta = conform_spring_oscillate(osc, m.wheel_comp[k], m.spring_energy,
			                                 shock, traits.spring, m.slide_z);
		}
		depth[k] -= delta;
		store_osc(m.wheel_osc[k], osc);
	}
}

// The bike's own grounded loop over its two wheels, with no crash gate anywhere: a wheel off
// the ground drops its corner by its sink beyond one growth step (only past it; no hard-landing
// marker: the +0x2EE store there needs both wheels down inside the one-wheel-off test, so it is
// dead), then the tracked loop's impulse, settle term (against wheel 0's amplitude) and compress
// arm, and the free decay gated on the two wheel sinks alone; the resolved depth lifts the
// corner. The entity+0x60 freeze (the record held at the pair's max) is never set here.
// [orig: Entity_ProcessLightVehiclePhysics @0x479600 - the loop @0x47B8CE..0x47BB7D (the wheel
//  body @0x47B930): the min
//  @0x47B8CE..0x47B90F (100000 init @0x47B598), spring 0 to the lift @0x47B933..0x47B93A; the
//  catch-up @0x47B960..0x47B9A0 (`sub eax, 64h; test eax, eax; jle` @0x47B96E..0x47B973, the dead
//  store @0x47B977..0x47B986, +0x60 @0x47B98C, +0x3DD @0x47B992); the impulse @0x47B9A7..0x47BA14
//  (thr @0x47B9AF..0x47B9C2); the settle term @0x47BA16..0x47BA86 (`[esi+304h]` @0x47BA2D); the
//  compress @0x47BA89..0x47BAEC (Suspension_CompressWheelQuadratic @0x45CFB0, the calls @0x47BAD0
//  / @0x47BAE4); the decay @0x47BAEE..0x47BB18 (sinks 0 / 1 @0x47BAFC / @0x47BB04,
//  Suspension_OscillateWheelFast @0x45D110, the call @0x47BB13); `sub [esp+edi+..+var_1F4], eax`
//  @0x47BB1B; +0x60 @0x47BB22]
void VehicleSystem::suspension_bike_grounded_loop(Entity &veh, const VehicleTraits &traits,
		int32_t depth[2], const bool contact[2], int32_t corner_adj[2]) {
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	if (traits.spring == 0)
		return;
	const int32_t min_depth = std::min(100000, std::min(depth[0], depth[1]));
	const int32_t travel = conform_travel_from_def(traits.spring_comp);
	const int32_t thr = traits.spring_comp <= 10 ? kImpulseThresholdSoft : kImpulseThresholdHard;
	int32_t loose_shock = 0;
	int32_t &shock = shock_field(world, veh, traits, loose_shock);
	for (int k = 0; k < 2; ++k) {
		if (!contact[k]) {
			const int32_t c = m.plat_acc[k] - kSinkGrowthBike;
			if (c > 0 && !m.wheelie_request)
				corner_adj[k] -= c;
		}
		ConformOscillator osc = load_osc(m.wheel_osc[k]);
		if (m.plat_acc[k] > thr && contact[k]) {
			const int32_t a = std::abs(m.plat_acc[k]);
			const int32_t sq = io::bam_mul(io::bam_mul(traits.mass, a), a);
			osc.energy += conform_half_energy(sq);
			if (osc.energy < 0) osc.energy = 0x1000000;
			m.spring_energy += static_cast<int32_t>(
					static_cast<double>(osc.energy) * static_cast<double>(1.25f));
		} else if (m.spring_energy <= 0) {
			int32_t e = depth[k] - min_depth;
			if (e > m.wheel_osc[0].amplitude) {
				e -= m.wheel_osc[0].amplitude;
				if (e > 0) {
					if (e > kSpringStepCap) e = kSpringStepCap;
					const int32_t sq = io::bam_mul(io::bam_mul(2 * traits.spring, e), e);
					osc.energy += conform_half_energy(sq);
				}
			}
		}
		int32_t delta = 0;
		if (osc.energy > 0) {
			const int32_t q = (2 * osc.energy) / (2 * traits.spring);
			int32_t step = static_cast<int32_t>(std::sqrt(static_cast<double>(q)));
			if (step <= 0) osc.energy = 0;
			if (step > kSpringStepCap) step = kSpringStepCap;
			delta = conform_spring_compress(osc, m.wheel_comp[k], m.spring_energy,
			                                step, travel, traits.spring);
		} else if (osc.amplitude != 0 && m.plat_acc[0] < kOscillateSinkCeiling &&
		           m.plat_acc[1] < kOscillateSinkCeiling) {
			delta = conform_spring_oscillate(osc, m.wheel_comp[k], m.spring_energy,
			                                 shock, traits.spring, m.slide_z);
		}
		depth[k] -= delta;
		store_osc(m.wheel_osc[k], osc);
	}
}

// The bike's own airborne loop, not the platform twin: no settled or crash
// gate, the spring step per wheel, then the corner droop only while the nose
// is above -0x1000 or the flight is under 16 ticks. The droop scales the
// sink beyond one growth step by -1.5, and a front sink of 500+ within 500 of
// the rear zeroes the rear sink, so the rear lags and the nose drops.
// [orig: Entity_ProcessLightVehiclePhysics @0x479600 — the loop
//  @0x47B401..0x47B4E6: compress @0x47B431 / oscillate @0x47B444, the gate
//  @0x47B44C..0x47B45D (fwd.z from Math_ExtractRow0FromFixedPoint22 @0x4798D6,
//  +0x3D4), the rear clear @0x47B461..0x47B48A, the droop `fmul flt_7C6F68`
//  (-1.5f) @0x47B498..0x47B4BC under !+0x3DD @0x47B4AA, the hard-landing mark @0x47B4BE..0x47B4CE]
void VehicleSystem::suspension_bike_airborne_loop(Entity &veh, const VehicleTraits &traits,
		int32_t fwd_z16, int32_t corner_adj[2]) {
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	const int32_t travel = conform_travel_from_def(traits.spring_comp);
	int32_t loose_shock = 0;
	int32_t &shock = shock_field(world, veh, traits, loose_shock);
	for (int k = 0; k < 2; ++k) {
		ConformOscillator osc = load_osc(m.wheel_osc[k]);
		if (osc.energy > 0) {
			(void)conform_spring_compress(osc, m.wheel_comp[k], m.spring_energy,
			                              kSpringStepCap, travel, traits.spring);
		} else if (osc.amplitude != 0) {
			(void)conform_spring_oscillate(osc, m.wheel_comp[k], m.spring_energy,
			                               shock, traits.spring, m.slide_z);
		}
		store_osc(m.wheel_osc[k], osc);
		if (fwd_z16 <= -0x1000 && m.plat_airborne_ticks >= 16)
			continue;
		const int32_t c = m.plat_acc[k] - kSinkGrowthBike;
		if (m.plat_acc[0] >= 500 && std::abs(m.plat_acc[0] - m.plat_acc[1]) < 500)
			m.plat_acc[1] = 0;
		if (c <= 0)
			continue;
		const int32_t adj = static_cast<int32_t>(static_cast<float>(c) * -1.5f);
		if (!m.wheelie_request)
			corner_adj[k] += adj;
		if (adj < kHardLandingCatchupBelow && m.slide_z < 0)
			m.landing_2ee = 1;
	}
}

// The tank has a 1023-step linear spring and the slow oscillator. Its
// landing impulse comes from the oscillator's +0x10 channel, not the wheel
// sink, and it has no ground-family quadratic settling term.
// [orig: Entity_ProcessWheeledVehiclePhysics @0x475DE0, step @0x47690A,
// airborne @0x4787EC..0x47888D, grounded @0x478D34..0x478F57]
void VehicleSystem::suspension_tank_loop(Entity &veh, const VehicleTraits &traits, bool on_ground,
		int32_t depth[4], const bool contact[4], int32_t corner_adj[4]) {
	auto &m = veh.veh;
	constexpr int32_t step_cap = 65535 >> 6;
	const int32_t travel = conform_travel_from_def(traits.spring_comp);
	int32_t loose_shock = 0;
	int32_t &shock = shock_field(world_, veh, traits, loose_shock);
	for (int k = 0; k < 4; ++k) {
		if (on_ground && traits.spring == 0)
			continue;
		if (on_ground && !contact[k] && m.crash_request == 0 && m.crashed == 0)
			catch_up(m, k, 250, true, contact, corner_adj, /*tank=*/true);
		ConformOscillator osc = load_osc(m.wheel_osc[k]);
		int32_t delta = 0;
		if (on_ground) {
			depth[k] = std::max(0, depth[k]);
			if (m.wheel_osc[k].impulse > 0 && depth[k] != 0) {
				osc.energy = io::bam_add(osc.energy, io::bam_mul(traits.mass, m.wheel_osc[k].impulse));
				if (osc.energy > 65536)
					osc.energy = 65536;
				m.wheel_osc[k].impulse = 0;
				m.spring_energy = io::bam_add(m.spring_energy, int32_t(double(osc.energy) * 1.25));
			}
		}
		if (osc.energy > 0) {
			int32_t step = step_cap;
			if (on_ground) {
				step = osc.energy / io::bam_mul(2, traits.spring);
				if (step < 0) {
					step = step_cap;
					osc.energy = 0x1000000;
				} else if (step == 0)
					osc.energy = 0;
				step = std::min(step, step_cap);
			}
			delta = conform_spring_compress_linear(
					osc, m.wheel_comp[k], m.spring_energy, step, travel, traits.spring);
		} else if (on_ground && osc.amplitude != 0 && m.plat_acc[0] < 2000 &&
				m.plat_acc[1] < 2000 && m.plat_acc[2] < 2000 && m.plat_acc[3] < 2000) {
			// [orig: Suspension_OscillateWheel @0x45D240, flt_7C6A14]
			delta = conform_spring_oscillate(osc, m.wheel_comp[k], m.spring_energy, shock,
					traits.spring, m.slide_z, 0.08722222596406937f);
		}
		if (on_ground)
			depth[k] = io::bam_sub(depth[k], delta);
		store_osc(m.wheel_osc[k], osc);
		if (!on_ground && m.crash_request == 0 && m.crashed == 0)
			catch_up(m, k, 250, false, contact, corner_adj);
	}
}

bool VehicleSystem::suspension_arm(
		Entity &veh, bool eject_occupants, const int32_t corners[4][3], const bool *contacts) {
	World &world = world_;
	Entity::VehicleMotorState &m = veh.veh;
	// The gate: a crash request pending and not yet crashed
	// [orig: `cmp [+2EDh],0; jz` @0x46b1a6 then `cmp [+2ECh],0; jnz` @0x46b1b9].
	if (m.crash_request == 0 || m.crashed != 0) return false;
	// The one-shot role pick [orig: @0x46b1c5..0x46b1db] — consumed by the
	// (residual) impulse dump.
	m.susp_rate_pick = world.rules.logic_authority ? kSuspensionDisableRateAuthority
	                                           : kSuspensionDisableRateNonAuthority;
	m.byte_2ef = 0; // [orig: @0x46b1db]
	if (world.rules.logic_authority) {
		veh.flags |= kEntityFlagSuspensionCrashed; // [orig: @0x46b1ed]
	} else if ((veh.flags & kEntityFlagSuspensionCrashed) == 0) {
		return false; // [orig: the `test Flags, 0x10` skip @0x46b1f3]
	}
	m.crashed = 1;     // [orig: @0x46b1f9]
	m.landing_2ee = 0; // [orig: @0x46b20d]
	if (!eject_occupants)
		detail::vehicle_clear_chassis(m); // [orig: @0x46B213]
	if (corners != nullptr) {
		const bool airborne = (veh.flags & kEntityFlagInAir) != 0;
		for (int k = 0; k < 4; ++k) {
			if (airborne || (contacts != nullptr && !contacts[k])) {
				auto &force = m.chassis_forces[k];
				force.direction[0] = force.direction[1] = 0;
				force.direction[2] = -65536;
				force.rate = airborne ? m.plat_acc[k]
									  : int32_t(double(m.plat_acc[k]) * m.susp_rate_pick);
			}
		}
		detail::vehicle_clear_chassis_forces(veh, corners, 0); // [orig: @0x46B314]
	}
	if (eject_occupants) {
		// The bike twin ejects every rider at its latch
		// [orig: Entity_EjectAllOccupants @0x468b3b].
		for (Seat &s : veh.seats) {
			if (!s.occupant.valid()) continue;
			const EntityHandle occ = s.occupant;
			(void)world.vehicles.detach(occ);
		}
	}
	return true;
}

void vehicle_suspension_post_contact(Entity &veh, const bool contact[4], int32_t up_z16) {
	Entity::VehicleMotorState &m = veh.veh;
	// Each pad in contact zeroes its own sink [orig: tracked @0x47eced /
	//  @0x47ecfb / @0x47ed09 / @0x47ed17 — `cmp pad, 0; jz; mov sink, 0`].
	for (int k = 0; k < 4; ++k)
		if (contact[k]) m.plat_acc[k] = 0;
	// Then a diagonal pair in contact [orig: (0 && 2) || (1 && 3)
	//  @0x47ed1d..0x47ed2b], the hull upright [orig: `cmp ebp, 0; jle` on
	//  up.z @0x47ed2d] and not crashed [orig: @0x47ed31] → the hard-landing
	// marker, every sink and byte_2ef clear [orig: @0x47ed3a..0x47ed59; the
	// `crashed = 0` @0x47ed60 is already true under the gate].
	const bool pair = (contact[0] && contact[2]) || (contact[1] && contact[3]);
	if (pair && up_z16 > 0 && m.crashed == 0) {
		m.landing_2ee = 0;
		for (int k = 0; k < 4; ++k) m.plat_acc[k] = 0;
		m.byte_2ef = 0;
	}
}

void vehicle_suspension_tick_tail(Entity &veh, const VehicleTraits &traits) {
	Entity::VehicleMotorState &m = veh.veh;
	m.crash_request = 0; // [orig: @0x47eeee / @0x4795da / @0x47c0b6 — unconditional]
	// The amplitude tail [orig: @0x48178a..0x4817c4]: thr = ftol(0.01 ×
	// ftol(travel_locked)), where travel_locked = ftol(0xFFFF × (100 −
	// spring_comp) × 0.01) is the global dword_815184 the tracked solve stamps;
	// both products at the FPU's length [orig: `fild dword_815184; fmul flt_7C56A8`
	// @0x48178A..0x481790].
	const int32_t locked =
			conform_travel_locked(std::min(std::max(traits.spring_comp, 0), 100));
	const int32_t thr =
			static_cast<int32_t>(static_cast<double>(locked) * static_cast<double>(0.01f));
	bool quiet = true;
	for (int k = 0; k < 4; ++k) quiet = quiet && m.wheel_osc[k].amplitude <= thr;
	if (quiet && m.spring_energy != 0) m.spring_energy = 0;
}

} // namespace opennova::world
