// The heights' brushes (ADR 0046 S25, lane A4): each operator over the input plane (Raise and Lower by strength units
// a dab at full weight, Smooth towards the mean of TrnGen's 2 x 2 kernel and its mirror read as the dab found the
// heights, Flatten and Set towards their target, strength 1 an exact plateau), the clamps, the regions a stroke leaves
// due (one rectangle a leaf cell); a dab's texels as the game routes the mission to the atlas (a unit's texel, a tiled
// layout's repeats at their strongest weight, nothing off the terrain); and through the session, a stroke on a
// terrain's heights baked as it ends (the bake landing in the stroke's own step: one undo takes both), its tiles
// remade and the rest kept, and a save writing the baked .cpt.
#include <editor/documents/terrain_heights_brush.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/terrain_heights_patch.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_heights_plan.h>
#include <editor/session/terrain_sculpt.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>
#include <formats/png/png_encode.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

std::string fixture(const char *relative) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative; }

std::unique_ptr<TerrainHeightsDocument> open_tmap() {
	auto document = std::make_unique<TerrainHeightsDocument>();
	Diagnostic error;
	if (!document->load_bytes(test_io::read_file(fixture("terrain/tmap/Tmap.cpt")), "terrain/Tmap.cpt",
	                          AssetKind::TerrainPolyData, "jo", error))
		return nullptr;
	return document;
}

bool stroke(TerrainHeightsDocument &document, HeightsOperator op, float strength, int32_t target,
            std::vector<std::vector<HeightsDabTexel>> dabs, int32_t top = 65535) {
	auto edit = std::make_shared<HeightsStrokeEdit>();
	edit->op = op;
	edit->strength = strength;
	edit->target = target;
	edit->top = top;
	edit->dabs = std::move(dabs);
	Diagnostic error;
	return document.apply(RasterDocument::edit(edit), error);
}

int test_operators() {
	std::unique_ptr<TerrainHeightsDocument> doc = open_tmap();
	TEST_EXPECT(doc);
	const int h = doc->input().u16(312, 312), side = doc->input().u16(313, 312);
	TEST_EXPECT(stroke(*doc, HeightsOperator::Raise, 0.5f, 0, {{{312, 312, 1.0f}, {313, 312, 0.5f}}}));
	TEST_EXPECT(doc->input().u16(312, 312) == h + 128 && doc->input().u16(313, 312) == side + 64);
	TEST_EXPECT(doc->atlas().u16(312, 312) == h); // the baked atlas waits for the bake
	TEST_EXPECT(doc->unbaked_at(312, 312) && doc->live_height(312, 312) == h + 128);
	// Two dabs: the second from what the first left.
	TEST_EXPECT(stroke(*doc, HeightsOperator::Lower, 1.0f, 0, {{{312, 312, 1.0f}}, {{312, 312, 1.0f}}}));
	TEST_EXPECT(doc->input().u16(312, 312) == std::max(0, h + 128 - 512));
	// The clamps: [0, top].
	TEST_EXPECT(stroke(*doc, HeightsOperator::Lower, 1.0f, 0, std::vector<std::vector<HeightsDabTexel>>(300, {{400, 400, 1.0f}})));
	TEST_EXPECT(doc->input().u16(400, 400) == 0);
	TEST_EXPECT(stroke(*doc, HeightsOperator::Raise, 1.0f, 0, std::vector<std::vector<HeightsDabTexel>>(300, {{401, 400, 1.0f}}), 30000));
	TEST_EXPECT(doc->input().u16(401, 400) == 30000);
	// Set and Flatten at full strength: the target exactly; at half, halfway.
	TEST_EXPECT(stroke(*doc, HeightsOperator::Set, 1.0f, 12345, {{{500, 500, 1.0f}, {501, 500, 0.5f}}}));
	TEST_EXPECT(doc->input().u16(500, 500) == 12345);
	TEST_EXPECT(stroke(*doc, HeightsOperator::Flatten, 1.0f, 7000, {{{502, 500, 1.0f}}}));
	TEST_EXPECT(doc->input().u16(502, 500) == 7000);
	// Smooth: towards the mean of the kernel and its mirror, read before the dab.
	TEST_EXPECT(stroke(*doc, HeightsOperator::Set, 1.0f, 20000, {{{600, 600, 1.0f}}}));
	const auto at = [&](int x, int z) { return int(doc->input().u16(x, z)); };
	const int ahead = (at(600, 600) + at(601, 600) + at(600, 601) + at(601, 601)) >> 2;
	const int behind = (at(600, 600) + at(599, 600) + at(600, 599) + at(599, 599)) >> 2;
	const int spike = at(600, 600);
	TEST_EXPECT(stroke(*doc, HeightsOperator::Smooth, 1.0f, 0, {{{600, 600, 1.0f}}}));
	TEST_EXPECT(at(600, 600) == int(std::floor((ahead + behind) * 0.5 + 0.5)) && at(600, 600) < spike);
	// The regions due: one rectangle a leaf cell, each its texels' bounds.
	const std::vector<HeightsRect> regions = heights_regions({{312, 312, 1}, {313, 314, 1}, {330, 312, 1}, {700, 70, 1}});
	TEST_EXPECT(regions.size() == 3);
	std::vector<HeightsRect> due = {{312, 312, 2, 3}};
	join_heights_regions(due, {{316, 316, 4, 4}, {800, 800, 1, 1}});
	TEST_EXPECT(due.size() == 2 && due[0] == (HeightsRect{312, 312, 8, 8}));
	std::printf("operators: raise, lower, the clamps, set, flatten, smooth, the regions due\n");
	return 0;
}

