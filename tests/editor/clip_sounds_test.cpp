// A clip's events heard in the model preview (DI-04; editor/preview/preview_clip_sounds, the session's
// clip_sounds): as the preview clock runs a clip, each tick it passes through fires what the game's body
// fires there, through the engine's own rules. Headless (no device follows the viewport: the session's
// advance follows it), over minted fixtures alone: the skinned fixture rig, clips made here as the Blender
// add-on writes them (an .o3a), a bank minted through lwf::encode_lwf, SndProf.def text and a short wave.
//
// Pinned: the NPC body's odd ticks and the player body's even ones (a 30 fps clip of 8 frames: frame 2
// first read on tick 5, frame 6 on tick 13); a loop's second pass; the Surface's slots (ground, snow, an
// object, water); foley before the foot in one word; a repeated one-shot's start reading nothing and its
// next pass firing again; nothing over a seek, a frame step or a pause; ticks passed several at a time, the
// first run after a seek heard whole from where the seek put the clock;
// the item's profile and body (sound_profile, sound_profileFemale for a female player, move_function
// org2), a profile picked, an empty slot; Mute; the camera's distance (a volume by the falloff, a set out
// of range); the envelope's options, sound_profile, sound_body, each event's plays and sounds_fired; what
// the Shell is handed (clip_sounds_since); a timeline mark pressed (play_sound {frame}) and its refusals.
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/animation_map_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_clip_sounds.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/lwf.h>
#include <runtime/audio/sound_profile.h>

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

// A clip of the skinned fixture's three bones held at rest, `frames` frames at 30 a second, its event
// words by frame (the end pose's record repeating its last frame's, as retail's exporter writes it).
std::string clip_text(const std::string &name, int frames, bool loop, const std::map<int, uint32_t> &events) {
	std::string out = "clip " + name + "\nfps 30\nflags " + (loop ? "0x1" : "0x0") + "\nframes " + std::to_string(frames) + "\n";
	const char *bones[] = {"bone -1 0 0 0 0.5 \"BN01 Pelvis\"", "bone 0 0 0 1 0.5 \"BN02 Spine\"",
	                       "bone 0 0 0 -1 0.5 \"BN03 Leg\""};
	for (const char *bone : bones) {
		out += std::string(bone) + "\n";
		for (int i = 0; i <= frames; ++i) out += " k 0 0 0 1\n";
	}
	for (int i = 0; i <= frames; ++i) {
		const auto word = events.find(i < frames ? i : frames - 1);
		char line[64];
		std::snprintf(line, sizeof(line), "event 0 0 0 0x%X 0.9 1.7\n", word == events.end() ? 0u : word->second);
		out += line;
	}
	return out;
}

// SOLD.adm: its reset, a walk (a loop: the left foot on frame 2, the right on frame 6), a crawl (a loop:
// sound 5 on frame 1, sound 5 and the left foot on frame 3) and a one-shot (the right foot on frame 1).
std::string sold_clips() {
	return "o3a 1\nadm SOLD.adm\nrow anim_reset \"rest\"\nrow anim_walk_forward \"walk\"\n"
	       "row anim_walk_prone_forward \"crawl\"\nrow anim_idle \"once\"\n" +
	       clip_text("rest", 1, true, {}) + clip_text("walk", 8, true, {{2, 0x1u}, {6, 0x2u}}) +
	       clip_text("crawl", 4, true, {{1, 0x200u}, {3, 0x201u}}) + clip_text("once", 4, false, {{1, 0x2u}});
}

// A bank of one set per name, each one layer heard in either view playing its one wave (the name lower
// case), range 100 m, falloff 100 m, volume 200.
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

const char *kSets[] = {"FS_GND_L", "FS_GND_R", "FS_SNOW_L", "FS_SNOW_R", "FS_OBJ_L", "FS_OBJ_R", "FS_WATER", "FS_PRONE"};

std::string items(const std::string &extra) {
	return editor_test::crlf("begin \"Soldier\"\nid 100300\ntype person\ngraphic skinned\nanim_def sold\n" + extra + "end\n");
}

