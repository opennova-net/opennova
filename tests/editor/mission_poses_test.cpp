// A mission's people posed as the game spawns them (DI-38, editor/preview/mission_poses): over a real
// session, a project holding a person's .adm and clips, an item catalog and a minted mission, each
// organic's pose is the one the world's own organic init leaves the same body in (a World of the
// same records, initialized in pool order over the same clips: the state, the playheads, the served
// ring entries, the blend), with why a person is not posed (an item no catalog defines, another
// class, no .adm, no clips); a definition without `aidata` reads neither the route nor the guard;
// the pose follows the records live (a route edited, an SSN edited: an Update with the poses moved;
// another item's catalog edit), and its wire form names the row, the clip, the playhead and the alert byte; the
// people play on as the world's bodies do, their think of a still body included (S27: an alert person turns to
// idle_2 at its first think, a guard holds guard). And a project of several minted missions over two tables
// sharing their ring heads, every person of each posed and held to the world init's, as the shipped missions are.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_people.h>
#include <editor/preview/mission_poses.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/lwf/lwf.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/anim/rig_files.h>
#include <runtime/assets/asset_store.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/world.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/mission_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
namespace world = opennova::world;

namespace {

using editor_test::FakeDevices;
using editor_test::NoProcess;

constexpr const char *kMission = "missions/people.bms";


// A person's table (CRLF lines, as the game's config reader takes them): the reset and idle_2 rows
// idle, idle a ring of two (idle, then walk), the walk and the guard rows walk.
constexpr const char *kPeopleAdm = "anim_reset\t\"idle\"\r\nanim_idle\t\"idle\" \"walk\"\r\nanim_idle_2\t\"idle\"\r\n"
								   "anim_walk_forward\t\"walk\"\r\nanim_guard\t\"walk\"\r\n";

// The persons: a rifleman (org1, its body on the org1 row, aidata, the table), a civilian (org1, its body on
// the org0 row, the table, no aidata), a player (plyr), a statue (org1, aidata, no table), a pilot (org1,
// aidata, a table the project lacks).
constexpr const char *kItems = "begin \"Spawn Rifleman\"\nid 106200\ntype person\ngraphic shed\nanim_def people\n"
							   "move_function org1\nai_function org1\nattrib: aidata\nend\n"
							   "begin \"Spawn Civilian\"\nid 106201\ntype person\ngraphic shed\nanim_def people\n"
							   "move_function org0\nai_function org1\nend\n"
							   "begin \"Spawn Player\"\nid 106202\ntype person\ngraphic shed\nanim_def people\n"
							   "ai_function plyr\nattrib: aidata\nend\n"
							   "begin \"Spawn Statue\"\nid 106203\ntype person\ngraphic shed\nai_function org1\n"
							   "attrib: aidata\nend\n"
							   "begin \"Spawn Pilot\"\nid 106204\ntype person\ngraphic shed\nanim_def nowhere\n"
							   "ai_function org1\nattrib: aidata\nend\n";

// One placed person: its item, its SSN, its route, its attributes and its alert_state.
struct Placed {
	int item;
	int ssn;
	int route;
	uint32_t attributes;
	uint8_t alert;
};
// In the file's order: posed idle, a route's walk, a red guard, the civilian (its route, guard and alert unread),
// route 126 and a yellow 127 guard, a second rifleman of the ring on red alert, then the four not posed.
const Placed kPeople[] = { { 106200, 13, 0, 0, 0 }, { 106200, 6, 3, 0, 0 }, { 106200, 15, 0, 0x2, 2 },
	{ 106201, 4, 3, 0x2, 2 }, { 106200, 2, 126, 0, 0 }, { 106200, 1, 127, 0x2, 1 }, { 106200, 12, 0, 0, 2 },
	{ 106202, 7, 0, 0, 0 }, { 106203, 8, 0, 0, 0 }, { 106204, 9, 0, 0, 0 }, { 109999, 10, 0, 0, 0 } };

// A mission of `people` (in the file's order).
template <typename People> std::vector<uint8_t> mission_of(const People &people) {
	opennova::bms::File mission;
	opennova::mission::make_default(mission);
	for (const Placed &person : people) {
		opennova::bms::Entity entity =
				opennova::mission::new_entity(opennova::mission::EntityKind::Organic, person.item, person.ssn);
		entity.waypoint_id = uint8_t(person.route);
		entity.bmsi_attributes = person.attributes;
		entity.alert_state = person.alert;
		mission.organics.push_back(entity);
	}
	opennova::mission::sync_counts(mission);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::bms::write(mission, bytes, error)) bytes.clear();
	return bytes;
}
std::vector<uint8_t> people_mission() { return mission_of(kPeople); }