int test_dabs() {
	TrnConfig trn;
	trn.origin_x = -4;
	trn.origin_y = -4;
	trn.sector_grid[3][3] = 1;
	trn.sector_grid[3][4] = 3;
	trn.sector_grid[4][3] = 2;
	trn.sector_grid[4][4] = 4;
	TerrainBrush brush;
	brush.radius = 0.0f;
	// Mission (-200, 200): sector (3, 3), quadrant 1 at (312, 312).
	std::vector<HeightsDabTexel> one = heights_dab_texels(trn, brush, {-200.0, 200.0});
	TEST_EXPECT(one.size() == 1 && one[0].x == 312 && one[0].z == 312 && one[0].weight == 1.0f);
	// Quadrant 3 (grid value 3: +512 on x) east of it.
	one = heights_dab_texels(trn, brush, {200.0, 200.0});
	TEST_EXPECT(one.size() == 1 && one[0].x == 512 + 200 && one[0].z == 312);
	// A disc: its units' texels, the centre full, the rim light.
	brush.radius = 4.0f;
	brush.falloff = 0.5f;
	std::vector<HeightsDabTexel> disc = heights_dab_texels(trn, brush, {-200.0, 200.0});
	TEST_EXPECT(disc.size() > 40);
	float centre = 0.0f, mid = 0.0f;
	bool rim = false;
	for (const HeightsDabTexel &t : disc) {
		if (t.x == 312 && t.z == 312) centre = t.weight;
		if (t.x == 315 && t.z == 312) mid = t.weight; // 3 units out: halfway down the falloff (2 to 4)
		rim = rim || (t.x == 316 && t.z == 312);       // at the radius: weight 0, not taken
	}
	TEST_EXPECT(centre == 1.0f && std::fabs(mid - 0.5f) < 1e-5f && !rim);
	// Off the terrain (an empty sector): nothing.
	TEST_EXPECT(heights_dab_texels(trn, brush, {-1500.0, 1500.0}).empty());
	// A tiled layout: two sectors drawing quadrant 1 side by side; a disc wide enough to reach the same texel through
	// both takes it once, at its strongest weight.
	TrnConfig tiled = trn;
	tiled.sector_grid[3][4] = 1;
	brush.radius = 300.0f;
	brush.falloff = 1.0f;
	const std::vector<HeightsDabTexel> wide = heights_dab_texels(tiled, brush, {0.0, 200.0});
	size_t at_10 = 0;
	float strongest = 0.0f;
	for (const HeightsDabTexel &t : wide)
		if (t.x == 10 && t.z == 312) {
			++at_10;
			strongest = t.weight;
		}
	// Units x = 10 (sector 4) and x = -502 (sector 3) both route to texel 10; the nearer weighs more.
	TEST_EXPECT(at_10 == 1 && strongest > 0.9f);
	std::printf("dabs: a unit's texel, a disc, off the terrain, a tiled layout's repeats once at their strongest\n");
	return 0;
}

