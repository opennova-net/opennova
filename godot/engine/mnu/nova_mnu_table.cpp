#include "nova_mnu_table.h"

#include "nova_mnu_scroll.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>

using namespace godot;

void NovaMnuTable::add_column(int p_width, int p_h_align, bool p_bitmap_draw,
		int p_v_align, bool p_scale_bitmap, bool p_custom_draw) {
	ColumnDef col;
	col.width = p_width > 0 ? p_width : 80;
	col.h_align = p_h_align;
	col.v_align = p_v_align;
	col.bitmap_draw = p_bitmap_draw;
	col.scale_bitmap = p_scale_bitmap;
	col.custom_draw = p_custom_draw;
	columns_.push_back(col);
}

void NovaMnuTable::set_parts(Control *p_header, Control *p_viewport, Control *p_rows,
		NovaMnuScroll *p_scrollbar) {
	header_row_ = p_header;
	viewport_ = p_viewport;
	rows_container_ = p_rows;
	scrollbar_ = p_scrollbar;
}

float NovaMnuTable::column_x(int p_col) const {
	float x = 0.0f;
	for (int i = 0; i < p_col && i < static_cast<int>(columns_.size()); ++i) {
		x += columns_[i].width + column_spacing_;
	}
	return x;
}

float NovaMnuTable::total_width() const {
	float w = 0.0f;
	for (size_t i = 0; i < columns_.size(); ++i) {
		w += columns_[i].width;
		if (i + 1 < columns_.size()) {
			w += column_spacing_;
		}
	}
	return w;
}

float NovaMnuTable::scrollbar_width() const {
	if (scrollbar_ == nullptr) {
		return 0.0f;
	}
	if (has_scrollbar_rect_ && scrollbar_rect_.size.x > 0.0f) {
		return scrollbar_rect_.size.x;
	}
	return 16.0f;
}

void NovaMnuTable::_ready() {
	connect("resized", callable_mp(this, &NovaMnuTable::layout));
	if (scrollbar_ != nullptr && rows_container_ != nullptr) {
		scrollbar_->link_scroll_target(scrollbar_->get_path_to(rows_container_));
	}
	layout();
	rebuild_rows();
}

void NovaMnuTable::layout() {
	if (header_row_ == nullptr) {
		return;
	}
	const Vector2 sz = get_size();
	const float sbw = scrollbar_width();
	const float headerh = (float)row_height_;
	const float body_h = sz.y - headerh > 0.0f ? sz.y - headerh : 0.0f;

	header_row_->set_position(Vector2(0, 0));
	header_row_->set_size(Vector2(sz.x, headerh));

	// Scrollbar geometry: honor the authored <SCROLLBAR><POSITION> (table-relative)
	// when present, else a default right-edge strip below the header. Rows stop at
	// the scrollbar's left edge so they never underlap it.
	float content_w = sz.x - sbw;
	if (scrollbar_ != nullptr) {
		if (has_scrollbar_rect_) {
			scrollbar_->set_position(scrollbar_rect_.position);
			scrollbar_->set_size(Vector2(sbw,
					scrollbar_rect_.size.y > 0.0f ? scrollbar_rect_.size.y : body_h));
			content_w = scrollbar_rect_.position.x;
		} else {
			scrollbar_->set_position(Vector2(sz.x - sbw, headerh));
			scrollbar_->set_size(Vector2(sbw, body_h));
		}
	}
	if (viewport_ != nullptr) {
		viewport_->set_position(Vector2(0, headerh));
		viewport_->set_size(Vector2(content_w > 0.0f ? content_w : 0.0f, body_h));
	}
	update_scrollbar();
}

void NovaMnuTable::update_scrollbar() {
	if (scrollbar_ == nullptr || viewport_ == nullptr) {
		return;
	}
	const double total = (double)rows_.size() * row_height_;
	const double page = viewport_->get_size().y;
	scrollbar_->set_range(0.0, total, page);
}

