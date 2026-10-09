#include "fnt/fnt_resource.h"

#include <base/io/cp1252.h>

#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <vector>

using namespace opennova::fnt;

namespace godot {

void FntResource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create_blank", "page_count", "glyph_spacing"), &FntResource::create_blank);
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &FntResource::load_from_bytes);
	ClassDB::bind_method(D_METHOD("to_bytes"), &FntResource::to_bytes);

	ClassDB::bind_method(D_METHOD("get_page_count"), &FntResource::get_page_count);
	ClassDB::bind_method(D_METHOD("get_glyph_count"), &FntResource::get_glyph_count);
	ClassDB::bind_method(D_METHOD("get_first_char"), &FntResource::get_first_char);
	ClassDB::bind_method(D_METHOD("get_glyph_spacing"), &FntResource::get_glyph_spacing);

	ClassDB::bind_method(D_METHOD("get_page_image", "page"), &FntResource::get_page_image);

	ClassDB::bind_method(D_METHOD("get_glyph_rect", "char_code"), &FntResource::get_glyph_rect);
	ClassDB::bind_method(D_METHOD("set_glyph_rect", "char_code", "page", "rect"), &FntResource::set_glyph_rect);

	ClassDB::bind_method(D_METHOD("get_pixel_alpha", "page", "x", "y"), &FntResource::get_pixel_alpha);
	ClassDB::bind_method(D_METHOD("set_pixel_alpha", "page", "x", "y", "alpha"), &FntResource::set_pixel_alpha);

	ClassDB::bind_method(D_METHOD("to_font_file"), &FntResource::to_font_file);

	ClassDB::bind_static_method("FntResource", D_METHOD("pack_shelf", "sizes"), &FntResource::pack_shelf);

	BIND_CONSTANT(FIRST_CHAR);
	BIND_CONSTANT(GLYPH_COUNT);
	BIND_CONSTANT(TEXTURE_WIDTH);
	BIND_CONSTANT(TEXTURE_HEIGHT);
	BIND_CONSTANT(MAX_PAGES);
	BIND_CONSTANT(PACK_PAD);
	BIND_CONSTANT(MAGIC);
}

PackedInt32Array FntResource::pack_shelf(const PackedInt32Array &p_sizes) {
	PackedInt32Array result;
	if (p_sizes.size() % 2 != 0) {
		return result;
	}
	const size_t count = (size_t)(p_sizes.size() / 2);
	if (count == 0) {
		return result;
	}
	std::vector<fnt_pack_size_t> sizes(count);
	for (size_t i = 0; i < count; ++i) {
		const int32_t w = p_sizes[(int64_t)(i * 2)];
		const int32_t h = p_sizes[(int64_t)(i * 2 + 1)];
		sizes[i].width = w > 0 ? (uint32_t)w : 0u;
		sizes[i].height = h > 0 ? (uint32_t)h : 0u;
	}
	std::vector<fnt_pack_rect_t> rects(count);
	uint32_t page_count = 0;
	if (fnt_pack_shelf(sizes.data(), count, rects.data(), &page_count) != FNT_OK) {
		return result;
	}
	result.resize((int64_t)(count * 5));
	for (size_t i = 0; i < count; ++i) {
		result[(int64_t)(i * 5 + 0)] = (int32_t)rects[i].page;
		result[(int64_t)(i * 5 + 1)] = (int32_t)rects[i].x;
		result[(int64_t)(i * 5 + 2)] = (int32_t)rects[i].y;
		result[(int64_t)(i * 5 + 3)] = (int32_t)rects[i].width;
		result[(int64_t)(i * 5 + 4)] = (int32_t)rects[i].height;
	}
	return result;
}

FntResource::FntResource() {
	std::memset(&font_, 0, sizeof(font_));
}

FntResource::~FntResource() {
	_clear();
}

void FntResource::_clear() {
	if (valid_) {
		fnt_free(&font_);
		valid_ = false;
	} else {
		std::memset(&font_, 0, sizeof(font_));
	}
}

bool FntResource::_has_valid_font() const {
	return valid_ && font_.pages != nullptr && font_.num_pages > 0;
}

bool FntResource::_char_to_index(int p_char_code, uint32_t &r_index) const {
	if (p_char_code < static_cast<int>(FNT_FIRST_CHAR) ||
	    p_char_code >= static_cast<int>(FNT_FIRST_CHAR + FNT_GLYPH_COUNT)) {
		return false;
	}
	r_index = static_cast<uint32_t>(p_char_code - static_cast<int>(FNT_FIRST_CHAR));
	return true;
}

