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

bool MenuFrameCompiler::arrow_disabled_(int arrow, const MenuFrameState &state) const {
	return disabled_(*nodes_[static_cast<size_t>(arrow)].window, state_for(state, arrow));
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
	const auto arrow_hit = [&](int arrow) {
		if (arrow < 0) {
			return false;
		}
		const WidgetNode &part = nodes_[static_cast<size_t>(arrow)];
		if (!node_shown(*part.window, state_for(state, arrow))) {
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
	HitClaim hit;
	hit_roots_(state, mx, my, sx, sy, false, &hit);
	return hit.index;
}

// The pump's claim at a point with no state written [orig: CWnd_ProcessMouseEvent
// @ 0x647a00 — the claim walk, and the claimant's own-or-root cursor stamped
// @ 0x647b09]: nothing scrolls or is pressed where nothing pumps; a press's
// capture, where the embedder keeps one (MenuClickLatch), holds the claim to
// the captured window.
MenuFrameCompiler::MouseClaim MenuFrameCompiler::claim_at(const MenuFrameState &state,
		float mouse_x, float mouse_y, float scale_x, float scale_y,
		const MenuPumpWindow &capture) const {
	MouseClaim claim;
	if (screen_ == nullptr || nodes_.empty()) {
		return claim;
	}
	HitClaim hit;
	if (capture.valid()) {
		capture_hit_(state, capture, mouse_x, mouse_y, scale_x, scale_y, &hit);
	} else {
		hit_roots_(state, mouse_x, mouse_y, scale_x, scale_y, true, &hit);
	}
	fill_claim_(hit, &claim);
	claim.cursor = claim_cursor_(hit.index, hit.part, capture.index, capture.part);
	return claim;
}

// While a press holds the capture no window but the captured one takes the
// claim, and it takes it where its own rect holds the point, its children not
// consulted [orig: CWnd_ProcessMouseEvent @ 0x647a88..0x647b02 —
// g_UIMouseCaptureWnd set and not this window: no claim; this window: the child
// walk skipped @ 0x647afe; CWnd_HitTestPoint @ 0x646700 with no recursion
// @ 0x647a66], past the gate every claim passes first: visible in the
// hierarchy (@ 0x647a27). A window the pump does not reach (hidden, or outside
// the open popup) takes nothing, and neither does an open dropdown's list once
// it picked (hidden). A spin arrow and a scrollbar's window are windows of
// their own: their own rects.
void MenuFrameCompiler::capture_hit_(const MenuFrameState &state,
		const MenuPumpWindow &capture, float mx, float my, float sx, float sy,
		HitClaim *io_hit) const {
	if (capture.index < 0 || capture.index >= document_nodes_ || capture.part < 0 ||
			capture.part == kMenuPumpPartDropdown || capture.part > kMenuPumpPartScrollShuttle ||
			!widget_live(capture.index, state)) {
		return;
	}
	mnu::RectEdges rect;
	if (!widget_rect(capture.index, state, &rect)) {
		return;
	}
	const auto holds = [&](const mnu::RectEdges &r) {
		return mx >= emit_x(r.left, sx) && mx < emit_x(r.right, sx) &&
				my >= emit_x(r.top, sy) && my < emit_x(r.bottom, sy);
	};
	const WidgetNode &node = nodes_[static_cast<size_t>(capture.index)];
	if (capture.part == 1 || capture.part == 2) {
		const int arrow = capture.part == 1 ? node.spin_up : node.spin_down;
		if (node.window->type == mnu::WindowType::SpinList && arrow >= 0 &&
				!arrow_disabled_(arrow, state) &&
				spin_arrow_hit_(node, rect, state, mx, my, sx, sy) == capture.part) {
			*io_hit = HitClaim{ capture.index, capture.part };
		}
		return;
	}
	if (menu_pump_part_scroll(capture.part)) {
		ScrollParts parts;
		if (!scroll_windows_(capture.index, state, &parts)) {
			return;
		}
		const mnu::RectEdges &own = capture.part == kMenuPumpPartScrollUp ? parts.up
				: capture.part == kMenuPumpPartScrollDown                 ? parts.down
				: capture.part == kMenuPumpPartScrollShuttle              ? parts.shuttle
																		  : parts.window;
		if (holds(own)) {
			*io_hit = HitClaim{ capture.index, capture.part };
		}
		return;
	}
	if (holds(rect)) {
		*io_hit = HitClaim{ capture.index, 0 };
	}
}

MenuClickLatch::Claim MenuFrameCompiler::click_claim(const MouseClaim &claim,
		const MenuFrameState &state) const {
	MenuClickLatch::Claim out;
	// A scrollbar's window: its owner's, live while the owner is.
	if (claim.scroll_owner >= 0 && claim.scroll_owner < document_nodes_) {
		out.window = MenuPumpWindow{ claim.scroll_owner, claim.scroll_part };
		out.live = widget_live(claim.scroll_owner, state);
		return out;
	}
	if (claim.hovered < 0 || claim.hovered >= document_nodes_) {
		return out;
	}
	out.window = MenuPumpWindow{ claim.hovered, claim.spin_part };
	out.live = widget_live(claim.hovered, state);
	const WidgetNode &node = nodes_[static_cast<size_t>(claim.hovered)];
	if (claim.spin_part == 1 || claim.spin_part == 2) {
		// The arrow is a CButtonWnd of its own: live while its list is and it is
		// enabled [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0].
		const int arrow = claim.spin_part == 1 ? node.spin_up : node.spin_down;
		out.live = out.live && arrow >= 0 && !arrow_disabled_(arrow, state);
	}
	return out;
}

bool MenuFrameCompiler::pump_window_reached(const MenuPumpWindow &window,
		const MenuFrameState &state) const {
	if (!widget_reached(window.index, state)) {
		return false;
	}
	// A scrollbar's window is pumped while it shows.
	ScrollParts parts;
	return !menu_pump_part_scroll(window.part) || scroll_windows_(window.index, state, &parts);
}

// Every root in draw order: a later root's hit replaces an earlier one's, so
// the last root is front-most [orig: CUIScene_EndFrame @ 0x63e600 pumps the
// roots in reverse].
// With a popup open only the popup is pumped, from its own shown gate down (its
// ancestors are not consulted) [orig: CUIScene_EndFrame @ 0x63e600 calls the
// popup's CWnd_ProcessMouseEvent alone while g_UIOpenPopupWnd is set; the popup
// is visible in the hierarchy whatever its ancestors are, CWnd_IsVisibleInHierarchy
// @ 0x6462b8].
void MenuFrameCompiler::hit_roots_(const MenuFrameState &state, float mx,
		float my, float sx, float sy, bool pump, HitClaim *io_claim) const {
	bool holds = false;
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
		hit_walk(popup, origin_x, origin_y, state, mx, my, sx, sy, pump, true, io_claim, &holds);
		return;
	}
	for (int next = 0; next < document_nodes_;) {
		next = hit_walk(next, 0, 0, state, mx, my, sx, sy, pump, true, io_claim, &holds);
	}
}

// The press message's walk down one window [orig: CWnd_DispatchMouseEventToChildren
// @ 0x647900]: a window hidden or disabled takes nothing and passes nothing on
// (@ 0x647949..0x64795d, its own flags); its children, last first, each keep it
// from its own press where their own rect holds the point (CWnd_HitTestPoint
// unrecursed @ 0x6479b3, shown windows alone: a disabled child's rect too), and
// each takes the press while no window has taken the capture (@ 0x6479be);
// then the window's own handler where its rect held the point and no child's
// did (@ 0x6479e5). A widget's own windows are its last children: a spin
// list's arrows, a scrollbar's windows (its buttons first, then its track).
void MenuFrameCompiler::press_walk_(int index, int origin_x, int origin_y,
		const MenuFrameState &state, float mx, float my, float sx, float sy,
		std::vector<MenuPumpWindow> *out, MenuPumpWindow *capture) const {
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	const MenuWidgetState *ws = state_for(state, index);
	if (!node_shown(w, ws) || disabled_(w, ws)) {
		return;
	}
	const mnu::RectEdges rect = offset_rect(node_rect_(node, ws), origin_x, origin_y);
	auto holds = [&](const mnu::RectEdges &r) {
		return mx >= emit_x(r.left, sx) && mx < emit_x(r.right, sx) &&
				my >= emit_x(r.top, sy) && my < emit_x(r.bottom, sy);
	};
	// A window the press reaches: its own handler; a capturing one takes the
	// capture [orig: CButtonWnd_HandleNamedEvent @ 0x65839c].
	auto handle = [&](int part) {
		const MenuPumpWindow window{ index, part };
		out->push_back(window);
		if (menu_pump_window_captures(w.type, part)) {
			*capture = window;
		}
	};
	bool self_hit = holds(rect);
	// The widget's own windows, the last made first.
	ScrollParts parts;
	if (w.type == mnu::WindowType::SpinList) {
		for (const int arrow : { 2, 1 }) {
			const int arrow_node = arrow == 1 ? node.spin_up : node.spin_down;
			if (arrow_node < 0 || spin_arrow_hit_(node, rect, state, mx, my, sx, sy) != arrow) {
				continue;
			}
			self_hit = false;
			if (!capture->valid() && !arrow_disabled_(arrow_node, state)) {
				handle(arrow);
			}
		}
	} else if (scroll_windows_(index, state, &parts)) {
		const int part = scroll_window_part_(parts, mx, my, sx, sy);
		if (w.type == mnu::WindowType::Scroll) {
			// The SCROLL widget's buttons; its track is its own press.
			if (part != 0 && part != kMenuPumpPartScroll) {
				self_hit = false;
				if (!capture->valid()) {
					handle(part);
				}
			}
		} else if (part != 0) {
			// The owner's scroll window: its buttons, else its own press (the
			// track) [orig: CListWnd_CreateScrollChild @ 0x6444c0].
			self_hit = false;
			if (!capture->valid()) {
				handle(part);
			}
		}
	}
	// The authored children, last first.
	std::vector<int> children;
	int next = index + 1;
	for (size_t c = 0; c < w.children.size(); ++c) {
		children.push_back(next);
		next = skip_widget(next);
	}
	for (auto it = children.rbegin(); it != children.rend(); ++it) {
		const WidgetNode &child = nodes_[static_cast<size_t>(*it)];
		const MenuWidgetState *cws = state_for(state, *it);
		if (node_shown(*child.window, cws) &&
				holds(offset_rect(node_rect_(child, cws), rect.left, rect.top))) {
			self_hit = false;
		}
		if (!capture->valid()) {
			press_walk_(*it, rect.left, rect.top, state, mx, my, sx, sy, out, capture);
		}
	}
	if (self_hit) {
		handle(0);
	}
}

// The press [orig: UI_DispatchMouseEvent @ 0x63ab00 — the open popup alone
// (@ 0x63abb5), else every root, last first (@ 0x63abd3)]. Once a window has
// taken the capture, a root's walk hands the press to the captured window
// instead (CWnd_DispatchMouseEventToChildren @ 0x647926..0x64793f), so each
// root behind the one holding it presses it again.
std::vector<MenuPumpWindow> MenuFrameCompiler::press_reach(const MenuFrameState &state,
		float mouse_x, float mouse_y, float scale_x, float scale_y) const {
	std::vector<MenuPumpWindow> out;
	if (screen_ == nullptr || nodes_.empty()) {
		return out;
	}
	MenuPumpWindow capture;
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
		press_walk_(popup, origin_x, origin_y, state, mouse_x, mouse_y, scale_x, scale_y, &out,
				&capture);
		return out;
	}
	std::vector<int> roots;
	for (int next = 0; next < document_nodes_; next = skip_widget(next)) {
		roots.push_back(next);
	}
	for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
		if (capture.valid()) {
			out.push_back(capture);
			continue;
		}
		press_walk_(*it, 0, 0, state, mouse_x, mouse_y, scale_x, scale_y, &out, &capture);
	}
	return out;
}

