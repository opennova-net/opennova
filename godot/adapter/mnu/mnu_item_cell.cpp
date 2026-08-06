#include "mnu_item_cell.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/node_path.hpp>

using namespace godot;

void godot::mnu_build_item_cell(Control *mount, HorizontalAlignment halign,
		const Ref<LabelSettings> &label_settings) {
	if (mount == nullptr) {
		return;
	}

	// Text / id items: a centered label, styled with the widget font.
	Label *label = memnew(Label);
	label->set_name("CellLabel");
	label->set_anchors_preset(Control::PRESET_FULL_RECT);
	label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	label->set_horizontal_alignment(halign);
	label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	if (label_settings.is_valid()) {
		label->set_label_settings(label_settings);
	}
	mount->add_child(label);

	// Image items: the texture at its native size, centered in the cell rect
	// [orig: CSpinListWnd_Render @ 0x64b220 aligns by the native texture extents]. The
	// menu root scales the whole tree anamorphically, so no per-cell stretch is needed.
	TextureRect *image = memnew(TextureRect);
	image->set_name("CellImage");
	image->set_anchors_preset(Control::PRESET_FULL_RECT);
	image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	image->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	image->set_visible(false);
	mount->add_child(image);

	// Color items: a swatch filling the whole cell rect [orig: CSpinListWnd_Render
	// draws the packed RRGGBB color over the element's default rect].
	ColorRect *swatch = memnew(ColorRect);
	swatch->set_name("CellSwatch");
	swatch->set_anchors_preset(Control::PRESET_FULL_RECT);
	swatch->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	swatch->set_visible(false);
	mount->add_child(swatch);
}

void godot::mnu_show_item_cell(Control *mount, const MnuItemVisual &visual) {
	if (mount == nullptr) {
		return;
	}
	Label *label = Object::cast_to<Label>(mount->get_node_or_null(NodePath("CellLabel")));
	TextureRect *image = Object::cast_to<TextureRect>(mount->get_node_or_null(NodePath("CellImage")));
	ColorRect *swatch = Object::cast_to<ColorRect>(mount->get_node_or_null(NodePath("CellSwatch")));

	// An image item with an unresolved texture falls back to its text (the filename),
	// matching the graceful degradation elsewhere in the builder.
	const bool is_image = visual.kind == MnuItemVisual::IMAGE && visual.texture.is_valid();
	const bool is_color = visual.kind == MnuItemVisual::COLOR;

	if (label != nullptr) {
		label->set_visible(!is_image && !is_color);
		label->set_text(visual.text);
	}
	if (image != nullptr) {
		image->set_visible(is_image);
		if (is_image) {
			image->set_texture(visual.texture);
		}
	}
	if (swatch != nullptr) {
		swatch->set_visible(is_color);
		if (is_color) {
			swatch->set_color(visual.color);
		}
	}
}
