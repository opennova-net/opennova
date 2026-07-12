// emitter_translate primitive parity. Engine reference:
// CParticleEmitter_TranslatePosition @ 0x5efe90 — compute (new - old),
// update position, accumulate delta into the emitter's AABB-equivalent
// accumulator. Our portable form exposes the two deltas (per-frame +
// cumulative since init) without the AABB plumbing the engine maintains
// in `UpdateAllParticles @ 0x5f3be0`.

#include <particle/emitter.h>
#include <particle/particle.h>

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.001f) {
	return std::fabs(actual - expected) <= epsilon;
}

opennova::particle::ParticleDef minimal_def() {
	using namespace opennova::particle;
	ParticleDef def;
	def.id = "translate_test";
	def.emit_dur = 1.0f;
	def.emit_rate = 0.0f;  // no auto-spawn
	def.emit_burst = 1;
	def.age = 5.0f;
	def.alpha = 1.0f;
	def.color1 = def.color2 = def.color3 = def.color4 = {255, 255, 255};
	def.graphics[0].present = true;
	def.graphics[0].index = 1;
	return def;
}

bool test_translate_updates_position_and_delta() {
	using namespace opennova::particle;
	ParticleDef def = minimal_def();
	Emitter e;
	emitter_init(e, &def, {1.0f, 2.0f, 3.0f}, 1);

	if (!expect(near(e.position.x, 1.0f) && near(e.position.y, 2.0f) && near(e.position.z, 3.0f),
			"init seeds position from spawn arg")) return false;
	if (!expect(near(e.last_translation_delta.x, 0.0f) && near(e.last_translation_delta.y, 0.0f) && near(e.last_translation_delta.z, 0.0f),
			"init zeros last_translation_delta")) return false;
	if (!expect(near(e.cumulative_translation.x, 0.0f) && near(e.cumulative_translation.y, 0.0f) && near(e.cumulative_translation.z, 0.0f),
			"init zeros cumulative_translation")) return false;

	emitter_translate(e, {5.0f, 8.0f, -1.0f});

	if (!expect(near(e.position.x, 5.0f) && near(e.position.y, 8.0f) && near(e.position.z, -1.0f),
			"translate updates position to new_pos")) return false;
	if (!expect(near(e.prev_position.x, 1.0f) && near(e.prev_position.y, 2.0f) && near(e.prev_position.z, 3.0f),
			"translate stores old position in prev_position")) return false;
	if (!expect(near(e.last_translation_delta.x, 4.0f) && near(e.last_translation_delta.y, 6.0f) && near(e.last_translation_delta.z, -4.0f),
			"last_translation_delta = new_pos - old_pos")) return false;
	if (!expect(near(e.cumulative_translation.x, 4.0f) && near(e.cumulative_translation.y, 6.0f) && near(e.cumulative_translation.z, -4.0f),
			"first translate seeds cumulative_translation with the delta")) return false;
	return true;
}

bool test_translate_accumulates_across_frames() {
	using namespace opennova::particle;
	ParticleDef def = minimal_def();
	Emitter e;
	emitter_init(e, &def, {0.0f, 0.0f, 0.0f}, 1);

	emitter_translate(e, {1.0f, 0.0f, 0.0f});      // delta = (+1, 0, 0)
	emitter_translate(e, {1.0f, 5.0f, 2.0f});      // delta = (0, +5, +2)
	emitter_translate(e, {-2.0f, 5.0f, 2.0f});     // delta = (-3, 0, 0)

	// Cumulative = (+1, 0, 0) + (0, +5, +2) + (-3, 0, 0) = (-2, 5, 2)
	if (!expect(near(e.cumulative_translation.x, -2.0f) && near(e.cumulative_translation.y, 5.0f) && near(e.cumulative_translation.z, 2.0f),
			"cumulative_translation = sum of all deltas since init")) {
		std::fprintf(stderr, "  cumulative=(%f,%f,%f) want (-2, 5, 2)\n",
				e.cumulative_translation.x, e.cumulative_translation.y, e.cumulative_translation.z);
		return false;
	}
	// Last delta = the third translation's delta = (-3, 0, 0)
	if (!expect(near(e.last_translation_delta.x, -3.0f) && near(e.last_translation_delta.y, 0.0f) && near(e.last_translation_delta.z, 0.0f),
			"last_translation_delta reflects only the most recent translation")) {
		std::fprintf(stderr, "  last_delta=(%f,%f,%f) want (-3, 0, 0)\n",
				e.last_translation_delta.x, e.last_translation_delta.y, e.last_translation_delta.z);
		return false;
	}
	if (!expect(near(e.position.x, -2.0f) && near(e.position.y, 5.0f) && near(e.position.z, 2.0f),
			"position reflects the latest translate target")) return false;
	return true;
}

