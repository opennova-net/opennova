#include "rotor_wash.h"
#include "vehicle_motor_detail.h"
#include "world.h"
#include <base/io/rotating_prng.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/surface_type_map.h>

namespace opennova::world {
using namespace detail;
namespace {
int32_t radial(const int32_t delta[3], bool rounded) {
	const int32_t square =
			io::bam_add(q16_mul_rhu(delta[0], delta[0]), q16_mul_rhu(delta[1], delta[1]));
	if (square < 0)
		return 0; // x87 indefinite cannot name an in-pool radius.
	const double root = std::sqrt(double(square));
	return bam_shl_wrap(int32_t(rounded ? std::nearbyint(root) : root), 8);
}
int32_t projection(const CollisionMatrix &frame, const int32_t p[3], int32_t delta[3]) {
	int32_t axis[3], distance = 0;
	for (int k = 0; k < 3; ++k) {
		axis[k] = io::bam_sub(0, frame.m[8 + k]) >> 6;
		distance = io::bam_add(distance, q16_mul_rhu(frame.m[4 * k + 3], axis[k]));
	}
	for (int k = 2; k >= 0; --k)
		distance = io::bam_sub(distance, q16_mul_rhu(p[k], axis[k]));
	for (int k = 0; k < 3; ++k)
		delta[k] =
				io::bam_sub(p[k], io::bam_sub(frame.m[4 * k + 3], q16_mul_rhu(distance, axis[k])));
	return distance;
}
void radial_direction(const int32_t delta[3], int32_t radius, int32_t dir[3]) {
	const int32_t inverse = radius > 256 ? int32_t(0x100000000LL / radius) : 0;
	for (int k = 0; k < 3; ++k)
		dir[k] = radius > 256 ? q16_mul_rhu(inverse, delta[k]) : bam_shl_wrap(delta[k], 4);
}
} // namespace

// [orig: Entity_UpdateHeloRotorSpin @0x48FA70]
void RotorWashSystem::update(Entity &e, const VehicleTraits &traits) {
	auto &handle = e.veh.rotor_wash_handle;
	if (!handle && ((e.flags | e.engine_flags) & 4u) == 0) {
		std::size_t slot = 0;
		while (slot < count_ && zones_[slot].owner.valid())
			++slot;
		if (slot == count_)
			count_ = std::min<std::size_t>(127, count_ + 1);
		zones_[slot] = {};
		zones_[slot].owner = e.handle;
		zones_[slot].tick = world_.logic_tick;
		handle = uint16_t(slot | 0x8000u);
	}
	const auto slot = std::size_t(handle & 0x7fffu);
	if (!handle || slot >= count_ || zones_[slot].owner != e.handle)
		return;
	auto &z = zones_[slot];
	z.frame = entity_placement_matrix(e);
	z.intensity = int32_t(int64_t(e.veh.part_spin.speed) * 65536 / 214748352);
	const AiEntity *ai = world_.ai.for_handle(e.handle);
	const int32_t maximum = traits.player_speed ? traits.player_speed : ai ? ai->brain.f[49] : 0;
	const int32_t speed =
			int32_t(std::min(2147418100.0, std::hypot(double(e.veh.vel_x), double(e.veh.vel_y))));
	z.motion = maximum ? int32_t(int64_t(std::min(speed, maximum)) * 65536 / maximum) : 65536;
	for (int k = 0; k < 3; ++k) {
		z.lower[k] = io::bam_sub(z.frame.m[4 * k + 3], io::bam_add(z.radius, z.extent));
		z.upper[k] = io::bam_add(z.frame.m[4 * k + 3], io::bam_add(z.radius, z.extent));
	}
}

// [orig: Entity_Respawn @0x45FF40; focal-wind slot owner tag @0x5CAF40]
void RotorWashSystem::release(Entity &e) {
	const auto slot = std::size_t(e.veh.rotor_wash_handle & 0x7fffu);
	if (e.veh.rotor_wash_handle && slot < zones_.size() && zones_[slot].owner == e.handle)
		zones_[slot] = {};
	e.veh.rotor_wash_handle = 0;
}
void RotorWashSystem::clear() {
	water_wakes_.clear();
	zones_ = {};
	count_ = 0;
}
std::size_t RotorWashSystem::active_count() const {
	return std::count_if(
			zones_.begin(), zones_.begin() + count_, [](const Zone &z) { return z.owner.valid(); });
}
bool RotorWashSystem::contains(const Zone &z, const int32_t p[3]) const {
	if (!z.owner.valid() || z.intensity < 0x2000)
		return false;
	for (int k = 0; k < 3; ++k)
		if (p[k] < z.lower[k] || p[k] > z.upper[k])
			return false;
	return true;
}
int32_t RotorWashSystem::ground(int32_t x, int32_t y) const {
	return world_.tables.terrain
			? to_fixed(terrain::height_field_height_world_bilinear(
					  *world_.tables.terrain, float(from_fixed(x)), -float(from_fixed(y))))
			: 0;
}

// The nearest spherical candidate inside its AABB; the pool's broader 45-unit
// radius also feeds the foliage sampler. [orig: Terrain_FindNearestAmbientSoundZone @0x5CBCD0]
uint16_t RotorWashSystem::nearest(const int32_t p[3]) const {
	uint32_t best = UINT32_MAX;
	uint16_t result = 0;
	for (std::size_t i = 0; i < count_; ++i) {
		const auto &z = zones_[i];
		if (!contains(z, p))
			continue;
		int32_t square = 0;
		for (int k = 0; k < 3; ++k) {
			const int32_t d = io::bam_sub(z.frame.m[4 * k + 3], p[k]);
			square = io::bam_add(square, int32_t(int64_t(d) * d >> 22));
		}
		if (square < 0)
			continue;
		const uint32_t distance = uint32_t(int32_t(std::sqrt(double(square)))) << 11;
		if (distance < uint32_t(z.radius) && distance < best) {
			best = distance;
			result = uint16_t(i | 0x8000u);
		}
	}
	return result;
}

// [orig: WindZone_ApplyVortexForce @0x5CB8A0]
void RotorWashSystem::vortex(const Zone &z, int32_t p[3], int32_t v[3]) const {
	if (!contains(z, p))
		return;
	int32_t delta[3], dir[3];
	const int32_t axial = projection(z.frame, p, delta);
	const int32_t radius = radial(delta, false);
	radial_direction(delta, radius, dir);
	const int32_t floor = io::bam_add(std::max(ground(p[0], p[1]), world_.env.water_z), 114688);
	if (radius >= z.inner) {
		if (radius < z.outer) {
			for (int k = 0; k < 2; ++k)
				v[k] = io::bam_sub(io::bam_sub(v[k], v[k] >> 4), dir[k] >> 7);
			if (v[2] < 2048)
				v[2] = io::bam_add(v[2], 128);
		}
	} else {
		if (axial < (z.extent >> 1)) {
			const int32_t ratio = bam_shl_wrap(io::bam_abs(axial), 8) / z.extent;
			const int32_t suction = ratio > 255 ? 0 : q16_mul_rhu(z.intensity, 8 * (256 - ratio));
			v[2] = io::bam_sub(v[2], suction);
		}
		if (v[2] > 0)
			v[2] = io::bam_sub(v[2], 1024);
		p[2] = io::bam_sub(p[2], 4096);
		const bool above = io::bam_add(p[2], v[2]) > floor;
		if (!above) {
			p[2] = floor;
			v[2] = 0;
		}
		for (int k = 0; k < 2; ++k)
			v[k] = io::bam_add(v[k], q16_mul_rhu(z.intensity, dir[k]) >> (above ? 8 : 6));
	}
	if (p[2] <= floor) {
		p[2] = floor;
		if (v[2] < 0)
			v[2] = 0;
	}
}

// [orig: Terrain_CalcSectorRepulsionForce @0x5CBF50]
void RotorWashSystem::repel(const Zone &z, int32_t p[3], int32_t v[3]) const {
	if (!z.owner.valid() || z.intensity < 0x2000)
		return;
	int32_t delta[3];
	for (int k = 0; k < 3; ++k)
		delta[k] = io::bam_sub(p[k], z.frame.m[4 * k + 3]);
	int32_t radius = radial(delta, true), horizontal = 0;
	if (io::bam_abs(delta[2]) <= z.radius && radius < z.radius) {
		const int32_t vertical = 65536 - int32_t(int64_t(io::bam_abs(delta[2])) * 65536 / z.radius);
		horizontal = vertical;
		v[2] = io::bam_sub(0, std::max(0, vertical)) >> 2;
		if (radius > z.inner) {
			radius = std::min(radius, z.radius);
			const int32_t scaled = bam_shl_wrap(int32_t(int64_t(delta[2]) * 65536 / radius), 2);
			horizontal = io::bam_sub(
					horizontal, q16_mul_rhu(int32_t(int64_t(radius) * 65536 / z.radius), scaled));
		}
	}
	const int32_t floor = std::max(io::bam_add(ground(p[0], p[1]), 114688), world_.env.water_z);
	if (io::bam_add(v[2], p[2]) <= floor) {
		p[2] = floor;
		v[2] = 0;
	}
	if (!horizontal)
		return;
	int32_t dir[3] = {};
	if (radius > 256) {
		const int64_t d[3] = { delta[0], delta[1], delta[2] };
		q16_normalize(d, dir);
	}
	for (int k = 0; k < 2; ++k)
		v[k] = q16_mul_rhu(horizontal, dir[k]) >> 4;
}

// [orig: CParticleEmitter_UpdateAllParticles @0x5F3BE0]
void RotorWashSystem::apply(particle::Particle &particle, std::size_t index, bool repulsion) const {
	int32_t p[3] = { to_fixed(particle.position.x), to_fixed(-particle.position.z),
		to_fixed(particle.position.y) };
	int32_t v[3] = { to_fixed(particle.velocity.x), to_fixed(-particle.velocity.z),
		to_fixed(particle.velocity.y) };
	if (((uint32_t(particle.curve_phase) + uint32_t(index)) & 15u) == 15u)
		particle.force_zone = nearest(p);
	const std::size_t slot = particle.force_zone & 0x7fffu;
	if (!particle.force_zone || slot >= count_)
		return;
	if (repulsion)
		repel(zones_[slot], p, v);
	else
		vortex(zones_[slot], p, v);
	particle.velocity = { float(from_fixed(v[0])), float(from_fixed(v[2])),
		-float(from_fixed(v[1])) };
}

// [orig: find_nearest_force_zone @0x5CB5B0]
bool RotorWashSystem::sample_sway(const int32_t p[3], int32_t &magnitude, int32_t dir[3]) const {
	for (std::size_t i = 0; i < count_; ++i) {
		const auto &z = zones_[i];
		if (!contains(z, p))
			continue;
		int32_t delta[3];
		projection(z.frame, p, delta);
		const int32_t radius = radial(delta, true);
		if (radius >= z.radius)
			continue;
		magnitude = int32_t(65536.0 - double(radius) * 65536.0 / z.radius);
		radial_direction(delta, radius, dir);
		return true;
	}
	return false;
}

// The Sway renderer deforms its second bone around its authored pivot. All
// five waves share the entity-position/tick seed and the focal-wind magnitude.
// [orig: BoneCallback_Sway_World @0x4E2B10]
FocalSwayPose RotorWashSystem::sway_pose(const Entity &e) const {
	FocalSwayPose result;
	if (!e.render_sway)
		return result;
	const int32_t p[3] = { to_fixed(e.position.x), to_fixed(e.position.y),
		io::bam_add(to_fixed(e.position.z), 0x20000) };
	int32_t magnitude, direction[3];
	if (!sample_sway(p, magnitude, direction))
		return result;
	const uint32_t seed = world_.logic_tick + uint32_t(p[0] >> 14) + uint32_t(p[1] >> 14) +
			uint32_t(to_fixed(e.position.z) >> 14);
	auto wave = [&](uint32_t rate, int32_t weight, int shift) {
		const int32_t sine = sin22_of_bam_x87(int32_t(seed * rate));
		return q16_mul_rhu(weight, sine >> shift);
	};
	const int32_t a = wave(234881024u, std::min(magnitude, 49152), 11);
	const int32_t b = wave(184549376u, std::clamp(magnitude - 12288, 0, 16384), 10);
	const int32_t c = wave(134217728u, std::clamp(magnitude - 24576, 0, 16384), 10);
	const int32_t d = wave(83886080u, std::clamp(magnitude - 36864, 0, 16384), 9);
	const int32_t base = wave(33554432u, magnitude, 12);
	constexpr float radians = 9.58738019107841e-05f; // flt_7C7988
	const float x = float(b >> 2) * radians;
	const float y = float((a + d + base) >> 2) * radians;
	const float z = float(c >> 2) * radians;
	const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
	const float cz = std::cos(z), sz = std::sin(z);
	// Column-vector equivalent of retail's row-vector Ry * Rx * Rz.
	const float matrix[9] = { cz * cy - sz * sx * sy, -sz * cx, cz * sy + sz * sx * cy,
		sz * cy + cz * sx * sy, cz * cx, sz * sy - cz * sx * cy, -cx * sy, sx, cx * cy };
	std::copy_n(matrix, 9, result.basis);
	const float weight = float(magnitude) * 4.57763671875e-05f; // flt_7CD430
	result.offset = { float(from_fixed(direction[0])) * weight,
		float(from_fixed(direction[1])) * weight, float(from_fixed(direction[2])) * weight };
	result.active = true;
	return result;
}

// Rotor wash chooses the authored ground material, casts down the rotor axis,
// and spawns the effect's active children at each hit.
// [orig: WeatherParticle_UpdateAllEmitters @0x5CB220; sub_5F6C10 @0x5F6C10]
void RotorWashSystem::tick() {
	water_wakes_.tick();
	for (std::size_t i = 0; i < count_; ++i) {
		auto &z = zones_[i];
		if (!z.owner.valid())
			continue;
		Entity *owner = world_.registry.get(z.owner);
		if (!owner || ((owner->flags | owner->engine_flags) & 2u)) {
			if (owner)
				release(*owner);
			else
				z = {};
			continue;
		}
		if (!z.intensity || z.tick == world_.logic_tick)
			continue;
		int32_t intensity = z.motion < z.intensity ? z.intensity : z.motion - z.intensity;
		int32_t elapsed = int32_t(world_.logic_tick - z.tick);
		z.tick = world_.logic_tick;
		if (elapsed > 15)
			z.intensity /= 2;
		elapsed = std::min(5, elapsed);
		if (!world_.out.fire_sounds.listener_valid())
			continue;
		const auto &camera = world_.out.fire_sounds.listener();
		const int32_t dx = io::bam_abs(io::bam_sub(z.frame.m[3], to_fixed(camera.x)));
		const int32_t dy = io::bam_abs(io::bam_sub(z.frame.m[7], to_fixed(camera.y)));
		const int32_t distance = std::max(dx, dy) + (std::min(dx, dy) >> 1);
		if (distance > 0x2200000)
			continue;
		if (distance >= 0x200000)
			intensity -= (distance - 0x200000) >> 9;
		const bool water = std::max(ground(z.frame.m[3], z.frame.m[7]), world_.env.water_z) ==
				world_.env.water_z;
		if ((intensity >= z.dust && z.motion) || water)
			z.dust = intensity;
		else {
			intensity = z.dust;
			z.dust = std::max(0, z.dust - (elapsed << 6));
		}
		const int particles = (elapsed * (intensity + (world_.next_prng16_c() & 0x7fff))) >> 15;
		int32_t angle = int32_t(((world_.logic_tick << 16) + world_.next_prng16_c()) << 12);
		for (int n = 0; n < particles; ++n, angle = io::bam_add(angle, 1193046400)) {
			const int32_t radius = q16_mul_rhu(z.inner - 393216, world_.next_prng16_c()) + 327680;
			int32_t p[3] = { int32_t(int64_t(radius) * cos22_of_bam_x87(angle) >> 22),
				int32_t(int64_t(radius) * sin22_of_bam_x87(angle) >> 22), 0 };
			z.frame.transform_point(p, p);
			int32_t step[3];
			for (int k = 0; k < 3; ++k)
				step[k] = io::bam_sub(0, z.frame.m[8 + k]) >> 6;
			const int steps =
					(q16_mul_rhu(z.extent >> 1, world_.next_prng16_c()) + (z.extent >> 1)) >> 16;
			for (int nstep = 0; nstep < steps; ++nstep) {
				for (int k = 0; k < 3; ++k)
					p[k] = io::bam_add(p[k], step[k]);
				const int32_t floor = std::max(ground(p[0], p[1]), world_.env.water_z);
				if (floor <= p[2])
					continue;
				p[0] = io::bam_sub(p[0], step[0]);
				p[1] = io::bam_sub(p[1], step[1]);
				p[2] = io::bam_add(p[2], 114688 - step[2]);
				const bool water_hit = floor == world_.env.water_z;
				const int material =
						terrain::surface_type_at_fixed(world_.tables.surface_map, p[0], p[1]);
				const char *effect = water_hit ? "Effect_RwWater"
						: material == 2		   ? "Effect_RwGrass"
						: material == 3		   ? "Effect_RwSnow"
						: material == 5		   ? "Effect_RwSand"
											   : "Effect_RwDust";
				VehicleEffectEvent event;
				event.effect = effect;
				event.position = { float(from_fixed(p[0])), float(from_fixed(p[1])),
					float(from_fixed(p[2])) };
				event.direction = { 0, 0, 1 };
				event.source_tick = world_.logic_tick;
				event.force_zone = uint16_t(i | 0x8000u);
				world_.out.vehicle_effects.push_back(std::move(event));
				break;
			}
		}
		// The water wake projects the rotor origin down its axis every eighth
		// tick, then caps its height falloff by the rotor intensity.
		// [orig: WeatherParticle_UpdateAllEmitters @ 0x5CB220;
		//  sub_6108E0 @ 0x6108E0]
		if (world_.env.water_z != 0 && (world_.logic_tick & 7u) == 0) {
			const int32_t height = io::bam_sub(z.frame.m[11], world_.env.water_z);
			const float opacity = std::min(1.0f,
					std::min(
							float(1.0 - double(height) / z.extent), float(z.intensity) / 65536.0f));
			if (opacity > 0) {
				const int32_t x = io::bam_add(
						z.frame.m[3], q16_mul_rhu(height, io::bam_sub(0, z.frame.m[8]) >> 6));
				const int32_t y = io::bam_add(
						z.frame.m[7], q16_mul_rhu(height, io::bam_sub(0, z.frame.m[9]) >> 6));
				water_wakes_.add(x, y, opacity);
			}
		}
	}
}
} // namespace opennova::world