using editor_test::set_edit;

// A session over a project holding the people, their catalog and clips, the mission open and its
// viewport followed through the fake devices.
struct Rig {
	test_temp::TempDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	FakeDevices devices;
	std::string path;

	explicit Rig(const char *name) : dir(name) {}

	bool open() {
		session.handle(request::new_project(dir.file("project"), "People"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const std::string root = session.view().project.root;
		if (!test_io::write_file(root + "/" + kMission, people_mission()) ||
				!test_io::write_text(root + "/defs/items.def", kItems) ||
				!test_io::write_text(root + "/people.adm", kPeopleAdm))
			return false;
		for (const char *clip : { "idle.bad", "walk.bad" })
			if (!test_io::write_file(root + "/" + clip, test_io::read_file(test_paths_fixture(std::string("anim/") + clip))))
				return false;
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document(kMission));
		if (!session.outcome().done()) return false;
		path = session.document_for(kMission)->path();
		devices.sync(session);
		return true;
	}
	const MissionViewport *viewport() {
		return static_cast<const MissionViewport *>(session.viewports().find(path, ViewportKind::Mission));
	}
	void pump() { devices.sync(session); }
	ViewportAction last() { return devices.last(path, ViewportKind::Mission); }
	const MissionDocument &document() {
		return static_cast<const MissionDocument &>(*records_of(*session.document_for(kMission)));
	}
	std::vector<NodeAddress> organics() {
		std::vector<NodeAddress> out;
		for (const Node *row : document().rows_of(MissionKind::Organic)) out.push_back({ row->id, row->kind, 0 });
		return out;
	}
};

// A directory's files as the game's store finds them (assets::asset_file_name), for the world's side.
struct DirRigFiles : opennova::anim::RigFiles {
	std::string root;
	std::shared_ptr<const opennova::adm::AdmFile> animation_map(const std::string &name) const override {
		const std::vector<uint8_t> bytes =
				test_io::read_file(root + "/" + opennova::assets::asset_file_name(name, ".adm"));
		return bytes.empty() ? nullptr : opennova::assets::parse_animation_map(bytes.data(), bytes.size());
	}
	std::shared_ptr<const opennova::bad::BadFile> bone_animation(const std::string &name) const override {
		const std::vector<uint8_t> bytes =
				test_io::read_file(root + "/" + opennova::assets::asset_file_name(name, ".bad"));
		return bytes.empty() ? nullptr : opennova::assets::parse_bone_animation(bytes.data(), bytes.size());
	}
};

// The world's side: each body the game would init, in pool order, through the world's own organic
// init over its own clip registry.
struct WorldPeople {
	world::World world;
	opennova::anim::AdmRootMotion motion;
	std::vector<world::EntityHandle> handles;
	WorldPeople() {
		for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 4096);
		world.ai.root_motion = &motion;
	}
	// A body of `adm` with the record's SSN, route, guard and alert as the spawn folds them; the handle.
	world::EntityHandle add(int adm, int ssn, int route, bool guard, uint8_t alert = 0) {
		world::Entity seed;
		seed.kind = world::EntityKind::Organic;
		seed.item_id = 1;
		seed.has_item_def = true;
		seed.item_type = 3;
		seed.net_id = uint16_t(ssn);
		seed.flags = guard ? 0x40u : 0u;
		const world::EntityHandle handle = world.registry.spawn(0, seed);
		world.ai.attach(handle);
		world::AiEntity &body = *world.ai.for_handle(handle);
		body.inf.active = true;
		body.inf.adm_id = adm;
		body.net_id = ssn;
		if (route != 0) {
			body.slot.f[35] = 1;
			body.slot.f[37] = route;
		}
		body.slot.bytes()[world::AiSlot::kAlertByte] = alert;
		handles.push_back(handle);
		return handle;
	}
	void init() {
		for (const world::EntityHandle handle : handles) world::initialize_organic_ai(world, *world.registry.get(handle));
	}
	world::InfantryBodyPose pose(world::EntityHandle handle) {
		return world::infantry_body_pose(world.ai.for_handle(handle)->inf);
	}
	// How far the init lifted the body from its spawn at z 0 (no collision world: no ground solve), metres.
	double rise(world::EntityHandle handle) { return double(world.ai.for_handle(handle)->self->position.z.q16) / 65536.0; }
};

