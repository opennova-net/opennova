// A mission's items' effects as its start attaches them (DI-31, editor/preview/mission_effects): over a real
// session, a project holding a particle file, an item catalog whose rows author particle slots and a minted
// mission placing them, each placed entity's slot is what the game's start makes of it (the pool's attrib gate,
// a drivable item's waiting on a driver, an organic never walked, a graphic with no model), each attached slot's
// emitters at its model's points of the name along their directions (else once at its origin), under the
// entity's transform as the placement draws it (the item's scale with it); the scene plays on the preview clock,
// a jump spawning every slot again pre-aged; a move carries a slot's groups with its entity (no reopen), a slot
// removed lets go of its groups, one added spawns, another item swaps its slot, a particle file edited opens the
// scene again; the layer off closes it; the wire names each entity's slot and counts the scene's.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_effects.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/mission/placement_traits.h>
#include <runtime/particle/effect_scene.h>

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

using synth3di::Box;
using threedi::ThreediBuildVec3;

constexpr const char *kMission = "missions/fires.bms";
constexpr ThreediBuildVec3 kUp{ 0.0, 0.0, 1.0 };
constexpr ThreediBuildVec3 kForward{ 1.0, 0.0, 0.0 };

// The pump: one box, two Smoke points (one forward, one up; the name in either case) and its ground.
std::vector<uint8_t> pump_bytes() {
	synth3di::Model m;
	m.name = "fxpump";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "fxpump.tga");
	const int part = m.add_part(lod, 0, ThreediBuildVec3{});
	m.add_box(lod, part, paint, Box{ { -0.5, -0.5, 0.0 }, { 0.5, 0.5, 1.0 } });
	m.add_panm(lod, part, 0);
	m.add_user_point("Smoke", ThreediBuildVec3{ 0.4, 0.0, 1.0 }, kForward, 0, synth3di::kUserPointEffect);
	m.add_user_point("smoke", ThreediBuildVec3{ -0.4, 0.2, 1.0 }, kUp, 0, synth3di::kUserPointEffect);
	m.add_user_point("ground", ThreediBuildVec3{}, kUp, 0, synth3di::kUserPointGameplay);
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

// A smoke that emits for a long while, and a fire.
std::string particles(float smoke_rate = 10.0f) {
	return particle_text("p_smoke", 60.0f, smoke_rate, 2.0f) + particle_text("p_fire", 60.0f, 8.0f, 1.0f) +
			effect_text("Effect_Smoke", "p_smoke") + effect_text("Effect_Fire", "p_fire");
}

// The items: a pump (its Smoke points), a stack among the buildings naming no point of its model (once at the
// origin), a drivable car (its slot waits for a driver), a pack the powerup gate turns away, a ghost of no model,
// a pump scaled twice, a person (the organics are never walked), a fire.
const char *kItems =
		"begin \"Smoke pump\"\nid 100700\ntype object\ngraphic fxpump\nparticlefx Effect_Smoke Smoke\nend\n"
		"begin \"Smoke stack\"\nid 100701\ntype building\ngraphic fxpump\nparticlefx Effect_Smoke nowhere\nend\n"
		"begin \"Smoke car\"\nid 100702\ntype vehicle\ngraphic fxpump\nattrib: playercontrol\nparticlefx Effect_Smoke Smoke\nend\n"
		"begin \"Smoke pack\"\nid 100703\ntype object\ngraphic fxpump\nattrib: powerup\nparticlefx Effect_Smoke Smoke\nend\n"
		"begin \"Smoke ghost\"\nid 100704\ntype object\ngraphic nomodel\nparticlefx Effect_Smoke Smoke\nend\n"
		"begin \"Scaled pump\"\nid 100705\ntype object\ngraphic fxpump\nscale 2.0\nparticlefx Effect_Smoke Smoke\nend\n"
		"begin \"Smoke person\"\nid 100706\ntype person\ngraphic fxpump\nparticlefx Effect_Smoke Smoke\nend\n"
		"begin \"Fire pump\"\nid 100707\ntype object\ngraphic fxpump\nparticlefx Effect_Fire Smoke\nend\n";

