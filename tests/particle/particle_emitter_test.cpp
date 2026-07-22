// Portable emitter simulator — covers init, manual spawn, lifetime expiry,
// emit_burst, gravity/drag integration, and seeded-RNG determinism. Engine
// reference: CParticleEmitter_AdvanceFrame @ 0x5e6570 et al.

#include <particle/emitter.h>
#include <particle/parser.h>
#include <particle/particle.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
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

bool test_init_randomizes_retail_emission_window_and_rate() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_dur = 10.0f;
	def.emit_dur_adj = 2.0f;
	def.emit_rate = 20.0f;
	def.emit_rate_adj = 10.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 142u);
	if (!expect(near(e.emit_dur_remaining, 12.0f, 0.0001f), __func__)) return false;
	return expect(near(e.emit_rate, 20.263929f, 0.0001f), __func__);
}

bool test_init_converts_authored_orbital_speed_and_adjustment_to_radians() {
	using namespace opennova::particle;
	constexpr float kDegreesToRadians = 0.01745329251994329577f;

	ParticleDef smoke = make_minimal_def();
	smoke.orbitalspeed = 5.0f;
	Emitter smoke_emitter;
	emitter_init(smoke_emitter, &smoke, {0, 0, 0}, 142u);
	if (!expect(near(smoke_emitter.orbit_speed, 5.0f * kDegreesToRadians, 0.0001f),
			"base orbit speed is converted from authored degrees")) {
		return false;
	}

	ParticleDef fire = make_minimal_def();
	fire.orbitalspeed = 30.0f;
	fire.orbitalspeed_adj = 15.0f;
	Emitter fire_emitter;
	emitter_init(fire_emitter, &fire, {0, 0, 0}, 142u);
	return expect(near(fire_emitter.orbit_speed, 20.6891475f * kDegreesToRadians, 0.0001f),
			"orbit adjustment is randomized in degrees before conversion");
}

bool test_rand_unit_includes_retail_one_endpoint() {
	using namespace opennova::particle;
	Emitter e;
	e.rng_state = 142u;
	return expect(near(emitter_rand_unit(e), 1.0f, 1e-7f), __func__);
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

bool test_nonpositive_and_nonfinite_lifetimes_are_rejected() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_rate = 0.0f;
	def.age = 0.0f;
	def.age_adj = 0.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 7);
	if (!expect(!emitter_spawn_one(e) && e.particles.empty(), __func__)) return false;
	def.age = std::numeric_limits<float>::quiet_NaN();
	emitter_init(e, &def, {0, 0, 0}, 7);
	return expect(!emitter_spawn_one(e) && e.particles.empty(), __func__);
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
	// z_offset does NOT land in the spawn position — the engine folds it into
	// the render-side camera-ward pull (emitter+0x140 = -z_offset, seeded in
	// CEffectEmitter_Initialize @ 0x5e6349; applied per quad in
	// BuildBillboardQuads @ 0x5e71c9). Spawn position is (0, y_offset, 0)
	// [orig: SpawnParticle @ 0x5e78a1].
	if (!expect(near(p.position.z, 0.0f), "z_offset does not apply at spawn")) return false;
	return true;
}

bool test_curve_phase_clock() {
	// The engine's particle+0x30/+0x34 pair is the CURVE PHASE clock, not a
	// draw-size ramp: phase starts 0, advances at 256/lifetime per second, and
	// sweeps 0 -> 256 across the whole life as the LUT index source
	// [orig: SpawnParticle @ 0x5e788c/0x5e7898; UpdateParticles advance;
	// BuildBillboardQuads @ 0x5e6d60 indexes (int)phase % 256].
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_rate = 1.0f;
	def.emit_burst = 1;
	def.emit_dur = 5.0f;
	def.age = 2.0f;  // lifetime 2 s -> phase completes 256 at 2 s
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	if (!expect(emitter_spawn_one(e), "spawn for the phase test")) return false;
	if (!expect(near(e.particles[0].phase_rate, 256.0f / 2.0f),
			"phase_rate = 256 / lifetime [orig: @ 0x5e788c]")) {
		std::fprintf(stderr, "  got %f\n", e.particles[0].phase_rate);
		return false;
	}
	if (!expect(near(e.particles[0].curve_phase, 0.0f),
			"phase starts at 0 [orig: @ 0x5e7898]")) return false;
	emitter_advance(e, 1.0f);  // half the lifetime
	if (!expect(near(e.particles[0].curve_phase, 128.0f, 0.5f),
			"phase reaches 128 at half-life (the LUT midpoint)")) {
		std::fprintf(stderr, "  got %f\n", e.particles[0].curve_phase);
		return false;
	}
	return true;
}

