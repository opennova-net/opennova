// A weapon's first-person view in the model preview (DI-13; editor/preview/preview_first_person): a map a
// weapon names as its animadm plays on its gfx1 with the character's arms on the gun's rig, the eye the game
// draws the view model before, and the weapon's actions baked and run as the game's pump runs them, their
// sets fired at the clip's ticks. Headless (no device follows the viewport: the session's advance follows
// it), over minted fixtures alone: the skinned fixture as the gun and as the arms, clips made here as the
// Blender add-on writes them (an .o3a), a bank minted through lwf::encode_lwf, weapon.def and Avatars.def
// text and a short wave.
//
// Pinned: the gun and the arms (fp_viewmodel_spec: a character with none draws none, an emplaced weapon
// none); the character seeded and one chosen, its team; the actions' runs (FIRE's soundsetend on its
// first tick, RELOAD's soundset as it begins and soundsetend 20 ticks on, the ticks the channel steps the
// clip, the action handed to); the legs fired as the clock runs, again as a repeated one-shot plays again,
// muted, handed to the Shell; a first-person clip's events not fired and its marks refused; a leg pressed
// (play_sound {leg}) and its refusal; the eye: a posed camera standing where the game's presenter stands
// the view model (renderer::fp_viewmodel_pose) with the 4:3 drop, refusing the camera and Frame; a map an
// item pairs, and a model chosen, show no first person; the options' wire and refusals.
#include <cmath>
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
#include <editor/preview/preview_first_person.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/lwf.h>
#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/player_view.h>

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

// A clip of the skinned fixture's three bones held at rest, `frames` frames at 30 a second, its event words
// by frame.
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

// GUN.adm: its reset, the idle (a loop), the fire (a one-shot carrying a left footstep the game never reads
// in first person) and the reload (a one-shot).
std::string gun_clips() {
	return "o3a 1\nadm GUN.adm\nrow anim_reset \"rest\"\nrow anim_wpn_idle \"idle\"\nrow anim_wpn_fire \"fire\"\n"
	       "row anim_wpn_reload \"reload\"\n" +
	       clip_text("rest", 1, true, {}) + clip_text("idle", 8, true, {}) + clip_text("fire", 8, false, {{1, 0x1u}}) +
	       clip_text("reload", 16, false, {});
}

// A bank of one set per name, each one layer heard in either view playing its one wave.
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

const char *kSets[] = {"GS_TEST", "RL_TEST", "RL_END"};

std::string weapons(const std::string &flags) {
	return "weapon \"WPN_TEST\"\n\tclipsize 10\n\tstartrounds 50\n\tanimadm gun\n\tgfx1 gun\n" + flags +
	       "\tpos 25 -5 -145 0 0 0\n\ttpos -1 12 -138 0 0 0\n"
	       "\taction \"idle\"\n\t\tanim anim_wpn_idle\n\t\tdelayend 30\n\t\tfunction wpn_std_idle\n\tend\n"
	       "\taction \"fire\"\n\t\tanim anim_wpn_fire\n\t\tsoundsetend GS_TEST\n\t\tdelayend 4\n\t\tfunction wpn_std_fire\n\tend\n"
	       "\taction \"recoil\"\n\t\tfunction wpn_std_recoil\n\tend\n"
	       "\taction \"reload\"\n\t\tanim anim_wpn_reload\n\t\tsoundset RL_TEST\n\t\tsoundsetend RL_END\n\t\tdelaystart 20\n"
	       "\t\tfunction wpn_std_reload\n\tend\nend\n";
}

// Two characters: a good one with the arms (camo 1 2 3) and an evil one with none.
const char *kAvatars =
		"define head H1\n{\n\tname AV_H\n\tgraphic gun.3di\n\tcamo 0 0 0\n\tvoice 1\n\tsex m\n}\n"
		"define body B1\n{\n\tname AV_B\n\tgraphic gun.3di\n\tcamo 0 0 0\n}\n"
		"define arms A1\n{\n\tname AV_A\n\tgraphic arms.3di\n\tcamo 1 2 3\n}\n"
		"nationality 0 AV_GOOD\n{\n\talignment good\n\tdivision 0 AV_DIV\n\t{\n\t\tcombo 1 H1 B1 A1\n\t}\n}\n"
		"nationality 1 AV_BAD\n{\n\talignment evil\n\tdivision 0 AV_DIV2\n\t{\n\t\tcombo 2 H1 B1\n\t}\n}\n";

