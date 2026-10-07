// DI-22 (ADR 0046, the deep-integration plan's "a weapon fires in the editor"): a weapon.def record fired in the
// definition preview as the game fires it, through the game's own local player in a range of its own
// (preview/weapon_range) and drawn where the game's presenter draws it (preview/definition_weapon). Headless,
// over minted fixtures alone: the skinned fixture as the gun (its muzzle and shell points added) and as the arms,
// clips made here as the Blender add-on writes them (an .o3a), a bank minted through lwf::encode_lwf, weapon.def,
// ammo.def, Avatars.def and particle text, through a real session whose viewports fake devices follow.
//
// Pinned: a shot in first person (FIRE begins and finishes on one tick, its soundsetend the gunshot heard as the
// player hears their own weapon, its particle at the muzzle point the channel poses, the recoil row's casing at
// its point, the round leaving the eye, its tracer, its stop on the target 25 metres on playing the ammo's row for
// the face, the row's effect at the stop, its sound heard from there, a scar on the face); another surface plays
// another row; a held trigger on an auto weapon (the re-queue chain, one round each FIRE, its release); the
// reload refused on a full clip and run after a shot (its legs, the clip refilled); the scope refused on an
// unscoped weapon; the switch (SWITCHFROM, then SWITCHTO); the run a function of its gestures (the clock stepped
// back runs it again to the same events); third person, as a soldier's shot shows (the ammo's ai_launch and
// ai_launcheffect) and as another player's (FIRE's sets and particle at the gun's point); the eye's camera and its
// refusals; the options' wire and refusals; the commands and the envelope.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/definition_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/preview/weapon_range.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/lwf.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/ammo_table.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }

// --- the fixtures ----------------------------------------------------------------------------------------------

// A clip of the skinned fixture's three bones, `frames` frames at 30 a second; the fire clip turns the middle bone
// a quarter on its last frame, so a point on it moves with the clip.
std::string clip_text(const std::string &name, int frames, bool loop, bool turn) {
	std::string out = "clip " + name + "\nfps 30\nflags " + (loop ? "0x1" : "0x0") + "\nframes " + std::to_string(frames) + "\n";
	const char *bones[] = {"bone -1 0 0 0 0.5 \"BN01 Pelvis\"", "bone 0 0 0 1 0.5 \"BN02 Spine\"",
	                       "bone 0 0 0 -1 0.5 \"BN03 Leg\""};
	for (size_t b = 0; b < 3; ++b) {
		out += std::string(bones[b]) + "\n";
		for (int i = 0; i <= frames; ++i) out += turn && b == 1 && i >= 1 ? " k 0 0 0.3826834 0.9238795\n" : " k 0 0 0 1\n";
	}
	for (int i = 0; i <= frames; ++i) out += "event 0 0 0 0x0 0.9 1.7\n";
	return out;
}

// GUN.adm: its reset, the idle (a loop), the fire and the reload (one-shots), the holster and the draw.
std::string gun_clips() {
	return "o3a 1\nadm GUN.adm\nrow anim_reset \"rest\"\nrow anim_wpn_idle \"idle\"\nrow anim_wpn_fire \"fire\"\n"
	       "row anim_wpn_reload \"reload\"\nrow anim_wpn_switchfrom \"holster\"\nrow anim_wpn_switchto \"draw\"\n" +
	       clip_text("rest", 1, true, false) + clip_text("idle", 8, true, false) + clip_text("fire", 8, false, true) +
	       clip_text("reload", 16, false, false) + clip_text("holster", 8, false, false) + clip_text("draw", 8, false, false);
}

