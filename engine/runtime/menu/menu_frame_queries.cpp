// The menu frame compiler's widget queries: what an embedder asks of a configured screen
// (a widget's rect, name, font, shown and disabled state, edit limits, item rows), the
// interaction geometry the same layout math gives (list, combo popup and spin arrow rows,
// the front-most hit), and a widget's parse-time mnemonic.
// [orig: CUIElement_ParseXMLDefinition @ 0x648120 parse tail; CWnd_HitTestPoint @ 0x646700;
//  CListWnd_DrawItems @ 0x643f30; CComboWnd @ 0x65be40; CSpinListWnd_CreateUpDownChildren
//  @ 0x64b8b0; CUIScene_EndFrame @ 0x63e600]
// Witness record: docs/mnu/menu-re.md ("Widget render dispatch").

#include <runtime/menu/menu_frame_internal.h>

#include <algorithm>
#include <climits>

namespace opennova::menu {

int MenuFrameCompiler::row_height_(const WidgetNode &node) const {
	// [orig: authored MIN_ITEM_HEIGHT wins, else the "W" measure —
	//  CListWnd_DrawItems @ 0x643f30 (D-MNU-8)]
	const mnu::Window &w = *node.window;
	// A combo's rows live in its nested LIST_BOX. Direct LIST/MULTI/LAN_LIST
	// syntax stores the sibling MIN_ITEM_HEIGHT in table_data.
	if (w.type == mnu::WindowType::Combo) {
		if (w.list_box && w.list_box->table_data.has_min_item_height &&
				w.list_box->table_data.min_item_height >= 0) {
			return w.list_box->table_data.min_item_height;
		}
	} else if (w.table_data.has_min_item_height &&
			w.table_data.min_item_height >= 0) {
		return w.table_data.min_item_height;
	}
	int tw = 0;
	int row_h = 0;
	measure_text(node, "W", &tw, &row_h);
	return row_h;
}

bool MenuFrameCompiler::widget_shown(
		int index, const MenuFrameState &state) const {
	return widget_shown_(index, state);
}

bool MenuFrameCompiler::widget_shown_(int index, const MenuFrameState &state) const {
	// The draw/hit walk's shown gate over the widget AND its ancestors.
	int i = index;
	while (i >= 0) {
		const WidgetNode &node = nodes_[static_cast<size_t>(i)];
		if (!node_shown(*node.window, state_for(state, i))) {
			return false;
		}
		i = node.parent;
	}
	return true;
}

int MenuFrameCompiler::widget_count() const {
	return document_nodes_;
}

std::string MenuFrameCompiler::widget_name(int index) const {
	if (index < 0 || index >= document_nodes_) {
		return std::string();
	}
	return nodes_[static_cast<size_t>(index)].window->name;
}

int MenuFrameCompiler::widget_kind(int index) const {
	if (index < 0 || index >= document_nodes_) {
		return -1;
	}
	return static_cast<int>(nodes_[static_cast<size_t>(index)].window->type);
}

int MenuFrameCompiler::widget_parent(int index) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return -1;
	}
	return nodes_[static_cast<size_t>(index)].parent;
}

bool MenuFrameCompiler::widget_font(int index, std::string *name, uint32_t colors[4]) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (node.font <= 0 || node.font >= static_cast<int32_t>(font_names_.size())) {
		return false;
	}
	if (name != nullptr) {
		*name = font_names_[static_cast<size_t>(node.font)];
	}
	if (colors != nullptr) {
		for (int state = 0; state < 4; ++state) {
			colors[state] = node.colors[state];
		}
	}
	return true;
}

std::string MenuFrameCompiler::widget_authored_text(int index) const {
	if (index < 0 || index >= document_nodes_) {
		return std::string();
	}
	return widget_text(nodes_[static_cast<size_t>(index)], nullptr);
}

std::string MenuFrameCompiler::widget_string(int index, const std::string &key) const {
	if (index < 0 || index >= document_nodes_ || text_tables_ == nullptr) {
		return key;
	}
	const std::string *text = text_tables_->lookup(nodes_[static_cast<size_t>(index)].text_table, key);
	return text != nullptr ? *text : key;
}

bool MenuFrameCompiler::widget_disabled(int index,
		const MenuFrameState &state) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	return disabled_(*node.window, state_for(state, index));
}

const mnu::Window *MenuFrameCompiler::widget_window(int index) const {
	if (index < 0 || index >= document_nodes_) {
		return nullptr;
	}
	return nodes_[static_cast<size_t>(index)].window;
}