// A project of the soldier's rig and clips, its bank, its waves and its profiles; SOLD.adm open with the
// row `key` selected.
struct ClipProject {
	editor_test::TempProjectDir dir{"opennova_editor_clip_sounds"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	std::string path = "anims/SOLD.adm";
	bool made = false;

	ClipProject() {
		session.handle(request::new_project(dir.file("project"), "Clip Sounds"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const std::string source = dir.file("source");
		bool ok = editor_test::write_bytes(source + "/skinned.o3d",
		                                   test_io::read_file(repo() + "/fixtures/threedi/o3d/skinned.o3d")) &&
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
		                                                     "\tSSLFootGND FS_GND_L 0 0 0\n\tSSRFootGND FS_GND_R 0 0 0\n"
		                                                     "\tSSLFootSnow FS_SNOW_L 0 0 0\n\tSSRFootSnow FS_SNOW_R 0 0 0\n"
		                                                     "\tSSLFootOBJ FS_OBJ_L 0 0 0\n\tSSRFootOBJ FS_OBJ_R 0 0 0\n"
		                                                     "\tSSFootWater FS_WATER 0 0 0\n\tSSAudio5 FS_PRONE 0 0 0\nend\n"
		                                                     "begin \"on_female\"\n\tSSLFootGND FS_SNOW_L 0 0 0\nend\n"));
		ok = ok && write_items("sound_profile on_soldier\n");
		made = ok && select("anim_walk_forward");
	}

	bool write_items(const std::string &extra) {
		if (!editor_test::write_text(root + "/defs/items.def", items(extra))) return false;
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
		return dynamic_cast<const ModelViewport *>(session.viewports().find(path, ViewportKind::Model));
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		return session.outcome().done();
	}
	bool sound(const std::string &options) { return set(R"({"options": {"sound": )" + options + "}}"); }
	JsonValue json() {
		ViewportModel *model = session.viewports().follow_one(session.view(), path, ViewportKind::Model);
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	// From tick 0, the clock run to `ticks` (`per_call` ticks a Shell frame): the sounds fired on the way.
	std::vector<ClipSoundFired> run(int32_t ticks, double per_call = 0.25) {
		set(R"({"clock": {"ticks": 0, "playing": true, "rate": 1}})");
		const uint64_t before = session.viewports().clip_sound_seq();
		while (session.viewports().clock().ticks() < ticks) session.advance(per_call / 62.5);
		std::vector<ClipSoundFired> out;
		if (const ModelViewport *model = viewport())
			for (const ClipSoundFired &fired : model->sounds_fired())
				if (fired.seq > before) out.push_back(fired);
		return out;
	}
};

// (tick, slot) of each sound fired.
std::vector<std::pair<int32_t, int>> ticks_slots(const std::vector<ClipSoundFired> &fired) {
	std::vector<std::pair<int32_t, int>> out;
	for (const ClipSoundFired &one : fired) out.emplace_back(one.tick, one.slot);
	return out;
}

using TS = std::vector<std::pair<int32_t, int>>;

int test_walk_fires_on_the_body_s_ticks() {
	ClipProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	// The item's profile and body: on_soldier, its sound_profile; no move_function, an NPC's body.
	JsonValue shown = project.json();
	const JsonValue *animation = shown.get("body") ? shown.get("body")->get("animation") : nullptr;
	TEST_EXPECT(animation && animation->get("sound_profile") &&
	            animation->get("sound_profile")->get_string("name", "") == "on_soldier" &&
	            animation->get("sound_profile")->get_string("item", "") == "Soldier" &&
	            animation->get("sound_profile")->get_string("words", "") == "on_soldier, Soldier's sound_profile.");
	TEST_EXPECT(animation && animation->get("sound_body")->get_string("body", "") == "npc");
	const JsonValue *sound_options = shown.get("options") ? shown.get("options")->get("sound") : nullptr;
	TEST_EXPECT(sound_options && sound_options->get_string("surface", "") == "ground" &&
	            sound_options->get_string("body", "") == "auto" && !sound_options->get_bool("mute", true));
	// Each event says what it plays.
	const JsonValue *events = animation ? animation->get("events") : nullptr;
	TEST_EXPECT(events && events->array.size() == 2 && events->array[0].get("plays") &&
	            events->array[0].get("plays")->array.size() == 1 &&
	            events->array[0].get("plays")->array[0].string == "left footstep plays SSLFootGND: FS_GND_L (game.lwf)");

	// The NPC's body reads on odd ticks: frame 2 (ticks 5 and 6) on 5, frame 6 (13 and 14) on 13, and the
	// loop's second pass frame 2 again (21 and 22) on 21.
	std::vector<ClipSoundFired> fired = project.run(22);
	TEST_EXPECT(ticks_slots(fired) == TS({{5, audio::kSlotFootLGround}, {13, audio::kSlotFootRGround}, {21, audio::kSlotFootLGround}}));
	if (fired.size() == 3) {
		TEST_EXPECT(fired[0].frame == 2 && fired[0].foot == 0 && fired[0].set == "FS_GND_L" && fired[0].bank == "game.lwf" &&
		            fired[0].state == "played" && fired[0].profile == "on_soldier" && fired[1].frame == 6 && fired[1].foot == 1);
		TEST_EXPECT(fired[0].voices.size() == 1 && fired[0].voices[0].path == "sounds/fs_gnd_l.wav");
		// Heard at the camera: the layer's falloff lowers the member's 200.
		TEST_EXPECT(!fired[0].voices.empty() && fired[0].voices[0].volume > 0 && fired[0].voices[0].volume < 200);
		TEST_EXPECT(fired[0].words.rfind("Frame 2 (left footstep): on_soldier's SSLFootGND plays FS_GND_L in game.lwf: ", 0) == 0);
	}
	// What the Shell is handed: the three, in order, each its wave.
	const std::vector<ClipSoundPlay> plays = project.session.clip_sounds_since(fired.empty() ? 0 : fired.front().seq - 1);
	TEST_EXPECT(plays.size() == 3 && plays[0].voices.size() == 1 && plays[0].voices[0].path == "sounds/fs_gnd_l.wav" &&
	            plays[1].voices[0].path == "sounds/fs_gnd_r.wav" && plays[0].seq < plays[1].seq);
	// The wire's sounds_fired.
	shown = project.json();
	const JsonValue *wire = shown.get("body")->get("animation")->get("sounds_fired");
	TEST_EXPECT(wire && !wire->array.empty() && wire->array.back().get_string("keyword", "") == "SSLFootGND" &&
	            wire->array.back().get_string("foot", "") == "left" && wire->array.back().get_string("state", "") == "played");

	// The player's body reads on even ticks: 6 and 14.
	TEST_EXPECT(project.sound(R"({"body": "player"})"));
	TEST_EXPECT(ticks_slots(project.run(15)) == TS({{6, audio::kSlotFootLGround}, {14, audio::kSlotFootRGround}}));
	// Ticks passed four at a time fire the same.
	TEST_EXPECT(ticks_slots(project.run(15, 4.0)) == TS({{6, audio::kSlotFootLGround}, {14, audio::kSlotFootRGround}}));
	// The Play's first frame running seven ticks at once: the run from where its seek put the clock heard whole,
	// tick 6 among it.
	TEST_EXPECT(ticks_slots(project.run(15, 7.5)) == TS({{6, audio::kSlotFootLGround}, {14, audio::kSlotFootRGround}}));
	TEST_EXPECT(project.sound(R"({"body": "npc"})"));

	// The Surface: the slots the game's test picks.
	TEST_EXPECT(project.sound(R"({"surface": "snow"})"));
	fired = project.run(14);
	TEST_EXPECT(ticks_slots(fired) == TS({{5, audio::kSlotFootLSnow}, {13, audio::kSlotFootRSnow}}) &&
	            fired.size() == 2 && fired[0].set == "FS_SNOW_L" && fired[1].set == "FS_SNOW_R");
	TEST_EXPECT(project.sound(R"({"surface": "object"})"));
	TEST_EXPECT(ticks_slots(project.run(14)) == TS({{5, audio::kSlotFootLObject}, {13, audio::kSlotFootRObject}}));
	TEST_EXPECT(project.sound(R"({"surface": "water"})"));
	fired = project.run(14);
	TEST_EXPECT(ticks_slots(fired) == TS({{5, audio::kSlotFootWater}, {13, audio::kSlotFootWater}}) && fired.size() == 2 &&
	            fired[1].set == "FS_WATER");
	shown = project.json();
	TEST_EXPECT(shown.get("body")->get("animation")->get("events")->array[0].get("plays")->array[0].string ==
	            "left footstep plays SSFootWater: FS_WATER (game.lwf)");
	TEST_EXPECT(project.sound(R"({"surface": "ground"})"));
	TEST_EXPECT(!project.sound(R"({"surface": "mud"})") && !project.sound(R"({"body": "dog"})") &&
	            !project.sound(R"({"loud": true})"));

	// Nothing over a seek, a frame step or a pause.
	const uint64_t seq = project.session.viewports().clip_sound_seq();
	TEST_EXPECT(project.set(R"({"clock": {"ticks": 4, "playing": true}})"));
	TEST_EXPECT(project.set(R"({"clock": {"ticks": 14}})"));
	project.session.advance(0.001);
	TEST_EXPECT(project.set(R"({"frame": 2})"));
	project.session.advance(0.5);
	TEST_EXPECT(project.set(R"({"clock": {"ticks": 4, "playing": false}})"));
	project.session.advance(0.5);
	TEST_EXPECT(project.session.viewports().clip_sound_seq() == seq);

	// Mute: the events still fire and say what they play; the Shell is handed none.
	TEST_EXPECT(project.sound(R"({"mute": true})"));
	fired = project.run(14);
	TEST_EXPECT(fired.size() == 2 && fired[0].state == "muted" && !fired[0].voices.empty());
	TEST_EXPECT(project.session.clip_sounds_since(seq).empty());
	TEST_EXPECT(project.sound(R"({"mute": false})"));

	// The camera far off: past the set's 100 m range, the game plays nothing.
	TEST_EXPECT(project.set(R"({"camera": {"distance": 400}})"));
	fired = project.run(6);
	TEST_EXPECT(fired.size() == 1 && fired[0].state == "out_of_range" && fired[0].voices.empty());
	TEST_EXPECT(project.set(R"({"camera": {"frame": true}})"));

	// A profile picked: default's slots are empty, nothing plays.
	TEST_EXPECT(project.sound(R"({"profile": "default"})"));
	fired = project.run(6);
	TEST_EXPECT(fired.size() == 1 && fired[0].state == "empty" && fired[0].set.empty());
	TEST_EXPECT(project.viewport()->sound_binding().profile_words == "default, picked.");
	TEST_EXPECT(project.sound(R"({"profile": "nobody"})"));
	TEST_EXPECT(project.viewport()->sound_binding().profile == "default" &&
	            project.viewport()->sound_binding().profile_words == "No profile is named nobody: the game takes the first, default.");
	TEST_EXPECT(project.sound(R"({"profile": ""})"));
	return 0;
}

// Foley before the foot in one word; the crawl's sound 5 at the origin.
int test_crawl_and_one_shot() {
	ClipProject project;
	TEST_EXPECT(project.made && project.select("anim_walk_prone_forward"));
	// Frame 1 (ticks 3 and 4) on 3; frame 3 (7 and 8) on 7, sound 5 then the left foot.
	std::vector<ClipSoundFired> fired = project.run(8);
	TEST_EXPECT(ticks_slots(fired) == TS({{3, audio::kSlotAudio1 + 4}, {7, audio::kSlotAudio1 + 4}, {7, audio::kSlotFootLGround}}));
	if (fired.size() == 3) TEST_EXPECT(fired[0].set == "FS_PRONE" && fired[0].foot == -1 && fired[2].foot == 0);

	// A one-shot of 9 ticks repeated after the hold (31): its frame 1 on tick 3, then on 40 + 3; its start
	// (tick 40) reads nothing. With Repeat off it fires once.
	TEST_EXPECT(project.select("anim_idle"));
	project.json(); // followed: the clip the selection plays
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && !model->clip_loops() && model->clip_length_ticks() == 9);
	TEST_EXPECT(ticks_slots(project.run(45)) == TS({{3, audio::kSlotFootRGround}, {43, audio::kSlotFootRGround}}));
	TEST_EXPECT(project.set(R"({"options": {"repeat": false}})"));
	TEST_EXPECT(ticks_slots(project.run(45)) == TS({{3, audio::kSlotFootRGround}}));
	return 0;
}

// The item's body and profiles: move_function org2 a player's body (even ticks); a female player's
// sound_profileFemale; an item naming no profile binds default; a press of a mark plays it once.
int test_item_binding_and_press() {
	ClipProject project;
	TEST_EXPECT(project.made);
	TEST_EXPECT(project.write_items("sound_profile on_soldier\nsound_profileFemale on_female\nmove_function org2\n") &&
	            project.select("anim_walk_forward"));
	std::vector<ClipSoundFired> fired = project.run(15);
	TEST_EXPECT(ticks_slots(fired) == TS({{6, audio::kSlotFootLGround}, {14, audio::kSlotFootRGround}}));
	TEST_EXPECT(project.viewport()->sound_binding().player &&
	            project.viewport()->sound_binding().body_words.find("org2") != std::string::npos);
	TEST_EXPECT(project.sound(R"({"female": true})"));
	fired = project.run(7);
	TEST_EXPECT(fired.size() == 1 && fired[0].profile == "on_female" && fired[0].set == "FS_SNOW_L");
	// A female avatar's profile is a player's alone.
	TEST_EXPECT(project.sound(R"({"body": "npc"})"));
	TEST_EXPECT(project.viewport()->sound_binding().profile == "on_soldier");
	TEST_EXPECT(project.sound(R"({"body": "auto", "female": false})"));
	TEST_EXPECT(project.write_items("move_function org1\n") && project.select("anim_walk_forward"));
	project.json();
	TEST_EXPECT(!project.viewport()->sound_binding().player && project.viewport()->sound_binding().profile == "default" &&
	            project.viewport()->sound_binding().profile_words == "Soldier names no sound_profile: it binds default.");

	// A press of a mark (play_sound {frame}): the event played once through the workspace's sound.
	TEST_EXPECT(project.write_items("sound_profile on_soldier\n") && project.select("anim_walk_forward"));
	project.json();
	const WorkspaceView::Sound &sound = project.session.view().workspace.sound;
	project.session.handle(request::play_clip_event(project.path, 6));
	TEST_EXPECT(project.session.outcome().done() && sound.voices.size() == 1 && sound.voices[0].path == "sounds/fs_gnd_r.wav" &&
	            sound.set == "FS_GND_R" && sound.words.rfind("Frame 6 (right footstep): on_soldier's SSRFootGND plays ", 0) == 0);
	// The end pose is never read; frame 0 carries no event.
	project.session.handle(request::play_clip_event(project.path, 8));
	TEST_EXPECT(!project.session.outcome().done() &&
	            project.session.view().activity.status.find("never read") != std::string::npos);
	project.session.handle(request::play_clip_event(project.path, 0));
	TEST_EXPECT(!project.session.outcome().done() &&
	            project.session.view().activity.status.find("fires no sound") != std::string::npos);
	// The wire's form.
	JsonValue request = JsonValue::make_object();
	request.set("kind", JsonValue::make_string("play_sound"));
	request.set("path", JsonValue::make_string(project.path));
	JsonValue values = JsonValue::make_object();
	values.set("frame", JsonValue::make_string("2"));
	request.set("values", std::move(values));
	TEST_EXPECT(project.session.handle_json(request).get_bool("ok", false) && project.session.outcome().done() &&
	            sound.set == "FS_GND_L");
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_walk_fires_on_the_body_s_ticks() == 0);
	TEST_EXPECT(test_crawl_and_one_shot() == 0);
	TEST_EXPECT(test_item_binding_and_press() == 0);
	std::printf("editor_clip_sounds OK\n");
	return 0;
}