// The gun: the skinned fixture with a muzzle point on its middle part and a shell point on its root.
std::string gun_model() {
	std::string text = test_io::read_file_text(repo() + "/fixtures/threedi/o3d/skinned.o3d");
	text += "userpoint \"muzzle\" 0 1 0.5 0 1 0 1 83\nuserpoint \"shell\" 0.25 0 0 1 0 0 0 83\n";
	return text;
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

const char *kSets[] = {"GS_TEST", "GS_AI", "RL_TEST", "RL_END", "IMP_DIRT", "IMP_METAL", "ST_TEST", "SF_TEST"};

// The weapon: an auto rifle of ten rounds firing AMMO_TEST, FIRE's flash at its muzzle, RECOIL's casing at its
// shell point, a reload, the holster and the draw.
std::string weapons() {
	return "weapon \"WPN_TEST\"\n\tclipsize 10\n\tstartrounds 50\n\tanimadm gun\n\tgfx1 gun\n\tgfx3 gun\n"
	       "\tround_type AMMO_TEST\n\tlaunchuserpoint muzzle\n\tflags auto\n"
	       "\tpos 25 -5 -145 0 0 0\n\ttpos -1 12 -138 0 0 0\n"
	       "\taction \"idle\"\n\t\tanim anim_wpn_idle\n\t\tdelayend 30\n\t\tfunction wpn_std_idle\n\tend\n"
	       "\taction \"fire\"\n\t\tanim anim_wpn_fire\n\t\tsoundsetend GS_TEST\n\t\tdelayend 4\n\t\tparticle Flash\n"
	       "\t\tparticleuserpoint muzzle\n\t\tfunction wpn_std_fire\n\tend\n"
	       "\taction \"recoil\"\n\t\tparticle Shell\n\t\tparticleuserpoint shell\n\t\tfunction wpn_std_recoil\n\tend\n"
	       "\taction \"reload\"\n\t\tanim anim_wpn_reload\n\t\tsoundset RL_TEST\n\t\tsoundsetend RL_END\n\t\tdelaystart 20\n"
	       "\t\tfunction wpn_std_reload\n\tend\n"
	       "\taction \"switchto\"\n\t\tanim anim_wpn_switchto\n\t\tsoundset ST_TEST\n\t\tdelayend 2\n"
	       "\t\tfunction wpn_std_switchto\n\tend\n"
	       "\taction \"switchfrom\"\n\t\tanim anim_wpn_switchfrom\n\t\tsoundset SF_TEST\n\t\tdelayend 2\n"
	       "\t\tfunction wpn_std_switchfrom\n\tend\nend\n"
	       // A scoped rifle on the same map: its sight raised and lowered, its shots at a settled scope.
	       "weapon \"WPN_SCOPED\"\n\tclipsize 5\n\tstartrounds 10\n\tanimadm gun\n\tgfx1 gun\n\tgfx3 gun\n"
	       "\tround_type AMMO_TEST\n\tflags scoped\n\tscope_max_mag 4 0\n"
	       "\tpos 25 -5 -145 0 0 0\n\ttpos -1 12 -138 0 0 0\n"
	       "\taction \"fire\"\n\t\tanim anim_wpn_fire\n\t\tsoundsetend GS_TEST\n\t\tdelayend 4\n\t\tparticle Flash\n"
	       "\t\tparticleuserpoint muzzle\n\t\tfunction wpn_std_fire\n\tend\nend\n";
}

// The round: 600 m/s, every one a tracer, a ring scar, a dirt row and a metal row, a soldier's launch.
const char *kAmmo = "ammo AT_NULL\nend\n"
                    "ammo AMMO_TEST\n\tvelocity 600\n\tmax_age 3\n\tweight_in_grains 62\n\ttracerrate 1\n\ttracer_type 1 2\n"
                    "\tscar_type 1\n\teffects_table\n\t\tobj Hit IMP_DIRT 15\n\t\tdirt Hit IMP_DIRT 15\n"
                    "\t\tmetal Spark IMP_METAL 15\n\tend\n\tai_launch GS_AI\n\tai_launcheffect Flash\nend\n";

const char *kAvatars =
		"define head H1\n{\n\tname AV_H\n\tgraphic gun.3di\n\tcamo 0 0 0\n\tvoice 1\n\tsex m\n}\n"
		"define body B1\n{\n\tname AV_B\n\tgraphic gun.3di\n\tcamo 0 0 0\n}\n"
		"define arms A1\n{\n\tname AV_A\n\tgraphic arms.3di\n\tcamo 1 2 3\n}\n"
		"nationality 0 AV_GOOD\n{\n\talignment good\n\tdivision 0 AV_DIV\n\t{\n\t\tcombo 1 H1 B1 A1\n\t}\n}\n";

std::string particle_text(const std::string &id) {
	return "[particledef]\n{\n\tid = " + id + ";\n\temit_dur = 0.2;\n\temit_rate = 30;\n\temit_burst = 1;\n\tage = 0.5;\n"
	       "\tscale = 0.5;\n\tspeed = 2.0;\n\tspread = 30.0;\n}\n\n";
}

std::string particles() {
	std::string out;
	for (const char *id : {"Flash", "Shell", "Hit", "Spark"})
		out += particle_text(std::string("p_") + id) + "[effectdef]\n{\n\tid = " + id + ";\n\tpdefs = p_" + id + ";\n}\n\n";
	return out;
}

struct WeaponProject {
	editor_test::TempProjectDir dir{"opennova_editor_weapon_fire"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	std::string root;
	std::string path;
	bool made = false;

	WeaponProject() {
		session.handle(request::new_project(dir.file("project"), "Weapon Fire"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const std::string source = dir.file("source");
		bool ok = editor_test::write_text(source + "/gun.o3d", gun_model()) &&
		          editor_test::write_text(source + "/arms.o3d", gun_model()) &&
		          editor_test::write_text(source + "/gun.o3a", gun_clips());
		EditorRequest import = request::of(EditorRequestKind::ImportFiles);
		import.imports = {{source + "/gun.o3d", {}}, {source + "/arms.o3d", {}}, {source + "/gun.o3a", {}}};
		session.handle(import);
		session.run_operations();
		ok = ok && editor_test::write_bytes(root + "/sounds/game.lwf", bank_of(std::vector<std::string>(std::begin(kSets), std::end(kSets))));
		const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
		for (const char *set : kSets) ok = ok && editor_test::write_bytes(root + "/sounds/" + strutil::to_lower(set) + ".wav", tone);
		ok = ok && editor_test::write_text(root + "/" + at("Avatars.def"), editor_test::crlf(kAvatars)) &&
		     editor_test::write_text(root + "/" + at("weapon.def"), editor_test::crlf(weapons())) &&
		     editor_test::write_text(root + "/" + at("ammo.def"), editor_test::crlf(kAmmo)) &&
		     editor_test::write_text(root + "/particles/fire.ptl", particles());
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		made = ok && select("WPN_TEST");
	}
	std::string at(const std::string &name) {
		const AssetScan *scan = session.view().project.scan.get();
		const AssetEntry *entry = scan ? scan->find(name) : nullptr;
		return entry ? entry->relative_path : "defs/" + name;
	}
	bool select(const std::string &name) {
		const std::string file = at("weapon.def");
		session.handle(request::open_document(file));
		const DocumentBase *opened = session.document_for(file);
		const Document *table = opened ? records_of(*opened) : nullptr;
		if (!table) return false;
		path = table->path();
		for (const auto &row : table->rows())
			if (row && strutil::iequals(row->name(), name)) {
				session.handle(request::select_record(path, NodeAddress{row->id, 0, 0}));
				devices.sync(session);
				return session.view().documents.previews[ViewportKind::Definition].part == row->id;
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
	bool options(const std::string &options) { return set(R"({"options": )" + options + "}"); }
	// The gestures set and the clock held at `ticks`: the run there.
	bool perform(const std::string &gestures, int32_t ticks) {
		return set(R"({"gestures": )" + gestures + R"(, "clock": {"ticks": )" + std::to_string(ticks) + R"(, "playing": false}})");
	}
	bool hold(int32_t ticks) { return set(R"({"clock": {"ticks": )" + std::to_string(ticks) + R"(, "playing": false}})"); }
	// The gestures set, then the clock run from tick 0 to `ticks`: the sounds the viewport fired on the way.
	std::vector<ClipSoundFired> run(const std::string &gestures, int32_t ticks) {
		set(R"({"gestures": )" + gestures + R"(, "clock": {"ticks": 0, "playing": true, "rate": 1}})");
		const uint64_t before = session.viewports().clip_sound_seq();
		std::vector<ClipSoundFired> out;
		while (session.viewports().clock().ticks() < ticks) {
			session.advance(0.5 / 62.5);
			devices.sync(session);
			if (const DefinitionViewport *model = viewport())
				for (const ClipSoundFired &fired : model->sounds_fired())
					if (fired.seq > before &&
					    std::none_of(out.begin(), out.end(), [&](const ClipSoundFired &had) { return had.seq == fired.seq; }))
						out.push_back(fired);
		}
		return out;
	}
	JsonValue json() {
		const DefinitionViewport *model = viewport();
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	const WeaponRange *range() {
		const DefinitionViewport *model = viewport();
		return model && model->weapon_record() ? &model->weapon().range() : nullptr;
	}
};

using Kind = WeaponRangeEvent::Kind;

std::vector<WeaponRangeEvent> events_of(const WeaponRange &range, Kind kind, const std::string &action = std::string()) {
	std::vector<WeaponRangeEvent> out;
	for (const WeaponRangeEvent &event : range.events())
		if (event.kind == kind && (action.empty() || event.action == action)) out.push_back(event);
	return out;
}

std::vector<DefinitionSpawn> spawns_of(const DefinitionViewport &viewport, const std::string &source) {
	std::vector<DefinitionSpawn> out;
	for (const DefinitionSpawn &spawn : viewport.effects().spawns())
		if (spawn.source == source) out.push_back(spawn);
	return out;
}

float distance(const PreviewVec3 &a, const PreviewVec3 &b) {
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// --- the tests -------------------------------------------------------------------------------------------------

// One shot in first person: FIRE's legs, the muzzle and the casing where the gun is posed, the round, its tracer,
// its stop on the target, the row's effect, sound and scar.
int test_a_shot_in_first_person() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.options(R"({"weapon": "first"})"));
	TEST_EXPECT(project.perform(R"([{"tick": 0, "gesture": "fire"}])", 40));
	const DefinitionViewport *viewport = project.viewport();
	const WeaponRange *range = project.range();
	TEST_EXPECT(viewport && range && range->ready());
	if (!viewport || !range || !range->ready()) return 1;
	TEST_EXPECT(range->ammo() == "AMMO_TEST");
	// FIRE begins and finishes on one tick: its soundsetend is the gunshot; one round leaves.
	const std::vector<WeaponRangeEvent> begins = events_of(*range, Kind::Begin, "fire");
	const std::vector<WeaponRangeEvent> ends = events_of(*range, Kind::End, "fire");
	const std::vector<WeaponRangeEvent> fired = events_of(*range, Kind::Fired);
	TEST_EXPECT(begins.size() == 1 && ends.size() == 1 && fired.size() == 1);
	if (begins.size() != 1 || ends.size() != 1 || fired.size() != 1) return 1;
	TEST_EXPECT(begins[0].tick == ends[0].tick && fired[0].tick == begins[0].tick);
	TEST_EXPECT(ends[0].set == "GS_TEST");
	TEST_EXPECT(begins[0].effect == "Flash" && begins[0].point == "muzzle" && begins[0].admitted);
	TEST_EXPECT(begins[0].clip == "anim_wpn_fire");
	TEST_EXPECT(fired[0].tracer);
	TEST_EXPECT(range->shots() == 1);
	TEST_EXPECT(range->weapon_view().clip == 9);
	// The recoil row's casing at its point.
	const std::vector<WeaponRangeEvent> casings = events_of(*range, Kind::Effect, "recoil");
	TEST_EXPECT(casings.size() == 1 && casings[0].effect == "Shell" && casings[0].point == "shell");
	// The round stops on the target 25 metres on (600 m/s: under 10 metres a tick, its third tick in flight), the
	// face's dirt row: its effect at the stop, its sound readied there, a scar on the face.
	const std::vector<WeaponRangeEvent> impacts = events_of(*range, Kind::Impact);
	TEST_EXPECT(impacts.size() == 1);
	if (impacts.size() != 1) return 1;
	TEST_EXPECT(impacts[0].tag == 5 && impacts[0].effect == "Hit" && impacts[0].set == "IMP_DIRT");
	TEST_EXPECT(std::fabs(impacts[0].at.x - 25.0f) < 0.05f);
	TEST_EXPECT(impacts[0].tick > fired[0].tick && impacts[0].tick <= fired[0].tick + 4);
	const std::vector<WeaponRangeEvent> sounds = events_of(*range, Kind::Sound);
	TEST_EXPECT(sounds.size() == 1 && sounds[0].set == "IMP_DIRT" && sounds[0].tick == impacts[0].tick);
	TEST_EXPECT(range->scar_count() == 1);
	const renderer::ScarDrawList scars = viewport->weapon().scars();
	TEST_EXPECT(!scars.batches.empty() && scars.vertices.size() == 6);
	// In the preview: the muzzle where the channel poses the gun's point, the casing at the shell point, the impact
	// at the target's face, 25 metres down the eye's line.
	const std::vector<DefinitionSpawn> muzzle = spawns_of(*viewport, "begin");
	const std::vector<DefinitionSpawn> casing = spawns_of(*viewport, "direct");
	const std::vector<DefinitionSpawn> hit = spawns_of(*viewport, "impact");
	TEST_EXPECT(muzzle.size() == 1 && casing.size() == 1 && hit.size() == 1);
	if (muzzle.size() != 1 || hit.size() != 1) return 1;
	TEST_EXPECT(muzzle[0].effect == "Flash" && muzzle[0].tick == begins[0].tick && muzzle[0].point == "muzzle");
	TEST_EXPECT(hit[0].effect == "Hit" && hit[0].point == "dirt");
	const PreviewVec3 eye = viewport->weapon().eye_camera().pose.eye;
	const PreviewVec3 at{hit[0].pose.position.x, hit[0].pose.position.y, hit[0].pose.position.z};
	TEST_EXPECT(std::fabs(distance(eye, at) - 25.0f) < 0.1f);
	// An entity's stop takes the round's flight direction as its orientation (a terrain's none): along the eye's
	// line.
	const PreviewVec3 back = viewport->weapon().eye_camera().pose.back;
	TEST_EXPECT(-(hit[0].pose.forward.x * back.x + hit[0].pose.forward.y * back.y + hit[0].pose.forward.z * back.z) > 0.99f);
	TEST_EXPECT(viewport->effects().scene() != nullptr);
	// A tracer's trail as the round flies and drains after its stop: a point a tick from its first move on (the
	// first pre-move point never draws), its ring kept until it ages out.
	size_t most = 0;
	for (int32_t tick = fired[0].tick; tick < fired[0].tick + 20; ++tick) {
		TEST_EXPECT(project.hold(tick));
		for (const DefinitionTrail &trail : project.viewport()->weapon().trails()) most = std::max(most, trail.points.size());
	}
	TEST_EXPECT(most >= 2);
	return 0;
}

// The eye: the camera the game stands the view model before; the orbit moves only in the orbit's view.
int test_the_eye() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.options(R"({"weapon": "first"})"));
	const DefinitionViewport *viewport = project.viewport();
	TEST_EXPECT(viewport && viewport->weapon_record());
	if (!viewport) return 1;
	TEST_EXPECT(viewport->camera().posed);
	TEST_EXPECT(std::fabs(viewport->camera().pose.fov_degrees - 80.0f) < 1e-3f);
	TEST_EXPECT(!project.refusal(R"({"camera": {"yaw": 1.0}})").empty());
	editor_test::Gathered out;
	std::string error;
	const ViewportContext context = viewport_context(project.session.view(), *viewport);
	TEST_EXPECT(!viewport->command(context, "frame", {}, out, error) && !error.empty());
	TEST_EXPECT(project.options(R"({"fire": {"view": "orbit"}})"));
	viewport = project.viewport();
	TEST_EXPECT(viewport && !viewport->camera().posed);
	TEST_EXPECT(project.set(R"({"camera": {"yaw": 1.0}})"));
	return 0;
}

// Another surface plays another row; no target, no stop.
int test_surfaces() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.options(R"({"weapon": "first", "fire": {"surface": "metal", "range": 10}})"));
	TEST_EXPECT(project.perform(R"([{"tick": 0, "gesture": "fire"}])", 40));
	const WeaponRange *range = project.range();
	TEST_EXPECT(range != nullptr);
	if (!range) return 1;
	const std::vector<WeaponRangeEvent> impacts = events_of(*range, Kind::Impact);
	TEST_EXPECT(impacts.size() == 1 && impacts[0].tag == world::impact_effect_tag_index("metal") &&
	            impacts[0].effect == "Spark" && impacts[0].set == "IMP_METAL");
	TEST_EXPECT(impacts.size() == 1 && std::fabs(impacts[0].at.x - 10.0f) < 0.05f);
	TEST_EXPECT(project.options(R"({"fire": {"target": false}})"));
	TEST_EXPECT(project.hold(40));
	TEST_EXPECT(events_of(*project.range(), Kind::Impact).empty());
	TEST_EXPECT(events_of(*project.range(), Kind::Fired).size() == 1);
	TEST_EXPECT(project.range()->scar_count() == 0);
	return 0;
}

// A held trigger on an auto weapon: the re-queue chain fires until the release; the reload refused while the clip
// is full and run after the shots; the scope refused; the switch holsters and draws again.
int test_hold_reload_scope_switch() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.options(R"({"weapon": "first"})"));
	TEST_EXPECT(project.perform(R"([{"tick": 0, "gesture": "reload"}, {"tick": 2, "gesture": "hold"},
	                               {"tick": 40, "gesture": "release"}, {"tick": 80, "gesture": "reload"},
	                               {"tick": 400, "gesture": "scope"}, {"tick": 420, "gesture": "switch"}])",
	                            600));
	const WeaponRange *range = project.range();
	TEST_EXPECT(range != nullptr);
	if (!range) return 1;
	const std::vector<WeaponRangeEvent> refused = events_of(*range, Kind::Refused);
	TEST_EXPECT(refused.size() == 2);
	if (refused.size() == 2) {
		TEST_EXPECT(refused[0].tick == 0 && refused[0].action == "reload");
		TEST_EXPECT(refused[1].tick == 400 && refused[1].action == "scope");
	}
	const std::vector<WeaponRangeEvent> fired = events_of(*range, Kind::Fired);
	TEST_EXPECT(fired.size() > 2 && fired.size() <= 10);
	for (const WeaponRangeEvent &round : fired) TEST_EXPECT(round.tick >= 2 && round.tick <= 41);
	const std::vector<WeaponRangeEvent> reloads = events_of(*range, Kind::Begin, "reload");
	TEST_EXPECT(reloads.size() == 1 && reloads[0].tick >= 80 && reloads[0].set == "RL_TEST");
	TEST_EXPECT(events_of(*range, Kind::End, "reload").size() == 1);
	TEST_EXPECT(range->weapon_view().clip == 10);
	TEST_EXPECT(range->weapon_view().reserve == 50 - int32_t(fired.size()));
	const std::vector<WeaponRangeEvent> holster = events_of(*range, Kind::Begin, "switchfrom");
	const std::vector<WeaponRangeEvent> draw = events_of(*range, Kind::Begin, "switchto");
	TEST_EXPECT(holster.size() == 1 && draw.size() == 1);
	if (holster.size() == 1 && draw.size() == 1) {
		TEST_EXPECT(holster[0].tick >= 420 && draw[0].tick > holster[0].tick);
		TEST_EXPECT(holster[0].set == "SF_TEST" && draw[0].set == "ST_TEST");
	}
	return 0;
}

