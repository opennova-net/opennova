// What one effect's spawn reads of the effect catalog (runtime/particle/effect_closure): the effect
// the catalog registers first, its members all or nothing, its child chains, every table, and a scene
// over it alone spawning what the whole catalog spawns, particle for particle; the stock effect for an
// unknown name. Several names' closures folded into one config (effect_closures), each effect and
// definition once. And every effect of a catalog of the shipped files' cross-file shapes spawns from its
// closure exactly as from the whole catalog.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <formats/particle/parser.h>
#include <runtime/particle/effect_closure.h>
#include <runtime/particle/effect_scene.h>

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

// The text as the game's reader takes it, its lines ended CR LF.
particle::ParticleFile parsed(const std::string &text) {
	const std::string crlf = opennova::strutil::normalized_crlf_line_ends(text);
	particle::ParticleFile file;
	particle::ParseError error;
	particle::load_particles_from_buffer(crlf.data(), crlf.size(), file, error);
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

// A particle definition of the shipped files' fuller shape: `child` its child, `curves` its curve keys
// whole ("alpha_func = fade", "g2_scale_func = burn invert"), a second layer when `layers` is 2, `rate`
// its emit rate, `flags` its flags line's words (none: no flags line), `max_override` its emit_maxoverride.
std::string shaped_particle(const std::string &id, const std::string &child, const std::vector<std::string> &curves,
                            int layers = 1, int rate = 30, const std::string &flags = "", int max_override = 0) {
	std::string out = "[particledef]\n{\nid\t= " + id + ";\n";
	if (!child.empty()) out += "child_id\t= " + child + ";\n";
	if (!flags.empty()) out += "flags\t=  " + flags + " ;\n";
	out += "move\t=  NORMAL ;\n\n\temit_dur\t= 0.300;\n\temit_rate\t= " + std::to_string(rate) +
	       ".000;\n\temit_burst\t= 2;\n\temit_maxoverride\t= " + std::to_string(max_override) +
	       ";\n\temit_dur\t= 0.300;\n"
	       "\tage\t= 0.500;\n\tscale\t= 0.400;\n\tspeed\t= 3.000;\n\tspread\t= 45.000;\n\tgravity\t= 1.000;\n";
	for (const std::string &curve : curves) out += "\t" + curve + ";\n";
	for (int layer = 1; layer <= layers; ++layer)
		out += "\tgraphic" + std::to_string(layer) + " =\tlayer" + std::to_string(layer) + ".tga, additive;\n\tg" +
		       std::to_string(layer) + "_scale\t= 0.500;\n";
	return out + "\n}\n\n";
}

// The shapes of the references between the shipped effect files (the 85 of the JO:CA install, read once on
// 2026-10-19 by the retired particle_effect_closure_retail row): an effect naming particles another file
// defines (122 members), a particle whose child another file defines (1), curves naming a table of another
// file (5735; table.ptl's and troytabl.ptl's tables) or of their own, reversed (1256) or inverted (4), an
// emit rate curve (26); an effect (37), a particle (160) and a table (104) a later file defines again, the
// first registration standing; a member named twice in one effect (3); an effect of no members (3); a
// member named in another case than its particle's (3); child chains two deep (44); and the definitions
// whose spawn takes their own branches: a child emitting faster than the game's tick (fx_cmkp.ptl's
// fx_carm_kids at 100 a second, veh_joexp.ptl's m1a1_smkchildf_trail at 75), a child whose emit rate rides
// a curve (foliage.ptl's jtreegrpl_faldebt on falldeb_table), a particle capped by emit_maxoverride
// (bugSwarm.ptl's Mosquito_Swarm_L2 at 80; 13 in all), the water-plane flags ABOVEH20 (24 particles,
// fx_carm_kids among them) and BELOWH20 (6).
std::vector<particle::EffectCatalogDocument> shipped_shapes() {
	return {
	        document("a_fx.ptl",
	                 effect_text("Effect_Hit", "Hit_Spark, Hit_Dust, Hit_Spark") +
	                         shaped_particle("Hit_Dust", "Hit_Grit", {"alpha_func = fade", "scale_func = grow reverse"}) +
	                         effect_text("Effect_Fire", "Fire_Flame") +
	                         shaped_particle("Fire_Flame", "",
	                                         {"emit_rate_func = slow", "scale_func = burn", "g1_alpha_func = fade",
	                                          "g2_scale_func = burn invert", "g2_alpha_func = fade reverse"},
	                                         2) +
	                         table_text("burn", 180)),
	        document("b_fx.ptl",
	                 effect_text("effect_fire", "Hit_Spark") +
	                         shaped_particle("FIRE_FLAME", "", {"scale_func = grow"}) +
	                         shaped_particle("Hit_Spark", "Hit_Ember", {"red_func = fade", "green_func = fade",
	                                                                    "blue_func = fade invert"}) +
	                         shaped_particle("Hit_Ember", "Hit_Ash", {"g1_scale_func = grow reverse"}) +
	                         effect_text("Effect_Case", "HIT_DUST, hit_ember") +
	                         "[effectdef]\n{\n\tid = Effect_None;\n\n}\n\n" + table_text("burn", 40)),
	        document("c_fx.ptl", shaped_particle("Hit_Grit", "", {"alpha_func = fade"}) +
	                                     shaped_particle("Hit_Ash", "", {"scale_func = slow reverse"}) +
	                                     effect_text("Effect_Grit", "Hit_Grit")),
	        document("table.ptl", table_text("fade", 255) + table_text("grow", 128) + table_text("slow", 64)),
	        document("stock.ptl", effect_text("stockeffect", "Stock_Puff") + shaped_particle("Stock_Puff", "", {})),
	        document("water_fx.ptl",
	                 effect_text("Effect_Kids", "Kid_Parent") + shaped_particle("Kid_Parent", "Kid_Fast", {}) +
	                         shaped_particle("Kid_Fast", "", {}, 1, 100, "AMBIENTCOLOR ABOVEH20") +
	                         effect_text("Effect_Fall", "Fall_Parent") +
	                         shaped_particle("Fall_Parent", "Fall_Debris", {}) +
	                         shaped_particle("Fall_Debris", "", {"emit_rate_func = slow"}, 1, 60) +
	                         effect_text("Effect_Swarm", "Swarm_Bugs") +
	                         shaped_particle("Swarm_Bugs", "", {}, 1, 30, "FOREVEREMIT BELOWH20", 80)),
	};
}

// Every effect the catalog defines spawns from its closure exactly as from the whole catalog, the first
// registration of each id the one asked for: one shadowed effect. The block of no pdefs line is no
// effect to the game, so the catalog holds none of it [orig: CParticleTableDef_ParseScriptLine @ 0x5E93CB].
int test_shipped_shapes() {
	const std::vector<particle::EffectCatalogDocument> documents = shipped_shapes();
	particle::EffectSceneConfig whole;
	whole.documents = documents;
	particle::EffectScene full;
	full.open(whole);
	size_t effects = 0, spawned = 0, shadowed = 0;
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
			if (!same_run(a, b)) std::fprintf(stderr, "shapes: %s differs alone\n", effect.id.c_str());
			TEST_EXPECT(same_run(a, b) && closure.spawns() == (a.status == particle::EffectSpawnStatus::Spawned));
			++effects;
			spawned += a.status == particle::EffectSpawnStatus::Spawned;
			shadowed += closure.shadowed;
		}
	TEST_EXPECT(effects == 8 && spawned == 8 && shadowed == 1);
	// The fire is the first file's (its two-layer flame), the shadowed one's member never asked for.
	const particle::EffectClosure fire = particle::effect_closure(documents, "EFFECT_FIRE");
	TEST_EXPECT(fire.source == "a_fx.ptl" && fire.members == std::vector<std::string>({"Fire_Flame"}) &&
	            fire.definitions.size() == 1 && fire.definitions[0].source == "a_fx.ptl");
	// The hit: its members across two files, the member named twice, the children across a file and two deep.
	const particle::EffectClosure hit = particle::effect_closure(documents, "Effect_Hit");
	TEST_EXPECT(hit.spawns() && hit.unresolved_member == 3 && hit.definitions.size() == 5);
	const Ran ran = run(full, "Effect_Hit", 7);
	TEST_EXPECT(ran.status == particle::EffectSpawnStatus::Spawned && ran.counts.particle_count > 0);
	std::printf("shapes: %zu files, %zu effects, %zu spawn, each alike from its closure\n", documents.size(), effects,
	            spawned);
	return 0;
}
} // namespace

int main() {
	if (test_closure() != 0) return 1;
	if (test_closures() != 0) return 1;
	if (test_shipped_shapes() != 0) return 1;
	std::printf("particle_effect_closure: ok\n");
	return 0;
}
