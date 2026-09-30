// The compiled-menu interaction runtime — see menu_runtime.h for the witness
// map. [orig: CUIWidget_HandleScriptedAction @0x6497f0; UI_DispatchMouseEvent
// @0x63ab00]

#include <runtime/menu/menu_runtime.h>

#include <base/io/strutil.h>
#include <runtime/menu/menu_edit.h>
#include <runtime/menu/options_policy.h>

#include <algorithm>

namespace opennova::menu {

namespace {

constexpr int kKindStatic = static_cast<int>(mnu::WindowType::Static);
constexpr int kKindButton = static_cast<int>(mnu::WindowType::Button);
constexpr int kKindEdit = static_cast<int>(mnu::WindowType::Edit);
constexpr int kKindMultilineEdit = static_cast<int>(mnu::WindowType::MultilineEdit);
constexpr int kKindList = static_cast<int>(mnu::WindowType::List);
constexpr int kKindCheckBox = static_cast<int>(mnu::WindowType::CheckBox);
constexpr int kKindRadio = static_cast<int>(mnu::WindowType::Radio);
constexpr int kKindCombo = static_cast<int>(mnu::WindowType::Combo);
constexpr int kKindScroll = static_cast<int>(mnu::WindowType::Scroll);
constexpr int kKindTable = static_cast<int>(mnu::WindowType::Table);
constexpr int kKindSpinList = static_cast<int>(mnu::WindowType::SpinList);
constexpr int kKindMulti = static_cast<int>(mnu::WindowType::Multi);
constexpr int kKindLabel = static_cast<int>(mnu::WindowType::Label);
constexpr int kKindGoto = static_cast<int>(mnu::WindowType::Goto);
constexpr int kKindLanList = static_cast<int>(mnu::WindowType::LanList);

// Code points in a UTF-8 string (the caret rides characters, not bytes).
int utf8_length(const std::string &s) {
	int n = 0;
	for (unsigned char c : s)
		if ((c & 0xC0) != 0x80) ++n;
	return n;
}

std::string utf8_of(int code_point) {
	std::string out;
	const uint32_t c = static_cast<uint32_t>(code_point);
	if (c < 0x80) {
		out.push_back(static_cast<char>(c));
	} else if (c < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (c >> 6)));
		out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	} else if (c < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (c >> 12)));
		out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (c >> 18)));
		out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	}
	return out;
}

// Toggle `row` in a selection set (remove when present, append otherwise).
void toggle_row(std::vector<int> &selected, int row) {
	const auto it = std::find(selected.begin(), selected.end(), row);
	if (it != selected.end())
		selected.erase(it);
	else
		selected.push_back(row);
}

} // namespace

const mnu::Items *menu_items_container(const mnu::Window *window) {
	if (window == nullptr) return nullptr;
	switch (window->type) {
		case mnu::WindowType::List:
		case mnu::WindowType::Table:
		case mnu::WindowType::GlbTable:
		case mnu::WindowType::Multi:
		case mnu::WindowType::LanList:
		case mnu::WindowType::SpinList:
			return &window->items;
		case mnu::WindowType::Combo:
			// LIST_BOX owns combo items when it actually authors an ITEMS block
			// under an authored LIST_BOX. A latent/disabled LIST_BOX or a
			// styling-only one falls back to top-level ITEMS, matching runtime.
			return window->list_box.present && window->list_box.items.present
					? &window->list_box.items
					: &window->items;
		default:
			return nullptr;
	}
}

// ---- MenuDocIndex -----------------------------------------------------------

void MenuDocIndex::clear() {
	doc_ = nullptr;
	nodes_.clear();
	screen_ids_.clear();
}

int MenuDocIndex::add_window_(const mnu::Window &w, int parent_id, int screen_index) {
	const int id = static_cast<int>(nodes_.size()) + 1;
	Node node;
	node.id = id;
	node.parent_id = parent_id;
	node.screen_index = screen_index;
	node.window = &w;
	nodes_.push_back(std::move(node));
	std::vector<int> children;
	children.reserve(w.children.size());
	for (const mnu::Window &child : w.children)
		children.push_back(add_window_(child, id, screen_index));
	nodes_[static_cast<size_t>(id - 1)].child_ids = std::move(children);
	return id;
}

void MenuDocIndex::build(const mnu::Document &doc) {
	clear();
	doc_ = &doc;
	for (size_t i = 0; i < doc.screens.size(); ++i) {
		const int screen_id = static_cast<int>(nodes_.size()) + 1;
		Node node;
		node.id = screen_id;
		node.screen_index = static_cast<int>(i);
		nodes_.push_back(std::move(node));
		screen_ids_.push_back(screen_id);
		const int root_id = add_window_(doc.screens[i].root_window, screen_id, static_cast<int>(i));
		nodes_[static_cast<size_t>(screen_id - 1)].child_ids = { root_id };
	}
}

const MenuDocIndex::Node *MenuDocIndex::node(int id) const {
	if (id < 1 || id > static_cast<int>(nodes_.size())) return nullptr;
	return &nodes_[static_cast<size_t>(id - 1)];
}

const mnu::Window *MenuDocIndex::window(int id) const {
	const Node *n = node(id);
	return n != nullptr ? n->window : nullptr;
}

const mnu::Screen *MenuDocIndex::screen(int screen_id) const {
	const Node *n = node(screen_id);
	if (n == nullptr || n->window != nullptr || doc_ == nullptr) return nullptr;
	return &doc_->screens[static_cast<size_t>(n->screen_index)];
}