bool MenuFrameCompiler::widget_reached(int index, const MenuFrameState &state) const {
	if (index < 0 || index >= document_nodes_ || !widget_shown_(index, state)) {
		return false;
	}
	// While a popup is open the pump serves its subtree alone.
	const int popup = state.popup_root;
	return popup < 0 || popup >= document_nodes_ || in_subtree_(index, popup);
}

bool MenuFrameCompiler::widget_live(int index, const MenuFrameState &state) const {
	if (!widget_reached(index, state)) {
		return false;
	}
	// Enabled at every level up the chain [orig: CWnd_IsVisibleInHierarchy @ 0x6462a0 tests +0xE4 at
	// each one].
	for (int i = index; i >= 0; i = nodes_[static_cast<size_t>(i)].parent) {
		if (disabled_(*nodes_[static_cast<size_t>(i)].window, state_for(state, i))) {
			return false;
		}
	}
	return true;
}

bool MenuFrameCompiler::disabled_(const mnu::Window &w, const MenuWidgetState *ws) {
	return ws != nullptr && ws->has_disabled ? ws->disabled : w.disabled;
}

bool MenuFrameCompiler::widget_edit_limits(int index, EditLimits *out) const {
	if (out == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	// [orig: the parsed edit constraints — read-only widget[194], numeric
	//  widget[196] with the [min widget[202], max widget[201]] range, max len
	//  widget[200]; CEditWnd_InsertChar @ 0x661ee0]
	const mnu::Window &w = *nodes_[static_cast<size_t>(index)].window;
	*out = EditLimits{};
	out->read_only = w.readonly;
	out->numeric = w.number;
	out->min_value = w.has_minval ? w.minval : 0;
	out->max_value = w.has_maxval ? w.maxval : 0;
	if (w.number && !w.has_minval && !w.has_maxval) {
		// NUMBER with no authored range: the range gate never rejects.
		out->min_value = LONG_MIN;
		out->max_value = LONG_MAX;
	} else if (w.number && !w.has_maxval) {
		out->max_value = LONG_MAX;
	} else if (w.number && !w.has_minval) {
		out->min_value = LONG_MIN;
	}
	out->max_len = w.has_maxchar ? w.maxchar : -1;
	return true;
}

bool MenuFrameCompiler::widget_rect(int index, const MenuFrameState &state,
		mnu::RectEdges *out) const {
	if (out == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	// absolute = own rect + every ancestor's origin (the same accumulation the
	// draw walk threads through origin_x/origin_y; a moved widget's runtime rect).
	mnu::RectEdges rect = node_rect_(nodes_[static_cast<size_t>(index)], state_for(state, index));
	int p = nodes_[static_cast<size_t>(index)].parent;
	while (p >= 0) {
		const mnu::RectEdges pr = node_rect_(nodes_[static_cast<size_t>(p)], state_for(state, p));
		rect = offset_rect(rect, pr.left, pr.top);
		p = nodes_[static_cast<size_t>(p)].parent;
	}
	*out = rect;
	return true;
}

bool MenuFrameCompiler::widget_local_rect(int index, const MenuFrameState &state,
		mnu::RectEdges *out) const {
	if (out == nullptr || index < 0 || index >= static_cast<int>(nodes_.size())) return false;
	*out = node_rect_(nodes_[static_cast<size_t>(index)], state_for(state, index));
	return true;
}

int MenuFrameCompiler::item_count(int index, const MenuFrameState &state) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return 0;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const MenuWidgetState *ws = state_for(state, index);
	if (ws != nullptr && ws->has_items) {
		return static_cast<int>(ws->items.size());
	}
	// A combo's selectable rows are its popup rows when the nested LIST_BOX
	// collection is authored (D-MNU-7/8).
	const mnu::Window &w = *node.window;
	if (w.type == mnu::WindowType::Combo && w.list_box && w.list_box->items.present) {
		return static_cast<int>(node.popup_items.size());
	}
	return static_cast<int>(node.items.size());
}

int MenuFrameCompiler::list_row_at(int index, const MenuFrameState &state,
		float mx, float my, float sx, float sy) const {
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return -1;
	}
	return row_at_in_rect_(index, state, rect, ScrollbarKind::Embedded, mx, my,
			sx, sy);
}

