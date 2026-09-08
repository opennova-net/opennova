#pragma once

#include "collision.h"
#include "entity.h"
#include <runtime/particle/emitter.h>
#include <array>
#include <runtime/renderer/water_wake_frame.h>

namespace opennova::world {
class World;
struct VehicleTraits;

// The helicopter's bounded focal-wind pool, shared by downwash particles and
// procedural foliage. A handle retains its slot until its vehicle is released.
// [orig: terrain_overlay_alloc @0x5CAF40; sub_5CB020 @0x5CB020]
struct FocalSwayPose {
	bool active = false;
	float basis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	Vec3 offset;
};
class RotorWashSystem final : public particle::ParticleForceField {
public:
	explicit RotorWashSystem(World &world) : world_(world) {}
	void update(Entity &entity, const VehicleTraits &traits);
	void release(Entity &entity);
	void tick();
	void clear();
	void apply(particle::Particle &particle, std::size_t index, bool repulsion) const override;
	bool sample_sway(const int32_t position[3], int32_t &magnitude, int32_t direction[3]) const;
	std::size_t active_count() const;
	const renderer::WaterWakePool &water_wakes() const { return water_wakes_; }
	FocalSwayPose sway_pose(const Entity &entity) const;

private:
	struct Zone {
		EntityHandle owner;
		CollisionMatrix frame;
		int32_t lower[3] = {}, upper[3] = {};
		int32_t inner = 786432, outer = 983040, radius = 2949120, extent = 1966080;
		int32_t intensity = 0, motion = 65536, dust = 0;
		uint32_t tick = 0;
	};
	World &world_;
	renderer::WaterWakePool water_wakes_;
	std::array<Zone, 128> zones_{};
	std::size_t count_ = 0;
	bool contains(const Zone &zone, const int32_t position[3]) const;
	uint16_t nearest(const int32_t position[3]) const;
	int32_t ground(int32_t x, int32_t y) const;
	void vortex(const Zone &zone, int32_t position[3], int32_t velocity[3]) const;
	void repel(const Zone &zone, int32_t position[3], int32_t velocity[3]) const;
};
} // namespace opennova::world