int MenuDocIndex::screen_root_id(int screen_id) const {
	const Node *n = node(screen_id);
	if (n == nullptr || n->window != nullptr || n->child_ids.empty()) return -1;
	return n->child_ids.front();
}

// ---- document / screen lifecycle --------------------------------------------

void MenuRuntime::emit_(const MenuEvent &event) const {
	if (sink_) sink_(event);
}

bool MenuRuntime::open_document(const mnu::Document *doc, const std::string &menu_file,
		const std::string &target_screen) {
	++open_generation_;
	index_.clear();
	menu_file_ = menu_file;
	id_state_.clear();
	name_to_id_.clear();
	id_info_.clear();
	screen_ids_.clear();
	screen_order_.clear();
	nav_stack_.clear();
	// A new document: no focus, no open popup, no claim.
	focus_id_ = -1;
	open_combo_id_ = -1;
	last_claim_ = -1;
	if (doc == nullptr) return false;
	index_.build(*doc);
	index_document_();
	if (screen_order_.empty()) return false;
	std::string initial = target_screen;
	if (initial.empty() || screen_ids_.find(strutil::to_upper(initial)) == screen_ids_.end())
		initial = screen_order_.front();
	return show_screen(initial);
}

void MenuRuntime::index_document_() {
	for (int screen_id : index_.screen_ids()) {
		const mnu::Screen *screen = index_.screen(screen_id);
		if (screen == nullptr) continue;
		screen_ids_[strutil::to_upper(screen->name)] = screen_id;
		screen_order_.push_back(screen->name);
		index_widget_subtree_(screen->name, index_.screen_root_id(screen_id));
	}
}

void MenuRuntime::index_widget_subtree_(const std::string &screen_name, int id) {
	const MenuDocIndex::Node *node = index_.node(id);
	if (node == nullptr || node->window == nullptr) return;
	WidgetInfo info;
	info.screen = screen_name;
	info.name = node->window->name;
	info.kind = static_cast<int>(node->window->type);
	if (!info.name.empty()) {
		// First match in document order wins (the find_child equivalent).
		name_to_id_.emplace(strutil::to_upper(info.name), id);
	}
	id_info_[id] = std::move(info);
	for (int child : node->child_ids) index_widget_subtree_(screen_name, child);
}

int MenuRuntime::current_screen_id() const {
	const auto it = screen_ids_.find(strutil::to_upper(current_screen_));
	return it != screen_ids_.end() ? it->second : -1;
}

bool MenuRuntime::show_screen(const std::string &name) {
	if (index_.document() == nullptr) return false;
	const auto found = screen_ids_.find(strutil::to_upper(name));
	if (found == screen_ids_.end()) return false;
	close_active_combo_popup();
	// Screen names match case-insensitively; the AUTHORED spelling is the
	// current screen, so the per-widget screen tests below compare equal
	// whatever casing the caller used.
	const mnu::Screen *screen = index_.screen(found->second);
	current_screen_ = screen != nullptr ? screen->name : name;
	focus_id_ = -1; // a screen change drops the edit focus
	configure_frame_();
	on_screen_shown_();
	return true;
}

bool MenuRuntime::navigate_to_screen(const std::string &name) {
	const std::string previous = current_screen_;
	if (!show_screen(name)) return false;
	if (!previous.empty() && previous != current_screen_) nav_stack_.push_back(previous);
	return true;
}

bool MenuRuntime::pop_screen() {
	if (!nav_stack_.empty()) {
		const std::string prev = nav_stack_.back();
		nav_stack_.pop_back();
		return show_screen(prev);
	}
	MenuEvent e;
	e.kind = MenuEvent::Kind::QuitRequested;
	emit_(e);
	return false;
}

void MenuRuntime::on_screen_shown_() {
	MenuEvent changed;
	changed.kind = MenuEvent::Kind::ScreenChanged;
	changed.text = current_screen_;
	emit_(changed);
	// The MUSICVAR push fires on every screen event, repeats and zeros
	// included [orig: UI_DispatchScreenEvent @0x54e6a0 ->
	// AudioVM_SetVariable(index, value) @0x54eff4].
	MenuEvent music;
	music.kind = MenuEvent::Kind::MusicVar;
	const mnu::Screen *screen = index_.screen(current_screen_id());
	music.value = screen != nullptr && screen->has_music_var ? screen->music_var : 0;
	emit_(music);
}

void MenuRuntime::configure_frame_() {
	if (index_.document() == nullptr) return;
	// The index maps drive the state store even frameless (headless seam tests
	// run companions without a render surface).
	rebuild_index_maps_();
	if (frame_ == nullptr) return;
	frame_->configure_screen(current_screen_);
	replay_state_();
	frame_->screen_configured();
}

// The current screen's id<->pre-order-index maps: the frame's index space is
// the same pre-order DFS as the document walk (root first, children in
// authored order) — the documented seam contract on the frame compiler.
void MenuRuntime::rebuild_index_maps_() {
	id_of_index_.clear();
	index_of_id_.clear();
	const int screen_id = current_screen_id();
	if (screen_id < 0) return;
	map_widget_subtree_(index_.screen_root_id(screen_id));
}

