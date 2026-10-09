// DI-14 (ADR 0046, the deep-integration plan's particle effect preview): a particle file's effect played
// through the engine's own effect scene (the reader's places, each effect's block and its id's place, are
// formats/particle's: tests/particle/particle_effect_places_test). The closure (runtime/particle/effect_closure): the effect the catalog registers first,
// its members all or nothing, its child chains, every table, and a scene over it alone spawning what the
// whole catalog spawns, particle for particle; the stock effect for an unknown name. The playback
// (preview/effect_playback): spawned at tick 0, stepped a tick at a time (one call or many alike), spawned
// again at the tick it dies while it loops and left dead when it does not, a jump spawning it pre-aged
// (bounded by the engine's catch-up), the wind. The particle type (documents/particle_type): the
// reader's stop and a duplicate effect as findings at their places. The kinds'
// table (ViewportKind::Effect, the particle type's Preview kind; the script device its Main). Through a
// real session over a project: the catalog in the effect system's order (a .ptl before another, the
// gore set the project picks), the viewport made as the file opens and its device given the scene, its
// envelope (what the game resolves the name to, a definition another file shadows, the playing cycle),
// the clock sought to 0 for an effect newly shown, a SetViewport of its options refused for an effect
// the file lacks, a Go to of an effect's name opening the text at its id (the graph's symbol, read from
// the open document too) and the preview following it, an edit reopening the scene with its age kept, a
// file the reader stops in, and a graphic the device read moving. The canvas: an orbit, a pan, the
// wheel and F each one SetViewport of its camera; the frame and replay commands. The retail leg
// (OPENNOVA_JO_DIR): every effect the install's catalog defines spawns from its closure exactly as from
// the whole catalog.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/resource_index/resource_index.h>
#include <editor/assets/install_view.h>
#include <editor/documents/document_types.h>
#include <editor/documents/particle_type.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/text_document.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/effect_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/particle/parser.h>
#include <runtime/particle/effect_closure.h>
#include <runtime/particle/effect_scene.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
namespace particle = opennova::particle;
using opennova::io::JsonValue;

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

// What a scene holds after `effect` spawned at the play pose and ran `ticks` game ticks: its particles'
// positions, flattened, and its live counts.
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
	request.pose = effect_play_pose();
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

// --- the closure ---------------------------------------------------------------------------------------

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

// --- the playback --------------------------------------------------------------------------------------

std::vector<particle::EffectCatalogDocument> short_catalog() {
	// A puff that emits for 0.1 s and whose particles live 0.2 s: dead within 0.3 s (about 19 ticks).
	return {document("puff.ptl", effect_text("Puff", "Dot") + particle_text("Dot", 0.1f, 50.0f, 0.2f) + table_text("grow", 128))};
}

