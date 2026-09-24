// The AI window's world-space layers (overlay_canvas.h), the ImGui return of
// the retired GDScript AI view (b17fa294b): a state/alert label over every
// nearby brain, the nav routes its brains walk with each follower's current
// node, the target and aim lines, and the perception rings for the selected
// and engaged brains. A developer window into OUR AI port, not retail UI.
// Every element draws the AiDebugSnapshot the AI window holds (positions in
// 16.16 mission fixed, converted once here); the embedder pushes it every
// logic tick while a layer is on.
#pragma once

#include <runtime/devtools/ai_debug_snapshot.h>
#include <runtime/devtools/overlay_canvas.h>

#include <cstdint>
#include <string>

namespace opennova::devtools {

class EntitiesWindow;

class AiOverlayLayer : public OverlayLayer {
public:
	enum class Element : uint8_t { Labels, Routes, Targets, Rings };

	static constexpr float kLabelRange = 150.0f; // units from the camera
	static constexpr int kLabelMax = 64;
	static constexpr int kRingMax = 24;          // rings beyond the selection
	static constexpr float kLabelLift = 2.2f;
	static constexpr float kAimRayLength = 10.0f;

	AiOverlayLayer(const AiDebugSnapshot &snapshot, const EntitiesWindow &entities, Element element)
			: snapshot_(snapshot), entities_(entities), element_(element) {}

	const char *group() const override { return "AI"; }
	const char *label() const override;
	const char *tooltip() const override;
	int draw_priority() const override;
	int text_budget() const override { return kLabelMax; }
	void draw(OverlayCanvas &canvas) override;

	// The label lines for one row (the retired view's _label_text), for tests.
	static std::string label_text(const world::inspect::AiOverlayRow &row);
	// Green idle, yellow alerted, red engaged.
	static uint32_t alert_color(int32_t alert, float alpha = 1.0f);

private:
	void draw_labels(OverlayCanvas &canvas) const;
	void draw_routes(OverlayCanvas &canvas) const;
	void draw_targets(OverlayCanvas &canvas) const;
	void draw_rings(OverlayCanvas &canvas) const;

	const AiDebugSnapshot &snapshot_;
	const EntitiesWindow &entities_;
	Element element_;
};

}  // namespace opennova::devtools
