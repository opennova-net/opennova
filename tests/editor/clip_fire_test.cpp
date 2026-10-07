// DI-24 (ADR 0046, the deep-integration plan's "clip fire events fire the item's ammo"): as the model preview's clock
// runs a clip on an NPC's body, each tick the body's fire block reads a fire bit fires what the game fires there, the
// item's ammo from its launch point as the clip poses it, through the game's NPC fire entry in DI-22's weapon range
// (preview/preview_clip_fire, preview/weapon_range's soldier shots). Headless (the session's advance follows the
// viewport), over minted fixtures alone: the skinned fixture with a launch point on its middle part, clips made here
// as the Blender add-on writes them (an .o3a), a bank minted through lwf::encode_lwf, SndProf.def, items.def,
// ammo.def and particle text.
//
// Pinned: the fire block's order on the NPC body's odd ticks (0x4 the closeattack ammo from its point, 0x8 the
// easyrocket and a different advancedrocket from the rocket point, 0x10 the marker3 ammo from the body's origin where
// no point is named); each shot from the launch point as the clip poses it there; the round's launch (the ammo's
// ai_launch heard, its ai_launcheffect at the point), its flight, its tracer and its stop on the target (the row's
// effect, sound and scar); the sounds as the clock runs, muted; the player's body, an item of no person class, an item
// naming no ammo and an easyrocket the advancedrocket repeats; a timeline mark pressed (play_sound {frame}) firing its
// shot once, and its refusal; the run again to the same events when the clock steps back; the options' wire and
// refusals; the envelope's fire and each event's fires; the shots against the game's own org1 fire pass run on the same
// word, bytes and launch point (the same ammo, order, place, angles and launch sound).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_clip_fire.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/lwf.h>
#include <runtime/anim/anim_event_bits.h>
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }

// A clip of the skinned fixture's three bones, `frames` frames at 30 a second, its event words by frame; `turn` turns
// the middle bone an eighth from frame 1 on, so a point on it moves with the clip.
std::string clip_text(const std::string &name, int frames, bool loop, bool turn, const std::map<int, uint32_t> &events) {
	std::string out = "clip " + name + "\nfps 30\nflags " + (loop ? "0x1" : "0x0") + "\nframes " + std::to_string(frames) + "\n";
	const char *bones[] = {"bone -1 0 0 0 0.5 \"BN01 Pelvis\"", "bone 0 0 0 1 0.5 \"BN02 Spine\"",
	                       "bone 0 0 0 -1 0.5 \"BN03 Leg\""};
	for (size_t b = 0; b < 3; ++b) {
		out += std::string(bones[b]) + "\n";
		for (int i = 0; i <= frames; ++i) out += turn && b == 1 && i >= 1 ? " k 0 0 0.3826834 0.9238795\n" : " k 0 0 0 1\n";
	}
	for (int i = 0; i <= frames; ++i) {
		const auto word = events.find(i < frames ? i : frames - 1);
		char line[64];
		std::snprintf(line, sizeof(line), "event 0 0 0 0x%X 0.9 1.7\n", word == events.end() ? 0u : word->second);
		out += line;
	}
	return out;
}

// SOLD.adm: its reset and a fire loop of 8 frames: the first ammo on frame 2 (ticks 5 and 6), the second on frame 4
// (9 and 10), the fourth with a left foot on frame 6 (13 and 14).
std::string sold_clips() {
	return "o3a 1\nadm SOLD.adm\nrow anim_reset \"rest\"\nrow anim_attack \"fire\"\n" + clip_text("rest", 1, true, false, {}) +
	       clip_text("fire", 8, true, true,
	                 {{2, anim::kAnimEventFirePrimary}, {4, anim::kAnimEventFireSecondary},
	                  {6, anim::kAnimEventFireMarker3 | anim::kAnimEventFootLeft}});
}

// The soldier: the skinned fixture with its muzzle point on the middle part.
std::string soldier_model() {
	std::string text = test_io::read_file_text(repo() + "/fixtures/threedi/o3d/skinned.o3d");
	text += "userpoint \"MFlash01\" 0 1 0.5 0 1 0 1 83\n";
	return text;
}