Error FntResource::create_blank(int p_page_count, int p_glyph_spacing) {
	fnt_font_t fresh;
	fnt_error_t err = fnt_init_blank(&fresh, static_cast<uint32_t>(p_page_count), p_glyph_spacing);
	if (err != FNT_OK) {
		return ERR_INVALID_PARAMETER;
	}

	_clear();
	font_ = fresh;
	valid_ = true;
	emit_changed();
	return OK;
}

Error FntResource::load_from_bytes(const PackedByteArray &p_bytes) {
	fnt_font_t parsed;
	fnt_error_t err = fnt_parse(p_bytes.ptr(), p_bytes.size(), &parsed);
	if (err != FNT_OK) {
		UtilityFunctions::push_error("FntResource: failed to parse .fnt: ", fnt_error_string(err));
		return ERR_PARSE_ERROR;
	}

	_clear();
	font_ = parsed;
	valid_ = true;
	emit_changed();
	return OK;
}

PackedByteArray FntResource::to_bytes() const {
	PackedByteArray result;
	if (!_has_valid_font()) {
		return result;
	}

	size_t size = fnt_calculate_file_size(font_.num_pages);
	result.resize(size);
	size_t written = 0;
	fnt_error_t err = fnt_write(&font_, result.ptrw(), result.size(), &written);
	if (err != FNT_OK) {
		UtilityFunctions::push_error("FntResource: failed to write .fnt: ", fnt_error_string(err));
		result.resize(0);
		return result;
	}
	result.resize(written);
	return result;
}

int FntResource::get_page_count() const {
	return _has_valid_font() ? static_cast<int>(font_.num_pages) : 0;
}

int FntResource::get_glyph_count() const {
	return FNT_GLYPH_COUNT;
}

int FntResource::get_first_char() const {
	return FNT_FIRST_CHAR;
}

int FntResource::get_glyph_spacing() const {
	return _has_valid_font() ? font_.glyph_spacing : 0;
}

Ref<Image> FntResource::get_page_image(int p_page) const {
	if (!_has_valid_font() || p_page < 0 || p_page >= static_cast<int>(font_.num_pages)) {
		return Ref<Image>();
	}

	const uint8_t *page = fnt_get_page_data_const(&font_, static_cast<uint32_t>(p_page));
	if (!page) {
		return Ref<Image>();
	}

	PackedByteArray data;
	data.resize(FNT_TEXTURE_SIZE);
	std::memcpy(data.ptrw(), page, FNT_TEXTURE_SIZE);
	return Image::create_from_data(FNT_TEXTURE_WIDTH, FNT_TEXTURE_HEIGHT, false, Image::FORMAT_RGBA8, data);
}

Rect2i FntResource::get_glyph_rect(int p_char_code) const {
	uint32_t index = 0;
	if (!_has_valid_font() || !_char_to_index(p_char_code, index)) {
		return Rect2i();
	}
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	fnt_uv_to_pixels(&font_.glyphs[index].uv, &x0, &y0, &x1, &y1);
	return Rect2i(x0, y0, x1 - x0, y1 - y0);
}

Error FntResource::set_glyph_rect(int p_char_code, int p_page, const Rect2i &p_rect) {
	uint32_t index = 0;
	if (!_has_valid_font() || !_char_to_index(p_char_code, index) ||
	    p_page < 0 || p_page >= static_cast<int>(font_.num_pages)) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_rect.position.x < 0 || p_rect.position.y < 0 ||
	    p_rect.size.x < 0 || p_rect.size.y < 0 ||
	    p_rect.position.x + p_rect.size.x > static_cast<int>(FNT_TEXTURE_WIDTH) ||
	    p_rect.position.y + p_rect.size.y > static_cast<int>(FNT_TEXTURE_HEIGHT)) {
		return ERR_INVALID_PARAMETER;
	}

	fnt_glyph_t &glyph = font_.glyphs[index];
	glyph.page = static_cast<uint32_t>(p_page);
	fnt_pixels_to_uv(p_rect.position.x, p_rect.position.y, p_rect.position.x + p_rect.size.x,
	                 p_rect.position.y + p_rect.size.y, &glyph.uv);
	emit_changed();
	return OK;
}