bool same(const world::InfantryBodyPose &a, const world::InfantryBodyPose &b) {
	return a.state == b.state && a.phase == b.phase && a.variant == b.variant && a.parked == b.parked &&
			a.blending == b.blending && a.source_state == b.source_state && a.source_phase == b.source_phase &&
			a.source_variant == b.source_variant && a.weight == b.weight;
}

} // namespace

static int test_poses() {
	Rig rig("opennova_editor_mission_poses");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready);
	if (!viewport) return 1;
	const MissionPoses &poses = viewport->poses();
	const std::vector<NodeAddress> rows = rig.organics();
	TEST_EXPECT(rows.size() == std::size(kPeople) && poses.poses().size() == rows.size());
	TEST_EXPECT(poses.posed() == 7 && poses.runs() == 1);
	const auto pose_of = [&](size_t i) { return poses.pose(rows[i].row); };
	// Why the last four stand as they are.
	TEST_EXPECT(pose_of(7)->status == "class" && pose_of(7)->ai_function == "plyr");
	TEST_EXPECT(pose_of(8)->status == "no_adm");
	TEST_EXPECT(pose_of(9)->status == "no_clips" && pose_of(9)->adm == "default.adm");
	TEST_EXPECT(pose_of(10)->status == "no_item");
	// The requests and the fields that chose them; the civilian has no AI slot, so neither its route
	// nor its guard reach the init.
	const int states[] = { 43, 1, 140, 43, 44, 43, 43 };
	const char *because[] = { "idle", "route", "guard", "idle", "route_126", "route_127", "idle" };
	for (size_t i = 0; i < 7; ++i) {
		TEST_EXPECT(pose_of(i)->status == "posed" && pose_of(i)->adm == "people.adm");
		TEST_EXPECT(pose_of(i)->state == states[i] && std::string(pose_of(i)->because) == because[i]);
		TEST_EXPECT(pose_of(i)->updates == world::organic_warmup_updates(uint32_t(kPeople[i].ssn)) + 1);
	}
	TEST_EXPECT(pose_of(0)->ai_slot && !pose_of(3)->ai_slot);
	TEST_EXPECT(pose_of(0)->updates == 363 && pose_of(0)->pose.state == 43);
	// What the think reads: the alert byte and the guard through the AI slot (the civilian has none).
	const int alerts[] = { 0, 0, 2, 0, 0, 1, 2 };
	const bool guards[] = { false, false, true, false, false, true, false };
	for (size_t i = 0; i < 7; ++i) {
		TEST_EXPECT(pose_of(i)->alert == alerts[i] && pose_of(i)->guarding == guards[i]);
		TEST_EXPECT(pose_of(i)->ssn == kPeople[i].ssn);
	}

	// Each posed body as the world's own init leaves it, the ring heads shared in pool order.
	{
		WorldPeople people;
		DirRigFiles files;
		files.root = rig.session.view().project.root;
		const int adm = people.motion.register_adm(&files, "people.adm");
		TEST_EXPECT(adm == 0);
		std::vector<world::EntityHandle> bodies;
		for (size_t i = 0; i < 7; ++i) {
			const bool slot = kPeople[i].item == 106200;
			bodies.push_back(people.add(adm, kPeople[i].ssn, slot ? kPeople[i].route : 0,
					slot && (kPeople[i].attributes & 0x2u) != 0));
		}
		people.init();
		for (size_t i = 0; i < 7; ++i) {
			TEST_EXPECT(same(pose_of(i)->pose, people.pose(bodies[i])));
			TEST_EXPECT(pose_of(i)->rise == people.rise(bodies[i]) && pose_of(i)->rise > 0.0);
		}
		// Each clip the file its served ring entry loaded.
		for (size_t i = 0; i < 7; ++i)
			TEST_EXPECT(pose_of(i)->clip == people.motion.clip_file(adm, pose_of(i)->pose.state, pose_of(i)->pose.variant));
		TEST_EXPECT(pose_of(1)->clip == "walk.bad" && pose_of(4)->clip == "idle.bad");
	}

	// No terrain read (the project names none): each stands its rise over its record, unsettled.
	for (size_t i = 0; i < 7; ++i) TEST_EXPECT(pose_of(i)->lift == pose_of(i)->rise && !pose_of(i)->settled);
	// Over a terrain (flat at 0, the records at z 0): the warmup's ground solve stands each origin its
	// last capsule bottom over the ground, as the world's init does over the same terrain.
	{
		std::vector<uint16_t> heights(512 * 512, 0);
		std::vector<int> sectors(256, 1);
		opennova::terrain::TerrainHeightField field;
		field.heightmap = heights.data();
		field.dim = 512;
		field.layout.sector_grid = sectors.data();
		MissionPoses own;
		TEST_EXPECT(own.refresh(rig.session.view(), viewport->scene()));
		MissionPeopleFloor floor;
		floor.terrain = &field;
		TEST_EXPECT(own.stand(viewport->scene(), floor));
		TEST_EXPECT(!own.stand(viewport->scene(), floor)); // stood: nothing moves
		for (size_t i = 0; i < 7; ++i) {
			const MissionPose *stood = own.pose(rows[i].row);
			TEST_EXPECT(stood && stood->settled && stood->lift == double(stood->capsule_bottom) / 65536.0);
			TEST_EXPECT(stood->lift > 0.0);
		}
	}

	// Its wire form: the row, the clip, the playhead.
	{
		const JsonValue json = mission_pose_json(*pose_of(1));
		TEST_EXPECT(json.get_string("status", "") == "posed" && json.get_string("row", "") == "anim_walk_forward");
		TEST_EXPECT(json.get_string("because", "") == "route" && json.get_number("rise", -1) == pose_of(1)->rise);
		const JsonValue *playing = json.get("playing");
		TEST_EXPECT(playing && playing->get_string("clip", "") == "walk.bad" &&
				playing->get_number("phase", -1) == double(pose_of(1)->pose.phase));
		TEST_EXPECT(mission_pose_json(*pose_of(6)).get_number("alert", -1) == 2.0 && json.get_number("alert", -1) == 0.0);
		const JsonValue not_posed = mission_pose_json(*pose_of(10));
		TEST_EXPECT(not_posed.get_string("status", "") == "no_item" && not_posed.get("playing") == nullptr);
		// The viewport's items carry it for each organic, and the body counts the posed.
		const JsonValue envelope = viewport_to_json(rig.session.view(), *viewport, JsonPage());
		const JsonValue *items = envelope.get("items");
		TEST_EXPECT(items && items->is_array());
		size_t carried = 0;
		for (const JsonValue &item : items->array)
			if (const JsonValue *pose = item.get("pose")) carried += pose->get("status") ? 1 : 0;
		TEST_EXPECT(carried == rows.size());
		const JsonValue *body = envelope.get("body");
		TEST_EXPECT(body && body->get_number("posed", -1) == 7.0);
	}

	// Live with the records: a route taken away (an Update, the walk now the idle) and an SSN edited
	// (the warmup's count); a transform posing nothing again.
	{
		const size_t runs = poses.runs();
		rig.session.handle(request::edit_record(kMission, set_edit(rows[0], "x", 12.5)));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(rig.last() == ViewportAction::Update && poses.runs() == runs);
		const uint32_t stamp = pose_of(1)->stamp;
		rig.session.handle(request::edit_record(kMission, set_edit(rows[1], "waypoint_id", int64_t(0))));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(rig.last() == ViewportAction::Update && poses.runs() == runs + 1);
		TEST_EXPECT(pose_of(1)->state == 43 && std::string(pose_of(1)->because) == "idle" && pose_of(1)->stamp != stamp);
		const int32_t phase = pose_of(0)->pose.phase;
		rig.session.handle(request::edit_record(kMission, set_edit(rows[0], "id", int64_t(0))));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(pose_of(0)->updates == 11 && pose_of(0)->pose.phase != phase);
		// An alert_state edit (S27): an Update, the pose's alert byte moved, its stamp with it (the people start again
		// on the poses' serial); the civilian's, whose definition takes no AI slot, moves no pose.
		const uint32_t serial = poses.serial();
		rig.session.handle(request::edit_record(kMission, set_edit(rows[6], "alert_state", int64_t(0))));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(rig.last() == ViewportAction::Update && pose_of(6)->alert == 0 && poses.serial() != serial);
		const uint32_t alerted = poses.serial();
		rig.session.handle(request::edit_record(kMission, set_edit(rows[3], "alert_state", int64_t(1))));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(pose_of(3)->alert == 0 && poses.serial() == alerted);
		// The catalog's table for the rifleman taken away outside the editor: those people stand.
		std::string edited = kItems;
		const std::string table = "anim_def people\nmove_function org1\nai_function org1\nattrib";
		const size_t at = edited.find(table);
		TEST_EXPECT(at != std::string::npos);
		edited.replace(at, table.size(), "move_function org1\nai_function org1\nattrib");
		TEST_EXPECT(test_io::write_text(rig.session.view().project.root + "/defs/items.def", edited));
		rig.session.handle(request::rescan());
		rig.session.run_operations();
		rig.pump();
		TEST_EXPECT(pose_of(0)->status == "no_adm" && pose_of(3)->status == "posed");
	}
	std::printf("test_poses passed\n");
	return 0;
}