// A scoped weapon: the scope raised, the game's ease promoting it, the SIGHTS card up in the view model's place
// (the gun and the arms not drawn), a shot at the settled scope showing no muzzle flash; lowered, the gun again.
int test_scope() {
	WeaponProject project;
	TEST_EXPECT(project.made && project.select("WPN_SCOPED"));
	TEST_EXPECT(project.options(R"({"weapon": "first"})"));
	TEST_EXPECT(project.perform(R"([{"tick": 2, "gesture": "scope"}, {"tick": 60, "gesture": "fire"},
	                               {"tick": 120, "gesture": "scope"}])",
	                            100));
	const WeaponRange *range = project.range();
	TEST_EXPECT(range != nullptr);
	if (!range) return 1;
	TEST_EXPECT(events_of(*range, Kind::Refused).empty());
	TEST_EXPECT(range->scoped());
	TEST_EXPECT(project.viewport()->weapon().card_up());
	const std::vector<WeaponRangeEvent> begins = events_of(*range, Kind::Begin, "fire");
	TEST_EXPECT(begins.size() == 1 && begins[0].effect == "Flash" && !begins[0].admitted);
	TEST_EXPECT(spawns_of(*project.viewport(), "begin").empty());
	TEST_EXPECT(project.hold(200));
	TEST_EXPECT(!project.range()->scoped() && !project.viewport()->weapon().card_up());
	return 0;
}

