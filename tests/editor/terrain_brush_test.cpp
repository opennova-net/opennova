// The terrain brush (ADR 0046 S25): its tools' tokens, families and planes; a dab's weight (full inside the falloff,
// a smoothstep to the rim, hard-edged on a categorical plane, one texel at radius 0); the dabs along a pointer's path
// (the press's point one, then one every step, the walk carried across frames, none for a pointer held still); and
// the brush on the wire (written and read back, its bounds refused).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/text_document.h>
#include <editor/preview/terrain_brush.h>
#include <editor/preview/terrain_brush_canvas.h>
#include <editor/session/view/session_view.h>
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

// A flat ground under a picture one pixel a metre, 10 m high.
struct FlatSpace final : TerrainBrushSpace {
	bool pointer_ground(float x, float y, TerrainPoint &at, double &height) const override {
		at = TerrainPoint{double(x), double(y)};
		height = 10.0;
		return true;
	}
	bool to_picture(TerrainPoint at, double, float &x, float &y) const override {
		x = float(at.x);
		y = float(at.y);
		return true;
	}
	double height(TerrainPoint) const override { return 10.0; }
};

struct Recorder final : CanvasRequests {
	std::vector<EditorRequest> requests;
	void request(EditorRequest each) override { requests.push_back(std::move(each)); }
};

// The member `key` of the terrain_brush a set_workspace sets (null: none).
JsonValue brush_member(const EditorRequest &request, const char *key) {
	JsonValue json;
	std::string error;
	if (!opennova::io::json_parse(request.workspace, json, error)) return JsonValue();
	const JsonValue *brush = json.get("terrain_brush");
	const JsonValue *member = brush ? brush->get(key) : nullptr;
	return member ? *member : JsonValue();
}

CanvasInput pointer(float x, float y, bool pressed, bool down) {
	CanvasInput in;
	in.width = in.height = 100;
	in.mouse = CanvasPoint{x, y};
	in.screen = in.mouse;
	in.hovered = true;
	in.pressed = pressed;
	in.down = down;
	return in;
}

// The brush half over a canvas: live with the pane shown and a tool; a press lays the first dab, a drag the dabs its
// path passes (one stroke a frame, under the gesture's token), the release ends it by its token, Esc cancels it; Shift
// inverts; the keys set the brush; a stamp is a tile's drop, the tile keys the tile commands.
int test_half() {
	TextDocument mission;
	Diagnostic error;
	const std::string text = "x";
	TEST_EXPECT(mission.load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "m.bms", AssetKind::Mission, "jo", error));
	SessionView view;
	TerrainBrushHalf half(ViewportKind::Mission);
	half.follow(view, &mission);
	TEST_EXPECT(!half.live()); // the pane hidden
	view.workspace.documents["m.bms"].terrain = true;
	view.workspace.terrain_brush.radius = 4.0f;
	view.workspace.terrain_brush.spacing = 0.25f; // a dab a metre
	half.follow(view, &mission);
	TEST_EXPECT(half.live());
	const FlatSpace space;
	CanvasGesture gesture;
	CanvasSubject subject;
	subject.path = "m.bms";
	subject.identity = mission.identity();
	Recorder out;
	TEST_EXPECT(half.input(gesture, subject, &space, pointer(10.0f, 20.0f, true, true), out) && half.stroking());
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::TerrainStroke);
	const TerrainStrokeAsk &first = out.requests[0].stroke;
	TEST_EXPECT(first.plane == TerrainPlane::Heights && first.points.size() == 1 && first.points[0].first == 10.0 &&
	            first.gesture != 0 && !first.end && first.brush_named == 0);
	const uint64_t token = first.gesture;
	CanvasInput drag = pointer(13.0f, 20.0f, false, true);
	drag.keys.shift = true;
	TEST_EXPECT(half.input(gesture, subject, &space, drag, out));
	TEST_EXPECT(out.requests.size() == 2 && out.requests[1].stroke.points.size() == 3 && out.requests[1].stroke.gesture == token);
	TEST_EXPECT((out.requests[1].stroke.brush_named & kBrushTool) && out.requests[1].stroke.brush.tool == TerrainTool::Lower);
	TEST_EXPECT(half.input(gesture, subject, &space, pointer(13.0f, 20.0f, false, false), out) && !half.stroking());
	TEST_EXPECT(out.requests.size() == 3 && out.requests[2].kind == EditorRequestKind::EndEdit &&
	            out.requests[2].gesture == token && !out.requests[2].cancel);
	// Esc during a press: the stroke cancelled by its token.
	out.requests.clear();
	TEST_EXPECT(half.input(gesture, subject, &space, pointer(5.0f, 5.0f, true, true), out));
	CanvasInput escape = pointer(5.0f, 5.0f, false, true);
	escape.keyboard.focused = true;
	escape.keyboard.escape = true;
	TEST_EXPECT(half.input(gesture, subject, &space, escape, out) && !half.stroking());
	TEST_EXPECT(out.requests.size() >= 2 && out.requests[1].kind == EditorRequestKind::EndEdit && out.requests[1].cancel &&
	            out.requests[1].gesture == out.requests[0].stroke.gesture);
	// The keys: ] grows the radius by a quarter, Shift+[ lowers the strength; Esc with no press puts the brush down.
	out.requests.clear();
	CanvasInput keys = pointer(0.0f, 0.0f, false, false);
	keys.keyboard.focused = true;
	keys.keyboard.bracket = 1;
	TEST_EXPECT(half.input(gesture, subject, &space, keys, out) && out.requests.size() == 1 &&
	            out.requests[0].kind == EditorRequestKind::SetWorkspace && brush_member(out.requests[0], "radius").number == 5.0);
	keys.keyboard.bracket = 0;
	keys.keyboard.escape = true;
	TEST_EXPECT(half.input(gesture, subject, &space, keys, out) && brush_member(out.requests.back(), "tool").string == "none");
	// A stamp: the tile's drop at the pointer; the tile keys: the tile commands.
	view.workspace.terrain_brush.tool = TerrainTool::Stamp;
	view.workspace.terrain_brush.cell = 12;
	half.follow(view, &mission);
	out.requests.clear();
	TEST_EXPECT(half.input(gesture, subject, &space, pointer(30.0f, 40.0f, true, true), out) && out.requests.size() == 1 &&
	            out.requests[0].kind == EditorRequestKind::EditInViewport && out.requests[0].drop.reference == "tile" &&
	            out.requests[0].drop.name == "12" && out.requests[0].drop.x == 30.0f);
	view.workspace.terrain_brush.tool = TerrainTool::Tiles;
	half.follow(view, &mission);
	out.requests.clear();
	CanvasInput turn = pointer(0.0f, 0.0f, false, false);
	turn.keyboard.focused = true;
	turn.keyboard.rotate = true;
	TEST_EXPECT(half.input(gesture, subject, &space, turn, out) && out.requests.size() == 1 &&
	            out.requests[0].command.name == "tile_rotate" && out.requests[0].command.by == std::vector<double>{1.0});
	std::printf("half: the press, the path's dabs, the release and the cancel by token, Shift, the keys, a stamp\n");
	return 0;
}

} // namespace

int main() {
	if (test_tools() != 0) return 1;
	if (test_weight() != 0) return 1;
	if (test_dabs() != 0) return 1;
	if (test_half() != 0) return 1;
	return test_wire();
}