int test_session() {
	editor_test::TempProjectDir dir("opennova_editor_heights_brushes");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Heights"));
	const std::string root = session.view().project.root;
	const std::vector<uint8_t> cpt_bytes = test_io::read_file(fixture("terrain/tmap/Tmap.cpt"));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.cpt", cpt_bytes));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn"))));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &v = session.view();
	const AssetEntry *trn = v.project.scan->find("Tmap.trn");
	const AssetEntry *cpt = v.project.scan->find("Tmap.cpt");
	TEST_EXPECT(trn && cpt && cpt->kind == AssetKind::TerrainPolyData);
	session.handle(request::open_document(trn->relative_path));
	// A raise across the quadrant at mission (-200, 200), atlas (312, 312), ended with its sample.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Heights;
	ask.brush.tool = TerrainTool::Raise;
	ask.brush.radius = 8.0f;
	ask.brush.strength = 1.0f;
	ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
	ask.points = {{-200.0, 200.0}, {-196.0, 200.0}};
	ask.end = true;
	session.handle(request::terrain_stroke(trn->relative_path, ask));
	TEST_EXPECT(session.outcome().done() && !session.outcome().stroke.changed.empty());
	const auto heights_doc = [&]() -> const TerrainHeightsDocument * {
		for (const auto &document : v.documents.open)
			if (document->path() == cpt->relative_path) return dynamic_cast<const TerrainHeightsDocument *>(document.get());
		return nullptr;
	};
	TEST_EXPECT(heights_doc() && heights_doc()->dirty() && !heights_doc()->heights().unbaked.empty());
	// The bake lands in the stroke's step: the regions settled, the atlas there drawn back from the remade leaf.
	TEST_EXPECT(editor_test::heights_settled(session, cpt->relative_path));
	const TerrainHeightsDocument *heights = heights_doc();
	CptFile shipped;
	std::string why;
	TEST_EXPECT(load_cpt(cpt_bytes.data(), cpt_bytes.size(), shipped, why));
	TEST_EXPECT(heights->atlas().u16(312, 312) != shipped.depth_buffer[312 * 1024 + 312]);
	TEST_EXPECT(heights->atlas().u16(900, 900) == shipped.depth_buffer[900 * 1024 + 900]);
	size_t remade = 0;
	for (size_t i = 0; i < heights->heights().tiles.size(); ++i) {
		const CptTile &a = *heights->heights().tiles[i], &b = shipped.tiles[i];
		remade += a.vertex_indices != b.vertex_indices || a.lods[0].indices != b.lods[0].indices;
	}
	TEST_EXPECT(remade >= 1 && remade <= 8);
	// One undo takes the stroke and its bake.
	session.handle(request::undo(cpt->relative_path));
	heights = heights_doc();
	TEST_EXPECT(heights && !heights->dirty() && heights->serialize().text == std::string(cpt_bytes.begin(), cpt_bytes.end()));
	session.handle(request::redo(cpt->relative_path));
	heights = heights_doc();
	TEST_EXPECT(heights && heights->heights().unbaked.empty() && heights->dirty());
	// A save writes the baked .cpt.
	session.handle(request::save(cpt->relative_path));
	heights = heights_doc();
	TEST_EXPECT(heights && !heights->dirty());
	const std::vector<uint8_t> saved = test_io::read_file(root + "/terrain/Tmap.cpt");
	CptFile back;
	TEST_EXPECT(load_cpt(saved.data(), saved.size(), back, why) && back.tiles.size() == shipped.tiles.size());
	TEST_EXPECT(back.depth_buffer[312 * 1024 + 312] == heights->atlas().u16(312, 312));
	std::printf("session: a stroke baked as it ends, one undo for both, a save of the baked .cpt\n");
	return 0;
}

