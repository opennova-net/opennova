#pragma once

#include <string>
#include <vector>

#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_kinds.h>

namespace opennova::editor {

class PreviewClock;
class ViewportModel;
struct SessionView;

// What a device reports each pump (ADR 0046 S13 V5): the files its picture read, each with its
// stamp then (a model's textures, a flipbook's frames, read as they are first drawn too), so the
// viewport makes its picture again when one moves; where its own compile placed each widget of a
// menu's screen (in the compiled screen's pre-order, design units: the Shell's MenuFrame beside the
// viewport's headless compile, which the hit tests and the handles read); and the size its picture
// is now, in pixels, with whether a canvas drew it this frame at a size of the canvas's own (a
// design picture fitted or scaled, a picture that fills the canvas: the viewport's device size then
// set by no SetViewport) rather than the viewport's.
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
class ViewportDevice {
public:
	virtual ~ViewportDevice() = default;
	// The picture this frame, where `picture` says (the Shell's SubViewport draws its texture
	// through the ImGui pass as the canvas's current item, at the picture's size); it renders on the
	// frames it is drawn.
	virtual void draw(const ViewportPicture &picture) = 0;
	// The surface under the picture's point (x, y), in the viewport's space: what a drop lands on (a
	// mission's terrain). False where the device has none (no kind drops yet).
	virtual bool surface_at(float x, float y, float point[3]) const {
		(void)x;
		(void)y;
		(void)point;
		return false;
	}
	// Each pump: what its viewport asks after a follow (`action`: its picture made again, its state
	// applied again, or dropped), then what follows the state (the size it draws at, a camera placed,
	// a level drawn, a rig bound, a clip posed at `clock`); what its picture read and placed reported
	// in `report`.
	virtual void take(ViewportAction action, const ViewportModel &model, const SessionView &view,
			const PreviewClock &clock, ViewportDeviceReport &report) = 0;
	// Every frame: what the clock drives (a model's part animations, flipbooks and clip).
	virtual void tick(const ViewportModel &model, const PreviewClock &clock) = 0;
};

// Where a canvas finds the device of a viewport (the Shell's devices, ViewportDeviceCache; a test's):
// the device drawing the viewport of `kind` over the document at `path`, used now (the cache gives
// up the least recently used); null while none is made (the canvas draws nothing, and the cache makes
// one at the next pump).
class ViewportDeviceSource {
public:
	virtual ~ViewportDeviceSource() = default;
	virtual ViewportDevice *device(const std::string &path, ViewportKind kind) = 0;
};

} // namespace opennova::editor
