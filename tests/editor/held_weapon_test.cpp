// A person's held weapon in the model preview (ADR 0046 DI-13's follow-up;
// editor/preview/preview_held_weapon): a row of a person's map plays with the weapon the body would
// hold in it, the game's weapon table read from weapon.def, the weapon reaching the row through the
// game's own selectors, drawn by the present row's gate and attach and hung from bone 16. Headless
// (no device follows the viewport: the session's follow does), over minted fixtures alone: the
// synthetic 19-bone person (fixtures/threedi/synth/person.3di, bone 16 the weapon hand) with the
// rig19 clips (fixtures/anim/rig19_*.bad) under a map written here, the synthetic gun
// (fixtures/threedi/synth/gun.3di) as two third-person models, items.def and weapon.def text.
//
// Pinned: Auto's weapon for each kind of row (the knife's and the grenade's hold rows, their
// attacks, the reloads by clip and hold kind, the binoculars, a rifle mirroring a body row, a hold
// row no weapon reaches, a death row drawing none); the frame (the hand for the 0x80 rows, else the
// body's, the attach triple at rest the preview's identity); where it hangs (bone 16's joint nudged
// by the hand, following the clip); a gfx3 the project lacks drawing nothing; an NPC's map holding
// none unless one is picked; the options' wire, none, a weapon named and one weapon.def lacks, and
// their refusals; the weapon's record a Go to into weapon.def; the model read again only as
// weapon.def moves.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_held_weapon.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/infantry.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() {
	return test_paths_repo_root(__FILE__);
}

// The soldier's map: the rig19 clips under the rows a held weapon plays with.
const char *kMap = "anim_reset \"rig19_rst\"\n"
				   "anim_idle \"rig19_idle\"\n"
				   "anim_walk_forward \"rig19_walk\"\n"
				   "anim_knife \"rig19_idle\"\n"
				   "anim_grenade \"rig19_twist\"\n"
				   "anim_stinger \"rig19_idle\"\n"
				   "anim_knife_attack \"rig19_rld1\"\n"
				   "anim_grenade_attack \"rig19_rld2\"\n"
				   "anim_binoculars \"rig19_idle\"\n"
				   "anim_reload \"rig19_rld1\"\n"
				   "anim_reload2 \"rig19_rld2\"\n"
				   "anim_death_bullet_torso_forward \"rig19_dthf\"\n";

const char *kClips[] = { "rig19_rst", "rig19_idle", "rig19_walk", "rig19_twist", "rig19_rld1",
	"rig19_rld2", "rig19_dthf" };

// The weapons, in the game's file order: a rifle (no hold kind: it mirrors the body), a pistol
// (hold 2), a knife (hold 1, attack 1, no clip) and a frag (hold 3, attack 2) whose gfx3 the
// project lacks.
const char *kWeapons =
		"weapon \"WPN_RIFLE\"\n\tclipsize 30\n\tgfx3 rifle_3\nend\n\n"
		"weapon \"WPN_PISTOL\"\n\tclipsize 15\n\tgfx3 rifle_3\n\tspecial_hold 2\nend\n\n"
		"weapon \"WPN_KNIFE\"\n\tclipsize -1\n\tgfx3 knife_3\n\tspecial_hold 1\n\tattack_anim "
		"1\nend\n\n"
		"weapon \"WPN_FRAG\"\n\tclipsize 1\n\tgfx3 frag_3\n\tspecial_hold 3\n\tattack_anim "
		"2\nend\n";

std::string items(const std::string &ai_function, const std::string &move_function) {
	return test_io::crlf("begin \"Soldier\"\nid 105305\ntype person\ngraphic person\nanim_def "
						 "soldier\nai_function " +
			ai_function + "\nmove_function " + move_function + "\nend\n");
}

// A project of the person, its map and clips, the two third-person models, items.def and
// weapon.def; the map open with the row `key` selected.
struct PersonProject {
	test_temp::TempDir dir{ "opennova_editor_held_weapon" };
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	std::string root;
	std::string path = "anims/soldier.adm";
	bool made = false;

