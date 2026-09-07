#include <runtime/world/vehicle_system.h>
#include <runtime/world/vehicle_sound.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

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
constexpr uint16_t kEmitterLifetimeTicks = 30;
constexpr int32_t kUnityQ16 = 0x10000;
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
    if (!vehicle.primary_occupant.valid()) return false;
    const Entity *claimant = world.registry.get(vehicle.primary_occupant);
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

void emit_emitter(World &world, Entity &vehicle, uint8_t lane, int slot,
                  const std::string &set_name, int32_t pitch_q16,
                  uint16_t volume_q8_8) {
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
    ev.lifetime_ticks = kEmitterLifetimeTicks;
    ev.pitch_q16 = pitch_q16;
    ev.volume_q8_8 = volume_q8_8;
    ev.set_name = set_name;
    if (pitch_q16 != 0 && volume_q8_8 != 0) {
        vehicle.veh.sound_anchor_until_tick =
                ev.emitted_tick + kEmitterLifetimeTicks;
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

void VehicleSystem::update_ground_sound(Entity &vehicle, const VehicleTraits &traits, bool wrecked, bool collided) {
    World &world = world_;
    const audio::SoundProfile *profile = profile_for(world, traits);

    // The PlayerControl ground caller skips the movement-sound function entirely
    // without entity+368. This is why an NPC claimant runs the engine just like a
    // player, while a passenger or surviving non-claimant controller does not.
    // [orig: caller gate @0x48d181..0x48d1c7]
    if (traits.player_control && !has_live_primary_claimant(world, vehicle)) {
        const uint32_t now = producer_tick(world);
        const uint32_t remaining =
                vehicle.veh.sound_anchor_until_tick - now;
        if (vehicle.veh.sound_anchor_until_tick != 0 &&
            remaining <= kEmitterLifetimeTicks) {
            // Retail slots retain the entity-position pointer while an
            // unrefreshed lane expires. This source-only intent keeps that
            // residual lane spatially attached without renewing its lifetime.
            emit_source_anchor(world, vehicle);
        }
        return;
    }

    const Entity *claimant = world.registry.get(vehicle.primary_occupant);
    const bool submerged_driver = claimant != nullptr && claimant->player_class != 0 &&
            watercraft_driver_submerged(world, *claimant);
    // Retail zeroes all four sound arguments when a player claimant's eye is
    // submerged, before selecting the stop/collision/motion branch.
    // [orig: Entity_ProcessMovementSoundEffects @0x5294A0, gate @0x5294A8..0x5294D8]
    const bool stopped = wrecked || ((vehicle.flags | vehicle.engine_flags) & 2u) != 0 ||
            submerged_driver;
    // The family's max drive speed: boats author waterSpeed (+0x8EC) and no
    // player_speed — the ground field would zero the denominator and pin every
    // boat in the stop lane.
    int32_t max_speed = traits.family == VehicleFamily::Watercraft
            ? traits.water_speed : traits.player_speed;
    // The ground caller falls back to brain speed A, then the command.
    // [orig: Entity_UpdateVehiclePhysics @0x48AF00, caller @0x48D196..0x48D1B8]
    if (max_speed == 0 && traits.family != VehicleFamily::Watercraft) {
        if (const AiEntity *ai = world.ai.for_handle(vehicle.handle))
            max_speed = ai->brain.f[AiBrain::kSpeedA];
        if (max_speed == 0) max_speed = vehicle.veh.cmd_speed;
    }
    const int64_t denominator = magnitude_i32(max_speed);
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
        emit_emitter(world, vehicle, kIdleLane, audio::kSlotSoundLoop1,
                     set_for_slot(profile, traits, audio::kSlotSoundLoop1),
                     kUnityQ16, kFullVolumeQ8_8);
    } else {
        int64_t speed = magnitude_i32(vehicle.veh.speed);
        if (speed == 0 && vehicle.saved_live_valid) {
            const double dx = static_cast<double>(vehicle.saved_live_pos[0]) -
                    to_fixed(vehicle.position.x);
            const double dy = static_cast<double>(vehicle.saved_live_pos[1]) -
                    to_fixed(vehicle.position.y);
            const double dz = static_cast<double>(vehicle.saved_live_pos[2]) -
                    to_fixed(vehicle.position.z);
            // fild/square/sum/fsqrt, flt_7C19E0 clamp, then truncation.
            // Direction still comes from the original signed speed argument.
            // [orig: Entity_ProcessMovementSoundEffects @0x5294A0, displacement @0x52953B..0x52959D]
            speed = static_cast<int64_t>(std::min(std::sqrt(dx*dx + dy*dy + dz*dz),
                                                2147418112.0));
        }
        if (speed < 256) speed = 0; // sub-1/256-unit motion is treated as stationary

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
                emit_emitter(world, vehicle, kIdleLane, audio::kSlotSoundLoop1,
                             set_for_slot(profile, traits, audio::kSlotSoundLoop1),
                             kUnityQ16, static_cast<uint16_t>(volume));
            }
        }
    }

    // The movement direction latch plays enginereverse on both direction edges.
    // Entering reverse waits for command AND actual speed to be negative; leaving
    // only waits for a positive command.
    // [orig: vehicleData+0x318 bit2 @0x48d1d1..0x48d222]
    if (!vehicle.primary_occupant.valid()) return;
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

