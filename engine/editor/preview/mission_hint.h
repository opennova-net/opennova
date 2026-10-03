#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_options.h>

namespace opennova::editor {

// What the line under the mission's picture says (the canvas's hint): what the current tool does and
// what a click or a drag on the picture does now, in a modder's words, the keys that reach the rest.
struct MissionHintInput {
	MissionTool tool = MissionTool::Select;
	std::string item; // the item Place places, by its name ("" none picked)
	int path = 0; // the path Path adds stops to (0 none picked)
	bool editable = true; // edits are taken now
	std::string not_editable; // why not
	bool current = true; // the picture is the document as it is now
	std::string hovered; // the mark under the pointer, by its title ("" none)
	bool hovered_area = false; // it is an area
	bool hovered_selected = false;
	bool handle = false; // the pointer is on the primary's handle `which`
	MissionHandle which = MissionHandle::Move;
	bool dragging = false; // a drag is under way, of `which` (a marquee when !handle)
	bool copying = false; // the drag copies (Alt)
	size_t selected = 0; // how many records are selected
	float snap = 0.0f; // metres (0 free)
	float turn = 0.0f; // degrees (0 whole degrees)
	bool empty_mission = false; // the mission has no entity and no area
};

std::string mission_canvas_hint(const MissionHintInput &in);

// A snap's words: "1 m", "0.25 m", "free".
std::string mission_snap_words(float metres);
// A turn snap's words: "15 deg", "whole degrees".
std::string mission_turn_words(float degrees);
// What a handle does, in a few words ("move on the ground", "raise or lower", "turn", "move the west
// edge"), and its step ("1 m steps", "15 deg steps").
std::string mission_handle_words(MissionHandle handle);
std::string mission_handle_step(MissionHandle handle, float snap, float turn);

} // namespace opennova::editor