int test_playback() {
	const particle::EffectClosure closure = particle::effect_closure(short_catalog(), "Puff");
	EffectPlayback loop;
	loop.open(closure.config, "Puff");
	EffectPlayOptions options;
	loop.play_to(0, options);
	TEST_EXPECT(loop.alive() && loop.spawns() == 1 && loop.cycle_start() == 0 && loop.age() == 0 &&
	            loop.last_status() == particle::EffectSpawnStatus::Spawned);
	// A tick at a time to its death: spawned again at the tick it died.
	int32_t died = -1;
	for (int32_t tick = 1; tick <= 60 && died < 0; ++tick) {
		loop.play_to(tick, options);
		if (loop.spawns() == 2) died = tick;
	}
	TEST_EXPECT(died > 10 && died < 40 && loop.cycle_start() == died && loop.alive());
	// The same ticks in one call: the same scene (each tick the scene's fixed step).
	EffectPlayback once;
	once.open(closure.config, "Puff");
	once.play_to(0, options);
	once.play_to(died + 5, options);
	loop.play_to(died + 5, options);
	particle::ParticleFrameSnapshot a, b;
	loop.scene()->write_snapshot(a);
	once.scene()->write_snapshot(b);
	TEST_EXPECT(once.spawns() == 2 && once.cycle_start() == died && a.particles.size() == b.particles.size());
	for (size_t i = 0; i < a.particles.size(); ++i)
		TEST_EXPECT(a.particles[i].position.x == b.particles[i].position.x && a.particles[i].position.y == b.particles[i].position.y);
	// Not looping: dead it stays, until the clock goes back.
	EffectPlayback single;
	single.open(closure.config, "Puff");
	EffectPlayOptions no_loop;
	no_loop.loop = false;
	single.play_to(0, no_loop);
	single.play_to(80, no_loop);
	TEST_EXPECT(!single.alive() && single.spawns() == 1 && single.scene()->live_counts().group_count == 0);
	// Loop turned on: a cycle begins at once.
	single.play_to(80, options);
	TEST_EXPECT(single.alive() && single.spawns() == 2 && single.cycle_start() == 80);
	// A step back: a jump, spawned again pre-aged by its age in the playing cycle.
	single.play_to(85, options);
	single.play_to(83, options);
	TEST_EXPECT(single.alive() && single.spawns() == 3 && single.pre_aged() == 3 && single.cycle_start() == 80);
	// Back before the cycle: its age since tick 0.
	single.play_to(5, options);
	TEST_EXPECT(single.alive() && single.pre_aged() == 5 && single.cycle_start() == 0);
	// A jump past the engine's catch-up bound: the age it allows; one so old it died within it spawned anew.
	EffectPlayback far;
	far.open(closure.config, "Puff");
	far.play_to(0, options);
	far.play_to(1000, options);
	TEST_EXPECT(far.alive() && far.cycle_start() == 1000 && far.spawns() == 3 && far.age() == 0);
	// The wind: GLOBALWIND particles drift with the mission header's.
	const particle::EffectClosure windy_closure = particle::effect_closure(
	        {document("w.ptl", effect_text("Windy", "Drift") + particle_text("Drift", 1.0f, 30.0f, 2.0f, "", "", "GLOBALWIND") +
	                                   table_text("grow", 128))},
	        "Windy");
	EffectPlayback calm, windy;
	EffectPlayOptions wind;
	wind.wind_speed = 200;
	wind.wind_direction = 90;
	calm.open(windy_closure.config, "Windy");
	windy.open(windy_closure.config, "Windy");
	calm.play_to(0, options);
	windy.play_to(0, wind);
	calm.play_to(40, options);
	windy.play_to(40, wind);
	particle::ParticleFrameSnapshot still, blown;
	calm.scene()->write_snapshot(still);
	windy.scene()->write_snapshot(blown);
	TEST_EXPECT(!still.particles.empty() && still.particles.size() == blown.particles.size() &&
	            still.particles[0].position.x != blown.particles[0].position.x);
	// Closed: nothing plays.
	far.close();
	far.play_to(10, options);
	TEST_EXPECT(!far.scene() && !far.alive());
	std::printf("playback: Puff died at tick %d and spawned again there; a jump to 1000 spawned it anew\n", died);
	return 0;
}

// --- the particle type -----------------------------------------------------------------------------------

