// The follow pass (S25 lane A5): at a heights bake's landing, the entities of the missions on the terrain follow the
// ground (each z moved by the change of the ground under it, as the original editor samples it); a mission open and one
// closed (opened in the background, only where an entity moves); one Undo takes the stroke, its bake and the entities
// that followed it, one Redo brings them back; a follower edited since keeps its later edit. Every entity here is
// unlocked but one, whose Height lock keeps it where it stands. Then the links over the heights' raster history: a
// redo whose stroke a later stroke dropped is refused, named; a follower's undo under a later stroke names the .cpt as
// keeping it.
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_reads.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/session/mission_base_terrain.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_heights_plan.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_mis.h>
#include <formats/trn/trn_io.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

std::string fixture(const char *relative) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative; }

// A mission on Tmap holding one building at each point (mission units).
std::vector<uint8_t> mission_at(const std::vector<std::pair<float, float>> &points) {
	bms::File file;
	mission::BlankMission blank;
	blank.name = "Follow";
	blank.terrain = "Tmap";
	blank.environment = "day";
	std::string error;
	if (!mission::make_blank(file, blank, error)) return {};
	for (const auto &[x, y] : points) {
		mission::EntityTransform at;
		at.x = x;
		at.y = y;
		at.z = 4.0f;
		mission::add_entity(file, mission::EntityKind::Building, 100701, at);
	}
	mission::sync_counts(file);
	std::vector<uint8_t> bytes;
	if (!bms::write(file, bytes, error)) bytes.clear();
	return bytes;
}

const DocumentBase *open_at(ProjectSession &session, const std::string &path) {
	for (const auto &document : session.view().documents.open)
		if (document->path() == path) return document.get();
	return nullptr;
}

// The z of each entity of the open mission at `path`, in the file's order.
std::vector<double> heights_of(ProjectSession &session, const std::string &path) {
	std::vector<double> out;
	if (const auto *mission = dynamic_cast<const MissionDocument *>(open_at(session, path)))
		for (const MissionEntityRead &read : mission_entities(*mission)) out.push_back(read.z);
	return out;
}

bool has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }

// The change of the ground under (x, y), 16.16, from `before` to `after`, as the original editor samples it over Tmap.
int32_t change_at(const TrnConfig &trn, int32_t scale, const std::vector<uint16_t> &before,
		const std::vector<uint16_t> &after, double x, double y) {
	const int32_t fx = bms::to_fixed_16_16(x), fy = bms::to_fixed_16_16(y);
	return mission::mis_terrain_height(base_terrain_sampler(trn, scale, after.data()), fx, fy) -
	       mission::mis_terrain_height(base_terrain_sampler(trn, scale, before.data()), fx, fy);
}

