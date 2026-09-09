// GRM fixture is authored in fixtures/grm/person.grm. These tests run the
// real mounted format, WAC VM, slot scheduler, mesh evaluator and retry path.
#include <base/crt/crt_rng.h>
#include <base/io/crt_rand.h>
#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/facial_animation.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>
#include "common/test_paths.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace opennova;
using namespace opennova::world;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
bool near(float a, float b) { return std::fabs(a - b) < 0.00001f; }

struct Motion : IRootMotionSource {
	bool has_clip(int, int state) const override { return state > 0 && state < 252; }
	int32_t clip_length_ticks(int, int, int) const override { return -1; }
	bool advance(int, int, int32_t &, RootMotionFrame &) override { return false; }
};

struct Fixture {
	ResourceIndex index;
	World world;
	def::DefItemDef definition{};
	def::DefItemsFile definitions{};
	Motion motion;
	Fixture() {
		CHECK(index.scan(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/grm"));
		world.registry.configure_pool(0, 300);
		definition.id = 100011;
		definition.type = def::DEF_ITEM_TYPE_PERSON;
		std::strcpy(definition.graphic, "folder\\PeRsOn.3di");
		definitions.entries = &definition; definitions.count = 1;
		world.facials.configure(world, &index, definitions);
	}
	EntityHandle spawn(uint16_t net_id, float x = 0.0f) {
		Entity e;
		e.item_id = 11; e.item_type = 3; e.has_item_def = true;
		e.kind = EntityKind::Organic; e.net_id = net_id;
		e.health = 100; e.position.x = x;
		const auto h = world.registry.spawn(0, e);
		world.facials.initialize(world);
		return h;
	}
	Entity &entity(EntityHandle h) { return *world.registry.get(h); }
	FacialSlot &slot(EntityHandle h) { return *world.facials.for_entity(entity(h)); }
	void script(const char *source) {
		wac::CompileEnv env;
		const auto program = wac::compile_source(source, env);
		CHECK(program.ok());
		wac::WacVm vm; vm.load(program); vm.execute(world);
	}
};

void test_commands_and_retry() {
	Fixture f;
	const auto a = f.spawn(7), b = f.spawn(8);
	CHECK(f.slot(a).model == f.slot(b).model);
	CHECK(f.entity(a).facial_slot == 0x8000 && f.entity(b).facial_slot == 0x8001);
	// The gaze pair draws from the render/effects stream and leaves the
	// session-seeded sim stream alone (D-NET-115). Without a present pass the
	// display counter stays 0, so every tick draws the pair for each slot.
	crt::crt_srand(1);
	const uint32_t sim_state = f.world.crt_rand.state;
	f.world.facials.tick(f.world);
	CHECK(f.world.crt_rand.state == sim_state);
	io::CrtRand effects_oracle;
	effects_oracle.seed(1);
	for (int draw = 0; draw < 4; ++draw) effects_oracle.next();
	CHECK(crt::crt_rand_state() == effects_oracle.state);
	f.world.cached.local_player = a;
	const auto baseline = f.world.snapshot();
	for (int attempt = 0; attempt < 2; ++attempt) {
		CHECK(f.slot(a).expression_override == -1);
		f.script("face(HAPPY) store(v1)\nssnface(8,FACE_ANGRY) store(v2)\n"
				"v3=FACE_SURPRISE\nssnface(8,v3) store(v4)\n");
		CHECK(f.world.script.vars.get_mission(1) == 0 && f.world.script.vars.get_mission(2) == 1);
		CHECK(f.world.script.vars.get_mission(3) == 5 && f.world.script.vars.get_mission(4) == 1);
		CHECK(f.slot(a).expression_override == 1 && f.slot(a).override_timer == 80);
		CHECK(f.slot(b).expression_override == 5);
		CHECK(f.world.diagnostics.empty());
		f.world.facials.tick(f.world);
		f.world.restore(baseline);
	}
	f.entity(b).health = 0; f.entity(b).has_item_def = false;
	f.script("ssnface(8,\"DISGUST\") store(v1)\n");
	CHECK(f.world.script.vars.get_mission(1) == 1 && f.slot(b).expression_override == 6);
	f.entity(b).item_id = 0;
	f.script("ssnface(8,NORMAL) store(v1)\n");
	CHECK(f.world.script.vars.get_mission(1) == 0 && f.slot(b).expression_override == 6);
	f.entity(a).facial_slot = 0;
	f.script("face(FEAR) store(v1)\n");
	CHECK(f.world.script.vars.get_mission(1) == 0);
	f.world.cached.local_player = {};
	f.script("face(FEAR) store(v1)\n");
	CHECK(f.world.script.vars.get_mission(1) == 1);
	wac::CompileEnv env;
	CHECK(!wac::compile_source("face(1)\n", env).ok());
	CHECK(!wac::compile_source("ssnface(7,missing)\n", env).ok());
	CHECK(wac::compile_source("v1=FACE_SMIRK\nface(v1)\n", env).ok());
}

void test_transition_and_eye_timing() {
	FacialSlot s;
	s.expression_override = 1; s.override_timer = 80; s.automatic = 4;
	step_facial_animation(s, false, 1, 0);
	CHECK(s.current == 0 && s.next == 0 && s.blend == 1.0f && s.override_timer == 80);
	step_facial_animation(s, false, 1, 0);
	CHECK(s.current == 0 && s.next == 1 && s.blend == 0.0f && s.override_timer == 79);
	for (int i = 0; i < 7; ++i) step_facial_animation(s, false, 1, 0);
	CHECK(s.current == 0 && s.next == 1 && s.blend == 0.875f && s.override_timer == 79);
	step_facial_animation(s, false, 1, 0);
	CHECK(s.current == 1 && s.next == 1 && s.blend == 1.0f && s.override_timer == 78);
	for (int i = 0; i < 78; ++i) step_facial_animation(s, false, 1, 0);
	CHECK(s.override_timer == 0 && s.expression_override == 1);
	step_facial_animation(s, false, 1, 0);
	CHECK(s.override_timer == -1 && s.expression_override == -1 && s.next == 1);
	step_facial_animation(s, false, 1, 0);
	CHECK(s.next == 4 && s.current == 1);
	s.blend = 1.0f; s.automatic = -1;
	step_facial_animation(s, false, 1, 7u << 10);
	CHECK(s.next == 7);

	// Two draws only on a multiple-of-64 DISPLAY frame; each logic call
	// on that display frame draws again, as the original does. The draws
	// come from the thread-local effects stream (the same MSVC recurrence
	// the oracle spells), never the sim stream.
	crt::crt_srand(1);
	FacialSlot eyes;
	io::CrtRand oracle;
	oracle.seed(1);
	const float x = float((int(oracle.next()) - 16384) * 0.000061035156);
	const float y = float((int(oracle.next()) - 16384) * 0.000061035156);
	step_facial_animation(eyes, false, 64, 0);
	CHECK(crt::crt_rand_state() == oracle.state && near(eyes.eyes.x, x * .25f));
	CHECK(near(eyes.eyes.y, y * .25f));
	eyes.directed_eyes = {1.0f, 2.0f}; eyes.directed_timer = 2;
	eyes.eyes = {};
	step_facial_animation(eyes, false, 65, 0);
	CHECK(crt::crt_rand_state() == oracle.state); // no draw off the 64 cadence
	CHECK(eyes.directed_timer == 1 && eyes.eyes.x == .25f && eyes.eyes.y == .5f);
	eyes.blend = 1.0f; eyes.expression_override = 4; eyes.override_timer = 20;
	step_facial_animation(eyes, true, 65, 0);
	CHECK(eyes.next == 5 && eyes.override_timer == 20 && eyes.directed_timer == 1);
	CHECK(near(eyes.eyes.x, .2375f) && near(eyes.eyes.y, .365f));
}

void test_mesh_and_texture_schedule() {
	Fixture f;
	const auto a = f.spawn(1, 10), b = f.spawn(2, 20), c = f.spawn(3, 30), d = f.spawn(4, 40);
	auto &s = f.slot(a);
	auto positions = evaluate_facial_mesh(*s.model, 0, 1, .5f);
	CHECK(near(positions[1].x, .975f) && near(positions[1].y, .05f));
	CHECK(near(positions[2].x, 1.05f) && near(positions[2].y, .9f));
	positions = evaluate_facial_mesh(*s.model, 4, 1, .5f); // unknown ANGRY falls to HAPPY
	CHECK(near(positions[1].x, .95f) && near(positions[2].y, .8f));
	positions = evaluate_facial_mesh(*s.model, 4, 7, .5f); // neither gesture exists
	CHECK(positions[1].x == 1.0f && positions[2].y == 1.0f);
	std::vector<FacialDraw> draws;
	f.world.facials.compile_draws(f.world, 0, 0, true, draws);
	CHECK(draws.size() == 3 && draws[0].owner == a && draws[2].owner == c);
	CHECK(draws[0].resolution == 256 && draws[1].resolution == 128 && draws[2].resolution == 64);
	CHECK(!draws[0].redraw && draws[1].redraw && draws[2].redraw);
	CHECK(!f.slot(d).active);
	f.world.facials.compile_draws(f.world, 0, 0, true, draws);
	CHECK(!draws[0].redraw && !draws[1].redraw && draws[2].redraw);
	f.entity(a).flags |= 1u;
	f.world.cached.local_player = b;
	f.world.facials.compile_draws(f.world, 0, 0, true, draws);
	CHECK(draws.size() == 2 && draws[0].owner == c && draws[1].owner == d);
	CHECK(draws[0].redraw && draws[1].redraw);
	f.world.facials.compile_draws(f.world, 0, 0, false, draws);
	CHECK(draws.size() == 3 && draws[0].owner == b && draws[0].redraw);
}

void test_slot_lifetime_and_capacity() {
	Fixture f;
	const auto first = f.spawn(1);
	const auto serial = f.entity(first).registry_spawn_id;
	f.world.facials.override_expression(f.entity(first), 4);
	const Entity seed = f.entity(first);
	CHECK(f.world.commands.remove_ssn(first));
	CHECK(!f.world.facials.slots()[0].owner.valid());
	const auto replacement = f.world.registry.spawn(0, seed);
	CHECK(replacement == first && f.entity(replacement).registry_spawn_id != serial);
	f.world.facials.initialize(f.world);
	CHECK(f.entity(replacement).facial_slot == 0x8001);
	CHECK(f.slot(replacement).expression_override == -1 && !f.world.facials.slots()[0].owner.valid());
	for (int i = 2; i < 256; ++i) f.spawn(uint16_t(i + 1));
	const auto overflow = f.spawn(300);
	CHECK(f.world.facials.slots().size() == 256 && f.entity(overflow).facial_slot == 0);
	f.world.facials.override_expression(seed, 8); // stale allocation cannot reach replacement
	CHECK(f.slot(replacement).expression_override == -1);
}

void test_automatic_expression() {
	Fixture f;
	const auto h = f.spawn(1), target = f.spawn(2);
	f.world.ai.attach(h); f.world.ai.attach(target);
	auto &body = *f.world.ai.for_handle(h);
	auto &other = *f.world.ai.for_handle(target);
	body.inf.active = true; body.inf.adm_id = 1; body.health = 100;
	body.inf.anim_state = 140; body.inf.body_heading = 0;
	body.slot.f[17] = 20 * 65536;
	body.inf.head_look_target = target;
	other.pos[0] = 65536; other.heading = INT32_MIN;
	f.world.ai.root_motion = &f.motion;
	infantry_attention_think(f.world.ai, body, f.world, 0x80);
	CHECK(f.slot(h).automatic == 6);
	infantry_attention_think(f.world.ai, body, f.world, 0x180);
	CHECK(f.slot(h).automatic == 7);
	other.pos[0] = 5 * 65536;
	body.inf.anim_state = 140;
	infantry_attention_think(f.world.ai, body, f.world, 0x90);
	CHECK(f.slot(h).automatic == 8); // animation table entry 140
}

} // namespace

int main() {
	test_commands_and_retry();
	test_transition_and_eye_timing();
	test_mesh_and_texture_schedule();
	test_slot_lifetime_and_capacity();
	test_automatic_expression();
	return failures ? 1 : 0;
}
