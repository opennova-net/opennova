#include "fnt/fnt.h"

#include <stdlib.h>
#include <string.h>

static uint32_t read_u32_le(const uint8_t *p) {
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int32_t read_i32_le(const uint8_t *p) {
	return (int32_t)read_u32_le(p);
}

static float read_f32_le(const uint8_t *p) {
	uint32_t bits = read_u32_le(p);
	float result;
	memcpy(&result, &bits, sizeof(float));
	return result;
}

static void write_u32_le(uint8_t *p, uint32_t value) {
	p[0] = (uint8_t)(value & 0xFFu);
	p[1] = (uint8_t)((value >> 8) & 0xFFu);
	p[2] = (uint8_t)((value >> 16) & 0xFFu);
	p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void write_i32_le(uint8_t *p, int32_t value) {
	write_u32_le(p, (uint32_t)value);
}

static void write_f32_le(uint8_t *p, float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof(float));
	write_u32_le(p, bits);
}

fnt_error_t fnt_parse_header(const uint8_t *data, size_t size,
                             uint32_t *num_pages, int32_t *shadow_offset) {
	if (!data) {
		return FNT_ERR_NULL_POINTER;
	}
	if (size < FNT_HEADER_SIZE) {
		return FNT_ERR_BUFFER_TOO_SMALL;
	}
	if (read_u32_le(data) != FNT_MAGIC) {
		return FNT_ERR_INVALID_MAGIC;
	}
	/* +4 is the DESIGN WIDTH, not a version — the engine reads it as the 800/dw
	 * scale and never validates it [orig: sub_674740 @ 0x674740]. We match: no
	 * equality gate here (a non-800 font is scaled by fnt_design_scale, D-FNT-1). */

	uint32_t pages = read_u32_le(data + 8);
	if (pages == 0 || pages > FNT_MAX_PAGES) {
		return FNT_ERR_INVALID_PAGE_COUNT;
	}

	if (num_pages) {
		*num_pages = pages;
	}
	if (shadow_offset) {
		*shadow_offset = read_i32_le(data + 12);
	}
	return FNT_OK;
}

void fnt_free(fnt_font_t *font) {
	if (!font) {
		return;
	}
	free(font->pages);
	memset(font, 0, sizeof(*font));
}

fnt_error_t fnt_init_blank(fnt_font_t *font, uint32_t num_pages, int32_t shadow_offset) {
	if (!font) {
		return FNT_ERR_NULL_POINTER;
	}
	if (num_pages == 0 || num_pages > FNT_MAX_PAGES) {
		return FNT_ERR_INVALID_PAGE_COUNT;
	}

	memset(font, 0, sizeof(*font));
	font->design_width = FNT_DEFAULT_DESIGN_WIDTH; /* fnt_parse overrides from the file */
	font->num_pages = num_pages;
	font->shadow_offset = shadow_offset;

	size_t page_bytes = (size_t)num_pages * FNT_TEXTURE_SIZE;
	font->pages = (uint8_t *)malloc(page_bytes);
	if (!font->pages) {
		memset(font, 0, sizeof(*font));
		return FNT_ERR_OUT_OF_MEMORY;
	}

	for (size_t i = 0; i < page_bytes; i += FNT_TEXTURE_CHANNELS) {
		font->pages[i + 0] = 255;
		font->pages[i + 1] = 255;
		font->pages[i + 2] = 255;
		font->pages[i + 3] = 0;
	}
	return FNT_OK;
}

fnt_error_t fnt_validate(const fnt_font_t *font) {
	if (!font) {
		return FNT_ERR_NULL_POINTER;
	}
	if (font->design_width == 0) {
		return FNT_ERR_INVALID_VERSION; /* a 0 design width has no valid render scale */
	}
	if (font->num_pages == 0 || font->num_pages > FNT_MAX_PAGES) {
		return FNT_ERR_INVALID_PAGE_COUNT;
	}
	if (!font->pages) {
		return FNT_ERR_NULL_POINTER;
	}
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		if (font->glyphs[i].page >= font->num_pages) {
			return FNT_ERR_INVALID_GLYPH_PAGE;
		}
	}
	return FNT_OK;
}

fnt_error_t fnt_parse(const uint8_t *data, size_t size, fnt_font_t *font) {
	if (!data || !font) {
		return FNT_ERR_NULL_POINTER;
	}

	uint32_t num_pages = 0;
	int32_t shadow_offset = 0;
	fnt_error_t err = fnt_parse_header(data, size, &num_pages, &shadow_offset);
	if (err != FNT_OK) {
		return err;
	}

	size_t required_size = fnt_calculate_file_size(num_pages);
	if (size < required_size) {
		return FNT_ERR_BUFFER_TOO_SMALL;
	}

	err = fnt_init_blank(font, num_pages, shadow_offset);
	if (err != FNT_OK) {
		return err;
	}
	font->design_width = read_u32_le(data + 4); /* the file's own +4 word (D-FNT-2) */

	const uint8_t *glyph_data = data + FNT_HEADER_SIZE;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const uint8_t *entry = glyph_data + ((size_t)i * FNT_GLYPH_SIZE);
		font->glyphs[i].page = read_u32_le(entry);
		font->glyphs[i].uv.u0 = read_f32_le(entry + 4);
		font->glyphs[i].uv.v0 = read_f32_le(entry + 8);
		font->glyphs[i].uv.u1 = read_f32_le(entry + 12);
		font->glyphs[i].uv.v1 = read_f32_le(entry + 16);
	}

	memcpy(font->pages, data + FNT_TOTAL_HEADER, (size_t)num_pages * FNT_TEXTURE_SIZE);

	err = fnt_validate(font);
	if (err != FNT_OK) {
		fnt_free(font);
		return err;
	}
	return FNT_OK;
}