int test_particle_type() {
	const DocumentType *type = document_type_for(AssetKind::Particles);
	TEST_EXPECT(type && type->id == DocumentTypeId::Particles && std::string(type->name) == "particle" && !type->references &&
	            document_content(*type) == DocumentContent::Text);
	const auto validate = [&](const std::string &text) {
		std::unique_ptr<DocumentBase> made = type->make();
		Diagnostic error;
		made->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "particles/t.ptl", AssetKind::Particles, "jo", error);
		return type->validate_file(*made);
	};
	TEST_EXPECT(validate(effect_text("Ok", "Dot") + particle_text("Dot", 0.1f, 1.0f, 1.0f)).empty());
	// The reader stops at an open block: one finding, at its line.
	const std::vector<Diagnostic> open = validate(effect_text("Ok", "Dot") + "[particledef]\n{\n\tid = Dot;\n");
	if (!open.empty()) std::printf("unreadable at %zu:%zu: %s\n", open[0].line, open[0].column, open[0].message.c_str());
	TEST_EXPECT(open.size() == 1 && open[0].code() == "particle.unreadable" && open[0].severity == DiagnosticSeverity::Warning &&
	            open[0].line == 10 && open[0].column == 1);
	// An effect defined twice: the second, at its id.
	const std::vector<Diagnostic> twice = validate(effect_text("Twice", "Dot") + effect_text("twice", "Dot"));
	TEST_EXPECT(twice.size() == 1 && twice[0].code() == "particle.duplicate_effect" && twice[0].line == 9 &&
	            twice[0].column == 7 && twice[0].message.find("line 3") != std::string::npos);
	std::printf("particle type: %s at 10:1, %s at 9:7\n", open[0].code().c_str(), twice[0].code().c_str());
	return 0;
}

// --- the kinds' table ------------------------------------------------------------------------------------

int test_kind() {
	ViewportKind named = ViewportKind::kCount;
	TEST_EXPECT(std::string(viewport_kind_token(ViewportKind::Effect)) == "effect" && viewport_kind_from_token("effect", named) &&
	            named == ViewportKind::Effect);
	const ViewportKindRow &row = viewport_kind_row(ViewportKind::Effect);
	TEST_EXPECT(row.kind == ViewportKind::Effect && row.role == ViewportRole::Preview && row.as_saved && !row.part &&
	            !row.holds_for_gesture && row.canvas && row.feed_count == 1 && !row.files);
	TEST_EXPECT(preview_kind_of(DocumentTypeId::Particles) == ViewportKind::Effect &&
	            main_viewport_kind(DocumentTypeId::Particles) == ViewportKind::Script &&
	            default_viewport_kind(DocumentTypeId::Particles) == ViewportKind::Effect);
	std::unique_ptr<ViewportModel> made = row.make("particles/t.ptl");
	TEST_EXPECT(made && made->make_canvas() && made->camera_json().is_object() && std::string(made->reason()) == "no_project" &&
	            made->status() == ViewportStatus::Empty && made->options_json().get("loop") &&
	            made->options_json().get("loop")->boolean);
	return 0;
}

// --- through a session -------------------------------------------------------------------------------------