// The run is a function of its gestures: stepped back, it runs again to the same events; a gesture added on the
// clock's tick drops those after it.
int test_the_run_again() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.options(R"({"weapon": "first"})"));
	TEST_EXPECT(project.perform(R"([{"tick": 0, "gesture": "hold"}, {"tick": 30, "gesture": "release"}])", 60));
	const WeaponRange *range = project.range();
	TEST_EXPECT(range != nullptr);
	if (!range) return 1;
	const std::vector<WeaponRangeEvent> first = range->events();
	const uint64_t runs = range->runs();
	TEST_EXPECT(project.hold(10));
	TEST_EXPECT(project.hold(60));
	range = project.range();
	TEST_EXPECT(range->runs() > runs);
	TEST_EXPECT(range->events().size() == first.size());
	for (size_t i = 0; i < first.size() && i < range->events().size(); ++i) {
		TEST_EXPECT(range->events()[i].tick == first[i].tick && range->events()[i].kind == first[i].kind);
		TEST_EXPECT(std::fabs(range->events()[i].at.x - first[i].at.x) < 1e-5f);
	}
	// A gesture on tick 20 (the clock there): the release on 30 is gone, the hold runs on.
	TEST_EXPECT(project.hold(20));
	TEST_EXPECT(project.set(R"({"gesture": "fire"})"));
	range = project.range();
	TEST_EXPECT(range->gestures().size() == 2 && range->gestures().back().tick == 20);
	return 0;
}

