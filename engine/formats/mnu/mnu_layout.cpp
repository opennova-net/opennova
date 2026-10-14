#include <formats/mnu/mnu_layout.h>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <formats/mns/mns.h>
#include <formats/mnu/mnu_xml.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

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

namespace {

// The model's narrow text as the wide characters the game reads: a Unicode menu's UTF-8, else the code page's
// bytes, each its cp1252 character (retail_text_to_utf8's rule).
std::u32string wide_of(const std::string &text) {
	std::u32string out;
	if (!is_valid_utf8(text)) {
		for (const char c : text) out.push_back(cp1252_decode_byte(static_cast<std::uint8_t>(c)));
		return out;
	}
	for (size_t i = 0; i < text.size();) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		size_t len = 1;
		char32_t cp = c;
		if (c >= 0x80) {
			len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
			cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
			for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
		}
		out.push_back(cp);
		i += len;
	}
	return out;
}

} // namespace

// [orig: CRT_wcstoxl @ 0x76e93b in base 16, unsigned: the iswspace skip, the sign, the "0x" prefix after a zero
// digit, CRT_wchartodigit's digits and the Latin letters, ULONG_MAX on overflow and then -value for '-']
// (mnu_xml::wcstoul, the reader's own port of it).
std::uint32_t color_value(const std::u32string &text) {
	return static_cast<std::uint32_t>(mnu_xml::wcstoul(text, 16));
}

std::uint32_t color_value(const std::string &text) { return color_value(wide_of(text)); }

bool color_reads_whole(const std::u32string &text, size_t *digits_read) {
	size_t i = 0;
	while (i < text.size() && mnu_xml::is_space(text[i])) ++i;
	if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
	if (i + 1 < text.size() && mnu_xml::wchartodigit(text[i]) == 0 && (text[i + 1] == 'x' || text[i + 1] == 'X'))
		i += 2;
	size_t digits = 0;
	for (; i < text.size(); ++i, ++digits) {
		const char32_t c = text[i];
		const int digit = mnu_xml::wchartodigit(c);
		if (digit < 0 && !((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) break;
	}
	if (digits_read) *digits_read = digits;
	while (i < text.size() && mnu_xml::is_space(text[i])) ++i;
	return digits > 0 && i == text.size();
}

bool color_reads_whole(const std::string &text, size_t *digits_read) {
	return color_reads_whole(wide_of(text), digits_read);
}

std::string color_text(std::uint32_t word) {
	char text[9];
	std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(word));
	return text;
}

// [orig: wcstoul base 16 @ 0x64bd10; forced opaque @ 0x64b220]
std::uint32_t item_color_argb(std::uint32_t word) {
	return word | 0xFF000000u;
}

Color color_of(const std::string &text) { return color_of(wide_of(text), text); }

Color color_of(const std::u32string &text, const std::string &narrow) {
	Color color;
	color.present = true;
	if (mns::is_variable_reference(narrow)) color.var = mns::variable_name(narrow);
	else color.argb = color_value(text);
	return color;
}

std::string color_word(const Color &color) {
	return color.var.empty() ? color_text(color.argb) : "%" + color.var + "%";
}

std::string appearance_text(const Appearance &row) {
	return appearance_color_type(row.type) ? color_word(row.color) : row.value;
}

std::string item_text(const Item &item) {
	return item_color_type(item.type) ? color_word(item.color) : item.text;
}

}  // namespace opennova::mnu