// A stroke in the mission's 3D view (S25 A4, the device hook's portable half): the view's picture patched in place,
// never made again from the .cpt (its device takes an Update, its builds stand, its ground is not read again): the
// stroke's regions as the live heights, the ground the game collides with patched under them; the bake's landing the
// tiles it replaced; the undo the shipped heights and tiles back.
int test_view() {
	editor_test::TempProjectDir dir("opennova_editor_heights_view");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Heights"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	const std::vector<uint8_t> cpt_bytes = test_io::read_file(fixture("terrain/tmap/Tmap.cpt"));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.cpt", cpt_bytes));
	TEST_EXPECT(editor_test::write_bytes(root + "/terrain/Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn"))));
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
	const std::string m = v.project.scan->find("m.bms")->relative_path;
	const std::string cpt = v.project.scan->find("Tmap.cpt")->relative_path;
	session.handle(request::open_document(m));
	devices.sync(session);
	const auto *mission = static_cast<const MissionViewport *>(session.viewports().find(m, ViewportKind::Mission));
	TEST_EXPECT(mission && devices.last(m, ViewportKind::Mission) == ViewportAction::Rebuild);
	if (!mission) return 1;
	const uint64_t builds = mission->builds();
	// The shipped height under the stroke, from the file (the view's ground is not read before the stroke: a stroke
	// before anything asked the ground patches nothing, its first read the document's state).
	CptFile shipped;
	std::string why;
	TEST_EXPECT(load_cpt(cpt_bytes.data(), cpt_bytes.size(), shipped, why));
	const uint16_t before = shipped.depth_buffer[312 * 1024 + 312];
	const MissionGround &ground = mission->ground_reader();
	const auto at = [&](int x, int y) {
		return ground.height_field() ? ground.height_field()->heightmap[size_t(y) * 1024 + size_t(x)] : uint16_t(0);
	};

	// A raise at mission (-200, 200), atlas (312, 312), its stroke open: an Update of the picture, its heights patched.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Heights;
	ask.brush.tool = TerrainTool::Raise;
	ask.brush.radius = 8.0f;
	ask.brush.strength = 1.0f;
	ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
	ask.points = {{-200.0, 200.0}};
	ask.gesture = 4242;
	ask.end = false;
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(devices.last(m, ViewportKind::Mission) == ViewportAction::Update && mission->builds() == builds);
	TEST_EXPECT(!mission->heights_patches().empty());
	if (!mission->heights_patches().empty()) {
		const HeightsDevicePatch &patch = mission->heights_patches().back();
		TEST_EXPECT(patch.path == cpt && !patch.regions.empty() && patch.tile_indices.empty() && patch.serial > 0);
		bool covers = false;
		for (const HeightsDevicePatch::Region &region : patch.regions)
			covers = covers || (312 >= region.x && 312 < region.x + region.w && 312 >= region.y && 312 < region.y + region.h &&
			                    region.heights[size_t(312 - region.y) * size_t(region.w) + size_t(312 - region.x)] == before + 256);
		TEST_EXPECT(covers);
	}
	// The ground, first asked now, reads the document's state; read once, it is patched from here on.
	TEST_EXPECT(mission->followed_ground(v).terrain() && at(312, 312) == before + 256);
	const int reads = ground.reads();
	const uint64_t revision = ground.revision();
	ask.points = {{-200.0, 200.0}};
	session.handle(request::terrain_stroke(m, ask));
	devices.sync(session);
	TEST_EXPECT(devices.last(m, ViewportKind::Mission) == ViewportAction::Update && mission->builds() == builds);
	TEST_EXPECT(mission->followed_ground(v).reads() == reads && ground.revision() > revision && at(312, 312) == before + 512);

	// The stroke ends; its bake lands: the tiles it replaced, the picture still never made again.
	ask.points = {{-196.0, 200.0}};
	ask.end = true;
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(editor_test::heights_settled(session, cpt));
	devices.sync(session);
	size_t tiles = 0;
	for (const HeightsDevicePatch &patch : mission->heights_patches()) tiles += patch.tile_indices.size();
	TEST_EXPECT(tiles >= 1 && tiles <= 8 && mission->builds() == builds && mission->followed_ground(v).reads() == reads);
	const uint16_t baked = at(312, 312);
	TEST_EXPECT(baked > before);

	// One undo: the shipped heights and tiles back, patched.
	session.handle(request::undo(m));
	devices.sync(session);
	TEST_EXPECT(at(312, 312) == before && mission->builds() == builds && mission->followed_ground(v).reads() == reads);
	tiles = 0;
	for (const HeightsDevicePatch &patch : mission->heights_patches()) tiles += patch.tile_indices.size();
	TEST_EXPECT(tiles >= 2);
	std::printf("view: a stroke patched into the mission's 3D view and its ground, its landing's tiles, its undo\n");
	return 0;
}

