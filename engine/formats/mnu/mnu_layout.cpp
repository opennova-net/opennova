#include <formats/mnu/mnu_layout.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdlib>

namespace opennova::mnu {

// [orig: CUIElement_InitBorderMaterials @ 0x646f70 — the SIZE x SIZE grid slices]
FrameTileRect frame_tile_rect(int size, int col, int row) {
	FrameTileRect r;
	r.x = col * size;
	r.y = row * size;
	r.size = size;
	return r;
}

int frame_stencil_tile_size(int authored_size, int texture_width) {
	if (authored_size > 0) return authored_size;
	return texture_width / 4;
}

// [orig: CUIElement_DrawFrame @ 0x64a210 — the 8 border quads hang outside
// the rect by SIZE, pulled back by INSETX/INSETY; edges stretch between the
// corners]
std::array<FrameBorderPiece, 8> frame_border_layout(int size, int insetx,
		int insety) {
	const float s = static_cast<float>(size);
	const float ix = static_cast<float>(insetx);
	const float iy = static_cast<float>(insety);
	const float x0 = -s + ix; // border overhangs the rect [orig: left - SIZE + INSETX]
	const float y0 = -s + iy;
	const float xr = -ix - 1.0f; // against the right edge (anchor 1)
	const float yb = -iy - 1.0f; // against the bottom edge (anchor 1)

	auto piece = [&](const char *name, int col, int row, float ax0, float ax1,
						 float ay0, float ay1, float l, float t, float r,
						 float b) {
		FrameBorderPiece p;
		p.name = name;
		p.tile_col = col;
		p.tile_row = row;
		p.anchor_left = ax0;
		p.anchor_right = ax1;
		p.anchor_top = ay0;
		p.anchor_bottom = ay1;
		p.off_left = l;
		p.off_top = t;
		p.off_right = r;
		p.off_bottom = b;
		return p;
	};
	return {
		piece("FrameTL", 0, 0, 0, 0, 0, 0, x0, y0, x0 + s, y0 + s),
		piece("FrameTop", 1, 0, 0, 1, 0, 0, x0 + s, y0, xr, y0 + s),
		piece("FrameTR", 2, 0, 1, 1, 0, 0, xr, y0, xr + s, y0 + s),
		piece("FrameLeft", 0, 1, 0, 0, 0, 1, x0, y0 + s, x0 + s, yb),
		piece("FrameRight", 2, 1, 1, 1, 0, 1, xr, y0 + s, xr + s, yb),
		piece("FrameBL", 0, 2, 0, 0, 1, 1, x0, yb, x0 + s, yb + s),
		piece("FrameBottom", 1, 2, 0, 1, 1, 1, x0 + s, yb, xr, yb + s),
		piece("FrameBR", 2, 2, 1, 1, 1, 1, xr, yb, xr + s, yb + s),
	};
}

// [orig: the POSITION branch @ 0x648120 — missing edges read 0 like the
// zeroed element fields; the parse tail's degenerate-axis fallback to the
// accumulated max texture extents]
RectEdges position_rect(bool has_left, int left, bool has_top, int top,
		bool has_right, int right, bool has_bottom, int bottom,
		int max_image_w, int max_image_h) {
	RectEdges r;
	r.left = has_left ? left : 0;
	r.top = has_top ? top : 0;
	r.right = has_right ? right : 0;
	r.bottom = has_bottom ? bottom : 0;
	if (r.right <= r.left) r.right = r.left + max_image_w;
	if (r.bottom <= r.top) r.bottom = r.top + max_image_h;
	return r;
}

// [orig: the APPEARANCE branch @ 0x648120 — height is the authored HEIGHT
// attr (the sprite-sheet frame height) when present, else the texture height]
int appearance_extent_height(bool has_height, int height, int texture_height) {
	return has_height && height > 0 ? height : texture_height;
}

// [orig: reached from the shared text-widget parse @ 0x657c30 and the edit
// override @ 0x661d10]
bool window_type_is_text_sized(WindowType t) {
	return t == WindowType::Static || t == WindowType::Label ||
			t == WindowType::Button || t == WindowType::Radio ||
			t == WindowType::CheckBox || t == WindowType::Edit ||
			t == WindowType::Marquee;
}

// [orig: CStaticWnd_AdjustRectToTextSize @ 0x6575f0]
RectEdges adjust_rect_to_text_size(const RectEdges &rect, int text_w,
		int text_h, const std::string &justify, const std::string &vjustify) {
	RectEdges r = rect;
	if (r.right <= r.left) {
		const int anchor = r.left; // right == left after the extent stage
		if (strutil::iequals(justify, "center")) {
			r.left = anchor - text_w / 2;
			r.right = r.left + text_w;
		} else if (strutil::iequals(justify, "right")) {
			r.left = anchor - text_w; // the right edge stays at the anchor
		} else {
			r.right = anchor + text_w;
		}
	}
	if (r.bottom <= r.top) {
		const int anchor = r.top;
		if (strutil::iequals(vjustify, "center")) {
			r.top = anchor - text_h / 2;
			r.bottom = r.top + text_h;
		} else if (strutil::iequals(vjustify, "bottom")) {
			r.top = anchor - text_h; // the bottom edge stays at the anchor
		} else {
			r.bottom = anchor + text_h;
		}
	}
	return r;
}

// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 — parent-relative
// authored coords; a missing far edge sizes from the appearance texture]
RectEdges spin_button_rect(bool has_left, int left, bool has_top, int top,
		bool has_right, int right, bool has_bottom, int bottom,
		int extent_w, int extent_h) {
	RectEdges r;
	r.left = has_left ? left : 0;
	r.top = has_top ? top : 0;
	const int w = has_right ? (right - r.left) : (extent_w > 0 ? extent_w : 16);
	const int h = has_bottom ? (bottom - r.top) : (extent_h > 0 ? extent_h : 12);
	r.right = r.left + std::max(w, 0);
	r.bottom = r.top + std::max(h, 0);
	return r;
}

// [orig: wcstoul base 16 @ 0x64bd10; forced opaque @ 0x64b220]
std::uint32_t item_color_argb(const std::string &hex_text) {
	const unsigned long v = std::strtoul(hex_text.c_str(), nullptr, 16);
	return static_cast<std::uint32_t>(v) | 0xFF000000u;
}

}  // namespace opennova::mnu