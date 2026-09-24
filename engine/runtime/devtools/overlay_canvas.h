// The overlay canvas and layer (ADR 0039 d6 as amended): a window's
// world-space draw layer paints mission-frame primitives through the canvas,
// which projects them with the pushed OverlayCamera onto the Game window's
// ImGui draw list, clipped to the game image. There is no depth test: an
// overlay draws over the scene.
//
// A layer is owned by its window, registered on the pass (the Overlays menu
// lists it by group), toggled independently of the window, and drawn only
// while the tools are open. Budgets keep a runaway record from flooding the
// draw list: a primitive past its layer's budget is counted, not drawn.
#pragma once

#include <runtime/devtools/overlay_camera.h>
#include <runtime/world/geom.h>

#include <cstdint>

struct ImDrawList;

namespace opennova::devtools {

// Packed ImGui colour (IM_COL32 order: 0xAABBGGRR).
uint32_t overlay_rgba(float r, float g, float b, float a = 1.0f);
// The retired GDScript views' per-index hue walk: hsv(fract(i * 0.618...),
// 0.75, 1) — distinct neighbours for routes, groups and teams.
uint32_t overlay_index_color(int index, float alpha = 1.0f);
// The colour with its alpha scaled (0..1).
uint32_t overlay_fade(uint32_t rgba, float alpha);

struct OverlayLayerStats {
	int lines = 0;
	int texts = 0;
	int dropped = 0; // primitives past the layer's budgets this frame
};

class OverlayCanvas {
public:
	OverlayCanvas(ImDrawList *draw_list, const OverlayCamera &camera, const OverlayRect &rect);

	const OverlayCamera &camera() const { return camera_; }
	const OverlayRect &rect() const { return rect_; }
	// Project a mission point to image pixels (false behind the camera).
	bool project(const world::Vec3 &p, float out[2]) const;
	// Distance from the eye (for range culls and label fading).
	float distance(const world::Vec3 &p) const;

	void line(const world::Vec3 &a, const world::Vec3 &b, uint32_t rgba, float thickness = 1.5f);
	void cross(const world::Vec3 &p, float half_size, uint32_t rgba);
	// A diamond in the mission ground plane around `center` (radius r).
	void diamond(const world::Vec3 &center, float r, uint32_t rgba);
	// A circle in the mission ground plane.
	void ground_circle(const world::Vec3 &center, float r, uint32_t rgba, int segments = 32);
	// An axis-aligned box by its mission min/max corners.
	void box(const world::Vec3 &min, const world::Vec3 &max, uint32_t rgba);
	void triangle(const world::Vec3 &a, const world::Vec3 &b, const world::Vec3 &c, uint32_t rgba);
	// A sphere as its screen-space outline (one circle, radius from projecting
	// the centre raised by r).
	void sphere_outline(const world::Vec3 &center, float r, uint32_t rgba);
	// Text centred above the anchor on a dark backing rect.
	void text(const world::Vec3 &anchor, const char *text, uint32_t rgba);

	// Budgets and stats for the layer being drawn (the pass sets them).
	void begin_layer(int line_budget, int text_budget);
	OverlayLayerStats end_layer() const { return stats_; }
	// The draw list's vertex guard: false once the list is too full for 16-bit
	// indices without the backend's vertex-offset support.
	bool has_room() const;

private:
	bool take_line();
	bool take_text();

	ImDrawList *draw_list_;
	OverlayCamera camera_;
	OverlayRect rect_;
	int line_budget_ = 0;
	int text_budget_ = 0;
	OverlayLayerStats stats_{};
};

class OverlayLayer {
public:
	virtual ~OverlayLayer() = default;
	virtual const char *group() const = 0; // the Overlays menu section
	virtual const char *label() const = 0;
	virtual const char *tooltip() const { return ""; }
	virtual int draw_priority() const { return 0; } // lower draws underneath
	virtual int line_budget() const { return 4000; }
	virtual int text_budget() const { return 64; }
	virtual void draw(OverlayCanvas &canvas) = 0;
	// The toggle's edge (a layer arms or drops its record here).
	virtual void on_enabled(bool enabled) { (void)enabled; }

	bool enabled() const { return enabled_; }
	const OverlayLayerStats &last_stats() const { return stats_; }

protected:
	explicit OverlayLayer(bool enabled_by_default = false) : enabled_(enabled_by_default) {}

private:
	friend class ImGuiPass;
	bool enabled_;
	OverlayLayerStats stats_{};
};

}  // namespace opennova::devtools