struct Placed {
	mission::EntityKind kind;
	int item;
	float x, y, z;
	int yaw;
};
// In the file's order within each pool.
const Placed kPlaced[] = {
	{ mission::EntityKind::Item, 100700, 10.0f, 20.0f, 1.0f, 90 },
	{ mission::EntityKind::Item, 100702, -10.0f, 5.0f, 0.0f, 0 },
	{ mission::EntityKind::Item, 100703, 0.0f, -15.0f, 0.0f, 0 },
	{ mission::EntityKind::Item, 100704, 5.0f, 5.0f, 0.0f, 0 },
	{ mission::EntityKind::Item, 100705, -30.0f, 40.0f, 2.0f, 45 },
	{ mission::EntityKind::Building, 100701, 50.0f, -50.0f, 3.0f, 180 },
	{ mission::EntityKind::Organic, 100706, 0.0f, 0.0f, 0.0f, 0 },
};

std::vector<uint8_t> fires_mission() {
	bms::File mission;
	mission::make_default(mission);
	for (const Placed &placed : kPlaced) {
		mission::EntityTransform at;
		at.x = placed.x;
		at.y = placed.y;
		at.z = placed.z;
		at.yaw = placed.yaw;
		mission::add_entity(mission, placed.kind, placed.item, at);
	}
	mission::sync_counts(mission);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bms::write(mission, bytes, error)) bytes.clear();
	return bytes;
}

Edit set_of(const NodeAddress &record, const char *field, Value value) {
	Edit edit;
	edit.address = record;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

bool near(float a, float b, float within = 1e-3f) {
	return std::fabs(a - b) <= within;
}

struct Rig {
	editor_test::TempProjectDir dir{ "opennova_editor_mission_effects" };
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	editor_test::FakeDevices devices;
	std::string root;
	std::string path;

	bool open() {
		session.handle(request::new_project(dir.file("project"), "Fires"));
		session.run_operations();
		editor_test::create_missing_files(session);
		root = session.view().project.root;
		if (!editor_test::write_bytes(root + "/" + kMission, fires_mission()) ||
				!editor_test::write_text(root + "/defs/items.def", kItems) ||
				!editor_test::write_bytes(root + "/models/fxpump.3di", pump_bytes()) ||
				!editor_test::write_text(root + "/particles/fx.ptl", particles()))
			return false;
		editor_test::handle_to_end(session, request::rescan());
		while (session.view().activity.validation.running) session.poll();
		session.handle(request::open_document(kMission));
		if (!session.outcome().done()) return false;
		path = session.document_for(kMission)->path();
		devices.sync(session);
		return true;
	}
	const MissionViewport *viewport() {
		return static_cast<const MissionViewport *>(session.viewports().follow_one(session.view(), path, ViewportKind::Mission));
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		devices.sync(session);
		return session.outcome().done();
	}
	// The clock held at `ticks` (a seek), or run on to it a frame at a time.
	bool hold(int32_t ticks) { return set(R"({"clock": {"ticks": )" + std::to_string(ticks) + R"(, "playing": false}})"); }
	void run_to(int32_t ticks) {
		set(R"({"clock": {"playing": true, "rate": 1}})");
		while (session.viewports().clock().ticks() < ticks) {
			session.advance(0.5 / 62.5);
			devices.sync(session);
		}
		set(R"({"clock": {"playing": false}})");
	}
	const MissionDocument &document() {
		return static_cast<const MissionDocument &>(*records_of(*session.document_for(kMission)));
	}
	std::vector<NodeAddress> rows(MissionKind kind) {
		std::vector<NodeAddress> out;
		for (const Node *row : document().rows_of(kind)) out.push_back({ row->id, row->kind, 0 });
		return out;
	}
	JsonValue json() {
		const MissionViewport *model = viewport();
		return model ? viewport_to_json(session.view(), *model, JsonPage()) : JsonValue();
	}
};

// The pose an attached group of row `row` stands at in the scene (its first live group's).
bool group_pose(const MissionEffects &effects, NodeId row, particle::EffectPose &out) {
	if (!effects.scene()) return false;
	const particle::EffectDebugSnapshot snapshot = effects.scene()->inspect(false);
	for (const particle::EffectGroupDebugSnapshot &group : snapshot.groups)
		if (group.owner.value == uint64_t(row) && !group.detached) {
			out = group.pose;
			return true;
		}
	return false;
}

size_t live_groups_of(const MissionEffects &effects, NodeId row) {
	if (!effects.scene()) return 0;
	size_t count = 0;
	for (const particle::EffectGroupDebugSnapshot &group : effects.scene()->inspect(false).groups)
		count += group.owner.value == uint64_t(row) && !group.detached ? 1 : 0;
	return count;
}

// Where a model point of the pump stands under an entity at (x, y, z) with `yaw`, scaled `scale`: the
// presentation frame's transform (bms_to_presentation_*) over the point as ObjectData's godot_vec3 reads it.
particle::Vec3 expected_point(float x, float y, float z, int yaw, float scale, const ThreediBuildVec3 &model_point) {
	const mission::PlacementVec3 at = mission::bms_to_presentation_position(mission::PlacementVec3{ x, y, z });
	const mission::PlacementBasis basis = mission::bms_to_presentation_basis(0.0f, float(yaw), 0.0f);
	// The model's raw (forward, left, up) to its Godot model space: (-left, up, forward) mirrored on x.
	const float local[3] = { float(model_point.y), float(model_point.z), float(model_point.x) };
	return particle::Vec3{ at.x + scale * (basis.x.x * local[0] + basis.y.x * local[1] + basis.z.x * local[2]),
		at.y + scale * (basis.x.y * local[0] + basis.y.y * local[1] + basis.z.y * local[2]),
		at.z + scale * (basis.x.z * local[0] + basis.y.z * local[1] + basis.z.z * local[2]) };
}

} // namespace