// A project with the Tmap terrain and a mission on it, the mission open (the view a stroke brushes in).
struct SeedRig {
	editor_test::TempProjectDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string m, cpt;
	explicit SeedRig(const char *name) : dir(name) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Heights"));
		const SessionView &v = session.view();
		const std::string root = v.project.root;
		editor_test::write_bytes(root + "/terrain/Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt")));
		editor_test::write_bytes(root + "/terrain/Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn")));
		editor_test::write_text(root + "/day.env", "env");
		editor_test::handle_to_end(session, request::rescan());
		editor_test::set_missions(session, true);
		editor_test::handle_to_end(session,
		        request::create_file("m.bms", "mission", {{"terrain", "Tmap.trn"}, {"environment", "day.env"}}));
		m = v.project.scan->find("m.bms")->relative_path;
		cpt = v.project.scan->find("Tmap.cpt")->relative_path;
		session.handle(request::open_document(m));
	}
	// One sample of a Raise in the mission's view.
	void sample(double x, double y, bool end) {
		TerrainStrokeAsk ask;
		ask.plane = TerrainPlane::Heights;
		ask.brush.tool = TerrainTool::Raise;
		ask.brush.radius = 8.0f;
		ask.brush.strength = 1.0f;
		ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
		ask.points = {{x, y}};
		ask.gesture = 9191;
		ask.end = end;
		session.handle(request::terrain_stroke(m, ask));
	}
	const TerrainHeightsDocument *heights() {
		for (const auto &document : session.view().documents.open)
			if (document->path() == cpt) return dynamic_cast<const TerrainHeightsDocument *>(document.get());
		return nullptr;
	}
};