void NovaMnuTable::rebuild_rows() {
	if (rows_container_ == nullptr) {
		return;
	}
	while (rows_container_->get_child_count() > 0) {
		Node *c = rows_container_->get_child(0);
		rows_container_->remove_child(c);
		c->queue_free();
	}

	const float width = total_width();
	const bool has_custom_shell =
			!behavior_.edit_mode && has_connections(StringName("custom_cell_requested"));
	for (int r = 0; r < static_cast<int>(rows_.size()); ++r) {
		Control *row = memnew(Control);
		row->set_name(String("Row") + String::num_int64(r));
		row->set_position(Vector2(0, r * row_height_));
		row->set_size(Vector2(width, row_height_));
		row->set_mouse_filter(Control::MOUSE_FILTER_STOP);
		row->connect("gui_input", callable_mp(this, &NovaMnuTable::on_row_input).bind(r));

		if (selected_rows_.count(r) > 0) {
			ColorRect *bg = memnew(ColorRect);
			bg->set_name("Selected");
			bg->set_color(selection_color_);
			bg->set_anchors_preset(Control::PRESET_FULL_RECT);
			bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			row->add_child(bg);
		}

		std::vector<Control *> custom_slots(columns_.size(), nullptr);
		for (int c = 0; c < static_cast<int>(columns_.size()); ++c) {
			const Cell cell = c < static_cast<int>(rows_[r].size()) ? rows_[r][c] : Cell();
			const float x = column_x(c);
			if (columns_[c].custom_draw && has_custom_shell) {
				// The table retains geometry/lifetime ownership. A connected shell
				// synchronously fills this ephemeral slot when the completed row is
				// parented below; every rebuild intentionally creates fresh slots.
				Control *slot = memnew(Control);
				slot->set_name(String("Cell") + String::num_int64(c));
				slot->set_position(Vector2(x, 0));
				slot->set_size(Vector2(columns_[c].width, row_height_));
				slot->set_clip_contents(true);
				slot->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
				row->add_child(slot);
				custom_slots[c] = slot;
			} else if (columns_[c].bitmap_draw || cell.image.is_valid()) {
				TextureRect *tr = memnew(TextureRect);
				tr->set_name(String("Cell") + String::num_int64(c));
				if (cell.image.is_valid()) {
					tr->set_texture(cell.image);
				}
				Vector2 cell_pos(x, 0);
				Vector2 cell_size(columns_[c].width, row_height_);
				if (!columns_[c].scale_bitmap && cell.image.is_valid()) {
					cell_size.x = MIN(cell_size.x, static_cast<float>(cell.image->get_width()));
					cell_size.y = MIN(cell_size.y, static_cast<float>(cell.image->get_height()));
					if (columns_[c].h_align == HORIZONTAL_ALIGNMENT_CENTER) {
						cell_pos.x += (columns_[c].width - cell_size.x) * 0.5f;
					} else if (columns_[c].h_align == HORIZONTAL_ALIGNMENT_RIGHT) {
						cell_pos.x += columns_[c].width - cell_size.x;
					}
					if (columns_[c].v_align == VERTICAL_ALIGNMENT_CENTER) {
						cell_pos.y += (row_height_ - cell_size.y) * 0.5f;
					} else if (columns_[c].v_align == VERTICAL_ALIGNMENT_BOTTOM) {
						cell_pos.y += row_height_ - cell_size.y;
					}
				}
				tr->set_position(cell_pos);
				tr->set_size(cell_size);
				tr->set_clip_contents(true);
				tr->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
				tr->set_stretch_mode(columns_[c].scale_bitmap
								? TextureRect::STRETCH_SCALE
								: TextureRect::STRETCH_KEEP_CENTERED);
				tr->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
				row->add_child(tr);
			} else {
				Label *lbl = memnew(Label);
				lbl->set_name(String("Cell") + String::num_int64(c));
				lbl->set_position(Vector2(x, 0));
				lbl->set_size(Vector2(columns_[c].width, row_height_));
				lbl->set_text(cell.text);
				lbl->set_horizontal_alignment((HorizontalAlignment)columns_[c].h_align);
				lbl->set_vertical_alignment((VerticalAlignment)columns_[c].v_align);
				lbl->set_clip_text(true);
				lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
				if (cell_font_.is_valid()) {
					lbl->add_theme_font_override("font", cell_font_);
				}
				if (cell_font_size_ > 0) {
					lbl->add_theme_font_size_override("font_size", cell_font_size_);
				}
				if (has_cell_font_color_) {
					lbl->add_theme_color_override("font_color", cell_font_color_);
				}
				row->add_child(lbl);
			}
		}
		// Per-row rule (the ITEMS %TRIM_COLOR% outline) so rows read as a grid.
		if (has_outline_) {
			ColorRect *rule = memnew(ColorRect);
			rule->set_name("Rule");
			rule->set_color(outline_color_);
			rule->set_position(Vector2(0, row_height_ - 1));
			rule->set_size(Vector2(width, 1));
			rule->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			row->add_child(rule);
		}
		rows_container_->add_child(row);
		for (int c = 0; c < static_cast<int>(custom_slots.size()); ++c) {
			if (custom_slots[c] == nullptr) {
				continue;
			}
			const Cell cell = c < static_cast<int>(rows_[r].size()) ? rows_[r][c] : Cell();
			emit_signal("custom_cell_requested", r, c,
					cell.value.is_empty() ? cell.text : cell.value, custom_slots[c]);
		}
	}
	rows_container_->set_size(Vector2(width, rows_.size() * row_height_));
	update_scrollbar();
}

