#pragma once

#include <cstdint>
#include <vector>

#include <editor/preview/canvas_gesture.h>

namespace opennova::editor {

// What a canvas draws over the device's picture (ADR 0046 S13 V2): shapes in the picture's
// pixels, made by a kind's canvas (preview/menu_canvas, preview/model_canvas) as pure functions
// of the picture, the selection and the pointer, and drawn by ui/viewport_canvas, which colours
// each by its role; and the cursor the pointer shows.

enum class OverlayKind : uint8_t {
	Rect, // points[0] its top-left corner, points[1] its bottom-right one
	Line, // from points[0] to points[1]
	Circle, // centred on points[0], `size` its radius
	Quad, // points[0..3]
	Marker, // a glyph reaching `size` pixels from points[0], whatever the zoom
};

// What a shape is to the author, which picks its colour: a Normal shape draws in its own `rgb`;
// Selected marks what is selected, Hover what is under the pointer, Marquee the box a drag
// selects with (filled faintly), Note a window the compiler noted something on.
enum class OverlayRole : uint8_t { Normal, Selected, Hover, Marquee, Note };

// A Marker's look: a handle's square (filled, a dark edge), a light's dot (filled, a dark
// ring), a pivot's cross, a note's corner (a triangle in the corner at its point).
enum class OverlayGlyph : uint8_t { Square, Dot, Cross, Corner };

struct OverlayShape {
	OverlayKind kind = OverlayKind::Rect;
	OverlayRole role = OverlayRole::Normal;
	OverlayGlyph glyph = OverlayGlyph::Square; // a Marker's
	CanvasPoint points[4];
	float size = 0.0f; // a Circle's radius, a Marker's reach from its point
	float thickness = 1.0f; // an outline's or a line's width
	bool filled = false; // a Rect, Circle or Quad filled instead of outlined
	uint32_t rgb = 0xFFFFFF; // a Normal shape's colour, 0xRRGGBB
	uint8_t alpha = 255; // and its opacity
};

// One frame's shapes, drawn in order: a later one over an earlier one.
class OverlayList {
public:
	std::vector<OverlayShape> shapes;

	void rect(CanvasPoint min, CanvasPoint max, OverlayRole role, float thickness = 1.0f) {
		OverlayShape &shape = add(OverlayKind::Rect, role, 0xFFFFFF);
		shape.points[0] = min;
		shape.points[1] = max;
		shape.thickness = thickness;
	}
	void line(CanvasPoint from, CanvasPoint to, uint32_t rgb, float thickness) {
		OverlayShape &shape = add(OverlayKind::Line, OverlayRole::Normal, rgb);
		shape.points[0] = from;
		shape.points[1] = to;
		shape.thickness = thickness;
	}
	// A circle's outline.
	void ring(CanvasPoint centre, float radius, OverlayRole role, float thickness,
			uint32_t rgb = 0xFFFFFF, uint8_t alpha = 255) {
		OverlayShape &shape = add(OverlayKind::Circle, role, rgb);
		shape.points[0] = centre;
		shape.size = radius;
		shape.thickness = thickness;
		shape.alpha = alpha;
	}
	// A filled circle.
	void disc(CanvasPoint centre, float radius, OverlayRole role) {
		OverlayShape &shape = add(OverlayKind::Circle, role, 0xFFFFFF);
		shape.points[0] = centre;
		shape.size = radius;
		shape.filled = true;
	}
	// A filled quad in its own colour.
	void quad(CanvasPoint a, CanvasPoint b, CanvasPoint c, CanvasPoint d, uint32_t rgb) {
		OverlayShape &shape = add(OverlayKind::Quad, OverlayRole::Normal, rgb);
		shape.points[0] = a;
		shape.points[1] = b;
		shape.points[2] = c;
		shape.points[3] = d;
		shape.filled = true;
	}
	void marker(CanvasPoint at, OverlayGlyph glyph, float size, OverlayRole role,
			uint32_t rgb = 0xFFFFFF, float thickness = 1.0f) {
		OverlayShape &shape = add(OverlayKind::Marker, role, rgb);
		shape.glyph = glyph;
		shape.points[0] = at;
		shape.size = size;
		shape.thickness = thickness;
	}

private:
	OverlayShape &add(OverlayKind kind, OverlayRole role, uint32_t rgb) {
		shapes.emplace_back();
		OverlayShape &shape = shapes.back();
		shape.kind = kind;
		shape.role = role;
		shape.rgb = rgb;
		return shape;
	}
};

// The cursor the pointer shows over a canvas (Default: the canvas leaves it as it is): a move,
// or a resize along an axis or a diagonal.
enum class CanvasCursor : uint8_t { Default, Move, ResizeEW, ResizeNS, ResizeNWSE, ResizeNESW };

} // namespace opennova::editor
