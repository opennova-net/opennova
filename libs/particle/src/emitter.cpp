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

Vec3 vec3_cross(Vec3 a, Vec3 b) noexcept {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float vec3_dot(Vec3 a, Vec3 b) noexcept {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Rodrigues' rotation formula: rotate v around (already-normalized) axis by
// `angle` radians. Engine: `init_D3DXMatrixRotationAxis(m, axis, angle)` then
// `D3DXVec3TransformCoord(out, v, m)`. This avoids materializing a 4x4
// matrix when we only need a single rotation per call.
Vec3 vec3_rotate_around_axis(Vec3 v, Vec3 axis, float angle) noexcept {
	const float c = std::cos(angle);
	const float s = std::sin(angle);
	const Vec3 cross = vec3_cross(axis, v);
	const float dot = vec3_dot(axis, v);
	return {
		v.x * c + cross.x * s + axis.x * dot * (1.0f - c),
		v.y * c + cross.y * s + axis.y * dot * (1.0f - c),
		v.z * c + cross.z * s + axis.z * dot * (1.0f - c),
	};
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

// Per-axis lerp(skip, size, rand_unit) for the annular shape range. Engine
// reads the `[skip, size]` range from def[3940..3948] / def[3928..3936] (= our
// emit_shape_size_skip / emit_shape_size). The result is the per-axis
// magnitude scaling applied to a unit direction in Sphere / Cone modes.
Vec3 annular_axis_scales(Emitter &e, const ParticleDef &def) noexcept {
	const float r0 = lerp(def.emit_shape_size_skip.x, def.emit_shape_size.x, emitter_rand_unit(e));
	const float r1 = lerp(def.emit_shape_size_skip.y, def.emit_shape_size.y, emitter_rand_unit(e));
	const float r2 = lerp(def.emit_shape_size_skip.z, def.emit_shape_size.z, emitter_rand_unit(e));
	return {r0, r1, r2};
}

void apply_emission_shape(Emitter &e, const ParticleDef &def, Particle &p) noexcept {
	// CParticleEmitter_SpawnParticle @ 0x5e7640 — switch on def.emit_shape.
	const EmitShape shape = static_cast<EmitShape>(def.emit_shape);
	switch (shape) {
		case EmitShape::Point: {
			// No shape impulse — particle spawns at emitter position with zero
			// velocity (any subsequent velocity comes from spread / orbital
			// scalars in the post-shape pass).
			break;
		}
		case EmitShape::Box: {
			// Engine case 1: pick one axis at random; assign a directed impulse
			// of `±emit_shape_size[axis]` (sign = +1 when ONEFRAME flag is set,
			// else `±1` from `rand() & 1`). Other axes get a small range
			// perturbation `rand_signed × emit_shape_size_skip[k]`. Position-
			// space, not velocity. The engine's exact form multiplies the
			// chosen axis by a deg2rad constant — a coordinate-space quirk we
			// intentionally elide; the resulting "one dominant axis, others
			// inset" geometry matches engine intent.
			const std::uint32_t axis = emitter_rand10(e) % 3u;
			const float chosen_sign =
					(def.flags & particle_flag::OneFrame) != 0 ? 1.0f :
					((emitter_rand10(e) & 1u) != 0 ? 1.0f : -1.0f);
			const float chosen_size = axis == 0 ? def.emit_shape_size.x :
					axis == 1 ? def.emit_shape_size.y : def.emit_shape_size.z;
			Vec3 offset{
				emitter_rand_signed(e) * def.emit_shape_size_skip.x,
				emitter_rand_signed(e) * def.emit_shape_size_skip.y,
				emitter_rand_signed(e) * def.emit_shape_size_skip.z,
			};
			if (axis == 0) offset.x = chosen_sign * chosen_size;
			else if (axis == 1) offset.y = chosen_sign * chosen_size;
			else offset.z = chosen_sign * chosen_size;
			p.position = vec3_add(p.position, offset);
			break;
		}
		case EmitShape::Sphere: {
			// Engine case 2: random unit direction, then per-axis annular
			// `lerp(skip, size, rand)` magnitude → velocity. Hollow ellipsoidal
			// shell with radial thickness `[skip, size]` per axis. Engine reads
			// shape size from def[3928..]; magnitude is NOT def.speed (that is
			// reserved for the WANDER move integrator).
			Vec3 unit{
				emitter_rand_signed(e),
				emitter_rand_signed(e),
				emitter_rand_signed(e),
			};
			unit = vec3_normalize(unit);
			const Vec3 r = annular_axis_scales(e, def);
			p.velocity = {unit.x * r.x, unit.y * r.y, unit.z * r.z};
			break;
		}
		case EmitShape::Cone: {
			// Engine case 3: vtable[+0x1C](this, &out_dir, dir, ...) builds a
			// random direction within the cone's spread half-angle around the
			// passed-in dir, then per-axis annular `lerp(skip, size, rand)`
			// scales it into a velocity. We approximate by building a
			// perpendicular basis off `Emitter::forward` and offsetting in
			// (right, up) by a half-angle drawn from `def.spread` degrees.
			const Vec3 forward = vec3_normalize(e.forward);
			Vec3 right_axis = std::abs(forward.x) > 0.9f ?
					Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
			Vec3 up = vec3_normalize(vec3_cross(forward, right_axis));
			Vec3 right = vec3_cross(up, forward);
			const float half_angle = def.spread * 0.0174533f;
			const float yaw = emitter_rand_signed(e) * half_angle;
			const float pitch = emitter_rand_signed(e) * half_angle;
			// Small-angle direction perturbation: forward + yaw*right + pitch*up,
			// normalized. Approximates a square (yaw,pitch)-bounded cone region
			// — close to the engine's spherical-cap sampling for typical
			// `def.spread` ≤ 30° and easier to reason about in tests.
			Vec3 cone_dir{
				forward.x + right.x * yaw + up.x * pitch,
				forward.y + right.y * yaw + up.y * pitch,
				forward.z + right.z * yaw + up.z * pitch,
			};
			cone_dir = vec3_normalize(cone_dir);
			const Vec3 r = annular_axis_scales(e, def);
			p.velocity = {cone_dir.x * r.x, cone_dir.y * r.y, cone_dir.z * r.z};
			break;
		}
	}
}

void integrate_particle(Particle &p, const ParticleDef &def, const Emitter &e, float dt) noexcept {
	// CParticleEmitter_UpdateParticles @ 0x5e6980. Two physics dispatches:
	//   move & 1 (NORMAL):    pos += vel*dt; vel.y += gravity_slot*dt; vel -= drag*dt*vel
	//   move & 2 (GRAVITATE): same translation, then a constant-magnitude
	//                         force along normalize(pos - emitter.pos),
	//                         per-axis masked AFTER the normalize.
	// The engine reads one emitter slot (+308) as both the NORMAL gravity
	// accel and the GRAVITATE force scalar; the manager seeds it in
	// CEffectEmitter_Initialize @ 0x5e6020.
	// WANDER/BUBBLE are engine-vestigial (zero xrefs); ORBIT rides below.
	p.position = vec3_add(p.position, vec3_scale(p.velocity, dt));

	const bool gravitate = (def.move & move_flag::Gravitate) != 0;
	if (gravitate) {
		// Engine [orig: CParticleEmitter_UpdateParticles @ 0x5e6980, move&2]:
		// `delta = pos - emitter.pos`, D3DXVec3Normalize FIRST (`sub_68B032
		// @ 0x68B032` thunks the off_85072C IAT entry — constant-magnitude
		// force, NOT a distance-proportional spring), THEN `gravity_mask`
		// (def+3916) scales the unit vector per axis with no renormalize —
		// a zeroed axis drops that component, leaving a sub-unit force.
		// Re-witnessed 2026-07-10 (order was mask-then-normalize here before;
		// identical for the corpus-typical {1,1,1} mask). The direction is
		// REPULSIVE (away from the emitter) — authors flip via negative mask
		// components.
		Vec3 delta{
			p.position.x - e.position.x,
			p.position.y - e.position.y,
			p.position.z - e.position.z,
		};
		const float len = vec3_length(delta);
		if (len > 1e-6f) {
			delta = {
				(delta.x / len) * def.gravity_mask.x,
				(delta.y / len) * def.gravity_mask.y,
				(delta.z / len) * def.gravity_mask.z,
			};
			// Spring scalar source: prefer the explicit `Emitter::spring_const`
			// (engine-faithful — set by the manager in
			// `CEffectEmitter_Initialize @ 0x5e6020`) when non-zero; otherwise
			// fall back to `def.gravity` so stand-alone callers without a
			// manager still get sensible behaviour.
			const float spring = e.spring_const != 0.0f ? e.spring_const : def.gravity;
			p.velocity = vec3_add(p.velocity, vec3_scale(delta, spring * dt));
		}
	} else {
		// NORMAL move: y-only gravity accel. The engine applies the raw
		// emitter gravity slot here — gravity_mask is read ONLY in the
		// GRAVITATE branch [orig: @ 0x5e6980; mask read at def+3916 sits
		// inside the move&2 path]. Re-witnessed 2026-07-10 (was scaled by
		// gravity_mask.y here before).
		p.velocity.y -= def.gravity * dt;
	}

	// Drag: exponential decay (1 - drag*dt) per axis. Engine writes
	// `vel -= drag*dt * vel`; same form, clamped to non-negative coefficient.
	const float drag_coef = clampf(def.drag * dt, 0.0f, 1.0f);
	p.velocity.x -= p.velocity.x * drag_coef;
	p.velocity.y -= p.velocity.y * drag_coef;
	p.velocity.z -= p.velocity.z * drag_coef;

	// ORBIT modifier (move & 4 — engine bit 2 per the 0x848800 reorder, see
	// particle.h::move_flag). When set, after the ballistic / spring step, the
	// engine rotates the relative position vector and velocity around
	// `def.orbital_axis` by an angle proportional to time. We use
	// `def.orbitalspeed * dt` as the per-frame angle (engine derives a similar
	// quantity from emitter state × particle.age × dt; the exact FPU stack
	// chain is not byte-decodable without full register tracing). Rotates
	// both position offset and velocity so the orbital trajectory stays
	// stable across frames. Engine cite: CParticleEmitter_UpdateAllParticles
	// @ 0x5f3be0 — `(move & 4)` branch + init_D3DXMatrixRotationAxis call.
	if ((def.move & move_flag::Orbit) != 0 && def.orbitalspeed != 0.0f) {
		const Vec3 axis = vec3_normalize(def.orbital_axis);
		const float angle = def.orbitalspeed * dt;
		const Vec3 rel{
			p.position.x - e.position.x,
			p.position.y - e.position.y,
			p.position.z - e.position.z,
		};
		const Vec3 rotated_rel = vec3_rotate_around_axis(rel, axis, angle);
		p.position = {
			e.position.x + rotated_rel.x,
			e.position.y + rotated_rel.y,
			e.position.z + rotated_rel.z,
		};
		p.velocity = vec3_rotate_around_axis(p.velocity, axis, angle);
	}

	// Scale grows from 0 toward 1 over the particle's lifetime
	// (engine: `*(extra+48) += *(extra+52) * dt` with scale_velocity = 1/age).
	p.scale += p.scale_velocity * dt;
	if (p.scale > 1.0f) p.scale = 1.0f;

	// Rotation accumulates at the per-particle rate.
	p.rotation += p.rotation_rate * dt;

	// Kill-plane check. Engine: CParticleEmitter_UpdateParticles @ 0x5e6980
	// reads `*(emitter+332)` as a `float*` threshold; def.flags bit 27
	// (0x08000000) → kill if particle.y > threshold; bit 28 (0x10000000)
	// → kill if particle.y <= threshold. Engine writes `particle.age = 0`
	// to mark expired (no position clamp); next-frame `expire_dead` pass
	// removes. Our portable form lifts the trigger to runtime emitter
	// scalars `kill_plane_mode` + `kill_plane_y` so the API surface
	// doesn't depend on engine-internal flag bits 27/28 (those are
	// outside the 26-name flag table at 0x846A18 — manager-set, not
	// authored).
	if (e.kill_plane_mode == 1u && p.position.y > e.kill_plane_y) {
		p.age = 0.0f;
	} else if (e.kill_plane_mode == 2u && p.position.y <= e.kill_plane_y) {
		p.age = 0.0f;
	}

	// Age decrements unless the def has the NEVERAGE flag (bit 0x04 in engine,
	// our particle_flag::NeverAge constant).
	if ((def.flags & particle_flag::NeverAge) == 0) {
		p.age -= dt;
	}
}

// CParticleEmitter_SpawnParticle @ 0x5e7640: bits 0x01..0x100 are set when
// the chosen graphic layer has a non-null LUT pointer at the matching offset.
// We follow per-graphic curves with particle-level fallback (mirrors the
// renderer's choose_curve precedence in nova_particle_emitter.cpp).
std::uint32_t compute_spawn_flags(const ParticleDef &def, std::uint32_t graphic_idx) noexcept {
	using namespace particle_runtime_flag;
	const GraphicLayer &layer = def.graphics[graphic_idx];
	std::uint32_t flags = 0;
	if (layer.alpha_func.baked || def.alpha_func.baked) flags |= AlphaCurve;
	if (layer.red_func.baked   || def.red_func.baked)   flags |= RedCurve;
	if (layer.green_func.baked || def.green_func.baked) flags |= GreenCurve;
	if (layer.blue_func.baked  || def.blue_func.baked)  flags |= BlueCurve;
	if (layer.scale_func.baked || def.scale_func.baked) flags |= ScaleCurve;
	if (layer.flip_frames > 1) flags |= Flipbook;
	if (layer.blend_mode == BlendMode::Bump || layer.blend_mode == BlendMode::Bumpadd) flags |= LitColor;
	if (layer.blend_mode == BlendMode::Distort) flags |= Distort;
	return flags;
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
	p.flags = compute_spawn_flags(def, p.graphic_layer);
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
	e.emit_started = false;
	e.emit_delay_remaining = def != nullptr ? std::max(def->emit_delay, 0.0f) : 0.0f;
	e.emit_dur_remaining = def != nullptr ? std::max(def->emit_dur, 0.0f) : 0.0f;
	e.age = 0.0f;
	e.next_serial = 0;
	e.active = def != nullptr;
	e.finite = def != nullptr ? (def->flags & particle_flag::ForeverEmit) == 0 : true;
	e.last_translation_delta = {0.0f, 0.0f, 0.0f};
	e.cumulative_translation = {0.0f, 0.0f, 0.0f};
	// Note: `color_tint`, `spring_const`, and `lod_divisor` are NOT reset
	// here — they're user-controlled / manager-set scalars (engine
	// equivalents are set per-frame from outside `Initialize`), and
	// resetting them on every `play()` / `restart()` would clobber the
	// caller's intent.
	// Avoid seed=0 producing a zero-bound LCG for the first few values.
	e.rng_state = seed != 0 ? seed : 0x9E3779B9u;
}

void emitter_translate(Emitter &e, Vec3 new_pos) noexcept {
	// CParticleEmitter_TranslatePosition @ 0x5efe90: compute delta, update
	// position, accumulate. Engine maintains two parallel accumulators
	// (emitter+212/+224 = AABB min/max); we expose just the deltas because
	// the AABB itself isn't tracked yet.
	const Vec3 delta{
		new_pos.x - e.position.x,
		new_pos.y - e.position.y,
		new_pos.z - e.position.z,
	};
	e.prev_position = e.position;
	e.position = new_pos;
	e.last_translation_delta = delta;
	e.cumulative_translation = vec3_add(e.cumulative_translation, delta);

	// Engine `PositionRelative` flag (bit 19 = 0x80000): when set, particles
	// stay attached to the emitter — translating the emitter carries every
	// alive particle along by the same delta. Default (flag clear) is engine
	// behaviour where particles render in world space and are "left behind"
	// when the emitter moves. Corpus survey: none of the 5 reference fixtures
	// author PositionRelative, so the world-space default matches typical
	// authoring intent.
	if (e.def != nullptr && (e.def->flags & particle_flag::PositionRelative) != 0) {
		for (Particle &p : e.particles) {
			p.position.x += delta.x;
			p.position.y += delta.y;
			p.position.z += delta.z;
		}
	}
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
				integrate_particle(p, def, e, dt);
			}
			expire_dead(e);
			return;
		}
		dt += e.emit_delay_remaining; // consume any sub-step overrun
	}

	// Emission. emit_rate is particles/sec; emit_burst is particles spawned
	// per emission tick. emit_dur counts down (unless FOREVEREMIT flag is set).
	//
	// Engine: CEffectEmitter_AdvanceEmission @ 0x5e1d30 — when the def's
	// emit_rate_func resolves to a tabledef, the engine scales the emission
	// interval by the LUT byte at age-normalized index, divided by 128.0
	// (engine 128 = 1.0 neutral, 0 = no emission, 255 ≈ 2× faster):
	//   interval = (1 / emit_rate) / (lut[t * 256 % 256] / 128.0)
	// We mirror by computing the same scale factor here. `t` is normalized
	// against `def.emit_dur` so the curve plays out across the emitter's
	// finite emission window; FOREVEREMIT loops the curve modulo 256.
	const bool can_emit = e.finite ? e.emit_dur_remaining > 0.0f : true;
	if (can_emit && def.emit_rate > 0.0f) {
		float rate_scale = 1.0f;
		if (def.emit_rate_func.baked) {
			float t_norm = 0.0f;
			if (def.emit_dur > 1e-6f) {
				t_norm = clampf(e.age / def.emit_dur, 0.0f, 0.999999f);
			} else {
				// FOREVEREMIT or zero-dur: cycle through the LUT every second.
				t_norm = e.age - std::floor(e.age);
			}
			const int lut_idx = static_cast<int>(t_norm * 256.0f) & 0xFF;
			const std::uint8_t lut_byte = def.emit_rate_func.baked_lut[static_cast<std::size_t>(lut_idx)];
			rate_scale = static_cast<float>(lut_byte) / 128.0f;
		}
		const float scaled_rate = def.emit_rate * rate_scale;
		if (scaled_rate > 1e-3f) {
			const float interval = 1.0f / scaled_rate;
			// The first burst lands on the first emitting advance (t ≈ 0), not one
			// full interval in: a flash-class def (emit_dur 0.1, emit_rate 10) must
			// emit inside its authored window at all. The base cadence is not pinned
			// by the AdvanceEmission @ 0x5e1d30 record (it witnesses only the LUT
			// scaling); an immediate first burst is the only reading under which such
			// windows produce their particles.
			if (!e.emit_started) {
				e.emit_started = true;
				e.emit_accumulator += interval;
			}
			e.emit_accumulator += dt;
			// Bound this frame's bursts by the remaining window so one large dt
			// cannot overshoot it; the window itself elapses with AGE below.
			float window = e.finite ? e.emit_dur_remaining : 0.0f;
			while (e.emit_accumulator >= interval) {
				e.emit_accumulator -= interval;
				const int burst = std::max(def.emit_burst, 1);
				for (int b = 0; b < burst; ++b) {
					emit_one_internal(e, def);
				}
				if (e.finite) {
					window -= interval;
					if (window <= 0.0f) {
						break;
					}
				}
			}
		} else {
			// Curve is zero or near-zero: pause emission this frame but keep
			// the accumulator unchanged so an instant rate-recovery picks up
			// where it left off.
		}
	}
	// The emission window elapses with age — the same clock the rate LUT indexes by
	// (t = age / emit_dur) — not per fired burst: a def with emit_rate 1.0 and
	// emit_dur 0.5 closes its window at t = 0.5 regardless of cadence.
	if (e.finite) {
		e.emit_dur_remaining -= dt;
		if (e.emit_dur_remaining < 0.0f) {
			e.emit_dur_remaining = 0.0f;
		}
	}

	// Physics integration + aging.
	for (Particle &p : e.particles) {
		integrate_particle(p, def, e, dt);
	}
	expire_dead(e);

	// Emitter is "done" when finite duration is exhausted AND no live particles
	// remain — caller can flip e.active off based on this if they want pooling.
}

} // namespace opennova::particle