std::vector<uint8_t> bank_of(const std::vector<std::string> &sets) {
	lwf::File bank;
	for (size_t i = 0; i < sets.size(); ++i) {
		lwf::Single single;
		single.name = sets[i];
		single.path = strutil::to_lower(sets[i]) + ".wav";
		single.value_hi = 0xD200;
		bank.singles.push_back(single);
		lwf::Multi set;
		set.name = sets[i];
		set.pitch_base = lwf::kAuthoredSetPitchBase;
		set.target_id = 100;
		set.playlist_indices.push_back(uint32_t(i));
		bank.multis.push_back(set);
		lwf::Playlist layer;
		layer.falloff_radius = 100;
		layer.flags = lwf::kFlagInternal | lwf::kFlagExternal;
		layer.sndparm_indices.push_back(uint32_t(i));
		bank.playlists.push_back(layer);
		lwf::Sndparm member;
		member.single_index = uint32_t(i);
		member.pitch_scaled = lwf::kPitchUnityQ16;
		member.volume = 200;
		member.clamp_volume = 255;
		bank.sndparms.push_back(member);
	}
	std::vector<uint8_t> out;
	std::string error;
	lwf::encode_lwf(bank, out, error);
	return out;
}

const char *kSets[] = {"FS_GND_L", "GS_AI", "GS_ROCKET", "IMP_DIRT", "IMP_METAL"};

// The rounds: the rifle's (600 m/s, every one a tracer, a ring scar, dirt and metal rows, a soldier's launch) and two
// rockets'; AT_NULL the table's null first row.
const char *kAmmo = "ammo AT_NULL\nend\n"
                    "ammo AMMO_TEST\n\tvelocity 600\n\tmax_age 3\n\tweight_in_grains 62\n\ttracerrate 1\n\ttracer_type 1 2\n"
                    "\tscar_type 1\n\teffects_table\n\t\tobj Hit IMP_DIRT 15\n\t\tdirt Hit IMP_DIRT 15\n"
                    "\t\tmetal Spark IMP_METAL 15\n\tend\n\tai_launch GS_AI\n\tai_launcheffect Flash\nend\n"
                    "ammo AMMO_ROCKET\n\tvelocity 120\n\tmax_age 4\n\tweight_in_grains 62\n"
                    "\tai_launch GS_ROCKET\n\tai_launcheffect Smoke\nend\n"
                    "ammo AMMO_ROCKET2\n\tvelocity 120\n\tmax_age 4\n\tweight_in_grains 62\n"
                    "\tai_launch GS_ROCKET\n\tai_launcheffect Smoke\nend\n";

std::string particle_text(const std::string &id) {
	return "[particledef]\n{\n\tid = " + id + ";\n\temit_dur = 0.2;\n\temit_rate = 30;\n\temit_burst = 1;\n\tage = 0.5;\n"
	       "\tscale = 0.5;\n\tspeed = 2.0;\n\tspread = 30.0;\n}\n\n";
}

std::string particles() {
	std::string out;
	for (const char *id : {"Flash", "Smoke", "Hit", "Spark"})
		out += particle_text(std::string("p_") + id) + "[effectdef]\n{\n\tid = " + id + ";\n\tpdefs = p_" + id + ";\n}\n\n";
	return out;
}

// The soldier's item: an org1 person on the SOLD map, its ammo and points as `extra` adds to them.
std::string items(const std::string &ai_function, const std::string &extra) {
	return editor_test::crlf("begin \"Soldier\"\nid 100300\ntype person\ngraphic skinned\nanim_def sold\n"
	                         "ai_function " + ai_function + "\nmove_function org1\nsound_profile on_soldier\n" + extra + "end\n");
}

const char *kArmed = "ammo_closeattack AMMO_TEST\nammo_easyrocket AMMO_ROCKET\nammo_advancedrocket AMMO_ROCKET2\n"
                     "ammo_marker3 AMMO_TEST\nlaunchups_closeattack mflash01\nlaunchups_rocket MFlash01\n";

