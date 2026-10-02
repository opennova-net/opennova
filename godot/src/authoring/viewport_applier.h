#pragma once

#include <cstdint>
#include <string>

#include <editor/session/operation_progress.h>

namespace opennova::editor {
class PreviewClock;
class ViewportModel;
struct SessionView;
struct ViewportDeviceReport;
} // namespace opennova::editor

namespace godot {

// What one unit of an applier's build came to (ADR 0046 S13 V6): more units to run, the picture
// built, or the build failed (the applier keeps the last picture it built and says why).
enum class ApplierStep : uint8_t { More, Built, Failed };

// A viewport kind's device work (ADR 0046 S13 V5): the runtime's nodes a kind's picture is made of,
// under the device's SubViewport (authoring/viewport_device), and what they do as the viewport asks.
// One per device, made by the kind's row of the devices' table (authoring/viewport_devices): the
// menu's over the runtime's MenuFrame, the model's over ObjectModel, its rig and its PanmClock. It
// reads its viewport and never changes it; what it read and where it placed things goes back in the
// device's report. A kind builds its picture in one step as it takes the Rebuild (the menu's when its
// frame keeps every texture the screen names), or over several frames a unit at a time (S13 V6: the
// model's textures, then its meshes, then the scene with its rig, then the pose; the menu's textures
// not decoded yet, then its configure), the device stepping the units within the Shell's frame
// budget and drawing the last picture until the build ends.
class ViewportApplier {
public:
	virtual ~ViewportApplier() = default;
	// The picture made again from the viewport: its build begun, the units planned and none run (one
	// in flight dropped first, its partial work discarded), or the picture made whole here by a kind
	// that builds in one step (building() then false), at `clock` (a menu's frame takes its time).
	virtual void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) = 0;
	// One unit of the build begun (S13 V6): More, Built once the picture is built (its state applied as
	// update() applies it, at `clock`), or Failed with `failure` saying why (the last picture kept: the
	// protocol's, which no applier here produces yet). Never called while building() is false.
	virtual ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) {
		(void)model;
		(void)clock;
		(void)failure;
		return ApplierStep::Built;
	}
	// True while a build begun has units to run.
	virtual bool building() const { return false; }
	// The build's units: done of total in steps, and what it works on (its label).
	virtual opennova::editor::OperationProgress progress() const { return opennova::editor::OperationProgress(); }
	// The viewport's state applied again over the picture that stands (a model's level, registers,
	// rig and clip at `clock`; a menu's options); never asked while a build runs (the build applies
	// the state as it ends, the same way: an Update folded into a build loses nothing).
	virtual void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) = 0;
	// What it built dropped, a build in flight with it.
	virtual void clear() = 0;
	// Each pump, after the action: what follows the state continuously over a built picture (a
	// camera placed, a level drawn, a rig bound, a clip posed at `clock`), and what its picture read
	// and placed reported (while a build runs, the files its units have read so far, nothing applied).
	virtual void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
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
	// Where the segment from `from` to `to` meets the surface the picture draws, and the surface's
	// height at (x, y), in the viewport's space (opennova::editor::ViewportDevice's; a mission's
	// terrain): answered by the applier while the layer that holds the surface stands, whatever else
	// it builds meanwhile (an added model's units leave the ground as it was). None for a kind with no
	// surface.
	virtual bool surface_between(const double from[3], const double to[3], double point[3]) const {
		(void)from;
		(void)to;
		(void)point;
		return false;
	}
	virtual bool ground_at(double x, double y, double &height) const {
		(void)x;
		(void)y;
		(void)height;
		return false;
	}
	// The process-wide scene state its picture renders with (ADR 0046 S14, E13; the portable
	// ViewportDevice::scene_state): 0 for the shipped defaults (the menu's, the model's), else a
	// value of the applier's own (the mission's: its device). publish_scene_state writes every global
	// of it again (an environment's shader globals, the water plane); present runs the frame's legs
	// over its picture before it renders, `dt` the preview clock's seconds since its last present (0
	// while the clock is paused, so a test's frame is deterministic).
	virtual uint64_t scene_state() const { return 0; }
	virtual void publish_scene_state() {}
	virtual void present(double dt) { (void)dt; }
};

} // namespace godot
