// The tile tools through the session (ADR 0046 S25 B2): a batch of the Tiles tools is an edit_record of the view's
// terrain_view, which goes to the tile placement the view's tiles go to. A mission with no .til gets one made first,
// seeded with the terrain's own tiles (the game reads the mission's in place of the terrain's), one step of the file
// history; the .til opens quietly (the mission kept active) and each batch is a step of the mission view's Undo. The
// view's planners (preview/tile_commands.cpp) plan a click's select, a stamp, the turn, flip, layer and delete
// commands and a drag as those requests; the terrain query's tiles and tiles_at ops answer the view's tiles; the type's
// project check notes tiles past their set's cells and an empty .til hiding the terrain's tiles.
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/til_document.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/terrain_brush.h>
#include <editor/preview/terrain_view_commands.h>
#include <editor/preview/tile_tools.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/tga/tga.h>
#include <formats/til/til_io.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string fixture(const std::string &name) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + name; }

bool contains(const std::string &text, const std::string &part) {
	if (text.find(part) != std::string::npos) return true;
	std::printf("  \"%s\" lacks \"%s\"\n", text.c_str(), part.c_str());
	return false;
}

// A picture one pixel a metre over flat ground.
struct FlatSpace final : TerrainBrushSpace {
	bool pointer_ground(float x, float y, TerrainPoint &at, double &height) const override {
		at = TerrainPoint{double(x), double(y)};
		height = 0.0;
		return true;
	}
	bool to_picture(TerrainPoint at, double, float &x, float &y) const override {
		x = float(at.x);
		y = float(at.y);
		return true;
	}
	double height(TerrainPoint) const override { return 0.0; }
};

struct Recorder final : CanvasRequests {
	std::vector<EditorRequest> requests;
	std::string words;
	void request(EditorRequest each) override { requests.push_back(std::move(each)); }
	void served(std::string said, int64_t) override { words = std::move(said); }
};

// The requests a planner raised, served in order: each done.
bool serve(ProjectSession &session, Recorder &recorder) {
	bool done = true;
	for (const EditorRequest &each : recorder.requests) {
		session.handle(each);
		if (!session.outcome().done()) {
			std::printf("  refused: %s\n",
			            session.outcome().findings.empty() ? "?" : session.outcome().findings.back().message.c_str());
			done = false;
		}
	}
	recorder.requests.clear();
	return done;
}

std::vector<uint8_t> two_tiles() {
	TilFile file;
	file.entries = {make_til_entry(0, 0, 1, 0, 0), make_til_entry(16 << 16, 0, 2, 0, 0)};
	std::vector<uint8_t> out;
	std::string why;
	save_til(file, out, why);
	return out;
}

JsonValue ask(ProjectSession &session, const std::string &path, const char *op, std::string &error, double x = 0.0,
              double y = 0.0, bool at = false) {
	JsonValue args = JsonValue::make_object();
	args.set("path", io::json_string(path));
	args.set("op", io::json_string(op));
	if (at) {
		args.set("x", io::json_number(x));
		args.set("y", io::json_number(y));
	}
	error.clear();
	return session.query("terrain", args, error);
}

