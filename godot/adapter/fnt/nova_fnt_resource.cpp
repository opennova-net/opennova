#include "fnt/nova_fnt_resource.h"

#include "util/nova_cp1252.h"

#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <vector>

namespace godot {

void NovaFntResource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create_blank", "page_count", "shadow_offset"), &NovaFntResource::create_blank);
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &NovaFntResource::load_from_bytes);
	ClassDB::bind_method(D_METHOD("to_bytes"), &NovaFntResource::to_bytes);

	ClassDB::bind_method(D_METHOD("get_page_count"), &NovaFntResource::get_page_count);
	ClassDB::bind_method(D_METHOD("get_glyph_count"), &NovaFntResource::get_glyph_count);
	ClassDB::bind_method(D_METHOD("get_first_char"), &NovaFntResource::get_first_char);
	ClassDB::bind_method(D_METHOD("get_shadow_offset"), &NovaFntResource::get_shadow_offset);
	ClassDB::bind_method(D_METHOD("set_shadow_offset", "shadow_offset"), &NovaFntResource::set_shadow_offset);

	ClassDB::bind_method(D_METHOD("get_page_image", "page"), &NovaFntResource::get_page_image);
	ClassDB::bind_method(D_METHOD("set_page_image", "page", "image"), &NovaFntResource::set_page_image);

	ClassDB::bind_method(D_METHOD("get_glyph_page", "char_code"), &NovaFntResource::get_glyph_page);
	ClassDB::bind_method(D_METHOD("get_glyph_rect", "char_code"), &NovaFntResource::get_glyph_rect);
	ClassDB::bind_method(D_METHOD("set_glyph_rect", "char_code", "page", "rect"), &NovaFntResource::set_glyph_rect);

	ClassDB::bind_method(D_METHOD("get_pixel_alpha", "page", "x", "y"), &NovaFntResource::get_pixel_alpha);
	ClassDB::bind_method(D_METHOD("set_pixel_alpha", "page", "x", "y", "alpha"), &NovaFntResource::set_pixel_alpha);

	ClassDB::bind_method(D_METHOD("to_font_file"), &NovaFntResource::to_font_file);

	ClassDB::bind_static_method("NovaFntResource", D_METHOD("pack_shelf", "sizes"), &NovaFntResource::pack_shelf);

	BIND_CONSTANT(FIRST_CHAR);
	BIND_CONSTANT(GLYPH_COUNT);
	BIND_CONSTANT(TEXTURE_WIDTH);
	BIND_CONSTANT(TEXTURE_HEIGHT);
	BIND_CONSTANT(MAX_PAGES);
	BIND_CONSTANT(PACK_PAD);
	BIND_CONSTANT(MAGIC);
}

