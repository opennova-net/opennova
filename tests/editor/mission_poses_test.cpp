// A mission's people posed as the game spawns them (DI-38, editor/preview/mission_poses): over a real
// session, a project holding a person's .adm and clips, an item catalog and a minted mission, each
// organic's pose is the one the world's own organic init leaves the same body in (a World of the
// same records, initialized in pool order over the same clips: the state, the playheads, the served
// ring entries, the blend), with why a person is not posed (an item no catalog defines, another
// class, no .adm, no clips); a definition without `aidata` reads neither the route nor the guard;
// the pose follows the records live (a route edited, an SSN edited: an Update with the poses moved;
// another item's catalog edit), and its wire form names the row, the clip and the playhead. Mixed:
// the retail leg (OPENNOVA_JO_DIR) poses every shipped mission's people over the game's own catalog
// and clips and holds each pose to the world init's.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
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
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
namespace world = opennova::world;

namespace {

using editor_test::FakeDevices;
using editor_test::NoProcess;

constexpr const char *kMission = "missions/people.bms";

std::string fixture(const std::string &rel) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + rel;
}

// A person's table (CRLF lines, as the game's config reader takes them): the reset and idle_2 rows
// idle, idle a ring of two (idle, then walk), the walk and the guard rows walk.
constexpr const char *kPeopleAdm = "anim_reset\t\"idle\"\r\nanim_idle\t\"idle\" \"walk\"\r\nanim_idle_2\t\"idle\"\r\n"
								   "anim_walk_forward\t\"walk\"\r\nanim_guard\t\"walk\"\r\n";

// The persons: a rifleman (org1, aidata, the table), a civilian (org1, the table, no aidata), a player
// (plyr), a statue (org1, aidata, no table), a pilot (org1, aidata, a table the project lacks).
constexpr const char *kItems = "begin \"Spawn Rifleman\"\nid 106200\ntype person\ngraphic shed\nanim_def people\n"
							   "ai_function org1\nattrib: aidata\nend\n"
							   "begin \"Spawn Civilian\"\nid 106201\ntype person\ngraphic shed\nanim_def people\n"
							   "ai_function org1\nend\n"
							   "begin \"Spawn Player\"\nid 106202\ntype person\ngraphic shed\nanim_def people\n"
							   "ai_function plyr\nattrib: aidata\nend\n"
							   "begin \"Spawn Statue\"\nid 106203\ntype person\ngraphic shed\nai_function org1\n"
							   "attrib: aidata\nend\n"
							   "begin \"Spawn Pilot\"\nid 106204\ntype person\ngraphic shed\nanim_def nowhere\n"
							   "ai_function org1\nattrib: aidata\nend\n";

// One placed person: its item, its SSN, its route and its attributes.
struct Placed {
	int item;
	int ssn;
	int route;
	uint32_t attributes;
};
// In the file's order: posed idle, a route's walk, a guard, the civilian (its route and guard unread),
// route 126 and 127, a second rifleman of the ring, then the four not posed.
const Placed kPeople[] = { { 106200, 13, 0, 0 }, { 106200, 6, 3, 0 }, { 106200, 15, 0, 0x2 }, { 106201, 4, 3, 0x2 },
	{ 106200, 2, 126, 0 }, { 106200, 1, 127, 0x2 }, { 106200, 12, 0, 0 }, { 106202, 7, 0, 0 }, { 106203, 8, 0, 0 },
	{ 106204, 9, 0, 0 }, { 109999, 10, 0, 0 } };

std::vector<uint8_t> people_mission() {
	opennova::bms::File mission;
	opennova::mission::make_default(mission);
	for (const Placed &person : kPeople) {
		opennova::bms::Entity entity =
				opennova::mission::new_entity(opennova::mission::EntityKind::Organic, person.item, person.ssn);
		entity.waypoint_id = uint8_t(person.route);
		entity.bmsi_attributes = person.attributes;
		mission.organics.push_back(entity);
	}
	opennova::mission::sync_counts(mission);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::bms::write(mission, bytes, error)) bytes.clear();
	return bytes;
}