void MenuRuntime::map_widget_subtree_(int id) {
	const MenuDocIndex::Node *node = index_.node(id);
	if (node == nullptr) return;
	index_of_id_[id] = static_cast<int>(id_of_index_.size());
	id_of_index_.push_back(id);
	for (int child : node->child_ids) map_widget_subtree_(child);
}

// Replay the document-id state onto the fresh compile's index map.
void MenuRuntime::replay_state_() {
	for (size_t i = 0; i < id_of_index_.size(); ++i) {
		const MenuWidgetRuntimeState *state = saved_state_(id_of_index_[i]);
		if (state == nullptr || state->empty()) continue;
		const int index = static_cast<int>(i);
		if (state->has_shown) frame_->set_widget_shown_override(index, state->shown);
		if (state->has_disabled) frame_->set_widget_disabled(index, state->disabled);
		if (state->has_checked) frame_->set_widget_checked(index, state->checked);
		if (state->has_text) frame_->set_widget_text(index, state->text);
		if (state->has_items) frame_->set_widget_items(index, state->items);
		if (state->has_selected_item || state->has_scroll_row)
			frame_->set_widget_selection(index, state->selected_item, -1, state->scroll_row);
		if (state->has_scroll_range)
			frame_->set_widget_scroll_range(index, state->scroll_range.minimum,
					state->scroll_range.maximum, state->scroll_range.page,
					state->scroll_range.value);
		if (state->has_selected_set) frame_->set_widget_selected_set(index, state->selected_set);
		if (state->has_table_rows) frame_->set_widget_table_rows(index, state->table_rows);
		if (state->has_rect)
			frame_->set_widget_rect(index, state->rect_left, state->rect_top, state->rect_right,
					state->rect_bottom);
	}
}

// ---- addressing -------------------------------------------------------------

int MenuRuntime::widget_id(const std::string &name) const {
	const auto it = name_to_id_.find(strutil::to_upper(name));
	return it != name_to_id_.end() ? it->second : -1;
}

std::string MenuRuntime::widget_name_of(int id) const {
	const auto it = id_info_.find(id);
	return it != id_info_.end() ? it->second.name : std::string();
}

int MenuRuntime::widget_kind_of(int id) const {
	const auto it = id_info_.find(id);
	return it != id_info_.end() ? it->second.kind : -1;
}

std::string MenuRuntime::widget_screen_of(int id) const {
	const auto it = id_info_.find(id);
	return it != id_info_.end() ? it->second.screen : std::string();
}

int MenuRuntime::frame_index(int id) const {
	// Frameless (headless seam tests): every frame-dependent path takes its
	// state-store fallback.
	if (frame_ == nullptr) return -1;
	const auto it = index_of_id_.find(id);
	return it != index_of_id_.end() ? it->second : -1;
}

int MenuRuntime::id_at_index(int index) const {
	if (index < 0 || index >= static_cast<int>(id_of_index_.size())) return -1;
	return id_of_index_[static_cast<size_t>(index)];
}

MenuWidgetRuntimeState &MenuRuntime::state_of_(int id) {
	// An absent widget (-1, the retail null CUIWidget_FindByName) has no state:
	// its writes land in a throwaway so the store never grows a -1 row.
	if (id < 0) {
		scratch_state_ = MenuWidgetRuntimeState();
		return scratch_state_;
	}
	return id_state_[id];
}

const MenuWidgetRuntimeState *MenuRuntime::saved_state_(int id) const {
	const auto it = id_state_.find(id);
	return it != id_state_.end() ? &it->second : nullptr;
}

// ---- state ------------------------------------------------------------------

void MenuRuntime::set_widget_shown(int id, bool shown) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.shown = shown;
	state.has_shown = true;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_shown_override(index, shown);
	MenuEvent e;
	e.kind = MenuEvent::Kind::ShownChanged;
	e.id = id;
	e.flag = shown;
	emit_(e);
}

bool MenuRuntime::is_widget_shown(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_shown) return state->shown;
	const mnu::Window *w = index_.window(id);
	return !(w != nullptr && w->hidden);
}

void MenuRuntime::set_widget_disabled(int id, bool disabled) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.disabled = disabled;
	state.has_disabled = true;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_disabled(index, disabled);
}

bool MenuRuntime::is_widget_disabled(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_disabled) return state->disabled;
	const mnu::Window *w = index_.window(id);
	return w != nullptr && w->disabled;
}

void MenuRuntime::set_widget_checked(int id, bool checked) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.checked = checked;
	state.has_checked = true;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_checked(index, checked);
}

bool MenuRuntime::is_widget_checked(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_checked) return state->checked;
	const mnu::Window *w = index_.window(id);
	return w != nullptr && w->checked;
}

void MenuRuntime::set_widget_text(int id, const std::string &text) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.text = text;
	state.has_text = true;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_text(index, text);
}

void MenuRuntime::set_widget_rect(int id, int left, int top, int right, int bottom) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.has_rect = true;
	state.rect_left = left;
	state.rect_top = top;
	state.rect_right = right;
	state.rect_bottom = bottom;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_rect(index, left, top, right, bottom);
}

std::string MenuRuntime::get_widget_text(int id) const {
	const int index = frame_index(id);
	if (index >= 0) return frame_->get_widget_text(index);
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_text) return state->text;
	const mnu::Window *w = index_.window(id);
	return w != nullptr ? w->string_data.value : std::string();
}

void MenuRuntime::remember_widget_text(int id, const std::string &text) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.text = text;
	state.has_text = true;
}

