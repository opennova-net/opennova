// The terrain family and the tiles' scope (ADR 0046 S25): the files a view's brush edits, as the game resolves them for it. A mission's
// planes are the files its terrain's .trn names, read through overcast.def and its .env (D-TERRAIN-18), its heights
// the .cpt the .trn names, another terrain's where it names one (D-TERRAIN-17), its tiles its own <mission>.til; a
// plane the .trn names no file for, or one the project lacks, refused with the fix (the pane's Add or Import); a
// terrain's own tiles under no mission. The terrain query's family, brush and sample ops answer them; a stroke on a
// plane resolves its file and opens it quietly (its planner not in yet: refused, the file open, the active document
// kept). The tile tools in the mission's map: a stamp lays a tile in the mission's .til, opened quietly and noted as
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

	// A stroke on the surface map: its file opened quietly (the mission kept active), the stroke refused while the maps'
	// planner is not in.
	TerrainStrokeAsk stroke;
	stroke.plane = TerrainPlane::Surface;
	stroke.points = {{-256.0, 256.0}};
	session.handle(request::terrain_stroke(m->relative_path, stroke));
	TEST_EXPECT(!session.outcome().done() && session.outcome().findings.back().code() == "terrain.stroke");
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
	map_view.project(placement->file().entries[0].x_fixed / 65536.0 + 8.0, -placement->file().entries[0].z_fixed / 65536.0 + 8.0,
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

} // namespace

int main() {
	if (test_routes() != 0) return 1;
	return test_tiles_scope();
}
