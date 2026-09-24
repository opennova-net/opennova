// The Entities window's world-space layers (overlay_canvas.h): the selected
// entity's marker (the pick highlight, on by default) and name/team/health
// labels over the entities near the camera. Both draw the EntityMarkersRecord
// the embedder pushes per logic tick while a layer wants it — the engine's
// world::inspect::entity_markers join, by value.
#pragma once

#include <runtime/devtools/overlay_canvas.h>
#include <runtime/world/inspect_markers.h>

#include <cstdint>
#include <vector>

namespace opennova::devtools {

struct EntityMarkersRecord {
	bool valid = false;
	uint64_t logic_tick = 0;
	std::vector<world::inspect::EntityMarker> rows;
};

class EntitySelectionLayer : public OverlayLayer {
public:
	explicit EntitySelectionLayer(const EntityMarkersRecord &record)
			: OverlayLayer(/*enabled_by_default=*/true), record_(record) {}
	const char *group() const override { return "Entities"; }
	const char *label() const override { return "Selection"; }
	const char *tooltip() const override {
		return "Mark the selected entity (a world pick or an Entities row) in the Game view.";
	}
	int draw_priority() const override { return 90; }
	void draw(OverlayCanvas &canvas) override;

private:
	const EntityMarkersRecord &record_;
};

class EntityLabelsLayer : public OverlayLayer {
public:
	// The labels' reach and count (the query the embedder runs).
	static constexpr float kRangeUnits = 150.0f;
	static constexpr int32_t kCap = 64;

	explicit EntityLabelsLayer(const EntityMarkersRecord &record) : record_(record) {}
	const char *group() const override { return "Entities"; }
	const char *label() const override { return "Labels"; }
	const char *tooltip() const override {
		return "Name, team and health over the entities near the camera (the nearest 64 within 150 units).";
	}
	int draw_priority() const override { return 60; }
	int text_budget() const override { return kCap; }
	void draw(OverlayCanvas &canvas) override;

private:
	const EntityMarkersRecord &record_;
};

}  // namespace opennova::devtools
