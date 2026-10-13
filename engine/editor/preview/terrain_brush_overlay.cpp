// The ground overlay a view shows for the terrain brush (terrain_brush_overlay.h, ADR 0046 S25). Tooling: the
// editor's own choice of what to show, over DI-29's overlays of what the game reads.

#include <editor/preview/terrain_brush_overlay.h>

#include <editor/session/view/session_view.h>

namespace opennova::editor {

MissionGroundOverlay terrain_brush_overlay(const SessionView &view, const std::string &path) {
	if (!view.workspace.document(path).terrain) return MissionGroundOverlay::None;
	switch (view.workspace.terrain_brush.tool) {
	case TerrainTool::Surface: return MissionGroundOverlay::Surfaces;
	case TerrainTool::Foliage: return MissionGroundOverlay::Foliage;
	default: return MissionGroundOverlay::None;
	}
}

bool BrushOverlay::follow(MissionGroundOverlay painting, MissionGroundOverlay &overlay) {
	// The first follow of a view the person's set_viewport made: their overlay stands over the brush's.
	if (!followed_) {
		followed_ = true;
		if (chosen_ && painting != MissionGroundOverlay::None) {
			shown_ = painting;
			before_ = overlay;
			return false;
		}
		chosen_ = false;
	}
	if (painting == shown_) return false;
	const MissionGroundOverlay was = overlay;
	if (painting == MissionGroundOverlay::None) {
		// Stopped: the person's own again, unless they chose one meanwhile (theirs stays).
		if (!chosen_ && overlay == shown_) overlay = before_;
	} else {
		// Begun, or moved to the other map: the option kept to go back to is the person's (a choice of theirs while the
		// brush painted the other map among them).
		if (shown_ == MissionGroundOverlay::None || chosen_ || overlay != shown_) before_ = overlay;
		overlay = painting;
	}
	chosen_ = false;
	shown_ = painting;
	return overlay != was;
}

void BrushOverlay::chosen() { chosen_ = true; }

io::JsonValue BrushOverlay::to_json() const {
	if (shown() == MissionGroundOverlay::None) return io::JsonValue::make_null();
	io::JsonValue out = io::JsonValue::make_object();
	out.set("overlay", io::json_string(mission_ground_overlay_token(shown_)));
	out.set("returns_to", io::json_string(mission_ground_overlay_token(before_)));
	return out;
}

} // namespace opennova::editor
