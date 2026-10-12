// The terrain family and the tiles' scope (ADR 0046 S25): the files a view's brush edits, as the game resolves them for it. A mission's
// planes are the files its terrain's .trn names, read through overcast.def and its .env (D-TERRAIN-18), its heights
// the .cpt the .trn names, another terrain's where it names one (D-TERRAIN-17), its tiles its own <mission>.til; a
// plane the .trn names no file for, or one the project lacks, refused with the fix (the pane's Add or Import); a
// terrain's own tiles under no mission. The terrain query's family, brush and sample ops answer them; a stroke on a
// plane resolves its file and opens it quietly (the active document kept). The tile tools in the mission's map: a stamp lays a tile in the mission's .til, opened quietly and noted as
// edited from the mission; an Undo in the mission takes it back and a Redo makes it again; a Save of the mission saves
// the .til; a Close of the mission lists it and Don't save leaves it open with its edits; a drag of a tile is one
// step; a tile command turns the selected tiles.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/til_document.h>
#include <editor/model/document.h>
#include <editor/preview/mission_map.h>
#include <editor/preview/terrain_brush_canvas.h>
#include <editor/preview/terrain_brush_spaces.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/mission_viewport.h>
#include <editor/session/session_core.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

// The requests a canvas raised.
struct Gathered final : CanvasRequests {
	std::vector<EditorRequest> requests;
	void request(EditorRequest each) override { requests.push_back(std::move(each)); }
};

std::string fixture(const std::string &name) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + name; }

bool contains(const std::string &text, const std::string &part) {
	if (text.find(part) != std::string::npos) return true;
	std::printf("  \"%s\" lacks \"%s\"\n", text.c_str(), part.c_str());
	return false;
}

JsonValue ask(ProjectSession &session, const std::string &path, const char *op, std::string &error) {
	JsonValue args = JsonValue::make_object();
	args.set("path", opennova::io::json_string(path));
	args.set("op", opennova::io::json_string(op));
	error.clear();
	return session.query("terrain", args, error);
}

int test_routes() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_family");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Family"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixture(std::string("terrain/tmap/") + name))));
	// A second terrain naming the first's height data (D-TERRAIN-17: 15 of JO:CA's 38 do).
	const std::vector<uint8_t> tmap = test_io::read_file(fixture("terrain/tmap/Tmap.trn"));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tother.trn", tmap));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	editor_test::handle_to_end(session, request::create_file("o.bms", "mission", {{"terrain", "Tother.trn"}, {"environment", "day.env"}}));
	const AssetEntry *m = v.project.scan->find("m.bms");
	const AssetEntry *o = v.project.scan->find("o.bms");
	TEST_EXPECT(m && o);
	session.handle(request::open_document(m->relative_path));
	session.handle(request::open_document(o->relative_path));

	std::string error;
	JsonValue family = ask(session, m->relative_path, "family", error);
	TEST_EXPECT(error.empty() && family.is_object());
	const JsonValue *planes = family.get("planes");
	TEST_EXPECT(planes && planes->get("heights") && planes->get("surface"));
	TEST_EXPECT(planes->get("heights")->get_string("path", "") == "terrain/Tmap.cpt");
	TEST_EXPECT(contains(planes->get("heights")->get_string("words", ""), "Tmap.cpt, in place"));
	TEST_EXPECT(planes->get("surface")->get_string("path", "") == "terrain/Tmap_m.pcx");
	TEST_EXPECT(planes->get("foliage")->get_string("path", "") == "terrain/Tmap_f.pcx");
	// The colour and blend maps the .trn names are no files of the project: refused with the fix.
	TEST_EXPECT(planes->get("colour")->get_string("path", "x").empty() &&
	            contains(planes->get("colour")->get_string("refusal", ""), "mnml_c.tga is not a file of the project"));
	TEST_EXPECT(contains(planes->get("blend")->get_string("refusal", ""), "mnml_d1.tga"));
	// The mission's own tiles, made with its first tile.
	TEST_EXPECT(family.get("tiles")->get_string("name", "") == "m.til" &&
	            contains(family.get("tiles")->get_string("words", ""), "made with its first tile"));
	// The mission on the other terrain: its heights the .cpt its .trn names (Tmap's).
	family = ask(session, o->relative_path, "family", error);
	TEST_EXPECT(error.empty() && family.get("terrain")->string == "terrain/Tother.trn");
	TEST_EXPECT(family.get("planes")->get("heights")->get_string("path", "") == "terrain/Tmap.cpt");

	// The brush: live once the Terrain pane shows, its plane and where its edits go.
	session.handle(request::set_workspace(R"({"terrain_brush":{"tool":"surface"}})"));
	JsonValue brush = ask(session, m->relative_path, "brush", error);
	TEST_EXPECT(error.empty() && !brush.get_bool("live", true) && brush.get_string("plane", "") == "surface");
	session.handle(request::open_document(m->relative_path));
	session.handle(request::set_workspace(R"({"document":{"terrain":true}})"));
	TEST_EXPECT(session.outcome().done() && v.workspace.document(m->relative_path).terrain);
	brush = ask(session, m->relative_path, "brush", error);
	TEST_EXPECT(brush.get_bool("live", false) && brush.get("edits")->get_string("path", "") == "terrain/Tmap_m.pcx");

	// A sample of the ground: its height and its surface in the game's words.
	JsonValue args = JsonValue::make_object();
	args.set("path", opennova::io::json_string(m->relative_path));
	args.set("op", opennova::io::json_string("sample"));
	args.set("x", opennova::io::json_number(-256.0));
	args.set("y", opennova::io::json_number(256.0));
	const JsonValue sample = session.query("terrain", args, error);
	TEST_EXPECT(error.empty() && sample.get("ground") && sample.get("ground")->is_object());

	// A stroke on the surface map: its file opened quietly (the mission kept active), the stroke laid by the maps'
	// planner (S25 B4: editor_terrain_map_paint).
	TerrainStrokeAsk stroke;
	stroke.plane = TerrainPlane::Surface;
	stroke.points = {{-256.0, 256.0}};
	session.handle(request::terrain_stroke(m->relative_path, stroke));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(session.document_base_for("terrain/Tmap_m.pcx") != nullptr && v.documents.active == m->relative_path);
	// A plane the project has no file of: refused with the fix, nothing opened.
	stroke.plane = TerrainPlane::Colour;
	session.handle(request::terrain_stroke(m->relative_path, stroke));
	TEST_EXPECT(!session.outcome().done() && contains(session.outcome().findings.back().message, "copy it into the project"));
	std::printf("routes: the .trn's files, another's height data, the refusals, the brush, a sample, a quiet open\n");
	return 0;
}

