// Emit-shape geometry parity. Engine reference:
// CParticleEmitter_SpawnParticle @ 0x5e7640 — switch on def.emit_shape
// (def+3924, read @ 0x5e78ef). Every shape displaces the spawn POSITION:
//   Box (1) [@ 0x5e7900..0x5e795e]: one dominant axis (rand % 3) pushed
//               one-sided into [skip/2, size/2] (sign random unless
//               SIGNEDROTATIONS pins it positive @ 0x5e790f); the other two
//               axes get ±size/2 uniform.
//   Sphere (2): the direction helper with polar range [0, 360] × per-axis
//               annular lerp(skip, size, r) magnitude added to POSITION — a
//               hollow ellipsoidal shell.
//   Cone (3):   the direction helper pinned to polar 90 (flt_7DCBF0 for both
//               bounds @ 0x5e7a33..0x5e7a48) — a RING around the spawn
//               direction, annular magnitude, added to POSITION.
// Velocity is seeded separately for ALL shapes: the direction helper draws a
// polar angle uniform in [spread_skip, spread] and a uniform azimuth around
// the spawn direction, × (speed + speed_adj × rand_signed)
// [orig: CParticleSystem_ComputeConeDirectionVector @ 0x5e1f10 / CParticleSystem_GenerateRandomDirectionBasis
//  @ 0x5e22a0; the post-switch call + def+3892/+3896 multiply @ 0x5e7c41..0x5e7d1e].
// We test the geometric properties (dominance, range bounds, shell radii)
// rather than byte-exact matches against the FPU stream.

#include <runtime/particle/emitter.h>
#include <formats/particle/particle.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near_value(float actual, float expected, float epsilon = 0.001f) {
	return std::fabs(actual - expected) <= epsilon;
}

opennova::particle::ParticleDef base_def() {
	using namespace opennova::particle;
	ParticleDef def;
	def.id = "shape";
	def.emit_dur = 1.0f;
	def.emit_rate = 0.0f;
	def.emit_burst = 1;
	def.age = 5.0f;
	def.alpha = 1.0f;
	def.color1 = def.color2 = def.color3 = def.color4 = {255, 255, 255};
	GraphicLayer &g0 = def.graphics[0];
	g0.present = true;
	g0.index = 1;
	return def;
}

bool test_box_picks_one_dominant_axis() {
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Box);
	def.emit_shape_size = {4.0f, 4.0f, 4.0f};
	// skip == size pins the dominant-axis offset to exactly size/2 and the
	// dominant "grow" term (rand01 × (size − skip) / 2) to zero.
	def.emit_shape_size_skip = {4.0f, 4.0f, 4.0f};

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0xC0DE);
	int dominant_x = 0, dominant_y = 0, dominant_z = 0;
	for (int i = 0; i < 200; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 pos = e.particles[0].position;
		// The chosen axis sits at exactly ±size/2 = ±2.0; the other two are
		// uniform in ±size/2, so |axis| == 2.0 identifies the dominant one
		// (measure-zero ties aside — the LCG never lands exactly on ±1023/1023
		// twice in this loop).
		if (std::abs(std::abs(pos.x) - 2.0f) < 0.01f) ++dominant_x;
		else if (std::abs(std::abs(pos.y) - 2.0f) < 0.01f) ++dominant_y;
		else if (std::abs(std::abs(pos.z) - 2.0f) < 0.01f) ++dominant_z;
		const bool in_box = std::abs(pos.x) <= 2.01f && std::abs(pos.y) <= 2.01f &&
				std::abs(pos.z) <= 2.01f;
		if (!expect(in_box, "box spawn stays within the ±size/2 half-extents")) {
			std::fprintf(stderr, "  iter=%d pos=(%f,%f,%f)\n", i, pos.x, pos.y, pos.z);
			return false;
		}
	}
	const int total = dominant_x + dominant_y + dominant_z;
	if (!expect(total == 200, "every box spawn pins one dominant axis at ±size/2")) {
		std::fprintf(stderr, "  matched=%d (x=%d y=%d z=%d)\n", total, dominant_x, dominant_y, dominant_z);
		return false;
	}
	if (!expect(dominant_x > 30 && dominant_y > 30 && dominant_z > 30,
			"all three axes get picked over 200 trials")) {
		std::fprintf(stderr, "  x=%d y=%d z=%d\n", dominant_x, dominant_y, dominant_z);
		return false;
	}
	return true;
}

