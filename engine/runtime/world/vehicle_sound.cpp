// Ground-vehicle sound-profile consumer (the VehicleSystem sound verbs).
//
// The portable producer for retail's generic entity-attached sound-emitter
// seam. It owns authored slot selection and fixed-point gain/pitch math;
// callers and the Godot presenter only see SoundEmitterEvent rows.
// [orig: Entity_ProcessMovementSoundEffects @0x5294a0]
#include <runtime/world/vehicle_system.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include <base/io/fixed.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/world/ai.h>
#include <runtime/world/geom.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

constexpr uint8_t kIdleLane = 0;
constexpr uint8_t kForwardLane = 10;
constexpr uint8_t kReverseLane = 20;
constexpr uint8_t kPivotLane = 40;
constexpr uint16_t kEmitterLifetimeTicks = 30;
constexpr int32_t kUnityQ16 = io::kFp16OneInt;
constexpr uint16_t kFullVolumeQ8_8 = 0xFFFF;

int64_t magnitude_i32(int32_t value) {
    return value < 0 ? -static_cast<int64_t>(value)
                     : static_cast<int64_t>(value);
}

int32_t clamp_i32(int64_t value) {
    return static_cast<int32_t>(std::clamp(
            value, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
            static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

int64_t rounded_q16_product(int64_t value) {
    // Arithmetic `(value + 0x8000) >> 16` without relying on a
    // negative-right-shift implementation choice.
    const int64_t biased = value + 0x8000;
    if (biased >= 0) return biased / 0x10000;
    return -((-biased + 0xFFFF) / 0x10000);
}

const audio::SoundProfile *profile_for(const World &world,
                                       const VehicleTraits &traits) {
    if (!traits.sound_profile.empty())
        return world.tables.sound_profiles.find(traits.sound_profile.c_str());
    return world.tables.sound_profiles.find("default");
}

std::string set_for_slot(const audio::SoundProfile *profile,
                         const VehicleTraits &traits, int slot) {
    if (slot >= 0 && slot < static_cast<int>(traits.sound_loops.size()) &&
        !traits.sound_loops[static_cast<size_t>(slot)].empty())
        return traits.sound_loops[static_cast<size_t>(slot)];
    if (profile == nullptr || slot < 0 || slot >= audio::kSoundProfileSlotCount)
        return {};
    return profile->set_names[static_cast<size_t>(slot)];
}

uint32_t producer_tick(const World &world) {
    // World increments its public clock after the system loop. Presentation
    // observes that post-tick value, so stamp the registration in the same frame.
    return world.logic_tick + 1;
}

bool has_live_primary_claimant(const World &world, const Entity &vehicle) {
    Entity scratch; // a joiner's remote claimant projects here
    const Entity *claimant = world.vehicles.claimant(vehicle, scratch);
    return claimant != nullptr && claimant->mounted &&
           claimant->mount_target == vehicle.handle;
}

void emit_source_anchor(World &world, const Entity &vehicle) {
    SoundEmitterEvent ev;
    ev.source_spawn_id = vehicle.registry_spawn_id;
    ev.source_handle = vehicle.handle.packed;
    ev.pos = vehicle.position;
    ev.source_bms_id = vehicle.bms_id;
    ev.emitted_tick = producer_tick(world);
    ev.lifetime_ticks = kEmitterLifetimeTicks;
    ev.source_only = true;
    world.out.sound_emitters.publish(std::move(ev));
}

// Retail slots retain the entity-position pointer while an unrefreshed lane
// expires. While a lane registered by an earlier fold is still alive, this
// source-only intent keeps it spatially attached without renewing it.
void anchor_residual_lanes(World &world, const Entity &vehicle) {
    const uint32_t remaining = vehicle.veh.sound_anchor_until_tick - producer_tick(world);
    if (vehicle.veh.sound_anchor_until_tick != 0 && remaining <= kEmitterLifetimeTicks)
        emit_source_anchor(world, vehicle);
}

// `lifetime_ticks` is the registration's effect_params word +16: 30 for the
// ground fold's loops, 15 for the helicopter's three. [orig: SoundEmitter_Register
// @0x529270 packs its fifth argument @0x5292A6; update_vehicle_effect_emissions
// @0x528F94 stores 15; SoundEmitter_RegisterSetLayers copies +16 into slot word
// 21 @0x528471]
void emit_emitter(World &world, Entity &vehicle, uint8_t lane, int slot,
                  const std::string &set_name, int32_t pitch_q16,
                  uint16_t volume_q8_8,
                  uint16_t lifetime_ticks = kEmitterLifetimeTicks) {
    // A null sound-set pointer makes the original wrapper a no-op. Zero controls,
    // however, are an explicit keyed clear and must cross the host seam even when
    // the authored slot itself is empty.
    if (set_name.empty() && pitch_q16 != 0 && volume_q8_8 != 0) return;
    SoundEmitterEvent ev;
    ev.source_spawn_id = vehicle.registry_spawn_id;
    ev.source_handle = vehicle.handle.packed;
    ev.pos = vehicle.position;
    ev.source_bms_id = vehicle.bms_id;
    ev.emitted_tick = producer_tick(world);
    ev.lane = lane;
    ev.slot = static_cast<uint8_t>(slot);
    ev.lifetime_ticks = lifetime_ticks;
    ev.pitch_q16 = pitch_q16;
    ev.volume_q8_8 = volume_q8_8;
    ev.set_name = set_name;
    if (pitch_q16 != 0 && volume_q8_8 != 0) {
        vehicle.veh.sound_anchor_until_tick =
                ev.emitted_tick + lifetime_ticks;
    }
    world.out.sound_emitters.publish(std::move(ev));
}

void emit_profile_oneshot(World &world, const Entity &vehicle,
                          const audio::SoundProfile *profile,
                          const VehicleTraits &traits, int slot) {
    const std::string set = set_for_slot(profile, traits, slot);
    if (set.empty()) return;
    SoundSlotEvent ev;
    ev.source_handle = vehicle.handle.packed;
    ev.pos[0] = to_fixed(vehicle.position.x);
    ev.pos[1] = to_fixed(vehicle.position.y);
    ev.pos[2] = to_fixed(vehicle.position.z);
    ev.slot = static_cast<uint8_t>(slot);
    std::snprintf(ev.set_name, sizeof(ev.set_name), "%s", set.c_str());
    world.out.slot_sounds.push_back(ev);
}

// The all-zero movement fold (speed and reference speed zero): an explicit
// clear of the forward and reverse lanes; the idle lane expires on its own.
// [orig: Entity_ProcessMovementSoundEffects @0x5294A0 called with zeros]
void clear_motion_lanes(World &world, Entity &vehicle) {
	emit_emitter(world, vehicle, kForwardLane, audio::kSlotSoundLoop1 + 1, {}, 0, 0);
	emit_emitter(world, vehicle, kReverseLane, audio::kSlotSoundLoop1 + 2, {}, 0, 0);
}

int32_t interpolated_pitch(const audio::SoundProfile *profile, int slot,
                           int64_t speed, int64_t speed_denominator) {
    int64_t low = 0;
    int64_t high = kUnityQ16;
    int32_t gears = 0;
    if (profile != nullptr && slot >= 0 && slot < audio::kSoundProfileSlotCount) {
        low = profile->param2_q16[static_cast<size_t>(slot)];
        high = profile->param3_q16[static_cast<size_t>(slot)];
        gears = profile->param4[static_cast<size_t>(slot)];
        if (high == 0) high = kUnityQ16;
    }
    if (speed_denominator <= 0) return high;
    if (speed < 0) speed = 0;

    // Forward profiles may divide the speed range into authored gear bands.
    // Each band repeats a low->high pitch ramp while its endpoints fall by a
    // quarter across the complete gear count.
    // [orig: the param4>1 branch @0x52968f..0x529765]
    if (slot == audio::kSlotSoundLoop1 + 1 && gears > 1) {
        const int64_t band = speed_denominator / gears;
        if (speed == speed_denominator || band <= 0) return high;
        const int64_t gear = speed / band;
        const int64_t remainder = speed % band;
        if (gear > 0) {
            const int64_t divisor = 4LL * gears;
            low = clamp_i32(low - gear * (low / divisor));
            // Sequential on purpose: retail computes the high endpoint from
            // the already-adjusted low endpoint.
            high = clamp_i32(high - gear * (low / divisor));
        }
        const int64_t ratio = (remainder << 16) / band;
        return clamp_i32(low + rounded_q16_product(ratio * (high - low)));
    }

    // The straight interpolation caps only the numerator at 0xFFFF; it does
    // not clamp to playerSpeed. That distinction matters for unusually fast
    // authored vehicles and matches the original signed-word saturation.
    speed = std::min<int64_t>(speed, 0xFFFF);
    const int64_t ratio = std::min(
            (speed << 16) / speed_denominator,
            static_cast<int64_t>(std::numeric_limits<int32_t>::max()));
    return clamp_i32(low + rounded_q16_product(ratio * (high - low)));
}

} // namespace

// Profile SSAudio3 carries the contact scrape; other contact transitions use
// their own family slot. [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0,
// profile lookup @0x47CE17..0x47CE62]
void VehicleSystem::play_contact_sound(Entity &vehicle, const VehicleTraits &traits, int slot) {
	const auto *profile = profile_for(world_, traits);
	if (!set_for_slot(profile, traits, slot).empty())
		emit_profile_oneshot(world_, vehicle, profile, traits, slot);
	else if (slot == 25 || slot == 26 || slot == 36)
		// The global name-to-slot table @0x82F590 binds +0x24E0908 and
		// +0x24E08D4 to these sets. Fallbacks use propagation delay.
		world_.out.fire_sounds.play_with_distance_delay(
				slot == 25 ? "TIRE_SKID" : "IMP_DEBMED_LAND", vehicle.position, vehicle.bms_id,
				vehicle.handle.packed);
}

void VehicleSystem::update_ground_sound(Entity &vehicle, const VehicleTraits &traits, bool wrecked, bool collided) {
    World &world = world_;
	// Every mover fronts its movement fold with the outer loop's last-tick
	// flag: a catch-up tick leaves the registered lanes untouched.
	// [orig: dword_24E0E80 set by Game_MainLoop @0x52BA24..0x52BA3A; read by
	//  cveh @0x48D156, ctan @0x48AA9E, cbik @0x48670A, cbot @0x48EDFF and the
	//  selector-zero ground @0x46F7C8 and boat @0x471523 movers]
	if (!world.rules.last_tick_of_batch)
		return;
	// A crash-settled tank skips the movement fold; its registered lanes expire
	// while the source anchor keeps them on the hull.
	// [orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, jump @ 0x48AABE]
	if (traits.family == VehicleFamily::Tank && vehicle.veh.settle_2f0 != 0) {
		anchor_residual_lanes(world, vehicle);
		return;
	}
    const audio::SoundProfile *profile = profile_for(world, traits);

    // The PlayerControl ground caller skips the movement-sound function entirely
    // without entity+368. This is why an NPC claimant runs the engine just like a
    // player, while a passenger or surviving non-claimant controller does not.
    // [orig: caller gate @0x48d181..0x48d1c7; the selector-zero ground mover's
    // twin @0x46f7e2..0x46f7f7 and the selector-zero boat's @0x471533..0x471546
    // (IDB: Entity_ProcessInfantryPhysics @0x46E100 / Entity_ProcessAirVehiclePhysics
    // @0x46FA00, both misnomers)]
    if (traits.player_control && !has_live_primary_claimant(world, vehicle)) {
        anchor_residual_lanes(world, vehicle);
        return;
    }

	Entity claimant_scratch;
	const Entity *claimant = world.vehicles.claimant(vehicle, claimant_scratch);
	const bool submerged_driver = claimant != nullptr && claimant->player_class != 0 &&
			watercraft_driver_submerged(world, *claimant);
	// Retail zeroes all four sound arguments when a player claimant's eye is
	// submerged, before selecting the stop/collision/motion branch.
	// [orig: Entity_ProcessMovementSoundEffects @0x5294A0, gate @0x5294A8..0x5294D8]
	const bool stopped =
			wrecked || ((vehicle.flags | vehicle.engine_flags) & 2u) != 0 || submerged_driver;
	// The family's max drive speed: boats author waterSpeed (+0x8EC) and no
	// player_speed — the ground field would zero the denominator and pin every
	// boat in the stop lane.
	int32_t max_speed =
			traits.family == VehicleFamily::Watercraft ? traits.water_speed : traits.player_speed;
	// The ground caller falls back to brain speed A, then the command; the
	// selector-zero ground mover repeats that chain verbatim and the
	// selector-zero boat runs it from waterSpeed (and skips the fold entirely
	// when nothing resolves, see tick_simple_motor).
	// [orig: Entity_UpdateVehiclePhysics @0x48AF00, caller @0x48D196..0x48D1B8;
	//  @0x46F7F9..0x46F813; @0x4715AA..0x4715C6]
	if (max_speed == 0 && (traits.family != VehicleFamily::Watercraft || traits.physics == 0)) {
		if (const AiEntity *ai = world.ai.for_handle(vehicle.handle))
			max_speed = ai->brain.f[AiBrain::kSpeedA];
		if (max_speed == 0)
			max_speed = vehicle.veh.cmd_speed;
	}
	const int64_t denominator = magnitude_i32(max_speed);
	const auto emit_idle_and_pivot = [&](uint16_t volume) {
		emit_emitter(world, vehicle, kIdleLane, audio::kSlotSoundLoop1,
				set_for_slot(profile, traits, audio::kSlotSoundLoop1), kUnityQ16, volume);
		// Retail reaches the fourth loop only after an idle registration. The
		// tank caller passes abs(entity+0xA4), its YAW RATE; +0xA0 is slide_z.
		// Preserve the signed abs wrap and low-word volume at INT_MIN.
		// [orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, @ 0x48AAE0;
		// Entity_ProcessMovementSoundEffects @ 0x5294A0, @ 0x529887..0x5298C6]
		// docs/audio/lwf-dbf-sound-re.md (D-SND-17).
		if (traits.family == VehicleFamily::Tank && vehicle.veh.pivot_sound_latched) {
			const int32_t rate = vehicle.veh.wheel_rate_bam;
			const int32_t amount = rate < 0 ? io::bam_sub(0, rate) : rate;
			if (amount != 0)
				emit_emitter(world, vehicle, kPivotLane, audio::kSlotSoundLoop1 + 3,
						set_for_slot(profile, traits, audio::kSlotSoundLoop1 + 3), kUnityQ16,
						static_cast<uint16_t>(std::min<int32_t>(amount, 0xFFFF)));
		}
	};
	if (stopped || denominator <= 0) {
		emit_emitter(world, vehicle, kForwardLane, audio::kSlotSoundLoop1 + 1,
                     {}, 0, 0);
        emit_emitter(world, vehicle, kReverseLane, audio::kSlotSoundLoop1 + 2,
                     {}, 0, 0);
	} else if (collided) {
		// Collision is not the all-zero/wreck branch. Retail clears reverse,
        // clears forward, then falls through to a forced full-volume idle
        // registration for this tick.
        // [orig: @0x5297db..0x529882]
        emit_emitter(world, vehicle, kReverseLane, audio::kSlotSoundLoop1 + 2,
                     {}, 0, 0);
        emit_emitter(world, vehicle, kForwardLane, audio::kSlotSoundLoop1 + 1,
                     {}, 0, 0);
		emit_idle_and_pivot(kFullVolumeQ8_8);
	} else {
		int64_t speed = magnitude_i32(vehicle.veh.speed);
		if (speed == 0 && vehicle.saved_live_valid) {
			const double dx =
					static_cast<double>(vehicle.saved_live_pos[0]) - to_fixed(vehicle.position.x);
			const double dy =
					static_cast<double>(vehicle.saved_live_pos[1]) - to_fixed(vehicle.position.y);
			const double dz =
					static_cast<double>(vehicle.saved_live_pos[2]) - to_fixed(vehicle.position.z);
			// fild/square/sum/fsqrt, flt_7C19E0 clamp, then truncation.
			// Direction still comes from the original signed speed argument.
			// [orig: Entity_ProcessMovementSoundEffects @0x5294A0, displacement
			// @0x52953B..0x52959D]
			speed = static_cast<int64_t>(
					std::min(std::sqrt(dx * dx + dy * dy + dz * dz), 2147418112.0));
		}
		if (speed < 256)
			speed = 0; // sub-1/256-unit motion is treated as stationary

		if (speed > 0) {
            const bool reverse = vehicle.veh.speed < 0;
            const int slot = reverse ? audio::kSlotSoundLoop1 + 2
                                     : audio::kSlotSoundLoop1 + 1;
            const uint8_t lane = reverse ? kReverseLane : kForwardLane;
            const int64_t ramp_denominator = denominator / 16;
            int64_t volume = kFullVolumeQ8_8;
            if (ramp_denominator > 0 && speed < ramp_denominator) {
                volume = (speed << 16) / ramp_denominator;
            }
            volume = std::clamp<int64_t>(volume, 0, kFullVolumeQ8_8);
            const int32_t pitch =
                    interpolated_pitch(profile, slot, speed, denominator);
            emit_emitter(world, vehicle, lane, slot,
                         set_for_slot(profile, traits, slot), pitch,
                         static_cast<uint16_t>(volume));
        }

        // Idle remains a simultaneous lane and fades inversely across the complete
        // authored speed range. At or above playerSpeed it is simply not refreshed,
        // so the shared emitter lifetime retires it.
        if (speed < denominator) {
            int64_t volume = kUnityQ16 - (speed << 16) / denominator;
            volume = std::clamp<int64_t>(volume, 0, kFullVolumeQ8_8);
            if (volume > 0) {
				emit_idle_and_pivot(static_cast<uint16_t>(volume));
            }
        }
	}

	// The movement direction latch plays enginereverse on both direction edges.
    // Entering reverse waits for command AND actual speed to be negative; leaving
    // only waits for a positive command. The latch is vehicleData+0x318 bit 1
    // (value 2, `test al, 2` @0x48d1d7); bit 2 (value 4) is the lights latch
    // @0x48d358, bit 3 (value 8) the skid latch @0x48d2c4 and bit 0 (value 1)
    // the claimant latch @0x48d3a5.
    // [orig: @0x48d1d1..0x48d222; the selector-zero twins @0x46f828..0x46f888
    //  and @0x4715da..0x471667]
    if (!world.vehicles.claimant_present(vehicle)) return;
    if (!vehicle.veh.reverse_sound_latched && vehicle.veh.cmd_speed < 0 &&
        vehicle.veh.speed < 0) {
        vehicle.veh.reverse_sound_latched = true;
        emit_profile_oneshot(world, vehicle, profile, traits,
                             audio::kSlotEngineReverse);
    } else if (vehicle.veh.reverse_sound_latched && vehicle.veh.cmd_speed > 0) {
        vehicle.veh.reverse_sound_latched = false;
        emit_profile_oneshot(world, vehicle, profile, traits,
                             audio::kSlotEngineReverse);
    }
}

// Skid edge and free-rev cadence follow movement sound. The settle byte
// (+0x2F0) skips the movement, direction and high-rev sections; cveh's jump
// lands ON the skid section, so a settled wreck still clears a latched skid,
// while ctan's jump lands past its skid section. The skid test is the ftol of
// the contact-direction magnitude (sqrt of the three squares, flt_7C19E0 =
// 2147418112.0 clamp): a sub-unit direction truncates to no skid. The timer
// increments even without an occupant; a successful rev resets it before the
// increment.
// [orig: cveh settle jump @0x48D163..0x48D16A -> @0x48D264, high-rev
// @0x48D22A..0x48D261, skid @0x48D264..0x48D34E (bit 8 set @0x48D2EB, clear
// @0x48D345), timer @0x48D43D; cbik @0x4867DD..0x486904; ctan settle jump
// @0x48AAB7..0x48AABE -> @0x48AD49, skid @0x48ABCB..0x48ACB5]
void VehicleSystem::update_traction_sound(Entity &vehicle, const VehicleTraits &traits) {
	auto &m = vehicle.veh;
	// The high-rev, skid and tank pivot sections share the fold's last-tick
	// gate, the tank's +0x328 prior-rate store included; the rev timer does
	// not. [orig: ctan `jz loc_48AD5B` @0x48AAA5; cveh @0x48D15D; cbik @0x486710]
	Entity claimant_scratch;
	const bool claimant = world_.vehicles.claimant(vehicle, claimant_scratch) != nullptr;
	if (world_.rules.last_tick_of_batch) {
		if (m.settle_2f0 == 0 && claimant &&
				m.rev_sound_ticks > 124 && m.plat_airborne_ticks > 30) {
			m.rev_sound_ticks = 0;
			emit_profile_oneshot(world_, vehicle, profile_for(world_, traits), traits, 33);
		}
		if (m.settle_2f0 == 0 || traits.family != VehicleFamily::Tank) {
			const double cx = m.contact_direction[0], cy = m.contact_direction[1],
						 cz = m.contact_direction[2];
			const int32_t magnitude = static_cast<int32_t>(
					std::min(std::sqrt(cx * cx + cy * cy + cz * cz), 2147418112.0));
			const bool skid =
					magnitude != 0 && m.speed != 0 && (vehicle.flags & kEntityFlagInAir) == 0;
			if (skid && !m.skid_sound_latched)
				play_contact_sound(vehicle, traits, 25);
			m.skid_sound_latched = skid;
		}
		if (traits.family == VehicleFamily::Tank) {
			// The stationary-turn edge follows the loop, so its first refresh is
			// next tick. Start above ten degrees of heading error; retain through
			// the threshold until translation, zero rate, or a rate sign change.
			// Crash-settled tanks preserve the latch but still save the current
			// rate. [orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0,
			// @ 0x48ACB5..0x48AD53]
			if (m.settle_2f0 == 0) {
				if (claimant &&
						!m.pivot_sound_latched && m.speed == 0 && m.wheel_rate_bam != 0) {
					const int32_t error = io::bam_sub(m.steer_target_bam, m.yaw_bam);
					const int32_t magnitude = error < 0 ? io::bam_sub(0, error) : error;
					if (magnitude > 0x071C71C0) {
						m.pivot_sound_latched = true;
						emit_profile_oneshot(world_, vehicle, profile_for(world_, traits), traits,
								audio::kSlotSwivelShift);
					}
				} else if (m.pivot_sound_latched && (m.speed != 0 || m.wheel_rate_bam == 0 ||
						((uint32_t(m.pivot_sound_prev_rate) ^ uint32_t(m.wheel_rate_bam)) &
								0x80000000u))) {
					m.pivot_sound_latched = false;
				}
			}
			m.pivot_sound_prev_rate = m.wheel_rate_bam;
		}
	}
	++m.rev_sound_ticks;
}

// The tank's tread cue: every even tick adds this speed and the previous even
// tick's; a sum at or past +-0x80000 plays profile slot 45 on the hull and
// restarts. It runs for every tank, crash-settled, unoccupied or not, outside
// the last-tick gate. `add eax, eax; sar eax, 1` keeps the sum's low 31 bits.
// [orig: Entity_UpdateTankVehiclePhysics @0x48AE45..0x48AEA1, `test byte ptr
//  tick, 1` @0x48AE45 on current_tick, which Game_ProcessMainFrame @0x5265B4
//  increments before the entity update]
void VehicleSystem::update_tread_sound(Entity &vehicle, const VehicleTraits &traits) {
	auto &m = vehicle.veh;
	if ((world_.logic_tick & 1u) != 0)
		return;
	const uint32_t sum = uint32_t(m.speed) + uint32_t(m.tread_sound_prev_speed);
	m.tread_sound_accum = io::bam_add(m.tread_sound_accum, io::bam_sar(int32_t(sum << 1), 1));
	m.tread_sound_prev_speed = m.speed;
	if (m.tread_sound_accum >= 0x80000 || m.tread_sound_accum <= -0x80000) {
		emit_profile_oneshot(
				world_, vehicle, profile_for(world_, traits), traits, audio::kSlotDriveRepeat);
		m.tread_sound_accum = 0;
	}
}

// The claimant and lights edge lanes follow the continuous movement fold. The
// selector-zero ground mover shares this tail verbatim (lights bit 2 -> slot
// 24, claimant bit 0 -> slot 30 on the occupant eye above water, the
// all-zero fold plus slot 31 on the hull +0x18000 when the claimant leaves);
// the selector-zero boat runs the claimant edge at its HEAD (inside the
// PlayerControl block, before the authority split) and only the lights leg
// in its tail (tick_simple_motor).
// [orig: Entity_UpdateVehiclePhysics @0x48AF00, tail @0x48D34E..0x48D429;
// Entity_UpdateWatercraftPhysics @0x48D480, edge @0x48DAD1..0x48DB6B;
// Entity_ProcessInfantryPhysics @0x46E100 (IDB misnomer, the selector-zero
// ground mover) lights @0x46F8C3..0x46F8FC, claimant edge @0x46F8FC..0x46F99C;
// Entity_ProcessAirVehiclePhysics @0x46FA00 (the selector-zero boat) claimant
// edge @0x470055..0x4700EB, lights @0x4714EA..0x471523]
void VehicleSystem::update_engine_sound(Entity &vehicle, const VehicleTraits &traits) {
	if (vehicle_family_uses_direct_air_mover(traits.family))
		return;
	const auto *profile = profile_for(world_, traits);
	const bool lights = (vehicle.flags & 0x80u) != 0;
	if (traits.family != VehicleFamily::Watercraft || traits.physics == 0) {
		if (lights && !vehicle.veh.light_sound_latched)
			emit_profile_oneshot(world_, vehicle, profile, traits, audio::kSlotAudio1);
		vehicle.veh.light_sound_latched = lights;
	}
	update_claimant_engine_sound(vehicle, traits);
}

void VehicleSystem::update_claimant_engine_sound(Entity &vehicle, const VehicleTraits &traits) {
	if (vehicle_family_uses_direct_air_mover(traits.family))
		return;
	if (!traits.player_control)
		return;
	const auto *profile = profile_for(world_, traits);
	Entity occupant_scratch;
	const Entity *occupant = world_.vehicles.claimant(vehicle, occupant_scratch);
	if (occupant != nullptr) {
		if (!vehicle.veh.engine_sound_latched) {
			vehicle.veh.engine_sound_latched = true;
			if (static_cast<int64_t>(to_fixed(occupant->position.z)) + occupant->eye_offset_z >
					world_.env.water_z)
				emit_profile_oneshot(world_, vehicle, profile, traits, audio::kSlotEngineStart);
		}
	} else if (vehicle.veh.engine_sound_latched) {
		// The leave edge clears bit 0 alone. Every mover but the full
		// watercraft runs the all-zero fold first; all of them sound the stop
		// on the hull when its Z + 0x18000 clears the water plane.
		// [orig: ctan @0x48ADE9..0x48AE33; cveh @0x48D3DA..0x48D421; selector-zero
		//  boat @0x4700A1..0x4700EB; cbot without the fold @0x48DB26..0x48DB5D]
		vehicle.veh.engine_sound_latched = false;
		if (traits.family != VehicleFamily::Watercraft || traits.physics == 0)
			clear_motion_lanes(world_, vehicle);
		if (static_cast<int64_t>(to_fixed(vehicle.position.z)) + 0x18000 > world_.env.water_z)
			emit_profile_oneshot(world_, vehicle, profile, traits, audio::kSlotEngineStop);
	}
}

// The helo start is a cold-rotor edge, not the generic ground claimant edge.
// [orig: Entity_UpdateHeloRotorSpin @0x48FA70, start @0x48FAA1..0x48FB0C]
// The helicopter's three simultaneous loops use rotor speed and climb intensity.
// The aircraft caller runs this while the rotor coasts after its claimant leaves.
// [orig: update_vehicle_effect_emissions @ 0x528F20;
//  Entity_UpdateHeloRotorSpin @ 0x48FA70]
void VehicleSystem::update_rotor_sound(Entity &vehicle, const VehicleTraits &traits) {
	// The same last-tick gate fronts the rotor loops.
	// [orig: Entity_UpdateHeloRotorSpin @0x48FDD3..0x48FDDA]
	if (!world_.rules.last_tick_of_batch)
		return;
	if (((vehicle.flags | vehicle.engine_flags) & 2u) != 0)
		return;
	const auto *profile = profile_for(world_, traits);
	if (profile == nullptr)
		return;
	const auto &p = profile->loop_params;
	const int32_t ratio = std::min<int32_t>(
			65536, static_cast<int32_t>(int64_t(vehicle.veh.part_spin.speed) * 65536 / 214748352));
	int32_t lateral = 0;
	if (ratio != 0) {
		const int64_t vertical = magnitude_i32(vehicle.veh.slide_z);
		const int32_t collective = vertical < 334 || traits.climb_speed == 0
				? 0
				: static_cast<int32_t>(vertical * 65536 / traits.climb_speed);
		const int32_t product = static_cast<int32_t>(167772u * uint32_t(collective) + 128u);
		lateral = (product >> 8) - 2048;
	}
	// Invalid ranges leave the destination untouched. The cruise volume reuses
	// the medium-volume scratch word, including this authored-degenerate case.
	// Keep the float reciprocal: even an exact upper endpoint loses one unit
	// before the original ftol chop. [orig: interpolate_value_in_range @ 0x527EA0]
	const auto interpolate = [](int32_t &out, int32_t current, int32_t start, int32_t end,
									 int32_t low, int32_t high) {
		if (start >= end)
			return;
		if (current < start) {
			out = low;
			return;
		}
		if (current > end) {
			out = high;
			return;
		}
		constexpr double scale = 1.5259021893143654e-05;
		const double span =
				(double(high >= low ? high : low) - double(high >= low ? low : high)) * scale;
		const double range = (double(end) - start) * scale;
		const double offset = span / range * ((double(current) - start) * scale) *
				(high >= low ? -65535.0 : 65535.0);
		out = static_cast<int32_t>(uint32_t(low) - uint32_t(static_cast<int32_t>(offset)));
	};
	int32_t scratch = 0, medium_pitch = 0, cruise_pitch = 0;
	if (ratio <= p[1])
		interpolate(scratch, ratio, p[0], p[1], 0, 65535);
	else if (ratio <= p[2])
		scratch = 65535;
	else if (ratio <= p[3])
		interpolate(scratch, ratio, p[2], p[3], 65535, 0);
	const int32_t medium = scratch;
	interpolate(medium_pitch, ratio, p[4], p[5], p[6], p[7]);
	interpolate(scratch, ratio, p[8], p[9], 0, 65535);
	interpolate(cruise_pitch, ratio, p[8], p[9], p[10], p[11]);
	const auto volume = [](int32_t value) {
		return static_cast<uint16_t>((255u * uint16_t(value) + 128u) >> 8);
	};
	const uint16_t medium_volume = volume(medium), cruise_volume = volume(scratch),
				   lateral_volume = volume(std::min(lateral, 65535));
	if (medium_volume == 0 && cruise_volume == 0 && lateral_volume == 0)
		return;
	// Lanes 21/11/1 read itemDef soundLoopId[2]/[1]/[0] (+2100/+2096/+2092;
	// ItemDef.soundLoopId uint32_t[7] @0x82C, filled from res[16+i] =
	// Soundloop_1..7 by ItemDef_ResolveAllResources @0x49E7F0), i.e. the
	// Soundloop_3/2/1 profile slots, each registered with lifetime 15
	// (effect_params +16 @0x528F94). Retail sndprof.def helicopter profiles
	// author only soundloop_2 (*_ILP) and soundloop_3 (*_DLP), so the lateral
	// lane is normally empty and Soundloop_4..7 are never consulted here.
	// [orig: update_vehicle_effect_emissions @0x52919D..0x5291CE (lane 21),
	//  @0x5291ED..0x52921D (lane 11), @0x529235..0x529260 (lane 1)]
	constexpr uint16_t kRotorLifetimeTicks = 15;
	const auto emit = [&](uint8_t lane, int slot, int32_t pitch, uint16_t level) {
		const auto set = set_for_slot(profile, traits, slot);
		if (!set.empty())
			emit_emitter(world_, vehicle, lane, slot, set, pitch, level, kRotorLifetimeTicks);
	};
	emit(21, audio::kSlotSoundLoop1 + 2, cruise_pitch, cruise_volume);
	emit(11, audio::kSlotSoundLoop1 + 1, medium_pitch, medium_volume);
	emit(1, audio::kSlotSoundLoop1, ratio, lateral_volume);
}

void VehicleSystem::play_rotor_start_sound(Entity &vehicle, const VehicleTraits &traits) {
	if (!traits.player_control || vehicle.veh.part_spin.rate != 0 ||
			vehicle.veh.part_spin.speed > 10737417 || vehicle.veh.engine_sound_latched ||
			static_cast<int64_t>(to_fixed(vehicle.position.z)) + vehicle.eye_offset_z <=
					world_.env.water_z)
		return;
	Entity occupant_scratch;
	const Entity *occupant = world_.vehicles.claimant(vehicle, occupant_scratch);
	if (occupant == nullptr)
		return;
	// Retail anchors this one-shot on the pilot, whose sound pointer is passed
	// with the pilot position, while selecting the vehicle's profile slot.
	emit_profile_oneshot(
			world_, *occupant, profile_for(world_, traits), traits, audio::kSlotEngineStart);
}

// The PlayerControl claimant's detach leg: the all-zero fold, then the stop
// one-shot positioned at and anchored on the departing occupant while its eye
// clears the water plane. It writes no brain+0x318 latch, so the mover's own
// leave edge sounds a second stop on the hull the next tick.
// [orig: Entity_DetachFromVehicle @0x4355F0, fold @0x435709..0x435716, stop
//  @0x43571B..0x43573E (`push esi; lea eax,[esi+4]`)]
void VehicleSystem::play_claimant_detach_sound(Entity &vehicle, const Entity *occupant) {
	World &world = world_;
	const VehicleTraits *traits = world.vehicles.traits.get(vehicle.item_id);
    if (traits == nullptr || !traits->player_control) return;
	clear_motion_lanes(world, vehicle);
	if (occupant != nullptr &&
			static_cast<int64_t>(to_fixed(occupant->position.z)) + occupant->eye_offset_z >
					world.env.water_z)
		emit_profile_oneshot(
				world, *occupant, profile_for(world, *traits), *traits, audio::kSlotEngineStop);
}

} // namespace opennova::world
