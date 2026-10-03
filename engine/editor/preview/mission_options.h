#pragma once

#include <cstdint>
#include <string>

#include <base/io/json.h>

namespace opennova::editor {

// The mission canvas's tools (ADR 0046 S15, Placing and tweaking): Select (a click selects, a drag
// moves or draws a box), Place (each click places the picked item), Path (each click adds a stop to
// the picked path: a marker and the stop naming it), Area (a box dragged on the ground makes an area
// trigger). Esc goes back to Select.
enum class MissionTool : uint8_t { Select, Place, Path, Area };
// "select", "place", "path", "area".
const char *mission_tool_token(MissionTool tool);
bool mission_tool_from_token(const std::string &token, MissionTool &out);

// The mission viewport's options (ADR 0046 S14): the layers its device draws (the terrain, the sky,
// the water, the models, the static terrain shadows), the marks the canvas draws over them (each
// pool's entities, the area triggers, the paths, the labels beside every shown mark rather than
// the hovered and selected ones alone), how far from the eye a mark is still drawn and picked
// (metres; 0: no limit), whether a move keeps each entity's height over the ground (stick), and the
// time of day the device shows (hours, 0..24; below 0 the mission's own start time). S15: the tool a
// click on its canvas takes, the item its Place tool places (an items.def id; 0 none picked) and the
// path its Path tool adds stops to (1 to 122; 0 none picked), the toolbar's and the wire's alike. Set
// by a SetViewport's `options` member (its wire form below), every member optional.
struct MissionViewportOptions {
	bool terrain = true, sky = true, water = true, models = true, shadows = true;
	bool items = true, buildings = true, markers = true, organics = true;
	bool areas = true, paths = true, labels = false;
	float mark_range = 600.0f;
	bool stick = true;
	double time = -1.0;
	MissionTool tool = MissionTool::Select;
	int64_t item = 0;
	int path = 0;
};

bool operator==(const MissionViewportOptions &a, const MissionViewportOptions &b);
inline bool operator!=(const MissionViewportOptions &a, const MissionViewportOptions &b) { return !(a == b); }

// The wire form: {show: {terrain, sky, water, models, shadows}, marks: {items, buildings, markers,
// organics, areas, paths, labels}, mark_range, stick, time (null: the mission's start time), tool
// (its token), item, path}.
io::JsonValue mission_options_to_json(const MissionViewportOptions &options);
// A SetViewport's options member set over `held`: every member checked before any applies; false,
// nothing changed, with `error` naming the member and what it takes.
bool mission_options_from_json(const io::JsonValue &json, MissionViewportOptions &held, std::string &error);

} // namespace opennova::editor
