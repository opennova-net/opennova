#include "particle/emitter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "particle/particle.h"

namespace opennova::particle {

namespace {

constexpr float kDegreesToRadians = 0.01745329251994329577f;

// Numerically-stable LCG. Constants are the classic glibc `rand()` parameters.
// We don't need engine bit-exact RNG output because the simulator is not
// claiming byte parity with retail; we DO need cross-platform determinism so
// tests that pin spawn positions hold on every platform.
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

// Random direction from independently bounded yaw and pitch rotations around
// `axis`. The retail direction-helper vtable receives both spread and
// spread_skip; each signed random angle has magnitude in [skip, spread].
// Applying real axis rotations keeps each Euler component inside its authored
// bound while preserving the helper's two-draw RNG cadence.
Vec3 random_direction_in_cap(
		Emitter &e, Vec3 axis, float outer_angle_rad, float inner_angle_rad = 0.0f) noexcept {
	const Vec3 forward = vec3_normalize(axis);
	Vec3 seed_axis = std::abs(forward.x) > 0.9f ?
			Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
	Vec3 up = vec3_normalize(vec3_cross(forward, seed_axis));
	Vec3 right = vec3_cross(up, forward);
	constexpr float kTwoPi = 6.28318530717958647692f;
	const float outer = std::isfinite(outer_angle_rad)
			? clampf(std::abs(outer_angle_rad), 0.0f, kTwoPi) : 0.0f;
	const float inner = std::isfinite(inner_angle_rad)
			? clampf(std::abs(inner_angle_rad), 0.0f, outer) : 0.0f;
	const auto bounded_angle = [inner, outer](float sample) {
		const float magnitude = lerp(inner, outer, std::abs(sample));
		return std::copysign(magnitude, sample);
	};
	const float yaw = bounded_angle(emitter_rand_signed(e));
	const float pitch = bounded_angle(emitter_rand_signed(e));
	const Vec3 yawed_forward = vec3_rotate_around_axis(forward, up, yaw);
	const Vec3 yawed_right = vec3_rotate_around_axis(right, up, yaw);
	return vec3_normalize(vec3_rotate_around_axis(yawed_forward, yawed_right, pitch));
}

void apply_emission_shape(Emitter &e, const ParticleDef &def, Particle &p) noexcept {
	// CParticleEmitter_SpawnParticle @ 0x5e7640 — switch on def.emit_shape
	// (def+3924, read @ 0x5e78ef). Every shape displaces the spawn POSITION;
	// velocity is seeded separately for ALL shapes (spread cone x speed, in
	// emit_one_internal) — the earlier port wrote the annular scales into
	// velocity, which turned hollow-shell spawn volumes into speed
	// distributions.
	const EmitShape shape = static_cast<EmitShape>(def.emit_shape);
	switch (shape) {
		case EmitShape::Point: {
			// No shape offset (engine switch default).
			break;
		}
		case EmitShape::Box: {
			// Engine case 1 [orig: SpawnParticle @ 0x5e7900..0x5e795e]: pick one
			// dominant axis (rand % 3). Sign is random unless SIGNEDROTATIONS
			// (0x800000) pins it positive [@ 0x5e790f]. The dominant axis
			// starts at `sign * skip[axis] * 0.5` [@ 0x5e7939: the def+3940
			// table * flt_7C3B94], then the per-axis loop adds:
			//   - dominant axis: rand01 * (size[axis] - skip[axis]) * 0.5,
			//     pushed OUTWARD (sign-matched to the accumulated component) —
			//     total one-sided offset in [skip/2, size/2];
			//   - other axes: rand_signed * size[axis] * 0.5.
			// I.e. a hollow-box shell: inner half-extent skip/2, outer size/2.
			const std::uint32_t axis = emitter_rand10(e) % 3u;
			const float sign =
					(def.flags & particle_flag::SignedRotations) != 0 ? 1.0f :
					((emitter_rand10(e) & 1u) != 0 ? 1.0f : -1.0f);
			const float size_a[3] = {def.emit_shape_size.x, def.emit_shape_size.y,
					def.emit_shape_size.z};
			const float skip_a[3] = {def.emit_shape_size_skip.x, def.emit_shape_size_skip.y,
					def.emit_shape_size_skip.z};
			float off[3] = {0.0f, 0.0f, 0.0f};
			off[axis] = sign * skip_a[axis] * 0.5f;
			for (std::uint32_t k = 0; k < 3; ++k) {
				if (k == axis) {
					const float grow = emitter_rand_unit(e) * (size_a[k] - skip_a[k]) * 0.5f;
					off[k] += off[k] < 0.0f ? -grow : grow;
				} else {
					off[k] += emitter_rand_signed(e) * size_a[k] * 0.5f;
				}
			}
			p.position = vec3_add(p.position, {off[0], off[1], off[2]});
			break;
		}
		case EmitShape::Sphere: {
			// Engine case 2 [orig: SpawnParticle @ 0x5e7640 case 2]: random
			// unit direction over the full sphere (the engine passes 360.0 =
			// flt_7C3BA4 to its direction helper), each component scaled by the
			// annular `lerp(skip, size, rand01)` per axis, added to POSITION —
			// a hollow ellipsoidal spawn shell.
			Vec3 unit{
				emitter_rand_signed(e),
				emitter_rand_signed(e),
				emitter_rand_signed(e),
			};
			unit = vec3_normalize(unit);
			const Vec3 r = annular_axis_scales(e, def);
			p.position = vec3_add(p.position, {unit.x * r.x, unit.y * r.y, unit.z * r.z});
			break;
		}
		case EmitShape::Cone: {
			// Engine case 3 [orig: SpawnParticle @ 0x5e7640 case 3]: direction
			// within a fixed 90-degree cap (flt_7DCBF0) around the emitter
			// forward, annular per-axis magnitude, added to POSITION — a hollow
			// hemispherical spawn cap.
			const Vec3 dir = random_direction_in_cap(e, e.forward, 90.0f * 0.0174533f);
			const Vec3 r = annular_axis_scales(e, def);
			p.position = vec3_add(p.position, {dir.x * r.x, dir.y * r.y, dir.z * r.z});
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
		// identical for the corpus-typical {1,1,1} mask). The normalized delta
		// points outward; the converted shared gravity slot supplies the sign
		// (positive authored gravity therefore attracts). Negative mask
		// components flip individual axes.
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
			// use the same converted retail gravity slot as NORMAL movement.
			const float spring = e.spring_const != 0.0f ? e.spring_const : e.gravity_accel;
			p.velocity = vec3_add(p.velocity, vec3_scale(delta, spring * dt));
		}
	} else {
		// NORMAL move: y-only gravity accel. The engine applies the converted
		// emitter gravity slot here — gravity_mask is read ONLY in the
		// GRAVITATE branch [orig: @ 0x5e6980; mask read at def+3916 sits
		// inside the move&2 path]. Re-witnessed 2026-07-10 (was scaled by
		// gravity_mask.y here before). The slot ADDS onto vel.y, but
		// CEffectEmitter_Initialize seeds it as authored gravity × -0.09803897:
		// positive authored gravity sinks/attracts and negative gravity lifts.
		p.velocity.y += e.gravity_accel * dt;
	}

	// Drag: Euler decay (1 - drag_slot*dt) per axis. Engine writes
	// `vel -= drag_slot*dt * vel`; Initialize seeds drag_slot = authored × .01.
	const float drag_coef = e.drag_coefficient * dt;
	p.velocity.x -= p.velocity.x * drag_coef;
	p.velocity.y -= p.velocity.y * drag_coef;
	p.velocity.z -= p.velocity.z * drag_coef;

	// ORBIT modifier (move & 4 — engine bit 2 per the 0x848800 reorder, see
	// particle.h::move_flag). When set, after the ballistic / spring step, the
	// engine rotates the relative position vector and velocity around
	// `def.orbital_axis` by an angle proportional to time. `emitter_init` stores
	// the randomized authored degrees/sec as radians/sec, so we use
	// `orbit_speed * dt` as the per-frame angle (engine derives a similar
	// quantity from emitter state × particle.age × dt; the exact FPU stack
	// chain is not byte-decodable without full register tracing). Rotates
	// both position offset and velocity so the orbital trajectory stays
	// stable across frames. Engine cite: CParticleEmitter_UpdateAllParticles
	// @ 0x5f3be0 — `(move & 4)` branch + init_D3DXMatrixRotationAxis call.
	if ((def.move & move_flag::Orbit) != 0 && e.orbit_speed != 0.0f) {
		const Vec3 axis = vec3_normalize(def.orbital_axis);
		const float angle = e.orbit_speed * dt;
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

	// Curve phase advances 0 -> 256 across the lifetime, unclamped — for
	// NEVERAGE particles it keeps running and the renderer's `% 256` wraps the
	// curves cyclically [orig: UpdateParticles @ 0x5e6980 — `+0x30 += +0x34 *
	// dt`; BuildBillboardQuads @ 0x5e6d60 indexes `(int)phase % 256`]. The
	// prior port misread this pair as a draw-size "spawn-pop ramp".
	p.curve_phase += p.phase_rate * dt;

	// Euler angles accumulate at their per-particle rates. Roll drives the
	// billboard spin; yaw/pitch only render for YAWANDPITCH defs
	// [orig: the +0x3C += +0x40 form; yaw/pitch pairs in the emitter+0x150
	// array via CParticleEmitter_UpdateAllParticles @ 0x5f3be0].
	p.rotation += p.rotation_rate * dt;
	p.yaw += p.yaw_rate * dt;
	p.pitch += p.pitch_rate * dt;

	// Kill-plane check. Engine: CParticleEmitter_UpdateParticles @ 0x5e6980
	// reads `*(emitter+332)` as a `float*` threshold; def.flags bit 27
	// (0x08000000) → kill if particle.y > threshold; bit 28 (0x10000000)
	// → kill if particle.y <= threshold. Engine writes `particle.age = 0`
	// to mark expired (no position clamp); next-frame `expire_dead` pass
	// removes. Our portable form lifts the trigger to runtime emitter
	// scalars `kill_plane_mode` + `kill_plane_y`: bits 27/28 are named and
	// authored, while the threshold itself remains manager/site supplied.
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

bool emit_one_internal(Emitter &e, const ParticleDef &def) noexcept {
	const std::size_t pool_limit = std::min(e.max_particles, kEmitterHardParticleLimit);
	if (e.particles.size() >= pool_limit) {
		return false;
	}
	Particle p{};
	// Spawn order mirrors CParticleEmitter_SpawnParticle @ 0x5e7640.
	p.graphic_layer = static_cast<std::uint8_t>(pick_graphic(e, def));
	const GraphicLayer &layer = def.graphics[p.graphic_layer];
	p.flags = compute_spawn_flags(def, p.graphic_layer);
	// Base draw size, randomized once from the chosen graphic layer's
	// scale/scale_adj (the layer inherits particle-level scale at parse)
	// [orig: @ 0x5e7862 — rand_signed * graphic+332 + graphic+328 into +0x38].
	p.size = layer.scale + emitter_rand_signed(e) * layer.scale_adj;
	// Alpha comes from the chosen GRAPHIC's alpha (already def-inherited at
	// parse), not the def's — [orig: @ 0x5e77a0: graphic+0x198 * 255].
	p.alpha = static_cast<std::uint8_t>(clampf(layer.alpha * 255.0f, 0.0f, 255.0f));
	p.lifetime = def.age + emitter_rand_signed(e) * def.age_adj;
	p.age = p.lifetime;
	const bool valid_lifetime = std::isfinite(p.lifetime) && p.lifetime > 0.0f;
	// Roll seed = orientation.z + orientationadj.z * rand01 (degrees; we store
	// radians) [orig: @ 0x5e7803 — def+3824/+3836 into +0x3C]. Rate = roll_rot
	// family with a random sign flip unless SIGNEDROTATIONS pins it
	// [orig: @ 0x5e782a..0x5e7889 — def+3856/+3860, flag 0x800000 gate].
	p.rotation = (def.orientation.z + emitter_rand_unit(e) * def.orientationadj.z) * 0.0174533f;
	const float roll_sign =
			(def.flags & particle_flag::SignedRotations) != 0 ? 1.0f :
			((emitter_rand10(e) & 1u) != 0 ? 1.0f : -1.0f);
	p.rotation_rate = (def.roll_rot * roll_sign +
			emitter_rand_signed(e) * def.roll_rot_adj) * 0.0174533f;
	// YAWANDPITCH particles additionally carry yaw/pitch Euler state (the
	// engine's parallel array at emitter+0x150)
	// [orig: CParticleEmitter_SpawnNewParticle @ 0x5f3663/0x5f36a5 —
	// orientation.x/adj.x seed + yaw_rot-family rate; the pitch pair follows
	// the same shape]. Same SIGNEDROTATIONS gate per rate.
	if ((def.flags & particle_flag::YawAndPitch) != 0) {
		p.yaw = (def.orientation.x + emitter_rand_unit(e) * def.orientationadj.x) * 0.0174533f;
		const float yaw_sign =
				(def.flags & particle_flag::SignedRotations) != 0 ? 1.0f :
				((emitter_rand10(e) & 1u) != 0 ? 1.0f : -1.0f);
		p.yaw_rate = (def.yaw_rot * yaw_sign +
				emitter_rand_signed(e) * def.yaw_rot_adj) * 0.0174533f;
		p.pitch = (def.orientation.y + emitter_rand_unit(e) * def.orientationadj.y) * 0.0174533f;
		const float pitch_sign =
				(def.flags & particle_flag::SignedRotations) != 0 ? 1.0f :
				((emitter_rand10(e) & 1u) != 0 ? 1.0f : -1.0f);
		p.pitch_rate = (def.pitch_rot * pitch_sign +
				emitter_rand_signed(e) * def.pitch_rot_adj) * 0.0174533f;
	}
	// Curve phase starts at 0 and sweeps to 256 across the lifetime
	// [orig: @ 0x5e788c/0x5e7898 — flt_7D1D70 (256.0) / age into +0x34, zero
	// into +0x30]. This is the LUT index clock, not a draw-size ramp.
	p.curve_phase = 0.0f;
	p.phase_rate = valid_lifetime ? 256.0f / p.lifetime : 0.0f;
	// Spawn position: emitter + (0, y_offset, 0) [orig: @ 0x5e78a1 — only
	// def+3724 lands in the position; z_offset is NOT positional — it becomes
	// the render-side camera-ward pull (emitter+0x140, seeded -z_offset in
	// CEffectEmitter_Initialize @ 0x5e6349)].
	p.position = e.position;
	p.position.y += def.y_offset;
	p.color_slot = static_cast<std::uint8_t>(pick_color_slot(e));
	p.color = color_for_slot(def, p.graphic_layer, p.color_slot);
	p.serial = e.next_serial++;
	apply_emission_shape(e, def, p);
	// Velocity: EVERY shape gets direction-in-spread-cone around the emitter
	// forward, scaled by `speed + speed_adj * rand_signed`
	// [orig: SpawnParticle @ 0x5e7640 post-switch block — the vtable direction
	// helper, then the def+3892/+3896 multiply onto all three components;
	// EMITVECTOR (0x10000) selects the alternate direction helper, an
	// unported distinction].
	if (def.speed != 0.0f || def.speed_adj != 0.0f) {
		const float speed = def.speed + emitter_rand_signed(e) * def.speed_adj;
		const Vec3 dir = random_direction_in_cap(
				e, e.forward, def.spread * 0.0174533f, def.spread_skip * 0.0174533f);
		p.velocity = vec3_add(p.velocity, vec3_scale(dir, speed));
	}
	// Retail completes the spawn RNG path but rejects particles whose resolved
	// lifetime is non-positive or non-finite instead of clamping them alive.
	if (!valid_lifetime) {
		return false;
	}
	e.particles.push_back(p);
	return true;
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
	return static_cast<float>(emitter_rand10(e)) / 1023.0f;
}

float emitter_rand_signed(Emitter &e) noexcept {
	return emitter_rand_unit(e) * 2.0f - 1.0f;
}

void emitter_init(Emitter &e, const ParticleDef *def, Vec3 pos, std::uint32_t seed) {
	e.rng_state = seed != 0 ? seed : 0x9E3779B9u;
	e.def = def;
	e.position = pos;
	e.prev_position = pos;
	e.forward = {0.0f, 0.0f, 1.0f};
	e.particles.clear();
	e.emit_accumulator = 0.0f;
	e.emit_started = false;
	e.emit_delay_remaining = def != nullptr ? std::max(def->emit_delay, 0.0f) : 0.0f;
	if (def != nullptr) {
		const float duration = def->emit_dur + emitter_rand_signed(e) * def->emit_dur_adj;
		const float rate = def->emit_rate + emitter_rand_signed(e) * def->emit_rate_adj;
		e.emit_dur_total = std::isfinite(duration) ? std::max(duration, 0.0f) : 0.0f;
		e.emit_dur_remaining = e.emit_dur_total;
		e.emit_rate = std::isfinite(rate) ? std::max(rate, 0.0f) : 0.0f;
	} else {
		e.emit_dur_total = 0.0f;
		e.emit_dur_remaining = 0.0f;
		e.emit_rate = 0.0f;
	}
	e.age = 0.0f;
	e.gravity_accel = def != nullptr && std::isfinite(def->gravity)
			? def->gravity * -0.09803897f : 0.0f;
	e.drag_coefficient = def != nullptr && std::isfinite(def->drag)
			? def->drag * 0.01f : 0.0f;
	e.orbit_speed = def != nullptr && std::isfinite(def->orbitalspeed)
			? def->orbitalspeed : 0.0f;
	// The adjustment is authored as a signed random range. Avoid an otherwise
	// unused RNG draw when the adjustment is zero so definitions without the
	// field keep their established deterministic spawn sequence.
	if (def != nullptr && def->orbitalspeed_adj != 0.0f &&
			std::isfinite(def->orbitalspeed_adj)) {
		e.orbit_speed += emitter_rand_signed(e) * def->orbitalspeed_adj;
	}
	if (!std::isfinite(e.orbit_speed)) {
		e.orbit_speed = 0.0f;
	} else {
		// CParticleEmitter_SpawnNewParticle @ 0x5f37a2..0x5f37c3 first
		// randomizes orbitalspeed +/- orbitalspeed_adj in authored degrees/sec,
		// then multiplies the result by pi/180 before storing the runtime rate.
		e.orbit_speed *= kDegreesToRadians;
	}
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
	const std::size_t pool_limit = std::min(e.max_particles, kEmitterHardParticleLimit);
	if (e.def == nullptr || e.particles.size() >= pool_limit) {
		return false;
	}
	return emit_one_internal(e, *e.def);
}

void emitter_advance(Emitter &e, float dt) {
	if (!e.active || e.def == nullptr || !std::isfinite(dt) || dt <= 0.0f) {
		return;
	}
	const ParticleDef &def = *e.def;
	// Retail's AdvanceFrame expiry pass runs before UpdateParticles. A particle
	// that reaches age <= 0 during this integration remains observable for its
	// terminal frame and is reclaimed at the beginning of the next advance.
	expire_dead(e);
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
	if (can_emit && e.emit_rate > 0.0f) {
		float rate_scale = 1.0f;
		if (def.emit_rate_func.baked) {
			float t_norm = 0.0f;
			if (e.emit_dur_total > 1e-6f) {
				t_norm = clampf(e.age / e.emit_dur_total, 0.0f, 0.999999f);
			} else {
				// FOREVEREMIT or zero-dur: cycle through the LUT every second.
				t_norm = e.age - std::floor(e.age);
			}
			const int lut_idx = static_cast<int>(t_norm * 256.0f) & 0xFF;
			const std::uint8_t lut_byte = def.emit_rate_func.baked_lut[static_cast<std::size_t>(lut_idx)];
			rate_scale = static_cast<float>(lut_byte) / 128.0f;
		}
		const float scaled_rate = e.emit_rate * rate_scale;
		if (std::isfinite(scaled_rate) && scaled_rate > 1e-3f) {
			const float interval = 1.0f / scaled_rate;
			if (std::isfinite(interval) && interval > 0.0f) {
				// The first burst lands on the first emitting advance (t ≈ 0), not one
				// full interval in: a flash-class def (emit_dur 0.1, emit_rate 10) must
				// emit inside its authored window at all. The base cadence is not pinned
				// by the AdvanceEmission @ 0x5e1d30 record (it witnesses only the LUT
				// scaling); an immediate first burst is the only reading under which such
				// windows produce their particles.
				if (!std::isfinite(e.emit_accumulator) || e.emit_accumulator < 0.0f) {
					e.emit_accumulator = 0.0f;
				}
				if (!e.emit_started) {
					e.emit_started = true;
					e.emit_accumulator += interval;
				}
				e.emit_accumulator += dt;
				if (!std::isfinite(e.emit_accumulator)) {
					e.emit_accumulator = interval;
				}
				// Bound this frame's bursts by the remaining window so one large dt
				// cannot overshoot it; the window itself elapses with AGE below.
				float window = e.finite ? e.emit_dur_remaining : 0.0f;
				const std::size_t pool_limit = std::min(e.max_particles, kEmitterHardParticleLimit);
				while (e.emit_accumulator >= interval && e.particles.size() < pool_limit) {
					e.emit_accumulator -= interval;
					const std::size_t remaining = pool_limit - e.particles.size();
					const std::size_t requested = def.emit_burst > 0
							? static_cast<std::size_t>(def.emit_burst) : std::size_t{1};
					const std::size_t burst = std::min(requested, remaining);
					bool emitted = false;
					for (std::size_t b = 0; b < burst; ++b) {
						if (!emit_one_internal(e, def)) {
							break;
						}
						emitted = true;
					}
					if (!emitted) {
						break;
					}
					if (e.finite) {
						window -= interval;
						if (window <= 0.0f) {
							break;
						}
					}
				}
				// A full pool must not turn an authored extreme rate into unbounded
				// catch-up work. Advance the cadence and retain only the fractional
				// interval, just as if the skipped full-pool ticks had been consumed.
				if (e.particles.size() >= pool_limit && e.emit_accumulator >= interval) {
					e.emit_accumulator = std::fmod(e.emit_accumulator, interval);
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

	// Emitter is "done" when finite duration is exhausted AND no live particles
	// remain — caller can flip e.active off based on this if they want pooling.
}

} // namespace opennova::particle