// The tiles' scope over the mission's .til (S25 C1: the undo scope proven over records).
int test_tiles_scope() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_tiles_scope");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Tiles"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixture(std::string("terrain/tmap/") + name))));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	const std::string mission = v.project.scan->find("m.bms")->relative_path;
	const size_t slash = mission.rfind('/');
	EditorRequest til = request::create_file("m.til", "tile_info");
	til.folder = slash == std::string::npos ? std::string("/") : mission.substr(0, slash);
	editor_test::handle_to_end(session, til);
	const AssetEntry *til_entry = v.project.scan->find("m.til");
	TEST_EXPECT(til_entry != nullptr);
	const std::string tiles = til_entry->relative_path;
	session.handle(request::close_document(tiles));
	TEST_EXPECT(session.document_base_for(tiles) == nullptr);
	session.handle(request::open_document(mission));
	session.handle(request::set_workspace(R"({"document":{"terrain":true,"map":true},"terrain_brush":{"tool":"stamp","cell":3}})"));
	TEST_EXPECT(session.outcome().done());
	const auto rows = [&]() -> size_t {
		const DocumentBase *document = session.document_base_for(tiles);
		const Document *records = document ? records_of(*document) : nullptr;
		return records ? records->rows().size() : size_t(-1);
	};
	// A stamp in the map: the .til opened quietly, a tile laid, the mission still active.
	ViewportDrop stamp;
	stamp.reference = "tile";
	stamp.name = "3";
	stamp.x = 40.0f;
	stamp.y = 40.0f;
	stamp.kind = ViewportKind::Map;
	session.handle(request::edit_in_viewport(mission, stamp));
	TEST_EXPECT(session.outcome().done() && rows() == 1 && v.documents.active == mission);
	TEST_EXPECT(v.documents.edited_from.count(mission) && v.documents.edited_from.at(mission).front() == tiles);
	// Undo in the mission takes the stamp back; Redo makes it again.
	TEST_EXPECT(v.documents.undo_next.path == tiles);
	session.handle(request::undo(mission));
	TEST_EXPECT(rows() == 0 && session.outcome().stepped.size() == 1 && session.outcome().stepped[0] == tiles);
	session.handle(request::redo(mission));
	TEST_EXPECT(rows() == 1);
	// A Save of the mission saves its .til.
	session.handle(request::save(mission));
	TEST_EXPECT(session.outcome().done() && !session.document_base_for(tiles)->dirty());
	TEST_EXPECT(test_io::read_file(root + "/" + tiles).size() == 16 + 12);
	// The tile selected by a click, turned by a command.
	session.handle(request::set_workspace(R"({"terrain_brush":{"tool":"tiles"}})"));
	const TilDocument *placement = dynamic_cast<const TilDocument *>(session.document_base_for(tiles));
	const ViewportModel *map_model = session.viewports().find(mission, ViewportKind::Map);
	TEST_EXPECT(placement && placement->file().entries.size() == 1 && map_model);
	if (!placement || placement->file().entries.size() != 1 || !map_model) return 1;
	const MissionMapView map_view =
			static_cast<const MissionMapViewport *>(map_model)->view(map_model->size().width, map_model->size().height);
	ViewportCommand click;
	click.name = "click";
	click.kind = ViewportKind::Map;
	click.has_at = true;
	// The middle of the square the game draws the tile over: 8 units east and south of its corner.
	map_view.project(placement->file().entries[0].x_fixed / 65536.0 + 8.0, placement->file().entries[0].z_fixed / 65536.0 - 8.0,
	                 click.at_x, click.at_y);
	session.handle(request::edit_in_viewport(mission, click));
	TEST_EXPECT(session.outcome().done() && v.workspace.document(mission).tiles.size() == 1);
	ViewportCommand turn;
	turn.name = "tile_rotate";
	turn.kind = ViewportKind::Map;
	turn.by = {1.0};
	session.handle(request::edit_in_viewport(mission, turn));
	TEST_EXPECT(session.outcome().done() && session.document_base_for(tiles)->dirty());
	// The canvas: a press on the selected tile keeps the selection for its drag, the drag moves the tile's corner by
	// the pointer's travel (whole units), the release ends it by its token; one step, which the mission's Undo takes.
	{
		const DocumentBase *view_document = session.document_base_for(mission);
		TerrainBrushHalf half(ViewportKind::Map);
		half.follow(v, view_document);
		TEST_EXPECT(half.live());
		const MapBrushSpace space(map_view, nullptr);
		CanvasGesture gesture;
		CanvasSubject subject;
		subject.path = mission;
		subject.identity = view_document->identity();
		Gathered out;
		const int32_t before = placement->file().entries[0].x_fixed;
		CanvasInput in;
		in.width = map_model->size().width;
		in.height = map_model->size().height;
		in.hovered = true;
		in.mouse = CanvasPoint{click.at_x, click.at_y};
		in.screen = in.mouse;
		in.pressed = true;
		in.down = true;
		const ViewportContext context = viewport_context(v, *map_model);
		TEST_EXPECT(half.input(gesture, subject, &space, in, out, &context) && out.requests.empty());
		in.pressed = false;
		in.mouse.x += 40.0f;
		in.screen = in.mouse;
		in.delta = CanvasPoint{40.0f, 0.0f};
		// The drag planned as the canvas plans its own: a batch on the .til under the gesture's token.
		TEST_EXPECT(half.input(gesture, subject, &space, in, out, &context) && out.requests.size() == 1 &&
		            out.requests[0].kind == EditorRequestKind::EditRecord && out.requests[0].path == tiles &&
		            !out.requests[0].edits.empty() && out.requests[0].edits.front().gesture != 0);
		in.down = false;
		in.delta = CanvasPoint{};
		TEST_EXPECT(half.input(gesture, subject, &space, in, out, &context) && out.requests.size() == 2 &&
		            out.requests[1].kind == EditorRequestKind::EndEdit &&
		            out.requests[1].gesture == out.requests[0].edits.front().gesture);
		for (const EditorRequest &each : out.requests) {
			session.handle(each);
			TEST_EXPECT(session.outcome().done());
		}
		const double moved = double(placement->file().entries[0].x_fixed - before) / 65536.0;
		TEST_EXPECT(std::fabs(moved - 40.0 * double(map_view.scale)) <= 1.0 && moved != 0.0);
		session.handle(request::undo(mission));
		TEST_EXPECT(placement->file().entries[0].x_fixed == before);
		session.handle(request::redo(mission));
		TEST_EXPECT(placement->file().entries[0].x_fixed != before);
	}
	// A Close of the mission lists the .til's unsaved turn; Don't save drops the mission alone, the .til kept open.
	session.handle(request::close_document(mission));
	TEST_EXPECT(v.dialogs.unsaved_prompt.open && v.dialogs.unsaved_prompt.target == mission &&
	            std::find(v.dialogs.unsaved_prompt.files.begin(), v.dialogs.unsaved_prompt.files.end(), tiles) !=
	                    v.dialogs.unsaved_prompt.files.end());
	session.handle(request::resolve_unsaved(UnsavedChoice::Discard));
	TEST_EXPECT(session.document_base_for(mission) == nullptr && session.document_base_for(tiles) != nullptr &&
	            session.document_base_for(tiles)->dirty());
	std::printf("tiles' scope: a quiet stamp, Undo and Redo in the mission, its Save, a select and a turn, its Close\n");
	return 0;
}