bool test_box_dominant_axis_spans_skip_to_size() {
	// With skip < size, the dominant axis lands one-sided in [skip/2, size/2]
	// [orig: @ 0x5e7939 initial skip/2 + the sign-matched rand01 × (size−skip)/2
	// grow term].
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Box);
	def.emit_shape_size = {6.0f, 6.0f, 6.0f};
	def.emit_shape_size_skip = {2.0f, 2.0f, 2.0f};

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0xB0B0);
	for (int i = 0; i < 200; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 pos = e.particles[0].position;
		const float ax = std::max({std::abs(pos.x), std::abs(pos.y), std::abs(pos.z)});
		if (!expect(ax >= 1.0f - 0.01f && ax <= 3.0f + 0.01f,
				"dominant axis magnitude within [skip/2, size/2]")) {
			std::fprintf(stderr, "  iter=%d pos=(%f,%f,%f) ax=%f\n", i, pos.x, pos.y, pos.z, ax);
			return false;
		}
	}
	return true;
}

bool test_box_signedrotations_flag_clamps_sign_positive() {
	// SIGNEDROTATIONS (0x800000) forces the dominant-axis sign to +1 instead
	// of the random ±1 from `rand() & 1` [orig: @ 0x5e790f].
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Box);
	def.emit_shape_size = {6.0f, 6.0f, 6.0f};
	def.emit_shape_size_skip = {6.0f, 6.0f, 6.0f};
	def.flags = particle_flag::SignedRotations;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 7);
	for (int i = 0; i < 100; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 pos = e.particles[0].position;
		// Whichever axis was chosen, its sign must be +1 (it sits at +size/2).
		const float chosen = std::max({pos.x, pos.y, pos.z});
		if (!expect(chosen >= 2.99f, "SIGNEDROTATIONS keeps the dominant-axis sign positive")) {
			std::fprintf(stderr, "  iter=%d pos=(%f,%f,%f)\n", i, pos.x, pos.y, pos.z);
			return false;
		}
	}
	return true;
}

bool test_sphere_position_shell_within_skip_size_range() {
	// Sphere displaces POSITION into a hollow shell: |pos| ∈ [skip, size]
	// (per-axis annular lerp over a unit direction).
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Sphere);
	def.emit_shape_size = {2.0f, 2.0f, 2.0f};
	def.emit_shape_size_skip = {1.0f, 1.0f, 1.0f};

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0xBEEF);
	for (int i = 0; i < 200; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 pos = e.particles[0].position;
		const float radius = std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
		if (!expect(radius >= 0.9f && radius <= 2.05f,
				"sphere spawn radius within the annular [skip, size] shell")) {
			std::fprintf(stderr, "  iter=%d pos=(%f,%f,%f) r=%f\n", i, pos.x, pos.y, pos.z, radius);
			return false;
		}
		const Vec3 v = e.particles[0].velocity;
		if (!expect(v.x == 0.0f && v.y == 0.0f && v.z == 0.0f,
				"shape displaces position, not velocity (def.speed == 0)")) {
			return false;
		}
	}
	return true;
}