// The claimant and lights edge lanes follow the continuous movement fold.
// [orig: Entity_UpdateVehiclePhysics @0x48AF00, tail @0x48D34E..0x48D429;
// Entity_UpdateWatercraftPhysics @0x48D480, edge @0x48DAD1..0x48DB6B]
void VehicleSystem::update_engine_sound(Entity &vehicle, const VehicleTraits &traits) {
    if (vehicle_family_uses_direct_air_mover(traits.family)) return;
    const auto *profile = profile_for(world_, traits);
    const bool lights = (vehicle.flags & 0x80u) != 0;
    if (traits.family != VehicleFamily::Watercraft) {
        if (lights && !vehicle.veh.light_sound_latched)
            emit_profile_oneshot(world_, vehicle, profile, traits, audio::kSlotAudio1);
        vehicle.veh.light_sound_latched = lights;
    }
    if (!traits.player_control) return;
    const Entity *occupant = world_.registry.get(vehicle.primary_occupant);
    if (occupant != nullptr) {
        if (!vehicle.veh.engine_sound_latched) {
            vehicle.veh.engine_sound_latched = true;
            if (static_cast<int64_t>(to_fixed(occupant->position.z)) +
                    occupant->eye_offset_z > world_.env.water_z)
                emit_profile_oneshot(world_, vehicle, profile, traits, audio::kSlotEngineStart);
        }
    } else if (vehicle.veh.engine_sound_latched) {
        stop_ground_sound(vehicle, 0x18000);
    }
}

// The helo start is a cold-rotor edge, not the generic ground claimant edge.
// [orig: Entity_UpdateHeloRotorSpin @0x48FA70, start @0x48FAA1..0x48FB0C]
void VehicleSystem::play_rotor_start_sound(Entity &vehicle, const VehicleTraits &traits) {
    if (!traits.player_control || vehicle.veh.part_spin.rate != 0 ||
        vehicle.veh.part_spin.speed > 10737417 || vehicle.veh.engine_sound_latched ||
        static_cast<int64_t>(to_fixed(vehicle.position.z)) + vehicle.eye_offset_z <=
                world_.env.water_z)
        return;
    const Entity *occupant = world_.registry.get(vehicle.primary_occupant);
    if (occupant == nullptr) return;
    // Retail anchors this one-shot on the pilot, whose sound pointer is passed
    // with the pilot position, while selecting the vehicle's profile slot.
    emit_profile_oneshot(world_, *occupant, profile_for(world_, traits), traits,
                         audio::kSlotEngineStart);
}

void VehicleSystem::stop_ground_sound(Entity &vehicle, int32_t water_clearance_q16) {
    World &world = world_;
    const VehicleTraits *traits = world.vehicles.traits.get(vehicle.item_id);
    if (traits == nullptr || !traits->player_control) return;
    const audio::SoundProfile *profile = profile_for(world, *traits);
    emit_emitter(world, vehicle, kForwardLane, audio::kSlotSoundLoop1 + 1,
                 {}, 0, 0);
    emit_emitter(world, vehicle, kReverseLane, audio::kSlotSoundLoop1 + 2,
                 {}, 0, 0);
    vehicle.veh.reverse_sound_latched = false;
    vehicle.veh.engine_sound_latched = false;
    if (world.env.water_z == 0 ||
        static_cast<int64_t>(to_fixed(vehicle.position.z)) + water_clearance_q16 > world.env.water_z) {
        emit_profile_oneshot(world, vehicle, profile, *traits,
                             audio::kSlotEngineStop);
    }
}

} // namespace opennova::world