// A project of the gun (the skinned fixture), the arms (the same model under another name), GUN.adm's clips,
// its bank and waves, weapon.def and Avatars.def; GUN.adm open with the row `key` selected.
struct GunProject {
	editor_test::TempProjectDir dir{"opennova_editor_first_person"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	std::string path = "anims/GUN.adm";
	bool made = false;

	GunProject() {
		session.handle(request::new_project(dir.file("project"), "First Person"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const std::string source = dir.file("source");
		const std::vector<uint8_t> skinned = test_io::read_file(repo() + "/fixtures/threedi/o3d/skinned.o3d");
		bool ok = editor_test::write_bytes(source + "/gun.o3d", skinned) &&
		          editor_test::write_bytes(source + "/arms.o3d", skinned) &&
		          editor_test::write_text(source + "/gun.o3a", gun_clips());
		EditorRequest import = request::of(EditorRequestKind::ImportFiles);
		import.imports = {{source + "/gun.o3d", {}}, {source + "/arms.o3d", {}}, {source + "/gun.o3a", {}}};
		session.handle(import);
		session.run_operations();
		ok = ok && editor_test::write_bytes(root + "/sounds/game.lwf", bank_of(std::vector<std::string>(std::begin(kSets), std::end(kSets))));
		const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
		for (const char *set : kSets) ok = ok && editor_test::write_bytes(root + "/sounds/" + strutil::to_lower(set) + ".wav", tone);
		ok = ok && editor_test::write_text(root + "/" + at("Avatars.def"), editor_test::crlf(kAvatars));
		made = ok && write_weapons("\tflags auto\n") && select("anim_wpn_fire");
	}

	// Where the project keeps a file of the name (a blank Create Missing made), else under defs/.
	std::string at(const std::string &name) {
		const AssetScan *scan = session.view().project.scan.get();
		const AssetEntry *entry = scan ? scan->find(name) : nullptr;
		return entry ? entry->relative_path : "defs/" + name;
	}
	bool write_weapons(const std::string &flags) {
		if (!editor_test::write_text(root + "/" + at("weapon.def"), editor_test::crlf(weapons(flags)))) return false;
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
	bool first_person(const std::string &options) { return set(R"({"options": {"first_person": )" + options + "}}"); }
	JsonValue json() {
		ViewportModel *model = session.viewports().follow_one(session.view(), path, ViewportKind::Model);
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	const JsonValue *first_person_json(JsonValue &shown) {
		shown = json();
		const JsonValue *body = shown.get("body");
		const JsonValue *animation = body ? body->get("animation") : nullptr;
		return animation ? animation->get("first_person") : nullptr;
	}
	// From tick 0, the clock run to `ticks`: the sounds fired on the way.
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

const WeaponActionRun *run_of(const FirstPersonSources &sources, const char *suffix) {
	for (const WeaponActionRun &run : sources.actions())
		if (run.suffix == suffix) return &run;
	return nullptr;
}

// The gun with its arms: the weapon pairing the map, the character's arms on the gun's rig, the characters,
// the team; a character with none, and an emplaced weapon, draw the gun alone.
int test_gun_with_its_arms() {
	GunProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	JsonValue shown;
	const JsonValue *fp = project.first_person_json(shown);
	TEST_EXPECT(fp && fp->is_object());
	if (!fp || !fp->is_object()) return 1;
	TEST_EXPECT(fp->get_string("weapon", "") == "WPN_TEST" && fp->get_string("gun", "") == "gun" &&
	            fp->get_string("arms", "") == "arms.3di" && fp->get_string("arms_file", "") == "arms.3di" &&
	            fp->get_string("adm", "") == "gun" && !fp->get_bool("emplaced", true) && fp->get_string("view", "") == "orbit");
	TEST_EXPECT(fp->get_string("words", "") ==
	            "WPN_TEST draws gun with arms.3di, the arms of AV_GOOD, AV_DIV, combo 1, posed by the gun's bones.");
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && model->first_person().active() && model->first_person().arms_model() &&
	            model->first_person().arms_note().empty());
	// The characters by their packed ids; the seeded one is the good side's first, the player team 1.
	const JsonValue *characters = fp->get("characters");
	TEST_EXPECT(characters && characters->array.size() == 2 && characters->array[0].get_bool("chosen", false) &&
	            characters->array[0].get_string("side", "") == "good" && characters->array[1].get_string("arms", "x").empty() &&
	            characters->array[1].get_string("side", "") == "evil");
	TEST_EXPECT(fp->get_number("team", 0) == 1 && model && model->first_person().character() &&
	            model->first_person().character()->camo[0] == 1 && model->first_person().character()->camo[2] == 3);
	// The view record: pos, its cant, tpos, renderfov's default.
	TEST_EXPECT(fp->get("pos") && fp->get("pos")->array.size() == 3 && fp->get("pos")->array[2].number == -145.0 &&
	            fp->get_number("renderfov", 0) == 80.0);
	// The evil character has no arms: the gun alone, the player team 2.
	if (characters && characters->array.size() == 2) {
		const int evil = int(characters->array[1].get_number("id", -1));
		TEST_EXPECT(project.first_person(R"({"character": )" + std::to_string(evil) + "}"));
		fp = project.first_person_json(shown);
		TEST_EXPECT(fp && fp->get_string("arms", "x").empty() && fp->get_number("team", 0) == 2 &&
		            fp->get_string("words", "") == "WPN_TEST draws gun with no arms: AV_BAD, AV_DIV2, combo 2 has none.");
		TEST_EXPECT(!project.viewport()->first_person().arms_model());
		// An id no character has: the seeded one.
		TEST_EXPECT(project.first_person(R"({"character": 4095})"));
		project.json();
		TEST_EXPECT(project.viewport()->first_person().arms_model() && project.viewport()->first_person().team() == 1);
		TEST_EXPECT(project.first_person(R"({"character": -1})"));
	}
	// An emplaced weapon draws its gun alone [renderer::fp_viewmodel_spec].
	TEST_EXPECT(project.write_weapons("\tflags emplaced\n") && project.select("anim_wpn_fire"));
	fp = project.first_person_json(shown);
	TEST_EXPECT(fp && fp->get_bool("emplaced", false) && fp->get_string("arms", "x").empty() &&
	            !project.viewport()->first_person().arms_model() &&
	            fp->get_string("words", "") == "WPN_TEST draws gun with no arms: an emplaced weapon draws its gun alone.");
	return 0;
}

// The weapon's actions as the game bakes and runs them, and their legs fired on the clip's ticks.
int test_actions_and_their_sets() {
	GunProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	project.json();
	const FirstPersonSources &sources = project.viewport()->first_person();
	TEST_EXPECT(sources.baked() && sources.actions().size() == world::weapon_action::kCount);
	// FIRE: its clip as its phase begins, its soundsetend (the shot) on that tick, the channel stepping three
	// ticks of its counter's four, then RECOIL.
	const WeaponActionRun *fire = run_of(sources, "fire");
	TEST_EXPECT(fire && fire->begins && fire->finish == 0 && fire->stepped == 3 && fire->next == "recoil" &&
	            fire->handler == "wpn_std_fire" && fire->legs.size() == 1 && fire->legs[0].end &&
	            fire->legs[0].set == "GS_TEST" && fire->legs[0].tick == 0);
	// RELOAD: its soundset as it begins, its soundsetend 20 ticks on, 19 ticks of its clip, then IDLE.
	const WeaponActionRun *reload = run_of(sources, "reload");
	TEST_EXPECT(reload && reload->begins && reload->finish == 20 && reload->stepped == 19 && reload->next == "idle" &&
	            reload->legs.size() == 2 && !reload->legs[0].end && reload->legs[0].set == "RL_TEST" &&
	            reload->legs[0].tick == 0 && reload->legs[1].end && reload->legs[1].set == "RL_END" && reload->legs[1].tick == 20);
	// IDLE's entry plays its clip and never begins a phase; it runs until another action is asked for.
	const WeaponActionRun *idle = run_of(sources, "idle");
	TEST_EXPECT(idle && !idle->begins && idle->next.empty() && idle->stepped == -1);
	// The row's action, on the wire.
	JsonValue shown;
	const JsonValue *fp = project.first_person_json(shown);
	TEST_EXPECT(fp && fp->get_string("action", "") == "fire" && fp->get("playing") && fp->get("playing")->array.size() == 1 &&
	            !fp->get_bool("events_read", true));
	// The clip's own event is read by nothing: what it plays says so.
	const JsonValue *events = shown.get("body")->get("animation")->get("events");
	TEST_EXPECT(events && events->array.size() == 1 && events->array[0].get("plays") &&
	            events->array[0].get("plays")->array.size() == 1 &&
	            events->array[0].get("plays")->array[0].string.rfind("The game reads no first-person clip event", 0) == 0);

	// The clock runs: the shot as the clock leaves the clip's tick 0, again as the repeated one-shot plays
	// again (its 17 ticks and the hold), never the clip's footstep.
	std::vector<ClipSoundFired> fired = project.run(60);
	TEST_EXPECT(project.viewport()->clip_length_ticks() == 17);
	TEST_EXPECT(fired.size() == 2 && fired[0].tick == 0 && fired[0].action == "fire" && fired[0].leg == "end" &&
	            fired[0].set == "GS_TEST" && fired[0].bank == "game.lwf" && fired[0].state == "played" &&
	            fired[0].slot == -1 && fired[1].tick == 48);
	if (!fired.empty()) {
		TEST_EXPECT(fired[0].voices.size() == 1 && fired[0].voices[0].path == "sounds/gs_test.wav");
		TEST_EXPECT(fired[0].words.rfind("FIRE finishes (tick 0 of its clip): GS_TEST in game.lwf: ", 0) == 0);
		// What the Shell is handed.
		const std::vector<ClipSoundPlay> plays = project.session.clip_sounds_since(fired[0].seq - 1);
		TEST_EXPECT(plays.size() == 2 && plays[0].voices.size() == 1 && plays[0].voices[0].path == "sounds/gs_test.wav");
	}
	shown = project.json();
	const JsonValue *wire = shown.get("body")->get("animation")->get("sounds_fired");
	TEST_EXPECT(wire && !wire->array.empty() && wire->array.back().get_string("action", "") == "fire" &&
	            wire->array.back().get_string("leg", "") == "end");
	// Muted: fired and said, not handed over.
	TEST_EXPECT(project.set(R"({"options": {"sound": {"mute": true}}})"));
	fired = project.run(4);
	TEST_EXPECT(fired.size() == 1 && fired[0].state == "muted");
	TEST_EXPECT(project.set(R"({"options": {"sound": {"mute": false}}})"));

	// The reload row: its begin on tick 0 and its end on the 20th, before the clip (33 ticks) plays again.
	TEST_EXPECT(project.select("anim_wpn_reload"));
	fired = project.run(30);
	TEST_EXPECT(fired.size() == 2 && fired[0].tick == 0 && fired[0].set == "RL_TEST" && fired[0].leg == "begin" &&
	            fired[1].tick == 20 && fired[1].set == "RL_END" && fired[1].leg == "end");

	// A leg pressed (play_sound {leg}): played once through the workspace's sound; a leg the action lacks,
	// and a first-person clip's frame, refused.
	const WorkspaceView::Sound &sound = project.session.view().workspace.sound;
	project.session.handle(request::play_action_leg(project.path, true));
	TEST_EXPECT(project.session.outcome().done() && sound.set == "RL_END" && sound.voices.size() == 1 &&
	            sound.voices[0].path == "sounds/rl_end.wav");
	TEST_EXPECT(project.select("anim_wpn_fire"));
	project.json();
	project.session.handle(request::play_action_leg(project.path, false));
	TEST_EXPECT(!project.session.outcome().done() &&
	            project.session.view().activity.status.find("fire plays no soundset.") != std::string::npos);
	project.session.handle(request::play_clip_event(project.path, 1));
	TEST_EXPECT(!project.session.outcome().done() &&
	            project.session.view().activity.status.find("reads no first-person clip event") != std::string::npos);
	// The idle row: no action's phase begins, no set plays.
	TEST_EXPECT(project.select("anim_wpn_idle"));
	TEST_EXPECT(project.run(40).empty());
	return 0;
}

// The first-person eye: a posed camera where the game's presenter stands the view model, the camera and
// Frame refused while it shows; the options' wire and refusals.
int test_the_eye() {
	GunProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.set(R"({"device": {"width": 800, "height": 600}})"));
	TEST_EXPECT(project.first_person(R"({"view": "eye"})"));
	JsonValue shown;
	project.first_person_json(shown);
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && model->eye_view() && model->camera().posed && model->camera().fov_degrees() == 80.0f &&
	            model->camera().near_plane == renderer::kViewmodelPassNearZ);
	// Where the presenter stands the model: the root at the pos over 256 on the camera's axes (a 4:3 picture
	// takes the framing drop), the rig's yaw-180, no cant. The model's origin seen from the eye is the root.
	const float drop = float(world::kFpNarrowAspectDropQ16) / 65536.0f;
	const float units[3] = {25.0f / 256.0f, -5.0f / 256.0f, -145.0f / 256.0f - drop};
	const float cant[3] = {0.0f, 0.0f, 0.0f};
	const float rig[3] = {0.0f, renderer::kViewmodelRigYawDeg, 0.0f};
	const renderer::FpViewmodelPose pose = renderer::fp_viewmodel_pose(units, cant, rig);
	if (model) {
		PreviewVec3 right, up, back;
		model->camera().axes(right, up, back);
		const PreviewVec3 eye = model->camera().eye();
		const auto dot = [](const PreviewVec3 &a, const PreviewVec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
		const PreviewVec3 to{-eye.x, -eye.y, -eye.z};
		TEST_EXPECT(std::fabs(dot(to, right) - pose.origin[0]) < 1e-5f && std::fabs(dot(to, up) - pose.origin[1]) < 1e-5f &&
		            std::fabs(dot(to, back) - pose.origin[2]) < 1e-5f);
		// Its axes are the root's rows: the model's +x is the camera's left (the yaw-180).
		TEST_EXPECT(std::fabs(right.x + 1.0f) < 1e-5f && std::fabs(back.z + 1.0f) < 1e-5f);
	}
	const JsonValue *camera = shown.get("camera");
	TEST_EXPECT(camera && camera->get_string("view", "") == "eye" && camera->get("pose") && camera->get("pose")->get("eye"));
	// Nothing moves the eye: a camera and Frame refused, with why.
	TEST_EXPECT(!project.set(R"({"camera": {"yaw": 1.0}})"));
	TEST_EXPECT(project.session.view().activity.status.find("first-person eye") != std::string::npos);
	// A wider picture takes no drop.
	TEST_EXPECT(project.set(R"({"device": {"width": 1000, "height": 500}})"));
	project.json();
	if (project.viewport()) {
		PreviewVec3 right, up, back;
		project.viewport()->camera().axes(right, up, back);
		const PreviewVec3 eye = project.viewport()->camera().eye();
		const float wide[3] = {25.0f / 256.0f, -5.0f / 256.0f, -145.0f / 256.0f};
		const renderer::FpViewmodelPose open = renderer::fp_viewmodel_pose(wide, cant, rig);
		TEST_EXPECT(std::fabs(-(eye.x * up.x + eye.y * up.y + eye.z * up.z) - open.origin[1]) < 1e-5f);
	}
	// Back to the orbit: the camera moves again.
	TEST_EXPECT(project.first_person(R"({"view": "orbit"})"));
	project.json();
	TEST_EXPECT(!project.viewport()->eye_view() && !project.viewport()->camera().posed);
	TEST_EXPECT(project.set(R"({"camera": {"yaw": 1.0}})"));
	// The options' wire, and refusals.
	shown = project.json();
	const JsonValue *options = shown.get("options") ? shown.get("options")->get("first_person") : nullptr;
	TEST_EXPECT(options && options->get_string("view", "") == "orbit" && options->get_number("character", 0) == -1 &&
	            options->get_string("action", "x").empty());
	TEST_EXPECT(!project.first_person(R"({"view": "side"})"));
	TEST_EXPECT(!project.first_person(R"({"character": 70000})"));
	TEST_EXPECT(!project.first_person(R"({"action": "jump"})"));
	TEST_EXPECT(!project.first_person(R"({"arms": true})"));
	TEST_EXPECT(project.first_person(R"({"action": "RELOAD"})"));
	shown = project.json();
	TEST_EXPECT(shown.get("options")->get("first_person")->get_string("action", "") == "reload");
	return 0;
}

// A map an item pairs (no weapon's view) and a model chosen show no first person.
int test_no_first_person() {
	GunProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.set(R"({"options": {"rig_model": "gun.3di"}})"));
	JsonValue shown;
	const JsonValue *fp = project.first_person_json(shown);
	TEST_EXPECT(fp && fp->is_null() && !project.viewport()->first_person().active());
	TEST_EXPECT(project.set(R"({"options": {"rig_model": ""}})"));
	fp = project.first_person_json(shown);
	TEST_EXPECT(fp && fp->is_object());
	// The eye asked for over a map no weapon pairs shows the orbit.
	TEST_EXPECT(project.set(R"({"options": {"rig_model": "gun.3di", "first_person": {"view": "eye"}}})"));
	project.json();
	TEST_EXPECT(!project.viewport()->eye_view() && !project.viewport()->camera().posed);
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_gun_with_its_arms() == 0);
	TEST_EXPECT(test_actions_and_their_sets() == 0);
	TEST_EXPECT(test_the_eye() == 0);
	TEST_EXPECT(test_no_first_person() == 0);
	std::printf("editor_first_person OK\n");
	return 0;
}