int FntResource::get_pixel_alpha(int p_page, int p_x, int p_y) const {
	if (!_has_valid_font() || p_page < 0 || p_page >= static_cast<int>(font_.num_pages) ||
	    p_x < 0 || p_x >= static_cast<int>(FNT_TEXTURE_WIDTH) ||
	    p_y < 0 || p_y >= static_cast<int>(FNT_TEXTURE_HEIGHT)) {
		return 0;
	}
	const uint8_t *page = fnt_get_page_data_const(&font_, static_cast<uint32_t>(p_page));
	size_t offset = ((size_t)p_y * FNT_TEXTURE_WIDTH + (size_t)p_x) * FNT_TEXTURE_CHANNELS + 3;
	return page[offset];
}

Error FntResource::set_pixel_alpha(int p_page, int p_x, int p_y, int p_alpha) {
	if (!_has_valid_font() || p_page < 0 || p_page >= static_cast<int>(font_.num_pages) ||
	    p_x < 0 || p_x >= static_cast<int>(FNT_TEXTURE_WIDTH) ||
	    p_y < 0 || p_y >= static_cast<int>(FNT_TEXTURE_HEIGHT) ||
	    p_alpha < 0 || p_alpha > 255) {
		return ERR_INVALID_PARAMETER;
	}
	uint8_t *page = fnt_get_page_data(&font_, static_cast<uint32_t>(p_page));
	size_t offset = ((size_t)p_y * FNT_TEXTURE_WIDTH + (size_t)p_x) * FNT_TEXTURE_CHANNELS;
	page[offset + 0] = 255;
	page[offset + 1] = 255;
	page[offset + 2] = 255;
	page[offset + 3] = static_cast<uint8_t>(p_alpha);
	emit_changed();
	return OK;
}

Ref<FontFile> FntResource::to_font_file() const {
	if (!_has_valid_font()) {
		return Ref<FontFile>();
	}

	Ref<FontFile> font;
	font.instantiate();
	font->set_allow_system_fallback(false);

	const int32_t cache_index = 0;
	int32_t font_height = 16;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		int width = 0;
		int height = 0;
		fnt_get_glyph_size(&font_.glyphs[i], &width, &height);
		if (height > font_height) {
			font_height = height;
		}
	}

	Vector2i size_key(font_height, 0);

	for (uint32_t page = 0; page < font_.num_pages; ++page) {
		Ref<Image> image = get_page_image(static_cast<int>(page));
		if (image.is_valid()) {
			font->set_texture_image(cache_index, size_key, static_cast<int32_t>(page), image);
		}
	}

	int32_t advance_adjust = font_.glyph_spacing - 1;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const std::uint8_t retail_byte = static_cast<std::uint8_t>(FNT_FIRST_CHAR + i);
		// The non-printing byte range and its witness live at fnt.h's
		// fnt_byte_is_nonprinting; every printing byte directly selects its FNT
		// record. Expose that record at the codepoint produced by OpenNova's
		// CP1252 string boundary so Godot selects the same bitmap and metrics.
		if (fnt_byte_is_nonprinting(retail_byte)) {
			continue;
		}
		const int32_t char_code = static_cast<int32_t>(opennova::cp1252_decode_byte(retail_byte));
		const fnt_glyph_t *glyph = &font_.glyphs[i];

		int x0 = 0;
		int y0 = 0;
		int x1 = 0;
		int y1 = 0;
		fnt_uv_to_pixels(&glyph->uv, &x0, &y0, &x1, &y1);
		int width = x1 - x0;
		int height = y1 - y0;

		if (width <= 0 || height <= 0) {
			if (char_code == 32) {
				// font_height / 3 as the empty-space advance is an uncited
				// runtime choice pending witness (the retail advance walk is
				// glyph-width driven; see docs/fonts/fnt-re.md).
				font->set_glyph_advance(cache_index, font_height, char_code, Vector2(font_height / 3.0f, 0));
			}
			continue;
		}

		font->set_glyph_texture_idx(cache_index, size_key, char_code, static_cast<int32_t>(glyph->page));
		font->set_glyph_uv_rect(cache_index, size_key, char_code, Rect2(x0, y0, width, height));
		font->set_glyph_size(cache_index, size_key, char_code, Vector2(width, height));
		font->set_glyph_offset(cache_index, size_key, char_code, Vector2(0, 0));
		int advance = width + advance_adjust;
		if (advance < 1) {
			advance = 1;
		}
		font->set_glyph_advance(cache_index, font_height, char_code, Vector2(advance, 0));
	}

	font->set_fixed_size(font_height);
	font->set_fixed_size_scale_mode(TextServer::FIXED_SIZE_SCALE_DISABLE);
	font->set_cache_ascent(cache_index, font_height, 0);
	font->set_cache_descent(cache_index, font_height, font_height);
	return font;
}

} // namespace godot