void MenuRuntime::set_widget_items(int id, const std::vector<std::string> &items) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.items = items;
	state.has_items = true;
	// Fresh rows reset the selection unless the caller re-selects.
	state.selected_item = items.empty() ? -1 : 0;
	state.has_selected_item = true;
	state.scroll_row = 0;
	state.has_scroll_row = true;
	const int index = frame_index(id);
	if (index >= 0) {
		frame_->set_widget_items(index, items);
		frame_->set_widget_selection(index, state.selected_item, -1, 0);
	}
}

std::vector<std::string> MenuRuntime::get_widget_items(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_items) return state->items;
	std::vector<std::string> out;
	if (const mnu::Items *items = menu_items_container(index_.window(id)))
		for (const mnu::Item &item : items->items) out.push_back(item.text);
	return out;
}

int MenuRuntime::item_count(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_items) return static_cast<int>(state->items.size());
	const int index = frame_index(id);
	if (index >= 0) return frame_->item_count(index);
	const mnu::Items *items = menu_items_container(index_.window(id));
	return items != nullptr ? static_cast<int>(items->items.size()) : 0;
}

std::string MenuRuntime::item_text(int id, int row) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_items) {
		return row >= 0 && row < static_cast<int>(state->items.size())
				? state->items[static_cast<size_t>(row)]
				: std::string();
	}
	const mnu::Items *items = menu_items_container(index_.window(id));
	if (items == nullptr || row < 0 || row >= static_cast<int>(items->items.size()))
		return std::string();
	return items->items[static_cast<size_t>(row)].text;
}

std::string MenuRuntime::item_value(int id, int row) const {
	const mnu::Items *items = menu_items_container(index_.window(id));
	if (items == nullptr || row < 0 || row >= static_cast<int>(items->items.size()))
		return std::string();
	return items->items[static_cast<size_t>(row)].value;
}

void MenuRuntime::select_row_by_value(int id, const std::string &value, bool emit) {
	const mnu::Items *items = menu_items_container(index_.window(id));
	if (items == nullptr) return;
	select_row(id, spinlist_row_for_value(*items, value), emit);
}

void MenuRuntime::select_row(int id, int row, bool emit) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.selected_item = row;
	state.has_selected_item = true;
	const int scroll_row = state.scroll_row;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_selection(index, row, -1, scroll_row);
	if (emit) emit_value_changed_for_(id, row);
}

int MenuRuntime::selected_row(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_selected_item) return state->selected_item;
	return item_count(id) > 0 ? 0 : -1;
}

std::vector<int> MenuRuntime::selected_rows(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_selected_set) return state->selected_set;
	std::vector<int> out;
	const int row = selected_row(id);
	if (row >= 0) out.push_back(row);
	return out;
}

std::vector<int> MenuRuntime::selected_set(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	return state != nullptr ? state->selected_set : std::vector<int>();
}

void MenuRuntime::set_selected_set(int id, const std::vector<int> &rows) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.selected_set = rows;
	state.has_selected_set = true;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_selected_set(index, rows);
}

void MenuRuntime::set_scroll_row(int id, int row) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.scroll_row = std::max(row, 0);
	state.has_scroll_row = true;
	const int index = frame_index(id);
	if (index >= 0)
		frame_->set_widget_selection(index, state.has_selected_item ? state.selected_item : -1,
				-1, state.scroll_row);
}

void MenuRuntime::on_frame_scroll_value(int index, int value) {
	const int id = id_at_index(index);
	if (id < 0) return;
	if (widget_kind_of(id) != kKindScroll) {
		set_scroll_row(id, value);
		return;
	}
	const auto it = id_state_.find(id);
	if (it == id_state_.end() || !it->second.has_scroll_range) return;
	MenuScrollRangeState &scroll = it->second.scroll_range;
	if (value == scroll.value) return;
	scroll.value = std::clamp(value, scroll.minimum, scroll.maximum);
	MenuEvent e;
	e.kind = MenuEvent::Kind::ValueChanged;
	e.text = widget_name_of(id);
	e.text2 = "scroll";
	e.value = scroll.value;
	e.text3 = std::to_string(scroll.value);
	emit_(e);
}

void MenuRuntime::set_widget_scroll_range(int id, int minimum, int maximum, int page, int value) {
	// The original range normalization: invalid min/max -> zero, value clamped.
	if (minimum > maximum) {
		minimum = 0;
		maximum = 0;
	}
	value = std::clamp(value, minimum, maximum);
	MenuWidgetRuntimeState &state = state_of_(id);
	state.scroll_range = MenuScrollRangeState{ minimum, maximum, page, value };
	state.has_scroll_range = true;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_scroll_range(index, minimum, maximum, page, value);
}

bool MenuRuntime::get_widget_scroll_range(int id, MenuScrollRangeState &out) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state == nullptr || !state->has_scroll_range) return false;
	out = state->scroll_range;
	return true;
}

// ---- tables -----------------------------------------------------------------

void MenuRuntime::table_add_row(int id, const std::vector<std::string> &cells) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.table_rows.push_back(cells);
	state.has_table_rows = true;
	push_table_rows_(id);
}

void MenuRuntime::table_remove_row(int id, int row) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (row < 0 || row >= static_cast<int>(state.table_rows.size())) return;
	state.table_rows.erase(state.table_rows.begin() + row);
	std::vector<int> reindexed;
	for (int r : state.table_selected) {
		if (r < row)
			reindexed.push_back(r);
		else if (r > row)
			reindexed.push_back(r - 1);
	}
	state.table_selected = std::move(reindexed);
	push_table_rows_(id);
}