bool test_spawn_size_from_graphic_scale() {
	// Base draw size is randomized once at spawn from the chosen graphic
	// layer: size = graphic.scale + graphic.scale_adj * rand_signed
	// [orig: SpawnParticle @ 0x5e7862 — graphic+328/+332 into +0x38]. With
	// scale_adj = 0 the size pins exactly; it never changes over the life.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.graphics[0].scale = 7.5f;
	def.graphics[0].scale_adj = 0.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 3);
	if (!expect(emitter_spawn_one(e), "spawn for the size test")) return false;
	if (!expect(near(e.particles[0].size, 7.5f),
			"size = graphic.scale at spawn [orig: @ 0x5e7862]")) {
		std::fprintf(stderr, "  got %f\n", e.particles[0].size);
		return false;
	}
	emitter_advance(e, 0.5f);
	if (!expect(near(e.particles[0].size, 7.5f), "size is fixed over the life")) return false;
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
	emitter_advance(e, 1.5f);     // 2 emit cycles: the primed burst at t≈0, then t=1.0
	if (!expect(e.particles.size() == 10, "primed + first-interval cycles spawn 5 each")) {
		std::fprintf(stderr, "  got %zu\n", e.particles.size());
		return false;
	}
	emitter_advance(e, 1.0f);     // another cycle (at t=2.0)
	if (!expect(e.particles.size() == 15, "second-interval cycle adds another 5")) {
		std::fprintf(stderr, "  got %zu\n", e.particles.size());
		return false;
	}
	return true;
}

bool test_nonfinite_dt_is_ignored() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_rate = 0.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	if (!expect(emitter_spawn_one(e), __func__)) return false;
	const float age_before = e.particles[0].age;
	const std::uint32_t rng_before = e.rng_state;
	emitter_advance(e, std::numeric_limits<float>::quiet_NaN());
	if (!expect(e.particles[0].age == age_before && e.age == 0.0f &&
			e.rng_state == rng_before, __func__)) return false;
	emitter_advance(e, std::numeric_limits<float>::infinity());
	return expect(e.particles[0].age == age_before && e.age == 0.0f &&
			e.rng_state == rng_before, __func__);
}

bool test_extreme_rate_and_burst_stop_at_capacity() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_rate = std::numeric_limits<float>::max();
	def.emit_burst = std::numeric_limits<int>::max();
	def.emit_dur = 1.0f;
	def.age = 100.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	e.max_particles = std::numeric_limits<std::size_t>::max();
	emitter_advance(e, 0.1f);
	if (!expect(e.particles.size() == kEmitterHardParticleLimit, __func__)) return false;
	return expect(std::isfinite(e.emit_accumulator), __func__);
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
	// Advance well past the lifetime. Retail leaves the particle in its
	// terminal frame and reclaims it at the next AdvanceFrame expiry pass.
	emitter_advance(e, 2.0f);
	if (!expect(!e.particles.empty(), "terminal frame remains observable")) return false;
	emitter_advance(e, 0.001f);
	if (!expect(e.particles.empty(), "all particles expire after lifetime")) {
		std::fprintf(stderr, "  still alive: %zu\n", e.particles.size());
		return false;
	}
	return true;
}

bool test_terminal_frame_is_removed_on_next_advance() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.age = 0.1f;
	def.age_adj = 0.0f;
	def.emit_rate = 0.0f;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 99);
	if (!expect(emitter_spawn_one(e), __func__)) return false;
	emitter_advance(e, 0.1f);
	if (!expect(e.particles.size() == 1, __func__)) return false;
	if (!expect(e.particles[0].age <= 0.0f, __func__)) return false;
	emitter_advance(e, 0.001f);
	return expect(e.particles.empty(), __func__);
}

