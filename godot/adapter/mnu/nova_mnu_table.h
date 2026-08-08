#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <map>
#include <set>
#include <utility>
#include <vector>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;
class NovaMnuScroll;

// A table view (type="table"). Custom composition (a header row + a clipped
// viewport over a shell-populated rows container + an embedded NovaMnuScroll) rather
// than Godot's Tree, because the MNU TableScrollbar art, per-column BODY bitmap
// columns, and value->image SUBST cells don't map onto Tree's fixed theming.
//
// The .mnu supplies the column template (widths, justification, headers, body
// formatting, value substitutions); rows are populated at runtime by the engine
// (mission tables, server browsers) via add_row_values()/set_cell_*. Selection and
// header-click sort relay through signals and the owning menu. Inert in edit_mode
// (sample rows show but selection/sort are not wired).
class NovaMnuTable : public Control {
	GDCLASS(NovaMnuTable, Control)

private:
	struct ColumnDef {
		int width = 80;
		int h_align = 0; // HORIZONTAL_ALIGNMENT_LEFT
		int v_align = 1; // VERTICAL_ALIGNMENT_CENTER
		bool bitmap_draw = false;
		bool scale_bitmap = false;
		bool custom_draw = false;
	};
	struct Cell {
		String text;
		Ref<Texture2D> image;
		String value;
	};

	MnuWidgetBehavior behavior_;
	std::vector<ColumnDef> columns_;
	int column_spacing_ = 0;
	int row_height_ = 16;
	bool multiselect_ = false;
	Color selection_color_ = Color(0.2f, 0.4f, 0.8f, 1.0f);
	bool has_outline_ = false;
	Color outline_color_ = Color(0.2f, 0.25f, 0.35f, 1.0f);
	Ref<Font> cell_font_;
	int cell_font_size_ = 0;
	bool has_cell_font_color_ = false;
	Color cell_font_color_ = Color(1, 1, 1, 1);
	std::map<std::pair<int, String>, Ref<Texture2D>> subst_;

	std::vector<std::vector<Cell>> rows_;
	std::set<int> selected_rows_;
	int sort_column_ = -1;
	bool sort_ascending_ = true;

	Control *header_row_ = nullptr;
	Control *viewport_ = nullptr;
	Control *rows_container_ = nullptr;
	NovaMnuScroll *scrollbar_ = nullptr;
	Rect2 scrollbar_rect_;          // authored <SCROLLBAR><POSITION>, table-relative
	bool has_scrollbar_rect_ = false;

	float column_x(int p_col) const;
	float total_width() const;
	float scrollbar_width() const;
	void layout();
	void rebuild_rows();
	void update_scrollbar();
	void on_row_input(const Ref<InputEvent> &p_event, int p_row);

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void add_column(int p_width, int p_h_align, bool p_bitmap_draw,
			int p_v_align = 1, bool p_scale_bitmap = false, bool p_custom_draw = false);
	void set_column_spacing(int p_s) { column_spacing_ = p_s; }
	void set_row_height(int p_h) {
		if (p_h > 0) {
			row_height_ = p_h;
		}
	}
	void set_multiselect(bool p_m) { multiselect_ = p_m; }
	void set_colors(const Color &p_outline, bool p_has_outline, const Color &p_selection) {
		outline_color_ = p_outline;
		has_outline_ = p_has_outline;
		selection_color_ = p_selection;
	}
	void set_cell_font(const Ref<Font> &p_font, int p_size) {
		cell_font_ = p_font;
		cell_font_size_ = p_size;
	}
	void set_cell_font_color(const Color &p_color) {
		cell_font_color_ = p_color;
		has_cell_font_color_ = true;
	}
	void add_substitution(int p_col, const String &p_value, const Ref<Texture2D> &p_tex) {
		subst_[std::make_pair(p_col, p_value)] = p_tex;
	}
	void set_parts(Control *p_header, Control *p_viewport, Control *p_rows, NovaMnuScroll *p_scrollbar);
	void set_scrollbar_rect(const Rect2 &p_rect) {
		scrollbar_rect_ = p_rect;
		has_scrollbar_rect_ = true;
	}

	// --- Runtime data binding ---
	int add_row();
	int add_row_values(const PackedStringArray &p_cells);
	void add_rows(const TypedArray<PackedStringArray> &p_rows); // append many, rebuild once
	void set_row(int p_row, const PackedStringArray &p_cells);
	void set_cell_text(int p_row, int p_col, const String &p_text);
	void set_cell_value(int p_row, int p_col, const String &p_value);
	void set_cell_image(int p_row, int p_col, const Ref<Texture2D> &p_tex);
	void remove_row(int p_row);
	void clear_rows();
	int get_row_count() const { return static_cast<int>(rows_.size()); }
	int get_column_count() const { return static_cast<int>(columns_.size()); }
	String get_cell_text(int p_row, int p_col) const;

	PackedInt32Array get_selected_rows() const;
	int get_selected_row() const;
	void select_row(int p_row, bool p_additive = false);
	void clear_selection();
	void sort_by_column(int p_col, bool p_ascending);
	void header_clicked(int p_col);
	void rebuild(); // public manual rebuild after a batch of cell mutations
};

} // namespace godot
