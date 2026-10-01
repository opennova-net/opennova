#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/preview/viewport_build_report.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_kinds.h>

namespace opennova::editor {

class PreviewClock;
class ViewportModel;
struct SessionView;

// What a device reports each pump (ADR 0046 S13 V5): the files its picture read, each with its
// stamp then (a model's textures, a flipbook's frames, read as they are first drawn too, and while a
// build runs, the files its units have read so far), so the viewport makes its picture again when one
// moves; where its own compile placed each widget of a menu's screen (in the compiled screen's
// pre-order, design units: the Shell's MenuFrame beside the viewport's headless compile, which the
// hit tests and the handles read); the size its picture is now, in pixels, with whether a canvas drew
// it this frame at a size of the canvas's own (a design picture fitted or scaled, a picture that
// fills the canvas: the viewport's device size then set by no SetViewport) rather than the
// viewport's; its build (S13 V6: ViewportDevice::build(), which the device cache reads); and the
// names its picture asked the project's files for and did not find (a mission's terrain texture, a
// model an item names), each once, which its viewport notes.
struct ViewportDeviceReport {
	struct Rect {
		bool placed = false; // the widget has a rect
		int left = 0;
		int top = 0;
		int right = 0;
		int bottom = 0;
	};
	FileStamps files;
	std::vector<Rect> rects;
	int width = 0;
	int height = 0;
	bool canvas_sized = false;
	ViewportBuildReport build;
	std::vector<std::string> missing;
};

// Where a canvas draws a device's picture this frame (ADR 0046 S13 V5), in the pixels the canvas
// reads the pointer in (the OS window's): the picture's top left corner and its size, and the part of
// the window the canvas shows it in (a picture zoomed or scrolled past the canvas is clipped to it).
// A device that draws through the ImGui pass (the SubViewport's texture, as the canvas's current
// item) reads the size alone; one that places a Godot Control over the canvas (S13 V10's CodeEdit)
// reads where, and what of it shows.
struct ViewportPicture {
	float x = 0.0f;
	float y = 0.0f;
	int width = 0;
	int height = 0;
	float clip_left = 0.0f;
	float clip_top = 0.0f;
	float clip_right = 0.0f;
	float clip_bottom = 0.0f;
	// The canvas draws the picture at a size of its own (a design picture fitted or scaled, a picture
	// that fills it), not at the viewport's device size (a menu's Device size zoom).
	bool canvas_sized = true;
};

// A device (ADR 0046 S13 V5): what draws one viewport's picture, the Shell's (an offscreen
// SubViewport with the kind's applier: the runtime's MenuFrame, ObjectModel, its rig and its part
// clock) or a test's. It follows its viewport, never changing it: it takes the action the
// viewport's follow came to and reports what it read; it draws only when a canvas draws it, and
// keeps its last picture between. Beside the SubViewport's texture the base admits a device that
// draws otherwise in the rect the canvas reserves (a Godot Control such as S13 V10's CodeEdit,
// placed at the picture on each frame it is drawn and hidden on a tick no draw came before) and one
// with no picture at all (an audio device auditioning a sound bank: taken and ticked, never drawn).
// A device may build its picture over several frames (S13 V6): a Rebuild begins the build, each
// frame's steps run its units within the Shell's budget, and until it is built (or fails) the device
// draws the last picture it built, never a half-built one.
class ViewportDevice {
public:
	virtual ~ViewportDevice() = default;
	// The picture this frame, where `picture` says (the Shell's SubViewport draws its texture
	// through the ImGui pass as the canvas's current item, at the picture's size); it renders on the
	// frames it is drawn, its picture built (while a build runs, or after one failed, the last picture
	// it built is drawn as it was).
	virtual void draw(const ViewportPicture &picture) = 0;
	// The surface under the picture's point (x, y), in the viewport's space: what a drop lands on (a
	// mission's terrain), answered only while its picture is built (none while a build runs). False
	// where the device has none (no kind drops yet).
	virtual bool surface_at(float x, float y, float point[3]) const {
		(void)x;
		(void)y;
		(void)point;
		return false;
	}
	// Where the segment from `from` to `to` first meets the surface the picture draws (a mission's
	// terrain), each a point of the viewport's space (a mission's frame, metres): what a planner's ray
	// lands on, taken from the viewport's own camera rather than the device's last applied one (a
	// SetViewport and a drag in one pump agree). False where the segment misses it, the device has no
	// surface, or the surface is not built.
	virtual bool surface_between(const double from[3], const double to[3], double point[3]) const {
		(void)from;
		(void)to;
		(void)point;
		return false;
	}
	// The surface's height at (x, y) of the viewport's space (a mission's ground under an entity: a move
	// of several keeps each one's height over it); false as surface_between.
	virtual bool ground_at(double x, double y, double &height) const {
		(void)x;
		(void)y;
		(void)height;
		return false;
	}
	// Each pump: what its viewport asks after a follow (`action`: its picture made again, its state
	// applied again, or dropped), then what follows the state (the size it draws at, a camera placed,
	// a level drawn, a rig bound, a clip posed at `clock`); what its picture read and placed reported
	// in `report`. A Rebuild begins the build of the viewport's newest generation (builds()),
	// dropping one in flight; a Clear drops one in flight with the picture. The viewport never asks
	// an Update of a build in flight (its state is applied as the build ends).
	virtual void take(ViewportAction action, const ViewportModel &model, const SessionView &view,
			const PreviewClock &clock, ViewportDeviceReport &report) = 0;
	// Every frame: what the clock drives (a model's part animations, flipbooks and clip).
	virtual void tick(const ViewportModel &model, const PreviewClock &clock) = 0;
	// Every frame after the tick (S13 V6): one unit of the build in flight (a texture decoded, a
	// level's meshes, the scene, the pose at `clock`, a menu's screen configured); false when none is
	// in flight (built, failed, or none begun: a device that makes its picture whole as it takes the
	// Rebuild never has one).
	virtual bool step(const ViewportModel &model, const PreviewClock &clock) {
		(void)model;
		(void)clock;
		return false;
	}
	// Its build as it stands now (ViewportBuildReport; none for a device that never builds over
	// frames).
	virtual ViewportBuildReport build() const { return ViewportBuildReport(); }
};

// Where a canvas finds the device of a viewport (the Shell's devices, ViewportDeviceCache; a test's):
// the device drawing the viewport of `kind` over the document at `path`, used now (the cache gives
// up the least recently used); null while none is made (the canvas draws nothing, and the cache makes
// one at the next pump). And where a planner with no canvas finds it (the wire's drag and drop,
// viewport_context): the device held for the viewport, read and not used (peek: its recency stands,
// none is made).
class ViewportDeviceSource {
public:
	virtual ~ViewportDeviceSource() = default;
	virtual ViewportDevice *device(const std::string &path, ViewportKind kind) = 0;
	virtual const ViewportDevice *peek(const std::string &path, ViewportKind kind) const = 0;
};

} // namespace opennova::editor