fnt_error_t fnt_write(const fnt_font_t *font, uint8_t *out, size_t out_size, size_t *written_size) {
	if (!font || !out) {
		return FNT_ERR_NULL_POINTER;
	}

	fnt_error_t err = fnt_validate(font);
	if (err != FNT_OK) {
		return err;
	}

	size_t required_size = fnt_calculate_file_size(font->num_pages);
	if (out_size < required_size) {
		return FNT_ERR_BUFFER_TOO_SMALL;
	}

	memset(out, 0, required_size);
	write_u32_le(out, FNT_MAGIC);
	write_u32_le(out + 4, font->design_width); /* emit the font's design width, not a fixed version */
	write_u32_le(out + 8, font->num_pages);
	write_i32_le(out + 12, font->shadow_offset);

	uint8_t *glyph_data = out + FNT_HEADER_SIZE;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const fnt_glyph_t *glyph = &font->glyphs[i];
		uint8_t *entry = glyph_data + ((size_t)i * FNT_GLYPH_SIZE);
		write_u32_le(entry, glyph->page);
		write_f32_le(entry + 4, glyph->uv.u0);
		write_f32_le(entry + 8, glyph->uv.v0);
		write_f32_le(entry + 12, glyph->uv.u1);
		write_f32_le(entry + 16, glyph->uv.v1);
	}

	uint8_t *dst_pages = out + FNT_TOTAL_HEADER;
	size_t page_bytes = (size_t)font->num_pages * FNT_TEXTURE_SIZE;
	for (size_t i = 0; i < page_bytes; i += FNT_TEXTURE_CHANNELS) {
		dst_pages[i + 0] = 255;
		dst_pages[i + 1] = 255;
		dst_pages[i + 2] = 255;
		dst_pages[i + 3] = font->pages[i + 3];
	}

	if (written_size) {
		*written_size = required_size;
	}
	return FNT_OK;
}

const fnt_glyph_t *fnt_get_glyph(const fnt_font_t *font, uint8_t character) {
	if (!font || character < FNT_FIRST_CHAR) {
		return NULL;
	}
	uint32_t index = (uint32_t)character - FNT_FIRST_CHAR;
	if (index >= FNT_GLYPH_COUNT) {
		return NULL;
	}
	return &font->glyphs[index];
}

uint8_t *fnt_get_page_data(fnt_font_t *font, uint32_t page_index) {
	if (!font || !font->pages || page_index >= font->num_pages) {
		return NULL;
	}
	return font->pages + ((size_t)page_index * FNT_TEXTURE_SIZE);
}

const uint8_t *fnt_get_page_data_const(const fnt_font_t *font, uint32_t page_index) {
	if (!font || !font->pages || page_index >= font->num_pages) {
		return NULL;
	}
	return font->pages + ((size_t)page_index * FNT_TEXTURE_SIZE);
}

fnt_error_t fnt_pack_shelf(const fnt_pack_size_t *sizes, size_t count,
                           fnt_pack_rect_t *out_rects, uint32_t *out_page_count) {
	if (!sizes || !out_rects || !out_page_count) {
		return FNT_ERR_NULL_POINTER;
	}
	const uint32_t max_w = FNT_TEXTURE_WIDTH - 2u * FNT_PACK_PAD;
	const uint32_t max_h = FNT_TEXTURE_HEIGHT - 2u * FNT_PACK_PAD;
	uint32_t page = 0;
	uint32_t cx = FNT_PACK_PAD;
	uint32_t cy = FNT_PACK_PAD;
	uint32_t shelf_h = 0;
	for (size_t i = 0; i < count; ++i) {
		if (sizes[i].width == 0 || sizes[i].height == 0) {
			out_rects[i].page = 0;
			out_rects[i].x = 0;
			out_rects[i].y = 0;
			out_rects[i].width = 0;
			out_rects[i].height = 0;
			continue;
		}
		uint32_t w = sizes[i].width < max_w ? sizes[i].width : max_w;
		uint32_t h = sizes[i].height < max_h ? sizes[i].height : max_h;
		if (cx + w + FNT_PACK_PAD > FNT_TEXTURE_WIDTH) {
			cx = FNT_PACK_PAD;
			cy += shelf_h + FNT_PACK_PAD;
			shelf_h = 0;
		}
		if (cy + h + FNT_PACK_PAD > FNT_TEXTURE_HEIGHT) {
			page += 1;
			cx = FNT_PACK_PAD;
			cy = FNT_PACK_PAD;
			shelf_h = 0;
			if (page >= FNT_MAX_PAGES) {
				return FNT_ERR_INVALID_PAGE_COUNT;
			}
		}
		out_rects[i].page = page;
		out_rects[i].x = cx;
		out_rects[i].y = cy;
		out_rects[i].width = w;
		out_rects[i].height = h;
		cx += w + FNT_PACK_PAD;
		if (h > shelf_h) {
			shelf_h = h;
		}
	}
	*out_page_count = page + 1;
	return FNT_OK;
}

const char *fnt_error_string(fnt_error_t error) {
	switch (error) {
		case FNT_OK: return "Success";
		case FNT_ERR_NULL_POINTER: return "Null pointer";
		case FNT_ERR_INVALID_MAGIC: return "Invalid FNT magic";
		case FNT_ERR_INVALID_VERSION: return "Invalid FNT version";
		case FNT_ERR_BUFFER_TOO_SMALL: return "Buffer too small";
		case FNT_ERR_INVALID_PAGE_COUNT: return "Invalid texture page count";
		case FNT_ERR_OUT_OF_MEMORY: return "Out of memory";
		case FNT_ERR_INVALID_GLYPH_PAGE: return "Glyph references an invalid page";
		default: return "Unknown FNT error";
	}
}