// A second person's table, its rows rings of other lengths than the people's: a body of it serves the ring heads
// of its own table, which the world shares among the bodies of each table in pool order.
constexpr const char *kTroopsAdm = "anim_reset\t\"walk\"\r\nanim_idle\t\"walk\" \"idle\" \"walk\"\r\nanim_idle_2\t\"walk\"\r\n"
								   "anim_walk_forward\t\"idle\" \"walk\"\r\nanim_guard\t\"idle\" \"walk\"\r\n";
// A trooper (org1, its body on the org1 row, aidata, the second table) and a militiaman (its body on the org0 row,
// the second table, no aidata), beside the catalog's people.
constexpr const char *kTroopItems = "begin \"Spawn Trooper\"\nid 106205\ntype person\ngraphic shed\nanim_def troops\n"
									"move_function org1\nai_function org1\nattrib: aidata\nend\n"
									"begin \"Spawn Militia\"\nid 106206\ntype person\ngraphic shed\nanim_def troops\n"
									"move_function org0\nai_function org1\nend\n";
// The missions: the two tables' people mixed in pool order, idle, on routes (126 and 127 among them), on guard and
// alert; a garrison of one table's nine, its rings wrapped; a village of the people's and a trooper.
const Placed kPatrol[] = { { 106200, 21, 0, 0, 0 }, { 106205, 22, 3, 0, 0 }, { 106200, 23, 0, 0x2, 2 },
	{ 106205, 24, 0, 0x2, 1 }, { 106205, 25, 126, 0, 0 }, { 106200, 26, 127, 0, 0 }, { 106205, 27, 0, 0, 2 },
	{ 106206, 28, 3, 0x2, 2 } };
