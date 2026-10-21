#include <editor/preview/mission_hint.h>

#include <cmath>
#include <cstdio>

namespace opennova::editor {

// A turn snap's words: "15 deg", "whole degrees".
std::string mission_turn_words(float degrees);

// A snap's words: "1 m", "0.25 m", "free".
std::string mission_snap_words(float metres);

std::string mission_snap_words(float metres) {
	if (!(metres > 0.0f)) return "free";
	char text[32];
	if (std::fabs(metres - std::round(metres)) < 1e-4f) std::snprintf(text, sizeof(text), "%.0f m", double(metres));
	else std::snprintf(text, sizeof(text), "%g m", double(metres));
	return text;
}

std::string mission_turn_words(float degrees) {
	if (!(degrees > 1.0f)) return "whole degrees";
	char text[32];
	std::snprintf(text, sizeof(text), "%.0f deg", double(degrees));
	return text;
}

std::string mission_handle_words(MissionHandle handle) {
	switch (handle) {
	case MissionHandle::Move: return "move on the ground";
	case MissionHandle::Height: return "raise or lower";
	case MissionHandle::Yaw: return "turn";
	case MissionHandle::XMin: return "move the west edge";
	case MissionHandle::XMax: return "move the east edge";
	case MissionHandle::YMin: return "move the south edge";
	case MissionHandle::YMax: return "move the north edge";
	case MissionHandle::XMinYMin: return "move the south-west corner";
	case MissionHandle::XMaxYMin: return "move the south-east corner";
	case MissionHandle::XMaxYMax: return "move the north-east corner";
	case MissionHandle::XMinYMax: return "move the north-west corner";
	case MissionHandle::ZMin: return "raise or lower the bottom";
	case MissionHandle::ZMax: return "raise or lower the top";
	}
	return "move";
}

std::string mission_handle_step(MissionHandle handle, float snap, float turn) {
	if (handle == MissionHandle::Yaw) return turn > 1.0f ? mission_turn_words(turn) + " steps" : "whole degrees";
	return snap > 0.0f ? mission_snap_words(snap) + " steps" : "free";
}

std::string mission_canvas_hint(const MissionHintInput &in) {
	// A line of at most ~60 characters: what the tool does now and its keys (the tools' tooltips hold the rest).
	if (!in.current) return "Catching up with the mission: edits wait for it.";
	// Shooting edits nothing: it takes a mission that is held too.
	if (in.tool == MissionTool::Shoot)
		return in.ammo.empty() ? "Shoot: pick an ammo in the list (Esc stops)."
		                       : "Shoot " + in.ammo + ": click to fire one (Esc stops).";
	if (!in.editable && in.tool != MissionTool::Select)
		return "Nothing can be placed: " + (in.not_editable.empty() ? std::string("the mission is held.") : in.not_editable);
	switch (in.tool) {
	case MissionTool::Place:
		if (in.item.empty()) return "Place: pick an item in the palette (Esc stops).";
		return "Place " + in.item + ": click the ground (Esc stops).";
	case MissionTool::Path:
		if (in.path <= 0) return "Path stops: pick a path in the list (Esc stops).";
		return "Path " + std::to_string(in.path) + ": click the ground to add a stop (Esc stops).";
	case MissionTool::Area:
		if (in.dragging) return "Let go to make an area trigger over the box.";
		// The box takes the toolbar's grid whatever is held.
		return "Area: drag a box on the ground (snaps to " + mission_snap_words(in.grid) + "; Esc stops).";
	case MissionTool::Select:
	case MissionTool::Shoot: break;
	}
	if (in.dragging) {
		if (!in.handle) return "Let go to select the box's marks (Shift adds, Ctrl toggles).";
		if (in.copying) return "Let go to place the copies (" + mission_snap_words(in.snap) + " steps; Ctrl: free).";
		return "Drag to " + mission_handle_words(in.which) + " (" + mission_handle_step(in.which, in.snap, in.turn) +
				"; Ctrl: free).";
	}
	if (in.handle && in.editable)
		return "Handle: drag to " + mission_handle_words(in.which) + " (" + mission_handle_step(in.which, in.snap, in.turn) +
				").";
	if (!in.hovered.empty())
		return in.hovered + ": click to select" + (in.editable && in.hovered_drags ? ", drag to move." : ".");
	if (in.empty_mission) return "An empty mission: Place puts items down, Area a trigger.";
	if (in.selected > 0) {
		const std::string line = std::to_string(in.selected) + " selected";
		// The arrows go a metre when the snap is free.
		if (in.editable)
			return line + ": arrows nudge (" + mission_snap_words(in.grid > 0.0f ? in.grid : 1.0f) +
					"), PgUp/PgDn raise, Delete removes.";
		return line + ": F frames; " + in.not_editable;
	}
	return "Click a mark to select it, drag for a box; F frames all.";
}

} // namespace opennova::editor