struct EffectRig {
	editor_test::TempProjectDir dir{"opennova_editor_effect_viewport"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	const std::string first = "particles/a.ptl";
	const std::string second = "particles/b.ptl";
	std::string root() const { return dir.file("project"); }
	const SessionView &view() const { return session.view(); }
	const EffectViewport *viewport(const std::string &path) {
		return static_cast<const EffectViewport *>(session.viewports().find(path, ViewportKind::Effect));
	}
	editor_test::FakeDevice *device(const std::string &path) { return devices.held(path, ViewportKind::Effect); }
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	JsonValue envelope(const std::string &path) {
		const EffectViewport *model = viewport(path);
		return model ? viewport_to_json(view(), *model, JsonPage()) : JsonValue::make_null();
	}
};

bool make_project(EffectRig &rig) {
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Effects"));
	editor_test::create_missing_files(rig.session);
	const std::string root = rig.root();
	// a.ptl sorts before b.ptl: its Shared wins; b.ptl's own Smoke is its alone. A gore-set .ptg the
	// project does not load (no fgn2.bin): the .ptu is the set.
	const bool written =
	        editor_test::write_text(root + "/" + rig.first, effect_text("Shared", "Dot") + particle_text("Dot", 0.1f, 50.0f, 0.2f, "dot.tga") +
	                                                           table_text("grow", 128)) &&
	        editor_test::write_text(root + "/" + rig.second, effect_text("Smoke", "Cloud") + effect_text("Shared", "Cloud") +
	                                                            particle_text("Cloud", 0.5f, 20.0f, 1.0f, "cloud.tga")) &&
	        editor_test::write_text(root + "/particles/gore.ptu", effect_text("Gore", "Dot")) &&
	        editor_test::write_text(root + "/particles/gore.ptg", effect_text("GoreG", "Dot"));
	editor_test::handle_to_end(rig.session, request::rescan());
	return written;
}

int test_session() {
	EffectRig rig;
	TEST_EXPECT(make_project(rig));
	editor_test::handle_to_end(rig.session, request::open_document(rig.second));
	rig.pump();
	// The Preview window's kind for it; its viewport made, its device given the scene; the script device
	// is its Document tab's.
	TEST_EXPECT(rig.view().documents.preview_shown == ViewportKind::Effect);
	const EffectViewport *viewport = rig.viewport(rig.second);
	editor_test::FakeDevice *device = rig.device(rig.second);
	TEST_EXPECT(viewport && device && device->since(0) == std::vector<ViewportAction>{ViewportAction::Rebuild});
	// The catalog in the effect system's order: the .ptl files by name, then the gore set the project picks.
	const std::vector<PreviewEffectCatalog::File> &files = viewport->catalog().files();
	TEST_EXPECT(files.size() == 3 && files[0].path == rig.first && files[1].path == rig.second &&
	            files[2].path == "particles/gore.ptu" && files[0].read && files[1].read);
	// The file's first effect shown, from tick 0; Shared listed as another file's.
	TEST_EXPECT(viewport->status() == ViewportStatus::Ready && viewport->shown_effect() == "Smoke" &&
	            viewport->effects().size() == 2 && viewport->effects()[0].registered && !viewport->effects()[1].registered &&
	            viewport->playback().alive() && viewport->loaded());
	JsonValue envelope = rig.envelope(rig.second);
	const JsonValue *body = envelope.get("body");
	TEST_EXPECT(body && body->get_string("effect", "") == "Smoke" && body->get("resolved") &&
	            body->get("resolved")->get_string("defined_in", "") == rig.second && body->get("play") &&
	            body->get("play")->get("alive")->boolean);
	// The clock runs: the effect plays its ticks.
	rig.session.advance(0.5);
	rig.pump();
	TEST_EXPECT(viewport->playback().tick() == int32_t(rig.view().documents.viewports->clock().ticks()) &&
	            viewport->playback().tick() >= 30 && viewport->playback().scene()->live_counts().particle_count > 0);
	// Shared: the game spawns a.ptl's for the name; the clock sought to 0 as it is newly shown.
	EffectViewportOptions options = viewport->options();
	options.effect = "shared";
	TEST_EXPECT(editor_test::serve(rig.session, {request::set_viewport(rig.second, effect_options_change(options))}));
	rig.pump();
	TEST_EXPECT(viewport->shown_effect() == "Shared" && viewport->closure().source == rig.first &&
	            rig.view().documents.viewports->clock().ticks() == 0 && viewport->playback().tick() == 0);
	std::string file, locator;
	TEST_EXPECT(viewport->spawned_place(file, locator) && file == rig.first && locator == "3:7");
	envelope = rig.envelope(rig.second);
	TEST_EXPECT(envelope.get("body")->get("resolved")->get_string("defined_in", "") == rig.first &&
	            !envelope.get("body")->get("resolved")->get("this_file")->boolean);
	// An effect the file lacks: refused, the options as they were.
	options.effect = "Nothing";
	rig.session.handle(request::set_viewport(rig.second, effect_options_change(options)));
	TEST_EXPECT(!rig.session.outcome().done() && viewport->options().effect == "Shared");
	// A Go to of Smoke's name: the graph's symbol (read from the open document) opens the text at its id,
	// and the preview follows it.
	const AssetGraph &graph = *rig.view().findings.graph;
	const GraphSymbol *smoke = nullptr;
	for (const GraphSymbol *symbol : graph.symbols_of_kind(ReferenceKind::Particle))
		if (symbol->display == "Smoke") smoke = symbol;
	TEST_EXPECT(smoke && smoke->file == rig.second && smoke->locator == "3:7" && smoke->line == 3);
	const ReferenceTarget target = symbol_target(*rig.view().project.scan, *smoke);
	TEST_EXPECT(target.editable && target.locator == "3:7");
	editor_test::handle_to_end(rig.session, request::open_document(target.file, target.locator, target.field));
	rig.pump();
	TEST_EXPECT(viewport->shown_effect() == "Smoke" && viewport->options().effect == "Smoke");
	// An edit of the text: the scene opened again over it, the effect at the age it had.
	rig.session.advance(0.2);
	rig.pump();
	const uint64_t opens = viewport->opens();
	const int32_t age = viewport->playback().age();
	DocumentBase *opened = rig.session.document_base_for(rig.second);
	const TextDocument *text = opened ? text_of(*static_cast<const DocumentBase *>(opened)) : nullptr;
	TEST_EXPECT(text && age > 0);
	size_t offset = 0;
	TEST_EXPECT(text->offset_of(1, 1, offset));
	editor_test::handle_to_end(rig.session,
	                           request::edit_record(rig.second, TextDocument::replace(text->span_at(0, 0), "\n")));
	rig.pump();
	TEST_EXPECT(viewport->opens() == opens + 1 && viewport->playback().cycle_start() == viewport->playback().tick() - age &&
	            device->last() == ViewportAction::Rebuild);
	// The symbol moved with the edit (the graph reads the open document through the reader).
	const GraphSymbol *moved = nullptr;
	for (const GraphSymbol *symbol : rig.view().findings.graph->symbols_of_kind(ReferenceKind::Particle))
		if (symbol->display == "Smoke") moved = symbol;
	TEST_EXPECT(moved && moved->locator == "4:7");
	// A text the reader stops in: the viewport says where, its effect kept for when it reads again.
	const size_t end = text->text().size();
	editor_test::handle_to_end(rig.session,
	                           request::edit_record(rig.second, TextDocument::replace(text->span_at(end, 0), "[effectdef]\n{\n")));
	rig.pump();
	TEST_EXPECT(viewport->view_status() == EffectViewStatus::Unreadable && viewport->status() == ViewportStatus::Failed &&
	            viewport->detail().find("line") != std::string::npos && device->last() == ViewportAction::Clear);
	editor_test::handle_to_end(rig.session, request::undo(rig.second));
	rig.pump();
	TEST_EXPECT(viewport->status() == ViewportStatus::Ready && viewport->shown_effect() == "Smoke" &&
	            device->last() == ViewportAction::Rebuild);
	// A graphic the device read moves: the picture built again.
	device->reads = {"particles/a.ptl"};
	editor_test::handle_to_end(rig.session, request::open_document(rig.first));
	rig.pump();
	editor_test::FakeDevice *first_device = rig.device(rig.first);
	TEST_EXPECT(first_device && rig.viewport(rig.first) && rig.viewport(rig.first)->shown_effect() == "Shared" &&
	            rig.viewport(rig.first)->closure().source == rig.first);
	// The gore set the project does not pick is not loaded: its effects are what the game spawns for the
	// name elsewhere (here none: the stock effect is absent, nothing spawns).
	editor_test::handle_to_end(rig.session, request::open_document("particles/gore.ptg"));
	rig.pump();
	const EffectViewport *gore = rig.viewport("particles/gore.ptg");
	TEST_EXPECT(gore && !gore->loaded() && gore->shown_effect() == "GoreG" && gore->view_status() == EffectViewStatus::SpawnsNothing);
	std::printf("session: Smoke played from b.ptl, Shared resolved to a.ptl at 3:7, an edit reopened the scene at age %d\n", age);
	return 0;
}

// --- the canvas ----------------------------------------------------------------------------------------------

int test_canvas() {
	EffectRig rig;
	TEST_EXPECT(make_project(rig));
	editor_test::handle_to_end(rig.session, request::open_document(rig.second));
	rig.pump();
	const EffectViewport *viewport = rig.viewport(rig.second);
	TEST_EXPECT(viewport);
	const ViewportContext context = viewport_context(rig.view(), *viewport);
	const std::unique_ptr<CanvasHalf> made = viewport->make_canvas();
	CanvasHalf &canvas = *made;
	editor_test::Gathered out;
	canvas.follow(*viewport, context, out);
	CanvasInput in;
	in.width = 800;
	in.height = 600;
	in.hovered = true;
	in.pressed = in.down = true;
	in.mouse = in.screen = CanvasPoint{100.0f, 100.0f};
	canvas.input(context, in, out);
	in.pressed = false;
	in.screen = in.mouse = CanvasPoint{160.0f, 120.0f};
	in.delta = CanvasPoint{60.0f, 20.0f};
	canvas.input(context, in, out);
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::SetViewport);
	TEST_EXPECT(editor_test::serve(rig.session, out.requests) && viewport->camera().yaw != 0.6f);
	// The wheel: one SetViewport, nearer.
	out.requests.clear();
	in.down = false;
	in.delta = CanvasPoint{};
	canvas.input(context, in, out);
	const float distance = viewport->camera().distance;
	in.wheel = 1.0f;
	canvas.input(context, in, out);
	TEST_EXPECT(!out.requests.empty() && editor_test::serve(rig.session, out.requests) && viewport->camera().distance < distance);
	// The commands: frame (the camera on the live particles), replay (the clock at 0); another refused.
	out.requests.clear();
	std::string error;
	rig.session.advance(0.3);
	rig.pump();
	TEST_EXPECT(viewport->command(context, "frame", {}, out, error) && viewport->command(context, "replay", {}, out, error) &&
	            out.requests.size() == 2 && editor_test::serve(rig.session, out.requests));
	TEST_EXPECT(rig.view().documents.viewports->clock().ticks() == 0);
	TEST_EXPECT(!viewport->command(context, "align_left", {}, out, error) && error.find("frame, replay") != std::string::npos);
	// Nothing is dragged; a point names nothing.
	TEST_EXPECT(!viewport->drag(context, ViewportDrag(), out, error) && viewport->hit(context, 10, 10).index == -1);
	return 0;
}

