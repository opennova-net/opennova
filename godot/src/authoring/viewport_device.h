#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <functional>
#include <memory>

#include <editor/preview/viewport_device.h>

#include "authoring/viewport_applier.h"

namespace godot {

// The editor's device for one viewport (ADR 0046 S13 V5): an offscreen SubViewport in a world of
// its own, the kind's applier's nodes under it (authoring/viewport_devices makes it by kind). It
// renders only on the frames a canvas draws it (UPDATE_ONCE: the canvas's draw sizes it to the
// picture and hands its texture to the ImGui pass), and keeps its last picture between; a whole
// frame no canvas draws it (a headless run, the MCP, its window hidden) it is the size the
// viewport's state says. Its viewport asks, it applies (authoring/viewport_applier): a picture made again, the state
// applied again, or dropped, then what follows the state every pump; it reports its size and
// whether a canvas sized it. Given up, its SubViewport is retired to the Shell, which frees it at
// its next frame: a texture drawn this frame is never freed under the frame's draw list.
//
// A picture its kind builds over several frames (S13 V6, the model's) is built by the units the
// Shell's frames step (step(), within the Shell's budget), the build of the viewport's newest
// generation: a newer Rebuild drops the one in flight and begins anew, a Clear drops it with the
// picture. Until the build ends the SubViewport is not rendered, nor sized again once it holds a
// picture (its texture, the last picture, would be made anew empty), so a canvas draws the last
// picture as it was, never a half-built one; a build that fails keeps that picture until a build of
// a newer generation ends.
// What each build cost (its frames, its longest frame's units, its longest unit) goes in its report.
class ViewportDevice final : public opennova::editor::ViewportDevice {
public:
	// Where a device given up hands its SubViewport (the Shell frees it at its next frame, so a
	// texture the ImGui pass drew this frame lives until the frame is drawn).
	using Retire = std::function<void(SubViewport *)>;
	// `make` builds the kind's nodes under the device's SubViewport (the devices' table's row);
	// `retire` takes the SubViewport when the device goes (none: freed at the end of the frame).
	ViewportDevice(Node &owner, const String &name,
			const std::function<std::unique_ptr<ViewportApplier>(SubViewport &)> &make, Retire retire = nullptr);
	~ViewportDevice() override;

	void draw(const opennova::editor::ViewportPicture &picture) override;
	bool surface_at(float x, float y, float point[3]) const override;
	void take(opennova::editor::ViewportAction action, const opennova::editor::ViewportModel &model,
			const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	bool step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	opennova::editor::ViewportBuildReport build() const override { return build_; }

	ViewportApplier &applier() { return *applier_; }
	SubViewport *sub_viewport() const { return viewport_; }

private:
	void size_(int width, int height);
	// The picture stands as its last build left it: a build runs, or the last one failed.
	bool held_() const { return build_.loading || build_.failed; }

	SubViewport *viewport_ = nullptr;
	uint64_t viewport_id_ = 0; // its instance, checked as the device goes
	std::unique_ptr<ViewportApplier> applier_;
	Retire retire_;
	// The ticks a canvas's draw sizes it for: 2 at a draw (this frame, and the next one up to its
	// layout pass, which draws it again: the editor MCP's calls come between), one less each tick;
	// at 0 the viewport's state sizes it (a headless run, the MCP, its window hidden). Whether the
	// last draw was at a size of the canvas's own (a menu at its Device size is not).
	int drawn_ = 0;
	bool canvas_sized_ = false;
	// It rendered a picture (a canvas drew it built): there is a last picture to keep.
	bool rendered_ = false;
	// Its build (S13 V6): the generation it builds or built, where it stands, what it cost; and the
	// microseconds its units ran this frame (-1 from the tick until one runs).
	opennova::editor::ViewportBuildReport build_;
	int64_t frame_us_ = -1;
};

} // namespace godot
