#pragma once

namespace opennova::editor {
class PreviewClock;
class ViewportModel;
struct SessionView;
struct ViewportDeviceReport;
} // namespace opennova::editor

namespace godot {

// A viewport kind's device work (ADR 0046 S13 V5): the runtime's nodes a kind's picture is made of,
// under the device's SubViewport (authoring/viewport_device), and what they do as the viewport asks.
// One per device, made by the kind's row of the devices' table (authoring/viewport_devices): the
// menu's over the runtime's MenuFrame, the model's over ObjectModel, its rig and its PanmClock. It
// reads its viewport and never changes it; what it read and where it placed things goes back in the
// device's report.
class ViewportApplier {
public:
	virtual ~ViewportApplier() = default;
	// The picture made again from the viewport (a screen configured, a scene built), then its state
	// applied; what it read reported.
	virtual void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view) = 0;
	// The viewport's state applied again over the picture that stands (a model's level, registers
	// and rig).
	virtual void update(const opennova::editor::ViewportModel &model) = 0;
	// What it built dropped.
	virtual void clear() = 0;
	// Each pump, after the action: what follows the state continuously (a camera placed, a level
	// drawn, a rig bound, a clip posed at `clock`), and what its picture read and placed reported.
	virtual void step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) = 0;
	// Every frame: what the clock drives (a model's part animations, flipbooks, generators, clip).
	virtual void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) = 0;
	// The picture's size changed (a menu's frame laid out across it).
	virtual void resize(int width, int height) = 0;
	// The surface under the picture's point (x, y), the viewport's space (a drop's ground): none for
	// today's kinds.
	virtual bool surface_at(float x, float y, float point[3]) const {
		(void)x;
		(void)y;
		(void)point;
		return false;
	}
};

} // namespace godot