int test_tiles_in_a_mission() {
	editor_test::TempProjectDir dir("opennova_editor_tile_edit");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Tiles"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	for (const char *name : {"Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		TEST_EXPECT(editor_test::write_bytes(root + "/terrain/" + name, test_io::read_file(fixture(std::string("terrain/tmap/") + name))));
	// The terrain names its own tiles (polytrn_tileinfo), two of them.
	std::vector<uint8_t> trn = test_io::read_file(fixture("terrain/tmap/Tmap.trn"));
	const std::string tileinfo = "\r\npolytrn_tileinfo        Tmap_t\r\n";
	trn.insert(trn.end(), tileinfo.begin(), tileinfo.end());
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.trn", trn));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap_t.til", two_tiles()));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	const AssetEntry *entry = v.project.scan->find("m.bms");
	TEST_EXPECT(entry != nullptr);
	const std::string m = entry->relative_path;
	session.handle(request::open_document(m));

	// Before any tile: the view's tiles are the terrain's own, its .til not made yet.
	std::string error;
	JsonValue tiles = ask(session, m, "tiles", error);
	TEST_EXPECT(error.empty() && tiles.get_bool("missing", false) && tiles.get_string("from", "") == "Tmap_t.til");
	TEST_EXPECT(tiles.get("tiles") && tiles.get("tiles")->array.size() == 2);
	const std::string til = tiles.get_string("path", "");
	TEST_EXPECT(contains(til, "m.til") && tiles.get_string("tile_set", "") == "mnml_t.tga");

	// A stamp: the mission's .til made (the terrain's two tiles in it), opened quietly, the stamp over them unsaved.
	session.handle(request::tile_edit(m, til, plan_tile_stamp(5, 0, 100.3, 50.6, false)));
	TEST_EXPECT(session.outcome().done());
	TilFile made;
	const std::vector<uint8_t> on_disk = test_io::read_file(root + "/" + til);
	TEST_EXPECT(load_til(on_disk.data(), on_disk.size(), made, error) && made.entries.size() == 2);
	const auto *document = dynamic_cast<const TilDocument *>(session.document_base_for(til));
	TEST_EXPECT(document && document->rows().size() == 3 && document->unsaved());
	TEST_EXPECT(v.documents.active == m);
	const TilOverlayEntry stamped = document->file().entries.back();
	TEST_EXPECT(stamped.tile_index == 5 && stamped.x_fixed == (100 << 16) && stamped.z_fixed == -(51 << 16));
	TEST_EXPECT(contains(v.activity.status, "Laid tiles in m.til"));
	// The mission view's Undo takes the stamp back; Redo lays it again.
	session.handle(request::undo(m));
	TEST_EXPECT(session.outcome().done() && document->rows().size() == 2);
	session.handle(request::redo(m));
	TEST_EXPECT(document->rows().size() == 3);
	// Another file than the view's tiles go to: refused.
	session.handle(request::tile_edit(m, "terrain/Tmap_t.til", plan_tile_stamp(5, 0, 0.0, 0.0, false)));
	TEST_EXPECT(!session.outcome().done() && contains(session.outcome().findings.back().message, "tiles go to"));

	// The view's planners over a flat picture one pixel a metre.
	PreviewClock clock;
	FlatSpace space;
	const DocumentBase *mission = session.document_base_for(m);
	ViewportContext context{ViewportInput{v, clock, mission}};
	TerrainViewContext view{context, space, m};
	Recorder out;
	// A click on the stamped tile's square selects it.
	ViewportCommand click;
	click.name = "click";
	click.has_at = true;
	click.at_x = 108.0f;
	click.at_y = 59.0f;
	TEST_EXPECT(plan_tile_select(view, click, out, error) && serve(session, out));
	const NodeId stamped_id = document->rows().back()->id;
	TEST_EXPECT(v.workspace.document(m).tiles == std::vector<NodeId>{stamped_id});
	TEST_EXPECT(contains(out.words, "cell 5"));
	// Turned a quarter clockwise, flipped across, laid under the others: each a step on the selected tile.
	ViewportCommand turn;
	turn.name = "tile_rotate";
	turn.by = {1.0};
	TEST_EXPECT(plan_tile_command(view, turn, out, error) && serve(session, out));
	TEST_EXPECT(document->file().entries.back().flags == 4);
	ViewportCommand flip;
	flip.name = "tile_flip";
	flip.value = "h";
	TEST_EXPECT(plan_tile_command(view, flip, out, error) && serve(session, out));
	TEST_EXPECT(document->file().entries.back().flags == tile_flipped(4, true));
	ViewportCommand layer;
	layer.name = "tile_layer";
	layer.by = {-1.0};
	TEST_EXPECT(plan_tile_command(view, layer, out, error) && serve(session, out));
	TEST_EXPECT(document->file().entries.front().tile_index == 5 && document->rows().front()->id == stamped_id);
	// A drag takes its corner to the point; the gesture ends with the sample.
	ViewportDrag drag;
	drag.id = stamped_id;
	drag.handle = "tile";
	drag.by = false;
	drag.x = 200.0f;
	drag.y = 300.0f;
	TEST_EXPECT(plan_tile_drag(view, drag, out, error) && out.requests.size() == 2 && serve(session, out));
	TEST_EXPECT(document->file().entries.front().x_fixed == (200 << 16) && document->file().entries.front().z_fixed == -(300 << 16));
	TEST_EXPECT(v.documents.gestures.empty());
	JsonValue at = ask(session, m, "tiles_at", error, 208.0, 308.0, true);
	TEST_EXPECT(error.empty() && at.get("tiles")->array.size() == 1 && at.get("tiles")->array[0].get_bool("selected", false));
	// A stamp on the grid where the brush's cell is dropped.
	ViewportDrop drop;
	drop.reference = "tile";
	drop.name = "7";
	drop.x = 20.0f;
	drop.y = 40.0f;
	drop.snap = 16.0f;
	TEST_EXPECT(plan_tile_drop(view, drop, out, error) && serve(session, out));
	TEST_EXPECT(document->file().entries.back().tile_index == 7 && document->file().entries.back().x_fixed == (16 << 16) &&
	            document->file().entries.back().z_fixed == -(32 << 16));
	// Delete: the selected tile removed, the selection emptied.
	ViewportCommand remove;
	remove.name = "tile_delete";
	TEST_EXPECT(plan_tile_command(view, remove, out, error) && serve(session, out));
	TEST_EXPECT(document->rows().size() == 3 && v.workspace.document(m).tiles.empty());
	// Nothing selected: a command is refused, saying why.
	TEST_EXPECT(!plan_tile_command(view, turn, out, error) && contains(error, "Select tiles first"));
	// Saving the view writes the .til with it.
	session.handle(request::save(m));
	TEST_EXPECT(!document->unsaved());
	const std::vector<uint8_t> saved = test_io::read_file(root + "/" + til);
	TEST_EXPECT(load_til(saved.data(), saved.size(), made, error) && made.entries.size() == 3);

	// The type's project check (S25 B5). A tile set of two cells (128 x 64): the mission's tiles 2 and 7 name cells
	// past it, which the game draws from the set's last row.
	std::vector<uint8_t> set_pixels(size_t(128) * 64 * 4, 90), set;
	std::string why;
	TEST_EXPECT(tga::tga_write_rgba32(set_pixels.data(), 128, 64, set, why) && editor_test::write_bytes(root + "/terrain/mnml_t.tga", set));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const auto finding = [&](const char *code, const std::string &asset) -> const Diagnostic * {
		for (const Diagnostic &d : v.findings.diagnostics)
			if (d.code() == code && d.asset == asset) return &d;
		return nullptr;
	};
	const Diagnostic *past = finding("til.index_past_atlas", til);
	TEST_EXPECT(past && past->severity == DiagnosticSeverity::Warning && contains(past->message, "2 tiles name cells past the 2"));
	TEST_EXPECT(!finding("til.empty", til));
	// Every tile removed and saved: the mission's .til hides the terrain's two tiles.
	std::vector<NodeId> all;
	for (const auto &row : document->rows()) all.push_back(row->id);
	session.handle(request::tile_edit(m, til, plan_tile_delete(*document, all)));
	session.handle(request::save(m));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const Diagnostic *empty = finding("til.empty", til);
	TEST_EXPECT(empty && contains(empty->message, "Tmap_t.til's 2 tiles") && !finding("til.index_past_atlas", til));
	std::printf("tiles in a mission: made seeded, laid, selected, turned, flipped, layered, dragged, stamped, removed, saved\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_tiles_in_a_mission();
	if (failures == 0) std::printf("editor_tile_edit: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