Edit set_of(const NodeAddress &record, const char *field, Value value) {
	Edit edit;
	edit.address = record;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// A session over a project holding the people, their catalog and clips, the mission open and its
// viewport followed through the fake devices.
struct Rig {
	editor_test::TempProjectDir dir;
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
		if (!editor_test::write_bytes(root + "/" + kMission, people_mission()) ||
				!editor_test::write_text(root + "/defs/items.def", kItems) ||
				!editor_test::write_text(root + "/people.adm", kPeopleAdm))
			return false;
		for (const char *clip : { "idle.bad", "walk.bad" })
			if (!editor_test::write_bytes(root + "/" + clip, test_io::read_file(fixture(std::string("anim/") + clip))))
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
	// A body of `adm` with the record's SSN, route and guard as the spawn folds them; the handle.
	world::EntityHandle add(int adm, int ssn, int route, bool guard) {
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
	double rise(world::EntityHandle handle) { return double(world.ai.for_handle(handle)->pos[2]) / 65536.0; }
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
		TEST_EXPECT(own.stand(viewport->scene(), &field));
		TEST_EXPECT(!own.stand(viewport->scene(), &field)); // stood: nothing moves
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
		rig.session.handle(request::edit_record(kMission, set_of(rows[0], "x", 12.5)));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(rig.last() == ViewportAction::Update && poses.runs() == runs);
		const uint32_t stamp = pose_of(1)->stamp;
		rig.session.handle(request::edit_record(kMission, set_of(rows[1], "waypoint_id", int64_t(0))));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(rig.last() == ViewportAction::Update && poses.runs() == runs + 1);
		TEST_EXPECT(pose_of(1)->state == 43 && std::string(pose_of(1)->because) == "idle" && pose_of(1)->stamp != stamp);
		const int32_t phase = pose_of(0)->pose.phase;
		rig.session.handle(request::edit_record(kMission, set_of(rows[0], "id", int64_t(0))));
		TEST_EXPECT(rig.session.outcome().done());
		rig.pump();
		TEST_EXPECT(pose_of(0)->updates == 11 && pose_of(0)->pose.phase != phase);
		// The catalog's table for the rifleman taken away outside the editor: those people stand.
		std::string edited = kItems;
		const std::string table = "anim_def people\nai_function org1\nattrib";
		const size_t at = edited.find(table);
		TEST_EXPECT(at != std::string::npos);
		edited.replace(at, table.size(), "ai_function org1\nattrib");
		TEST_EXPECT(editor_test::write_text(rig.session.view().project.root + "/defs/items.def", edited));
		rig.session.handle(request::rescan());
		rig.session.run_operations();
		rig.pump();
		TEST_EXPECT(pose_of(0)->status == "no_adm" && pose_of(3)->status == "posed");
	}
	std::printf("test_poses passed\n");
	return 0;
}

// The retail leg (OPENNOVA_JO_DIR): every shipped mission of the base game written into one project with
// the game's catalog and every animation table and clip, each opened in its viewport; every person the
// game poses is held to the world's own init over the same clips (each mission's bodies in pool order);
// prints the poses by state.
static int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped person posed as the game spawns it)");
	opennova::Vfs game;
	game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
	TEST_EXPECT(game.mount_game(root, std::string(), opennova::VfsMountMode::Packed));
	Rig rig("opennova_editor_mission_poses_retail");
	rig.session.handle(request::new_project(rig.dir.file("project"), "People"));
	rig.session.run_operations();
	editor_test::create_missing_files(rig.session);
	const std::string project = rig.session.view().project.root;
	std::vector<std::string> missions;
	std::set<std::string> seen;
	std::string items_def;
	for (const opennova::VfsFileLocation &file : game.list_files()) {
		const std::string name = std::filesystem::path(file.logical_name).filename().string();
		const std::string lower = retail::lower_ascii(name);
		const std::string extension = retail::lower_ascii(std::filesystem::path(name).extension().string());
		if (extension != ".bms" && extension != ".adm" && extension != ".bad" && lower != "items.def") continue;
		if (!seen.insert(lower).second) continue;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(game.read_file(file.logical_name, bytes));
		const std::string at = extension == ".bms" ? "missions/" + name : lower == "items.def" ? "defs/items.def" : name;
		TEST_EXPECT(editor_test::write_bytes(project + "/" + at, bytes));
		if (extension == ".bms") missions.push_back(at);
	}
	if (missions.empty()) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	struct VfsRigFiles : opennova::anim::RigFiles {
		opennova::Vfs *vfs = nullptr;
		std::shared_ptr<const opennova::adm::AdmFile> animation_map(const std::string &name) const override {
			std::vector<uint8_t> bytes;
			if (!vfs->read_file(opennova::assets::asset_file_name(name, ".adm"), bytes) || bytes.empty()) return nullptr;
			return opennova::assets::parse_animation_map(bytes.data(), bytes.size());
		}
		std::shared_ptr<const opennova::bad::BadFile> bone_animation(const std::string &name) const override {
			std::vector<uint8_t> bytes;
			if (!vfs->read_file(opennova::assets::asset_file_name(name, ".bad"), bytes) || bytes.empty()) return nullptr;
			return opennova::assets::parse_bone_animation(bytes.data(), bytes.size());
		}
	} files;
	files.vfs = &game;
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
	std::printf("retail: %zu missions, %zu people, %zu posed (%s), %zu held to the world's init, in %.1f s\n",
			missions.size(), people, posed, states.c_str(), held, seconds);
	TEST_EXPECT(posed > 0 && held == posed);
	std::printf("test_retail passed\n");
	return 0;
}

