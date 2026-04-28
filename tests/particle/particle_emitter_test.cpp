// Portable emitter simulator — covers init, manual spawn, lifetime expiry,
// emit_burst, gravity/drag integration, and seeded-RNG determinism. Engine
// reference: CParticleEmitter_AdvanceFrame @ 0x5e6570 et al.

#include <particle/emitter.h>
#include <particle/parser.h>
#include <particle/particle.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.001f) {
	return std::fabs(actual - expected) <= epsilon;
}

opennova::particle::ParticleDef make_minimal_def() {
	using namespace opennova::particle;
	ParticleDef def;
	def.id = "test";
	def.emit_dur = 1.0f;
	def.emit_rate = 10.0f;       // 10 particles/sec
	def.emit_burst = 1;
	def.emit_shape = static_cast<int>(EmitShape::Point);
	def.age = 2.0f;
	def.alpha = 1.0f;
	def.color1 = {255, 200, 100};
	def.color2 = {255, 200, 100};
	def.color3 = {255, 200, 100};
	def.color4 = {255, 200, 100};
	def.scale = 1.0f;
	GraphicLayer &g0 = def.graphics[0];
	g0.present = true;
	g0.index = 1;
	g0.color1 = def.color1;
	g0.color2 = def.color2;
	g0.color3 = def.color3;
	g0.color4 = def.color4;
	return def;
}

bool test_init() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	Emitter e;
	emitter_init(e, &def, {1.0f, 2.0f, 3.0f}, 12345);
	if (!expect(e.def == &def, "def assigned")) return false;
	if (!expect(near(e.position.x, 1.0f) && near(e.position.y, 2.0f) && near(e.position.z, 3.0f), "position copied")) return false;
	if (!expect(e.particles.empty(), "no particles at init")) return false;
	if (!expect(e.active, "active by default")) return false;
	if (!expect(e.finite, "finite by default (no FOREVEREMIT)")) return false;
	if (!expect(near(e.emit_dur_remaining, 1.0f), "emit_dur loaded")) return false;
	return true;
}

bool test_manual_spawn() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 7);
	if (!expect(emitter_spawn_one(e), "manual spawn returns true")) return false;
	if (!expect(e.particles.size() == 1, "one particle after manual spawn")) return false;
	const Particle &p = e.particles[0];
	if (!expect(near(p.position.x, 0.0f), "spawned at emitter origin")) return false;
	if (!expect(near(p.age, p.lifetime), "age == lifetime at spawn")) return false;
	if (!expect(p.lifetime > 0.0f, "lifetime is positive")) return false;
	if (!expect(p.serial == 0, "first serial is 0")) return false;
	if (!expect(emitter_spawn_one(e), "second spawn ok")) return false;
	if (!expect(e.particles[1].serial == 1, "second serial is 1")) return false;
	return true;
}

bool test_spawn_records_visual_choices() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.y_offset = 3.0f;
	def.z_offset = -2.0f;
	def.graphics[0].present = false;
	GraphicLayer &g2 = def.graphics[2];
	g2.present = true;
	g2.index = 3;
	g2.color1 = {10, 20, 30};
	g2.color2 = {40, 50, 60};
	g2.color3 = {70, 80, 90};
	g2.color4 = {100, 110, 120};
	g2.color_overrides_set = true;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 7);
	if (!expect(emitter_spawn_one(e), "visual choice spawn returns true")) return false;
	const Particle &p = e.particles[0];
	if (!expect(p.color_slot <= 3, "spawn records color slot")) return false;
	if (!expect(p.graphic_layer == 2, "single present graphic preserves actual layer index")) return false;
	if (!expect(near(p.position.y, 3.0f), "y_offset applies at spawn")) return false;
	if (!expect(near(p.position.z, -2.0f), "z_offset applies at spawn")) return false;
	return true;
}

bool test_advance_emits() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_rate = 10.0f;        // every 100ms
	def.emit_burst = 1;
	def.emit_dur = 0.5f;          // half a second of emission
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_advance(e, 0.5f);     // half-second advance: ~5 emissions
	if (!expect(e.particles.size() >= 4 && e.particles.size() <= 6, "approx 5 particles after 0.5s @10Hz")) {
		std::fprintf(stderr, "  got %zu\n", e.particles.size());
		return false;
	}
	return true;
}

