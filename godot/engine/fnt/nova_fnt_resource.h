#ifndef OPENNOVA_FNT_NOVA_FNT_RESOURCE_H
#define OPENNOVA_FNT_NOVA_FNT_RESOURCE_H

#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>

#include <fnt/fnt.h>

namespace godot {

class NovaFntResource : public Resource {
	GDCLASS(NovaFntResource, Resource)

protected:
	static void _bind_methods();

public:
	// FNT format facts, single-sourced from libs/fnt (ENG-4): editor scripts
	// alias these instead of re-declaring the numbers.
	enum {
		FIRST_CHAR = FNT_FIRST_CHAR,
		GLYPH_COUNT = FNT_GLYPH_COUNT,
		TEXTURE_WIDTH = FNT_TEXTURE_WIDTH,
		TEXTURE_HEIGHT = FNT_TEXTURE_HEIGHT,
		MAX_PAGES = FNT_MAX_PAGES,
		PACK_PAD = FNT_PACK_PAD,
		MAGIC = FNT_MAGIC, /* "FNT0" little-endian — the header 4CC */
	};

	NovaFntResource();
	~NovaFntResource();

	// fnt_pack_shelf over (width, height) pairs; returns (page, x, y, w, h)
	// quintuples per input cell, or an empty array on overflow/bad input.
	static PackedInt32Array pack_shelf(const PackedInt32Array &p_sizes);

	Error create_blank(int p_page_count, int p_shadow_offset);
	Error load_from_bytes(const PackedByteArray &p_bytes);
	PackedByteArray to_bytes() const;

	int get_page_count() const;
	int get_glyph_count() const;
	int get_first_char() const;
	int get_shadow_offset() const;
	void set_shadow_offset(int p_shadow_offset);

	Ref<Image> get_page_image(int p_page) const;
	Error set_page_image(int p_page, const Ref<Image> &p_image);

	int get_glyph_page(int p_char_code) const;
	Rect2i get_glyph_rect(int p_char_code) const;
	Error set_glyph_rect(int p_char_code, int p_page, const Rect2i &p_rect);

	int get_pixel_alpha(int p_page, int p_x, int p_y) const;
	Error set_pixel_alpha(int p_page, int p_x, int p_y, int p_alpha);

	Ref<FontFile> to_font_file() const;

private:
	fnt_font_t font_;
	bool valid_ = false;

	void _clear();
	bool _has_valid_font() const;
	bool _char_to_index(int p_char_code, uint32_t &r_index) const;
};

} // namespace godot

#endif // OPENNOVA_FNT_NOVA_FNT_RESOURCE_H