	explicit PersonProject(
			const std::string &ai_function = "plyr", const std::string &move_function = "org2") {
		session.handle(request::new_project(dir.file("project"), "Held Weapon"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		const std::vector<uint8_t> gun =
				test_io::read_file(repo() + "/fixtures/threedi/synth/gun.3di");
		bool ok = test_io::write_file(root + "/models/person.3di",
						  test_io::read_file(repo() + "/fixtures/threedi/synth/person.3di")) &&
				test_io::write_file(root + "/models/rifle_3.3di", gun) &&
				test_io::write_file(root + "/models/knife_3.3di", gun) &&
				test_io::write_text(root + "/" + path, test_io::crlf(kMap));
		for (const char *clip : kClips)
			ok = ok &&
					test_io::write_file(root + "/anims/" + clip + ".bad",
							test_io::read_file(repo() + "/fixtures/anim/" + clip + ".bad"));
		ok = ok &&
				test_io::write_text(
						root + "/" + at("items.def"), items(ai_function, move_function));
		made = ok && write_weapons(kWeapons) && select("anim_knife");
	}

	// Where the project keeps a file of the name (a blank Create Missing made), else under defs/.
	std::string at(const std::string &name) {
		const AssetScan *scan = session.view().project.scan.get();
		const AssetEntry *entry = scan ? scan->find(name) : nullptr;
		return entry ? entry->relative_path : "defs/" + name;
	}
	bool write_weapons(const std::string &text) {
		if (!test_io::write_text(root + "/" + at("weapon.def"), test_io::crlf(text)))
			return false;
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running)
			session.poll();
		return true;
	}
	bool select(const char *key) {
		session.handle(request::open_document(path));
		Document *table = session.document_for(path);
		NodeAddress row;
		if (!table || !find_definition(AssetGraph(), *table, key, row))
			return false;
		session.handle(request::select_record(path, row));
		return session.view().documents.previews[ViewportKind::Model].path == path;
	}
	const ModelViewport *viewport() {
		return dynamic_cast<const ModelViewport *>(
				session.viewports().follow_one(session.view(), path, ViewportKind::Model));
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		return session.outcome().done();
	}
	bool held(const std::string &options) {
		return set(R"({"options": {"held": )" + options + "}}");
	}
	bool hold(int32_t ticks) {
		return set(R"({"clock": {"ticks": )" + std::to_string(ticks) + R"(, "playing": false}})");
	}
	JsonValue json() {
		ViewportModel *model =
				session.viewports().follow_one(session.view(), path, ViewportKind::Model);
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
	// The envelope's `held` (null where none has a say).
	JsonValue held_json() {
		const JsonValue shown = json();
		const JsonValue *body = shown.get("body");
		const JsonValue *animation = body ? body->get("animation") : nullptr;
		const JsonValue *held = animation ? animation->get("held") : nullptr;
		return held ? *held : JsonValue();
	}
};

double length(const PreviewVec3 &v) {
	return std::sqrt(double(dot(v, v)));
}
bool near(const PreviewVec3 &a, const PreviewVec3 &b, double within = 1e-4) {
	return std::fabs(a.x - b.x) < within && std::fabs(a.y - b.y) < within &&
			std::fabs(a.z - b.z) < within;
}

// Auto's weapon for each kind of row, how it reaches the row, and the frame it hangs in.
int test_auto_by_row() {
	PersonProject project;
	TEST_EXPECT(project.made);
	if (!project.made)
		return 1;
	struct Row {
		const char *key;
		const char *weapon;
		const char *route;
		bool hand;
		bool drawn;
	};
	const Row rows[] = {
		{ "anim_knife", "WPN_KNIFE", "hold", true, true },
		{ "anim_knife_attack", "WPN_KNIFE", "attack", true, true },
		{ "anim_grenade", "WPN_FRAG", "hold", true, false }, // its gfx3 is no model of the project
		{ "anim_grenade_attack", "WPN_FRAG", "attack", true, false },
		{ "anim_walk_forward", "WPN_RIFLE", "mirror", false,
				true }, // a rifle's hold mirrors the body
		{ "anim_idle", "WPN_RIFLE", "mirror", false, true },
		{ "anim_reload", "WPN_RIFLE", "reload", true,
				true }, // the first with a clip, not a pistol's hold
		{ "anim_reload2", "WPN_PISTOL", "reload", true, true }, // a pistol's reload
		{ "anim_binoculars", "WPN_RIFLE", "binoculars", true, true },
	};
	for (const Row &row : rows) {
		TEST_EXPECT(project.select(row.key));
		const JsonValue held = project.held_json();
		std::printf("held_weapon: %s -> %s (%s, %s) %s\n", row.key,
				held.get_string("weapon", "").c_str(), held.get_string("route", "").c_str(),
				held.get_string("frame", "").c_str(), held.get_string("words", "").c_str());
		TEST_EXPECT(held.is_object() && held.get_string("weapon", "") == row.weapon &&
				held.get_string("route", "") == row.route && held.get_bool("player", false) &&
				!held.get_bool("named", true));
		TEST_EXPECT(held.get_string("frame", "") == (row.hand ? "hand" : "entity"));
		TEST_EXPECT(held.get_bool("drawn", !row.drawn) == row.drawn);
		TEST_EXPECT(held.get_number("bone", -1) == world::kHeldWeaponBoneIndex);
		const ModelViewport *model = project.viewport();
		TEST_EXPECT(model && model->held_weapon().active() &&
				(model->held_weapon().model() != nullptr) == row.drawn);
		if (row.drawn) {
			TEST_EXPECT(held.get("position") && held.get("position")->is_array() &&
					held.get("screen") && held.get("screen")->is_array());
			TEST_EXPECT(held.get_string("point", "").rfind("BN17", 0) == 0);
		}
	}
	// The knife's model and record: its gfx3 the project's knife_3.3di, its record a Go to into
	// weapon.def.
	TEST_EXPECT(project.select("anim_knife"));
	JsonValue held = project.held_json();
	TEST_EXPECT(held.get_string("gfx3", "") == "knife_3" &&
			held.get_string("model", "") == "knife_3.3di" &&
			held.get_string("model_path", "") == "models/knife_3.3di" &&
			held.get_number("adm", 0) == 3 && held.get_number("hold", 0) == 1 &&
			held.get_number("attack", 0) == 1 && held.get_number("state", 0) == 50);
	const JsonValue *record = held.get("record");
	TEST_EXPECT(record && record->is_object() &&
			record->get_string("file", "") == project.at("weapon.def") &&
			!record->get_string("locator", "").empty());
	// The choices: every weapon of weapon.def by its ADM index, each with how it reaches the row.
	const JsonValue *choices = held.get("choices");
	TEST_EXPECT(choices && choices->array.size() == 4 &&
			choices->array[0].get_string("weapon", "") == "WPN_RIFLE" &&
			choices->array[0].get_number("adm", 0) == 1 &&
			choices->array[0].get_string("route", "") == "none" &&
			choices->array[2].get_string("route", "") == "hold");
	// The frag's gfx3 the project lacks: nothing draws, and the words say why.
	TEST_EXPECT(project.select("anim_grenade"));
	held = project.held_json();
	TEST_EXPECT(held.get_string("model", "x").empty() &&
			held.get_string("words", "").find("no model") != std::string::npos);
	// A hold row no weapon reaches: none, and the note says to pick one.
	TEST_EXPECT(project.select("anim_stinger"));
	held = project.held_json();
	TEST_EXPECT(held.get_string("weapon", "x").empty() && !held.get_bool("drawn", true) &&
			held.get_string("words", "")
							.rfind("No weapon of weapon.def plays row 53 (anim_stinger)", 0) == 0 &&
			!held.get_string("note", "").empty());
	// A death row: a dead body draws none (the present row's gate), whatever weapon.
	TEST_EXPECT(project.select("anim_death_bullet_torso_forward"));
	held = project.held_json();
	TEST_EXPECT(held.get_string("weapon", "") == "WPN_RIFLE" && !held.get_bool("drawn", true) &&
			held.get_string("words", "").find("dead body") != std::string::npos);
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && !model->held_weapon().model());
	return 0;
}

// Where it hangs: bone 16's joint, the nudge carried by the hand; the body's frame at rest the
// preview's own axes (the present row's triple, mission yaw 180), the hand's turned with the bone;
// and it follows the hand as the clip plays.
int test_where_it_hangs() {
	PersonProject project;
	TEST_EXPECT(project.made);
	if (!project.made)
		return 1;
	// The rifle on the walk: the entity frame, the triple at rest.
	TEST_EXPECT(project.select("anim_walk_forward") && project.hold(0));
	const ModelViewport *model = project.viewport();
	TEST_EXPECT(model && model->held_weapon().model() && !model->held_weapon().hand_frame());
	if (!model)
		return 1;
	const float *attach = model->held_weapon().attach_deg();
	TEST_EXPECT(std::fabs(attach[0]) < 1e-3 && std::fabs(std::fabs(attach[1]) - 180.0) < 1e-3 &&
			std::fabs(attach[2]) < 1e-3);
	std::vector<PreviewJoint> joints = model->joints(project.session.viewports().clock());
	TEST_EXPECT(joints.size() > size_t(world::kHeldWeaponBoneIndex));
	HeldWeaponPlace place;
	TEST_EXPECT(model->held_weapon().place(joints, place));
	TEST_EXPECT(near(place.muzzle, PreviewVec3{ 0.0f, 0.0f, 1.0f }) &&
			near(place.up, PreviewVec3{ 0.0f, 1.0f, 0.0f }));
	const PreviewJoint &hand = joints[size_t(world::kHeldWeaponBoneIndex)];
	const PreviewVec3 nudge = preview_joint_carry(hand,
			PreviewVec3{ world::kHeldWeaponAttachNudgeX, world::kHeldWeaponAttachNudgeY,
					world::kHeldWeaponAttachNudgeZ },
			true);
	TEST_EXPECT(near(place.at,
			PreviewVec3{ hand.at.x + nudge.x, hand.at.y + nudge.y, hand.at.z + nudge.z }));
	TEST_EXPECT(std::fabs(length(PreviewVec3{ place.at.x - hand.at.x, place.at.y - hand.at.y,
								  place.at.z - hand.at.z }) -
						std::sqrt(0.05 * 0.05 * 2.0 + 0.051 * 0.051)) < 1e-4);
	// The envelope says the same place.
	JsonValue held = project.held_json();
	const JsonValue *position = held.get("position");
	TEST_EXPECT(position && position->is_array() && position->array.size() == 3 &&
			position->array[0].number == double(place.at.x) &&
			position->array[1].number == double(place.at.y) &&
			position->array[2].number == double(place.at.z));
	// The knife: the hand frame, the muzzle the hand's turn of the calibration, unit and square to
	// its up.
	TEST_EXPECT(project.select("anim_knife") && project.hold(0));
	model = project.viewport();
	TEST_EXPECT(model && model->held_weapon().hand_frame());
	if (!model)
		return 1;
	joints = model->joints(project.session.viewports().clock());
	TEST_EXPECT(model->held_weapon().place(joints, place));
	const PreviewJoint &knife_hand = joints[size_t(world::kHeldWeaponBoneIndex)];
	const double z = world::kHeldWeaponHandFrameZRad, y = world::kHeldWeaponHandFrameYRad;
	const PreviewVec3 muzzle = preview_joint_carry(knife_hand,
			PreviewVec3{ float(std::sin(y) * std::cos(z)), float(std::sin(y) * std::sin(z)),
					float(std::cos(y)) },
			true);
	TEST_EXPECT(near(place.muzzle, muzzle) && std::fabs(length(place.muzzle) - 1.0) < 1e-4 &&
			std::fabs(length(place.up) - 1.0) < 1e-4 &&
			std::fabs(dot(place.muzzle, place.up)) < 1e-4);
	TEST_EXPECT(!near(place.muzzle, PreviewVec3{ 0.0f, 0.0f, 1.0f }, 1e-2));
	// The reload turns the arms: the weapon follows the hand from frame to frame.
	TEST_EXPECT(project.select("anim_reload") && project.hold(0));
	model = project.viewport();
	TEST_EXPECT(model && model->held_weapon().model());
	if (!model)
		return 1;
	HeldWeaponPlace first, later;
	TEST_EXPECT(
			model->held_weapon().place(model->joints(project.session.viewports().clock()), first));
	TEST_EXPECT(project.set(R"({"frame": 10})"));
	model = project.viewport();
	TEST_EXPECT(model &&
			model->held_weapon().place(model->joints(project.session.viewports().clock()), later));
	TEST_EXPECT(!near(first.at, later.at, 1e-3));
	return 0;
}

// The options: none, a weapon named (carried through a body row, its own hold's frame), one
// weapon.def lacks, and the refusals; the wire; the model read again only as weapon.def moves.
int test_options() {
	PersonProject project;
	TEST_EXPECT(project.made);
	if (!project.made)
		return 1;
	const ModelViewport *model = project.viewport();
	const uint64_t reads = model ? model->held_weapon().reads() : 0;
	const uint64_t serial = model ? model->held_weapon().serial() : 0;
	TEST_EXPECT(model && reads == 1);
	// The knife's attack is the knife's model: nothing to make again.
	TEST_EXPECT(project.select("anim_knife_attack"));
	model = project.viewport();
	TEST_EXPECT(model && model->held_weapon().serial() == serial &&
			model->held_weapon().reads() == reads);
	// None.
	TEST_EXPECT(project.held(R"({"weapon": "NONE"})"));
	JsonValue shown = project.json();
	TEST_EXPECT(shown.get("options") && shown.get("options")->get("held") &&
			shown.get("options")->get("held")->get_string("weapon", "") == "none");
	JsonValue held = project.held_json();
	TEST_EXPECT(held.get_string("weapon", "x").empty() && !held.get_bool("drawn", true) &&
			held.get_string("chosen", "") == "none");
	model = project.viewport();
	TEST_EXPECT(model && !model->held_weapon().model() && model->held_weapon().serial() != serial);
	// The knife named on the walk: carried through it, its own hold row on the upper body, so the
	// hand frame.
	TEST_EXPECT(project.select("anim_walk_forward") && project.held(R"({"weapon": "wpn_knife"})"));
	held = project.held_json();
	TEST_EXPECT(held.get_string("weapon", "") == "WPN_KNIFE" && held.get_bool("named", false) &&
			held.get_string("route", "") == "carried" &&
			held.get_number("state", 0) == world::anim_state::kHoldKnife &&
			held.get_string("frame", "") == "hand" && held.get_bool("drawn", false));
	TEST_EXPECT(held.get_string("note", "").find("carried through row 1 (anim_walk_forward)") !=
			std::string::npos);
	// A weapon weapon.def lacks: Auto's shows, and the note says so.
	TEST_EXPECT(project.held(R"({"weapon": "WPN_NOPE"})"));
	held = project.held_json();
	TEST_EXPECT(held.get_string("weapon", "") == "WPN_RIFLE" && !held.get_bool("named", true) &&
			held.get_string("note", "") == "WPN_NOPE is not in weapon.def: WPN_RIFLE shows.");
	// The refusals.
	TEST_EXPECT(!project.held("3"));
	TEST_EXPECT(!project.held(R"({"weapon": 3})"));
	TEST_EXPECT(!project.held(R"({"gun": ""})"));
	TEST_EXPECT(project.held(R"({"weapon": ""})"));
	// weapon.def moves: the table read again, its models with it.
	model = project.viewport();
	const uint64_t before = model ? model->held_weapon().reads() : 0;
	TEST_EXPECT(project.write_weapons(
			std::string(kWeapons) + "\nweapon \"WPN_EXTRA\"\n\tclipsize 5\nend\n"));
	model = project.viewport();
	TEST_EXPECT(model && model->held_weapon().reads() == before + 1 &&
			model->held_weapon().choices().size() == 5);
	return 0;
}

// An NPC's map: its class never sets a held weapon, so Auto draws none; one picked draws.
int test_npc_holds_none() {
	PersonProject project("org1", "org1");
	TEST_EXPECT(project.made);
	if (!project.made)
		return 1;
	JsonValue held = project.held_json();
	TEST_EXPECT(held.is_object() && !held.get_bool("player", true) &&
			held.get_string("weapon", "x").empty() && !held.get_bool("drawn", true) &&
			held.get_string("words", "").find("ai_function org1") != std::string::npos);
	TEST_EXPECT(project.held(R"({"weapon": "WPN_KNIFE"})"));
	held = project.held_json();
	TEST_EXPECT(held.get_string("weapon", "") == "WPN_KNIFE" && held.get_bool("drawn", false));
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_auto_by_row() == 0);
	TEST_EXPECT(test_where_it_hangs() == 0);
	TEST_EXPECT(test_options() == 0);
	TEST_EXPECT(test_npc_holds_none() == 0);
	std::printf("editor_held_weapon OK\n");
	return 0;
}