bool test_velocity_from_speed_within_spread_cone() {
	// Velocity is direction-within-def.spread around emitter.forward ×
	// (speed + speed_adj × rand_signed) — independent of the emit shape.
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Point);
	def.speed = 10.0f;
	def.speed_adj = 2.0f;
	def.spread = 15.0f; // 15° half-angle

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0xCAFE);
	e.forward = {0.0f, 1.0f, 0.0f};
	const float half_angle_rad = 15.0f * 0.0174533f;
	// Allow some slack since our (yaw, pitch) parameterization picks a square
	// region rather than a disc; the worst case is `sqrt(2) * half_angle`.
	const float allowed_cos = std::cos(half_angle_rad * 1.5f);

	for (int i = 0; i < 100; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 v = e.particles[0].velocity;
		const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		if (!expect(speed >= 8.0f - 0.05f && speed <= 12.0f + 0.05f,
				"velocity magnitude = speed ± speed_adj")) {
			std::fprintf(stderr, "  iter=%d speed=%f\n", i, speed);
			return false;
		}
		const float dot = (v.x * e.forward.x + v.y * e.forward.y + v.z * e.forward.z) / speed;
		if (!expect(dot > allowed_cos,
				"velocity direction stays within the widened spread half-angle")) {
			std::fprintf(stderr, "  iter=%d dot=%f allowed=%f v=(%f,%f,%f)\n",
					i, dot, allowed_cos, v.x, v.y, v.z);
			return false;
		}
	}
	return true;
}

bool test_velocity_spread_skip_excludes_inner_cone() {
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.speed = 10.0f;
	def.spread = 90.0f;
	def.spread_skip = 90.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0x5150);
	e.forward = {0.0f, 1.0f, 0.0f};
	for (int i = 0; i < 64; ++i) {
		e.particles.clear();
		if (!emitter_spawn_one(e)) return false;
		const Vec3 v = e.particles[0].velocity;
		const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		const float forward_dot = v.y / speed;
		if (!expect(std::fabs(forward_dot) < 0.001f, __func__)) return false;
	}
	return true;
}

bool test_velocity_spread_skip_pins_the_polar_angle() {
	// spread_skip == spread collapses the helper's polar range to one angle:
	// every direction sits on the 60-degree cone around the axis.
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.speed = 10.0f;
	def.spread = 60.0f;
	def.spread_skip = 60.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0x6161);
	e.forward = {0.0f, 1.0f, 0.0f};
	const float expected_forward_dot = 0.5f; // cos(60 degrees)
	for (int i = 0; i < 32; ++i) {
		e.particles.clear();
		if (!emitter_spawn_one(e)) return false;
		const Vec3 v = e.particles[0].velocity;
		const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		const float forward_dot = v.y / speed;
		if (!expect(std::fabs(forward_dot - expected_forward_dot) < 0.001f, __func__)) {
			std::fprintf(stderr, "  iter=%d dot=%f\n", i, forward_dot);
			return false;
		}
	}
	return true;
}

bool test_cone_position_cap_around_forward() {
	// Cone displaces POSITION onto the ring perpendicular to the spawn
	// direction (both helper bounds are flt_7DCBF0 = 90.0) — every offset is
	// orthogonal to the axis at the annular radius.
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Cone);
	def.emit_shape_size = {1.0f, 1.0f, 1.0f};
	def.emit_shape_size_skip = {1.0f, 1.0f, 1.0f}; // fixed magnitude 1

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0xF00D);
	e.forward = {0.0f, 1.0f, 0.0f};
	for (int i = 0; i < 100; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 pos = e.particles[0].position;
		const float radius = std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
		if (!expect(std::fabs(radius - 1.0f) < 0.001f, "cone offset sits at the annular radius")) {
			std::fprintf(stderr, "  iter=%d pos=(%f,%f,%f) r=%f\n", i, pos.x, pos.y, pos.z, radius);
			return false;
		}
		const float dot = pos.y / radius;
		if (!expect(std::fabs(dot) < 0.001f, "cone spawn offset is a ring perpendicular to the axis")) {
			std::fprintf(stderr, "  iter=%d pos=(%f,%f,%f) dot=%f\n", i, pos.x, pos.y, pos.z, dot);
			return false;
		}
	}
	return true;
}

