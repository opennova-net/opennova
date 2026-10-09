// DI-21 (ADR 0046, the deep-integration plan's "an item's picture: the Definition viewport"): a definition
// table's selected record drawn as the game draws the thing it defines. Headless, over minted fixtures (models
// built through our own writer, a particle file, an items.def, a weapon.def, an ammo.def, a bank and a wave),
// through a real session whose viewports fake devices follow.
//
// Pinned: the kinds' table (the definition's kind, the catalog's Preview, a record no page of its table); the
// engine's lifted death banks (world::death_bank_spawns: the families, their points, the Dead bank's fallback at
// the origin, nothing without a husk model); the effects' playback (preview/definition_effects: each spawn on its
// tick, a jump pre-aging every spawn due, one scene over several effects' closures); an item alive (its graphic,
// its particle slot at its user points along their directions, a drivable item's slot only while occupied, the
// enemy's model); its death (Destroying: the graphic until the swap, the husk from it with its fade's registers
// and its pieces' sections, the death's banks spawned at the piece model's points on the swap's tick, the death
// sound heard as the clock runs from the death, its first frame however long, and never over a seek); its husk
// (the wreck standing, its Fire and Other banks burning) and its final husk; a person posed as its spawn poses it
// on its graphic's rig; a weapon's third- and first-person models; an ammo's round as its tracer item; a powerup
// and a record of no model said in words; the envelope; the options refused; the canvas and its commands.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/definition_effects.h>
#include <editor/preview/definition_viewport.h>
#include <editor/preview/effect_catalog.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/lwf.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <runtime/world/destruction.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"
#include "fixtures/minimal_3di_builder.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }

// --- the fixtures ----------------------------------------------------------------------------------------------

using synth3di::Box;
using threedi::ThreediBuildVec3;

constexpr ThreediBuildVec3 kUp{0.0, 0.0, 1.0};
constexpr ThreediBuildVec3 kForward{1.0, 0.0, 0.0};

// The pump: one box, two Smoke points (one forward, one up) and its ground.
std::vector<uint8_t> pump_bytes() {
	synth3di::Model m;
	m.name = "fxpump";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "fxpump.tga");
	const int part = m.add_part(lod, 0, ThreediBuildVec3{});
	m.add_box(lod, part, paint, Box{{-0.5, -0.5, 0.0}, {0.5, 0.5, 1.0}});
	m.add_panm(lod, part, 0);
	m.add_user_point("Smoke", ThreediBuildVec3{0.4, 0.0, 1.0}, kForward, 0, synth3di::kUserPointEffect);
	m.add_user_point("smoke", ThreediBuildVec3{-0.4, 0.2, 1.0}, kUp, 0, synth3di::kUserPointEffect);
	m.add_user_point("ground", ThreediBuildVec3{}, kUp, 0, synth3di::kUserPointGameplay);
	std::vector<uint8_t> out;
	if (!synth3di::mint(m, out)) out.clear();
	return out;
}

// The pump's wreck: a hull and two more sections, its Dead, Fire (two) and Other points.
std::vector<uint8_t> wreck_bytes() {
	synth3di::Model m;
	m.name = "fxwreck";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "fxwreck.tga");
	for (int i = 0; i < 3; ++i) {
		const int part = m.add_part(lod, 0, ThreediBuildVec3{0.0, 0.3 * i, 0.0});
		m.add_box(lod, part, paint, Box{{-0.4, -0.4 + 0.3 * i, 0.0}, {0.4, -0.2 + 0.3 * i, 0.5}});
		m.add_panm(lod, part, 0);
	}
	m.add_user_point("Dead", ThreediBuildVec3{0.0, 0.0, 0.6}, kUp, 0, synth3di::kUserPointEffect);
	m.add_user_point("Fire", ThreediBuildVec3{0.2, 0.0, 0.5}, kUp, 0, synth3di::kUserPointEffect);
	m.add_user_point("Fire", ThreediBuildVec3{-0.2, 0.0, 0.5}, kForward, 0, synth3di::kUserPointEffect);
	m.add_user_point("Other", ThreediBuildVec3{0.0, 0.3, 0.4}, kUp, 0, synth3di::kUserPointEffect);
	std::vector<uint8_t> out;
	if (!synth3di::mint(m, out)) out.clear();
	return out;
}

// A particle `id` emitting `rate` a second for `dur` seconds, each living `age` seconds.
std::string particle_text(const std::string &id, float dur, float rate, float age) {
	return "[particledef]\n{\n\tid = " + id + ";\n\temit_dur = " + std::to_string(dur) + ";\n\temit_rate = " +
	       std::to_string(rate) + ";\n\temit_burst = 1;\n\tage = " + std::to_string(age) +
	       ";\n\tscale = 0.5;\n\tspeed = 2.0;\n\tspread = 30.0;\n\tgravity = 1.0;\n\tscale_func = grow;\n}\n\n";
}