bool test_translate_zero_delta_is_safe() {
	using namespace opennova::particle;
	ParticleDef def = minimal_def();
	Emitter e;
	emitter_init(e, &def, {3.0f, 4.0f, 5.0f}, 1);

	// Translate to a non-trivial position first, then to the same position.
	emitter_translate(e, {7.0f, 4.0f, 5.0f});
	const Vec3 cum_before = e.cumulative_translation;
	emitter_translate(e, {7.0f, 4.0f, 5.0f});

	if (!expect(near(e.last_translation_delta.x, 0.0f) && near(e.last_translation_delta.y, 0.0f) && near(e.last_translation_delta.z, 0.0f),
			"zero delta translate yields zero last_translation_delta")) return false;
	if (!expect(near(e.cumulative_translation.x, cum_before.x) && near(e.cumulative_translation.y, cum_before.y) && near(e.cumulative_translation.z, cum_before.z),
			"zero delta translate leaves cumulative_translation unchanged")) return false;
	if (!expect(near(e.prev_position.x, 7.0f) && near(e.prev_position.y, 4.0f) && near(e.prev_position.z, 5.0f),
			"prev_position equals the (unchanged) current position after zero translate")) return false;
	return true;
}

bool test_translate_position_relative_carries_particles() {
	// Engine flag PositionRelative (bit 18): when set, alive particles
	// translate WITH the emitter. Spawn one particle, translate the emitter,
	// verify the particle's position shifted by the same delta.
	using namespace opennova::particle;
	ParticleDef def = minimal_def();
	def.flags = particle_flag::PositionRelative;
	Emitter e;
	emitter_init(e, &def, {0.0f, 0.0f, 0.0f}, 1);
	emitter_spawn_one(e);
	if (!expect(e.particles.size() == 1, "spawned one particle")) return false;
	const Vec3 p_before = e.particles[0].position;

	emitter_translate(e, {10.0f, 5.0f, -3.0f});
	const Vec3 p_after = e.particles[0].position;

	if (!expect(near(p_after.x - p_before.x, 10.0f) &&
				near(p_after.y - p_before.y, 5.0f) &&
				near(p_after.z - p_before.z, -3.0f),
			"PositionRelative shifts alive particles by the emitter delta")) {
		std::fprintf(stderr, "  before=(%f,%f,%f) after=(%f,%f,%f)\n",
				p_before.x, p_before.y, p_before.z, p_after.x, p_after.y, p_after.z);
		return false;
	}
	return true;
}

bool test_translate_default_leaves_particles_in_world() {
	// PositionRelative clear (engine default): particles stay in world space.
	// Translate the emitter, verify alive particles are unchanged.
	using namespace opennova::particle;
	ParticleDef def = minimal_def();
	def.flags = 0;  // PositionRelative explicitly clear
	Emitter e;
	emitter_init(e, &def, {0.0f, 0.0f, 0.0f}, 1);
	emitter_spawn_one(e);
	const Vec3 p_before = e.particles[0].position;

	emitter_translate(e, {10.0f, 5.0f, -3.0f});
	const Vec3 p_after = e.particles[0].position;

	if (!expect(near(p_after.x, p_before.x) && near(p_after.y, p_before.y) && near(p_after.z, p_before.z),
			"default PositionRelative-clear leaves alive particles where they were")) {
		std::fprintf(stderr, "  before=(%f,%f,%f) after=(%f,%f,%f)\n",
				p_before.x, p_before.y, p_before.z, p_after.x, p_after.y, p_after.z);
		return false;
	}
	return true;
}

bool test_translate_updates_prev_position_chain() {
	// Each translate stores the OLD current as prev_position. Two translates
	// should leave prev_position pointing at the result of the FIRST translate
	// (not the original spawn position).
	using namespace opennova::particle;
	ParticleDef def = minimal_def();
	Emitter e;
	emitter_init(e, &def, {0.0f, 0.0f, 0.0f}, 1);

	emitter_translate(e, {10.0f, 0.0f, 0.0f});
	emitter_translate(e, {20.0f, 0.0f, 0.0f});

	if (!expect(near(e.prev_position.x, 10.0f) && near(e.prev_position.y, 0.0f) && near(e.prev_position.z, 0.0f),
			"prev_position tracks the previous translate target, not the init spawn")) {
		std::fprintf(stderr, "  prev=(%f,%f,%f) want (10, 0, 0)\n",
				e.prev_position.x, e.prev_position.y, e.prev_position.z);
		return false;
	}
	return true;
}

} // namespace

int main() {
	int failures = 0;
	if (!test_translate_updates_position_and_delta()) ++failures;
	if (!test_translate_accumulates_across_frames())  ++failures;
	if (!test_translate_zero_delta_is_safe())         ++failures;
	if (!test_translate_position_relative_carries_particles()) ++failures;
	if (!test_translate_default_leaves_particles_in_world()) ++failures;
	if (!test_translate_updates_prev_position_chain()) ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