void MenuRuntime::table_clear_rows(int id) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.table_rows.clear();
	state.has_table_rows = true;
	state.table_selected.clear();
	push_table_rows_(id);
}

int MenuRuntime::table_row_count(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	return state != nullptr ? static_cast<int>(state->table_rows.size()) : 0;
}

std::string MenuRuntime::table_cell_text(int id, int row, int col) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state == nullptr || row < 0 || row >= static_cast<int>(state->table_rows.size()))
		return std::string();
	const std::vector<std::string> &cells = state->table_rows[static_cast<size_t>(row)];
	return col >= 0 && col < static_cast<int>(cells.size()) ? cells[static_cast<size_t>(col)]
															  : std::string();
}

std::vector<int> MenuRuntime::table_selected_rows(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	return state != nullptr ? state->table_selected : std::vector<int>();
}

void MenuRuntime::table_select_row(int id, int row, bool additive) {
	MenuWidgetRuntimeState &state = state_of_(id);
	std::vector<int> selected = additive ? state.table_selected : std::vector<int>();
	toggle_row(selected, row);
	state.table_selected = std::move(selected);
	push_table_selection_(id);
}

void MenuRuntime::push_table_rows_(int id) {
	const int index = frame_index(id);
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (index < 0 || state == nullptr) return;
	frame_->set_widget_table_rows(index, state->table_rows);
	push_table_selection_(id);
}

void MenuRuntime::push_table_selection_(int id) {
	const int index = frame_index(id);
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (index < 0 || state == nullptr) return;
	frame_->set_widget_selected_set(index, state->table_selected);
	const int first = state->table_selected.empty() ? -1 : state->table_selected.front();
	frame_->set_widget_selection(index, first, -1, state->scroll_row);
}

// ---- activation / actions ---------------------------------------------------

void MenuRuntime::activate(int id) {
	const uint32_t generation_at_emit = open_generation_;
	MenuEvent e;
	e.kind = MenuEvent::Kind::WidgetActivated;
	e.id = id;
	e.text = widget_name_of(id);
	emit_(e);
	if (open_generation_ != generation_at_emit) return; // an observer swapped the document
	const mnu::Window *w = index_.window(id);
	if (w == nullptr) return;
	// A cross-.mnu jump frees the document mid-walk: dispatch a copy.
	const std::vector<mnu::Action> actions = w->actions;
	for (const mnu::Action &action : actions) dispatch_action(action);
}

void MenuRuntime::select_radio(int id) {
	set_widget_checked(id, true);
	const mnu::Window *w = index_.window(id);
	const int group = w != nullptr ? w->group : 0;
	// Group exclusivity within the widget's screen (the authored GROUP id).
	const std::string screen = widget_screen_of(id);
	for (int other = 1; other <= index_.node_count(); ++other) {
		if (other == id) continue;
		const auto it = id_info_.find(other);
		if (it == id_info_.end() || it->second.screen != screen || it->second.kind != kKindRadio)
			continue;
		const mnu::Window *ow = index_.window(other);
		if ((ow != nullptr ? ow->group : 0) != group) continue;
		set_widget_checked(other, false);
	}
}

void MenuRuntime::emit_edit_changed(int id) {
	MenuEvent e;
	e.kind = MenuEvent::Kind::ValueChanged;
	e.text = widget_name_of(id);
	e.text2 = widget_kind_of(id) == kKindMultilineEdit ? "multiline" : "edit";
	e.value = -1;
	e.text3 = get_widget_text(id);
	emit_(e);
}

void MenuRuntime::spin_cycle(int id, int delta) {
	const int count = item_count(id);
	if (count <= 0) return;
	const int row = ((selected_row(id) + delta) % count + count) % count;
	select_row(id, row, true);
	play_widget_state_sound(id, "SELECTED");
}

bool MenuRuntime::dispatch_action(const mnu::Action &action) {
	const std::string type = strutil::to_lower(action.type);
	if (type == "window")
		return handle_window_action(action.target, strutil::to_lower(action.state), action.toggle);
	if (type == "screen") {
		// Same-file detection: shipped menus spell same-file jumps with their
		// own filename; empty file = same file.
		if (action.file.empty() || strutil::iequals(action.file, menu_file_))
			return navigate_to_screen(action.target);
		MenuEvent e;
		e.kind = MenuEvent::Kind::MenuRequested;
		e.text = action.file;
		e.text2 = action.target;
		emit_(e);
		return true;
	}
	if (type == "pop" || type == "pop_screen") {
		pop_screen();
		return true;
	}
	if (type == "quit" || type == "quit_game") {
		MenuEvent e;
		e.kind = MenuEvent::Kind::QuitRequested;
		emit_(e);
		return true;
	}
	if (type == "url") {
		MenuEvent e;
		e.kind = MenuEvent::Kind::UrlRequested;
		e.text = action.target;
		emit_(e);
		return true;
	}
	if (type == "tab") {
		// TAB selects the named focus target; the compiled path focuses edit
		// targets (the only focus model the frame carries).
		const int target_id = widget_id(action.target);
		if (target_id < 0 || widget_screen_of(target_id) != current_screen_) return false;
		if (!is_widget_shown(target_id) || is_widget_disabled(target_id)) return false;
		if (widget_kind_of(target_id) == kKindEdit) focus_edit(target_id);
		return true;
	}
	return false;
}

