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



} // namespace opennova::world

#endif // OPENNOVA_WORLD_VEHICLE_SOUND_H