std::string effect_text(const std::string &id, const std::string &pdefs) {
	return "[effectdef]\n{\n\tid = " + id + ";\n\tpdefs = " + pdefs + ";\n}\n\n";
}

// The effects: a smoke that emits for a long while, a death burst, a fire and an other.
std::string particles() {
	return particle_text("p_smoke", 30.0f, 10.0f, 2.0f) + particle_text("p_dead", 0.2f, 40.0f, 1.0f) +
	       particle_text("p_fire", 30.0f, 8.0f, 1.0f) + particle_text("p_other", 30.0f, 4.0f, 1.0f) +
	       effect_text("Effect_Smoke", "p_smoke") + effect_text("Effect_Dead", "p_dead") +
	       effect_text("Effect_Fire", "p_fire") + effect_text("Effect_Other", "p_other");
}

// A bank of one set playing one wave.
std::vector<uint8_t> bank_of(const std::string &set) {
	lwf::File bank;
	lwf::Single single;
	single.name = set;
	single.path = strutil::to_lower(set) + ".wav";
	single.value_hi = 0xD200;
	bank.singles.push_back(single);
	lwf::Multi multi;
	multi.name = set;
	multi.pitch_base = lwf::kAuthoredSetPitchBase;
	multi.target_id = 100;
	multi.playlist_indices.push_back(0);
	bank.multis.push_back(multi);
	lwf::Playlist layer;
	layer.falloff_radius = 100;
	layer.flags = lwf::kFlagInternal | lwf::kFlagExternal;
	layer.sndparm_indices.push_back(0);
	bank.playlists.push_back(layer);
	lwf::Sndparm member;
	member.single_index = 0;
	member.pitch_scaled = lwf::kPitchUnityQ16;
	member.volume = 200;
	member.clamp_volume = 255;
	bank.sndparms.push_back(member);
	std::vector<uint8_t> out;
	std::string error;
	lwf::encode_lwf(bank, out, error);
	return out;
}

// The soldier's clips as the Blender add-on writes them (an .o3a): SKIN.adm's reset, idle and walk rows over the
// skinned fixture's three bones.
std::string soldier_clips() {
	std::string out = "o3a 1\nadm SKIN.adm\nrow anim_reset \"reset\"\nrow anim_idle \"walk\"\nrow anim_walk_forward \"walk\"\n";
	const char *bones[] = {"bone -1 0 0 0 0.5 \"BN01 Pelvis\"", "bone 0 0 0 1 0.5 \"BN02 Spine\"",
	                       "bone 0 0 0 -1 0.5 \"BN03 Leg\""};
	out += "clip reset\nfps 30\nflags 0x1\nframes 1\n";
	for (const char *bone : bones) out += std::string(bone) + "\n k 0 0 0 1\n k 0 0 0 1\n";
	out += "event 0 0 0 0x0 0.9 1.7\nevent 0 0 0 0x0 0.9 1.7\n";
	out += "clip walk\nfps 30\nflags 0x1\nframes 4\n";
	for (size_t b = 0; b < 3; ++b) {
		out += std::string(bones[b]) + "\n";
		for (int k = 0; k < 5; ++k) out += b == 0 && (k == 1 || k == 3) ? " k 0 0 0.3826834 0.9238795\n" : " k 0 0 0 1\n";
	}
	for (int k = 0; k < 5; ++k) out += "event 0 0 0 0x0 0.9 1.7\n";
	return out;
}

// The items: the pump (gnrc: its husk four ticks on), a drivable one (its slot waits for a driver), one seen by
// its enemies as the wreck, a soldier, the round's item, a statue of no model.
const char *kItems =
		"begin \"Smoke pump\"\nid 100700\ntype object\ngraphic fxpump\nai_function gnrc\nhusk fxwreck\n"
		"husk_sub_part_types 01_HULL 02_CHUNK_M 03_CHUNK_S\ndestroy_timing 0.5 1.0 0.25\n"
		"particlefx Effect_Smoke Smoke\nparticledeath Effect_Dead\nparticlefire Effect_Fire\nparticleother Effect_Other\n"
		"sounddeath EXPLO_PUMP\nend\n"
		"begin \"Smoke car\"\nid 100701\ntype vehicle\ngraphic fxpump\nattrib: playercontrol\nparticlefx Effect_Smoke nowhere\nend\n"
		"begin \"Two faced\"\nid 100702\ntype object\ngraphic fxpump\ngraphicenemy fxwreck\nend\n"
		"begin \"Soldier\"\nid 100703\ntype person\ngraphic skinned\nanim_def SKIN\nai_function org1\nattrib: aidata\nend\n"
		"begin \"Round\"\nid 100704\ntype object\ngraphic fxwreck\nend\n"
		"begin \"Statue\"\nid 100705\ntype decoration\nend\n";

