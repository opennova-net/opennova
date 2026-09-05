#pragma once

#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>

#include <formats/fnt/fnt.h>

namespace godot {

class FntResource : public Resource {
	GDCLASS(FntResource, Resource)

protected:
	static void _bind_methods();

public:
	// FNT format facts, single-sourced from engine/formats/fnt (ENG-4): editor scripts
	// alias these instead of re-declaring the numbers.
	enum {
		FIRST_CHAR = opennova::fnt::FNT_FIRST_CHAR,
		GLYPH_COUNT = opennova::fnt::FNT_GLYPH_COUNT,
		TEXTURE_WIDTH = opennova::fnt::FNT_TEXTURE_WIDTH,
		TEXTURE_HEIGHT = opennova::fnt::FNT_TEXTURE_HEIGHT,
		MAX_PAGES = opennova::fnt::FNT_MAX_PAGES,
		PACK_PAD = opennova::fnt::FNT_PACK_PAD,
		MAGIC = opennova::fnt::FNT_MAGIC, /* "FNT0" little-endian — the header 4CC */
	};

	FntResource();
	~FntResource();

	// fnt_pack_shelf over (width, height) pairs; returns (page, x, y, w, h)
	// quintuples per input cell, or an empty array on overflow/bad input.
	static PackedInt32Array pack_shelf(const PackedInt32Array &p_sizes);

	Error create_blank(int p_page_count, int p_glyph_spacing);
	Error load_from_bytes(const PackedByteArray &p_bytes);
	PackedByteArray to_bytes() const;

	int get_page_count() const;
	int get_glyph_count() const;
	int get_first_char() const;
	int get_glyph_spacing() const;

	Ref<Image> get_page_image(int p_page) const;

	Rect2i get_glyph_rect(int p_char_code) const;
	Error set_glyph_rect(int p_char_code, int p_page, const Rect2i &p_rect);

	int get_pixel_alpha(int p_page, int p_x, int p_y) const;
	Error set_pixel_alpha(int p_page, int p_x, int p_y, int p_alpha);

	Ref<FontFile> to_font_file() const;

private:
	opennova::fnt::fnt_font_t font_;
	bool valid_ = false;

	void _clear();
	bool _has_valid_font() const;
	bool _char_to_index(int p_char_code, uint32_t &r_index) const;
};

} // namespace godot
