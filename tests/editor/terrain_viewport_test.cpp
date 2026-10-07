// The terrain viewport (the deep-integration plan's DI-30b; editor/preview/terrain_viewport.h): a terrain drawn on its
// own, the Document tab's main view beside its records. Its kind's row; through a session over the Tmap fixture: opened,
// its device asked to make the picture; drawn under the environment of the mission that runs on it (its header's
// environment, tile set, tiles and overrides) or the engine's own; the ground as the game reads it (the terrain, the
// mission's tiles, else the terrain's own polytrn_tileinfo); the camera framed on the mapped sectors within the fog;
// DI-29's overlays an Update; the ground under a point in the game's words (DI-07); an edit of the terrain building
// the picture again; a terrain the gate refuses; the wire's options, its refusals and its commands.
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/document_types.h>
#include <editor/documents/terrain_document.h>
#include <editor/preview/terrain_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/til/til.h>
#include <formats/til/til_io.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

int test_kind() {
	TEST_EXPECT(std::string(viewport_kind_token(ViewportKind::Terrain)) == "terrain");
	ViewportKind named = ViewportKind::kCount;
	TEST_EXPECT(viewport_kind_from_token("terrain", named) && named == ViewportKind::Terrain);
	const ViewportKindRow &row = viewport_kind_row(ViewportKind::Terrain);
	// The Document tab's main view, read as Save would write it, two devices at most (each holds a terrain).
	TEST_EXPECT(row.role == ViewportRole::Main && row.as_saved && !row.part && row.canvas && row.devices == 2);
	TEST_EXPECT(main_viewport_kind(DocumentTypeId::Terrain) == ViewportKind::Terrain);
	TEST_EXPECT(default_viewport_kind(DocumentTypeId::Terrain) == ViewportKind::Terrain);
	TEST_EXPECT(preview_kind_of(DocumentTypeId::Terrain) == ViewportKind::kCount);
	std::printf("kind: the terrain's, the Main role, as saved, two devices\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{ "opennova_editor_terrain_viewport" };
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	editor_test::FakeDevices devices;
	const std::string path = "Tmap.trn";
	std::string root() const { return session.view().project.root; }
	const TerrainViewport *viewport() {
		return static_cast<const TerrainViewport *>(session.viewports().find(path, ViewportKind::Terrain));
	}
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	bool set(const std::string &change) {
		session.handle(request::set_viewport(path, change));
		const bool done = session.outcome().done();
		pump();
		return done;
	}
	JsonValue query(const char *op, double x = -1.0, double y = -1.0) {
		JsonValue args = JsonValue::make_object();
		args.set("path", opennova::io::json_string(path));
		args.set("op", opennova::io::json_string(op));
		if (x >= 0.0) {
			args.set("x", opennova::io::json_number(x));
			args.set("y", opennova::io::json_number(y));
		}
		std::string error;
		return session.query("viewport", args, error);
	}
	JsonValue body() {
		const JsonValue answer = query("state");
		const JsonValue *body = answer.get("body");
		return body ? *body : JsonValue();
	}
};

const JsonValue *at(const JsonValue &value, const char *a, const char *b = nullptr) {
	const JsonValue *one = value.get(a);
	return b && one ? one->get(b) : one;
}

int write_mission(const std::string &root, const char *name, const char *tile_set) {
	opennova::bms::File file;
	opennova::mission::make_default(file);
	std::string why;
	TEST_EXPECT(opennova::mission::set_header_string(file, "terrain", "Tmap", why) &&
	            opennova::mission::set_header_string(file, "environment", "day", why) &&
	            opennova::mission::set_header_string(file, "terrain_tile", tile_set, why) &&
	            opennova::mission::set_header_int(file, "start_time", 9 << 8, why) &&
	            opennova::mission::set_header_int(file, "minutes_per_day", 60, why));
	std::vector<uint8_t> bytes;
	TEST_EXPECT(opennova::bms::write(file, bytes, why));
	TEST_EXPECT(editor_test::write_bytes(root + "/" + name, bytes));
	return 0;
}

std::vector<uint8_t> til_of(int tiles) {
	opennova::TilFile til;
	for (int i = 0; i < tiles; ++i) til.entries.push_back(opennova::make_til_overlay_entry(-2 + i, 1, uint8_t(i), 0));
	std::vector<uint8_t> out;
	std::string error;
	opennova::save_til(til, out, error);
	return out;
}

int test_session(const std::string &repo) {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.dir.file("project"), "Terrain"));
	editor_test::create_missing_files(rig.session);
	const std::string root = rig.root();
	const std::string fixtures = repo + "/fixtures/terrain/tmap/";
	// Tmap with its own tile placement (own.til, two tiles), and a mission on it placing three of its own.
	const std::vector<uint8_t> fixture = test_io::read_file(fixtures + "Tmap.trn");
	std::string trn(fixture.begin(), fixture.end());
	trn += "\r\npolytrn_tileinfo own.til\r\n";
	TEST_EXPECT(editor_test::write_text(root + "/Tmap.trn", trn));
	for (const char *name : { "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx" })
		TEST_EXPECT(editor_test::write_bytes(root + "/" + name, test_io::read_file(fixtures + name)));
	TEST_EXPECT(editor_test::write_bytes(root + "/own.til", til_of(2)));
	TEST_EXPECT(editor_test::write_bytes(root + "/landing.til", til_of(3)));
	opennova::env::Config day;
	day.fog_level = 800.0f;
	day.sky_height = 175.0f;
	std::ostringstream text;
	std::string error;
	TEST_EXPECT(opennova::env::save_env(text, day, error));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", text.str()));
	TEST_EXPECT(write_mission(root, "landing.bms", "rock.tga") == 0);
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.pump();

	// Opened: its records beside its picture, the device asked to make it.
	editor_test::handle_to_end(rig.session, request::open_document(rig.path));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	const TerrainViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready && viewport->terrain_name() == "Tmap");
	if (!viewport) return 1;
	editor_test::FakeDevice *device = rig.devices.held(rig.path, ViewportKind::Terrain);
	TEST_EXPECT(device && device->since(0) == std::vector<ViewportAction>{ ViewportAction::Rebuild });
	rig.pump();
	TEST_EXPECT(device && device->last() == ViewportAction::Keep);

	// Under landing.bms, the one mission on it: its environment, tile set and tiles (its own .til before the
	// terrain's), its clock.
	TEST_EXPECT(viewport->uses().missions.size() == 1 && viewport->mission() && viewport->mission()->mission == "landing.bms");
	TEST_EXPECT(viewport->header().terrain == "Tmap" && viewport->header().environment == "day" &&
	            viewport->header().tile_set == "rock.tga" && viewport->header().start_time == (9 << 8) &&
	            viewport->mission_name() == "landing");
	TEST_EXPECT(viewport->ground().terrain() && viewport->ground().tiles_file() == "landing.til" &&
	            viewport->ground().tiles() == 3 && viewport->ground().surface_map() == "Tmap_m.pcx");
	// Framed on the mapped sectors (Tmap's 2 x 2 in its 8 x 8 grid from -4, -4: the middle at the origin), no
	// farther than the fog lets it see (the day's 800 m).
	TEST_EXPECT(viewport->fog_reach() > 0.0f && viewport->camera().distance <= viewport->fog_reach() + 0.01f);
	TEST_EXPECT(std::fabs(viewport->camera().target.x) < 1.0f && std::fabs(viewport->camera().target.z) < 1.0f);
	JsonValue body = rig.body();
	TEST_EXPECT(at(body, "mission", "path") && at(body, "mission", "path")->string == "landing.bms" &&
	            at(body, "ground", "tiles_file")->string == "landing.til" && at(body, "ground", "grid") &&
	            at(body, "ground", "grid")->get_number("width", 0) == 8.0);

	// Under the engine's own environment: no mission, the terrain's own tiles (own.til) as the game falls back on
	// them [orig: Terrain_Init @ 0x60FCFD], a Rebuild.
	TEST_EXPECT(rig.set(R"({"kind": "terrain", "options": {"mission": "none"}})"));
	TEST_EXPECT(!viewport->mission() && viewport->header().environment.empty() && viewport->mission_name().empty());
	TEST_EXPECT(viewport->ground().tiles_file() == "own.til" && viewport->ground().tiles() == 2);
	TEST_EXPECT(device && device->last() == ViewportAction::Rebuild);
	bool neutral = false;
	const JsonValue state = rig.query("state");
	if (const JsonValue *notes = state.get("notes"))
		for (const JsonValue &note : notes->array) neutral |= note.get_string("code", "") == "terrain.neutral";
	TEST_EXPECT(neutral && at(rig.body(), "mission") && at(rig.body(), "mission")->is_null());

	// DI-29's overlays: an Update, the legend in the body.
	const uint64_t serial = viewport->overlay_serial();
	TEST_EXPECT(rig.set(R"({"kind": "terrain", "options": {"overlay": "surfaces"}})"));
	TEST_EXPECT(viewport->overlay().kind == MissionGroundOverlay::Surfaces && viewport->overlay_serial() > serial &&
	            !viewport->overlay().legend.empty());
	TEST_EXPECT(device && device->last() == ViewportAction::Update);
	body = rig.body();
	TEST_EXPECT(at(body, "overlay", "kind") && at(body, "overlay", "kind")->string == "surfaces");
	TEST_EXPECT(rig.set(R"({"kind": "terrain", "options": {"overlay": "foliage", "show": {"foliage": false}}})"));
	TEST_EXPECT(viewport->overlay().kind == MissionGroundOverlay::Foliage && !viewport->options().foliage);

	// The ground under a point (DI-07), over the device's surface: a flat ground at the terrain's height.
	rig.session.viewports().set_devices(&rig.devices.cache);
	if (device) device->ground = [](double, double) { return 30.0; };
	const JsonValue hit = rig.query("hit", 512.0, 500.0);
	const JsonValue *ground = hit.get("ground");
	TEST_EXPECT(ground && ground->get_string("on", "") == "terrain" && ground->get("surface") &&
	            !ground->get_string("line", "").empty() && ground->get("footstep"));
	rig.session.viewports().set_devices(nullptr);

	// The wire's refusals: a mission that does not run on it, an option it does not take.
	TEST_EXPECT(!rig.set(R"({"kind": "terrain", "options": {"mission": "other.bms"}})"));
	TEST_EXPECT(!rig.set(R"({"kind": "terrain", "options": {"weather": 1}})"));
	TEST_EXPECT(!rig.set(R"({"kind": "terrain", "options": {"overlay": "heights"}})"));
	// The commands: top (straight down, north up) and frame (back on the mapped sectors), each a SetViewport of the
	// camera; another refused, naming them.
	{
		editor_test::Gathered gathered;
		std::string why;
		const ViewportContext context = viewport_context(rig.session.view(), *viewport);
		TEST_EXPECT(viewport->command(context, "top", {}, gathered, why) && gathered.requests.size() == 1);
		for (const EditorRequest &request : gathered.requests) rig.session.handle(request);
		rig.pump();
		TEST_EXPECT(viewport->camera().pitch > 1.5f);
		gathered.requests.clear();
		TEST_EXPECT(viewport->command(context, "frame", {}, gathered, why) && gathered.requests.size() == 1);
		for (const EditorRequest &request : gathered.requests) rig.session.handle(request);
		rig.pump();
		TEST_EXPECT(viewport->camera().pitch < 1.0f);
		TEST_EXPECT(!viewport->command(context, "dance", {}, gathered, why) && why.find("frame, top") != std::string::npos);
	}

	// An edit of the terrain builds the picture again; one the gate refuses leaves no ground to draw.
	Document *open = rig.session.document_for(rig.path);
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	const NodeAddress row{ open->rows().front()->id, node_kind(TerrainKind::Terrain), 0 };
	Edit water;
	water.address = row;
	water.field = "water_height";
	water.value = int64_t(60);
	editor_test::handle_to_end(rig.session, request::edit_record(rig.path, water));
	rig.pump();
	TEST_EXPECT(device && device->last() == ViewportAction::Rebuild && std::fabs(viewport->ground().water_height() - 30.0) < 1e-6);
	Edit unnamed;
	unnamed.address = row;
	unnamed.field = "polytrn_colormap";
	unnamed.value = std::string();
	editor_test::handle_to_end(rig.session, request::edit_record(rig.path, unnamed));
	rig.pump();
	TEST_EXPECT(viewport->view_status() == TerrainViewStatus::Refused && viewport->status() == ViewportStatus::Failed &&
	            viewport->message().find("colour map") != std::string::npos);
	editor_test::handle_to_end(rig.session, request::undo(rig.path));
	rig.pump();
	TEST_EXPECT(viewport->view_status() == TerrainViewStatus::Ready);
	std::printf("session: under its mission and the engine's own environment, its tiles and the terrain's own, framed in "
	            "the fog, the overlays, the ground under a point, the wire's refusals, an edit, a refusal\n");
	return 0;
}

} // namespace

int main() {
	const std::string repo = test_paths_repo_root(__FILE__);
	int failures = test_kind();
	failures += test_session(repo);
	if (failures == 0) std::printf("editor_terrain_viewport: all passed\n");
	return failures == 0 ? 0 : 1;
}