const Placed kGarrison[] = { { 106205, 31, 0, 0, 0 }, { 106205, 32, 0, 0, 0 }, { 106205, 33, 5, 0, 0 },
	{ 106205, 34, 0, 0, 0 }, { 106205, 35, 0, 0, 1 }, { 106205, 36, 0, 0x2, 0 }, { 106205, 37, 0, 0, 0 },
	{ 106205, 38, 126, 0, 0 }, { 106205, 39, 0, 0, 2 } };
const Placed kVillage[] = { { 106201, 41, 0, 0, 0 }, { 106200, 42, 4, 0, 0 }, { 106201, 43, 0, 0x2, 0 },
	{ 106200, 44, 0, 0x2, 0 }, { 106200, 45, 0, 0, 1 }, { 106205, 46, 0, 0, 0 } };

// The sweep the shipped missions are held to, over minted ones (JO:CA's 88 base-game missions, measured once over the
// game's catalog and clips: 2,393 people, every one posed, 1,232 idle, 1,100 on a route's walk and 61 on guard, each
// held to the world's init): the three missions above written into one project with both tables, their clips and the
// catalog, each opened in its viewport; every person the game poses is held to the world's own init over the same
// clips (each mission's bodies in pool order, a fresh world a mission); prints the poses by state.
static int test_missions_sweep() {
	Rig rig("opennova_editor_mission_poses_sweep");
	rig.session.handle(request::new_project(rig.dir.file("project"), "People"));
	rig.session.run_operations();
	editor_test::create_missing_files(rig.session);
	const std::string project = rig.session.view().project.root;
	const std::vector<std::pair<std::string, std::vector<uint8_t>>> written = {
		{ "missions/patrol.bms", mission_of(kPatrol) }, { "missions/garrison.bms", mission_of(kGarrison) },
		{ "missions/village.bms", mission_of(kVillage) } };
	std::vector<std::string> missions;
	for (const auto &[path, bytes] : written) {
		TEST_EXPECT(!bytes.empty() && test_io::write_file(project + "/" + path, bytes));
		missions.push_back(path);
	}
	const size_t placed = std::size(kPatrol) + std::size(kGarrison) + std::size(kVillage);
	TEST_EXPECT(test_io::write_text(project + "/defs/items.def", std::string(kItems) + kTroopItems) &&
	            test_io::write_text(project + "/people.adm", kPeopleAdm) && test_io::write_text(project + "/troops.adm", kTroopsAdm));
	for (const char *clip : { "idle.bad", "walk.bad" })
		TEST_EXPECT(test_io::write_file(project + "/" + clip, test_io::read_file(test_paths_fixture(std::string("anim/") + clip))));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	DirRigFiles files;
	files.root = project;
	const auto started = std::chrono::steady_clock::now();
	size_t people = 0, posed = 0, held = 0;
	std::map<int, size_t> by_state;
	for (const std::string &path : missions) {
		rig.session.handle(request::open_document(path));
		TEST_EXPECT(rig.session.outcome().done());
		rig.path = rig.session.document_for(path)->path();
		rig.pump();
		const MissionViewport *viewport = rig.viewport();
		TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready);
		if (!viewport) continue;
		// The world's bodies of this mission, its persons in pool order over the game's own clips.
		WorldPeople world_people;
		std::vector<std::pair<const MissionPose *, world::EntityHandle>> pairs;
		for (const MissionEntityMark &mark : viewport->scene().entities()) {
			if (mark.pool != MissionPool::Organic) continue;
			++people;
			const MissionPose *pose = viewport->poses().pose(mark.row);
			TEST_EXPECT(pose != nullptr);
			if (!pose || pose->status != "posed") continue;
			++posed;
			++by_state[pose->state];
			const int adm = world_people.motion.register_adm(&files, pose->adm);
			TEST_EXPECT(adm >= 0);
			const bool slot = pose->ai_slot;
			pairs.push_back({ pose, world_people.add(adm, mark.ssn, slot ? mark.route : 0,
											  slot && (mark.attributes & 0x2u) != 0) });
		}
		world_people.init();
		for (const auto &pair : pairs) {
			const bool equal = same(pair.first->pose, world_people.pose(pair.second)) &&
					pair.first->rise == world_people.rise(pair.second);
			TEST_EXPECT(equal);
			held += equal ? 1 : 0;
		}
		rig.session.handle(request::close_document(path));
		rig.pump();
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::string states;
	for (const auto &entry : by_state)
		states += (states.empty() ? "" : ", ") + world::infantry_anim_key(entry.first) + " " + std::to_string(entry.second);
	std::printf("sweep: %zu missions, %zu people, %zu posed (%s), %zu held to the world's init, in %.1f s\n",
			missions.size(), people, posed, states.c_str(), held, seconds);
	// Every person placed posed, as every shipped one is, and held; the requests the shipped people make among them.
	TEST_EXPECT(people == placed && posed == people && held == posed);
	TEST_EXPECT(by_state.count(world::anim_state::kIdle) && by_state.count(world::anim_state::kWalkForward) &&
	            by_state.count(world::anim_state::kGuard));
	std::printf("test_missions_sweep passed\n");
	return 0;
}