bool test_burst() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_rate = 1.0f;         // one tick per second
	def.emit_burst = 5;           // 5 particles per tick
	def.emit_dur = 5.0f;
	def.age = 10.0f;              // long-lived so they don't die during the test
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_advance(e, 1.5f);     // 1 emit cycle (at t=1.0)
	if (!expect(e.particles.size() == 5, "burst of 5 spawns 5")) {
		std::fprintf(stderr, "  got %zu\n", e.particles.size());
		return false;
	}
	emitter_advance(e, 1.0f);     // another cycle (at t=2.0)
	if (!expect(e.particles.size() == 10, "second cycle adds another 5")) {
		std::fprintf(stderr, "  got %zu\n", e.particles.size());
		return false;
	}
	return true;
}

bool test_lifetime_expires() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.age = 0.5f;
	def.age_adj = 0.0f;
	def.emit_rate = 10.0f;
	def.emit_dur = 0.1f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 99);
	emitter_advance(e, 0.1f);
	const std::size_t after_first = e.particles.size();
	if (!expect(after_first > 0, "particles spawn during emit window")) return false;
	// Advance well past the lifetime + emit_delay; everything must die.
	emitter_advance(e, 2.0f);
	if (!expect(e.particles.empty(), "all particles expire after lifetime")) {
		std::fprintf(stderr, "  still alive: %zu\n", e.particles.size());
		return false;
	}
	return true;
}

bool test_gravity_pulls_y_down() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.gravity = 100.0f;
	def.gravity_mask = {0.0f, 1.0f, 0.0f}; // only y feels gravity
	def.drag = 0.0f;
	def.age = 5.0f;
	def.emit_rate = 0.0f;          // no auto emission; we'll spawn manually
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 42);
	emitter_spawn_one(e);

	// Engine ordering (UpdateParticles): position += velocity * dt first, then
	// velocity += gravity * dt. So a particle starting at zero velocity needs
	// at least two ticks before position visibly responds to gravity. Step
	// many small frames to accumulate displacement.
	const float y0 = e.particles[0].position.y;
	for (int i = 0; i < 60; ++i) {
		emitter_advance(e, 1.0f / 60.0f);
	}
	const float vy = e.particles[0].velocity.y;
	const float y1 = e.particles[0].position.y;
	if (!expect(vy < 0.0f, "gravity reduces vy")) {
		std::fprintf(stderr, "  vy=%f\n", vy);
		return false;
	}
	if (!expect(y1 < y0, "gravity pulls particle in -y direction over time")) {
		std::fprintf(stderr, "  y0=%f y1=%f vy=%f\n", y0, y1, vy);
		return false;
	}
	return true;
}

bool test_drag_decelerates() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.gravity = 0.0f;
	def.drag = 2.0f;               // strong drag
	def.age = 5.0f;
	def.emit_rate = 0.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_spawn_one(e);
	// Manually inject velocity since shape=Point gives zero.
	e.particles[0].velocity = {10.0f, 0.0f, 0.0f};
	emitter_advance(e, 0.1f);
	const float v_after = std::abs(e.particles[0].velocity.x);
	if (!expect(v_after < 10.0f && v_after > 0.0f, "drag reduces velocity magnitude")) {
		std::fprintf(stderr, "  v=%f\n", v_after);
		return false;
	}
	return true;
}