void NovaMnuTable::on_row_input(const Ref<InputEvent> &p_event, int p_row) {
	if (behavior_.edit_mode) {
		return;
	}
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_pressed()) {
		if (mb->is_double_click()) {
			emit_signal("row_activated", p_row);
			return;
		}
		const bool additive = multiselect_ && (mb->is_ctrl_pressed() || mb->is_shift_pressed());
		select_row(p_row, additive);
	}
}

int NovaMnuTable::add_row() {
	rows_.push_back(std::vector<Cell>(columns_.size()));
	rebuild_rows();
	return static_cast<int>(rows_.size()) - 1;
}

int NovaMnuTable::add_row_values(const PackedStringArray &p_cells) {
	std::vector<Cell> row(columns_.size());
	for (int c = 0; c < static_cast<int>(columns_.size()) && c < p_cells.size(); ++c) {
		row[c].text = p_cells[c];
	}
	rows_.push_back(row);
	rebuild_rows();
	return static_cast<int>(rows_.size()) - 1;
}

void NovaMnuTable::add_rows(const TypedArray<PackedStringArray> &p_rows) {
	for (int i = 0; i < p_rows.size(); ++i) {
		const PackedStringArray cells = p_rows[i];
		std::vector<Cell> row(columns_.size());
		for (int c = 0; c < static_cast<int>(columns_.size()) && c < cells.size(); ++c) {
			row[c].text = cells[c];
		}
		rows_.push_back(row);
	}
	rebuild_rows();
}

void NovaMnuTable::set_row(int p_row, const PackedStringArray &p_cells) {
	if (p_row < 0 || p_row >= static_cast<int>(rows_.size())) {
		return;
	}
	for (int c = 0; c < static_cast<int>(columns_.size()) && c < p_cells.size(); ++c) {
		rows_[p_row][c].text = p_cells[c];
		rows_[p_row][c].image = Ref<Texture2D>();
		rows_[p_row][c].value = String();
	}
	rebuild_rows();
}

void NovaMnuTable::set_cell_text(int p_row, int p_col, const String &p_text) {
	if (p_row < 0 || p_row >= static_cast<int>(rows_.size()) || p_col < 0 ||
			p_col >= static_cast<int>(columns_.size())) {
		return;
	}
	rows_[p_row][p_col].text = p_text;
	rows_[p_row][p_col].image = Ref<Texture2D>();
	rows_[p_row][p_col].value = String();
	rebuild_rows();
}