MenuPumpWindow MenuFrameCompiler::press_capture(const std::vector<MenuPumpWindow> &reach) const {
	MenuPumpWindow capture;
	for (const MenuPumpWindow &window : reach) {
		if (window.index >= 0 && window.index < document_nodes_ &&
				menu_pump_window_captures(nodes_[static_cast<size_t>(window.index)].window->type,
						window.part)) {
			capture = window;
		}
	}
	return capture;
}

std::vector<MenuPumpWindow> MenuFrameCompiler::press_mouse(MenuClickLatch &click,
		const MenuFrameState &state, float mouse_x, float mouse_y, float scale_x,
		float scale_y) const {
	std::vector<MenuPumpWindow> reach = press_reach(state, mouse_x, mouse_y, scale_x, scale_y);
	click.press(press_capture(reach));
	return reach;
}

MenuFrameCompiler::MouseSample MenuFrameCompiler::sample_mouse(MenuClickLatch &click,
		MenuFrameState &io_state, float mouse_x, float mouse_y, bool button_down, float scale_x,
		float scale_y) {
	MouseSample out;
	const MenuPumpWindow capture = click.capture_for(button_down);
	out.claim = pump_mouse(io_state, mouse_x, mouse_y, button_down, scale_x, scale_y, capture);
	io_state.cursor_x = mouse_x;
	io_state.cursor_y = mouse_y;
	out.clicked = click.sample(click_claim(out.claim, io_state), button_down,
			[this, &io_state](const MenuPumpWindow &window) {
				return pump_window_reached(window, io_state);
			});
	return out;
}

MenuFrameCompiler::MouseSample MenuFrameCompiler::peek_mouse(MenuClickLatch &click,
		const MenuFrameState &state, bool over, float mouse_x, float mouse_y, bool button_down,
		float scale_x, float scale_y) const {
	MouseSample out;
	const MenuPumpWindow capture = click.capture_for(button_down);
	if (over) {
		out.claim = claim_at(state, mouse_x, mouse_y, scale_x, scale_y, capture);
	}
	out.clicked = click.sample(click_claim(out.claim, state), button_down,
			[this, &state](const MenuPumpWindow &window) { return pump_window_reached(window, state); });
	return out;
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
