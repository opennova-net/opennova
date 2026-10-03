#include <formats/mnu/mnu_layout.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdint>

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
// accumulated max texture extents @ 0x649736..0x64975e]
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

// [orig: CUIElement_ParseXMLDefinition @ 0x6485cd..0x648634 — the width
// against CTextureManager_GetTextureWidth, the height against the row's HEIGHT
// (the loader's texFormat argument) unless it is -1, else
// CTextureManager_GetTextureHeight; both `jbe` (unsigned) against the maxima]
void ImageExtents::add(int texture_width, int texture_height, int height_attribute) {
	const uint32_t w = static_cast<uint32_t>(texture_width);
	if (w > width) width = w;
	const uint32_t h = height_attribute != -1 ? static_cast<uint32_t>(height_attribute)
											  : static_cast<uint32_t>(texture_height);
	if (h > height) height = h;
}

// [orig: every parse chaining to CUIButtonWidget_ParseXMLAttributes @ 0x657c30,
// which ends in CStaticWnd_AdjustRectToTextSize @ 0x658079 (the grill's S-chain)]
bool window_type_is_text_sized(WindowType t) {
	switch (t) {
		case WindowType::Static:
		case WindowType::Button:
		case WindowType::Edit:
		case WindowType::MultilineEdit:
		case WindowType::Radio:
		case WindowType::CheckBox:
		case WindowType::SpinList:
		case WindowType::List:
		case WindowType::LanList:
		case WindowType::Table:
		case WindowType::Combo:
			return true;
		default:
			return false;
	}
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

// [orig: CRT_wcstoxl @ 0x76e93b in base 16, unsigned: the whitespace skip, the
// sign, the "0x" prefix, the digits, ULONG_MAX on overflow (not negated), and
// -value for '-']
std::uint32_t color_value(const std::string &text) {
	size_t i = 0;
	while (i < text.size() && (text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r'))) ++i;
	bool negative = false;
	if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
		negative = text[i] == '-';
		++i;
	}
	if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;
	uint64_t value = 0;
	bool overflow = false;
	for (; i < text.size(); ++i) {
		const char c = text[i];
		int digit = 0;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
		else break;
		if (!overflow) value = value * 16u + static_cast<uint64_t>(digit);
		if (value > 0xFFFFFFFFull) overflow = true;
	}
	if (overflow) return 0xFFFFFFFFu;
	const uint32_t word = static_cast<uint32_t>(value);
	return negative ? static_cast<uint32_t>(0u - word) : word;
}

bool color_reads_whole(const std::string &text, size_t *digits_read) {
	size_t i = 0;
	const auto blank = [&](size_t at) { return text[at] == ' ' || (text[at] >= '\t' && text[at] <= '\r'); };
	while (i < text.size() && blank(i)) ++i;
	if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
	if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;
	size_t digits = 0;
	for (; i < text.size(); ++i, ++digits) {
		const char c = text[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) break;
	}
	if (digits_read) *digits_read = digits;
	while (i < text.size() && blank(i)) ++i;
	return digits > 0 && i == text.size();
}

// [orig: wcstoul base 16 @ 0x64bd10; forced opaque @ 0x64b220]
std::uint32_t item_color_argb(const std::string &hex_text) {
	return color_value(hex_text) | 0xFF000000u;
}

}  // namespace opennova::mnu