bool MenuRuntime::handle_window_action(const std::string &target, const std::string &state,
		bool toggle) {
	const int id = widget_id(target);
	if (id < 0 || widget_screen_of(id) != current_screen_) return false;
	if (state == "enable") {
		set_widget_disabled(id, toggle ? !is_widget_disabled(id) : false);
		return true;
	}
	if (state == "disable") {
		set_widget_disabled(id, toggle ? !is_widget_disabled(id) : true);
		return true;
	}
	if (state == "show") {
		set_widget_shown(id, toggle ? !is_widget_shown(id) : true);
		return true;
	}
	if (state == "hide" || state == "toggle") {
		set_widget_shown(id, (toggle || state == "toggle") ? !is_widget_shown(id) : false);
		return true;
	}
	return false;
}

void MenuRuntime::play_widget_state_sound(int id, const std::string &state_token) {
	const mnu::Window *w = index_.window(id);
	if (w == nullptr) return;
	for (const mnu::Sound &sound : w->sounds) {
		if (!strutil::iequals(sound.state, state_token)) continue;
		MenuEvent e;
		e.kind = MenuEvent::Kind::Sound;
		e.text = sound.file;
		e.text2 = sound.trigger;
		emit_(e);
		return;
	}
}

void MenuRuntime::emit_value_changed_for_(int id, int row) {
	const char *kind = "list";
	switch (widget_kind_of(id)) {
		case kKindCombo: kind = "combo"; break;
		case kKindMulti: kind = "multi"; break;
		case kKindSpinList: kind = "spinlist"; break;
		case kKindTable: kind = "table"; break;
		default: break;
	}
	MenuEvent e;
	e.kind = MenuEvent::Kind::ValueChanged;
	e.text = widget_name_of(id);
	e.text2 = kind;
	e.value = row;
	e.text3 = item_text(id, row);
	emit_(e);
}

// ---- input: mouse -----------------------------------------------------------

void MenuRuntime::process_mouse(float x, float y, bool button_down) {
	if (frame_ == nullptr || !frame_->is_configured()) return;
	last_mouse_x_ = x;
	last_mouse_y_ = y;
	const bool down_edge = button_down && !mouse_down_;
	mouse_down_ = button_down;

	// An open dropdown owns the mouse exclusively [orig: UI_DispatchMouseEvent
	// @0x63ab00 g_UIOpenPopupWnd gate; CComboWnd_HandleEvent @0x65c190,
	// outside check @0x65c290 — D-MNU-11/12]: a press picks a popup row or
	// dismisses (the dismissing click is consumed either way; a press on the
	// input-dead closed cell does nothing).
	if (open_combo_id_ >= 0) {
		const int combo_index = frame_index(open_combo_id_);
		if (combo_index < 0) {
			open_combo_id_ = -1;
		} else {
			frame_->set_cursor_state(false, x, y);
			// The popup's scrollbar child sees the sample ahead of row picking
			// [orig: CListWnd child walk @0x643f30 — the scrollbar child claims
			// first; parts = CScrollWnd_HandleEvent @0x64d050]. Its scroll_row
			// changes arrive through on_frame_scroll_value like the main pump's.
			if (frame_->process_popup_mouse(combo_index, x, y, button_down)) {
				frame_->set_widget_hover_item(combo_index, -1);
				return;
			}
			// The popup-exclusive pump hovers the row under the mouse (style 2)
			// [orig: the per-frame pump runs ONLY on the popup while open —
			// CUIScene_EndFrame @0x63e600 gate @0x63e691; the row mouseover style
			// = CListWnd_DrawItems @0x643f30].
			frame_->set_widget_hover_item(combo_index,
					frame_->combo_popup_row_at(combo_index, x, y));
			if (down_edge) {
				const int row = frame_->combo_popup_row_at(combo_index, x, y);
				if (row >= 0) {
					const int combo_id = open_combo_id_;
					select_row(combo_id, row, true);
					play_widget_state_sound(combo_id, "SELECTED");
					close_active_combo_popup();
				} else {
					float sx = 1.0f, sy = 1.0f;
					frame_->design_scale(sx, sy);
					if (!frame_->combo_popup_contains(combo_index, x, y) &&
							!frame_->widget_rect(combo_index).contains(x / sx, y / sy))
						close_active_combo_popup();
				}
			}
			return;
		}
	}

	// The CScrollWnd interaction (arrows/track/shuttle drag) lives in the
	// engine pump; its value changes arrive through on_frame_scroll_value.
	const int claim = frame_->process_mouse(x, y, button_down);
	if (frame_ == nullptr) return;
	frame_->set_cursor_state(false, x, y);
	if (claim != last_claim_) {
		on_claim_changed_(last_claim_, claim);
		last_claim_ = claim;
	}
	if (frame_ != nullptr) frame_->apply_claim_cursor();
}

bool MenuRuntime::process_wheel(float x, float y, int steps) {
	if (frame_ == nullptr || !frame_->is_configured()) return false;
	if (!frame_->process_mouse_wheel(x, y, steps)) return false;
	if (open_combo_id_ >= 0) {
		const int combo_index = frame_index(open_combo_id_);
		// Keep the popup row hover matching the rows that just moved under the
		// still cursor.
		if (combo_index >= 0)
			frame_->set_widget_hover_item(combo_index,
					frame_->combo_popup_row_at(combo_index, x, y));
	}
	return true;
}

