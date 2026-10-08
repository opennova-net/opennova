#include <runtime/particle/emitter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include <formats/particle/particle.h>

namespace opennova::particle {

namespace {

constexpr float kDegreesToRadians = 0.01745329251994329577f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

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

void bounds_reset(Emitter &e) noexcept {
	e.bounds_min = {kInfinity, kInfinity, kInfinity};
	e.bounds_max = {-kInfinity, -kInfinity, -kInfinity};
	e.bounds_valid = false;
}

void bounds_include(Emitter &e, const Vec3 &p) noexcept {
	e.bounds_min = {std::min(e.bounds_min.x, p.x), std::min(e.bounds_min.y, p.y),
			std::min(e.bounds_min.z, p.z)};
	e.bounds_max = {std::max(e.bounds_max.x, p.x), std::max(e.bounds_max.y, p.y),
			std::max(e.bounds_max.z, p.z)};
	e.bounds_valid = true;
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

// The retail direction helpers. Both draw a polar angle uniform in
// `[min_deg, max_deg]`, tilt world +Y by it (RotationX), spin the result around
// +Y by the azimuth (RotationY), and finally map local +Y onto `reference`
// through an orthonormal basis — unless `|reference|^2 < 0.99`, in which case
// the local vector stands (emission around world +Y).
//   vtable+28 [orig: CParticleSystem_GenerateRandomDirectionBasis @ 0x5e22a0]: azimuth =
//     rand01 * 2*pi, stored as the emitter's heading (+208) @ 0x5e2300.
//   vtable+32 [orig: CParticleSystem_ComputeConeDirectionVector @ 0x5e1f10]: azimuth =
//     heading + 2*pi / emit_burst @ 0x5e1f6e (the burst-distribute step; the
//     helper still consumes the second rand() draw @ 0x5e1f35).
// The basis choice only fixes where azimuth 0 points; its D3DX/FPU form is
// the accepted D-PTL-9 residual.
Vec3 cone_direction(Emitter &e, const ParticleDef &def, const Vec3 &reference,
		float max_deg, float min_deg, bool sequential_azimuth) noexcept {
	const float polar_unit = emitter_rand_unit(e);
	float heading;
	if (sequential_azimuth) {
		(void)emitter_rand10(e);
		const float burst = static_cast<float>(std::max(def.emit_burst, 1));
		heading = e.emit_heading + kTwoPi / burst;
	} else {
		heading = emitter_rand_unit(e) * kTwoPi;
	}
	e.emit_heading = heading;
	const float polar = (min_deg + (max_deg - min_deg) * polar_unit) * kDegreesToRadians;
	const float sp = std::sin(polar);
	const Vec3 local{sp * std::sin(heading), std::cos(polar), sp * std::cos(heading)};
	const float ref_len_sq = vec3_dot(reference, reference);
	if (!(ref_len_sq >= 0.99f)) {
		return local;
	}
	const Vec3 up = vec3_normalize(reference);
	const Vec3 helper = std::abs(up.z) < 0.9f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
	const Vec3 right = vec3_normalize(vec3_cross(up, helper));
	const Vec3 binormal = vec3_cross(right, up);
	return vec3_normalize({
		right.x * local.x + up.x * local.y + binormal.x * local.z,
		right.y * local.x + up.y * local.y + binormal.y * local.z,
		right.z * local.x + up.z * local.y + binormal.z * local.z,
	});
}

void apply_emission_shape(Emitter &e, const ParticleDef &def, const Vec3 &direction,
		Particle &p) noexcept {
	// CParticleEmitter_SpawnParticle @ 0x5e7640 — switch on def.emit_shape
	// (def+3924, read @ 0x5e78ef). Every shape displaces the spawn POSITION;
	// velocity is seeded separately for ALL shapes (spread cone x speed, in
	// emit_one_internal).
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
			// Engine case 2 [orig: SpawnParticle @ 0x5e7b46..0x5e7c3a]: the
			// direction helper with polar range [0, 360] — the 360 is
			// flt_7C3BA4 @ 0x5e7b64, the 0 is the FPU leftover spilled
			// @ 0x5e7b5e — so the polar angle is uniform over a full turn; each
			// component is then scaled by the annular `lerp(skip, size, rand01)`
			// per axis and added to POSITION — a hollow ellipsoidal spawn shell.
			const Vec3 dir = cone_direction(e, def, direction, 360.0f, 0.0f, false);
			const Vec3 r = annular_axis_scales(e, def);
			p.position = vec3_add(p.position, {dir.x * r.x, dir.y * r.y, dir.z * r.z});
			break;
		}
		case EmitShape::Cone: {
			// Engine case 3 [orig: SpawnParticle @ 0x5e7a2f..0x5e7b3e]: both
			// helper angles are flt_7DCBF0 = 90 @ 0x5e7a33..0x5e7a48, so the
			// polar angle is pinned at 90 degrees — a RING in the plane
			// perpendicular to the spawn direction (random azimuth), annular
			// per-axis magnitude, added to POSITION.
			const Vec3 dir = cone_direction(e, def, direction, 90.0f, 90.0f, false);
			const Vec3 r = annular_axis_scales(e, def);
			p.position = vec3_add(p.position, {dir.x * r.x, dir.y * r.y, dir.z * r.z});
			break;
		}
	}
}

// Retail's sphere-in-viewport test for the NOVISNOUPDATE gate: the AABB's
// center and half-diagonal (a zero extent becomes 0.01) against the clip state
// [orig: BoundingBox_IsVisibleInFrustum @ 0x5e45b0 -> @ 0x5f6ca0 ->
//  Viewport_TransformAndClipPoint @ 0x4115e0]. The port tests the sphere against
// the six camera planes instead of retail's fixed-point viewport projection.
bool emitter_bounds_visible(const Emitter &e, const ParticleViewFrustum &frustum) noexcept {
	const Vec3 center{
		(e.bounds_min.x + e.bounds_max.x) * 0.5f,
		(e.bounds_min.y + e.bounds_max.y) * 0.5f,
		(e.bounds_min.z + e.bounds_max.z) * 0.5f,
	};
	const Vec3 extent{
		e.bounds_max.x - e.bounds_min.x,
		e.bounds_max.y - e.bounds_min.y,
		e.bounds_max.z - e.bounds_min.z,
	};
	float radius = 0.5f * vec3_length(extent);
	if (radius == 0.0f) {
		radius = 0.0099999998f;
	}
	for (const auto &plane : frustum.planes) {
		const float distance = plane[0] * center.x + plane[1] * center.y +
				plane[2] * center.z + plane[3];
		if (distance < -radius) {
			return false;
		}
	}
	return true;
}

void integrate_particle(Particle &p, const ParticleDef &def, const Emitter &e, float dt,
		const EmitterEnvironment &env, std::size_t index) noexcept {
	// CParticleEmitter_UpdateParticles @ 0x5e6980 (base) and
	// CParticleEmitter_UpdateAllParticles @ 0x5f3be0 (super system) share the
	// same per-particle order: translate, gravity, drag, roll, age, curve
	// phase, then the super-system extras — GLOBALWIND drift, focal wind
	// zones, aux yaw/pitch rates, ORBIT. Two physics dispatches:
	//   move & 1 (NORMAL):    pos += vel*dt; vel.y += gravity_slot*dt
	//   move & 2 (GRAVITATE): same translation, then a constant-magnitude
	//                         force along normalize(pos - emitter.pos),
	//                         per-axis masked AFTER the normalize.
	// The engine reads one emitter slot (+0x134) as both the NORMAL gravity
	// accel and the GRAVITATE force scalar; CEffectEmitter_Initialize
	// @ 0x5e6020 seeds it. WANDER/BUBBLE are engine-vestigial (zero xrefs).
	// The COLLIDE* response inside both updaters is unreachable: its probe
	// CParticleEmitter_CollisionProbe_Stub @ 0x5f78f0 is `xor eax, eax; ret`
	// (D-PTL-30).
	p.position = vec3_add(p.position, vec3_scale(p.velocity, dt));

	const bool gravitate = (def.move & move_flag::Gravitate) != 0;
	if (gravitate) {
		// Engine [orig: CParticleEmitter_UpdateParticles @ 0x5e6980, move&2]:
		// `delta = pos - emitter.pos`, D3DXVec3Normalize FIRST (`sub_68B032
		// @ 0x68B032` thunks the off_85072C IAT entry — constant-magnitude
		// force, NOT a distance-proportional spring), THEN `gravity_mask`
		// (def+3916) scales the unit vector per axis with no renormalize —
		// a zeroed axis drops that component, leaving a sub-unit force.
		// The normalized delta points outward; the converted shared gravity
		// slot supplies the sign (positive authored gravity therefore attracts).
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
		// inside the move&2 path]. The slot ADDS onto vel.y, but
		// CEffectEmitter_Initialize seeds it as authored gravity × -0.0981:
		// positive authored gravity sinks/attracts and negative gravity lifts.
		p.velocity.y += e.gravity_accel * dt;
	}

	// Drag: Euler decay (1 - drag_slot*dt) per axis. Engine writes
	// `vel -= drag_slot*dt * vel`; Initialize seeds drag_slot = authored × .01.
	const float drag_coef = e.drag_coefficient * dt;
	p.velocity.x -= p.velocity.x * drag_coef;
	p.velocity.y -= p.velocity.y * drag_coef;
	p.velocity.z -= p.velocity.z * drag_coef;

	// Roll accumulates at its per-particle rate [orig: the `+0x3C += +0x40 * dt`
	// form @ 0x5f3f1a-family].
	p.rotation += p.rotation_rate * dt;

	// Age decrements unless the def has the NEVERAGE flag (bit 0x04 in engine,
	// our particle_flag::NeverAge constant).
	if ((def.flags & particle_flag::NeverAge) == 0) {
		p.age -= dt;
	}

	// Curve phase advances 0 -> 256 across the lifetime, unclamped — for
	// NEVERAGE particles it keeps running and the renderer's `% 256` wraps the
	// curves cyclically [orig: UpdateParticles @ 0x5e6980 — `+0x30 += +0x34 *
	// dt`; BuildBillboardQuads @ 0x5e6d60 indexes `(int)phase % 256`].
	p.curve_phase += p.phase_rate * dt;

	// GLOBALWIND (0x400): the mission wind drifts the POSITION by wind*dt
	// after the velocity integration; velocity itself is untouched
	// [orig: CParticleEmitter_UpdateAllParticles @ 0x5f3c49 (emitter+0x154..0x15C
	//  <- flt_848D40..48) and the per-particle `pos += wind * dt` @ 0x5f4150-family].
	if ((def.flags & particle_flag::GlobalWind) != 0) {
		p.position = vec3_add(p.position, vec3_scale(env.global_wind, dt));
	}

	// The JO focal-wind branch runs after translation, drag and phase advance,
	// before orbit/kill planes. Only velocity is copied back from fixed point.
	// [orig: CParticleEmitter_UpdateAllParticles @0x5F3BE0]
	if (env.forces && (def.flags & (particle_flag::FocalWind | particle_flag::FocalWindForceAging)))
		env.forces->apply(p, index, (def.flags & particle_flag::FocalWindForceAging) != 0);

	// Aux Euler angles accumulate at their per-particle rates; yaw/pitch only
	// render for YAWANDPITCH defs [orig: aux[2] += aux[3]*dt, aux[0] += aux[1]*dt
	// in CParticleEmitter_UpdateAllParticles @ 0x5f3be0].
	p.yaw += p.yaw_rate * dt;
	p.pitch += p.pitch_rate * dt;

	// ORBIT modifier (move & 4 — engine bit 2 per the 0x848800 reorder, see
	// particle.h::move_flag). After the ballistic / spring step the engine
	// rotates the emitter-relative position and the velocity around
	// `def.orbital_axis` (def+3872) by `orbit_rate * dt`, the rate being the
	// particle's own aux+16 word [orig: CParticleEmitter_UpdateAllParticles
	// @ 0x5f3be0 — `fmul [ebx+10h]` x dt into init_D3DXMatrixRotationAxis
	// (D3DXMatrixRotationAxis), then the transform pair on rel/velocity].
	if ((def.move & move_flag::Orbit) != 0 && p.orbit_rate != 0.0f) {
		const Vec3 axis = vec3_normalize(def.orbital_axis);
		const float angle = p.orbit_rate * dt;
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
}

// The kill-plane pass runs over every particle after the integration loop
// [orig: CParticleEmitter_UpdateParticles @ 0x5e6c4b..0x5e6d4f]. def.flags
// bit 27 (BELOWH20) kills when particle.y > threshold; bit 28 (ABOVEH20) when
// particle.y <= threshold. Engine writes `particle.age = 0` (no position clamp);
// the next advance's expiry pass removes it. Our portable form lifts the
// trigger to runtime emitter scalars `kill_plane_mode` + `kill_plane_y`: the
// bits are named and authored, while the threshold remains site supplied.
void apply_kill_plane(Emitter &e) noexcept {
	if (e.kill_plane_mode == 0u) {
		return;
	}
	for (Particle &p : e.particles) {
		if (e.kill_plane_mode == 1u && p.position.y > e.kill_plane_y) {
			p.age = 0.0f;
		} else if (e.kill_plane_mode == 2u && p.position.y <= e.kill_plane_y) {
			p.age = 0.0f;
		}
	}
}

// CParticleEmitter_SpawnParticle @ 0x5e7640: bits 0x01..0x100 are set when
// the chosen graphic layer has a non-null LUT pointer at the matching offset.
// We follow per-graphic curves with particle-level fallback.
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

// The parent particle's CURRENT LUT-modulated color, as the child-spawn
// inherit block recomputes it: each channel whose curve flag is set is scaled
// by `lut[(int)parent.phase % 256] / 256` [orig: SpawnParticle @ 0x5e7d8a..0x5e7f06].
// The manager tint retail also folds in there (`(byte * channel) >> 7`
// @ 0x5e7f0a..0x5e7f89) is a render-time environment value in this port and
// is applied to the child at draw time instead.
struct ModulatedColor {
	Color3 rgb{};
	std::uint8_t alpha = 255;
};

std::uint8_t modulate_channel(std::uint8_t channel, const CurveRef &graphic_curve,
		const CurveRef &definition_curve, std::size_t lut_index) noexcept {
	const CurveRef &curve = graphic_curve.baked ? graphic_curve : definition_curve;
	if (!curve.baked) {
		return channel;
	}
	return static_cast<std::uint8_t>(
			(static_cast<std::uint32_t>(channel) * curve.baked_lut[lut_index]) >> 8u);
}

ModulatedColor parent_modulated_color(const Particle &parent, const ParticleDef &parent_def) noexcept {
	using namespace particle_runtime_flag;
	ModulatedColor out;
	out.rgb = parent.color;
	out.alpha = parent.alpha;
	const std::size_t lut_index = static_cast<std::size_t>(
			static_cast<std::int32_t>(parent.curve_phase) & 0xFF);
	const GraphicLayer &layer = parent_def.graphics[parent.graphic_layer];
	if ((parent.flags & AlphaCurve) != 0) {
		out.alpha = modulate_channel(parent.alpha, layer.alpha_func, parent_def.alpha_func, lut_index);
	}
	if ((parent.flags & RedCurve) != 0) {
		out.rgb.r = modulate_channel(parent.color.r, layer.red_func, parent_def.red_func, lut_index);
	}
	if ((parent.flags & GreenCurve) != 0) {
		out.rgb.g = modulate_channel(parent.color.g, layer.green_func, parent_def.green_func, lut_index);
	}
	if ((parent.flags & BlueCurve) != 0) {
		out.rgb.b = modulate_channel(parent.color.b, layer.blue_func, parent_def.blue_func, lut_index);
	}
	return out;
}

float random_sign(Emitter &e, const ParticleDef &def) noexcept {
	// SIGNEDROTATIONS (0x800000) pins the sign positive; otherwise
	// `2 * (rand() & 1) - 1` [orig: SpawnParticle @ 0x5e784c..0x5e785a].
	if ((def.flags & particle_flag::SignedRotations) != 0) {
		return 1.0f;
	}
	return (emitter_rand10(e) & 1u) != 0 ? 1.0f : -1.0f;
}

// `position`/`direction` are the spawn-call arguments (the emitter's own for a
// scheduled spawn, the parent particle's position/velocity for a child spawn,
// the hit position and +Y for a rotor-wash re-trigger); `time_offset` is the
// sub-frame pre-age; `parent` is set only for child spawns; `distribute` picks
// the sequential-azimuth helper (particles 2..N of a BURSTDISTRIBUTE burst);
// `force_zone_window` is the open spawn window (retail dword_29D6BB0), 0 when
// the spawn must search `forces` for its nearest zone.
bool emit_one_internal(Emitter &e, const ParticleDef &def, const Vec3 &position,
		const Vec3 &direction, float time_offset, const Particle *parent,
		const ParticleDef *parent_def, bool distribute,
		std::uint16_t force_zone_window, const ParticleForceField *forces) noexcept {
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
	// parse), not the def's — [orig: @ 0x5e77a0: graphic+0x198 * 255, truncated].
	p.alpha = static_cast<std::uint8_t>(clampf(layer.alpha * 255.0f, 0.0f, 255.0f));
	p.color_slot = static_cast<std::uint8_t>(pick_color_slot(e));
	p.color = color_for_slot(def, p.graphic_layer, p.color_slot);
	p.lifetime = def.age + emitter_rand_signed(e) * def.age_adj;
	// Roll seed = orientation.z + orientationadj.z * rand01 (degrees; we store
	// radians) [orig: @ 0x5e7803 — def+3824/+3836 into +0x3C]. Rate = roll_rot
	// family with a random sign flip unless SIGNEDROTATIONS pins it
	// [orig: @ 0x5e782a..0x5e7889 — def+3856/+3860, flag 0x800000 gate].
	p.rotation = (def.orientation.z + emitter_rand_unit(e) * def.orientationadj.z) * kDegreesToRadians;
	const float roll_sign = random_sign(e, def);
	p.rotation_rate = (def.roll_rot * roll_sign +
			emitter_rand_signed(e) * def.roll_rot_adj) * kDegreesToRadians;
	// Curve phase starts at 0 and sweeps to 256 across the lifetime
	// [orig: @ 0x5e788c/0x5e7898 — flt_7D1D70 (256.0) / age into +0x34, zero
	// into +0x30]. This is the LUT index clock, not a draw-size ramp.
	p.curve_phase = 0.0f;
	p.phase_rate = std::isfinite(p.lifetime) && p.lifetime != 0.0f ? 256.0f / p.lifetime : 0.0f;
	// Spawn position: (0, y_offset, 0) [orig: @ 0x5e78a1 — only def+3724 lands
	// in the position; z_offset is NOT positional — it becomes the render-side
	// camera-ward pull (emitter+0x140, seeded -z_offset in CEffectEmitter_Initialize
	// @ 0x5e6349)], then the shape offset, then the spawn-call `position`
	// [orig: SpawnParticle @ 0x5e7d30..0x5e7d4f].
	p.position = {0.0f, e.spawn_y_offset, 0.0f};
	apply_emission_shape(e, def, direction, p);
	// Velocity: EVERY shape runs the direction helper around the spawn call's
	// direction argument (spread max, spread_skip min), then multiplies by
	// `speed + speed_adj * rand_signed` [orig: SpawnParticle @ 0x5e7c41..0x5e7d1e
	// — the vtable helper, then def+3892/+3896 onto all three components].
	{
		const Vec3 dir = cone_direction(e, def, direction, def.spread, def.spread_skip, distribute);
		const float speed = def.speed + emitter_rand_signed(e) * def.speed_adj;
		p.velocity = vec3_scale(dir, speed);
	}
	p.serial = e.next_serial++;
	p.position = vec3_add(p.position, position);
	// The sub-frame offset pre-ages the particle and pre-advances its curve
	// phase; the position is NOT integrated by it [orig: SpawnParticle
	// @ 0x5e7d52..0x5e7d63].
	p.age = p.lifetime - time_offset;
	p.curve_phase = time_offset * p.phase_rate;

	if (parent != nullptr && parent_def != nullptr) {
		// The parent-inherit block [orig: SpawnParticle @ 0x5e7d66..0x5e7fdf]:
		// USEPARENTSCALE (0x20) copies the parent's base size, USEPARENTALPHA
		// (0x80) and USEPARENTCOLOR (0x40) take the parent's current
		// LUT-modulated alpha / RGB, USEPARENTROTATIONS (0x100000) copies the roll.
		const ModulatedColor inherited = parent_modulated_color(*parent, *parent_def);
		if ((def.flags & particle_flag::UseParentScale) != 0) {
			p.size = parent->size;
		}
		if ((def.flags & particle_flag::UseParentAlpha) != 0) {
			p.alpha = inherited.alpha;
		}
		if ((def.flags & particle_flag::UseParentColor) != 0) {
			p.color = inherited.rgb;
		}
		if ((def.flags & particle_flag::UseParentRotations) != 0) {
			p.rotation = parent->rotation;
		}
	}

	if (e.child_def != nullptr) {
		// The child-emission schedule record [orig: SpawnParticle
		// @ 0x5e7fe2..0x5e80a6]: rate/dur draws from the CHILD def, clock =
		// child emit_delay, interval = 1/rate, budget = (int)(rate * dur);
		// ONMYDEATH (0x10) instead arms the clock with this particle's (already
		// offset-reduced) life and budgets one child burst.
		const ParticleDef &child = *e.child_def;
		const float rate = child.emit_rate + emitter_rand_signed(e) * child.emit_rate_adj;
		const float duration = child.emit_dur + emitter_rand_signed(e) * child.emit_dur_adj;
		p.child_clock = child.emit_delay;
		p.child_interval = rate != 0.0f ? 1.0f / rate : kInfinity;
		const float budget = rate * duration;
		p.child_budget = std::isfinite(budget)
				? static_cast<std::int32_t>(clampf(budget, -2147483648.0f, 2147483520.0f))
				: 0;
		if ((def.flags & particle_flag::OnMyDeath) != 0) {
			p.child_clock = p.age;
			p.child_budget = child.emit_burst;
		}
	}

	// The super-system spawn tail [orig: CParticleEmitter_SpawnNewParticle
	// @ 0x5f35b0]: aux yaw/pitch seeds and rates (the YAWANDPITCH channel, the
	// pairs at aux+0/+4 and aux+8/+12), the per-particle ORBIT rate at
	// aux+16, then the zone bind, then CONTROLEDALLIGNMENT (0x400000) copying
	// the parent's yaw/pitch/roll @ 0x5f37f5..0x5f3804. Every def spawns
	// through this path (the base system is a stride-72 variant of the same
	// record), so the draws are unconditional.
	p.yaw = (def.orientation.x + emitter_rand_unit(e) * def.orientationadj.x) * kDegreesToRadians;
	const float yaw_sign = random_sign(e, def);
	p.yaw_rate = (emitter_rand_signed(e) * def.yaw_rot_adj +
			def.yaw_rot * yaw_sign) * kDegreesToRadians;
	p.pitch = (def.orientation.y + emitter_rand_unit(e) * def.orientationadj.y) * kDegreesToRadians;
	const float pitch_sign = random_sign(e, def);
	p.pitch_rate = (emitter_rand_signed(e) * def.pitch_rot_adj +
			def.pitch_rot * pitch_sign) * kDegreesToRadians;
	const float orbit_sign = random_sign(e, def);
	p.orbit_rate = (emitter_rand_signed(e) * def.orbitalspeed_adj +
			def.orbitalspeed * orbit_sign) * kDegreesToRadians;
	if (!std::isfinite(p.orbit_rate)) {
		p.orbit_rate = 0.0f;
	}
	// Every spawned particle binds its focal-wind zone once the position is
	// final: the open trigger window when there is one, else the nearest
	// containing zone [orig: CParticleEmitter_SpawnNewParticle @0x5F35B0 ->
	// Terrain_FindNearestAmbientSoundZone(particle+24) @0x5F37C6..0x5F37D8;
	// the window is dword_29D6BB0 @0x5CBCD3].
	p.force_zone = force_zone_window != 0 ? force_zone_window
			: (forces != nullptr ? forces->zone_at(p.position) : std::uint16_t{0});
	if ((def.flags & particle_flag::ControledAlignment) != 0 && parent != nullptr) {
		p.yaw = parent->yaw;
		p.pitch = parent->pitch;
		p.rotation = parent->rotation;
	}

	// Retail completes the spawn RNG path, then rejects a particle whose
	// offset-reduced life sits in [0, time_offset] (a plain zero life at
	// offset 0); other values, negative ones included, are inserted and reaped
	// by the next expiry pass [orig: SpawnParticle @ 0x5e80a9..0x5e80c1].
	if (!std::isfinite(p.age) || (p.age >= 0.0f && p.age <= time_offset)) {
		return false;
	}
	e.particles.push_back(p);
	return true;
}

// The AdvanceFrame expiry pass, before integration: a slot whose remaining
// age is below this frame's dt (fcomp dt vs age, keep on C0|C3 = dt <= age)
// is reclaimed by copying the LAST slot into the hole, decrementing the
// count, and re-examining the same index. INITIALYCLIP (0x02) additionally
// reclaims a particle that has fallen below the emitter's Y. The pool is
// therefore a dense array in that move-last order - never a stable erase - and
// every later consumer (the view-depth fill the batch quicksort runs over)
// walks slots 0..count-1 in exactly this order.
// [orig: CParticleEmitter_AdvanceFrame @ 0x5e6570 - flag test @ 0x5e682a,
//  compares @ 0x5e6840 / @ 0x5e684e / @ 0x5e68b0, memcpy(slot, last)
//  @ 0x5e687e / @ 0x5e68d9, --count @ 0x5e6883 / @ 0x5e68de, re-examine
//  @ 0x5e688c / @ 0x5e68e7;
//  CParticleEmitter_ComputeViewDepths @ 0x5e75cf walks 0..count-1]
void expire_dead(Emitter &e, float dt) noexcept {
	const bool initial_clip = e.def != nullptr &&
			(e.def->flags & particle_flag::InitialClip) != 0;
	std::size_t i = 0;
	while (i < e.particles.size()) {
		const Particle &p = e.particles[i];
		const bool expired = p.age < dt ||
				(initial_clip && p.position.y < e.position.y);
		if (expired) {
			e.particles[i] = e.particles.back();
			e.particles.pop_back();
			continue;
		}
		++i;
	}
}

// The parent's per-particle child scheduler, run at the top of AdvanceFrame
// before its own expiry pass [orig: CParticleEmitter_AdvanceFrame
// @ 0x5e6633..0x5e6807]. Per parent particle with budget: once the clock has
// expired within this frame, reload the interval (the child's emit-rate curve
// scales it by the byte at `(int)((int)parent.phase / 128) & 0xFF` — the
// literal retail index, read @ 0x5e66dc..0x5e670f — dividing the interval by
// that byte @ 0x5e6713..0x5e6727), spawn one child burst pre-aged by the time
// past the expiry, and repeat while the remaining time still exceeds the
// interval. The leftover is DROPPED when a burst fired (the clock reloads to
// a full interval @ 0x5e6727); it carries only when the burst count is zero.
void child_spawn_pass(Emitter &e, Emitter &child, float dt,
		const EmitterEnvironment &env) noexcept {
	const ParticleDef &child_def = *child.def;
	const bool distribute_bursts =
			(child_def.flags & particle_flag::BurstDistribute) != 0;
	const int burst = child_def.emit_burst;
	// Spawning appends to `child.particles` only, so indexing the parent's
	// vector by position stays valid across the loop.
	for (std::size_t index = 0; index < e.particles.size(); ++index) {
		Particle &p = e.particles[index];
		if (p.child_budget <= 0) {
			continue;
		}
		float t = dt - p.child_clock;
		if (t <= 0.0f) {
			p.child_clock -= dt;
			continue;
		}
		std::int32_t spawned = 0;
		for (;;) {
			float interval = p.child_interval;
			if (child_def.emit_rate_func.baked) {
				const std::int32_t phase = static_cast<std::int32_t>(p.curve_phase);
				const std::int32_t index_raw = static_cast<std::int32_t>(
						static_cast<float>(phase) * (1.0f / 128.0f));
				const std::size_t lut_index = static_cast<std::size_t>(index_raw & 0xFF);
				const float byte = static_cast<float>(child_def.emit_rate_func.baked_lut[lut_index]);
				interval = byte > 0.0f ? interval / byte : kInfinity;
			}
			p.child_clock = interval;
			const std::size_t pool_limit = std::min(child.max_particles, kEmitterHardParticleLimit);
			for (int b = 0; b < burst; ++b) {
				if (child.particles.size() >= pool_limit) {
					break;
				}
				const bool distribute = b > 0 && distribute_bursts;
				(void)emit_one_internal(child, child_def, p.position, p.velocity, t,
						&e.particles[index], e.def, distribute, child.force_zone, env.forces);
				++spawned;
			}
			if (interval > 0.0f && child.particles.size() < pool_limit && t - interval > 0.0f) {
				t -= interval;
				continue;
			}
			break;
		}
		Particle &record = e.particles[index];
		if (spawned != 0) {
			record.child_budget -= spawned;
		} else {
			record.child_clock -= t;
		}
	}
}

// The self-emission schedule [orig: CEffectEmitter_AdvanceEmission @ 0x5e1d30].
// `T = carry + dt` is consumed interval by interval: each burst fires while
// `T - interval > 0` (@ 0x5e1dba..0x5e1dc7), every particle of it pre-aged by
// the time left in the frame after the burst (the spawn's 4th argument
// @ 0x5e1e1c..0x5e1e3e), the budget dropping one per spawn unless FOREVEREMIT
// (@ 0x5e1e4a); then `T -= interval` and the interval reloads from the base
// interval divided by `lut[(int)clock & 0xFF] / 128` when the def's emit-rate
// curve resolved (@ 0x5e1e7a..0x5e1ec3), the base itself having been divided by
// `lut[0] / 128` at Initialize (@ 0x5e6302..0x5e6323). The loop also stops once
// the budget is spent (@ 0x5e1ec9) and the remaining T carries to the next frame
// (@ 0x5e1eea). The pre-advance stepping of POSITIONINTERPOLATE (0x20000,
// @ 0x5e1d72..0x5e1da1) is not ported.
void advance_emission(Emitter &e, const ParticleDef &def, float dt,
		const EmitterEnvironment &env) noexcept {
	float remaining = e.emit_carry + dt;
	if (!std::isfinite(remaining)) {
		remaining = 0.0f;
	}
	const bool forever = (def.flags & particle_flag::ForeverEmit) != 0;
	const bool distribute_bursts = (def.flags & particle_flag::BurstDistribute) != 0;
	const float base_interval = [&]() {
		if (!(e.emit_rate > 0.0f) || !std::isfinite(e.emit_rate)) {
			return kInfinity;
		}
		float base = 1.0f / e.emit_rate;
		if (def.emit_rate_func.baked) {
			const float first = static_cast<float>(def.emit_rate_func.baked_lut[0]) / 128.0f;
			base = first > 0.0f ? base / first : kInfinity;
		}
		return base;
	}();
	const int burst = def.emit_burst > 0 ? def.emit_burst : 1;
	const std::size_t pool_limit = std::min(e.max_particles, kEmitterHardParticleLimit);
	if (e.emit_budget > 0) {
		while (remaining - e.emit_interval > 0.0f) {
			const float time_offset = remaining - e.emit_interval;
			for (int b = 0; b < burst; ++b) {
				// Retail keeps calling into a full pool; the port stops the burst
				// there so an authored extreme burst cannot spin.
				if (e.particles.size() >= pool_limit) {
					break;
				}
				const bool distribute = b > 0 && distribute_bursts;
				(void)emit_one_internal(e, def, e.position, e.forward, time_offset,
						nullptr, nullptr, distribute, e.force_zone, env.forces);
				if (!forever) {
					--e.emit_budget;
				}
			}
			remaining -= e.emit_interval;
			float interval = base_interval;
			if (def.emit_rate_func.baked && std::isfinite(interval)) {
				const std::int32_t clock = static_cast<std::int32_t>(clampf(
						e.emit_clock, -2147483648.0f, 2147483520.0f));
				const std::size_t lut_index = clock >= 0
						? static_cast<std::size_t>(clock & 0xFF) : std::size_t{0};
				const float scale = static_cast<float>(def.emit_rate_func.baked_lut[lut_index]) / 128.0f;
				interval = scale > 0.0f ? interval / scale : kInfinity;
			}
			e.emit_interval = interval;
			if (e.emit_budget <= 0) {
				break;
			}
			// A zero/non-finite reload would never consume the carried time
			// (retail spins); one burst per frame is the bounded reading, and a
			// full pool ends the frame's emission the same way.
			if (!(e.emit_interval > 0.0f) || e.particles.size() >= pool_limit) {
				remaining = 0.0f;
				break;
			}
		}
	}
	e.emit_carry = remaining;
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

std::int32_t emitter_initial_budget(const Emitter &e) noexcept {
	if (e.def == nullptr) {
		return 0;
	}
	if ((e.def->flags & particle_flag::ForeverEmit) != 0 || !e.self_emitting) {
		return kEmitterForeverBudget;
	}
	const float total = e.emit_rate * e.emit_dur_total;
	if (!std::isfinite(total) || total <= 0.0f) {
		return 0;
	}
	const std::int32_t per_burst = static_cast<std::int32_t>(
			std::min(total, 2147483520.0f));
	const std::int64_t budget = static_cast<std::int64_t>(std::max(e.def->emit_burst, 0)) *
			static_cast<std::int64_t>(per_burst);
	return static_cast<std::int32_t>(std::min<std::int64_t>(budget, kEmitterForeverBudget));
}

void emitter_init(Emitter &e, const ParticleDef *def, Vec3 pos, std::uint32_t seed,
		Vec3 direction) {
	e.rng_state = seed != 0 ? seed : 0x9E3779B9u;
	e.def = def;
	e.child_def = nullptr;
	e.position = pos;
	e.prev_position = pos;
	e.bounds_min = pos;
	e.bounds_max = pos;
	e.bounds_valid = true;
	// Only EMITVECTOR adopts the spawn direction; every other def emits around
	// world +Y through the direction helper's fallback
	// [orig: CEffectEmitter_Initialize @ 0x5e60f9..0x5e612e].
	e.spawn_direction = direction;
	e.forward = {0.0f, 0.0f, 0.0f};
	if (def != nullptr && (def->flags & particle_flag::EmitVector) != 0) {
		e.forward = direction;
	}
	e.particles.clear();
	e.emit_carry = 0.0f;
	e.emit_heading = 0.0f;
	if (def != nullptr) {
		// Signed 10-bit draws for `emit_dur ± emit_dur_adj` then
		// `emit_rate ± emit_rate_adj` [orig: CEffectEmitter_Initialize
		// @ 0x5e621d..0x5e6283].
		const float duration = def->emit_dur + emitter_rand_signed(e) * def->emit_dur_adj;
		const float rate = def->emit_rate + emitter_rand_signed(e) * def->emit_rate_adj;
		e.emit_dur_total = std::isfinite(duration) ? duration : 0.0f;
		e.emit_rate = std::isfinite(rate) ? std::max(rate, 0.0f) : 0.0f;
		e.spawn_y_offset = def->y_offset;
		e.camera_pull = def->z_offset;
		// The interval slot starts at emit_delay — that IS the delay
		// [orig: @ 0x5e62ee..0x5e62f4]; the emit-rate curve phase spans
		// `delay + dur` seconds, seeded negative by the delay
		// [orig: @ 0x5e635b..0x5e6379].
		e.emit_interval = std::isfinite(def->emit_delay) ? std::max(def->emit_delay, 0.0f) : 0.0f;
		const float span = e.emit_interval + e.emit_dur_total;
		if (std::isfinite(span) && span > 0.0f) {
			e.emit_clock_rate = 256.0f / span;
			e.emit_clock = -e.emit_interval * span / 256.0f;
		} else {
			e.emit_clock_rate = 0.0f;
			e.emit_clock = 0.0f;
		}
	} else {
		e.emit_dur_total = 0.0f;
		e.emit_rate = 0.0f;
		e.spawn_y_offset = 0.0f;
		e.camera_pull = 0.0f;
		e.emit_interval = 0.0f;
		e.emit_clock_rate = 0.0f;
		e.emit_clock = 0.0f;
	}
	e.age = 0.0f;
	// gravity x flt_7DC738 (-0.0981) and drag x flt_7C56A8 (0.01)
	// [orig: CEffectEmitter_Initialize @ 0x5e6273..0x5e6299].
	e.gravity_accel = def != nullptr && std::isfinite(def->gravity)
			? def->gravity * -0.0981f : 0.0f;
	e.drag_coefficient = def != nullptr && std::isfinite(def->drag)
			? def->drag * 0.01f : 0.0f;
	e.next_serial = 0;
	e.active = def != nullptr;
	e.emit_budget = emitter_initial_budget(e);
	e.last_translation_delta = {0.0f, 0.0f, 0.0f};
	e.cumulative_translation = {0.0f, 0.0f, 0.0f};
	// Note: `spring_const`, the kill plane and `self_emitting`
	// are NOT reset here — they are caller-set scalars (engine equivalents
	// come from the spawn descriptor or the manager), and resetting them on
	// every `play()` / `restart()` would clobber the caller's intent.
}

void emitter_translate(Emitter &e, Vec3 new_pos) noexcept {
	// CParticleEmitter_TranslatePosition @ 0x5efe90: compute delta, update
	// position, accumulate.
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
	// stay attached to the emitter — the update adds the emitter's frame delta
	// to every alive particle [orig: CParticleEmitter_UpdateAllParticles
	// @ 0x5f3be0, the `pos - prev_pos` add]. Default (flag clear) is engine
	// behaviour where particles render in world space and are "left behind"
	// when the emitter moves.
	if (e.def != nullptr && (e.def->flags & particle_flag::PositionRelative) != 0) {
		for (Particle &p : e.particles) {
			p.position.x += delta.x;
			p.position.y += delta.y;
			p.position.z += delta.z;
		}
	}
}

bool emitter_spawn_one(Emitter &e, const ParticleForceField *forces) {
	const std::size_t pool_limit = std::min(e.max_particles, kEmitterHardParticleLimit);
	if (e.def == nullptr || e.particles.size() >= pool_limit) {
		return false;
	}
	return emit_one_internal(e, *e.def, e.position, e.forward, 0.0f, nullptr, nullptr,
			false, e.force_zone, forces);
}

bool emitter_spawn_one_at(Emitter &e, Vec3 position, Vec3 forward,
		std::uint16_t force_zone_window, const ParticleForceField *forces) {
	const std::size_t pool_limit = std::min(e.max_particles, kEmitterHardParticleLimit);
	if (e.def == nullptr || e.particles.size() >= pool_limit) {
		return false;
	}
	return emit_one_internal(e, *e.def, position, forward, 0.0f, nullptr, nullptr,
			false, force_zone_window, forces);
}

bool emitter_alive(const Emitter &e) noexcept {
	// [orig: CParticleEmitter_IsAliveOrEmitting @ 0x5e2640]: alive while the
	// budget word holds and (the curve clock is under 256 or the emitter is
	// not self-emitting), else while live particles remain.
	if (e.def == nullptr || !e.active) {
		return false;
	}
	if (e.emit_budget > 0 && (e.emit_clock < 256.0f || !e.self_emitting)) {
		return true;
	}
	return !e.particles.empty();
}

void emitter_advance(Emitter &e, float dt, const EmitterEnvironment &env, Emitter *child,
		bool group_visible) {
	if (!e.active || e.def == nullptr || !std::isfinite(dt) || dt <= 0.0f) {
		return;
	}
	const ParticleDef &def = *e.def;

	// NOVISNOUPDATE (0x01): an emitter that still has budget and whose live
	// bounds (the emitter position when empty) fall outside the view is not
	// advanced at all this frame — retail freezes it until it is seen again
	// [orig: CParticleEmitter_AdvanceFrame @ 0x5e6588..0x5e65f3]. A group the
	// section gate hides takes that same frozen path before any frustum test
	// [orig: the group+0x6C test @ 0x5e65c9..0x5e65d0 -> the aliveness return
	// @ 0x5e65e5..0x5e65ec]. Without a camera (headless) the frustum leg is
	// skipped and the emitter advances.
	if ((def.flags & particle_flag::NoVisNoUpdate) != 0) {
		if (e.particles.empty() || !e.bounds_valid) {
			e.bounds_min = e.position;
			e.bounds_max = e.position;
			e.bounds_valid = true;
		}
		if (e.emit_budget > 0 &&
				(!group_visible ||
						(env.frustum != nullptr && env.frustum->valid &&
								!emitter_bounds_visible(e, *env.frustum)))) {
			return;
		}
	}

	// Child spawns run first, from the previous frame's particle state
	// [orig: AdvanceFrame @ 0x5e6622..0x5e6807].
	if (child != nullptr && child->def != nullptr && child->active && !e.particles.empty()) {
		child_spawn_pass(e, *child, dt, env);
	}

	// Retail's expiry pass reclaims every slot that would cross zero during
	// THIS integration (age < dt); a particle is never integrated past its
	// lifetime, and one that reaches exactly zero remains observable for that
	// terminal frame.
	expire_dead(e, dt);

	// Physics integration + aging, accumulating the live AABB
	// [orig: UpdateParticles @ 0x5e6980 / UpdateAllParticles @ 0x5f3be0].
	bounds_reset(e);
	for (std::size_t index = 0; index < e.particles.size(); ++index) {
		Particle &p = e.particles[index];
		integrate_particle(p, def, e, dt, env, index);
		bounds_include(e, p.position);
	}
	apply_kill_plane(e);
	if (!e.bounds_valid) {
		e.bounds_min = e.position;
		e.bounds_max = e.position;
		e.bounds_valid = true;
	}

	// Self-emission after the update, so a particle spawned this frame is
	// pre-aged by its sub-frame offset rather than integrated
	// [orig: AdvanceFrame @ 0x5e690e..0x5e6928 gates the vtable+20 call on
	//  emitter+0x104].
	if (e.self_emitting) {
		advance_emission(e, def, dt, env);
	}

	// The emit-rate curve clock and the frame bookkeeping
	// [orig: AdvanceFrame @ 0x5e692a..0x5e6951].
	e.emit_clock += e.emit_clock_rate * dt;
	e.age += dt;
	e.prev_position = e.position;
}

Vec3 mission_wind_vector(int wind_speed, int wind_direction_degrees) noexcept {
	// [orig: Weather_SetMissionWind @ 0x5de970]: the per-tick fixed-point magnitude is
	// `65536000 * speed / 60 / 60 / 65` (integer steps), pointed along the
	// compass heading `90 - direction` degrees in the game's horizontal plane
	// (x = cos, y = sin, up = 0); Render_EmitterEffect @ 0x5f70c0 converts
	// that vector to the effect frame and multiplies by 62 (ticks per second).
	// The port's effect frame keeps game x, maps game up to y and negates game
	// y into z (world::render_float_from_fixed's swizzle).
	const std::int64_t magnitude_fixed =
			((static_cast<std::int64_t>(65536000) * wind_speed) / 60) / 60 / 65;
	const float magnitude = static_cast<float>(magnitude_fixed) / 65536.0f * 62.0f;
	const float heading = static_cast<float>(90 - wind_direction_degrees) * kDegreesToRadians;
	return {magnitude * std::cos(heading), 0.0f, -magnitude * std::sin(heading)};
}

} // namespace opennova::particle