bool test_burst_distribute_spaces_the_azimuth_evenly() {
	// BURSTDISTRIBUTE (0x2000000) routes particles 2..N of a burst through
	// the sequential helper: azimuth += 2*pi / emit_burst per particle, so a
	// burst of 4 at a pinned polar angle lands on four quarter-turn headings
	// [orig: CEffectEmitter_AdvanceEmission @ 0x5e1e0d..0x5e1e3e ->
	//  CParticleSystem_ComputeConeDirectionVector @ 0x5e1f6e].
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.speed = 10.0f;
	def.spread = 90.0f;
	def.spread_skip = 90.0f;
	def.emit_burst = 4;
	def.emit_rate = 1.0f;
	def.emit_dur = 10.0f;
	def.age = 10.0f;
	def.flags |= particle_flag::BurstDistribute;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0x7777);
	e.forward = {0.0f, 1.0f, 0.0f};
	emitter_advance(e, 0.016f);
	if (!expect(e.particles.size() == 4, "one burst of four")) {
		std::fprintf(stderr, "  got %zu\n", e.particles.size());
		return false;
	}
	for (int i = 0; i < 4; ++i) {
		const Vec3 a = e.particles[static_cast<std::size_t>(i)].velocity;
		const Vec3 b = e.particles[static_cast<std::size_t>((i + 1) % 4)].velocity;
		const float dot = (a.x * b.x + a.z * b.z) / 100.0f;
		if (!expect(std::fabs(dot) < 0.01f, "consecutive burst members are a quarter turn apart")) {
			std::fprintf(stderr, "  i=%d dot=%f\n", i, dot);
			return false;
		}
	}
	return true;
}

bool test_emit_vector_selects_the_emission_axis() {
	// Only EMITVECTOR (0x10000) defs emit around the spawn direction; every
	// other def's axis is zero and the helper's fallback emits around world
	// +Y [orig: CEffectEmitter_Initialize @ 0x5e60f9..0x5e612e;
	//  CParticleSystem_ComputeConeDirectionVector @ 0x5e203a].
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.speed = 10.0f;
	def.spread = 0.0f;
	def.spread_skip = 0.0f;
	Emitter plain;
	emitter_init(plain, &def, {0, 0, 0}, 3, {1.0f, 0.0f, 0.0f});
	if (!expect(plain.forward.x == 0.0f && plain.forward.y == 0.0f && plain.forward.z == 0.0f,
			"a plain def keeps a zero axis")) return false;
	if (!emitter_spawn_one(plain)) return false;
	if (!expect(near_value(plain.particles[0].velocity.y, 10.0f), "and emits straight up")) {
		const Vec3 v = plain.particles[0].velocity;
		std::fprintf(stderr, "  v=(%f,%f,%f)\n", v.x, v.y, v.z);
		return false;
	}
	def.flags |= particle_flag::EmitVector;
	Emitter vectored;
	emitter_init(vectored, &def, {0, 0, 0}, 3, {1.0f, 0.0f, 0.0f});
	if (!emitter_spawn_one(vectored)) return false;
	if (!expect(near_value(vectored.particles[0].velocity.x, 10.0f), "EMITVECTOR emits along the spawn direction")) {
		const Vec3 v = vectored.particles[0].velocity;
		std::fprintf(stderr, "  v=(%f,%f,%f)\n", v.x, v.y, v.z);
		return false;
	}
	return true;
}

} // namespace

int main() {
	int failures = 0;
	if (!test_box_picks_one_dominant_axis())                    ++failures;
	if (!test_box_dominant_axis_spans_skip_to_size())           ++failures;
	if (!test_box_signedrotations_flag_clamps_sign_positive())  ++failures;
	if (!test_sphere_position_shell_within_skip_size_range())   ++failures;
	if (!test_velocity_from_speed_within_spread_cone())         ++failures;
	if (!test_velocity_spread_skip_excludes_inner_cone())       ++failures;
	if (!test_velocity_spread_skip_pins_the_polar_angle())     ++failures;
	if (!test_cone_position_cap_around_forward())               ++failures;
	if (!test_burst_distribute_spaces_the_azimuth_evenly())     ++failures;
	if (!test_emit_vector_selects_the_emission_axis())          ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
