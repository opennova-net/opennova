// Ground-vehicle sound-profile consumer.
//
// This module is the portable producer for retail's generic entity-attached
// sound-emitter seam. It owns authored slot selection and fixed-point gain/pitch
// math; callers and the Godot presenter only see SoundEmitterEvent rows.
// [orig: Entity_ProcessMovementSoundEffects @0x5294a0]
#ifndef OPENNOVA_WORLD_VEHICLE_SOUND_H
#define OPENNOVA_WORLD_VEHICLE_SOUND_H

namespace opennova::world {

class World;
struct Entity;
struct VehicleTraits;

// Refresh the active idle/forward/reverse emitter lanes from the vehicle's final
// motor state for this physics tick. PlayerControl vehicles key engine-running
// state on primary_occupant, identically for NPC and player claimants. A hull
// collision clears both motion lanes and forces a full idle refresh; wreck/all-zero
// clears motion without refreshing idle.
void update_ground_vehicle_sound(World &world, Entity &vehicle,
                                 const VehicleTraits &traits, bool wrecked,
                                 bool collided);

// Claimant-only detach/stale-claim leg: clear the motion lanes and fire the
// authored engine-stop one-shot when strictly above the mission water plane. The
// idle lane is not refreshed and expires from its 30-tick keep-alive, matching
// the original zero-argument movement-sound call.
void stop_ground_vehicle_sound(World &world, Entity &vehicle);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_VEHICLE_SOUND_H