PackedInt32Array NovaFntResource::pack_shelf(const PackedInt32Array &p_sizes) {
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

NovaFntResource::NovaFntResource() {
	std::memset(&font_, 0, sizeof(font_));
}

NovaFntResource::~NovaFntResource() {
	_clear();
}

void NovaFntResource::_clear() {
	if (valid_) {
		fnt_free(&font_);
		valid_ = false;
	} else {
		std::memset(&font_, 0, sizeof(font_));
	}
}

bool NovaFntResource::_has_valid_font() const {
	return valid_ && font_.pages != nullptr && font_.num_pages > 0;
}

bool NovaFntResource::_char_to_index(int p_char_code, uint32_t &r_index) const {
	if (p_char_code < static_cast<int>(FNT_FIRST_CHAR) ||
	    p_char_code >= static_cast<int>(FNT_FIRST_CHAR + FNT_GLYPH_COUNT)) {
		return false;
	}
	r_index = static_cast<uint32_t>(p_char_code - static_cast<int>(FNT_FIRST_CHAR));
	return true;
}

Error NovaFntResource::create_blank(int p_page_count, int p_shadow_offset) {
	fnt_font_t fresh;
	fnt_error_t err = fnt_init_blank(&fresh, static_cast<uint32_t>(p_page_count), p_shadow_offset);
	if (err != FNT_OK) {
		return ERR_INVALID_PARAMETER;
	}

	_clear();
	font_ = fresh;
	valid_ = true;
	emit_changed();
	return OK;
}

Error NovaFntResource::load_from_bytes(const PackedByteArray &p_bytes) {
	fnt_font_t parsed;
	fnt_error_t err = fnt_parse(p_bytes.ptr(), p_bytes.size(), &parsed);
	if (err != FNT_OK) {
		UtilityFunctions::push_error("NovaFntResource: failed to parse .fnt: ", fnt_error_string(err));
		return ERR_PARSE_ERROR;
	}

	_clear();
	font_ = parsed;
	valid_ = true;
	emit_changed();
	return OK;
}

PackedByteArray NovaFntResource::to_bytes() const {
	PackedByteArray result;
	if (!_has_valid_font()) {
		return result;
	}

	size_t size = fnt_calculate_file_size(font_.num_pages);
	result.resize(size);
	size_t written = 0;
	fnt_error_t err = fnt_write(&font_, result.ptrw(), result.size(), &written);
	if (err != FNT_OK) {
		UtilityFunctions::push_error("NovaFntResource: failed to write .fnt: ", fnt_error_string(err));
		result.resize(0);
		return result;
	}
	result.resize(written);
	return result;
}

int NovaFntResource::get_page_count() const {
	return _has_valid_font() ? static_cast<int>(font_.num_pages) : 0;
}

int NovaFntResource::get_glyph_count() const {
	return FNT_GLYPH_COUNT;
}

int NovaFntResource::get_first_char() const {
	return FNT_FIRST_CHAR;
}

int NovaFntResource::get_shadow_offset() const {
	return _has_valid_font() ? font_.shadow_offset : 0;
}

void NovaFntResource::set_shadow_offset(int p_shadow_offset) {
	if (!_has_valid_font() || font_.shadow_offset == p_shadow_offset) {
		return;
	}
	font_.shadow_offset = p_shadow_offset;
	emit_changed();
}

Ref<Image> NovaFntResource::get_page_image(int p_page) const {
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

Error NovaFntResource::set_page_image(int p_page, const Ref<Image> &p_image) {
	if (!_has_valid_font() || p_page < 0 || p_page >= static_cast<int>(font_.num_pages) || p_image.is_null()) {
		return ERR_INVALID_PARAMETER;
	}

	Ref<Image> image = p_image->duplicate();
	if (image.is_null() || image->is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	if (image->is_compressed()) {
		image->decompress();
	}
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	if (image->get_width() != static_cast<int>(FNT_TEXTURE_WIDTH) ||
	    image->get_height() != static_cast<int>(FNT_TEXTURE_HEIGHT)) {
		return ERR_INVALID_PARAMETER;
	}

	PackedByteArray src = image->get_data();
	uint8_t *dst = fnt_get_page_data(&font_, static_cast<uint32_t>(p_page));
	for (int i = 0; i < src.size(); i += FNT_TEXTURE_CHANNELS) {
		dst[i + 0] = 255;
		dst[i + 1] = 255;
		dst[i + 2] = 255;
		dst[i + 3] = src[i + 3];
	}
	emit_changed();
	return OK;
}

int NovaFntResource::get_glyph_page(int p_char_code) const {
	uint32_t index = 0;
	if (!_has_valid_font() || !_char_to_index(p_char_code, index)) {
		return -1;
	}
	return static_cast<int>(font_.glyphs[index].page);
}

Rect2i NovaFntResource::get_glyph_rect(int p_char_code) const {
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

Error NovaFntResource::set_glyph_rect(int p_char_code, int p_page, const Rect2i &p_rect) {
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
	glyph.uv.u0 = static_cast<float>(p_rect.position.x) / static_cast<float>(FNT_TEXTURE_WIDTH);
	glyph.uv.v0 = static_cast<float>(p_rect.position.y) / static_cast<float>(FNT_TEXTURE_HEIGHT);
	glyph.uv.u1 = static_cast<float>(p_rect.position.x + p_rect.size.x) / static_cast<float>(FNT_TEXTURE_WIDTH);
	glyph.uv.v1 = static_cast<float>(p_rect.position.y + p_rect.size.y) / static_cast<float>(FNT_TEXTURE_HEIGHT);
	emit_changed();
	return OK;
}

int NovaFntResource::get_pixel_alpha(int p_page, int p_x, int p_y) const {
	if (!_has_valid_font() || p_page < 0 || p_page >= static_cast<int>(font_.num_pages) ||
	    p_x < 0 || p_x >= static_cast<int>(FNT_TEXTURE_WIDTH) ||
	    p_y < 0 || p_y >= static_cast<int>(FNT_TEXTURE_HEIGHT)) {
		return 0;
	}
	const uint8_t *page = fnt_get_page_data_const(&font_, static_cast<uint32_t>(p_page));
	size_t offset = ((size_t)p_y * FNT_TEXTURE_WIDTH + (size_t)p_x) * FNT_TEXTURE_CHANNELS + 3;
	return page[offset];
}

Error NovaFntResource::set_pixel_alpha(int p_page, int p_x, int p_y, int p_alpha) {
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

Ref<FontFile> NovaFntResource::to_font_file() const {
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

	int32_t advance_adjust = font_.shadow_offset - 1;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const std::uint8_t retail_byte = static_cast<std::uint8_t>(FNT_FIRST_CHAR + i);
		// Retail measures and draws text from unsigned bytes. Bytes 0x7F..0x81
		// are non-printing controls; every other byte directly selects its FNT
		// record. Expose that record at the codepoint produced by OpenNova's
		// CP1252 string boundary so Godot selects the same bitmap and metrics.
		// [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]
		if (retail_byte >= 0x7F && retail_byte <= 0x81) {
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