struct FireProject {
	editor_test::TempProjectDir dir{"opennova_editor_clip_fire"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	std::string path = "anims/SOLD.adm";
	bool made = false;

	FireProject() {
		session.handle(request::new_project(dir.file("project"), "Clip Fire"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const std::string source = dir.file("source");
		bool ok = editor_test::write_text(source + "/skinned.o3d", soldier_model()) &&
		          editor_test::write_text(source + "/sold.o3a", sold_clips());
		EditorRequest import = request::of(EditorRequestKind::ImportFiles);
		import.imports = {{source + "/skinned.o3d", {}}, {source + "/sold.o3a", {}}};
		session.handle(import);
		session.run_operations();
		ok = ok && editor_test::write_bytes(root + "/sounds/game.lwf", bank_of(std::vector<std::string>(std::begin(kSets), std::end(kSets))));
		const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
		for (const char *set : kSets) ok = ok && editor_test::write_bytes(root + "/sounds/" + strutil::to_lower(set) + ".wav", tone);
		ok = ok && editor_test::write_text(root + "/defs/SndProf.def",
		                                   editor_test::crlf("begin \"default\"\nend\nbegin \"on_soldier\"\n"
		                                                     "\tSSLFootGND FS_GND_L 0 0 0\nend\n")) &&
		     editor_test::write_text(root + "/defs/ammo.def", editor_test::crlf(kAmmo)) &&
		     editor_test::write_text(root + "/particles/fire.ptl", particles());
		ok = ok && write_items("org1", kArmed);
		made = ok && select("anim_attack");
	}

	bool write_items(const std::string &ai_function, const std::string &extra) {
		if (!editor_test::write_text(root + "/defs/items.def", items(ai_function, extra))) return false;
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		return true;
	}
	bool select(const char *key) {
		session.handle(request::open_document(path));
		Document *table = session.document_for(path);
		NodeAddress row;
		if (!table || !find_definition(AssetGraph(), *table, key, row)) return false;
		session.handle(request::select_record(path, row));
		return session.view().documents.previews[ViewportKind::Model].path == path;
	}
	const ModelViewport *viewport() {
		return dynamic_cast<const ModelViewport *>(session.viewports().follow_one(session.view(), path, ViewportKind::Model));
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		return session.outcome().done();
	}
	std::string refusal(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		return session.outcome().done() || session.outcome().findings.empty() ? std::string()
		                                                                       : session.outcome().findings.front().message;
	}
	bool hold(int32_t ticks) { return set(R"({"clock": {"ticks": )" + std::to_string(ticks) + R"(, "playing": false}})"); }
	JsonValue json() {
		ViewportModel *model = session.viewports().follow_one(session.view(), path, ViewportKind::Model);
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	// From tick 0, the clock run to `ticks`: the sounds fired on the way.
	std::vector<ClipSoundFired> run(int32_t ticks) {
		set(R"({"clock": {"ticks": 0, "playing": true, "rate": 1}})");
		const uint64_t before = session.viewports().clip_sound_seq();
		std::vector<ClipSoundFired> out;
		while (session.viewports().clock().ticks() < ticks) {
			session.advance(0.5 / 62.5);
			if (const ModelViewport *model = viewport())
				for (const ClipSoundFired &fired : model->sounds_fired())
					if (fired.seq > before &&
					    std::none_of(out.begin(), out.end(), [&](const ClipSoundFired &had) { return had.seq == fired.seq; }))
						out.push_back(fired);
		}
		set(R"({"clock": {"playing": false}})");
		return out;
	}
};

using Kind = WeaponRangeEvent::Kind;

std::vector<WeaponRangeEvent> events_of(const WeaponRange &range, Kind kind) {
	std::vector<WeaponRangeEvent> out;
	for (const WeaponRangeEvent &event : range.events())
		if (event.kind == kind) out.push_back(event);
	return out;
}

float distance(const PreviewVec3 &a, const PreviewVec3 &b) {
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// (tick, ammo slot) of each shot that fires.
std::vector<std::pair<int32_t, int>> shot_slots(const ClipFire &fire) {
	std::vector<std::pair<int32_t, int>> out;
	for (const ClipFireShot &shot : fire.shots())
		if (shot.ammo != 0) out.emplace_back(shot.tick, shot.ammo_slot);
	return out;
}
using TS = std::vector<std::pair<int32_t, int>>;

const JsonValue *fire_json(const JsonValue &shown) {
	const JsonValue *body = shown.get("body");
	const JsonValue *animation = body ? body->get("animation") : nullptr;
	return animation ? animation->get("fire") : nullptr;
}

// --- the tests -------------------------------------------------------------------------------------------------

// The clip's fire events fire the item's ammo on the NPC body's odd ticks, in the block's order, each from its launch
// point as the clip poses it; each round launched, flown and stopped on the target as a soldier's shot shows.
int test_a_clip_fires_the_items_ammo() {
	FireProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	JsonValue shown = project.json();
	const JsonValue *fire = fire_json(shown);
	TEST_EXPECT(fire && fire->get_bool("armed", false) && fire->get_string("body", "") == "npc" &&
	            fire->get_string("item", "") == "Soldier" && fire->get_bool("person", false));
	if (!fire) return 1;
	// The item's bytes as the init resolves them (AT_NULL the null row 0), its points (MFlash01 the first, the
	// marker3 point none).
	const JsonValue *ammo = fire->get("ammo");
	TEST_EXPECT(ammo && ammo->array.size() == 4 && ammo->array[0].get_string("ammo", "") == "AMMO_TEST" &&
	            ammo->array[0].get_number("byte", 0) == 1 && ammo->array[1].get_string("ammo", "") == "AMMO_ROCKET" &&
	            ammo->array[2].get_number("byte", 0) == 3 && ammo->array[3].get_string("ammo", "") == "AMMO_TEST" &&
	            ammo->array[0].get_string("ai_launch", "") == "GS_AI" &&
	            ammo->array[0].get_string("ai_launcheffect", "") == "Flash");
	const JsonValue *launch = fire->get("launch");
	TEST_EXPECT(launch && launch->array.size() == 3 && launch->array[0].get_number("point", 0) == 1 &&
	            launch->array[1].get_number("point", 0) == 1 && launch->array[2].get_number("point", 1) == 0);
	// Each fire event says what it fires.
	const JsonValue *events = shown.get("body")->get("animation")->get("events");
	TEST_EXPECT(events && events->array.size() == 3);
	if (events && events->array.size() == 3) {
		const JsonValue *fires = events->array[0].get("fires");
		TEST_EXPECT(fires && fires->array.size() == 1 &&
		            fires->array[0].string == "an NPC fires its first ammo: AMMO_TEST (ammo_closeattack) from mflash01 "
		                                      "(launchups_closeattack), GS_AI heard, Flash at the point");
		const JsonValue *rockets = events->array[1].get("fires");
		TEST_EXPECT(rockets && rockets->array.size() == 2 &&
		            rockets->array[0].string.rfind("an NPC fires its second ammo: AMMO_ROCKET (ammo_easyrocket)", 0) == 0 &&
		            rockets->array[1].string.rfind("an NPC fires its second ammo: AMMO_ROCKET2 (ammo_advancedrocket)", 0) == 0);
		const JsonValue *marker = events->array[2].get("fires");
		TEST_EXPECT(marker && marker->array.size() == 1 &&
		            marker->array[0].string.find("AMMO_TEST (ammo_marker3) from the body's origin") != std::string::npos);
	}

	// The clock run through one pass: frame 2 read on tick 5, frame 4 on 9 (the easyrocket, then the advancedrocket that
	// differs, both from the rocket point), frame 6 on 13 (the marker3 ammo from the body's origin).
	std::vector<ClipSoundFired> sounds = project.run(16);
	const ModelViewport *viewport = project.viewport();
	TEST_EXPECT(viewport != nullptr);
	if (!viewport) return 1;
	const ClipFire &clip_fire = viewport->clip_fire();
	TEST_EXPECT(shot_slots(clip_fire) == TS({{5, 0}, {9, 1}, {9, 2}, {13, 3}}));
	const WeaponRange &range = clip_fire.range();
	TEST_EXPECT(range.ready() && range.shots() == 4);
	const std::vector<WeaponRangeEvent> fired = events_of(range, Kind::Fired);
	TEST_EXPECT(fired.size() == 4 && fired[0].tick == 5 && fired[0].round == 1 && fired[3].round == 4 && fired[0].tracer);
	// Each shot leaves its launch point as the clip poses it on the tick: the overlay marks MFlash01 there; the marker3
	// shot leaves the body's origin.
	if (clip_fire.shots().size() == 4) {
		const ClipFireShot &first = clip_fire.shots()[0];
		TEST_EXPECT(first.point == "MFlash01" && first.frame == 2 && first.clip_tick == 5);
		TEST_EXPECT(project.hold(5));
		bool marked = false;
		for (const ModelOverlay &overlay : project.viewport()->overlays(project.session.viewports().clock()))
			if (overlay.kind == ModelOverlayKind::UserPoint && overlay.name == "MFlash01")
				marked = distance(overlay.at, first.at) < 1e-4f;
		TEST_EXPECT(marked);
		// The clip turns the bone: the point is not where it rests.
		TEST_EXPECT(distance(first.at, PreviewVec3{0.0f, 1.0f, 0.5f}) > 0.05f);
		const ClipFireShot &marker = clip_fire.shots()[3];
		TEST_EXPECT(marker.point.empty() && distance(marker.at, PreviewVec3{}) < 1e-6f);
	}
	project.run(16);
	const ClipFire &again = project.viewport()->clip_fire();
	// The launches: the ammo arm's ai_launcheffect at the fire origin, the rockets' Smoke.
	const std::vector<WeaponRangeEvent> launches = events_of(again.range(), Kind::Launch);
	TEST_EXPECT(launches.size() == 4 && launches[0].effect == "Flash" && launches[1].effect == "Smoke" &&
	            launches[0].tick == 5);
	// The rifle round stops on the target 25 metres on, the dirt row: its effect, its sound, a scar on the face.
	const std::vector<WeaponRangeEvent> impacts = events_of(again.range(), Kind::Impact);
	TEST_EXPECT(!impacts.empty() && impacts[0].effect == "Hit" && impacts[0].set == "IMP_DIRT" && impacts[0].tag == 5 &&
	            std::fabs(impacts[0].at.x - 25.0f) < 0.05f && impacts[0].tick > 5 && impacts[0].tick <= 9);
	TEST_EXPECT(again.range().scar_count() >= 1);
	// The effects the run spawned, each on its tick.
	bool launch_spawn = false, impact_spawn = false;
	for (const DefinitionSpawn &spawn : again.effects().spawns()) {
		launch_spawn = launch_spawn || (spawn.source == "launch" && spawn.effect == "Flash" && spawn.tick == 5);
		impact_spawn = impact_spawn || (spawn.source == "impact" && spawn.effect == "Hit");
	}
	TEST_EXPECT(launch_spawn && impact_spawn && again.effects().scene() != nullptr);
	// The sounds as the clock ran: the left foot (the sound block reads the same word), GS_AI at the launch on tick
	// 5, the impact's IMP_DIRT, the rockets' GS_ROCKET.
	bool launch_heard = false, impact_heard = false, rocket_heard = false, foot_heard = false;
	for (const ClipSoundFired &one : sounds) {
		launch_heard = launch_heard || (one.set == "GS_AI" && one.tick == 5 && one.state == "played" &&
		                                one.voices.size() == 1 && one.voices[0].path == "sounds/gs_ai.wav");
		impact_heard = impact_heard || (one.set == "IMP_DIRT" && one.state == "played");
		rocket_heard = rocket_heard || one.set == "GS_ROCKET";
		foot_heard = foot_heard || (one.set == "FS_GND_L" && one.tick == 13);
	}
	TEST_EXPECT(launch_heard && impact_heard && rocket_heard && foot_heard);
	// Muted: the shots still fire and say what they sound.
	TEST_EXPECT(project.set(R"({"options": {"sound": {"mute": true}}})"));
	bool muted = false;
	for (const ClipSoundFired &one : project.run(8)) muted = muted || (one.set == "GS_AI" && one.state == "muted");
	TEST_EXPECT(muted);
	TEST_EXPECT(project.set(R"({"options": {"sound": {"mute": false}}})"));

	// The envelope: the shots, the events, the tracers, the scars, the effects.
	project.hold(7);
	shown = project.json();
	fire = fire_json(shown);
	TEST_EXPECT(fire && fire->get("shots") && fire->get("shots")->array.size() == 1 &&
	            fire->get("shots")->array[0].get_string("point", "") == "MFlash01" &&
	            fire->get("shots")->array[0].get_number("tick", 0) == 5 && fire->get_number("shots_fired", 0) == 1);
	TEST_EXPECT(fire && fire->get("tracers") && !fire->get("tracers")->array.empty());
	TEST_EXPECT(fire && fire->get("range") && fire->get("range")->get("target") &&
	            fire->get("range")->get("target")->get("corners"));
	return 0;
}

// Which bodies fire: the player's body none; an item of no person class, or naming no ammo, none; an advancedrocket
// that repeats the easyrocket fires once.
int test_bodies_and_items() {
	FireProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	// The player's body has no fire block.
	TEST_EXPECT(project.set(R"({"options": {"sound": {"body": "player"}}})"));
	project.run(16);
	TEST_EXPECT(!project.viewport()->clip_fire().armed() && project.viewport()->clip_fire().shots().empty() &&
	            project.viewport()->clip_fire().words().find("no fire block") != std::string::npos);
	JsonValue shown = project.json();
	const JsonValue *events = shown.get("body")->get("animation")->get("events");
	TEST_EXPECT(events && !events->array.empty() && events->array[0].get("fires")->array.size() == 1 &&
	            events->array[0].get("fires")->array[0].string.rfind("fires nothing: ", 0) == 0);
	TEST_EXPECT(project.set(R"({"options": {"sound": {"body": "npc"}}})"));
	project.run(16);
	TEST_EXPECT(project.viewport()->clip_fire().armed() && project.viewport()->clip_fire().range().shots() == 4);

	// An item of no person class: the organic init never runs, no byte is set.
	TEST_EXPECT(project.write_items("gnrc", kArmed) && project.select("anim_attack"));
	project.run(16);
	TEST_EXPECT(!project.viewport()->clip_fire().armed() &&
	            project.viewport()->clip_fire().words().find("is no person class") != std::string::npos &&
	            project.viewport()->clip_fire().range().shots() == 0);
	// An item naming no ammo.
	TEST_EXPECT(project.write_items("org1", "launchups_closeattack MFlash01\n") && project.select("anim_attack"));
	project.json();
	TEST_EXPECT(!project.viewport()->clip_fire().armed() &&
	            project.viewport()->clip_fire().words().find("names no ammo") != std::string::npos);
	// An advancedrocket that repeats the easyrocket: one rocket; a name ammo.def lacks: no byte.
	TEST_EXPECT(project.write_items("org1", "ammo_closeattack AMMO_MISSING\nammo_easyrocket AMMO_ROCKET\n"
	                                        "ammo_advancedrocket AMMO_ROCKET\nlaunchups_rocket MFlash01\n") &&
	            project.select("anim_attack"));
	project.run(16);
	TEST_EXPECT(shot_slots(project.viewport()->clip_fire()) == TS({{9, 1}}));
	shown = project.json();
	events = shown.get("body")->get("animation")->get("events");
	TEST_EXPECT(events && events->array[0].get("fires")->array.size() == 1 &&
	            events->array[0].get("fires")->array[0].string ==
	                    "an NPC fires its first ammo: its ammo_closeattack names no ammo: nothing fires");
	return 0;
}

// A mark pressed fires its shot once; the run again when the clock steps back; the options' wire and refusals.
int test_press_rerun_and_options() {
	FireProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	project.json();
	// A press of frame 2 (play_sound {frame}): its shot's ai_launch heard once from its launch point.
	const WorkspaceView::Sound &sound = project.session.view().workspace.sound;
	project.session.handle(request::play_clip_event(project.path, 2));
	TEST_EXPECT(project.session.outcome().done() && sound.set == "GS_AI" && sound.voices.size() == 1 &&
	            sound.voices[0].path == "sounds/gs_ai.wav" &&
	            sound.words.rfind("Frame 2 (an NPC fires its first ammo): AMMO_TEST (ammo_closeattack) from MFlash01 "
	                              "(launchups_closeattack). ", 0) == 0);
	// Frame 6 plays its foot and fires its marker3 shot.
	project.session.handle(request::play_clip_event(project.path, 6));
	TEST_EXPECT(project.session.outcome().done() && sound.voices.size() == 2);
	// A player's body fires nothing and frame 2 sounds nothing: refused, with why.
	TEST_EXPECT(project.set(R"({"options": {"sound": {"body": "player"}}})"));
	project.json();
	project.session.handle(request::play_clip_event(project.path, 2));
	TEST_EXPECT(!project.session.outcome().done() &&
	            project.session.view().activity.status.find("fires nothing") != std::string::npos);
	TEST_EXPECT(project.set(R"({"options": {"sound": {"body": "npc"}}})"));

	// The run is a function of the clock: stepped back, it runs again from tick 0 to the same events.
	project.run(16);
	const std::vector<WeaponRangeEvent> first = project.viewport()->clip_fire().range().events();
	const uint64_t runs = project.viewport()->clip_fire().range().runs();
	TEST_EXPECT(project.hold(8));
	const ClipFire &back = project.viewport()->clip_fire();
	TEST_EXPECT(back.range().runs() > runs);
	TEST_EXPECT(project.hold(16));
	const std::vector<WeaponRangeEvent> &second = project.viewport()->clip_fire().range().events();
	bool same = first.size() == second.size();
	for (size_t i = 0; same && i < first.size(); ++i)
		same = first[i].tick == second[i].tick && first[i].kind == second[i].kind && first[i].effect == second[i].effect &&
		       first[i].set == second[i].set;
	TEST_EXPECT(same);

	// The options: another surface plays another row; the target's distance; the enemy's view; no target.
	TEST_EXPECT(project.set(R"({"options": {"fire": {"surface": "metal", "range": 12, "enemy": true}}})"));
	JsonValue shown = project.json();
	const JsonValue *options = shown.get("options") ? shown.get("options")->get("fire") : nullptr;
	TEST_EXPECT(options && options->get_string("surface", "") == "metal" && options->get_number("range", 0) == 12 &&
	            options->get_bool("enemy", false) && options->get_bool("target", false));
	project.run(16);
	const std::vector<WeaponRangeEvent> impacts = events_of(project.viewport()->clip_fire().range(), Kind::Impact);
	TEST_EXPECT(!impacts.empty() && impacts[0].effect == "Spark" && impacts[0].set == "IMP_METAL" &&
	            std::fabs(impacts[0].at.x - 12.0f) < 0.05f);
	TEST_EXPECT(project.set(R"({"options": {"fire": {"target": false}}})"));
	project.run(16);
	TEST_EXPECT(events_of(project.viewport()->clip_fire().range(), Kind::Impact).empty());
	TEST_EXPECT(project.refusal(R"({"options": {"fire": {"surface": "lava"}}})").find("options.fire.surface") == 0);
	TEST_EXPECT(project.refusal(R"({"options": {"fire": {"range": 900}}})").find("options.fire.range") == 0);
	TEST_EXPECT(project.refusal(R"({"options": {"fire": {"speed": 1}}})").find("Unknown fire option") == 0);
	return 0;
}

// The editor's shots against the game's own: the org1 think's fire pass (world::AiSystem::infantry_fire_pass) run on
// the same word over a body of the same bytes, its pose provider handing the launch point the clip posed, fires the same
// ammo in the same order from the same place along the same angles, with the same fire record and launch sound.
struct LaunchPoint final : world::IPoseProvider {
	int32_t at[3] = {};
	bool resolve_organic_attachment(world::World &, world::EntityHandle, uint8_t index, int32_t out[3]) override {
		if (index == 0) return false;
		for (int i = 0; i < 3; ++i) out[i] = at[i];
		return true;
	}
};

int test_the_shot_is_the_games_npc_shot() {
	FireProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	project.run(16);
	const ClipFire &fire = project.viewport()->clip_fire();
	const world::AmmoTable *table = fire.range().ammo_table();
	TEST_EXPECT(table != nullptr && fire.shots().size() == 4);
	if (!table || fire.shots().size() != 4) return 1;
	// The rockets of frame 4 on tick 9, from the rocket point as the clip posed it there.
	const ClipFireShot &easy = fire.shots()[1];
	const ClipFireShot &advanced = fire.shots()[2];
	// The game: a world of the project's ammo, one org1 body at its origin, level, facing +x, the item's bytes.
	auto heap = std::make_unique<world::World>();
	world::World &game = *heap;
	game.tables.ammo = *table;
	game.registry.configure_pool(0, 4);
	game.registry.configure_pool(1, 4);
	world::Entity seed;
	seed.kind = world::EntityKind::Organic;
	seed.item_type = 3;
	seed.has_item_def = true;
	seed.alive = true;
	seed.health = 100;
	seed.team = 2;
	const world::EntityHandle soldier = game.registry.spawn(0, seed);
	game.ai.attach(soldier);
	game.ai.is_authority = true;
	world::AiEntity &body = *game.ai.for_handle(soldier);
	body.inf.active = true;
	body.pos[2] = 10 << 16; // over the water plane, as the range stands its soldier
	body.profile.organic = fire.weapons();
	body.inf.magazine = 5;
	// The launch point as the clip posed it, in the mission frame (the preview's (y, z, x)), over the body.
	LaunchPoint point;
	point.at[0] = int32_t(std::lround(double(easy.at.z) * 65536.0));
	point.at[1] = int32_t(std::lround(double(easy.at.x) * 65536.0));
	point.at[2] = int32_t(std::lround(double(easy.at.y) * 65536.0)) + body.pos[2];
	game.pose_provider = &point;
	game.out.fire_sounds.set_listener(world::Vec3{0.0f, 0.0f, 10.0f});
	body.inf.last_events = anim::kAnimEventFireSecondary;
	game.logic_tick = uint32_t(easy.tick);
	game.ai.infantry_fire_pass(body, game, uint32_t(easy.tick));
	// The same two shots, in the same order: the easyrocket's ammo then the advancedrocket's, a magazine round spent.
	TEST_EXPECT(game.round_sim.fired.size() == 2);
	if (game.round_sim.fired.size() != 2) return 1;
	TEST_EXPECT(game.round_sim.fired[0].ammo_index == easy.ammo && game.round_sim.fired[1].ammo_index == advanced.ammo);
	TEST_EXPECT(body.inf.magazine == 4);
	// From the same place (the editor's range stands its frame at the launch point's rest height) along the same
	// angles (the body's heading and pitch, level along +x).
	std::vector<WeaponRangeEvent> fired;
	for (const WeaponRangeEvent &event : fire.range().events())
		if (event.kind == WeaponRangeEvent::Kind::Fired && event.tick == easy.tick) fired.push_back(event);
	TEST_EXPECT(fired.size() == 2);
	for (size_t i = 0; i < fired.size() && i < 2; ++i) {
		const world::Vec3 &origin = game.round_sim.fired[i].origin;
		const PreviewVec3 editor = fire.to_preview(fired[i].at);
		TEST_EXPECT(std::fabs(origin.x - editor.z) < 1e-3f && std::fabs(origin.y - editor.x) < 1e-3f &&
		            std::fabs(origin.z - 10.0f - editor.y) < 1e-3f);
		TEST_EXPECT(game.round_sim.fired[i].yaw_bam == 0 && game.round_sim.fired[i].pitch_bam == 0);
	}
	// The launch sound: the ammo's ai_launch, the game's NPC entry's own leg.
	std::vector<std::string> heard;
	for (int step = 0; step < 4; ++step) {
		for (const world::ReadyFireSound &ready : game.out.fire_sounds.drain()) heard.push_back(ready.set_name);
		game.out.fire_sounds.tick();
	}
	TEST_EXPECT(heard.size() == 2 && heard[0] == "GS_ROCKET" && heard[1] == "GS_ROCKET");
	bool editor_heard = false;
	for (const WeaponRangeEvent &event : fire.range().events())
		editor_heard = editor_heard || (event.kind == WeaponRangeEvent::Kind::Sound && event.tick == easy.tick &&
		                                event.set == "GS_ROCKET");
	TEST_EXPECT(editor_heard);
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_a_clip_fires_the_items_ammo() == 0);
	TEST_EXPECT(test_bodies_and_items() == 0);
	TEST_EXPECT(test_press_rerun_and_options() == 0);
	TEST_EXPECT(test_the_shot_is_the_games_npc_shot() == 0);
	std::printf("editor_clip_fire OK\n");
	return 0;
}