// S23 C: the people play their clips on the preview clock from their spawn: each tick of the clock a tick of every
// posed body on the org1 physics row by the game's own org1 motor head (world::infantry_org1_motor_head), in pool order
// over the shared ring heads, and on its think ticks the think of a still body (S27, world::infantry_org1_still_think),
// so after each tick every such body is the world's own body after its init and as many ticks of the world's motor
// and its think from the game's first tick, and the event words are the world's: the red rifleman turns to idle_2 at
// its first think, the guards hold guard (the yellow 127 one turns to it), route 126's idle_2 turns to the idle; a body
// on the org0 row (the civilian) stands as its warmup left it, as the game's nullsub leaves it, and a route's walker
// stands at its spawn pose (S26: no think walks it along its route); a clock gone back starts them again from their
// spawn.
static int test_people_play() {
	Rig rig("opennova_editor_mission_people_play");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	if (!viewport) return 1;
	const MissionPeople &playing = viewport->people();
	const std::vector<NodeAddress> rows = rig.organics();
	// The org1 bodies play but a route's walker.
	const auto walker = [&](size_t i) {
		const MissionPose *pose = viewport->poses().pose(rows[i].row);
		return pose && pose->state == world::anim_state::kWalkForward;
	};
	size_t players = 0;
	for (size_t i = 0; i < 7; ++i) players += kPeople[i].item == 106200 && !walker(i) ? 1 : 0;
	TEST_EXPECT(walker(1) && players < 6 && playing.playing() == players);
	WorldPeople people;
	DirRigFiles files;
	files.root = rig.session.view().project.root;
	const int adm = people.motion.register_adm(&files, "people.adm");
	std::vector<world::EntityHandle> bodies;
	for (size_t i = 0; i < 7; ++i) {
		const bool slot = kPeople[i].item == 106200;
		bodies.push_back(people.add(adm, kPeople[i].ssn, slot ? kPeople[i].route : 0,
				slot && (kPeople[i].attributes & 0x2u) != 0, slot ? kPeople[i].alert : 0));
	}
	people.init();
	const int32_t start = playing.tick();
	TEST_EXPECT(playing.started_at() == start);
	size_t world_events = 0, wraps = 0;
	for (int32_t tick = 1; tick <= 240; ++tick) {
		rig.session.advance(1.0 / 62.5);
		rig.pump();
		for (size_t i = 0; i < bodies.size(); ++i) {
			if (kPeople[i].item != 106200) continue; // the civilian's org0 row runs no motor
			world::AiEntity &entity = *people.world.ai.for_handle(bodies[i]);
			const int32_t before = entity.inf.clip_phase;
			// The game's tick: the clock's first past the people's start is its first frame, tick 1.
			people.world.ai.update_organic(entity, people.world, uint32_t(tick));
			if (entity.inf.last_events != 0) ++world_events;
			wraps += entity.inf.clip_phase < before ? 1 : 0;
		}
		const int32_t played = rig.viewport()->people().tick() - start;
		TEST_EXPECT(played == tick);
		if (played != tick) return 1;
		for (size_t i = 0; i < 7; ++i) {
			const world::InfantryBodyPose *pose = rig.viewport()->people().pose(rows[i].row);
			if (kPeople[i].item == 106200 && walker(i)) {
				TEST_EXPECT(!pose);
				continue;
			}
			if (kPeople[i].item != 106200) {
				TEST_EXPECT(!pose);
				TEST_EXPECT(same(viewport->poses().pose(rows[i].row)->pose, people.pose(bodies[i])));
				continue;
			}
			TEST_EXPECT(pose && same(*pose, people.pose(bodies[i])));
		}
	}
	TEST_EXPECT(wraps > 0); // the clips played round
	// What the first thinks made of them.
	const int plays[] = { 43, -1, 140, -1, 43, 140, 44 };
	for (size_t i : { 0, 2, 4, 5, 6 }) {
		const world::InfantryBodyPose *pose = rig.viewport()->people().pose(rows[i].row);
		TEST_EXPECT(pose && pose->state == plays[i] && !pose->blending);
	}
	// The events kept: the last kCatchUpTicks ticks' words, the world's over the same ticks.
	size_t kept = 0;
	for (const MissionPeople::Event &event : rig.viewport()->people().events())
		kept += event.tick > rig.viewport()->people().tick() - MissionPeople::kCatchUpTicks ? 1 : 0;
	TEST_EXPECT(kept == rig.viewport()->people().events().size());
	std::printf("people play: %zu world event words over 240 ticks, %zu kept\n", world_events, kept);
	{
		// The fixture's walk and idle carry the feet's bits.
		std::set<uint32_t> words;
		for (const MissionPeople::Event &event : rig.viewport()->people().events()) words.insert(event.word);
		TEST_EXPECT(words == std::set<uint32_t>({ 0x1u, 0x2u }));
	}
	// A seek back: every person at its spawn again.
	const uint64_t serial = rig.viewport()->people().serial();
	MissionPeople again;
	again.reset(viewport->poses(), 0);
	TEST_EXPECT(again.playing() == players);
	for (size_t i = 0; i < 7; ++i)
		if (kPeople[i].item == 106200 && !walker(i))
			TEST_EXPECT(same(*again.pose(rows[i].row), viewport->poses().pose(rows[i].row)->pose));
	TEST_EXPECT(again.run_to(viewport->poses(), -5) && again.tick() == -5 && again.started_at() == -5);
	TEST_EXPECT(rig.viewport()->people().serial() == serial);
	std::printf("test_people_play passed\n");
	return 0;
}

