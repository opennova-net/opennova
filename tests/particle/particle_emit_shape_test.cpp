// Emit-shape geometry parity. Engine reference:
// CParticleEmitter_SpawnParticle @ 0x5e7640 — switch on def.emit_shape:
//   Box (1):    one-axis dominant impulse (±emit_shape_size[axis]) with the
//               other two axes inset by emit_shape_size_skip range.
//   Sphere (2): random unit direction × per-axis annular lerp(skip, size, rand)
//               magnitude, applied as velocity.
//   Cone (3):   spread half-angle around emitter.forward, per-axis annular
//               velocity scaling.
// We test the geometric properties (one-axis dominance, range bounds, cone
// half-angle) rather than byte-exact matches against the FPU stream.

#include <particle/emitter.h>
#include <particle/particle.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
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
	def.emit_shape_size = {2.0f, 2.0f, 2.0f};
	def.emit_shape_size_skip = {0.0f, 0.0f, 0.0f};

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 0xC0DE);
	int dominant_x = 0, dominant_y = 0, dominant_z = 0;
	for (int i = 0; i < 200; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 pos = e.particles[0].position;
		// Skip == 0 means non-chosen axes get exactly 0 offset; chosen axis is
		// ±2.0. So |pos[chosen]| == 2.0 and the other two are 0.
		if (std::abs(std::abs(pos.x) - 2.0f) < 0.01f && std::abs(pos.y) < 0.01f && std::abs(pos.z) < 0.01f) ++dominant_x;
		else if (std::abs(std::abs(pos.y) - 2.0f) < 0.01f && std::abs(pos.x) < 0.01f && std::abs(pos.z) < 0.01f) ++dominant_y;
		else if (std::abs(std::abs(pos.z) - 2.0f) < 0.01f && std::abs(pos.x) < 0.01f && std::abs(pos.y) < 0.01f) ++dominant_z;
	}
	const int total = dominant_x + dominant_y + dominant_z;
	if (!expect(total == 200, "every box spawn picks exactly one dominant axis at ±size")) {
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

bool test_box_oneframe_flag_clamps_sign_positive() {
	// `OneFrame` (def.flags bit 23 = 0x800000) forces the chosen-axis sign to
	// +1 instead of the random ±1 from `rand() & 1`.
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Box);
	def.emit_shape_size = {3.0f, 3.0f, 3.0f};
	def.emit_shape_size_skip = {0.0f, 0.0f, 0.0f};
	def.flags = particle_flag::OneFrame;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 7);
	for (int i = 0; i < 100; ++i) {
		e.particles.clear();
		emitter_spawn_one(e);
		const Vec3 pos = e.particles[0].position;
		// Whichever axis was chosen, its sign must be +1.
		const float chosen = std::max({pos.x, pos.y, pos.z});
		if (!expect(chosen >= 2.99f, "OneFrame keeps chosen-axis sign positive")) {
			std::fprintf(stderr, "  iter=%d pos=(%f,%f,%f)\n", i, pos.x, pos.y, pos.z);
			return false;
		}
	}
	return true;
}

bool test_sphere_velocity_per_axis_within_skip_size_range() {
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
		const Vec3 v = e.particles[0].velocity;
		// Each axis: |v[k]| = |unit[k]| * lerp(1, 2, r) so |v[k]| ∈ [0, 2].
		// |v[k]| is unit[k] (in [-1, 1]) times scale (in [1, 2]) — so |v[k]|
		// ≤ 2.0 always, and the magnitude of the velocity vector should be in
		// [1, 2] roughly (since unit is normalized).
		const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		if (!expect(speed >= 0.9f && speed <= 2.05f,
				"sphere velocity magnitude within annular range")) {
			std::fprintf(stderr, "  iter=%d v=(%f,%f,%f) speed=%f\n", i, v.x, v.y, v.z, speed);
			return false;
		}
	}
	return true;
}

bool test_cone_velocity_within_half_angle() {
	using namespace opennova::particle;
	ParticleDef def = base_def();
	def.emit_shape = static_cast<int>(EmitShape::Cone);
	def.emit_shape_size = {1.0f, 1.0f, 1.0f};
	def.emit_shape_size_skip = {1.0f, 1.0f, 1.0f}; // fixed magnitude 1
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
		if (speed <= 1e-3f) continue;
		const float dot = (v.x * e.forward.x + v.y * e.forward.y + v.z * e.forward.z) / speed;
		if (!expect(dot > allowed_cos,
				"cone velocity stays within widened half-angle of forward")) {
			std::fprintf(stderr, "  iter=%d dot=%f allowed=%f v=(%f,%f,%f)\n",
					i, dot, allowed_cos, v.x, v.y, v.z);
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	int failures = 0;
	if (!test_box_picks_one_dominant_axis())                    ++failures;
	if (!test_box_oneframe_flag_clamps_sign_positive())         ++failures;
	if (!test_sphere_velocity_per_axis_within_skip_size_range()) ++failures;
	if (!test_cone_velocity_within_half_angle())                ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
