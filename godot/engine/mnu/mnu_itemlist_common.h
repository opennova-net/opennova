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
// scrolling, activation, and shell-facing row interface, but suppress only its
// built-in glyphs and redraw those glyphs over the same live item rectangles.
// Because the overlay reads ItemList on every draw, rows added later by a Menu
// Shell through inherited add_item()/set_item_text() receive the authored layout
// without a second data model.
struct MnuItemListTextPalette {
	Color normal = Color(1, 1, 1, 1);
	Color hovered = Color(1, 1, 1, 1);
	Color hovered_selected = Color(1, 1, 1, 1);
	Color selected = Color(1, 1, 1, 1);
	Color disabled = Color(1, 1, 1, 0.5f);
	bool has_disabled = false;
};

struct MnuItemListTextLayout {
	HorizontalAlignment horizontal = HORIZONTAL_ALIGNMENT_LEFT;
	VerticalAlignment vertical = VERTICAL_ALIGNMENT_CENTER;
	bool enabled = false;
	bool widget_disabled = false;
	bool palette_configured = false;

	MnuItemListTextPalette palette;
	Color font_outline_color = Color(0, 0, 0, 1);

	void set_palette(const MnuItemListTextPalette &p_palette) {
		palette = p_palette;
		palette_configured = true;
	}

	void set_widget_disabled(bool p_disabled) {
		widget_disabled = p_disabled;
	}

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
			if (!palette_configured) {
				palette.normal = p_list->get_theme_color("font_color");
				palette.hovered = p_list->get_theme_color("font_hovered_color");
				palette.hovered_selected =
						p_list->get_theme_color("font_hovered_selected_color");
				palette.selected = p_list->get_theme_color("font_selected_color");
				palette.disabled = palette.normal;
				palette.disabled.a *= 0.5f;
			}
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

	Color color_for(ItemList *p_list, int p_index, bool p_hovered) const {
		if (p_index < 0 || p_index >= p_list->get_item_count()) {
			return palette.normal;
		}

		const bool selected = p_list->is_selected(p_index);
		Color color;
		if (selected && p_hovered) {
			color = palette.hovered_selected;
		} else if (selected) {
			color = palette.selected;
		} else if (p_hovered) {
			color = palette.hovered;
		} else {
			// ItemList uses Color() (opaque black) as its "no custom
			// foreground" sentinel. Alpha-testing that value paints every
			// ordinary row black; match ItemList's own exact sentinel check.
			const Color custom_color = p_list->get_item_custom_fg_color(p_index);
			color = custom_color != Color() ? custom_color : palette.normal;
		}
		if (widget_disabled || p_list->is_item_disabled(p_index)) {
			if (palette.has_disabled) {
				color = palette.disabled;
			} else {
				color.a *= 0.5f;
			}
		}
		return color;
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

			const Color color = color_for(p_list, i, hovered == i);

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
