// The heights' brushes (ADR 0046 S25, lane A4): each operator over the input plane (Raise and Lower by strength units
// a dab at full weight, Smooth towards the mean of TrnGen's 2 x 2 kernel and its mirror read as the dab found the
// heights, Flatten and Set towards their target, strength 1 an exact plateau), the clamps, the regions a stroke leaves
// due (one rectangle a leaf cell); a dab's texels as the game routes the mission to the atlas (a unit's texel, a tiled
// layout's repeats at their strongest weight, nothing off the terrain); and through the session, a stroke on a
// terrain's heights baked as it ends (the bake landing in the stroke's own step: one undo takes both), its tiles
// remade and the rest kept, and a save writing the baked .cpt.
#include <editor/documents/terrain_heights_brush.h>
#include <editor/documents/terrain_heights_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/terrain_heights_plan.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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

// Polls until the heights at `path` have no region due, at most `seconds`.
bool settled(ProjectSession &session, const std::string &path, double seconds) {
	const auto start = std::chrono::steady_clock::now();
	while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
		session.poll();
		for (const auto &document : session.view().documents.open)
			if (document->path() == path)
				if (const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(document.get()))
					if (heights->heights().unbaked.empty() && heights->heights().bake) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return false;
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
	TEST_EXPECT(settled(session, cpt->relative_path, 120.0));
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

} // namespace

int main() {
	const int failures = test_operators() + test_dabs() + test_session();
	if (failures == 0) std::printf("editor_heights_brushes: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