void NovaMnuTable::set_cell_value(int p_row, int p_col, const String &p_value) {
	if (p_row < 0 || p_row >= static_cast<int>(rows_.size()) || p_col < 0 ||
			p_col >= static_cast<int>(columns_.size())) {
		return;
	}
	rows_[p_row][p_col].value = p_value;
	const auto it = subst_.find(std::make_pair(p_col, p_value));
	if (it != subst_.end()) {
		rows_[p_row][p_col].image = it->second; // value -> image substitution
		rows_[p_row][p_col].text = String();
	} else {
		rows_[p_row][p_col].text = p_value;
		rows_[p_row][p_col].image = Ref<Texture2D>();
	}
	rebuild_rows();
}

void NovaMnuTable::set_cell_image(int p_row, int p_col, const Ref<Texture2D> &p_tex) {
	if (p_row < 0 || p_row >= static_cast<int>(rows_.size()) || p_col < 0 ||
			p_col >= static_cast<int>(columns_.size())) {
		return;
	}
	rows_[p_row][p_col].image = p_tex;
	rebuild_rows();
}

void NovaMnuTable::remove_row(int p_row) {
	if (p_row < 0 || p_row >= static_cast<int>(rows_.size())) {
		return;
	}
	rows_.erase(rows_.begin() + p_row);
	// Re-index the selection: drop the removed row, shift higher indices down one.
	std::set<int> updated;
	for (const int r : selected_rows_) {
		if (r < p_row) {
			updated.insert(r);
		} else if (r > p_row) {
			updated.insert(r - 1);
		}
	}
	selected_rows_.swap(updated);
	rebuild_rows();
}

void NovaMnuTable::clear_rows() {
	rows_.clear();
	selected_rows_.clear();
	rebuild_rows();
}

String NovaMnuTable::get_cell_text(int p_row, int p_col) const {
	if (p_row < 0 || p_row >= static_cast<int>(rows_.size()) || p_col < 0 ||
			p_col >= static_cast<int>(rows_[p_row].size())) {
		return String();
	}
	return rows_[p_row][p_col].text;
}

PackedInt32Array NovaMnuTable::get_selected_rows() const {
	PackedInt32Array out;
	for (const int r : selected_rows_) {
		out.push_back(r);
	}
	return out;
}

int NovaMnuTable::get_selected_row() const {
	return selected_rows_.empty() ? -1 : *selected_rows_.begin();
}

void NovaMnuTable::select_row(int p_row, bool p_additive) {
	if (p_row < 0 || p_row >= static_cast<int>(rows_.size())) {
		return;
	}
	// The authored ITEMS/MULTISELECT policy is authoritative for both mouse
	// input and shell/API calls.  A caller cannot force an additive selection on
	// a single-select table by passing true here.
	p_additive = multiselect_ && p_additive;
	if (!p_additive) {
		selected_rows_.clear();
		selected_rows_.insert(p_row);
	} else if (selected_rows_.count(p_row) > 0) {
		selected_rows_.erase(p_row);
	} else {
		selected_rows_.insert(p_row);
	}
	emit_signal("row_selected", p_row);
	emit_signal("selection_changed", get_selected_rows());
	rebuild_rows();
}

void NovaMnuTable::clear_selection() {
	selected_rows_.clear();
	rebuild_rows();
}

void NovaMnuTable::sort_by_column(int p_col, bool p_ascending) {
	if (p_col < 0 || p_col >= static_cast<int>(columns_.size())) {
		return;
	}
	sort_column_ = p_col;
	sort_ascending_ = p_ascending;
	std::stable_sort(rows_.begin(), rows_.end(),
			[p_col, p_ascending](const std::vector<Cell> &a, const std::vector<Cell> &b) {
				const String av = p_col < static_cast<int>(a.size()) ? a[p_col].text : String();
				const String bv = p_col < static_cast<int>(b.size()) ? b[p_col].text : String();
				return p_ascending ? (av < bv) : (bv < av);
			});
	selected_rows_.clear(); // row indices are no longer meaningful after a sort
	rebuild_rows();
	emit_signal("column_sorted", p_col, p_ascending);
}