// The seed (the first bake's state) made in the background as Sculpt is picked over the mission's terrain, before
// its heights file opens: a stroke started while it runs is served at once, its bake landing after it, the same
// bytes a stroke whose seed is made at its end lands. The seed is held on the worker (a test's hold) while the strokes
// are served: each is served with the seed still the worker's and not begun (a stroke that waited for it would never
// return), its regions left due for the bake after it.
int test_seed() {
	BakeTestHold hold;
	SeedRig early("opennova_editor_heights_seed_early");
	early.session.hold_bakes_for_test(&hold);
	early.session.handle(request::set_workspace(R"({"terrain_brush": {"tool": "raise"}})"));
	early.session.poll();
	const SessionView &v = early.session.view();
	TEST_EXPECT(v.documents.preparing == early.cpt);
	early.session.wait_for_bakes(); // the worker holds the seed
	early.sample(-200.0, 200.0, false);
	early.sample(-196.0, 200.0, false);
	early.sample(-192.0, 200.0, true);
	TEST_EXPECT(early.session.outcome().done() && early.heights() && early.heights()->dirty());
	TEST_EXPECT(v.documents.preparing == early.cpt && early.heights() && !early.heights()->heights().unbaked.empty());
	early.session.hold_bakes_for_test(nullptr);
	TEST_EXPECT(editor_test::heights_settled(early.session, early.cpt));

	// The same strokes with no seed made ahead (the file's seed made as the stroke ends).
	SeedRig late("opennova_editor_heights_seed_late");
	late.sample(-200.0, 200.0, false);
	late.sample(-196.0, 200.0, false);
	late.sample(-192.0, 200.0, true);
	TEST_EXPECT(editor_test::heights_settled(late.session, late.cpt));
	TEST_EXPECT(early.heights() && late.heights() && early.heights()->serialize().text == late.heights()->serialize().text);
	std::printf("seed: prepared in the background as Sculpt is picked, a stroke served meanwhile, its bake landing after\n");
	return 0;
}

// The session closed while a seed runs (the project or the editor closed as Sculpt is picked): the close cancels it,
// so the close does not wait out the bake of the whole file. Here the seed is held on the worker until the close (a
// test's hold): the worker's when the session goes, its bake begins under the close's cancel and comes back cancelled
// at its first node. A cancel set while a bake's walk runs, stopping it at its next node, is pinned on the walk itself
// (tests/cpt/trngen_region_bake_test's test_walk_cancelled_partway).
int test_close_during_seed() {
	BakeTestHold hold;
	auto rig = std::make_unique<SeedRig>("opennova_editor_heights_seed_close");
	rig->session.hold_bakes_for_test(&hold);
	rig->session.handle(request::set_workspace(R"({"terrain_brush": {"tool": "raise"}})"));
	rig->session.poll();
	TEST_EXPECT(rig->session.view().documents.preparing == rig->cpt);
	rig->session.wait_for_bakes(); // the worker holds the seed
	rig.reset();
	TEST_EXPECT(hold.cancelled == 1);
	std::printf("close: the session closed during its seed, the seed came back cancelled\n");
	return 0;
}