void MenuRuntime::on_claim_changed_(int previous, int current) {
	// The hover sound edges ride the visual-state transitions
	// [orig: CWnd_ProcessMouseEvent @0x647a00 — MOUSEIN on entering state
	// 2/3, MOUSEOUT on leaving the widget].
	if (previous >= 0) {
		const int prev_id = id_at_index(previous);
		if (prev_id >= 0) {
			play_widget_state_sound(prev_id, "MOUSEOUT");
			MenuEvent e;
			e.kind = MenuEvent::Kind::HoverChanged;
			e.id = prev_id;
			e.flag = false;
			emit_(e);
		}
	}
	if (current >= 0 && frame_ != nullptr && !frame_->is_widget_disabled(current)) {
		const int id = id_at_index(current);
		if (id >= 0) {
			play_widget_state_sound(id, "MOUSEIN");
			MenuEvent e;
			e.kind = MenuEvent::Kind::HoverChanged;
			e.id = id;
			e.flag = true;
			emit_(e);
		}
	}
}

void MenuRuntime::on_widget_clicked(int index, uint32_t now_ms, bool ctrl_down) {
	const int id = id_at_index(index);
	if (id < 0 || frame_ == nullptr || frame_->is_widget_disabled(index)) return;
	activate_widget_(id, index, last_mouse_x_, last_mouse_y_, now_ms, ctrl_down);
}

void MenuRuntime::activate_widget_(int id, int index, float x, float y, uint32_t now_ms,
		bool ctrl_down) {
	const int kind = widget_kind_of(id);
	switch (kind) {
		case kKindButton:
		case kKindGoto:
		case kKindStatic:
		case kKindLabel:
			play_widget_state_sound(id, "SELECTED");
			activate(id);
			break;
		case kKindCheckBox:
			set_widget_checked(id, !is_widget_checked(id));
			play_widget_state_sound(id, "SELECTED");
			activate(id);
			break;
		case kKindRadio:
			select_radio(id);
			play_widget_state_sound(id, "SELECTED");
			activate(id);
			break;
		case kKindCombo:
			play_widget_state_sound(id, "SELECTED");
			if (open_combo_id_ == id)
				close_active_combo_popup();
			else
				open_combo_popup_(id);
			break;
		case kKindList:
		case kKindMulti:
		case kKindLanList: {
			const int row = index >= 0 ? frame_->list_row_at(index, x, y) : -1;
			if (row >= 0) list_click_(id, kind, row, now_ms, ctrl_down);
			break;
		}
		case kKindSpinList: {
			const int arrow = frame_->spin_arrow_at(index, x, y);
			if (arrow == 1)
				spin_cycle(id, 1);
			else if (arrow == 2)
				spin_cycle(id, -1);
			break;
		}
		case kKindEdit:
			focus_edit(id);
			break;
		case kKindTable: {
			const int row = frame_->table_row_at(index, x, y);
			if (row >= 0) table_click_(id, row, now_ms, ctrl_down);
			break;
		}
		default: {
			// Generic containers: actions still dispatch (authored WINDOW
			// widgets carry SCREEN jumps in shipped menus).
			const mnu::Window *w = index_.window(id);
			if (w != nullptr && !w->actions.empty()) {
				play_widget_state_sound(id, "SELECTED");
				activate(id);
			}
			break;
		}
	}
}

// The double-click latch both row owners share: true on the second click of
// the same row inside the window (which then re-arms from scratch).
bool MenuRuntime::register_click_(int id, int row, uint32_t now_ms) {
	const bool is_double = id == last_click_id_ && row == last_click_row_ &&
			now_ms - last_click_ms_ <= kDoubleClickMs;
	last_click_id_ = id;
	last_click_row_ = row;
	last_click_ms_ = is_double ? 0 : now_ms;
	return is_double;
}

void MenuRuntime::list_click_(int id, int kind, int row, uint32_t now_ms, bool ctrl_down) {
	const bool is_double = register_click_(id, row, now_ms);
	if (kind == kKindMulti) {
		std::vector<int> selected = ctrl_down ? selected_set(id) : std::vector<int>();
		toggle_row(selected, row);
		set_selected_set(id, selected);
	}
	select_row(id, row, true); // emits the "list"/"multi" value change
	play_widget_state_sound(id, "SELECTED");
	if (is_double) {
		MenuEvent e;
		e.kind = MenuEvent::Kind::ListActivated;
		e.id = id;
		e.value = row;
		emit_(e);
	}
}

void MenuRuntime::table_click_(int id, int row, uint32_t now_ms, bool ctrl_down) {
	const bool is_double = register_click_(id, row, now_ms);
	const mnu::Window *w = index_.window(id);
	const bool multiselect = w != nullptr && w->items.multiselect;
	table_select_row(id, row, multiselect && ctrl_down);
	play_widget_state_sound(id, "SELECTED");
	if (is_double) {
		MenuEvent e;
		e.kind = MenuEvent::Kind::ListActivated;
		e.id = id;
		e.value = row;
		emit_(e);
	}
}

// ---- combo popups -----------------------------------------------------------

void MenuRuntime::open_combo_popup_(int id) {
	// One dropdown per menu: opening one closes the previous
	// [orig: g_UIActiveComboWnd @0x31C16D0, single-open toggle @0x65c210].
	close_active_combo_popup();
	open_combo_id_ = id;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_popup_open(index, true);
}