void NovaMnuTable::header_clicked(int p_col) {
	if (behavior_.edit_mode) {
		return;
	}
	const bool ascending = (sort_column_ == p_col) ? !sort_ascending_ : true;
	sort_by_column(p_col, ascending);
}

void NovaMnuTable::rebuild() {
	rebuild_rows();
}

void NovaMnuTable::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_column", "width", "h_align", "bitmap_draw", "v_align",
								 "scale_bitmap", "custom_draw"),
			&NovaMnuTable::add_column, DEFVAL(1), DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("add_row"), &NovaMnuTable::add_row);
	ClassDB::bind_method(D_METHOD("add_row_values", "cells"), &NovaMnuTable::add_row_values);
	ClassDB::bind_method(D_METHOD("add_rows", "rows"), &NovaMnuTable::add_rows);
	ClassDB::bind_method(D_METHOD("set_row", "row", "cells"), &NovaMnuTable::set_row);
	ClassDB::bind_method(D_METHOD("set_cell_text", "row", "col", "text"), &NovaMnuTable::set_cell_text);
	ClassDB::bind_method(D_METHOD("set_cell_value", "row", "col", "value"), &NovaMnuTable::set_cell_value);
	ClassDB::bind_method(D_METHOD("set_cell_image", "row", "col", "tex"), &NovaMnuTable::set_cell_image);
	ClassDB::bind_method(D_METHOD("remove_row", "row"), &NovaMnuTable::remove_row);
	ClassDB::bind_method(D_METHOD("clear_rows"), &NovaMnuTable::clear_rows);
	ClassDB::bind_method(D_METHOD("get_row_count"), &NovaMnuTable::get_row_count);
	ClassDB::bind_method(D_METHOD("get_column_count"), &NovaMnuTable::get_column_count);
	ClassDB::bind_method(D_METHOD("get_cell_text", "row", "col"), &NovaMnuTable::get_cell_text);
	ClassDB::bind_method(D_METHOD("get_selected_rows"), &NovaMnuTable::get_selected_rows);
	ClassDB::bind_method(D_METHOD("get_selected_row"), &NovaMnuTable::get_selected_row);
	ClassDB::bind_method(D_METHOD("select_row", "row", "additive"), &NovaMnuTable::select_row, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_selection"), &NovaMnuTable::clear_selection);
	ClassDB::bind_method(D_METHOD("sort_by_column", "col", "ascending"), &NovaMnuTable::sort_by_column);
	ClassDB::bind_method(D_METHOD("header_clicked", "col"), &NovaMnuTable::header_clicked);
	ClassDB::bind_method(D_METHOD("rebuild"), &NovaMnuTable::rebuild);

	ADD_SIGNAL(MethodInfo("row_selected", PropertyInfo(Variant::INT, "row")));
	ADD_SIGNAL(MethodInfo("selection_changed", PropertyInfo(Variant::PACKED_INT32_ARRAY, "rows")));
	ADD_SIGNAL(MethodInfo("row_activated", PropertyInfo(Variant::INT, "row")));
	ADD_SIGNAL(MethodInfo("column_sorted", PropertyInfo(Variant::INT, "column"),
			PropertyInfo(Variant::BOOL, "ascending")));
	ADD_SIGNAL(MethodInfo("custom_cell_requested", PropertyInfo(Variant::INT, "row"),
			PropertyInfo(Variant::INT, "column"), PropertyInfo(Variant::STRING, "value"),
			PropertyInfo(Variant::OBJECT, "slot", PROPERTY_HINT_NODE_TYPE, "Control")));
}