const char *kWeapons = "weapon \"W_FX\"\r\n\tgfx3 fxpump\r\nend\r\n";
const char *kAmmo = "ammo AMMO_FX\r\nvelocity 900\r\nfrndlytrcrid 704\r\nend\r\nammo AMMO_PLAIN\r\nvelocity 800\r\nend\r\n";

struct DefinitionRig {
	editor_test::TempProjectDir dir{"opennova_editor_definition_viewport"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	std::string root;
	std::string path;
	bool made = false;

	DefinitionRig() {
		session.handle(request::new_project(dir.file("project"), "Definitions"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		// The soldier's rig and clips, imported as the add-on writes them.
		const std::string source = dir.file("source");
		bool ok = editor_test::write_bytes(source + "/skinned.o3d",
		                                   test_io::read_file(repo() + "/fixtures/threedi/o3d/skinned.o3d")) &&
		          editor_test::write_text(source + "/skin.o3a", soldier_clips());
		EditorRequest import = request::of(EditorRequestKind::ImportFiles);
		import.imports = {{source + "/skinned.o3d", {}}, {source + "/skin.o3a", {}}};
		session.handle(import);
		session.run_operations();
		ok = ok && editor_test::write_bytes(root + "/models/fxpump.3di", pump_bytes()) &&
		     editor_test::write_bytes(root + "/models/fxwreck.3di", wreck_bytes()) &&
		     editor_test::write_text(root + "/particles/fx.ptl", particles()) &&
		     editor_test::write_bytes(root + "/sounds/game.lwf", bank_of("EXPLO_PUMP")) &&
		     editor_test::write_bytes(root + "/sounds/explo_pump.wav", test_io::read_file(repo() + "/fixtures/lwf/tone.wav")) &&
		     editor_test::write_text(root + "/defs/items.def", editor_test::crlf(kItems)) &&
		     editor_test::write_text(root + "/defs/weapon.def", kWeapons) &&
		     editor_test::write_text(root + "/defs/ammo.def", kAmmo);
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		made = ok;
	}
	// The record `name` of the table at `file` selected (the table opened first): the Preview follows it.
	bool select(const std::string &file, const std::string &name) {
		session.handle(request::open_document(file));
		const DocumentBase *opened = session.document_for(file);
		const Document *table = opened ? records_of(*opened) : nullptr;
		if (!table) return false;
		path = table->path();
		for (const auto &row : table->rows())
			if (row && strutil::iequals(row->name(), name)) {
				session.handle(request::select_record(path, NodeAddress{row->id, 0, 0}));
				devices.sync(session);
				return session.view().documents.previews[ViewportKind::Definition].path == path &&
				       session.view().documents.previews[ViewportKind::Definition].part == row->id;
			}
		return false;
	}
	const DefinitionViewport *viewport() {
		return dynamic_cast<const DefinitionViewport *>(
				session.viewports().follow_one(session.view(), path, ViewportKind::Definition));
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		devices.sync(session);
		return session.outcome().done();
	}
	std::string refusal(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		return session.outcome().done() || session.outcome().findings.empty() ? std::string()
		                                                                       : session.outcome().findings.front().message;
	}
	bool state(const char *token) { return set(std::string(R"({"options": {"state": ")") + token + "\"}}"); }
	bool hold(int32_t ticks) { return set(R"({"clock": {"ticks": )" + std::to_string(ticks) + R"(, "playing": false}})"); }
	// From tick 0, the clock run to `ticks`, `per_call` ticks a frame: the sounds the viewport fired on the way.
	std::vector<ClipSoundFired> run(int32_t ticks, double per_call = 0.5) {
		set(R"({"clock": {"ticks": 0, "playing": true, "rate": 1}})");
		const uint64_t before = session.viewports().clip_sound_seq();
		while (session.viewports().clock().ticks() < ticks) {
			session.advance(per_call / 62.5);
			devices.sync(session);
		}
		std::vector<ClipSoundFired> out;
		if (const DefinitionViewport *model = viewport())
			for (const ClipSoundFired &fired : model->sounds_fired())
				if (fired.seq > before) out.push_back(fired);
		return out;
	}
	JsonValue json() {
		const DefinitionViewport *model = viewport();
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	ViewportAction last() { return devices.last(path, ViewportKind::Definition); }
};

// The spawns of `source`.
std::vector<DefinitionSpawn> spawns_of(const DefinitionViewport &viewport, const std::string &source) {
	std::vector<DefinitionSpawn> out;
	for (const DefinitionSpawn &spawn : viewport.effects().spawns())
		if (spawn.source == source) out.push_back(spawn);
	return out;
}

bool near(float a, float b) {
	return std::fabs(a - b) < 1e-4f;
}

// A model's user point `index` in the preview's space, with its direction.
void point_of(const threedi::Threedi3di3 &model, size_t index, PreviewVec3 &at, PreviewVec3 &direction) {
	float position[3], axis[3];
	threedi::threedi_user_point_position(&model.user_points[index], position);
	threedi::threedi_user_point_direction(&model.user_points[index], axis);
	at = preview_from_model(position);
	direction = preview_from_model(axis);
}

} // namespace

// The kinds' table: the definition's kind, the catalog's Preview, its row the selection's and no page.
static int test_kinds() {
	ViewportKind named = ViewportKind::kCount;
	TEST_EXPECT(std::string(viewport_kind_token(ViewportKind::Definition)) == "definition");
	TEST_EXPECT(viewport_kind_from_token("definition", named) && named == ViewportKind::Definition);
	const ViewportKindRow &row = viewport_kind_row(ViewportKind::Definition);
	TEST_EXPECT(row.role == ViewportRole::Preview && row.part && !row.pages && !row.as_saved && row.canvas);
	TEST_EXPECT(preview_kind_of(DocumentTypeId::Catalog) == ViewportKind::Definition &&
	            viewport_kind_shows(ViewportKind::Definition, DocumentTypeId::Catalog) &&
	            default_viewport_kind(DocumentTypeId::Catalog) == ViewportKind::Definition);
	// A menu's screen is a page of it; a definition's record is not.
	TEST_EXPECT(viewport_kind_row(ViewportKind::Menu).part && viewport_kind_row(ViewportKind::Menu).pages);
	std::unique_ptr<ViewportModel> made = row.make("defs/items.def");
	TEST_EXPECT(made && std::string(made->reason()) == "no_project" && made->status() == ViewportStatus::Empty &&
	            made->make_canvas() != nullptr && made->options_json().get("state") &&
	            made->options_json().get_string("state", "") == "alive");
	std::printf("test_kinds passed\n");
	return 0;
}

// The death banks as the engine's death tail spawns them, lifted for the preview: each family at its points in
// the masked order, the Dead bank once at the origin where the piece model names no Dead point, the water death's
// effect under water, nothing without a husk model.
static int test_death_banks() {
	const std::vector<uint8_t> bytes = wreck_bytes();
	const assets::Model wreck = assets::parse_model(bytes.data(), bytes.size());
	TEST_EXPECT(wreck != nullptr);
	world::ItemDeathTraits traits;
	traits.husk_model_loaded = true;
	traits.particledeath = "Effect_Dead";
	traits.particleh2odeath = "Effect_Wet";
	traits.particlefire = "Effect_Fire";
	traits.particleother = "Effect_Other";
	world::death_effect_banks_of(*wreck, traits.effect_banks);
	TEST_EXPECT(traits.effect_banks[0].points.size() == 1 && traits.effect_banks[1].points.size() == 2 &&
	            traits.effect_banks[2].points.size() == 1);
	std::vector<world::DeathBankSpawn> spawns = world::death_bank_spawns(traits, false);
	TEST_EXPECT(spawns.size() == 4 && spawns[0].family == 1 && spawns[0].effect == "Effect_Dead" && spawns[0].section_tagged);
	TEST_EXPECT(spawns[1].family == 2 && spawns[1].slot == 0 && spawns[2].family == 2 && spawns[2].slot == 1 &&
	            spawns[3].family == 3);
	// Its point is the model's, in mission-local axes.
	float position[3];
	threedi::threedi_user_point_position(&wreck->user_points[0], position);
	TEST_EXPECT(near(spawns[0].point.local_pos.x, position[2]) && near(spawns[0].point.local_pos.y, -position[0]) &&
	            near(spawns[0].point.local_pos.z, position[1]));
	// Under water, the water death's effect at the Dead points.
	TEST_EXPECT(world::death_bank_spawns(traits, true).front().effect == "Effect_Wet");
	// No Dead point: once at the origin, untagged.
	traits.effect_banks[0] = world::DeathEffectBank();
	spawns = world::death_bank_spawns(traits, false);
	TEST_EXPECT(spawns.front().family == 1 && !spawns.front().section_tagged && spawns.front().point.local_pos.x == 0.0f);
	// No husk model loaded: nothing.
	traits.husk_model_loaded = false;
	TEST_EXPECT(world::death_bank_spawns(traits, false).empty());
	std::printf("test_death_banks passed\n");
	return 0;
}

// The effects' playback over a catalog: one scene over two effects' closures, each spawn on its tick, a jump
// making every spawn due again pre-aged.
static int test_effects_playback() {
	DefinitionRig rig;
	TEST_EXPECT(rig.made);
	PreviewEffectCatalog catalog;
	catalog.follow(rig.session.view().project.scan, rig.session.view().findings.assets);
	std::vector<particle::EffectClosure> each;
	const particle::EffectSceneConfig config =
			catalog.closures({"Effect_Smoke", "Effect_Fire", "Effect_Smoke"}, particle::EffectSceneConfig(), each);
	TEST_EXPECT(each.size() == 3 && each[0].spawns() && each[1].spawns() && config.documents.size() == 1 &&
	            config.documents[0].file.effects.size() == 2 && config.documents[0].file.particles.size() == 2);
	DefinitionEffects effects;
	std::vector<DefinitionSpawn> spawns = {
		{"Effect_Smoke", particle::forward_pose({0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}), 0, "particle_slot", "Smoke"},
		{"Effect_Fire", particle::descriptor_pose({0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}), 10, "fire", "Fire 1"},
	};
	TEST_EXPECT(effects.plan(catalog, catalog.serial(), spawns) && effects.scene() && effects.opens() == 1);
	// The same spawns over the same catalog: kept.
	TEST_EXPECT(!effects.plan(catalog, catalog.serial(), spawns) && effects.opens() == 1);
	effects.play_to(0);
	TEST_EXPECT(effects.status(0) == particle::EffectSpawnStatus::Spawned && effects.alive(0) &&
	            effects.status(1) == particle::EffectSpawnStatus::InvalidHandle);
	for (int32_t tick = 1; tick <= 12; ++tick) effects.play_to(tick);
	TEST_EXPECT(effects.status(1) == particle::EffectSpawnStatus::Spawned && effects.alive(1));
	const size_t stepped = effects.scene()->live_counts().particle_count;
	// A jump back: the scene emptied, each spawn due made again at its age.
	effects.play_to(5);
	TEST_EXPECT(effects.alive(0) && !effects.alive(1) && effects.status(1) == particle::EffectSpawnStatus::InvalidHandle);
	effects.play_to(12 + 300);
	TEST_EXPECT(effects.alive(0) && effects.alive(1) && stepped > 0);
	// The forward pose: forward along the point's direction, the basis right-handed as the device's.
	const particle::EffectPose &pose = spawns[0].pose;
	TEST_EXPECT(near(pose.forward.x, 1.0f) && near(pose.right.z, -1.0f) && near(pose.up.y, 1.0f));
	// The descriptor pose of no orientation emits around +Y, as an effect played alone.
	TEST_EXPECT(near(spawns[1].pose.forward.y, 1.0f) && near(effect_play_pose().forward.y, 1.0f));
	std::printf("test_effects_playback passed\n");
	return 0;
}

// An item alive: its graphic, its particle slot at the matching points along their directions; a drivable item's
// slot only while occupied; the enemy's model.
static int test_item_alive() {
	DefinitionRig rig;
	TEST_EXPECT(rig.made);
	TEST_EXPECT(rig.select("defs/items.def", "Smoke pump"));
	const DefinitionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->view_status() == DefinitionViewStatus::Ready);
	TEST_EXPECT(viewport->drawn().kind == "item" && viewport->drawn().field == "graphic" &&
	            viewport->drawn().name == "fxpump" && viewport->drawn().file == "models/fxpump.3di");
	TEST_EXPECT(rig.last() == ViewportAction::Rebuild);
	// The slot: both points named Smoke (any case) among the first 16, each at its place along its direction.
	const DefinitionParticleSlot &slot = viewport->particle_slot();
	TEST_EXPECT(slot.admitted && slot.user_points.size() == 2);
	const std::vector<DefinitionSpawn> smoke = spawns_of(*viewport, "particle_slot");
	TEST_EXPECT(smoke.size() == 2 && smoke[0].effect == "Effect_Smoke" && smoke[0].tick == 0 && smoke[0].point == "Smoke");
	PreviewVec3 at, direction;
	point_of(*viewport->model(), size_t(slot.user_points[0]), at, direction);
	TEST_EXPECT(near(smoke[0].pose.position.x, at.x) && near(smoke[0].pose.position.y, at.y) &&
	            near(smoke[0].pose.position.z, at.z));
	const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
	TEST_EXPECT(near(smoke[0].pose.forward.x, direction.x / length) && near(smoke[0].pose.forward.z, direction.z / length));
	// Played on the clock: the scene the device draws holds the smoke.
	rig.run(40);
	TEST_EXPECT(viewport->effects().scene() && viewport->effects().alive(0) &&
	            viewport->effects().scene()->live_counts().particle_count > 0);
	// Alive draws no death: no registers, no hidden section.
	TEST_EXPECT(viewport->ctrl_at(rig.session.viewports().clock()).empty() &&
	            viewport->hidden_sections_at(rig.session.viewports().clock()) == 0);
	// Its shadows, which fall only on a mission's terrain the picture has none of, said beside it: the sun
	// shadow among the buildings; NoShadow set on the record (an edit the picture follows), none.
	const auto said = [&](const char *words) {
		for (const std::string &note : rig.viewport()->notes())
			if (note.find(words) != std::string::npos) return true;
		return false;
	};
	TEST_EXPECT(said("sun shadow on a mission's terrain where a mission places it among the buildings") &&
	            !said("moving shadow") && !said("is NoShadow"));
	{
		Edit no_shadow;
		no_shadow.address = NodeAddress{rig.session.view().documents.previews[ViewportKind::Definition].part, 0, 0};
		no_shadow.field = "attrib";
		no_shadow.value = int64_t(def::DEF_ITEM_ATTRIB_NOSHADOW);
		rig.session.handle(request::edit_record(rig.path, no_shadow));
		rig.devices.sync(rig.session);
		TEST_EXPECT(rig.session.outcome().done() && said("Smoke pump is NoShadow: the game draws no sun shadow"));
		rig.session.handle(request::undo(rig.path));
		rig.devices.sync(rig.session);
		TEST_EXPECT(!said("is NoShadow") && said("among the buildings"));
	}
	// A drivable item: its slot (no point of the name: once at the origin) only while a driver controls it.
	TEST_EXPECT(rig.select("defs/items.def", "Smoke car"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport && !viewport->particle_slot().admitted && viewport->particle_slot().controller &&
	            viewport->effects().spawns().empty());
	TEST_EXPECT(rig.set(R"({"options": {"occupied": true}})"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->particle_slot().admitted && viewport->effects().spawns().size() == 1 &&
	            viewport->effects().spawns()[0].point.empty() && viewport->effects().spawns()[0].pose.position.y == 0.0f);
	// The enemy's model; an item authoring none keeps its graphic and says so.
	TEST_EXPECT(rig.set(R"({"options": {"occupied": false, "enemy": true}})"));
	TEST_EXPECT(rig.select("defs/items.def", "Two faced"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->drawn().field == "graphic_enemy" && viewport->drawn().name == "fxwreck");
	TEST_EXPECT(rig.select("defs/items.def", "Smoke pump"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->drawn().field == "graphic" && !viewport->notes().empty());
	std::printf("test_item_alive passed\n");
	return 0;
}

// Its death, its husk and its final husk.
static int test_item_death() {
	DefinitionRig rig;
	TEST_EXPECT(rig.made);
	TEST_EXPECT(rig.select("defs/items.def", "Smoke pump"));
	TEST_EXPECT(rig.state("destroying"));
	const DefinitionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->plan().swaps && viewport->plan().swap_tick == 4);
	// Before the swap (gnrc: four ticks on), the graphic; from it the husk, its pieces' sections gone.
	TEST_EXPECT(rig.hold(2));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->drawn().field == "graphic");
	TEST_EXPECT(rig.hold(4));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->drawn().field == "husk" && viewport->drawn().name == "fxwreck" &&
	            viewport->drawn().file == "models/fxwreck.3di" && rig.last() == ViewportAction::Rebuild);
	const PreviewClock &clock = rig.session.viewports().clock();
	TEST_EXPECT(viewport->hidden_sections_at(clock) == viewport->plan().hidden_sections && viewport->plan().hidden_sections != 0);
	// The fade's registers once its delay ran (half a second).
	TEST_EXPECT(viewport->ctrl_at(clock).empty());
	TEST_EXPECT(rig.hold(80));
	viewport = rig.viewport();
	const std::map<std::string, int64_t> registers = viewport->ctrl_at(rig.session.viewports().clock());
	TEST_EXPECT(registers.count(threedi::threedi_ctrl_register_name(size_t(threedi::THREEDI_CTRL_OBJECT_DESTROY))) == 1);
	// The death's banks at the piece model's points on the swap's tick, beside the item's slot.
	const std::vector<DefinitionSpawn> dead = spawns_of(*viewport, "dead");
	const std::vector<DefinitionSpawn> fire = spawns_of(*viewport, "fire");
	const std::vector<DefinitionSpawn> other = spawns_of(*viewport, "other");
	TEST_EXPECT(dead.size() == 1 && fire.size() == 2 && other.size() == 1 && spawns_of(*viewport, "particle_slot").size() == 2);
	TEST_EXPECT(dead[0].tick == 4 && dead[0].effect == "Effect_Dead" && dead[0].point == "Dead 1" &&
	            fire[1].point == "Fire 2" && other[0].effect == "Effect_Other");
	// Each at its point of the wreck, in the preview's space.
	const std::vector<uint8_t> bytes = wreck_bytes();
	const assets::Model wreck = assets::parse_model(bytes.data(), bytes.size());
	PreviewVec3 at, direction;
	point_of(*wreck, 0, at, direction);
	TEST_EXPECT(near(dead[0].pose.position.x, at.x) && near(dead[0].pose.position.y, at.y) &&
	            near(dead[0].pose.position.z, at.z));
	// The death sound heard as the clock runs from the death; nothing over a seek.
	const std::vector<ClipSoundFired> heard = rig.run(10);
	TEST_EXPECT(heard.size() == 1 && heard[0].set == "EXPLO_PUMP" && heard[0].tick == 4 && heard[0].state == "played" &&
	            heard[0].path == rig.path);
	// The Play's first frame running six ticks at once: heard all the same, from where its seek put the clock.
	const std::vector<ClipSoundFired> slow = rig.run(10, 6.0);
	TEST_EXPECT(slow.size() == 1 && slow[0].set == "EXPLO_PUMP" && slow[0].tick == 4);
	const size_t fired = viewport->sounds_fired().size();
	TEST_EXPECT(rig.hold(2) && rig.set(R"({"clock": {"ticks": 8, "playing": false}})"));
	rig.session.advance(0.1);
	TEST_EXPECT(rig.viewport()->sounds_fired().size() == fired);
	// Muted, it fires and says so, and nothing is played.
	TEST_EXPECT(rig.set(R"({"options": {"mute": true}})"));
	const std::vector<ClipSoundFired> muted = rig.run(10);
	TEST_EXPECT(muted.size() == 1 && muted[0].state == "muted");
	// The husk standing: the wreck, its sections gone, the fade done, its Fire and Other banks burning.
	TEST_EXPECT(rig.state("husk"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->drawn().field == "husk" && spawns_of(*viewport, "dead").empty() &&
	            spawns_of(*viewport, "fire").size() == 2 && spawns_of(*viewport, "other").size() == 1 &&
	            spawns_of(*viewport, "fire")[0].tick == 0);
	TEST_EXPECT(viewport->hidden_sections_at(rig.session.viewports().clock()) == viewport->plan().hidden_sections);
	// The final husk: its pieces' model (here the husk, which authors no huskfinal), whole.
	TEST_EXPECT(rig.state("husk_final"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->drawn().field == "husk" && viewport->drawn().name == "fxwreck" &&
	            viewport->hidden_sections_at(rig.session.viewports().clock()) == 0 && viewport->effects().spawns().empty());
	// A record of no model, and a class that never dies, say so.
	TEST_EXPECT(rig.select("defs/items.def", "Statue"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->view_status() == DefinitionViewStatus::NoModel && !viewport->message().empty());
	TEST_EXPECT(rig.state("destroying") && rig.select("defs/items.def", "Two faced"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->view_status() == DefinitionViewStatus::Ready && !viewport->plan().swaps &&
	            viewport->drawn().field.rfind("graphic", 0) == 0 && !viewport->notes().empty());
	std::printf("test_item_death passed\n");
	return 0;
}

// A person posed as its spawn poses it on its graphic's rig.
static int test_person() {
	DefinitionRig rig;
	TEST_EXPECT(rig.made);
	TEST_EXPECT(rig.select("defs/items.def", "Soldier"));
	const DefinitionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->view_status() == DefinitionViewStatus::Ready && viewport->drawn().name == "skinned");
	const MissionPose &person = viewport->person();
	TEST_EXPECT(person.status == "posed" && strutil::iequals(person.adm, "SKIN.adm") &&
	            person.state == world::anim_state::kIdle && person.updates > 10 && viewport->skeleton() != nullptr);
	const uint64_t serial = viewport->skeleton_serial();
	const uint32_t updates = person.updates;
	// Another SSN warms up longer: posed again over the same rig (not loaded again), the picture standing.
	const uint64_t builds = viewport->builds();
	TEST_EXPECT(rig.set(R"({"options": {"ssn": 15}})"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->person().status == "posed" && viewport->person().updates != updates &&
	            viewport->skeleton_serial() == serial && viewport->builds() == builds &&
	            rig.last() == ViewportAction::Update);
	// The envelope's person, as the mission's.
	JsonValue shown = rig.json();
	const JsonValue *body = shown.get("body");
	TEST_EXPECT(body && body->get("person") && body->get("person")->get_string("status", "") == "posed");
	std::printf("test_person passed\n");
	return 0;
}

// A weapon's models, an ammo's round, a powerup, and the refusals.
static int test_weapon_and_ammo() {
	DefinitionRig rig;
	TEST_EXPECT(rig.made);
	TEST_EXPECT(rig.select("defs/weapon.def", "W_FX"));
	const DefinitionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->view_status() == DefinitionViewStatus::Ready && viewport->drawn().kind == "weapon" &&
	            viewport->drawn().field == "gfx3" && viewport->drawn().name == "fxpump");
	TEST_EXPECT(rig.set(R"({"options": {"weapon": "first"}})"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->view_status() == DefinitionViewStatus::NoModel && viewport->drawn().field == "gfx1" &&
	            viewport->message().find("gfx1") != std::string::npos);
	// An ammo's round: the item its tracer id names, through the project's graph.
	TEST_EXPECT(rig.select("defs/ammo.def", "AMMO_FX"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->view_status() == DefinitionViewStatus::Ready && viewport->drawn().kind == "ammo" &&
	            viewport->drawn().field == "frndly_trcr_type_id" && viewport->drawn().via == "Round" &&
	            viewport->drawn().name == "fxwreck" && viewport->drawn().via_file == "defs/items.def");
	// Seen by the other side, the friendly item where it names no enemy one.
	TEST_EXPECT(rig.set(R"({"options": {"enemy": true}})"));
	viewport = rig.viewport();
	TEST_EXPECT(viewport->drawn().field == "frndly_trcr_type_id" && !viewport->notes().empty());
	// An ammo with no tracer item stands without a round model (DI-23): its impact rows and its range are its picture,
	// and the note says why nothing of the round draws.
	TEST_EXPECT(rig.select("defs/ammo.def", "AMMO_PLAIN"));
	viewport = rig.viewport();
	bool no_tracer = false;
	for (const std::string &note : viewport->notes()) no_tracer = no_tracer || note.find("names no tracer item") != std::string::npos;
	TEST_EXPECT(viewport->view_status() == DefinitionViewStatus::Ready && !viewport->model() && no_tracer &&
	            viewport->ammo_record() && viewport->impacts().found);
	// The refusals.
	TEST_EXPECT(rig.refusal(R"({"options": {"state": "burning"}})").find("options.state") != std::string::npos);
	TEST_EXPECT(rig.refusal(R"({"options": {"weapon": "side"}})").find("options.weapon") != std::string::npos);
	TEST_EXPECT(rig.refusal(R"({"options": {"ssn": -1}})").find("options.ssn") != std::string::npos);
	TEST_EXPECT(rig.refusal(R"({"options": {"colour": 1}})").find("Unknown definition option") != std::string::npos);
	std::printf("test_weapon_and_ammo passed\n");
	return 0;
}

// The envelope, the canvas and the commands.
static int test_wire_and_canvas() {
	DefinitionRig rig;
	TEST_EXPECT(rig.made);
	TEST_EXPECT(rig.select("defs/items.def", "Smoke pump"));
	rig.run(20);
	JsonValue shown = rig.json();
	TEST_EXPECT(shown.get_string("kind", "") == "definition" && shown.get_string("status", "") == "ready");
	const JsonValue *body = shown.get("body");
	TEST_EXPECT(body && body->get("record") && body->get("record")->get_string("name", "") == "Smoke pump" &&
	            body->get("draws") && body->get("draws")->get_string("file", "") == "models/fxpump.3di");
	const JsonValue *effects = body->get("effects");
	TEST_EXPECT(effects && effects->is_array() && effects->array.size() == 2 &&
	            effects->array[0].get_string("status", "") == "spawned" && effects->array[0].get_bool("spawns", false) &&
	            effects->array[0].get_string("defined_in", "") == "particles/fx.ptl");
	TEST_EXPECT(body->get("particle_slot") && body->get("particle_slot")->get_bool("admitted", false));
	TEST_EXPECT(body->get("death") && body->get("death")->get("legs") && !body->get("death")->get("legs")->array.empty());
	const JsonValue *items = shown.get("items");
	TEST_EXPECT(items && items->is_array() && items->array.size() == 2 &&
	            items->array[0].get_string("kind", "") == "particle_slot");
	// The canvas: a drag orbits, one SetViewport of its camera.
	const DefinitionViewport *viewport = rig.viewport();
	const ViewportContext context = viewport_context(rig.session.view(), *viewport);
	const std::unique_ptr<CanvasHalf> canvas = viewport->make_canvas();
	editor_test::Gathered out;
	canvas->follow(*viewport, context, out);
	CanvasInput in;
	in.width = 800;
	in.height = 600;
	in.hovered = true;
	in.pressed = in.down = true;
	in.mouse = in.screen = CanvasPoint{100.0f, 100.0f};
	canvas->input(context, in, out);
	in.pressed = false;
	in.screen = in.mouse = CanvasPoint{160.0f, 120.0f};
	in.delta = CanvasPoint{60.0f, 20.0f};
	canvas->input(context, in, out);
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::SetViewport);
	const float yaw = viewport->camera().yaw;
	TEST_EXPECT(editor_test::serve(rig.session, out.requests) && rig.viewport()->camera().yaw != yaw);
	// The commands: frame (the camera on the model), replay (tick 0, run); another refused.
	editor_test::Gathered framed;
	std::string error;
	TEST_EXPECT(viewport->command(context, "frame", {}, framed, error) && framed.requests.size() == 1);
	editor_test::Gathered replayed;
	TEST_EXPECT(viewport->command(context, "replay", {}, replayed, error) &&
	            editor_test::serve(rig.session, replayed.requests) && rig.session.viewports().clock().ticks() == 0);
	TEST_EXPECT(!viewport->command(context, "shoot", {}, framed, error) && error.find("frame, replay") != std::string::npos);
	TEST_EXPECT(!viewport->drag(context, ViewportDrag(), framed, error));
	std::printf("test_wire_and_canvas passed\n");
	return 0;
}

int main() {
	TEST_EXPECT(test_kinds() == 0);
	TEST_EXPECT(test_death_banks() == 0);
	TEST_EXPECT(test_effects_playback() == 0);
	TEST_EXPECT(test_item_alive() == 0);
	TEST_EXPECT(test_item_death() == 0);
	TEST_EXPECT(test_person() == 0);
	TEST_EXPECT(test_weapon_and_ammo() == 0);
	TEST_EXPECT(test_wire_and_canvas() == 0);
	std::printf("editor_definition_viewport OK\n");
	return 0;
}