// The sounds as the clock runs: the gunshot as the player hears their own weapon, the impact's row from the stop.
int test_the_sounds() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.options(R"({"weapon": "first"})"));
	const std::vector<ClipSoundFired> sounds = project.run(R"([{"tick": 2, "gesture": "fire"}])", 40);
	bool shot = false, impact = false;
	for (const ClipSoundFired &fired : sounds) {
		if (fired.set == "GS_TEST") {
			shot = true;
			TEST_EXPECT(fired.action == "fire" && fired.leg == "end" && fired.state == "played");
			TEST_EXPECT(!fired.voices.empty() && !fired.voices[0].path.empty());
		}
		if (fired.set == "IMP_DIRT") {
			impact = true;
			TEST_EXPECT(fired.leg.empty() && fired.state == "played");
		}
	}
	TEST_EXPECT(shot && impact);
	// Muted: they fire and say so; nothing reaches the Shell.
	TEST_EXPECT(project.options(R"({"mute": true})"));
	for (const ClipSoundFired &fired : project.run(R"([{"tick": 2, "gesture": "fire"}])", 40))
		TEST_EXPECT(fired.state == "muted");
	return 0;
}

// Third person: the gfx3, the shots as another sees them.
int test_third_person() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(project.options(R"({"weapon": "third"})"));
	TEST_EXPECT(project.perform(R"([{"tick": 0, "gesture": "fire"}])", 40));
	const DefinitionViewport *viewport = project.viewport();
	const WeaponRange *range = project.range();
	TEST_EXPECT(viewport && range);
	if (!viewport || !range) return 1;
	TEST_EXPECT(!viewport->camera().posed);
	TEST_EXPECT(range->shot_view() == WeaponShotView::Soldier);
	// A soldier's shot: the ammo's ai_launcheffect at the fire origin, its ai_launch heard; no own legs shown.
	const std::vector<WeaponRangeEvent> launch = events_of(*range, Kind::Launch);
	TEST_EXPECT(launch.size() == 1 && launch[0].effect == "Flash" && launch[0].point.empty());
	bool ai = false;
	for (const WeaponRangeEvent &sound : events_of(*range, Kind::Sound)) ai = ai || sound.set == "GS_AI";
	TEST_EXPECT(ai);
	TEST_EXPECT(spawns_of(*viewport, "begin").empty() && spawns_of(*viewport, "launch").size() == 1);
	// The round leaves the gun's launch point (launchuserpoint muzzle), the effect there.
	const std::vector<DefinitionSpawn> spawned = spawns_of(*viewport, "launch");
	const PreviewVec3 origin{spawned[0].pose.position.x, spawned[0].pose.position.y, spawned[0].pose.position.z};
	PreviewVec3 muzzle;
	bool found = false;
	const assets::Model &gun = viewport->model();
	for (size_t i = 0; gun && i < gun->user_point_count; ++i)
		if (strutil::iequals(strutil::fixed_string(gun->user_points[i].name, sizeof(gun->user_points[i].name)), "muzzle")) {
			float at[3];
			threedi::threedi_user_point_position(&gun->user_points[i], at);
			muzzle = preview_from_model(at);
			found = true;
		}
	TEST_EXPECT(found && distance(origin, muzzle) < 1e-3f);
	TEST_EXPECT(distance(viewport->weapon().to_preview(world::Vec3{}), muzzle) < 1e-3f);
	// Another player's shot: FIRE's particle at the gun's point, its sets heard at the shooter.
	TEST_EXPECT(project.options(R"({"fire": {"shooter": "player"}})"));
	TEST_EXPECT(project.hold(40));
	range = project.range();
	const std::vector<WeaponRangeEvent> adm = events_of(*range, Kind::Launch);
	TEST_EXPECT(adm.size() == 1 && adm[0].effect == "Flash" && adm[0].point == "muzzle");
	bool gunshot = false;
	for (const WeaponRangeEvent &sound : events_of(*range, Kind::Sound)) gunshot = gunshot || sound.set == "GS_TEST";
	TEST_EXPECT(gunshot);
	return 0;
}