// The views' rule during a stroke (S25): a Paint stroke over the wire in the mission's surface map, its samples one
// gesture. While it is open the mission's 3D view and its map patch their ground in place: no sample has the open map
// serialized again, no Rebuild is held, and the 3D view's ground reads the painted class at once. The stroke's end
// makes the pictures again (a Rebuild). A sample after ten seconds with none (a client gone) begins a gesture of its
// own, a step of its own.
int test_stroke_follows() {
	editor_test::TempProjectDir dir("opennova_editor_terrain_stroke_follows");
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Follows"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixture(std::string("terrain/tmap/") + name))));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	const std::string mission = v.project.scan->find("m.bms")->relative_path;
	session.handle(request::open_document(mission));
	session.handle(request::set_workspace(R"({"document":{"terrain":true,"map":true},"terrain_brush":{"tool":"surface","surface":12,"radius":4}})"));
	TEST_EXPECT(session.outcome().done());
	Viewports &viewports = session.viewports();
	for (const ViewportKind kind : {ViewportKind::Mission, ViewportKind::Map}) viewports.attach(mission, kind);
	// A frame of the Shell's: the viewports followed, their devices' actions taken, then the 3D view's canvas reading
	// its ground (the brush's hint and ring do each frame while it is live).
	const auto frame = [&]() {
		ViewportAction actions[2];
		viewports.follow(v);
		actions[0] = viewports.take_action(mission, ViewportKind::Mission);
		actions[1] = viewports.take_action(mission, ViewportKind::Map);
		if (const auto *drawn = static_cast<const MissionViewport *>(viewports.find(mission, ViewportKind::Mission)))
			drawn->followed_ground(v);
		return std::make_pair(actions[0], actions[1]);
	};
	frame();
	const auto *assets = v.findings.assets.get();
	TEST_EXPECT(assets != nullptr);
	if (!assets) return 1;
	// A sample's point on the island.
	const auto sample = [&](uint64_t gesture, double x, bool end) {
		TerrainStrokeAsk ask;
		ask.plane = TerrainPlane::Surface;
		ask.points.emplace_back(x, 0.0);
		ask.gesture = gesture;
		ask.end = end;
		session.handle(request::terrain_stroke(mission, ask));
		return session.outcome().done() ? session.outcome().gesture : 0;
	};
	const uint64_t gesture = sample(0, 0.0, false);
	TEST_EXPECT(gesture != 0);
	frame(); // the map opened quietly for the stroke: read once, as it now stands
	const uint64_t serialized = assets->serializations();
	const auto *view3d = static_cast<const MissionViewport *>(viewports.follow_one(v, mission, ViewportKind::Mission));
	TEST_EXPECT(view3d != nullptr);
	if (!view3d) return 1;
	const int reads = view3d->ground_reader().reads();
	bool rebuilt = false;
	for (int i = 1; i <= 4; ++i) {
		TEST_EXPECT(sample(gesture, 2.0 * i, false) == gesture);
		const auto actions = frame();
		rebuilt = rebuilt || actions.first == ViewportAction::Rebuild || actions.second == ViewportAction::Rebuild;
	}
	TEST_EXPECT(assets->serializations() == serialized && !rebuilt && view3d->ground_reader().reads() == reads);
	TEST_EXPECT(!view3d->terrain_patches().empty());
	TEST_EXPECT(view3d->ground_reader().terrain_at(8.0, 0.0, 0.0).surface == 12);
	// The stroke's end: the pictures made again.
	session.handle(request::end_edit(mission, gesture));
	const auto ended = frame();
	TEST_EXPECT(ended.first == ViewportAction::Rebuild && ended.second == ViewportAction::Rebuild);
	// The brush's drag over the wire names no record (id 0): a sample of a stroke at the map's pixel, its gesture
	// answered; a move's id 0 still refused.
	{
		JsonValue brush;
		std::string error;
		TEST_EXPECT(opennova::io::json_parse(R"({"kind": "edit_in_viewport", "path": ")" + mission +
		                                             R"(", "drag": {"id": 0, "handle": "brush", "to": [40, 40], "end": true, "kind": "map"}})",
		                                     brush, error));
		const JsonValue answer = session.handle_json(brush);
		const JsonValue *outcome = answer.get("outcome");
		const JsonValue *done = outcome ? outcome->get("done") : nullptr;
		const JsonValue *token = outcome ? outcome->get("gesture") : nullptr;
		TEST_EXPECT(done && done->is_bool() && done->boolean && token && token->is_number() && token->number > 0);
		JsonValue move;
		TEST_EXPECT(opennova::io::json_parse(R"({"kind": "edit_in_viewport", "path": ")" + mission +
		                                             R"(", "drag": {"id": 0, "handle": "move", "to": [40, 40], "kind": "map"}})",
		                                     move, error));
		const JsonValue moved = session.handle_json(move);
		const JsonValue *refused = moved.get("error");
		TEST_EXPECT(refused && contains(refused->string, "must be a record identity"));
		// A stroke of several samples over the wire, in the 3D view and on the map, each tool's: one gesture, each
		// sample going on with the one before.
		for (const char *tool : {"surface", "raise"}) {
			session.handle(request::set_workspace(std::string(R"({"terrain_brush":{"tool":")") + tool + R"("}})"));
			for (const char *kind : {"map", "mission"}) {
				// The pixels over the island's middle: the map's projection of the origin, the 3D view's middle.
				float px = 0.0f, py = 0.0f;
				const bool map = std::string(kind) == "map";
				const ViewportModel *model = viewports.find(mission, map ? ViewportKind::Map : ViewportKind::Mission);
				TEST_EXPECT(model != nullptr);
				if (!model) return 1;
				if (map) static_cast<const MissionMapViewport *>(model)->view(model->size().width, model->size().height).project(0.0, 0.0, px, py);
				else px = float(model->size().width) * 0.5f, py = float(model->size().height) * 0.5f;
				double token = 0.0;
				for (int i = 0; i < 4; ++i) {
					JsonValue sample;
					const std::string text = R"({"kind": "edit_in_viewport", "path": ")" + mission +
					                         R"(", "drag": {"id": 0, "handle": "brush", "to": [)" + std::to_string(px + 4.0f * float(i)) +
					                         R"(, )" + std::to_string(py) + R"(], "gesture": )" + std::to_string(uint64_t(token)) + R"(, "end": )" +
					                         (i == 3 ? "true" : "false") + R"(, "kind": ")" + kind + R"("}})";
					TEST_EXPECT(opennova::io::json_parse(text, sample, error));
					const JsonValue answer = session.handle_json(sample);
					const JsonValue *outcome = answer.get("outcome");
					const JsonValue *done = outcome ? outcome->get("done") : nullptr;
					const JsonValue *gesture = outcome ? outcome->get("gesture") : nullptr;
					if (!done || !done->boolean) std::printf("  %s %s sample %d: %s\n", tool, kind, i, opennova::io::json_write(answer).c_str());
					TEST_EXPECT(done && done->boolean && gesture && (i == 0 || gesture->number == token));
					if (i == 0) token = gesture->number;
				}
			}
		}
	}
	// A client gone: ten seconds with no sample end its gesture; its token's next sample begins one of its own.
	const uint64_t second = sample(0, 20.0, false);
	TEST_EXPECT(second != 0 && second != gesture);
	platform.clock += kWireGestureLapseMs;
	session.poll();
	TEST_EXPECT(!v.documents.gesture_in(v.documents.edited_from.at(mission).front()).open());
	TEST_EXPECT(sample(second, 22.0, true) == second);
	std::printf("stroke follows: no serialization per sample, no Rebuild, the ground patched; the end's Rebuild; a lapse\n");
	return 0;
}

} // namespace

int main() {
	if (test_routes() != 0) return 1;
	if (test_stroke_follows() != 0) return 1;
	return test_tiles_scope();
}
