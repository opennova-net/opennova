// The terrain brush (ADR 0046 S25): its tools' tokens, families and planes; a dab's weight (full inside the falloff,
// a smoothstep to the rim, hard-edged on a categorical plane, one texel at radius 0); the dabs along a pointer's path
// (the press's point one, then one every step, the walk carried across frames, none for a pointer held still); and
// the brush on the wire (written and read back, its bounds refused).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/terrain_brush.h>
#include <editor/session/view/terrain_tools_view.h>

#include "common/test_expect.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int test_tools() {
	for (size_t i = 0; i < kTerrainToolCount; ++i) {
		const TerrainTool tool = static_cast<TerrainTool>(i);
		TerrainTool back = TerrainTool::kCount;
		TEST_EXPECT(terrain_tool_from_token(terrain_tool_token(tool), back) && back == tool);
	}
	TerrainPlane plane = TerrainPlane::kCount;
	TEST_EXPECT(terrain_tool_plane(TerrainTool::Smooth, plane) && plane == TerrainPlane::Heights);
	TEST_EXPECT(terrain_tool_plane(TerrainTool::Detail, plane) && plane == TerrainPlane::Blend);
	TEST_EXPECT(!terrain_tool_plane(TerrainTool::Stamp, plane) && !terrain_tool_plane(TerrainTool::None, plane));
	TEST_EXPECT(terrain_tool_family(TerrainTool::Colour) == TerrainToolFamily::Paint &&
	            terrain_tool_family(TerrainTool::Tiles) == TerrainToolFamily::Tiles);
	TEST_EXPECT(terrain_plane_categorical(TerrainPlane::Surface) && !terrain_plane_categorical(TerrainPlane::Colour));
	std::printf("tools: tokens, families and planes\n");
	return 0;
}

int test_weight() {
	TerrainBrush brush;
	brush.radius = 10.0f;
	brush.falloff = 0.5f;
	TEST_EXPECT(terrain_brush_weight(brush, 0.0, false) == 1.0f && terrain_brush_weight(brush, 5.0, false) == 1.0f);
	TEST_EXPECT(near(terrain_brush_weight(brush, 7.5, false), 0.5) && terrain_brush_weight(brush, 10.0, false) == 0.0f);
	TEST_EXPECT(terrain_brush_weight(brush, 10.5, false) == 0.0f && terrain_brush_weight(brush, 9.0, true) == 1.0f);
	brush.radius = 0.0f;
	TEST_EXPECT(terrain_brush_weight(brush, 0.0, true) == 1.0f && terrain_brush_weight(brush, 0.1, true) == 0.0f);
	std::printf("weight: full, smoothstep, hard-edged, one texel\n");
	return 0;
}

int test_dabs() {
	TerrainBrush brush;
	brush.radius = 4.0f;
	brush.spacing = 0.5f;
	const double step = terrain_dab_step(brush, 1.0);
	TEST_EXPECT(near(step, 2.0) && near(terrain_dab_step(TerrainBrush{}, 64.0), 32.0));
	TerrainDabPath path;
	std::vector<TerrainPoint> dabs;
	path.begin({0.0, 0.0}, dabs);
	TEST_EXPECT(dabs.size() == 1);
	path.to({3.0, 0.0}, step, dabs); // one at 2, a metre carried
	TEST_EXPECT(dabs.size() == 2 && near(dabs[1].x, 2.0));
	path.to({3.0, 0.0}, step, dabs); // held still: nothing
	TEST_EXPECT(dabs.size() == 2);
	path.to({7.0, 0.0}, step, dabs); // at 4 and 6
	TEST_EXPECT(dabs.size() == 4 && near(dabs[2].x, 4.0) && near(dabs[3].x, 6.0));
	std::printf("dabs: the press, every step, the walk carried, none held still\n");
	return 0;
}

int test_wire() {
	TerrainBrush brush;
	brush.tool = TerrainTool::Colour;
	brush.colour = 0x12AB9F;
	brush.has_cursor = true;
	brush.cursor[0] = 3.5;
	brush.cursor[1] = -2.0;
	TerrainBrush back;
	uint32_t named = 0;
	std::string error;
	TEST_EXPECT(read_terrain_brush(terrain_brush_to_json(brush), back, named, false, error) && back == brush);
	TEST_EXPECT(named == (kBrushTool | kBrushRadius | kBrushStrength | kBrushFalloff | kBrushSpacing | kBrushHeight |
	                      kBrushSurface | kBrushFoliage | kBrushColour | kBrushLayer | kBrushCell | kBrushRotation |
	                      kBrushFlipH | kBrushFlipV | kBrushCursor));
	JsonValue json;
	TEST_EXPECT(opennova::io::json_parse(R"({"rotation":4})", json, error));
	TEST_EXPECT(!read_terrain_brush(json, back, named, false, error) && back == brush);
	TEST_EXPECT(opennova::io::json_parse(R"({"cell":3})", json, error));
	TEST_EXPECT(!read_terrain_brush(json, back, named, true, error)); // a stroke's brush takes no tile member
	std::printf("wire: the brush read back, its bounds refused\n");
	return 0;
}

} // namespace

int main() {
	if (test_tools() != 0) return 1;
	if (test_weight() != 0) return 1;
	if (test_dabs() != 0) return 1;
	return test_wire();
}