int test_follow() {
	editor_test::TempProjectDir dir("opennova_editor_height_lock_follow");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Follow"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	const std::vector<uint8_t> cpt_bytes = test_io::read_file(fixture("terrain/tmap/Tmap.cpt"));
	const std::vector<uint8_t> trn_bytes = test_io::read_file(fixture("terrain/tmap/Tmap.trn"));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.cpt", cpt_bytes));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.trn", trn_bytes));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	// Under the stroke at mission (-200, 200) and far from it (300, -300).
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/open.bms", mission_at({{-200.0f, 200.0f}, {300.0f, -300.0f}, {-201.0f, 199.0f}})));
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/closed.bms", mission_at({{-199.0f, 201.0f}})));
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/away.bms", mission_at({{300.0f, -300.0f}})));
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::rescan());
	// The project's references read (the graph the follow finds the missions by): the operations and the validation
	// they left due run to their end.
	session.run_operations();
	TEST_EXPECT(v.findings.graph && v.activity.validation.read && !v.activity.validation.files_unread);
	const AssetEntry *open_entry = v.project.scan->find("open.bms");
	const AssetEntry *closed_entry = v.project.scan->find("closed.bms");
	const AssetEntry *away_entry = v.project.scan->find("away.bms");
	const AssetEntry *cpt_entry = v.project.scan->find("Tmap.cpt");
	TEST_EXPECT(open_entry && closed_entry && away_entry && cpt_entry);
	if (!open_entry || !closed_entry || !away_entry || !cpt_entry) return 1;
	const std::string m = open_entry->relative_path, closed = closed_entry->relative_path,
	                  away = away_entry->relative_path, cpt = cpt_entry->relative_path;
	session.handle(request::open_document(m));
	// The third building, beside the first, height locked (lane C's Height lock).
	const auto *open_mission = dynamic_cast<const MissionDocument *>(open_at(session, m));
	TEST_EXPECT(open_mission && mission_entities(*open_mission).size() == 3);
	if (!open_mission) return 1;
	Edit lock_it;
	lock_it.address = mission_entities(*open_mission)[2].address;
	lock_it.field = "height_lock";
	lock_it.value = int64_t(1);
	session.handle(request::edit_record(m, lock_it));
	TEST_EXPECT(session.outcome().done());
	const std::vector<double> open_before = heights_of(session, m);
	TEST_EXPECT(open_before.size() == 3 && !open_at(session, closed) && !open_at(session, away));
	// The lock op: under an 8-unit disc at the first building, one entity follows the ground and one is locked.
	io::JsonValue args = io::JsonValue::make_object();
	args.set("path", io::json_string(m));
	args.set("op", io::json_string("lock"));
	args.set("x", io::json_number(-200.0));
	args.set("y", io::json_number(200.0));
	args.set("radius", io::json_number(8.0));
	std::string error;
	const io::JsonValue lock = session.query("terrain", args, error);
	TEST_EXPECT(error.empty() && lock.get("follow") && lock.get("follow")->number == 1.0 && lock.get("locked") &&
	            lock.get("locked")->number == 1.0);

	// A raise under the first building, ended; its bake lands.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Heights;
	ask.brush.tool = TerrainTool::Raise;
	ask.brush.radius = 8.0f;
	ask.brush.strength = 1.0f;
	ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
	ask.points = {{-204.0, 200.0}, {-196.0, 200.0}};
	ask.gesture = 7171;
	ask.end = true;
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(editor_test::heights_settled(session, cpt));

	// The ground before and after, as the original editor reads it.
	CptFile shipped;
	std::string why;
	TEST_EXPECT(load_cpt(cpt_bytes.data(), cpt_bytes.size(), shipped, why));
	const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(open_at(session, cpt));
	TEST_EXPECT(heights != nullptr);
	if (!heights) return 1;
	std::vector<uint16_t> after(size_t(1024) * 1024);
	for (int y = 0; y < 1024; ++y)
		for (int x = 0; x < 1024; ++x) after[size_t(y) * 1024 + size_t(x)] = heights->atlas().u16(x, y);
	TrnConfig trn;
	int32_t scale = 128;
	TEST_EXPECT(load_mission_trn(std::string(trn_bytes.begin(), trn_bytes.end()), TrnLaterTexts{}, trn, why));
	const int32_t under_first = change_at(trn, scale, shipped.depth_buffer, after, -200.0, 200.0);
	const int32_t under_closed = change_at(trn, scale, shipped.depth_buffer, after, -199.0, 201.0);
	TEST_EXPECT(under_first > 0 && under_closed > 0);
	TEST_EXPECT(change_at(trn, scale, shipped.depth_buffer, after, 300.0, -300.0) == 0);

	// The open mission's first building followed, the far one stayed; the closed one opened and followed; the one
	// whose entities the change does not reach stayed closed.
	const std::vector<double> open_after = heights_of(session, m);
	TEST_EXPECT(open_after.size() == 3 && open_after[0] == open_before[0] + under_first / 65536.0 &&
	            open_after[1] == open_before[1]);
	// The locked one kept its height, the ground under it changed as much.
	TEST_EXPECT(change_at(trn, scale, shipped.depth_buffer, after, -201.0, 199.0) > 0 && open_after[2] == open_before[2]);
	TEST_EXPECT(open_at(session, closed) && open_at(session, closed)->dirty());
	const std::vector<double> closed_after = heights_of(session, closed);
	TEST_EXPECT(closed_after.size() == 1 && closed_after[0] == 4.0 + under_closed / 65536.0);
	TEST_EXPECT(!open_at(session, away));

	// One Undo in the mission's view: the stroke, its bake and both followers.
	session.handle(request::undo(m));
	heights = dynamic_cast<const TerrainHeightsDocument *>(open_at(session, cpt));
	TEST_EXPECT(heights && !heights->dirty() &&
	            heights->serialize().text == std::string(cpt_bytes.begin(), cpt_bytes.end()));
	TEST_EXPECT(heights_of(session, m) == open_before);
	TEST_EXPECT(heights_of(session, closed) == std::vector<double>{4.0});
	// One Redo: all three again.
	session.handle(request::redo(m));
	TEST_EXPECT(heights_of(session, m) == open_after && heights_of(session, closed) == closed_after);
	heights = dynamic_cast<const TerrainHeightsDocument *>(open_at(session, cpt));
	TEST_EXPECT(heights && heights->dirty() && heights->heights().unbaked.empty());

	// A follower edited since keeps its later edit: the closed mission's building moved across, then an Undo in the
	// open mission's view takes the stroke, its bake and the open mission's follow, the edited one's x kept.
	const auto *closed_mission = dynamic_cast<const MissionDocument *>(open_at(session, closed));
	TEST_EXPECT(closed_mission != nullptr);
	if (!closed_mission) return 1;
	const NodeAddress building = mission_entities(*closed_mission).front().address;
	Edit across;
	across.address = building;
	across.field = "x";
	across.value = -150.0;
	session.handle(request::edit_record(closed, across));
	TEST_EXPECT(session.outcome().done());
	session.handle(request::undo(m));
	TEST_EXPECT(heights_of(session, m) == open_before);
	closed_mission = dynamic_cast<const MissionDocument *>(open_at(session, closed));
	TEST_EXPECT(closed_mission && mission_entities(*closed_mission).front().x == -150.0);

	// A stroke where no entity stands drops the .cpt's redo of the first: a Redo in the open mission's view would make
	// its follow again over ground that is gone, so it is refused and names the .cpt; nothing moves.
	TerrainStrokeAsk elsewhere = ask;
	elsewhere.points = {{-104.0, 100.0}, {-96.0, 100.0}};
	elsewhere.gesture = 7272;
	session.handle(request::terrain_stroke(m, elsewhere));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(editor_test::heights_settled(session, cpt));
	TEST_EXPECT(heights_of(session, m) == open_before);
	session.handle(request::redo(m));
	TEST_EXPECT(session.outcome().stepped.empty() && heights_of(session, m) == open_before);
	TEST_EXPECT(has(v.activity.status, "Nothing redone") && has(v.activity.status, "Tmap.cpt"));

	// A follower's own undo under a later stroke: a raise under the moved building follows it, a stroke elsewhere
	// lands on the .cpt after; an Undo in the follower's tab takes its follow and names the .cpt as keeping the stroke.
	const std::vector<double> closed_before = heights_of(session, closed);
	TerrainStrokeAsk under = ask;
	under.points = {{-154.0, 201.0}, {-146.0, 201.0}};
	under.gesture = 7373;
	session.handle(request::terrain_stroke(m, under));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(editor_test::heights_settled(session, cpt));
	const std::vector<double> closed_raised = heights_of(session, closed);
	TEST_EXPECT(closed_raised.size() == 1 && closed_before.size() == 1 && closed_raised[0] > closed_before[0]);
	TerrainStrokeAsk later = elsewhere;
	later.gesture = 7474;
	session.handle(request::terrain_stroke(m, later));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(editor_test::heights_settled(session, cpt));
	session.handle(request::undo(closed));
	TEST_EXPECT(heights_of(session, closed) == closed_before);
	TEST_EXPECT(has(v.activity.status, "Tmap.cpt was edited since and keeps its later edits"));
	std::printf("follow: an open and a closed mission's entities followed the ground, one Undo and Redo for all, an "
	            "edited follower kept\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_follow();
	if (failures == 0) std::printf("editor_height_lock_follow: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