void MenuRuntime::close_active_combo_popup() {
	if (open_combo_id_ < 0) return;
	const int index = frame_index(open_combo_id_);
	if (index >= 0) {
		frame_->set_widget_popup_open(index, false);
		frame_->set_widget_hover_item(index, -1);
	}
	open_combo_id_ = -1;
}

// ---- edit focus + keyboard --------------------------------------------------

void MenuRuntime::focus_edit(int id) {
	// Click focuses unless read-only [orig: CEditWnd_HandleInputEvent
	// @0x661510 — g_UIFocusWnd = this unless widget[194]].
	const mnu::Window *w = index_.window(id);
	if (w != nullptr && w->readonly) return;
	if (focus_id_ == id) return;
	clear_edit_focus_();
	focus_id_ = id;
	const int index = frame_index(id);
	if (index >= 0) {
		frame_->set_widget_focused(index, true);
		if (frame_->get_widget_caret(index) < 0)
			frame_->set_widget_caret(index, utf8_length(get_widget_text(id)));
	}
}

void MenuRuntime::clear_edit_focus_() {
	if (focus_id_ < 0) return;
	const int id = focus_id_;
	focus_id_ = -1;
	const int index = frame_index(id);
	if (index >= 0) {
		frame_->set_widget_focused(index, false);
		// Persist the edited text for cross-screen reads.
		remember_widget_text(id, frame_->get_widget_text(index));
	}
	emit_edit_changed(id);
}

bool MenuRuntime::handle_key(const MenuKeyInput &key, uint32_t now_ms, bool ctrl_down) {
	if (frame_ == nullptr || !frame_->is_configured()) return false;
	if (focus_id_ >= 0 && route_edit_key_(key)) return true;
	const char *vk = nullptr;
	if (key.key == MenuKeyInput::Key::Escape)
		vk = "VK_ESCAPE";
	else if (key.key == MenuKeyInput::Key::Enter)
		vk = "VK_RETURN";
	if (vk != nullptr) {
		const int target = frame_->hotkey_widget(vk, true);
		if (target >= 0 && trigger_hotkey_target_(target, now_ms, ctrl_down)) return true;
	}
	int unicode = key.unicode;
	if (unicode == 0 && key.printable_keycode >= 0x20 && key.printable_keycode <= 0x7E)
		unicode = key.printable_keycode;
	if (unicode > 0 && frame_ != nullptr) {
		const int target = frame_->hotkey_widget(utf8_of(unicode), false);
		if (target >= 0 && trigger_hotkey_target_(target, now_ms, ctrl_down)) return true;
	}
	return false;
}

bool MenuRuntime::route_edit_key_(const MenuKeyInput &key) {
	const int index = frame_index(focus_id_);
	if (index < 0) {
		focus_id_ = -1;
		return false;
	}
	const int id = focus_id_;
	int vk = 0;
	switch (key.key) {
		case MenuKeyInput::Key::Backspace: vk = kEditKeyBackspace; break;
		case MenuKeyInput::Key::Enter: vk = kEditKeyEnter; break;
		case MenuKeyInput::Key::End: vk = kEditKeyEnd; break;
		case MenuKeyInput::Key::Home: vk = kEditKeyHome; break;
		case MenuKeyInput::Key::Left: vk = kEditKeyLeft; break;
		case MenuKeyInput::Key::Right: vk = kEditKeyRight; break;
		case MenuKeyInput::Key::Delete: vk = kEditKeyDelete; break;
		default: break;
	}
	if (vk != 0) {
		const int result = frame_->edit_key(index, vk, key.shift);
		if (result == static_cast<int>(EditKeyResult::kCommit)) {
			// Enter commits: focus releases, then the edit's own callback takes
			// event 0x7000002 (the embedder's EditCommitted).
			// [orig: CEditWnd_HandleKeyEvent @0x6623a0 — g_UIFocusWnd = 0
			//  @0x66249f, the widget event 0x7000002 @0x6624e3]
			clear_edit_focus_();
			play_widget_state_sound(id, "SELECTED");
			MenuEvent committed;
			committed.kind = MenuEvent::Kind::EditCommitted;
			committed.id = id;
			committed.text = widget_name_of(id);
			emit_(committed);
		} else if (result == static_cast<int>(EditKeyResult::kChanged)) {
			emit_edit_changed(id);
		}
		return true;
	}
	if (key.unicode > 0) {
		if (frame_->edit_char(index, key.unicode)) emit_edit_changed(id);
		return true;
	}
	return false;
}

bool MenuRuntime::trigger_hotkey_target_(int index, uint32_t now_ms, bool ctrl_down) {
	// A disabled target consumes the key without firing (prevents a later
	// same-key widget firing through a disabled modal); an actionless match is
	// still consumed — actionless named controls are the retail Command seam
	// the shell wires by name.
	if (frame_->is_widget_disabled(index)) return true;
	const int id = id_at_index(index);
	if (id < 0) return true;
	if (widget_kind_of(id) == kKindEdit) {
		focus_edit(id);
		return true;
	}
	const MenuRectF rect = frame_->widget_rect(index);
	float sx = 1.0f, sy = 1.0f;
	frame_->design_scale(sx, sy);
	activate_widget_(id, index, (rect.x + rect.w * 0.5f) * sx, (rect.y + rect.h * 0.5f) * sy,
			now_ms, ctrl_down);
	return true;
}

} // namespace opennova::menu