// The wire: the options, the gestures and the commands; the envelope; the refusals.
int test_wire() {
	WeaponProject project;
	TEST_EXPECT(project.made);
	if (!project.made) return 1;
	TEST_EXPECT(!project.refusal(R"({"options": {"fire": {"surface": "move"}}})").empty());
	TEST_EXPECT(!project.refusal(R"({"options": {"fire": {"range": 1}}})").empty());
	TEST_EXPECT(!project.refusal(R"({"options": {"fire": {"shooter": "own"}}})").empty());
	TEST_EXPECT(!project.refusal(R"({"options": {"fire": {"aim": 1}}})").empty());
	TEST_EXPECT(!project.refusal(R"({"gesture": "jump"})").empty());
	TEST_EXPECT(!project.refusal(R"({"gestures": [{"tick": -1, "gesture": "fire"}]})").empty());
	TEST_EXPECT(project.options(R"({"weapon": "first", "fire": {"surface": "wood", "range": 30, "character": -1}})"));
	TEST_EXPECT(project.perform(R"([{"tick": 0, "gesture": "fire"}])", 40));
	const JsonValue shown = project.json();
	const JsonValue *options = shown.get("options");
	const JsonValue *fire = options ? options->get("fire") : nullptr;
	TEST_EXPECT(fire && fire->get("surface") && fire->get("surface")->string == "wood");
	TEST_EXPECT(fire && fire->get("range") && fire->get("range")->number == 30.0);
	const JsonValue *body = shown.get("body");
	const JsonValue *weapon = body ? body->get("weapon") : nullptr;
	TEST_EXPECT(weapon != nullptr);
	if (!weapon) return 1;
	TEST_EXPECT(weapon->get("view")->string == "first" && weapon->get("eye")->boolean);
	TEST_EXPECT(weapon->get("ammo")->string == "AMMO_TEST");
	TEST_EXPECT(weapon->get("shots")->number == 1.0);
	TEST_EXPECT(weapon->get("scars")->number == 1.0);
	TEST_EXPECT(weapon->get("state")->get("clip")->number == 9.0);
	TEST_EXPECT(weapon->get("range")->get("target")->get("surface")->string == "wood");
	TEST_EXPECT(weapon->get("first_person")->get("arms_file")->string == "arms.3di");
	TEST_EXPECT(weapon->get("gestures")->array.size() == 1);
	bool impact = false;
	for (const JsonValue &event : weapon->get("events")->array)
		impact = impact || (event.get("kind")->string == "impact" && event.get("row")->string == "wood");
	TEST_EXPECT(impact);
	// The commands: a gesture on the clock's tick, the clock run; clear; a gesture on another kind refused.
	const DefinitionViewport *viewport = project.viewport();
	editor_test::Gathered reload;
	std::string error;
	const ViewportContext context = viewport_context(project.session.view(), *viewport);
	TEST_EXPECT(viewport->command(context, "reload", {}, reload, error) && reload.requests.size() == 1);
	TEST_EXPECT(editor_test::serve(project.session, reload.requests));
	TEST_EXPECT(project.session.viewports().clock().playing());
	TEST_EXPECT(project.range()->gestures().size() == 2);
	editor_test::Gathered cleared;
	TEST_EXPECT(project.viewport()->command(context, "clear", {}, cleared, error) && cleared.requests.size() == 1);
	TEST_EXPECT(editor_test::serve(project.session, cleared.requests));
	TEST_EXPECT(project.range()->gestures().empty());
	editor_test::Gathered none;
	TEST_EXPECT(!project.viewport()->command(context, "aim", {}, none, error));
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_a_shot_in_first_person() == 0);
	TEST_EXPECT(test_the_eye() == 0);
	TEST_EXPECT(test_surfaces() == 0);
	TEST_EXPECT(test_hold_reload_scope_switch() == 0);
	TEST_EXPECT(test_the_run_again() == 0);
	TEST_EXPECT(test_scope() == 0);
	TEST_EXPECT(test_the_sounds() == 0);
	TEST_EXPECT(test_third_person() == 0);
	TEST_EXPECT(test_wire() == 0);
	std::printf("editor_weapon_fire OK\n");
	return 0;
}
