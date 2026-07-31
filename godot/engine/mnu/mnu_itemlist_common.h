#pragma once

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

// Shared ItemList runtime-data helpers for NovaMnuList / NovaMnuMulti, which are
// ~identical apart from select mode and which selection signal they relay. Kept as
// free functions (not a GDCLASS base) because both already subclass ItemList and
// GDExtension types cannot insert a shared base between them and that parent.

// Replace all rows with `items` (the common "here is the whole list" call).
inline void mnu_itemlist_set_items(ItemList *list, const PackedStringArray &items) {
	list->clear();
	for (int i = 0; i < items.size(); ++i) {
		list->add_item(items[i]);
	}
}

// First selected row, or -1 when nothing is selected.
inline int mnu_itemlist_first_selected(ItemList *list) {
	const PackedInt32Array sel = list->get_selected_items();
	return sel.is_empty() ? -1 : sel[0];
}

// ItemList does not expose per-row text alignment. Keep its native selection,
// scrolling, activation, and host-facing row interface, but suppress only its
// built-in glyphs and redraw those glyphs over the same live item rectangles.
// Because the overlay reads ItemList on every draw, rows added later by a Menu
// Host through inherited add_item()/set_item_text() receive the authored layout
// without a second data model.
struct MnuItemListTextLayout {
	HorizontalAlignment horizontal = HORIZONTAL_ALIGNMENT_LEFT;
	VerticalAlignment vertical = VERTICAL_ALIGNMENT_CENTER;
	bool enabled = false;

	Color font_color;
	Color font_hovered_color;
	Color font_hovered_selected_color;
	Color font_selected_color;
	Color font_outline_color;

	void configure(ItemList *p_list, int p_horizontal, int p_vertical) {
		switch (p_horizontal) {
			case HORIZONTAL_ALIGNMENT_CENTER:
				horizontal = HORIZONTAL_ALIGNMENT_CENTER;
				break;
			case HORIZONTAL_ALIGNMENT_RIGHT:
				horizontal = HORIZONTAL_ALIGNMENT_RIGHT;
				break;
			default:
				horizontal = HORIZONTAL_ALIGNMENT_LEFT;
				break;
		}
		switch (p_vertical) {
			case VERTICAL_ALIGNMENT_TOP:
				vertical = VERTICAL_ALIGNMENT_TOP;
				break;
			case VERTICAL_ALIGNMENT_BOTTOM:
				vertical = VERTICAL_ALIGNMENT_BOTTOM;
				break;
			default:
				vertical = VERTICAL_ALIGNMENT_CENTER;
				break;
		}

		if (!enabled) {
			font_color = p_list->get_theme_color("font_color");
			font_hovered_color = p_list->get_theme_color("font_hovered_color");
			font_hovered_selected_color =
					p_list->get_theme_color("font_hovered_selected_color");
			font_selected_color = p_list->get_theme_color("font_selected_color");
			font_outline_color = p_list->get_theme_color("font_outline_color");

			const Color transparent(1, 1, 1, 0);
			p_list->add_theme_color_override("font_color", transparent);
			p_list->add_theme_color_override("font_hovered_color", transparent);
			p_list->add_theme_color_override("font_hovered_selected_color", transparent);
			p_list->add_theme_color_override("font_selected_color", transparent);
			p_list->add_theme_color_override("font_outline_color", transparent);
			enabled = true;
		}
		p_list->queue_redraw();
	}

	void draw(ItemList *p_list) const {
		if (!enabled) {
			return;
		}
		const Ref<Font> font = p_list->get_theme_font("font");
		if (font.is_null()) {
			return;
		}
		const int font_size = p_list->get_theme_font_size("font_size");
		const int outline_size = p_list->get_theme_constant("outline_size");
		const float ascent = font->get_ascent(font_size);
		const float descent = font->get_descent(font_size);
		const float font_height = font->get_height(font_size);

		int hovered = -1;
		const Vector2 mouse = p_list->get_local_mouse_position();
		if (Rect2(Vector2(), p_list->get_size()).has_point(mouse)) {
			hovered = p_list->get_item_at_position(mouse, true);
		}

		for (int i = 0; i < p_list->get_item_count(); ++i) {
			const Rect2 rect = p_list->get_item_rect(i, true);
			if (rect.position.y + rect.size.y < 0 ||
					rect.position.y > p_list->get_size().y) {
				continue;
			}

			float baseline = rect.position.y + ascent;
			if (vertical == VERTICAL_ALIGNMENT_CENTER) {
				baseline = rect.position.y + (rect.size.y - font_height) * 0.5f + ascent;
			} else if (vertical == VERTICAL_ALIGNMENT_BOTTOM) {
				baseline = rect.position.y + rect.size.y - descent;
			}

			const bool selected = p_list->is_selected(i);
			Color color = selected ? font_selected_color : font_color;
			if (hovered == i) {
				color = selected ? font_hovered_selected_color : font_hovered_color;
			}
			const Color custom_color = p_list->get_item_custom_fg_color(i);
			if (custom_color.a > 0.0f) {
				color = custom_color;
			}
			if (p_list->is_item_disabled(i)) {
				color.a *= 0.5f;
			}

			const Vector2 origin(rect.position.x, baseline);
			if (outline_size > 0 && font_outline_color.a > 0.0f) {
				p_list->draw_string_outline(font, origin, p_list->get_item_text(i),
						horizontal, rect.size.x, font_size, outline_size,
						font_outline_color);
			}
			p_list->draw_string(font, origin, p_list->get_item_text(i),
					horizontal, rect.size.x, font_size, color);
		}
	}
};

} // namespace godot