// S23 C: with Listen on, the people's footsteps are heard as they play their clips: each event word on the body's odd
// tick (an NPC's sound block) plays its foot's slot of the item's sound profile, its set found in the project's bank,
// handed to the Shell as the clip preview's sounds are; with Listen off, nothing.
static int test_people_heard() {
	Rig rig("opennova_editor_mission_people_heard");
	TEST_EXPECT(rig.open());
	const std::string root = rig.session.view().project.root;
	// A bank of the two footsteps (heard to a kilometre), their wave, the profile and the rifleman naming it.
	opennova::lwf::File bank;
	const char *sets[] = { "PFS_GND_L", "PFS_GND_R" };
	for (size_t i = 0; i < 2; ++i) {
		opennova::lwf::Single single;
		single.name = sets[i];
		single.path = std::string(i == 0 ? "pfs_gnd_l" : "pfs_gnd_r") + ".wav";
		bank.singles.push_back(single);
		opennova::lwf::Multi set;
		set.name = sets[i];
		set.pitch_base = opennova::lwf::kAuthoredSetPitchBase;
		set.target_id = 1000;
		set.playlist_indices.push_back(uint32_t(i));
		bank.multis.push_back(set);
		opennova::lwf::Playlist layer;
		layer.falloff_radius = 1000;
		layer.flags = opennova::lwf::kFlagInternal | opennova::lwf::kFlagExternal;
		layer.sndparm_indices.push_back(uint32_t(i));
		bank.playlists.push_back(layer);
		opennova::lwf::Sndparm member;
		member.single_index = uint32_t(i);
		member.pitch_scaled = opennova::lwf::kPitchUnityQ16;
		member.volume = 200;
		member.clamp_volume = 255;
		bank.sndparms.push_back(member);
	}
	std::vector<uint8_t> bytes;
	std::string error;
	TEST_EXPECT(opennova::lwf::encode_lwf(bank, bytes, error));
	const std::vector<uint8_t> tone = test_io::read_file(test_paths_fixture("lwf/tone.wav"));
	std::string items = kItems;
	items.replace(items.find("ai_function org1\nattrib: aidata\nend\n"), 0, "sound_profile on_people\n");
	TEST_EXPECT(test_io::write_file(root + "/sounds/game.lwf", bytes) &&
	            test_io::write_file(root + "/sounds/pfs_gnd_l.wav", tone) &&
	            test_io::write_file(root + "/sounds/pfs_gnd_r.wav", tone) &&
	            test_io::write_text(root + "/defs/SndProf.def",
	                                    test_io::crlf("begin \"default\"\nend\nbegin \"on_people\"\n"
	                                                      "\tSSLFootGND PFS_GND_L 0 0 0\n\tSSRFootGND PFS_GND_R 0 0 0\nend\n")) &&
	            test_io::write_text(root + "/defs/items.def", items));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.pump();
	const auto run = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			rig.session.advance(2.0 / 62.5);
			rig.pump();
		}
	};
	const auto heard = [&]() {
		size_t count = 0;
		for (const ClipSoundPlay &play : rig.session.clip_sounds_since(0))
			for (const WorkspaceView::Voice &voice : play.voices)
				count += voice.path == "sounds/pfs_gnd_l.wav" || voice.path == "sounds/pfs_gnd_r.wav" ? 1 : 0;
		return count;
	};
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "mission", "clock": {"playing": true, "rate": 1}})"));
	rig.pump();
	run(60);
	TEST_EXPECT(heard() == 0); // Listen off: nothing heard
	rig.session.handle(request::set_viewport(kMission, R"({"kind": "mission", "options": {"listen": {"on": true}}})"));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	run(120);
	const size_t steps = heard();
	std::printf("people heard: %zu footsteps over 240 ticks\n", steps);
	TEST_EXPECT(steps > 0);
	const JsonValue body = viewport_to_json(rig.session.view(), *rig.viewport(), JsonPage());
	const JsonValue *people = body.get("body") ? body.get("body")->get("people") : nullptr;
	// The org1 bodies but the route's walker, who stands at its spawn pose (S26).
	TEST_EXPECT(people && people->get("playing") && people->get("playing")->number == 5.0);
	std::printf("test_people_heard passed\n");
	return 0;
}

int main() {
	TEST_EXPECT(test_poses() == 0);
	TEST_EXPECT(test_people_play() == 0);
	TEST_EXPECT(test_people_heard() == 0);
	TEST_EXPECT(test_missions_sweep() == 0);
	std::printf("editor_mission_poses: all tests passed\n");
	return 0;
}