static int test_slots_as_the_start_attaches_them() {
	Rig rig;
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->view_status() == MissionViewStatus::Ready);
	if (!viewport) return 1;
	const MissionEffects &effects = viewport->effects();
	const std::vector<NodeAddress> items = rig.rows(MissionKind::Item);
	const std::vector<NodeAddress> buildings = rig.rows(MissionKind::Building);
	const std::vector<NodeAddress> organics = rig.rows(MissionKind::Organic);
	TEST_EXPECT(items.size() == 5 && buildings.size() == 1 && organics.size() == 1);
	// The start's walk: the items, then the buildings; never the organics.
	TEST_EXPECT(effects.slots().size() == 6);
	TEST_EXPECT(effects.slot(organics[0].row) == nullptr);
	const MissionEffectSlot *pump = effects.slot(items[0].row);
	TEST_EXPECT(pump && std::string(pump->status) == "attached" && pump->effect == "Effect_Smoke" && pump->point == "Smoke");
	TEST_EXPECT(pump && pump->points.size() == 2 && pump->points[0] == "Smoke" && pump->points[1] == "smoke");
	TEST_EXPECT(std::string(effects.slot(items[1].row)->status) == "controller");
	TEST_EXPECT(std::string(effects.slot(items[2].row)->status) == "gated");
	TEST_EXPECT(std::string(effects.slot(items[3].row)->status) == "no_model");
	TEST_EXPECT(std::string(effects.slot(items[4].row)->status) == "attached");
	// A name no point of the model has: once at the origin.
	const MissionEffectSlot *stack = effects.slot(buildings[0].row);
	TEST_EXPECT(stack && std::string(stack->status) == "attached" && stack->points.size() == 1 && stack->points[0].empty());
	TEST_EXPECT(effects.slots().front().row == items[0].row && effects.slots().back().row == buildings[0].row);

	// Played on the clock: two emitters at the pump, two at the scaled pump, one at the stack.
	rig.run_to(30);
	TEST_EXPECT(effects.scene() && effects.opens() == 1);
	TEST_EXPECT(live_groups_of(effects, items[0].row) == 2 && live_groups_of(effects, items[4].row) == 2 &&
			live_groups_of(effects, buildings[0].row) == 1 && live_groups_of(effects, items[1].row) == 0);
	TEST_EXPECT(effects.alive(items[0].row) == 2 && effects.slot(items[0].row)->spawned == 2);
	TEST_EXPECT(effects.scene()->live_counts().particle_count > 0);
	// Each emitter where the game's attached spawn stands it: the point under the entity's transform, the item's
	// scale with it.
	particle::EffectPose pose;
	TEST_EXPECT(group_pose(effects, items[0].row, pose));
	const particle::Vec3 smoke = expected_point(10.0f, 20.0f, 1.0f, 90, 1.0f, ThreediBuildVec3{ 0.4, 0.0, 1.0 });
	TEST_EXPECT(near(pose.position.x, smoke.x) && near(pose.position.y, smoke.y) && near(pose.position.z, smoke.z));
	TEST_EXPECT(group_pose(effects, items[4].row, pose));
	const particle::Vec3 scaled = expected_point(-30.0f, 40.0f, 2.0f, 45, 2.0f, ThreediBuildVec3{ 0.4, 0.0, 1.0 });
	TEST_EXPECT(near(pose.position.x, scaled.x) && near(pose.position.y, scaled.y) && near(pose.position.z, scaled.z));
	TEST_EXPECT(group_pose(effects, buildings[0].row, pose));
	const particle::Vec3 origin = expected_point(50.0f, -50.0f, 3.0f, 180, 1.0f, ThreediBuildVec3{});
	TEST_EXPECT(near(pose.position.x, origin.x) && near(pose.position.y, origin.y) && near(pose.position.z, origin.z));

	// A jump of the clock (a seek back): every slot spawned again, pre-aged by its age, the scene not opened again.
	const uint64_t spawns = effects.spawns();
	TEST_EXPECT(rig.hold(5));
	rig.viewport();
	TEST_EXPECT(effects.opens() == 1 && effects.spawns() == spawns + 5 && effects.tick() == 5);
	TEST_EXPECT(effects.scene()->live_counts().particle_count > 0);

	// The wire: each entity's slot among the items, the scene's counts in the body.
	const JsonValue envelope = rig.json();
	const JsonValue *body = envelope.get("body");
	const JsonValue *counts = body ? body->get("effects") : nullptr;
	TEST_EXPECT(counts && counts->get_number("slots", -1) == 6.0 && counts->get_number("attached", -1) == 3.0 &&
			counts->get_number("controller", -1) == 1.0 && counts->get_number("gated", -1) == 1.0 &&
			counts->get_number("no_model", -1) == 1.0 && counts->get_number("emitters", -1) == 5.0);
	const JsonValue *named = counts ? counts->get("effects") : nullptr;
	TEST_EXPECT(named && named->array.size() == 1 && named->array[0].get_string("effect", "") == "Effect_Smoke" &&
			named->array[0].get_string("defined_in", "") == "particles/fx.ptl" && named->array[0].get_bool("spawns", false));
	size_t carried = 0;
	if (const JsonValue *marks = envelope.get("items"))
		for (const JsonValue &mark : marks->array)
			if (const JsonValue *effect = mark.get("effect")) {
				++carried;
				if (mark.get_number("id", 0) == double(items[0].row))
					TEST_EXPECT(effect->get_string("status", "") == "attached" && effect->get_number("spawned", -1) == 2.0);
			}
	TEST_EXPECT(carried == 6);
	return 0;
}