bool test_determinism_same_seed() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_shape = static_cast<int>(EmitShape::Sphere);
	def.emit_shape_size = {1.0f, 1.0f, 1.0f};
	def.speed = 5.0f;
	def.speed_adj = 1.0f;
	def.emit_rate = 50.0f;
	def.emit_dur = 0.1f;

	Emitter a, b;
	emitter_init(a, &def, {0, 0, 0}, 0xCAFEBABE);
	emitter_init(b, &def, {0, 0, 0}, 0xCAFEBABE);
	for (int i = 0; i < 10; ++i) {
		emitter_advance(a, 0.016f);
		emitter_advance(b, 0.016f);
	}
	if (!expect(a.particles.size() == b.particles.size(), "same particle count")) return false;
	for (std::size_t i = 0; i < a.particles.size(); ++i) {
		if (!expect(near(a.particles[i].position.x, b.particles[i].position.x), "same x")) return false;
		if (!expect(near(a.particles[i].position.y, b.particles[i].position.y), "same y")) return false;
		if (!expect(near(a.particles[i].velocity.z, b.particles[i].velocity.z), "same vz")) return false;
	}
	return true;
}

bool test_determinism_different_seed() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_shape = static_cast<int>(EmitShape::Sphere);
	def.emit_shape_size = {1.0f, 1.0f, 1.0f};
	def.speed = 5.0f;
	def.speed_adj = 1.0f;
	def.emit_rate = 50.0f;
	def.emit_dur = 0.1f;

	Emitter a, b;
	emitter_init(a, &def, {0, 0, 0}, 1);
	emitter_init(b, &def, {0, 0, 0}, 2);
	for (int i = 0; i < 5; ++i) {
		emitter_advance(a, 0.016f);
		emitter_advance(b, 0.016f);
	}
	if (a.particles.empty() || b.particles.empty()) {
		std::fprintf(stderr, "FAIL: at least one stream produced no particles\n");
		return false;
	}
	bool any_differ = false;
	for (std::size_t i = 0; i < std::min(a.particles.size(), b.particles.size()); ++i) {
		if (!near(a.particles[i].position.x, b.particles[i].position.x) ||
				!near(a.particles[i].position.y, b.particles[i].position.y) ||
				!near(a.particles[i].position.z, b.particles[i].position.z)) {
			any_differ = true;
			break;
		}
	}
	if (!expect(any_differ, "different seeds produce different positions")) return false;
	return true;
}

bool test_against_real_fixture() {
	// Drive the emitter against a parsed fixture (buildup.ptl). Sanity-check
	// that the parsed def hydrates the simulator without surprises.
#ifdef OPENNOVA_SOURCE_DIR
	const std::string path = std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/buildup.ptl";
#else
	const std::string path = "fixtures/particle/buildup.ptl";
#endif
	std::ifstream stream(path, std::ios::binary);
	opennova::particle::ParticleFile file;
	opennova::particle::ParseError error;
	if (!opennova::particle::load_particles(stream, file, error)) {
		std::fprintf(stderr, "FAIL: parse buildup.ptl: %s\n", error.message.c_str());
		return false;
	}
	const opennova::particle::ParticleDef *def = file.find_particle("Buildup dots");
	if (!expect(def != nullptr, "buildup particle resolves")) return false;

	opennova::particle::Emitter e;
	opennova::particle::emitter_init(e, def, {0, 0, 0}, 0xBEEF);
	// Step for a chunk of the def's emit_dur (60s). Just a few frames here.
	for (int i = 0; i < 30; ++i) {
		opennova::particle::emitter_advance(e, 1.0f / 60.0f);
	}
	if (!expect(!e.particles.empty(), "particles spawn from real fixture")) return false;
	// Buildup has gravity=600, particles should be falling.
	bool any_falling = false;
	for (const opennova::particle::Particle &p : e.particles) {
		if (p.velocity.y < 0.0f) {
			any_falling = true;
			break;
		}
	}
	if (!expect(any_falling, "gravity pulls buildup particles down")) return false;
	return true;
}

} // namespace

int main() {
	int failures = 0;
	if (!test_init())                       ++failures;
	if (!test_manual_spawn())               ++failures;
	if (!test_spawn_records_visual_choices()) ++failures;
	if (!test_advance_emits())              ++failures;
	if (!test_burst())                      ++failures;
	if (!test_lifetime_expires())           ++failures;
	if (!test_gravity_pulls_y_down())       ++failures;
	if (!test_drag_decelerates())           ++failures;
	if (!test_determinism_same_seed())      ++failures;
	if (!test_determinism_different_seed()) ++failures;
	if (!test_against_real_fixture())       ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
