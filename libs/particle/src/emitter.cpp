#include "particle/emitter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "particle/particle.h"

namespace opennova::particle {

namespace {

// Numerically-stable LCG. Constants are the classic glibc `rand()` parameters.
// We don't need engine bit-exact RNG output because the simulator is not
// claiming byte parity with retail; we DO need cross-platform determinism so
// tests that pin spawn positions hold on every host.
std::uint32_t lcg_step(std::uint32_t &state) noexcept {
	state = state * 1103515245u + 12345u;
	return (state >> 16) & 0x7FFFu;
}

float lerp(float a, float b, float t) noexcept {
	return a + (b - a) * t;
}

float clampf(float v, float lo, float hi) noexcept {
	return std::max(lo, std::min(hi, v));
}

Vec3 vec3_add(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vec3_scale(Vec3 v, float s) noexcept { return {v.x * s, v.y * s, v.z * s}; }

float vec3_length(Vec3 v) noexcept {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 vec3_normalize(Vec3 v) noexcept {
	const float len = vec3_length(v);
	if (len <= 1e-6f) {
		return {0.0f, 0.0f, 1.0f};
	}
	return {v.x / len, v.y / len, v.z / len};
}

Color3 color_for_slot(const ParticleDef &def, std::uint32_t graphic, std::uint32_t slot) noexcept {
	const std::uint32_t which = slot & 3u;
	const std::array<const Color3 *, 4> colors = {
		&def.graphics[graphic].color1, &def.graphics[graphic].color2,
		&def.graphics[graphic].color3, &def.graphics[graphic].color4,
	};
	if (def.graphics[graphic].color_overrides_set) {
		return *colors[which];
	}
	const std::array<const Color3 *, 4> particle_colors = {
		&def.color1, &def.color2, &def.color3, &def.color4,
	};
	return *particle_colors[which];
}

std::uint32_t pick_color_slot(Emitter &e) noexcept {
	return emitter_rand10(e) & 3u;
}

std::uint32_t pick_graphic(Emitter &e, const ParticleDef &def) noexcept {
	std::array<std::uint32_t, 4> present{};
	std::uint32_t count = 0;
	for (std::uint32_t i = 0; i < def.graphics.size(); ++i) {
		if (def.graphics[i].present) {
			present[count++] = i;
		}
	}
	if (count == 0) {
		return 0;
	}
	// Engine: `788 * (rand() % graphic_count)` — pick uniform random graphic.
	return present[emitter_rand10(e) % count];
}

void apply_emission_shape(Emitter &e, const ParticleDef &def, Particle &p) noexcept {
	const EmitShape shape = static_cast<EmitShape>(def.emit_shape);
	switch (shape) {
		case EmitShape::Point: {
			// All particles spawn at emitter position with zero velocity.
			break;
		}
		case EmitShape::Box: {
			// Engine case 1: axis-aligned random ±emit_shape_size for ONE axis,
			// chosen randomly. We expose all three axes — close enough for the
			// portable simulator and easier to reason about in tests.
			p.position.x += emitter_rand_signed(e) * def.emit_shape_size.x;
			p.position.y += emitter_rand_signed(e) * def.emit_shape_size.y;
			p.position.z += emitter_rand_signed(e) * def.emit_shape_size.z;
			break;
		}
		case EmitShape::Sphere: {
			// Engine case 2: spherical — random direction × scaled speed.
			const float ux = emitter_rand_signed(e);
			const float uy = emitter_rand_signed(e);
			const float uz = emitter_rand_signed(e);
			const Vec3 dir = vec3_normalize({ux, uy, uz});
			const float speed = def.speed + emitter_rand_unit(e) * def.speed_adj;
			p.velocity = vec3_scale(dir, speed);
			break;
		}
		case EmitShape::Cone: {
			// Engine case 3: cone around emitter.forward, half-angle from
			// emit_shape_size (interpreted as YPR offsets in degrees).
			const Vec3 base = vec3_normalize(e.forward);
			const float yaw_off   = emitter_rand_signed(e) * def.emit_shape_size.x * 0.0174533f;
			const float pitch_off = emitter_rand_signed(e) * def.emit_shape_size.y * 0.0174533f;
			const float cy = std::cos(yaw_off), sy = std::sin(yaw_off);
			const float cp = std::cos(pitch_off), sp = std::sin(pitch_off);
			Vec3 dir{
				base.x * cy * cp + sy,
				base.y * cp + sp,
				base.z * cy * cp,
			};
			dir = vec3_normalize(dir);
			const float speed = def.speed + emitter_rand_unit(e) * def.speed_adj;
			p.velocity = vec3_scale(dir, speed);
			break;
		}
	}
}

void integrate_particle(Particle &p, const ParticleDef &def, float dt) noexcept {
	// CParticleEmitter_UpdateParticles @ 0x5e6980 — Euler step, then drag,
	// then gravity. Engine adds gravity to vel.y; the sign convention depends
	// on whether the load step negated `def.gravity`. We treat positive
	// `def.gravity` as a downward pull on +y-up: vel.y -= g * dt.
	p.position = vec3_add(p.position, vec3_scale(p.velocity, dt));

	// Gravity: per-axis mask × scalar magnitude. Engine field `gravity_mask`
	// modulates which axes feel gravity (e.g. {1,1,1} = full pull on all axes,
	// rare; typical authoring is {1,1,1} but the *direction* comes from the
	// scalar.) We keep the simple +y model for the portable simulator.
	p.velocity.y -= def.gravity * def.gravity_mask.y * dt;

	// Drag: exponential decay (1 - drag*dt) per axis. Engine writes
	// `vel -= drag*dt * vel`; same form, clamped to non-negative coefficient.
	const float drag_coef = clampf(def.drag * dt, 0.0f, 1.0f);
	p.velocity.x -= p.velocity.x * drag_coef;
	p.velocity.y -= p.velocity.y * drag_coef;
	p.velocity.z -= p.velocity.z * drag_coef;

	// Scale grows from 0 toward 1 over the particle's lifetime
	// (engine: `*(extra+48) += *(extra+52) * dt` with scale_velocity = 1/age).
	p.scale += p.scale_velocity * dt;
	if (p.scale > 1.0f) p.scale = 1.0f;

	// Rotation accumulates at the per-particle rate.
	p.rotation += p.rotation_rate * dt;

	// Age decrements unless the def has the NEVERAGE flag (bit 0x04 in engine,
	// our particle_flag::NeverAge constant).
	if ((def.flags & particle_flag::NeverAge) == 0) {
		p.age -= dt;
	}
}

void emit_one_internal(Emitter &e, const ParticleDef &def) noexcept {
	if (e.particles.size() >= e.max_particles) {
		return;
	}
	Particle p{};
	p.position = e.position;
	p.lifetime = std::max(def.age + emitter_rand_unit(e) * def.age_adj, 1e-3f);
	p.age = p.lifetime;
	p.scale = 0.0f;
	p.scale_velocity = 1.0f / p.lifetime;
	p.rotation = (def.yaw_rot + emitter_rand_unit(e) * def.yaw_rot_adj) * 0.0174533f;
	p.rotation_rate = (def.roll_rot + emitter_rand_unit(e) * def.roll_rot_adj) * 0.0174533f;
	p.alpha = static_cast<std::uint8_t>(clampf(def.alpha * 255.0f, 0.0f, 255.0f));
	p.position.y += def.y_offset;
	p.position.z += def.z_offset;
	p.color_slot = static_cast<std::uint8_t>(pick_color_slot(e));
	p.graphic_layer = static_cast<std::uint8_t>(pick_graphic(e, def));
	p.color = color_for_slot(def, p.graphic_layer, p.color_slot);
	p.serial = e.next_serial++;
	apply_emission_shape(e, def, p);
	e.particles.push_back(p);
}

void expire_dead(Emitter &e) noexcept {
	e.particles.erase(
			std::remove_if(e.particles.begin(), e.particles.end(),
					[](const Particle &p) { return p.age <= 0.0f; }),
			e.particles.end());
}

} // namespace

std::uint32_t emitter_rand10(Emitter &e) noexcept {
	return lcg_step(e.rng_state) & 0x3FFu;
}

float emitter_rand_unit(Emitter &e) noexcept {
	return static_cast<float>(emitter_rand10(e)) / 1024.0f;
}

float emitter_rand_signed(Emitter &e) noexcept {
	return emitter_rand_unit(e) * 2.0f - 1.0f;
}

void emitter_init(Emitter &e, const ParticleDef *def, Vec3 pos, std::uint32_t seed) {
	e.def = def;
	e.position = pos;
	e.prev_position = pos;
	e.forward = {0.0f, 0.0f, 1.0f};
	e.particles.clear();
	e.emit_accumulator = 0.0f;
	e.emit_delay_remaining = def != nullptr ? std::max(def->emit_delay, 0.0f) : 0.0f;
	e.emit_dur_remaining = def != nullptr ? std::max(def->emit_dur, 0.0f) : 0.0f;
	e.age = 0.0f;
	e.next_serial = 0;
	e.active = def != nullptr;
	e.finite = def != nullptr ? (def->flags & particle_flag::ForeverEmit) == 0 : true;
	// Avoid seed=0 producing a zero-bound LCG for the first few values.
	e.rng_state = seed != 0 ? seed : 0x9E3779B9u;
}

bool emitter_spawn_one(Emitter &e) {
	if (e.def == nullptr || e.particles.size() >= e.max_particles) {
		return false;
	}
	emit_one_internal(e, *e.def);
	return true;
}

void emitter_advance(Emitter &e, float dt) {
	if (!e.active || e.def == nullptr || dt <= 0.0f) {
		return;
	}
	const ParticleDef &def = *e.def;
	e.age += dt;
	e.prev_position = e.position;

	// Honour emit_delay before any spawning starts.
	if (e.emit_delay_remaining > 0.0f) {
		e.emit_delay_remaining -= dt;
		if (e.emit_delay_remaining > 0.0f) {
			// Still in the warm-up: integrate existing particles only.
			for (Particle &p : e.particles) {
				integrate_particle(p, def, dt);
			}
			expire_dead(e);
			return;
		}
		dt += e.emit_delay_remaining; // consume any sub-step overrun
	}

	// Emission. emit_rate is particles/sec; emit_burst is particles spawned
	// per emission tick. emit_dur counts down (unless FOREVEREMIT flag is set).
	const bool can_emit = e.finite ? e.emit_dur_remaining > 0.0f : true;
	if (can_emit && def.emit_rate > 0.0f) {
		const float interval = 1.0f / std::max(def.emit_rate, 1e-3f);
		e.emit_accumulator += dt;
		while (e.emit_accumulator >= interval) {
			e.emit_accumulator -= interval;
			const int burst = std::max(def.emit_burst, 1);
			for (int b = 0; b < burst; ++b) {
				emit_one_internal(e, def);
			}
			if (e.finite) {
				e.emit_dur_remaining -= interval;
				if (e.emit_dur_remaining <= 0.0f) {
					break;
				}
			}
		}
	}
	if (e.finite && e.emit_dur_remaining < 0.0f) {
		e.emit_dur_remaining = 0.0f;
	}

	// Physics integration + aging.
	for (Particle &p : e.particles) {
		integrate_particle(p, def, dt);
	}
	expire_dead(e);

	// Emitter is "done" when finite duration is exhausted AND no live particles
	// remain — caller can flip e.active off based on this if they want pooling.
}

} // namespace opennova::particle