int MenuFrameCompiler::row_at_in_rect_(int index, const MenuFrameState &state,
		const mnu::RectEdges &rect, ScrollbarKind kind, float mx, float my,
		float sx, float sy) const {
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const int row_h = row_height_(node);
	const int count = item_count(index, state);
	if (row_h <= 0 || count == 0) {
		return -1;
	}
	if (mx < emit_x(rect.left, sx) || mx >= emit_x(rect.right, sx)) {
		return -1;
	}
	const MenuWidgetState *ws = state_for(state, index);
	const int first = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	const int visible = std::max((rect.bottom - rect.top) / row_h, 0);
	mnu::RectEdges scrollbar_rect;
	if (count > visible &&
			resolve_scrollbar_rect(node, kind, rect, 0,
					rect.bottom - rect.top, 22, &scrollbar_rect) &&
			mx >= emit_x(scrollbar_rect.left, sx) &&
			mx < emit_x(scrollbar_rect.right, sx) &&
			my >= emit_x(scrollbar_rect.top, sy) &&
			my < emit_x(scrollbar_rect.bottom, sy)) {
		// The original routes the child scrollbar before the list rows: the
		// covered strip never selects a row (the popup's part interaction
		// lives in pump_popup_mouse). [orig: CListWnd child walk @ 0x643f30]
		return -1;
	}
	int y = rect.top;
	for (int i = first; i < count; ++i) {
		if (y + row_h > rect.bottom) {
			break;
		}
		if (my >= emit_x(y, sy) && my < emit_x(y + row_h, sy)) {
			return i;
		}
		y += row_h;
	}
	return -1;
}

int MenuFrameCompiler::list_visible_rows(int index,
		const MenuFrameState &state) const {
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return 0;
	}
	const int row_h = row_height_(nodes_[static_cast<size_t>(index)]);
	if (row_h <= 0) {
		return 0;
	}
	return (rect.bottom - rect.top) / row_h;
}

bool MenuFrameCompiler::combo_popup_rect(int index, const MenuFrameState &state,
		mnu::RectEdges *out) const {
	if (out == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	const mnu::Window &w = *nodes_[static_cast<size_t>(index)].window;
	if (!w.list_box) {
		return false;
	}
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return false;
	}
	const mnu::RectEdges local = mnu::position_rect(
			w.list_box->position.has_left, w.list_box->position.left,
			w.list_box->position.has_top, w.list_box->position.top,
			w.list_box->position.has_right, w.list_box->position.right,
			w.list_box->position.has_bottom, w.list_box->position.bottom, 0, 0);
	*out = offset_rect(local, rect.left, rect.top);
	return true;
}

bool MenuFrameCompiler::combo_popup_contains(int index,
		const MenuFrameState &state, float mx, float my, float sx,
		float sy) const {
	mnu::RectEdges popup;
	if (!combo_popup_rect(index, state, &popup)) {
		return false;
	}
	return mx >= emit_x(popup.left, sx) && mx < emit_x(popup.right, sx) &&
			my >= emit_x(popup.top, sy) && my < emit_x(popup.bottom, sy);
}

int MenuFrameCompiler::combo_popup_row_at(int index,
		const MenuFrameState &state, float mx, float my, float sx,
		float sy) const {
	mnu::RectEdges popup;
	if (!combo_popup_rect(index, state, &popup)) {
		return -1;
	}
	return row_at_in_rect_(index, state, popup, ScrollbarKind::Popup, mx, my, sx,
			sy);
}

// Shared arrow hit over the spin list's ABSOLUTE rect: 0 none, 1 up, 2 down —
// the pump's claim walk and the driver's press routing use the same rects:
// each arrow window's own solved rect, parent-relative, PtInRect with no art
// check; a hidden arrow is never hit and the later (SPINDOWN) wins an overlap
// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0; CWnd_HitTestPoint
// @ 0x646700; the reverse child pump].
int MenuFrameCompiler::spin_arrow_hit_(const WidgetNode &node,
		const mnu::RectEdges &rect, const MenuFrameState &state, float mx,
		float my, float sx, float sy) const {
	(void)state;
	const auto arrow_hit = [&](int arrow) {
		if (arrow < 0) {
			return false;
		}
		const WidgetNode &part = nodes_[static_cast<size_t>(arrow)];
		if (!node_shown(*part.window, nullptr)) {
			return false;
		}
		const mnu::RectEdges abs = offset_rect(solve_rect(part), rect.left, rect.top);
		return mx >= emit_x(abs.left, sx) && mx < emit_x(abs.right, sx) &&
				my >= emit_x(abs.top, sy) && my < emit_x(abs.bottom, sy);
	};
	if (arrow_hit(node.spin_down)) {
		return 2;
	}
	if (arrow_hit(node.spin_up)) {
		return 1;
	}
	return 0;
}

