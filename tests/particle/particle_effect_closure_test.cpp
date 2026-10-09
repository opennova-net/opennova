// What one effect's spawn reads of the effect catalog (runtime/particle/effect_closure): the effect
// the catalog registers first, its members all or nothing, its child chains, every table, and a scene
// over it alone spawning what the whole catalog spawns, particle for particle; the stock effect for an
// unknown name. Several names' closures folded into one config (effect_closures), each effect and
// definition once. The retail leg (OPENNOVA_JO_DIR): every effect the install's catalog defines spawns
// from its closure exactly as from the whole catalog.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/particle/parser.h>
#include <runtime/particle/effect_closure.h>
#include <runtime/particle/effect_scene.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"

namespace particle = opennova::particle;

namespace {

// A particle definition `id` that lives `age` seconds, emitting for `dur` seconds at `rate` a second, its
// graphic `graphic` (none: no layer), its child `child`.
std::string particle_text(const std::string &id, float dur, float rate, float age, const std::string &graphic = "",
                          const std::string &child = "", const std::string &flags = "") {
	std::string out = "[particledef]\n{\n\tid = " + id + ";\n";
	if (!child.empty()) out += "\tchild_id = " + child + ";\n";
	if (!flags.empty()) out += "\tflags = " + flags + ";\n";
	out += "\temit_dur = " + std::to_string(dur) + ";\n\temit_rate = " + std::to_string(rate) +
	       ";\n\temit_burst = 1;\n\tage = " + std::to_string(age) + ";\n\tscale = 0.5;\n\tspeed = 2.0;\n\tspread = 30.0;\n"
	       "\tgravity = 1.0;\n\tscale_func = grow;\n";
	if (!graphic.empty()) out += "\tgraphic1 = " + graphic + ", additive;\n";
	return out + "}\n\n";
}

std::string effect_text(const std::string &id, const std::string &pdefs) {
	return "[effectdef]\n{\n\tid = " + id + ";\n\tpdefs = " + pdefs + ";\n}\n\n";
}

std::string table_text(const std::string &id, int value) {
	std::string out = "[tabledef]\n{\n\tid = " + id + ";\n";
	for (int row = 1; row <= 32; ++row) {
		out += "\ttl" + std::to_string(row) + " =";
		for (int i = 0; i < 8; ++i) out += std::string(i ? ", " : " ") + std::to_string(value);
		out += ";\n";
	}
	return out + "}\n\n";
}

particle::ParticleFile parsed(const std::string &text) {
	particle::ParticleFile file;
	particle::ParseError error;
	particle::load_particles_from_buffer(text.data(), text.size(), file, error);
	return file;
}

particle::EffectCatalogDocument document(const std::string &source, const std::string &text) {
	particle::EffectCatalogDocument out;
	out.source = source;
	out.file = parsed(text);
	return out;
}

// What a scene holds after `effect` spawned at the play pose (no orientation: around +Y) and ran `ticks`
// game ticks: its particles' positions, flattened, and its live counts.
struct Ran {
	std::vector<float> positions;
	particle::EffectLiveCounts counts;
	particle::EffectSpawnStatus status = particle::EffectSpawnStatus::InvalidHandle;
};
Ran run(particle::EffectScene &scene, const std::string &effect, int ticks) {
	Ran out;
	scene.reset_runtime_state();
	particle::EffectSpawnRequest request;
	request.effect = scene.intern(effect);
	request.pose = particle::descriptor_pose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
	out.status = scene.spawn(request).status;
	particle::EffectAdvanceRequest step;
	step.delta_seconds = 1.0f / 62.5f;
	for (int i = 0; i < ticks; ++i) scene.advance_simulation(step);
	particle::ParticleFrameSnapshot snapshot;
	scene.write_snapshot(snapshot);
	for (const particle::Particle &p : snapshot.particles) {
		out.positions.push_back(p.position.x);
		out.positions.push_back(p.position.y);
		out.positions.push_back(p.position.z);
	}
	out.counts = scene.live_counts();
	return out;
}

bool same_run(const Ran &a, const Ran &b) {
	return a.status == b.status && a.positions == b.positions && a.counts.group_count == b.counts.group_count &&
	       a.counts.emitter_count == b.counts.emitter_count && a.counts.particle_count == b.counts.particle_count;
}

int test_closure() {
	// Two documents in load order: the first defines Burst (two members, the first with a child) and a
	// table; the second defines Burst again (passed over), the members and the child, a table of the same
	// name (the first wins for a plain curve), and a particle nothing names.
	const std::vector<particle::EffectCatalogDocument> documents = {
	        document("a.ptl", effect_text("Burst", "Spark, Smoke") + particle_text("Spark", 0.2f, 40.0f, 0.4f, "", "Ember") +
	                                  table_text("grow", 200)),
	        document("b.ptl", effect_text("burst", "Smoke") + particle_text("SMOKE", 0.3f, 20.0f, 0.6f) +
	                                  particle_text("Ember", 0.1f, 10.0f, 0.2f) + particle_text("Unused", 1.0f, 1.0f, 1.0f) +
	                                  table_text("grow", 100) + effect_text("Broken", "Spark, Missing") +
	                                  effect_text("stockeffect", "Smoke")),
	};
	const particle::EffectClosure burst = particle::effect_closure(documents, "BURST");
	TEST_EXPECT(burst.found && !burst.stock && burst.effect == "Burst" && burst.source == "a.ptl" && burst.shadowed == 1 &&
	            burst.members == std::vector<std::string>({"Spark", "Smoke"}) && burst.unresolved_member == 2 && burst.spawns());
	TEST_EXPECT(burst.definitions.size() == 3 && burst.definitions[0].id == "Spark" && burst.definitions[1].id == "Ember" &&
	            burst.definitions[1].child && burst.definitions[1].member == 0 && burst.definitions[2].id == "SMOKE" &&
	            burst.definitions[2].source == "b.ptl" && !burst.definitions[2].child);
	TEST_EXPECT(burst.config.documents.size() == 1 && burst.config.documents[0].file.particles.size() == 3 &&
	            burst.config.documents[0].file.tables.size() == 2 && burst.config.documents[0].file.effects.size() == 1);
	// A scene over the closure spawns what the whole catalog spawns, particle for particle.
	particle::EffectSceneConfig whole;
	whole.documents = documents;
	particle::EffectScene full, alone;
	full.open(whole);
	alone.open(burst.config);
	for (const int ticks : {0, 1, 7, 30, 90}) {
		const Ran a = run(full, "Burst", ticks), b = run(alone, "Burst", ticks);
		TEST_EXPECT(a.status == particle::EffectSpawnStatus::Spawned && same_run(a, b));
		if (ticks == 7) TEST_EXPECT(a.counts.emitter_count == 3 && a.counts.particle_count > 0);
	}
	// One member no particle registers: all or nothing, nothing spawns from either.
	const particle::EffectClosure broken = particle::effect_closure(documents, "Broken");
	TEST_EXPECT(broken.found && broken.unresolved_member == 1 && !broken.spawns() && broken.definitions.empty());
	particle::EffectScene broken_scene;
	broken_scene.open(broken.config);
	TEST_EXPECT(run(full, "Broken", 5).status == particle::EffectSpawnStatus::EmptyEffect &&
	            run(broken_scene, "Broken", 5).status == particle::EffectSpawnStatus::EmptyEffect);
	// An unknown name: the stock effect stands in, as the intern clones it.
	const particle::EffectClosure unknown = particle::effect_closure(documents, "Effect_NoSuch");
	TEST_EXPECT(!unknown.found && unknown.stock && unknown.effect == "stockeffect" && unknown.spawns());
	particle::EffectScene stock_scene;
	stock_scene.open(unknown.config);
	TEST_EXPECT(same_run(run(full, "Effect_NoSuch", 20), run(stock_scene, "Effect_NoSuch", 20)));
	// No stockeffect either: nothing at all.
	const particle::EffectClosure none = particle::effect_closure({documents[0]}, "Effect_NoSuch");
	TEST_EXPECT(!none.found && !none.stock && !none.spawns() && none.config.documents.empty());
	std::printf("closure: Burst from a.ptl (one shadowed), 3 definitions, alike to the whole catalog at 5 ticks\n");
	return 0;
}

int test_closures() {
	// Smoke and Fire share a table; Smoke named twice. One config holds each effect and each definition
	// once, and every table once; each name keeps its own closure, in the order named.
	const std::vector<particle::EffectCatalogDocument> documents = {
	        document("fx.ptl", effect_text("Effect_Smoke", "Puff") + effect_text("Effect_Fire", "Flame") +
	                                   particle_text("Puff", 0.3f, 20.0f, 0.6f) + particle_text("Flame", 0.2f, 30.0f, 0.4f) +
	                                   table_text("grow", 128)),
	        document("more.ptl", effect_text("effect_smoke", "Flame") + particle_text("PUFF", 1.0f, 1.0f, 1.0f) +
	                                     table_text("shrink", 64)),
	};
	particle::EffectSceneConfig limits;
	limits.random_seed = 77;
	limits.max_live_groups = 9;
	std::vector<particle::EffectClosure> each;
	const particle::EffectSceneConfig config =
			particle::effect_closures(documents, {"Effect_Smoke", "Effect_Fire", "EFFECT_SMOKE"}, limits, each);
	TEST_EXPECT(each.size() == 3 && each[0].spawns() && each[1].spawns() && each[2].spawns() &&
	            each[0].effect == "Effect_Smoke" && each[1].effect == "Effect_Fire" && each[2].effect == "Effect_Smoke");
	TEST_EXPECT(config.random_seed == 77 && config.max_live_groups == 9 && config.documents.size() == 1);
	const particle::ParticleFile &merged = config.documents[0].file;
	TEST_EXPECT(config.documents[0].source == "fx.ptl" && merged.effects.size() == 2 && merged.particles.size() == 2 &&
	            merged.tables.size() == 2 && merged.particles[0].id == "Puff" && merged.particles[1].id == "Flame");
	// A scene over the folded config spawns each name as the whole catalog does (both seeded alike).
	particle::EffectSceneConfig whole = limits;
	whole.documents = documents;
	particle::EffectScene full, folded;
	full.open(whole);
	folded.open(config);
	for (const char *name : {"Effect_Smoke", "Effect_Fire"})
		TEST_EXPECT(same_run(run(full, name, 12), run(folded, name, 12)));
	// No name found an effect (and no stockeffect): no document at all.
	const particle::EffectSceneConfig nothing = particle::effect_closures(documents, {"Effect_NoSuch"}, limits, each);
	TEST_EXPECT(each.size() == 1 && !each[0].found && nothing.documents.empty() && nothing.random_seed == 77);
	std::printf("closures: Smoke, Fire, Smoke folded to 2 effects and 2 definitions\n");
	return 0;
}

// Every effect the install's catalog defines spawns from its closure exactly as from the whole catalog.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's particle files)");
		return 0;
	}
	opennova::ResourceIndex index;
	TEST_EXPECT(index.scan(install));
	std::vector<particle::EffectCatalogDocument> documents;
	for (const std::string &name : index.effect_files()) {
		std::vector<uint8_t> bytes;
		TEST_EXPECT(index.read_file(name, bytes));
		particle::EffectCatalogDocument read;
		read.source = name;
		particle::ParseError error;
		if (!particle::load_particles_from_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(), read.file,
		                                          error)) {
			std::printf("retail: %s does not read (%s, line %d)\n", name.c_str(), error.message.c_str(), error.line);
			continue;
		}
		documents.push_back(std::move(read));
	}
	particle::EffectSceneConfig whole;
	whole.documents = documents;
	particle::EffectScene full;
	full.open(whole);
	size_t effects = 0, spawned = 0;
	std::vector<std::string> seen;
	for (const particle::EffectCatalogDocument &read : documents)
		for (const particle::EffectDef &effect : read.file.effects) {
			const std::string key = opennova::strutil::to_lower(effect.id);
			if (effect.id.empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
			seen.push_back(key);
			const particle::EffectClosure closure = particle::effect_closure(documents, effect.id);
			particle::EffectScene alone;
			alone.open(closure.config);
			const Ran a = run(full, effect.id, 25), b = run(alone, effect.id, 25);
			if (!same_run(a, b)) std::fprintf(stderr, "retail: %s differs alone\n", effect.id.c_str());
			TEST_EXPECT(same_run(a, b) && closure.spawns() == (a.status == particle::EffectSpawnStatus::Spawned));
			++effects;
			spawned += a.status == particle::EffectSpawnStatus::Spawned;
		}
	TEST_EXPECT(documents.size() > 50 && effects > 100);
	std::printf("retail: %zu particle files, %zu effects, %zu spawn, each alike from its closure\n", documents.size(),
	            effects, spawned);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_closure() != 0) return 1;
	if (test_closures() != 0) return 1;
	if (test_retail() != 0) return 1;
	std::printf("particle_effect_closure: ok\n");
	return 0;
}