bool test_gravity_uses_retail_authored_units() {
	// NORMAL movement adds the emitter slot to vel.y. Initialize seeds that
	// slot as authored gravity × -0.09803897, so positive authored gravity
	// sinks and negative authored gravity lifts.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.gravity = 100.0f;
	def.gravity_mask = {0.0f, 1.0f, 0.0f}; // GRAVITATE-only input; inert here
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
	if (!expect(near(vy, -9.803897f, 0.001f), __func__)) return false;
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

bool test_drag_uses_retail_authored_units() {
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
	if (!expect(near(v_after, 9.98f, 0.0001f), __func__)) return false;
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

bool test_spawn_records_curve_flags() {
	// CParticleEmitter_SpawnParticle @ 0x5e7640: per-particle flags at +4
	// reflect which per-graphic LUTs resolved (curves the engine baked from
	// CEffectDef_ResolveAllReferences @ 0x5e9d70). Pin the bit table from
	// emitter.h::particle_runtime_flag.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	TableDef alpha_table;
	alpha_table.id = "atab";
	for (int r = 0; r < 32; ++r) {
		std::array<std::uint8_t, 8> row{};
		for (int c = 0; c < 8; ++c) row[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(r * 8 + c);
		alpha_table.rows.push_back(row);
	}
	tables.push_back(alpha_table);

	ParticleDef def = make_minimal_def();
	def.graphics[0].alpha_func.name = "atab";
	def.graphics[0].alpha_func.present = true;
	def.graphics[0].flip_frames = 4;
	def.graphics[0].blend_mode = BlendMode::Bumpadd;
	bake_particle_def_curves(def, tables);

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	if (!expect(emitter_spawn_one(e), "spawn returns true")) return false;
	const Particle &p = e.particles[0];
	if (!expect((p.flags & particle_runtime_flag::AlphaCurve) != 0,
			"alpha LUT presence sets bit 0x01")) return false;
	if (!expect((p.flags & (particle_runtime_flag::RedCurve |
			particle_runtime_flag::GreenCurve | particle_runtime_flag::BlueCurve |
			particle_runtime_flag::ScaleCurve)) == 0,
			"unbaked curves stay clear")) return false;
	if (!expect((p.flags & particle_runtime_flag::Flipbook) != 0,
			"flip_frames > 1 sets bit 0x20")) return false;
	if (!expect((p.flags & particle_runtime_flag::LitColor) != 0,
			"Bumpadd blend mode sets bit 0x80")) return false;
	if (!expect((p.flags & particle_runtime_flag::Distort) == 0,
			"non-distort blend mode leaves bit 0x100 clear")) return false;
	return true;
}

bool test_spawn_distort_flag() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.graphics[0].blend_mode = BlendMode::Distort;
	def.graphics[0].flip_frames = 1;
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	if (!expect(emitter_spawn_one(e), "distort spawn ok")) return false;
	const Particle &p = e.particles[0];
	if (!expect((p.flags & particle_runtime_flag::Distort) != 0,
			"Distort blend mode sets bit 0x100")) return false;
	if (!expect((p.flags & particle_runtime_flag::LitColor) == 0,
			"Distort does NOT also set bit 0x80")) return false;
	if (!expect((p.flags & particle_runtime_flag::Flipbook) == 0,
			"single-frame layer leaves bit 0x20 clear")) return false;
	return true;
}

bool test_gravitate_uses_converted_gravity_slot() {
	// CParticleEmitter_UpdateParticles @ 0x5e6980 / UpdateAllParticles
	// @ 0x5f3be0 path B: when move & 2 (GRAVITATE) is set,
	//   delta = pos - emitter.pos
	//   delta = D3DXVec3Normalize(delta)  // sub_68B032 @ 0x68B032
	//   vel += delta * spring * dt
	// Direction is AWAY from emitter (despite the misleading "GRAVITATE"
	// name — authors flip via negative gravity_mask for attractive). Force
	// is constant-magnitude (normalized), so a particle at 2× distance
	// accumulates the SAME velocity as one at 1× distance.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.move = move_flag::Gravitate;
	def.gravity = 5.0f;
	def.gravity_mask = {1.0f, 1.0f, 1.0f};
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e_near;
	emitter_init(e_near, &def, {0, 0, 0}, 1);
	emitter_spawn_one(e_near);
	e_near.particles[0].position = {10.0f, 0.0f, 0.0f};
	e_near.particles[0].velocity = {0.0f, 0.0f, 0.0f};
	emitter_advance(e_near, 0.1f);
	const Vec3 v_near = e_near.particles[0].velocity;

	Emitter e_far;
	emitter_init(e_far, &def, {0, 0, 0}, 2);
	emitter_spawn_one(e_far);
	e_far.particles[0].position = {20.0f, 0.0f, 0.0f};
	e_far.particles[0].velocity = {0.0f, 0.0f, 0.0f};
	emitter_advance(e_far, 0.1f);
	const Vec3 v_far = e_far.particles[0].velocity;

	if (!expect(v_near.x < 0.0f, "positive authored gravity attracts toward the emitter")) {
		std::fprintf(stderr, "  vx=%f\n", v_near.x);
		return false;
	}
	// Constant-magnitude (normalized): authored 5 becomes slot -0.49019485;
	// multiplying by dt 0.1 yields vx ≈ -0.0490195.
	if (!expect(near(v_near.x, -0.049019485f, 0.0001f),
			"velocity uses unit delta × converted gravity slot × dt")) {
		std::fprintf(stderr, "  vx=%f want~-0.049\n", v_near.x);
		return false;
	}
	// Far particle gets the SAME magnitude (engine normalizes delta).
	if (!expect(std::fabs(v_far.x - v_near.x) < 0.05f,
			"distance does not affect magnitude (engine normalizes delta)")) {
		std::fprintf(stderr, "  near=%f far=%f\n", v_near.x, v_far.x);
		return false;
	}
	return true;
}

bool test_gravitate_negative_mask_inverts_direction() {
	// Authors enable attractive gravitate via negative gravity_mask. With
	// {-1,1,1}, the x component of the normalized delta is sign-flipped
	// (mask applies AFTER normalize @ 0x5e6980). The converted gravity slot is
	// negative, so that flipped x component pushes outward while y/z attract.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.move = move_flag::Gravitate;
	def.gravity = 1.0f;
	def.gravity_mask = {-1.0f, 1.0f, 1.0f};
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_spawn_one(e);
	e.particles[0].position = {10.0f, 0.0f, 0.0f};
	e.particles[0].velocity = {0.0f, 0.0f, 0.0f};
	emitter_advance(e, 0.1f);
	const Vec3 v = e.particles[0].velocity;

	if (!expect(v.x > 0.0f, "negative x mask flips the converted force")) {
		std::fprintf(stderr, "  vx=%f (want > 0)\n", v.x);
		return false;
	}
	return true;
}

bool test_gravitate_respects_zero_mask() {
	// gravity_mask scales the ALREADY-normalized delta per axis with no
	// renormalize [orig: CParticleEmitter_UpdateParticles @ 0x5e6980, move&2:
	// D3DXVec3Normalize first, then the def+3916 mask]. A zeroed axis drops
	// that component, leaving a sub-unit force along the remaining axes.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.move = move_flag::Gravitate;
	def.gravity = 1.0f;
	def.gravity_mask = {0.0f, 1.0f, 0.0f};
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_spawn_one(e);
	e.particles[0].position = {10.0f, 10.0f, 10.0f};
	e.particles[0].velocity = {0.0f, 0.0f, 0.0f};
	emitter_advance(e, 0.1f);
	const Vec3 v = e.particles[0].velocity;

	if (!expect(std::fabs(v.x) < 0.001f, "x mask 0 zeroes x contribution")) {
		std::fprintf(stderr, "  vx=%f (want 0)\n", v.x);
		return false;
	}
	if (!expect(std::fabs(v.z) < 0.001f, "z mask 0 zeroes z contribution")) {
		std::fprintf(stderr, "  vz=%f (want 0)\n", v.z);
		return false;
	}
	// Pin the normalize-then-mask ORDER: unit delta of {10,10,10} has
	// y = 1/sqrt(3) ≈ 0.5774, so vy = 0.5774 × converted slot
	// (-0.09803897) × dt(0.1) ≈ -0.005660.
	if (!expect(near(v.y, -0.005660f, 0.0001f), "normalize then mask uses converted gravity")) {
		std::fprintf(stderr, "  vy=%f (want ~-0.005660)\n", v.y);
		return false;
	}
	return true;
}

bool test_orbit_rotates_around_axis() {
	// CParticleEmitter_UpdateAllParticles @ 0x5f3be0 — when `move & 4` (=
	// `move_flag::Orbit` per the 0x848800 reorder) is set, the relative
	// position vector and velocity rotate around `def.orbital_axis` by an
	// angle proportional to time. The portable simulator converts authored
	// degrees/sec to radians/sec before applying `orbit_speed * dt`; verify
	// that after one full 2π orbit the particle returns near its start.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.move = move_flag::Orbit;
	def.orbital_axis = {0.0f, 1.0f, 0.0f};
	def.orbitalspeed = 360.0f;  // authored degrees/sec: one full orbit per second
	def.gravity = 0.0f;
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_spawn_one(e);
	// Place particle at +x, no velocity. With orbital_axis = +y, rotation
	// around y sends +x → +z → -x → -z → +x.
	e.particles[0].position = {5.0f, 0.0f, 0.0f};
	e.particles[0].velocity = {0.0f, 0.0f, 0.0f};

	// Quarter orbit: should land near (0, 0, +5) or (0, 0, -5) depending on
	// rotation direction.
	emitter_advance(e, 0.25f);
	const Vec3 quarter = e.particles[0].position;
	const float r_quarter = std::sqrt(quarter.x * quarter.x + quarter.y * quarter.y + quarter.z * quarter.z);
	if (!expect(std::fabs(r_quarter - 5.0f) < 0.5f, "orbital radius preserved over quarter orbit")) {
		std::fprintf(stderr, "  pos=(%f,%f,%f) r=%f\n", quarter.x, quarter.y, quarter.z, r_quarter);
		return false;
	}
	if (!expect(std::fabs(quarter.x) < 1.0f && std::fabs(quarter.z) > 4.0f,
			"quarter orbit lands on the orbital plane (z dominant)")) {
		std::fprintf(stderr, "  pos=(%f,%f,%f)\n", quarter.x, quarter.y, quarter.z);
		return false;
	}

	// Full orbit (3/4 more): should return near starting (5, 0, 0).
	emitter_advance(e, 0.75f);
	const Vec3 full = e.particles[0].position;
	if (!expect(std::fabs(full.x - 5.0f) < 0.5f && std::fabs(full.z) < 0.5f,
			"full orbit returns near starting offset")) {
		std::fprintf(stderr, "  pos=(%f,%f,%f)\n", full.x, full.y, full.z);
		return false;
	}
	return true;
}

bool test_he_explosion_orbit_uses_authored_degrees() {
	// The shipped Crazy_Fire HE explosion layer authors 30 degrees/sec. Treating
	// that value as radians/sec produces about 4.77 turns in one second: the
	// visible hurricane failure this regression protects against.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.move = move_flag::Orbit;
	def.orbital_axis = {0.0f, 1.0f, 0.0f};
	def.orbitalspeed = 30.0f;
	def.gravity = 0.0f;
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1u);
	emitter_spawn_one(e);
	e.particles[0].position = {5.0f, 0.0f, 0.0f};
	e.particles[0].velocity = {0.0f, 0.0f, 0.0f};

	emitter_advance(e, 1.0f);
	const Vec3 position = e.particles[0].position;
	if (!expect(near(position.x, 4.330127f, 0.001f) && near(position.z, -2.5f, 0.001f),
			"HE orbit advances 30 degrees in one second")) {
		std::fprintf(stderr, "  pos=(%f,%f,%f)\n", position.x, position.y, position.z);
		return false;
	}
	return true;
}

bool test_gravitate_spring_const_overrides_def_gravity() {
	// Engine-faithful: the manager sets `*((float*)emitter+77)` = emitter+0x308
	// at spawn time (CEffectEmitter_Initialize @ 0x5e6020). Our portable
	// simulator exposes `Emitter::spring_const`; when non-zero it overrides
	// the converted gravity slot.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.move = move_flag::Gravitate;
	def.gravity = 5.0f;            // converted slot produces vel ≈ -0.049 in 0.1s
	def.gravity_mask = {1.0f, 1.0f, 1.0f};
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	e.spring_const = 20.0f;        // override: vel ≈ 2.0 in 0.1s
	emitter_spawn_one(e);
	e.particles[0].position = {10.0f, 0.0f, 0.0f};
	e.particles[0].velocity = {0.0f, 0.0f, 0.0f};
	emitter_advance(e, 0.1f);
	const Vec3 v = e.particles[0].velocity;

	if (!expect(std::fabs(v.x - 2.0f) < 0.05f,
			"spring_const override yields unit_delta * spring_const * dt")) {
		std::fprintf(stderr, "  vx=%f want~+2.0\n", v.x);
		return false;
	}

	// Reset spring_const = 0 → use converted gravity slot from def.gravity = 5.
	Emitter e2;
	emitter_init(e2, &def, {0, 0, 0}, 1);
	e2.spring_const = 0.0f;
	emitter_spawn_one(e2);
	e2.particles[0].position = {10.0f, 0.0f, 0.0f};
	e2.particles[0].velocity = {0.0f, 0.0f, 0.0f};
	emitter_advance(e2, 0.1f);
	const Vec3 v2 = e2.particles[0].velocity;

	if (!expect(near(v2.x, -0.049019485f, 0.0001f),
			"spring_const = 0 uses converted gravity slot")) {
		std::fprintf(stderr, "  vx=%f want~-0.049\n", v2.x);
		return false;
	}
	return true;
}

bool test_emit_rate_curve_zeroes_emission_when_lut_zero() {
	// CEffectEmitter_AdvanceEmission @ 0x5e1d30 scales the emission interval
	// by `lut[t*256] / 128`. A LUT filled with zeros must suppress all
	// emission until the curve recovers. Verifies our portable simulator
	// reads the baked LUT.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	TableDef zero;
	zero.id = "zero_curve";
	for (int r = 0; r < 32; ++r) {
		std::array<std::uint8_t, 8> row{};
		for (int c = 0; c < 8; ++c) row[static_cast<std::size_t>(c)] = 0;
		zero.rows.push_back(row);
	}
	tables.push_back(zero);

	ParticleDef def = make_minimal_def();
	def.emit_rate = 100.0f;       // would normally spawn many particles
	def.emit_dur = 1.0f;
	def.emit_rate_func.name = "zero_curve";
	def.emit_rate_func.present = true;
	bake_particle_def_curves(def, tables);

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_advance(e, 0.5f);
	if (!expect(e.particles.empty(), "zero-LUT emit_rate_func suppresses emission")) {
		std::fprintf(stderr, "  particles=%zu\n", e.particles.size());
		return false;
	}
	return true;
}

bool test_emit_rate_curve_neutral_lut_matches_constant_rate() {
	// LUT = 128 (= 1.0 neutral) should match the constant-rate emission count
	// within ±1 since the modulation is identity.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	TableDef neutral;
	neutral.id = "neutral";
	for (int r = 0; r < 32; ++r) {
		std::array<std::uint8_t, 8> row{};
		for (int c = 0; c < 8; ++c) row[static_cast<std::size_t>(c)] = 128;
		neutral.rows.push_back(row);
	}
	tables.push_back(neutral);

	ParticleDef def = make_minimal_def();
	def.emit_rate = 10.0f;
	def.emit_dur = 1.0f;
	def.emit_burst = 1;
	def.age = 100.0f;
	def.emit_rate_func.name = "neutral";
	def.emit_rate_func.present = true;
	bake_particle_def_curves(def, tables);

	Emitter e_curved;
	emitter_init(e_curved, &def, {0, 0, 0}, 1);
	emitter_advance(e_curved, 0.5f);
	const std::size_t curved_count = e_curved.particles.size();

	// Run the same advance with no curve baked.
	ParticleDef plain = make_minimal_def();
	plain.emit_rate = 10.0f;
	plain.emit_dur = 1.0f;
	plain.emit_burst = 1;
	plain.age = 100.0f;
	Emitter e_plain;
	emitter_init(e_plain, &plain, {0, 0, 0}, 1);
	emitter_advance(e_plain, 0.5f);
	const std::size_t plain_count = e_plain.particles.size();

	const std::size_t diff = curved_count > plain_count
			? curved_count - plain_count
			: plain_count - curved_count;
	if (!expect(diff <= 1, "neutral LUT byte 128 matches constant emit_rate")) {
		std::fprintf(stderr, "  curved=%zu plain=%zu\n", curved_count, plain_count);
		return false;
	}
	return true;
}

bool test_emit_rate_curve_doubles_with_lut_255() {
	// LUT = 255 maps to ~1.99 rate scale, so half-second of emission with
	// rate=10 + burst=1 should produce roughly twice as many particles as
	// the unmodulated case.
	using namespace opennova::particle;
	std::vector<TableDef> tables;
	TableDef boost;
	boost.id = "boost";
	for (int r = 0; r < 32; ++r) {
		std::array<std::uint8_t, 8> row{};
		for (int c = 0; c < 8; ++c) row[static_cast<std::size_t>(c)] = 255;
		boost.rows.push_back(row);
	}
	tables.push_back(boost);

	ParticleDef def = make_minimal_def();
	def.emit_rate = 10.0f;
	def.emit_dur = 1.0f;
	def.emit_burst = 1;
	def.age = 100.0f;
	def.emit_rate_func.name = "boost";
	def.emit_rate_func.present = true;
	bake_particle_def_curves(def, tables);

	Emitter e_boost;
	emitter_init(e_boost, &def, {0, 0, 0}, 1);
	emitter_advance(e_boost, 0.5f);
	const std::size_t boost_count = e_boost.particles.size();

	if (!expect(boost_count >= 8 && boost_count <= 12,
			"LUT 255 (~2x scale) produces ~10 particles in 0.5s with rate=10")) {
		std::fprintf(stderr, "  boost_count=%zu (expected ~10, range 8-12)\n", boost_count);
		return false;
	}
	return true;
}

bool test_lod_divisor_default_is_one() {
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	if (!expect(e.lod_divisor == 1u, "default lod_divisor is 1 (render every particle)")) {
		std::fprintf(stderr, "  got %u\n", e.lod_divisor);
		return false;
	}
	return true;
}

bool test_lod_divisor_does_not_affect_simulation() {
	// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 reads lod_divisor;
	// the simulator (UpdateParticles, SpawnParticle) does NOT. Verify our
	// portable simulator preserves this property: changing lod_divisor
	// after init must not affect particle spawn/integration counts.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.emit_rate = 50.0f;
	def.emit_dur = 1.0f;
	def.emit_burst = 1;

	Emitter e_full;
	emitter_init(e_full, &def, {0, 0, 0}, 0xCAFE);
	e_full.lod_divisor = 1;
	emitter_advance(e_full, 0.5f);

	Emitter e_culled;
	emitter_init(e_culled, &def, {0, 0, 0}, 0xCAFE);
	e_culled.lod_divisor = 4;
	emitter_advance(e_culled, 0.5f);

	if (!expect(e_full.particles.size() == e_culled.particles.size(),
			"lod_divisor must not affect simulator particle count")) {
		std::fprintf(stderr, "  full=%zu culled=%zu\n",
				e_full.particles.size(), e_culled.particles.size());
		return false;
	}
	// Per-particle state must also match (spawn order, positions, ...).
	for (std::size_t i = 0; i < e_full.particles.size(); ++i) {
		if (!expect(e_full.particles[i].serial == e_culled.particles[i].serial,
				"per-particle serials match across LOD")) return false;
	}
	return true;
}

bool test_kill_plane_disabled_keeps_particles_alive() {
	// Engine: CParticleEmitter_UpdateParticles @ 0x5e6980 only checks the
	// kill plane when def.flags bits 27/28 are set; with our portable
	// scalar mode == 0, no kill check fires regardless of threshold value
	// or particle position.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.gravity = 0.0f;
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	e.kill_plane_mode = 0;          // disabled
	e.kill_plane_y = 0.0f;
	emitter_spawn_one(e);
	e.particles[0].position = {0.0f, 5.0f, 0.0f};   // way above any threshold
	e.particles[0].velocity = {0.0f, -10.0f, 0.0f}; // descending
	emitter_advance(e, 0.5f);

	if (!expect(e.particles.size() == 1, "disabled mode does not kill regardless of y crossing")) {
		std::fprintf(stderr, "  alive=%zu\n", e.particles.size());
		return false;
	}
	return true;
}

bool test_kill_plane_above_kills_when_particle_rises() {
	// Mode 1 (KillAbove): particle.y > threshold → kill. Particle starts
	// at y=0 below threshold y=5, ascends with positive vel, dies once
	// it crosses.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.gravity = 0.0f;
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	e.kill_plane_mode = 1;
	e.kill_plane_y = 5.0f;
	emitter_spawn_one(e);
	e.particles[0].position = {0.0f, 0.0f, 0.0f};   // below threshold
	e.particles[0].velocity = {0.0f, 100.0f, 0.0f}; // rising

	emitter_advance(e, 0.1f);   // 0.0 + 100*0.1 = +10 → above threshold 5 → kills

	if (!expect(e.particles.size() == 1 && e.particles[0].age <= 0.0f,
			"mode 1 marks the crossing particle for next-frame removal")) {
		std::fprintf(stderr, "  alive=%zu y=%f\n",
				e.particles.size(),
				e.particles.empty() ? 0.0f : e.particles[0].position.y);
		return false;
	}
	emitter_advance(e, 0.001f);
	if (!expect(e.particles.empty(), "mode 1 reclaims the marked particle next frame")) return false;
	return true;
}

bool test_kill_plane_below_kills_at_threshold_or_lower() {
	// Mode 2 (KillAtOrBelow): particle.y <= threshold → kill. Particle
	// starts above threshold y=0, descends with negative vel, dies on
	// or after crossing.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.gravity = 0.0f;
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	e.kill_plane_mode = 2;
	e.kill_plane_y = 0.0f;
	emitter_spawn_one(e);
	e.particles[0].position = {0.0f, 5.0f, 0.0f};
	e.particles[0].velocity = {0.0f, -100.0f, 0.0f};  // descending

	emitter_advance(e, 0.1f);   // 5.0 + (-100)*0.1 = -5.0 → at/below 0 → kills

	if (!expect(e.particles.size() == 1 && e.particles[0].age <= 0.0f,
			"mode 2 marks the crossing particle for next-frame removal")) {
		std::fprintf(stderr, "  alive=%zu y=%f\n",
				e.particles.size(),
				e.particles.empty() ? 0.0f : e.particles[0].position.y);
		return false;
	}
	emitter_advance(e, 0.001f);
	if (!expect(e.particles.empty(), "mode 2 reclaims the marked particle next frame")) return false;

	// Boundary case: particle exactly at threshold should also be killed
	// (mode 2 condition is y <= threshold, inclusive).
	Emitter e2;
	emitter_init(e2, &def, {0, 0, 0}, 1);
	e2.kill_plane_mode = 2;
	e2.kill_plane_y = 0.0f;
	emitter_spawn_one(e2);
	e2.particles[0].position = {0.0f, 0.0f, 0.0f};   // exactly at threshold
	e2.particles[0].velocity = {0.0f, 0.0f, 0.0f};
	emitter_advance(e2, 0.01f);
	if (!expect(e2.particles.size() == 1 && e2.particles[0].age <= 0.0f,
			"mode 2 marks exactly-at-threshold particles (inclusive boundary)")) return false;
	emitter_advance(e2, 0.001f);
	if (!expect(e2.particles.empty(), "mode 2 reclaims threshold particles next frame")) return false;
	return true;
}

bool test_orbit_axis_y_keeps_y_constant() {
	// Rotation around y leaves y component invariant.
	using namespace opennova::particle;
	ParticleDef def = make_minimal_def();
	def.move = move_flag::Orbit;
	def.orbital_axis = {0.0f, 1.0f, 0.0f};
	def.orbitalspeed = 1.0f;
	def.gravity = 0.0f;
	def.drag = 0.0f;
	def.age = 100.0f;
	def.emit_rate = 0.0f;

	Emitter e;
	emitter_init(e, &def, {0, 0, 0}, 1);
	emitter_spawn_one(e);
	e.particles[0].position = {3.0f, 7.0f, 0.0f};
	e.particles[0].velocity = {0.0f, 0.0f, 0.0f};

	for (int i = 0; i < 20; ++i) {
		emitter_advance(e, 0.05f);
	}
	const float y_after = e.particles[0].position.y;
	if (!expect(std::fabs(y_after - 7.0f) < 0.05f,
			"orbital axis y preserves y component")) {
		std::fprintf(stderr, "  y=%f want 7.0\n", y_after);
		return false;
	}
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
	if (!test_init_randomizes_retail_emission_window_and_rate()) ++failures;
	if (!test_init_converts_authored_orbital_speed_and_adjustment_to_radians()) ++failures;
	if (!test_rand_unit_includes_retail_one_endpoint()) ++failures;
	if (!test_manual_spawn())               ++failures;
	if (!test_nonpositive_and_nonfinite_lifetimes_are_rejected()) ++failures;
	if (!test_spawn_records_visual_choices()) ++failures;
	if (!test_curve_phase_clock())          ++failures;
	if (!test_spawn_size_from_graphic_scale()) ++failures;
	if (!test_advance_emits())              ++failures;
	if (!test_burst())                      ++failures;
	if (!test_nonfinite_dt_is_ignored())    ++failures;
	if (!test_extreme_rate_and_burst_stop_at_capacity()) ++failures;
	if (!test_lifetime_expires())           ++failures;
	if (!test_terminal_frame_is_removed_on_next_advance()) ++failures;
	if (!test_gravity_uses_retail_authored_units()) ++failures;
	if (!test_drag_uses_retail_authored_units()) ++failures;
	if (!test_determinism_same_seed())      ++failures;
	if (!test_determinism_different_seed()) ++failures;
	if (!test_spawn_records_curve_flags())  ++failures;
	if (!test_spawn_distort_flag())         ++failures;
	if (!test_gravitate_uses_converted_gravity_slot()) ++failures;
	if (!test_gravitate_negative_mask_inverts_direction()) ++failures;
	if (!test_gravitate_respects_zero_mask()) ++failures;
	if (!test_gravitate_spring_const_overrides_def_gravity()) ++failures;
	if (!test_emit_rate_curve_zeroes_emission_when_lut_zero()) ++failures;
	if (!test_emit_rate_curve_neutral_lut_matches_constant_rate()) ++failures;
	if (!test_emit_rate_curve_doubles_with_lut_255())  ++failures;
	if (!test_lod_divisor_default_is_one())            ++failures;
	if (!test_lod_divisor_does_not_affect_simulation()) ++failures;
	if (!test_kill_plane_disabled_keeps_particles_alive()) ++failures;
	if (!test_kill_plane_above_kills_when_particle_rises()) ++failures;
	if (!test_kill_plane_below_kills_at_threshold_or_lower()) ++failures;
	if (!test_orbit_rotates_around_axis())  ++failures;
	if (!test_he_explosion_orbit_uses_authored_degrees()) ++failures;
	if (!test_orbit_axis_y_keeps_y_constant()) ++failures;
	if (!test_against_real_fixture())       ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
