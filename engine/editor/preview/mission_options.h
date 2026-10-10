#pragma once

#include <cstdint>
#include <string>

#include <base/io/json.h>
#include <editor/preview/mission_ground_overlay.h>

namespace opennova::editor {

// The mission canvas's tools (ADR 0046 S15, Placing and tweaking): Select (a click selects, a drag
// moves or draws a box), Place (each click places the picked item), Path (each click adds a stop to
// the picked path: a marker and the stop naming it), Area (a box dragged on the ground makes an area
// trigger). Esc goes back to Select. DI-23: Shoot (each click fires the picked ammo where it meets the ground or an
// object, preview/mission_shots).
enum class MissionTool : uint8_t { Select, Place, Path, Area, Shoot };
// "select", "place", "path", "area", "shoot".
const char *mission_tool_token(MissionTool tool);
bool mission_tool_from_token(const std::string &token, MissionTool &out);

// The mission view's Listen (ADR 0046 DI-36), its options' `listen`: on or off, and the master volume (0..1) every
// sound it plays is scaled by (the editor's, not the game's). Off by default: a mission opened is silent until asked.
struct MissionListenOptions {
	bool on = false;
	float volume = 1.0f;
	bool operator==(const MissionListenOptions &o) const { return on == o.on && volume == o.volume; }
	bool operator!=(const MissionListenOptions &o) const { return !(*this == o); }
};

// The mission viewport's options (ADR 0046 S14): the layers its device draws (the terrain, the sky,
// the water, the models, the shadows as the game casts them: the terrain's static ones and, S23 C, the
// entities' moving ground shadows, one layer), the marks the canvas draws over them (each
// pool's entities, the area triggers, the paths, the labels beside every shown mark rather than
// the hovered and selected ones alone), how far from the eye a mark is still drawn and picked
// (metres; 0: no limit), whether a move keeps each entity's height over the ground (stick), and the
// time of day the device shows (hours, 0..24; below 0 the mission's own start time). S15: the tool a
// click on its canvas takes, the item its Place tool places (an items.def id; 0 none picked) and the
// path its Path tool adds stops to (1 to 122; 0 none picked), the toolbar's and the wire's alike. The
// MCP gaps lane: the grid a canvas's move, placed record and area edge snap to (metres on the file's
// axes; 0 free; the toolbar's Snap), the steps a turned entity's heading snaps to (degrees; 0 whole
// degrees; its Turn), and the Place palette's search text; like the tool, the canvas's alone (no
// picture changes). DI-29: the ground overlay the device tints the terrain with (mission_ground_overlay.h:
// the surface classes, or the foliage). DI-31: three more of the device's layers, the terrain's foliage as the
// game grows it from its foliage map, each placed item's effects as the mission's start attaches them
// (mission_effects.h), and the lights the game lights the scene with (the placed models' own).
// DI-23: the ammo the Shoot tool fires (an ammo.def record; "" none picked). DI-36: the Listen
// (preview/mission_listen.h), which the device plays rather than draws.
// Set by a SetViewport's `options` member (its wire form below), every member optional.
struct MissionViewportOptions {
	bool terrain = true, sky = true, water = true, models = true, shadows = true;
	bool foliage = true, effects = true, lights = true;
	bool items = true, buildings = true, markers = true, organics = true;
	bool areas = true, paths = true, labels = false;
	float mark_range = 600.0f;
	bool stick = true;
	double time = -1.0;
	MissionTool tool = MissionTool::Select;
	int64_t item = 0;
	int path = 0;
	float snap = 1.0f;
	float turn = 15.0f;
	std::string palette;
	MissionGroundOverlay overlay = MissionGroundOverlay::None;
	std::string ammo;
	MissionListenOptions listen;
};

// The toolbar's Snap steps (metres; 0 free) and Turn steps (degrees; 0 whole degrees), in its lists' order.
inline constexpr float kMissionSnaps[] = { 0.0f, 0.25f, 1.0f, 5.0f, 10.0f };
inline constexpr float kMissionTurns[] = { 0.0f, 5.0f, 15.0f, 45.0f, 90.0f };

bool operator==(const MissionViewportOptions &a, const MissionViewportOptions &b);
inline bool operator!=(const MissionViewportOptions &a, const MissionViewportOptions &b) { return !(a == b); }

// The wire form: {show: {terrain, sky, water, models, shadows, foliage, effects, lights}, marks: {items,
// buildings, markers, organics, areas, paths, labels}, mark_range, stick, time (null: the mission's start
// time), tool (its token), item, path, snap, turn, palette, overlay (none, surfaces or foliage), ammo, listen
// {on, volume}}.
io::JsonValue mission_options_to_json(const MissionViewportOptions &options);
// A SetViewport's options member set over `held`: every member checked before any applies; false,
// nothing changed, with `error` naming the member and what it takes.
bool mission_options_from_json(const io::JsonValue &json, MissionViewportOptions &held, std::string &error);
// The Listen's own: {on, volume}; read over `held`, every member optional (false, nothing changed, with `error` naming
// the member, for another member or a value out of its range).
io::JsonValue mission_listen_options_to_json(const MissionListenOptions &options);
bool mission_listen_options_from_json(const io::JsonValue &json, MissionListenOptions &held, std::string &error);

} // namespace opennova::editor