int MenuFrameCompiler::spin_arrow_at(int index, const MenuFrameState &state,
		float mx, float my, float sx, float sy) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return 0;
	}
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return 0;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	return spin_arrow_hit_(node, rect, state, mx, my, sx, sy);
}

int MenuFrameCompiler::hit_widget(const MenuFrameState &state, float mx,
		float my, float sx, float sy) const {
	if (screen_ == nullptr || nodes_.empty()) {
		return -1;
	}
	int hit = -1;
	int part = 0;
	hit_roots_(state, mx, my, sx, sy, &hit, &part);
	return hit;
}

// The pump's claim at a point with no state written [orig: CWnd_ProcessMouseEvent
// @ 0x647a00 — the claim walk, and the claimant's own-or-root cursor stamped
// @ 0x647b09]: no scrollbar part takes the sample (nothing is pressed or
// captured where nothing pumps).
MenuFrameCompiler::MouseClaim MenuFrameCompiler::claim_at(const MenuFrameState &state,
		float mouse_x, float mouse_y, float scale_x, float scale_y) const {
	MouseClaim claim;
	if (screen_ == nullptr || nodes_.empty()) {
		return claim;
	}
	int hit = -1;
	int part = 0;
	hit_roots_(state, mouse_x, mouse_y, scale_x, scale_y, &hit, &part);
	claim.hovered = hit;
	claim.spin_part = part;
	claim.cursor = claim_cursor_(hit, part);
	return claim;
}

// Every root in draw order: a later root's hit replaces an earlier one's, so
// the last root is front-most [orig: CUIScene_EndFrame @ 0x63e600 pumps the
// roots in reverse].
// With a popup open only the popup is pumped, from its own shown gate down (its
// ancestors are not consulted) [orig: CUIScene_EndFrame @ 0x63e600 calls the
// popup's CWnd_ProcessMouseEvent alone while g_UIOpenPopupWnd is set].
void MenuFrameCompiler::hit_roots_(const MenuFrameState &state, float mx,
		float my, float sx, float sy, int *io_hit, int *io_part) const {
	const int popup = state.popup_root;
	if (popup >= 0 && popup < document_nodes_) {
		int origin_x = 0;
		int origin_y = 0;
		const int parent = nodes_[static_cast<size_t>(popup)].parent;
		mnu::RectEdges parent_rect;
		if (parent >= 0 && widget_rect(parent, state, &parent_rect)) {
			origin_x = parent_rect.left;
			origin_y = parent_rect.top;
		}
		hit_walk(popup, origin_x, origin_y, state, mx, my, sx, sy, io_hit, io_part);
		return;
	}
	for (int next = 0; next < document_nodes_;) {
		next = hit_walk(next, 0, 0, state, mx, my, sx, sy, io_hit, io_part);
	}
}

bool MenuFrameCompiler::in_subtree_(int index, int root) const {
	if (root < 0) {
		return true;
	}
	if (root >= document_nodes_ || index < root) {
		return false;
	}
	return index < skip_widget(root);
}

// The classes whose parse reads a STRING (the STATIC parse and every class built
// on it) register its {hot} mnemonic [orig: CUIButtonWidget_ParseXMLAttributes
// @ 0x657c30, the STATIC vtable's parse slot, reached by BUTTON, EDIT,
// MULTILINE_EDIT, RADIO, CHECKBOX, SPINLIST, LIST, LAN_LIST, TABLE, COMBOBOX and
// both RADIOEDIT parts; grill set A3's S-chain].
std::string MenuFrameCompiler::widget_mnemonic(int index) const {
	if (index < 0 || index >= document_nodes_) {
		return std::string();
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	switch (node.window->type) {
		case mnu::WindowType::Static:
		case mnu::WindowType::Button:
		case mnu::WindowType::Edit:
		case mnu::WindowType::MultilineEdit:
		case mnu::WindowType::Radio:
		case mnu::WindowType::CheckBox:
		case mnu::WindowType::SpinList:
		case mnu::WindowType::List:
		case mnu::WindowType::LanList:
		case mnu::WindowType::Table:
		case mnu::WindowType::Combo:
		case mnu::WindowType::RadioEdit:
			break;
		default:
			return std::string();
	}
	// The parse-time label: the authored STRING resolved, never a runtime text.
	return resolved_widget_text(node, nullptr).hotkey;
}

} // namespace opennova::menu