// An imported terrain sculpted (S25 A2b): a stroke in a mission on it routes to the terrain set's heightmap, opened as
// the heights under the import's white; its first bake lays its seed's whole bake (the .cpt the import makes of the
// image) under the stroke's regions, and the image then stands in for the import's .cpt in the project's files (what
// the views and the ground read); its save writes the heightmap, and the project's refresh imports it again, the
// .cpt it makes the bytes the image stood in for it with (DI-26's save-first then the build's refresh carry it).
int test_imported() {
	editor_test::TempProjectDir dir("opennova_editor_heights_imported");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Isle"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	constexpr int kSide = 1024;
	std::vector<uint16_t> samples(size_t(kSide) * kSide);
	for (int y = 0; y < kSide; ++y)
		for (int x = 0; x < kSide; ++x) {
			const double r = std::hypot(x - 512.0, y - 512.0) / 400.0;
			samples[size_t(y) * kSide + size_t(x)] = uint16_t(30000.0 * std::max(0.0, 1.0 - r));
		}
	std::vector<uint8_t> rgba(size_t(kSide) * kSide * 4, 200);
	const std::string images = dir.file("images");
	TEST_EXPECT(editor_test::write_bytes(images + "/height.png", png::encode_png_gray16(samples.data(), kSide, kSide)));
	TEST_EXPECT(editor_test::write_bytes(images + "/colour.png", png::encode_png_rgba(rgba.data(), kSide, kSide)));
	const ActionOutcome made = editor_test::handle_to_end(session, request::new_terrain("isle",
	        {{"heightmap", images + "/height.png"}, {"colormap", images + "/colour.png"}, {"top", "100"}}));
	TEST_EXPECT(!made.refused);
	TEST_EXPECT(editor_test::write_text(root + "/day.env", "env"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	editor_test::handle_to_end(session, request::create_file("m.bms", "mission", {{"terrain", "isle.trn"}, {"environment", "day.env"}}));
	const AssetEntry *mission = v.project.scan->find("m.bms");
	const AssetEntry *cpt_entry = v.project.scan->find("isle.cpt");
	const AssetEntry *image_entry = v.project.scan->find("isle_heightmap.png");
	TEST_EXPECT(mission && cpt_entry && image_entry && image_entry->kind == AssetKind::HeightmapInput);
	if (!mission || !cpt_entry || !image_entry) return 1;
	const std::string m = mission->relative_path, cpt = cpt_entry->relative_path, image = image_entry->relative_path;
	const std::vector<uint8_t> imported = test_io::read_file(root + "/" + cpt);
	session.handle(request::open_document(m));

	// A raise at the island's middle, ended: the stroke lands in the heightmap, the import's .cpt untouched.
	TerrainStrokeAsk ask;
	ask.plane = TerrainPlane::Heights;
	ask.brush.tool = TerrainTool::Raise;
	ask.brush.radius = 8.0f;
	ask.brush.strength = 1.0f;
	ask.brush_named = kBrushTool | kBrushRadius | kBrushStrength;
	ask.points = {{-4.0, 0.0}, {4.0, 0.0}};
	ask.gesture = 5151;
	ask.end = true;
	session.handle(request::terrain_stroke(m, ask));
	TEST_EXPECT(session.outcome().done());
	const auto heights_doc = [&]() -> const TerrainHeightsDocument * {
		for (const auto &document : v.documents.open)
			if (document->path() == image) return dynamic_cast<const TerrainHeightsDocument *>(document.get());
		return nullptr;
	};
	TEST_EXPECT(heights_doc() && heights_doc()->image() && heights_doc()->cpt_path() == cpt &&
	            heights_doc()->import_top() == 100.0 && heights_doc()->dirty());
	TEST_EXPECT(editor_test::heights_settled(session, image));
	const TerrainHeightsDocument *heights = heights_doc();
	TEST_EXPECT(heights && heights->heights().baked && heights->stand_in_paths() == std::vector<std::string>{cpt});
	if (!heights) return 1;
	TEST_EXPECT(test_io::read_file(root + "/" + cpt) == imported);
	// The project's files read the sculpted ground for the .cpt.
	std::vector<uint8_t> standing;
	TEST_EXPECT(v.findings.assets && v.findings.assets->read("isle.cpt", standing) && standing != imported);
	CptFile stood, was;
	std::string why;
	TEST_EXPECT(load_cpt(standing.data(), standing.size(), stood, why) && load_cpt(imported.data(), imported.size(), was, why));
	TEST_EXPECT(stood.depth_buffer[512 * 1024 + 512] > was.depth_buffer[512 * 1024 + 512] &&
	            stood.depth_buffer[100 * 1024 + 100] == was.depth_buffer[100 * 1024 + 100]);

	// Saved: the heightmap written; the refresh imports it again, its .cpt the bytes that stood in for it.
	session.handle(request::save(image));
	TEST_EXPECT(heights_doc() && !heights_doc()->dirty());
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(test_io::read_file(root + "/" + cpt) == standing);

	// The import's white changed while the heightmap is open and clean: the document read again under it (its write
	// decodes to what it shows) at the next poll, which reads the imports again (no bake: the reload's operations run).
	editor_test::handle_to_end(session, request::set_import_options(cpt, {{"top", "120"}}));
	session.poll();
	session.run_operations();
	TEST_EXPECT(heights_doc() && heights_doc()->import_top() == 120.0 && !heights_doc()->dirty());
	std::printf("imported: a stroke sculpts the heightmap, its bake stands in for the import's .cpt, the reimport makes those bytes\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_operators() + test_dabs() + test_session() + test_view() + test_seed() + test_close_during_seed() +
	                     test_imported();
	if (failures == 0) std::printf("editor_heights_brushes: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
