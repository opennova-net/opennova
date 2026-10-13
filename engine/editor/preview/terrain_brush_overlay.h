#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/preview/mission_ground_overlay.h>

namespace opennova::editor {

struct SessionView;

// The ground overlay a view shows for the terrain brush (ADR 0046 S25, the painting feedback of 2026-10-13): a stroke
// of the surface or the foliage map changes nothing the game draws, so while the brush paints one of them in a view
// (its Terrain pane shows and its tool is Surface or Foliage) the view's overlay option is that map's (DI-29's
// Surfaces or Foliage), and once the tool moves away it is the person's again. A choice of the person's while the brush
// paints (a set_viewport naming the overlay, whatever it names) is theirs and stays. The mission's 3D view and the
// terrain viewport keep one each (their follow_), the option on the wire saying what shows and the body's
// `brush_overlay` why.

// The overlay of the map the brush paints in the view of the document at `path` now: Surfaces for the Surface tool,
// Foliage for the Foliage tool, while the document's Terrain pane shows; None otherwise.
MissionGroundOverlay terrain_brush_overlay(const SessionView &view, const std::string &path);

class BrushOverlay {
public:
	// The view's overlay option `overlay` set as the brush asks (`painting`, terrain_brush_overlay's): the painted map's
	// overlay as the brush begins to paint it, the person's own again as it stops, where the brush's is still the one
	// showing (one the person chose meanwhile stays). True when `overlay` moved.
	bool follow(MissionGroundOverlay painting, MissionGroundOverlay &overlay);
	// The person chose the overlay (a set_viewport naming it, the brush's own value among them): theirs, which the brush
	// neither puts back nor replaces until the map it paints changes. One chosen as the view is made (the set_viewport
	// that makes it) stands over the brush's first follow.
	void chosen();
	// The overlay the brush shows now (None: none, or the person chose theirs) and the one the view goes back to as it
	// stops.
	MissionGroundOverlay shown() const { return chosen_ ? MissionGroundOverlay::None : shown_; }
	MissionGroundOverlay returns_to() const { return before_; }
	// On the wire (a viewport's body): {overlay, returns_to} while the brush shows one, else null.
	io::JsonValue to_json() const;

private:
	MissionGroundOverlay shown_ = MissionGroundOverlay::None; // the map the brush paints, as last followed
	MissionGroundOverlay before_ = MissionGroundOverlay::None;
	bool chosen_ = false;
	bool followed_ = false;
};

} // namespace opennova::editor