static int test_slots_follow_the_mission_live() {
	Rig rig;
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	if (!viewport) return 1;
	const MissionEffects &effects = viewport->effects();
	const std::vector<NodeAddress> items = rig.rows(MissionKind::Item);
	rig.run_to(20);
	TEST_EXPECT(effects.opens() == 1);
	const uint64_t spawns = effects.spawns();

	// A move: the pump's groups carried with it (the engine's owner pose), nothing opened or spawned again.
	particle::EffectPose before, after;
	TEST_EXPECT(group_pose(effects, items[0].row, before));
	rig.session.handle(request::edit_record(kMission, set_of(items[0], "x", 25.0)));
	rig.devices.sync(rig.session);
	rig.viewport();
	TEST_EXPECT(group_pose(effects, items[0].row, after));
	TEST_EXPECT(near(after.position.x - before.position.x, 15.0f) && near(after.position.y, before.position.y) &&
			near(after.position.z, before.position.z));
	TEST_EXPECT(effects.opens() == 1 && effects.spawns() == spawns);

	// Another item: its slot let go (its groups detached, draining) and the new one's spawned now.
	rig.session.handle(request::edit_record(kMission, set_of(items[0], "item", int64_t(100707))));
	rig.devices.sync(rig.session);
	rig.viewport();
	TEST_EXPECT(effects.slot(items[0].row) && effects.slot(items[0].row)->effect == "Effect_Fire");
	// The fire is an effect the scene lacked: it opened again over both, every slot spawned at its age next play.
	TEST_EXPECT(effects.opens() == 2);
	rig.run_to(rig.session.viewports().clock().ticks() + 2);
	TEST_EXPECT(live_groups_of(effects, items[0].row) == 2 && live_groups_of(effects, items[4].row) == 2);

	// A removal: the slot gone, its groups let go.
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = items[4];
	rig.session.handle(request::edit_record(kMission, remove));
	rig.devices.sync(rig.session);
	rig.viewport();
	TEST_EXPECT(effects.slot(items[4].row) == nullptr && live_groups_of(effects, items[4].row) == 0);
	TEST_EXPECT(effects.opens() == 2);

	// The particle file edited: the scene opened again over the new closures.
	TEST_EXPECT(editor_test::write_text(rig.root + "/particles/fx.ptl", particles(20.0f)));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.devices.sync(rig.session);
	rig.viewport();
	TEST_EXPECT(effects.opens() == 3);
	rig.run_to(rig.session.viewports().clock().ticks() + 2);
	TEST_EXPECT(live_groups_of(effects, items[0].row) == 2);

	// The layer off: nothing held, the body's effects null; on again, played anew.
	TEST_EXPECT(rig.set(R"({"options": {"show": {"effects": false}}})"));
	rig.viewport();
	TEST_EXPECT(!effects.scene() && effects.slots().empty());
	const JsonValue off = rig.json();
	TEST_EXPECT(off.get("body") && off.get("body")->get("effects") && off.get("body")->get("effects")->is_null());
	TEST_EXPECT(rig.set(R"({"options": {"show": {"effects": true}}})"));
	rig.run_to(rig.session.viewports().clock().ticks() + 2);
	TEST_EXPECT(effects.scene() && live_groups_of(effects, items[0].row) == 2);
	return 0;
}

static int test_options_wire() {
	MissionViewportOptions options;
	TEST_EXPECT(options.foliage && options.effects && options.lights);
	std::string error;
	JsonValue change;
	TEST_EXPECT(io::json_parse(R"({"show": {"foliage": false, "lights": false}})", change, error));
	TEST_EXPECT(mission_options_from_json(change, options, error));
	TEST_EXPECT(!options.foliage && options.effects && !options.lights);
	const JsonValue wire = mission_options_to_json(options);
	const JsonValue *show = wire.get("show");
	TEST_EXPECT(show && !show->get_bool("foliage", true) && show->get_bool("effects", false) && !show->get_bool("lights", true));
	TEST_EXPECT(io::json_parse(R"({"show": {"grass": true}})", change, error));
	TEST_EXPECT(!mission_options_from_json(change, options, error) && error.find("foliage, effects, lights") != std::string::npos);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_options_wire();
	failures += test_slots_as_the_start_attaches_them();
	failures += test_slots_follow_the_mission_live();
	return failures == 0 ? 0 : 1;
}
