// The terrain family (ADR 0046 S25): the files a view's brush edits, as the game resolves them for it. A mission's
// planes are the files its terrain's .trn names, read through overcast.def and its .env (D-TERRAIN-18), its heights
// the .cpt the .trn names, another terrain's where it names one (D-TERRAIN-17), its tiles its own <mission>.til; a
// plane the .trn names no file for, or one the project lacks, refused with the fix (the pane's Add or Import); a
// terrain's own tiles under no mission. The terrain query's family, brush and sample ops answer them; a stroke on a
// plane resolves its file and opens it quietly (its planner not in yet: refused, the file open, the active document
// kept).
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
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

} // namespace

int main() { return test_routes(); }
