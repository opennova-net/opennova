#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <functional>
#include <memory>

#include <editor/preview/viewport_device.h>

#include "authoring/viewport_applier.h"

namespace godot {

// The editor's device for one viewport (ADR 0046 S13 V5): an offscreen SubViewport in a world of
// its own, the kind's applier's nodes under it (authoring/viewport_devices makes it by kind). It
// renders only on the frames a canvas draws it (UPDATE_ONCE: the canvas's draw sizes it to the
// picture and hands its texture to the ImGui pass), and keeps its last picture between; until a
// canvas draws it (a headless run, the MCP) it is the size the viewport's state says. Its viewport
// asks, it applies (authoring/viewport_applier): a picture made again, the state applied again, or
// dropped, then what follows the state every pump. The SubViewport leaves the tree with it.
class ViewportDevice final : public opennova::editor::ViewportDevice {
public:
	// `make` builds the kind's nodes under the device's SubViewport (the devices' table's row).
	ViewportDevice(Node &owner, const String &name,
			const std::function<std::unique_ptr<ViewportApplier>(SubViewport &)> &make);
	~ViewportDevice() override;

	void draw(int width, int height) override;
	bool surface_at(float x, float y, float point[3]) const override;
	void take(opennova::editor::ViewportAction action, const opennova::editor::ViewportModel &model,
			const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;

	ViewportApplier &applier() { return *applier_; }
	SubViewport *sub_viewport() const { return viewport_; }

private:
	void size_(int width, int height);

	SubViewport *viewport_ = nullptr;
	std::unique_ptr<ViewportApplier> applier_;
	bool drawn_ = false; // a canvas drew it: the canvas sizes it from then on
};

} // namespace godot