// --- the retail leg ----------------------------------------------------------------------------------------------

// Every effect the install's catalog defines spawns from its closure exactly as from the whole catalog.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's particle files)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	InstallView install_view;
	std::string why;
	TEST_EXPECT(install_view.open(install_spec(install, project), why));
	const opennova::Vfs &mount = install_view.vfs();
	std::vector<std::string> names;
	bool german = false;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		names.push_back(location.logical_name);
		german = german || opennova::strutil::iequals(location.logical_name, "fgn2.bin");
	}
	const std::vector<std::string> order = opennova::effect_file_order(names, german ? ".ptg" : ".ptu");
	std::vector<particle::EffectCatalogDocument> documents;
	for (const std::string &name : order) {
		std::vector<uint8_t> bytes;
		TEST_EXPECT(mount.read_file(name, bytes));
		particle::EffectCatalogDocument read;
		read.source = name;
		particle::ParseError error;
		if (!particle::load_particles_from_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(), read.file, error)) {
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
	if (test_playback() != 0) return 1;
	if (test_particle_type() != 0) return 1;
	if (test_kind() != 0) return 1;
	if (test_session() != 0) return 1;
	if (test_canvas() != 0) return 1;
	if (test_retail() != 0) return 1;
	std::printf("effect_viewport: ok\n");
	return 0;
}