// S23 C: the people play their clips on the preview clock from their spawn: each tick of the clock a tick of every
// posed body as the game's org1 motor head runs it before its think (world::organic_body_tick), in pool order over the
// shared ring heads, so after each tick every body is the world's own body ticked so many times after its init, and the
// event words are the world's; a clock gone back starts them again from their spawn.
static int test_people_play() {
	Rig rig("opennova_editor_mission_people_play");
	TEST_EXPECT(rig.open());
	const MissionViewport *viewport = rig.viewport();
	if (!viewport) return 1;
	const MissionPeople &playing = viewport->people();
	const std::vector<NodeAddress> rows = rig.organics();
	TEST_EXPECT(playing.playing() == 7);
	WorldPeople people;
	DirRigFiles files;
	files.root = rig.session.view().project.root;
	const int adm = people.motion.register_adm(&files, "people.adm");
	std::vector<world::EntityHandle> bodies;
	for (size_t i = 0; i < 7; ++i) {
		const bool slot = kPeople[i].item == 106200;
		bodies.push_back(people.add(adm, kPeople[i].ssn, slot ? kPeople[i].route : 0,
				slot && (kPeople[i].attributes & 0x2u) != 0));
	}
	people.init();
	const int32_t start = playing.tick();
	TEST_EXPECT(playing.started_at() == start);
	size_t world_events = 0, wraps = 0;
	for (int32_t tick = 1; tick <= 240; ++tick) {
		rig.session.advance(1.0 / 62.5);
		rig.pump();
		for (const world::EntityHandle body : bodies) {
			world::RootMotionFrame frame;
			world::AiEntity &entity = *people.world.ai.for_handle(body);
			const int32_t before = entity.inf.clip_phase;
			if (world::organic_body_tick(entity.inf, &people.motion, people.world.ai.anim_rings, frame) != 0) ++world_events;
			wraps += entity.inf.clip_phase < before ? 1 : 0;
		}
		const int32_t played = rig.viewport()->people().tick() - start;
		TEST_EXPECT(played == tick);
		if (played != tick) return 1;
		for (size_t i = 0; i < 7; ++i) {
			const world::InfantryBodyPose *pose = rig.viewport()->people().pose(rows[i].row);
			TEST_EXPECT(pose && same(*pose, people.pose(bodies[i])));
		}
	}
	TEST_EXPECT(wraps > 0); // the clips played round
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
	TEST_EXPECT(again.playing() == 7);
	for (size_t i = 0; i < 7; ++i)
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
	const std::vector<uint8_t> tone = test_io::read_file(fixture("lwf/tone.wav"));
	std::string items = kItems;
	items.replace(items.find("ai_function org1\nattrib: aidata\nend\n"), 0, "sound_profile on_people\n");
	TEST_EXPECT(editor_test::write_bytes(root + "/sounds/game.lwf", bytes) &&
	            editor_test::write_bytes(root + "/sounds/pfs_gnd_l.wav", tone) &&
	            editor_test::write_bytes(root + "/sounds/pfs_gnd_r.wav", tone) &&
	            editor_test::write_text(root + "/defs/SndProf.def",
	                                    editor_test::crlf("begin \"default\"\nend\nbegin \"on_people\"\n"
	                                                      "\tSSLFootGND PFS_GND_L 0 0 0\n\tSSRFootGND PFS_GND_R 0 0 0\nend\n")) &&
	            editor_test::write_text(root + "/defs/items.def", items));
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
	TEST_EXPECT(people && people->get("playing") && people->get("playing")->number == 7.0);
	std::printf("test_people_heard passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	TEST_EXPECT(test_poses() == 0);
	TEST_EXPECT(test_people_play() == 0);
	TEST_EXPECT(test_people_heard() == 0);
	TEST_EXPECT(test_retail() == 0);
	std::printf("editor_mission_poses: all tests passed\n");
	return 0;
}
