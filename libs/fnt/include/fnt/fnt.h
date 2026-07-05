/**
 * @file fnt.h
 * @brief NovaLogic FNT bitmap font parser/writer.
 *
 * Supports the exact Nova FNT dialect used by Joint Operations assets:
 * FNT0, glyphs 32..255, 256x256 RGBA pages. The header word at +4 is the font's
 * DESIGN WIDTH (the reader scales glyphs by 800/design_width) — NOT a version;
 * the engine never validates it [orig: sub_674740 @ 0x674740 / sub_580400 @ 0x580400].
 */

#ifndef OPENNOVA_FNT_H
#define OPENNOVA_FNT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FNT_MAGIC 0x30544E46u /* "FNT0" in little-endian */
/* The header +4 word is the DESIGN WIDTH; the glyph render scale is
 * FNT_DESIGN_REFERENCE_WIDTH / design_width. Shipped JO fonts are 800-designed
 * (scale 1.0); a non-800 font is scaled, never rejected [orig: @ 0x674740]. */
#define FNT_DESIGN_REFERENCE_WIDTH 800u
#define FNT_DEFAULT_DESIGN_WIDTH 800u /* what our from-scratch writer emits */
#define FNT_HEADER_SIZE 32u
#define FNT_GLYPH_COUNT 224u
#define FNT_GLYPH_SIZE 20u
#define FNT_GLYPH_TABLE_SIZE (FNT_GLYPH_COUNT * FNT_GLYPH_SIZE)
#define FNT_TOTAL_HEADER (FNT_HEADER_SIZE + FNT_GLYPH_TABLE_SIZE)
#define FNT_TEXTURE_WIDTH 256u
#define FNT_TEXTURE_HEIGHT 256u
#define FNT_TEXTURE_CHANNELS 4u
#define FNT_TEXTURE_SIZE (FNT_TEXTURE_WIDTH * FNT_TEXTURE_HEIGHT * FNT_TEXTURE_CHANNELS)
#define FNT_FIRST_CHAR 32u
#define FNT_MAX_PAGES 16u

typedef enum {
	FNT_OK = 0,
	FNT_ERR_NULL_POINTER = -1,
	FNT_ERR_INVALID_MAGIC = -2,
	FNT_ERR_INVALID_VERSION = -3,
	FNT_ERR_BUFFER_TOO_SMALL = -4,
	FNT_ERR_INVALID_PAGE_COUNT = -5,
	FNT_ERR_OUT_OF_MEMORY = -6,
	FNT_ERR_INVALID_GLYPH_PAGE = -7
} fnt_error_t;

typedef struct {
	float u0;
	float v0;
	float u1;
	float v1;
} fnt_uv_t;

typedef struct {
	uint32_t page;
	fnt_uv_t uv;
} fnt_glyph_t;

typedef struct {
	uint32_t design_width; /* header +4; glyph render scale = 800/design_width */
	uint32_t num_pages;
	int32_t shadow_offset;
	fnt_glyph_t glyphs[FNT_GLYPH_COUNT];
	uint8_t *pages; /* Contiguous num_pages * FNT_TEXTURE_SIZE RGBA bytes. */
} fnt_font_t;

/* The engine's per-font glyph render scale [orig: this+4844 = 800.0 / fontData[1]
 * @ 0x674740]. 1.0 for the shipped 800-design fonts; guards a 0 design width. */
static inline float fnt_design_scale(uint32_t design_width) {
	return design_width ? (float)FNT_DESIGN_REFERENCE_WIDTH / (float)design_width : 1.0f;
}

static inline size_t fnt_calculate_file_size(uint32_t num_pages) {
	return FNT_TOTAL_HEADER + ((size_t)num_pages * FNT_TEXTURE_SIZE);
}

fnt_error_t fnt_parse_header(const uint8_t *data, size_t size,
                             uint32_t *num_pages, int32_t *shadow_offset);
fnt_error_t fnt_parse(const uint8_t *data, size_t size, fnt_font_t *font);
fnt_error_t fnt_init_blank(fnt_font_t *font, uint32_t num_pages, int32_t shadow_offset);
void fnt_free(fnt_font_t *font);
fnt_error_t fnt_validate(const fnt_font_t *font);
fnt_error_t fnt_write(const fnt_font_t *font, uint8_t *out, size_t out_size, size_t *written_size);

const fnt_glyph_t *fnt_get_glyph(const fnt_font_t *font, uint8_t character);
uint8_t *fnt_get_page_data(fnt_font_t *font, uint32_t page_index);
const uint8_t *fnt_get_page_data_const(const fnt_font_t *font, uint32_t page_index);

static inline void fnt_uv_to_pixels(const fnt_uv_t *uv,
                                    int *x0, int *y0, int *x1, int *y1) {
	*x0 = (int)(uv->u0 * (float)FNT_TEXTURE_WIDTH);
	*y0 = (int)(uv->v0 * (float)FNT_TEXTURE_HEIGHT);
	*x1 = (int)(uv->u1 * (float)FNT_TEXTURE_WIDTH);
	*y1 = (int)(uv->v1 * (float)FNT_TEXTURE_HEIGHT);
}

static inline void fnt_get_glyph_size(const fnt_glyph_t *glyph,
                                      int *width, int *height) {
	int x0, y0, x1, y1;
	fnt_uv_to_pixels(&glyph->uv, &x0, &y0, &x1, &y1);
	*width = x1 - x0;
	*height = y1 - y0;
}

const char *fnt_error_string(fnt_error_t error);

#ifdef __cplusplus
}
#endif

#endif /* OPENNOVA_FNT_H */
