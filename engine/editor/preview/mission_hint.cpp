#include <editor/preview/mission_hint.h>

#include <cmath>
#include <cstdio>

namespace opennova::editor {

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
	}
	return "move";
}

std::string mission_handle_step(MissionHandle handle, float snap, float turn) {
	if (handle == MissionHandle::Yaw) return turn > 1.0f ? mission_turn_words(turn) + " steps" : "whole degrees";
	return snap > 0.0f ? mission_snap_words(snap) + " steps" : "free";
}

std::string mission_canvas_hint(const MissionHintInput &in) {
	if (!in.current) return "The picture is catching up with the mission; edits wait for it.";
	// Shooting edits nothing: it takes a mission that is held too.
	if (in.tool == MissionTool::Shoot)
		return in.ammo.empty() ? "Shoot: pick an ammo, then click the terrain or an object to fire it there (Esc stops)."
		                       : "Shoot " + in.ammo + ": click the terrain or an object to fire one there; the impact "
		                                              "plays as the game plays it (Esc stops).";
	if (!in.editable && in.tool != MissionTool::Select)
		return "Nothing can be placed now: " + (in.not_editable.empty() ? std::string("the mission is held.") : in.not_editable);
	switch (in.tool) {
	case MissionTool::Place:
		if (in.item.empty()) return "Place: pick an item in the palette, then click the ground to place it (Esc stops).";
		return "Place " + in.item + ": click the ground to place one, facing the way the camera looks; it stays picked "
				"for the next (Esc stops).";
	case MissionTool::Path:
		if (in.path <= 0) return "Path stops: pick a path in the list, then click the ground to add its next stop (Esc stops).";
		return "Path " + std::to_string(in.path) + ": click the ground to add a stop (a marker the path visits next; Esc "
				"stops).";
	case MissionTool::Area:
		if (in.dragging) return "Let go to make an area trigger over the box.";
		// The box takes the toolbar's grid whatever is held.
		return "Area: drag a box on the ground to make an area trigger (snaps to " + mission_snap_words(in.grid) +
				"; Esc stops).";
	case MissionTool::Select:
	case MissionTool::Shoot: break;
	}
	if (in.dragging) {
		if (!in.handle) return "Let go to select what the box holds (Shift adds, Ctrl toggles).";
		if (in.copying) return "Let go to place the copies here (" + mission_snap_words(in.snap) + " steps; hold Ctrl for free).";
		return "Drag to " + mission_handle_words(in.which) + " (" + mission_handle_step(in.which, in.snap, in.turn) +
				"; hold Ctrl for free).";
	}
	if (in.handle && in.editable)
		return "Handle: drag to " + mission_handle_words(in.which) + " (" + mission_handle_step(in.which, in.snap, in.turn) +
				").";
	if (!in.hovered.empty()) {
		std::string line = in.hovered + ": click to select";
		if (in.editable && in.hovered_drags) line += ", drag to move (Alt-drag copies)";
		line += "; Shift adds, Ctrl toggles; right-click for more.";
		return line;
	}
	if (in.empty_mission)
		return "An empty mission: Place puts people, vehicles and buildings on the ground; Area draws a trigger zone; "
				"right-click for more.";
	if (in.selected > 0) {
		std::string line = std::to_string(in.selected) + (in.selected == 1 ? " selected" : " selected records");
		if (in.editable)
			// The arrows go a metre when the snap is free.
			line += ": arrows nudge (" + mission_snap_words(in.grid > 0.0f ? in.grid : 1.0f) + ", Shift finer), PgUp/PgDn raise, Ctrl+D "
					"duplicates, Delete removes, F frames; click empty ground to select nothing.";
		else
			line += ": F frames; " + in.not_editable;
		return line;
	}
	return "Click a mark to select it, drag for a box; right button looks (W A S D Q E fly), middle button pans, wheel "
			"zooms, F frames everything.";
}

} // namespace opennova::editor
