// The compiled-menu interaction runtime — see menu_runtime.h for the witness
// map. [orig: CUIWidget_HandleScriptedAction @0x6497f0; UI_DispatchMouseEvent
// @0x63ab00; UI_DispatchKeyboardEventToChildren @0x63ad10]

#include <runtime/menu/menu_runtime.h>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <runtime/menu/menu_edit.h>
#include <runtime/menu/options_policy.h>

#include <algorithm>
#include <cstdlib>

namespace opennova::menu {

namespace {

constexpr int kKindWindow = static_cast<int>(mnu::WindowType::Window);
constexpr int kKindEdit = static_cast<int>(mnu::WindowType::Edit);
constexpr int kKindMultilineEdit = static_cast<int>(mnu::WindowType::MultilineEdit);
constexpr int kKindList = static_cast<int>(mnu::WindowType::List);
constexpr int kKindCheckBox = static_cast<int>(mnu::WindowType::CheckBox);
constexpr int kKindRadio = static_cast<int>(mnu::WindowType::Radio);
constexpr int kKindCombo = static_cast<int>(mnu::WindowType::Combo);
constexpr int kKindScroll = static_cast<int>(mnu::WindowType::Scroll);
constexpr int kKindTable = static_cast<int>(mnu::WindowType::Table);
constexpr int kKindSpinList = static_cast<int>(mnu::WindowType::SpinList);
constexpr int kKindLanList = static_cast<int>(mnu::WindowType::LanList);
constexpr int kKindRadioEdit = static_cast<int>(mnu::WindowType::RadioEdit);
constexpr int kKindGlbTable = static_cast<int>(mnu::WindowType::GlbTable);
constexpr int kKindGopher = static_cast<int>(mnu::WindowType::Gopher);

// The ACTION type codes [orig: CUIElement_ParseXMLDefinition @ 0x648ee2 — the
// TYPE's first token by CRT_wcsicmp @ 0x648f4a..0x6490e9; any other token
// leaves 0].
enum ActionCode {
	kActionNone = 0,
	kActionScreen = 1,
	kActionWindow = 2,
	kActionUrl = 3,
	kActionFormPost = 4,
	kActionGlbFilter = 7,
	kActionGlbFilterNum = 8,
	kActionTab = 11,
	kActionPopScreen = 12,
};

int action_code(const std::string &type) {
	for (int i = 0; mnu::kActionTypes[i]; ++i)
		if (strutil::iequals(type, mnu::kActionTypes[i])) return i + 1;
	return kActionNone;
}

// A VIRTUAL HOTKEY's key [orig: CWnd_ParseVirtualKeyNameW @ 0x6467f0 — the
// untrimmed text by CRT_wcsicmp: VK_RETURN 13, VK_ESCAPE 27, VK_SPACE 32, any
// other name 0 (a row the registration skips: VK_ENTER is dead)].
int virtual_key_code(const std::string &name) {
	if (strutil::iequals(name, "VK_RETURN")) return 13;
	if (strutil::iequals(name, "VK_ESCAPE")) return 27;
	if (strutil::iequals(name, "VK_SPACE")) return 32;
	return 0;
}

// A character HOTKEY's key: the text's first wide character [orig: @ 0x649615,
// movzx of the first WCHAR]. A code-page menu's model holds bytes (the byte is
// the character below 0x80 and for Latin-1 above 0x9F); a Unicode menu's holds
// UTF-8.
int first_character(const std::string &text, bool utf8) {
	if (text.empty()) return 0;
	const unsigned char lead = static_cast<unsigned char>(text[0]);
	if (!utf8 || lead < 0x80) return lead;
	int extra = 0;
	uint32_t cp = 0;
	if ((lead & 0xE0) == 0xC0) {
		extra = 1;
		cp = lead & 0x1Fu;
	} else if ((lead & 0xF0) == 0xE0) {
		extra = 2;
		cp = lead & 0x0Fu;
	} else if ((lead & 0xF8) == 0xF0) {
		extra = 3;
		cp = lead & 0x07u;
	} else {
		return lead;
	}
	if (text.size() < static_cast<size_t>(extra) + 1) return lead;
	for (int i = 1; i <= extra; ++i) cp = (cp << 6) | (static_cast<unsigned char>(text[static_cast<size_t>(i)]) & 0x3Fu);
	return static_cast<int>(cp);
}

// The CRT tolower of the "C" locale both sides of a character row compare
// through [orig: UI_DispatchKeyboardEventToChildren @ 0x63ad78 / 0x63ad84].
int fold_key(int key) {
	return key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key;
}

bool contains_ci(const std::string &haystack, const char *needle) {
	return strutil::to_lower(haystack).find(needle) != std::string::npos;
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
		case mnu::WindowType::LanList:
		case mnu::WindowType::SpinList:
			return &window->items;
		case mnu::WindowType::Combo:
			// LIST_BOX owns combo items when it actually authors an ITEMS block
			// under an authored LIST_BOX. A latent/disabled LIST_BOX or a
			// styling-only one falls back to top-level ITEMS, matching runtime.
			return window->list_box && window->list_box->items.present
					? &window->list_box->items
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
	ids_.clear();
}

int MenuDocIndex::add_window_(const mnu::Window &w, int parent_id, int screen_index) {
	const int id = static_cast<int>(nodes_.size()) + 1;
	Node node;
	node.id = id;
	node.parent_id = parent_id;
	node.screen_index = screen_index;
	node.window = &w;
	nodes_.push_back(std::move(node));
	ids_[&w] = id;
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
		// Every root window, in document order.
		std::vector<int> roots;
		for (const mnu::Window &root : doc.screens[i].roots)
			roots.push_back(add_window_(root, screen_id, static_cast<int>(i)));
		nodes_[static_cast<size_t>(screen_id - 1)].child_ids = std::move(roots);
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

int MenuDocIndex::id_of(const mnu::Window *window) const {
	const auto it = window != nullptr ? ids_.find(window) : ids_.end();
	return it != ids_.end() ? it->second : -1;
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
	id_info_.clear();
	screen_ids_.clear();
	screen_order_.clear();
	nav_stack_.clear();
	hotkeys_.clear();
	pending_requests_.clear();
	// A new document: no focus, no open dropdown or popup, no claim.
	focus_id_ = -1;
	open_combo_id_ = -1;
	open_popup_id_ = -1;
	frame_popup_index_ = -1;
	last_claim_ = -1;
	// Its windows are new: none holds a sound state.
	sound_pump_.reset();
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
		if (const MenuDocIndex::Node *screen_node = index_.node(screen_id))
			for (int root : screen_node->child_ids) index_widget_subtree_(screen->name, root);
	}
}

void MenuRuntime::index_widget_subtree_(const std::string &screen_name, int id) {
	const MenuDocIndex::Node *node = index_.node(id);
	if (node == nullptr || node->window == nullptr) return;
	WidgetInfo info;
	info.screen = screen_name;
	info.name = node->window->name;
	info.kind = static_cast<int>(node->window->type);
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
	open_popup_id_ = -1;
	// The focus drops (the edit keeps what it was typed) [orig: @ 0x63b7ca].
	drop_focus_();
	// Screen names match case-insensitively; the AUTHORED spelling is the
	// current screen, so the per-widget screen tests below compare equal
	// whatever casing the caller used.
	const mnu::Screen *screen = index_.screen(found->second);
	current_screen_ = screen != nullptr ? screen->name : name;
	configure_frame_();
	on_screen_shown_();
	return true;
}

bool MenuRuntime::navigate_to_screen(const std::string &name) {
	if (index_.document() == nullptr ||
			screen_ids_.find(strutil::to_upper(name)) == screen_ids_.end())
		return false;
	if (!current_screen_.empty()) nav_stack_.push_back(current_screen_);
	return show_screen(name);
}

bool MenuRuntime::pop_screen() {
	if (!nav_stack_.empty()) {
		const std::string prev = nav_stack_.back();
		nav_stack_.pop_back();
		show_screen(prev); // [orig: CUIScene_SelectNodeByName(name, 0): no push]
		return true;
	}
	MenuEvent e;
	e.kind = MenuEvent::Kind::PopRequested;
	emit_(e);
	return false;
}

void MenuRuntime::leave_menu_mode() {
	// Retail's one history already holds this file's screens below the current one;
	// the runtime's own back stack is that part of it, so it moves over first.
	for (const std::string &screen : nav_stack_) history_.push(menu_file_, screen);
	nav_stack_.clear();
	// The screen the menu mode is left on, and the mark on it.
	// [orig: UIScene_MarkScreenHistory @0x63c3d0, from @0x54e514]
	history_.mark(menu_file_, current_screen_);
}

bool MenuRuntime::return_to_menu_mode(ScreenHistoryRow *out) {
	// [orig: UIScene_ReturnToHistoryScreen @0x63dfa0, pop_extra 1 @0x552680]
	return history_.return_to_mark(true, out);
}

void MenuRuntime::on_screen_shown_() {
	// The activate dispatch's STARTUP leg sets the screen's VERSION label to the
	// build's version text, the game's own (gameprofile::GameProfile::version_text;
	// none for a game whose binary is unwitnessed); the backdrop movies it toggles
	// are menu_video.h's.
	// [orig: UI_DispatchScreenEvent @0x54e6a0 case 5, stricmp(screen, "STARTUP")
	//  @0x54eeff -> UI_OnStartupScreenActivate @0x5557f0: UI_FindScreenControl(
	//  g_GameMenu, "STARTUP", "VERSION") @0x555840, then the control's vtable+76
	//  (CButtonWnd_SetLabel @0x6572f0) with byte_B4C070 @0x555857]
	if (strutil::iequals(current_screen_, "STARTUP")) {
		const int version = find_screen_control("STARTUP", "VERSION");
		const char *text = gameprofile::gameprofile_version_text_for_code(game_code_.c_str());
		if (version >= 0) set_widget_text(version, text != nullptr ? text : "");
	}
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
	if (frame_ != nullptr) {
		frame_->configure_screen(current_screen_);
		frame_popup_index_ = -1;
		replay_state_();
	}
	build_hotkeys_();
	if (frame_ == nullptr) return;
	sync_popup_();
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
	// Every root window of the screen, in document order (the frame's roots).
	if (const MenuDocIndex::Node *screen = index_.node(screen_id))
		for (int root : screen->child_ids) map_widget_subtree_(root);
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
		if (state->has_table_columns)
			frame_->set_widget_table_columns(index, true, state->table_columns,
					state->table_sort_column);
		if (state->has_table_rows) frame_->set_widget_table_rows(index, state->table_rows);
		if (state->has_clip)
			frame_->set_widget_clip_rect(index, state->clip_enabled, state->clip_left,
					state->clip_top, state->clip_right, state->clip_bottom);
		if (state->has_rect)
			frame_->set_widget_rect(index, state->rect_left, state->rect_top, state->rect_right,
					state->rect_bottom);
	}
}

// ---- addressing -------------------------------------------------------------

int MenuRuntime::widget_id(const std::string &name) const {
	const int here = find_control(std::string(), name);
	if (here >= 0) return here;
	for (int screen_id : index_.screen_ids()) {
		const mnu::Screen *screen = index_.screen(screen_id);
		if (screen == nullptr) continue;
		const int found = index_.id_of(mnu::find_window(*screen, name));
		if (found >= 0) return found;
	}
	return -1;
}

int MenuRuntime::find_control(const std::string &screen, const std::string &name) const {
	if (name.empty()) return -1;
	const auto it = screen_ids_.find(strutil::to_upper(screen.empty() ? current_screen_ : screen));
	if (it == screen_ids_.end()) return -1;
	const mnu::Screen *section = index_.screen(it->second);
	return section != nullptr ? index_.id_of(mnu::find_window(*section, name)) : -1;
}

int MenuRuntime::parent_window_(int id) const {
	const MenuDocIndex::Node *node = index_.node(id);
	if (node == nullptr || node->window == nullptr) return -1;
	const MenuDocIndex::Node *parent = index_.node(node->parent_id);
	return parent != nullptr && parent->window != nullptr ? parent->id : -1;
}

int MenuRuntime::find_screen_control(const std::string &screen, const std::string &name) const {
	// [orig: UI_FindScreenControl @0x63ae80 — the first section whose name stricmps
	//  equal, then CWnd_FindChildByName over its root windows in order (mnu::find_window)]
	for (int screen_id : index_.screen_ids()) {
		const mnu::Screen *section = index_.screen(screen_id);
		if (section == nullptr || !strutil::iequals(section->name, screen)) continue;
		return index_.id_of(mnu::find_window(*section, name));
	}
	return -1;
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

void MenuRuntime::set_widget_clip_rect(int id, bool enabled, int left, int top, int right,
		int bottom) {
	MenuWidgetRuntimeState &state = state_of_(id);
	state.has_clip = true;
	state.clip_enabled = enabled;
	state.clip_left = left;
	state.clip_top = top;
	state.clip_right = right;
	state.clip_bottom = bottom;
	const int index = frame_index(id);
	if (index >= 0) frame_->set_widget_clip_rect(index, enabled, left, top, right, bottom);
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
	// Fresh rows reset the selection unless the caller re-selects. A MULTISELECT
	// list's selection set goes too: its indexes named the old rows. (Retail keeps
	// its rows and hides the ones a filter drops, and a hidden row never reads as
	// selected [orig: CListWnd_IsRowSelected @ 0x645150]; a rebuilt row list has no
	// such row to carry the mark.)
	state.selected_item = items.empty() ? -1 : 0;
	state.has_selected_item = true;
	state.scroll_row = 0;
	state.has_scroll_row = true;
	const bool had_set = state.has_selected_set;
	state.selected_set.clear();
	state.has_selected_set = false;
	const int index = frame_index(id);
	if (index >= 0) {
		frame_->set_widget_items(index, items);
		frame_->set_widget_selection(index, state.selected_item, -1, 0);
		if (had_set) frame_->set_widget_selected_set(index, std::vector<int>());
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

std::string MenuRuntime::item_display_text(int id, int row) const {
	const int index = frame_index(id);
	return index >= 0 && frame_ != nullptr ? frame_->item_display_text(index, row) : std::string();
}

std::string MenuRuntime::widget_string(int id, const std::string &key) const {
	const int index = frame_index(id);
	return index >= 0 && frame_ != nullptr ? frame_->widget_string(index, key) : key;
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

bool MenuRuntime::table_multiselect_(int id) const {
	const mnu::Window *w = index_.window(id);
	return w != nullptr && w->items.multiselect;
}

// The COLUMN COUNT the row operations bound (1 without an authored count of
// 1 or more) [orig: resize_column_count @0x63f6c0].
int MenuRuntime::table_column_count_(int id) const {
	// Columns code installed resized the table to theirs [orig: resize_column_count
	// @0x63f6c0 from StatScreen_PopulateStatResultsList @ 0x562240].
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state != nullptr && state->has_table_columns)
		return static_cast<int>(state->table_columns.size());
	const mnu::Window *w = index_.window(id);
	if (w == nullptr) return 0;
	const mnu::TableColumn &column = w->table_data.column;
	return column.has_count && column.count >= 1 ? column.count : 1;
}

namespace {

// A record the table had, which a count that does not grow the table leaves in
// place (menu_table.h): the frame draws the authored column there.
MenuTableColumn kept_record() {
	MenuTableColumn column;
	column.kept = true;
	column.defined = false;
	return column;
}

// A record a growing count starts over: every member zero (menu_table.h)
// [orig: CTableWnd_ResizeColumnCount @0x63f6c0 — the memset @0x63f710].
MenuTableColumn zeroed_record() {
	MenuTableColumn column;
	column.justify = column.vjustify = 0;
	column.body_justify = column.body_vjustify = 0;
	column.ascending = false;
	column.defined = false;
	return column;
}

// CTableWnd_InitRow(column, numeric_sort, width, label, justify, vjustify) on one
// record: the label, the sort compare (+112), ascending (+120 = 1), the width
// (+124), the header justification with -1 taking 1 / 16 (+128 / +132), the
// cells' copied from it (+144 / +148). The record's cell type, cell offsets,
// SUBST rows and bitmap scale stay (`kept`, `cell_type`).
// [orig: CTableWnd_InitRow @0x63f9c0 — +112 @0x63fb5c, +120 @0x63fb75, +124
//  @0x63fb7f, +128 / +132 @0x63fb98..0x63fbd8, +0x90 / +0x94 copied
//  @0x63fbdf..0x63fc03]
void init_record(MenuTableColumn &record, bool numeric_sort, int width, const std::string &label,
		int justify, int vjustify) {
	record.defined = true;
	record.label = label;
	record.numeric_sort = numeric_sort;
	record.ascending = true;
	record.width = width;
	record.justify = justify == -1 ? 1 : justify;
	record.vjustify = vjustify == -1 ? 16 : vjustify;
	record.body_justify = record.justify;
	record.body_vjustify = record.vjustify;
}

} // namespace

// The count over the records the table holds (menu_table.h): the authored ones
// until code set a count, kept by a count that does not grow the table, every one
// started over by one that does. [orig: CTableWnd_ResizeColumnCount @0x63f6c0]
void MenuRuntime::table_resize_records_(int id, int count) {
	const int current = table_column_count_(id);
	MenuWidgetRuntimeState &state = state_of_(id);
	if (!state.has_table_columns)
		state.table_columns.assign(static_cast<size_t>(current), kept_record());
	if (count > current)
		state.table_columns.assign(static_cast<size_t>(count), zeroed_record());
	else
		state.table_columns.resize(static_cast<size_t>(count));
	state.has_table_columns = true;
}

void MenuRuntime::push_table_columns_(int id) {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	const int index = frame_index(id);
	if (state != nullptr && index >= 0)
		frame_->set_widget_table_columns(index, true, state->table_columns, state->table_sort_column);
}

bool MenuRuntime::table_set_column_count(int id, int count) {
	if (widget_kind_of(id) != kKindTable || count < 1) return false;
	table_resize_records_(id, count);
	push_table_columns_(id);
	return true;
}

bool MenuRuntime::table_init_column(int id, int column, int width, const std::string &label,
		int justify, int vjustify) {
	if (widget_kind_of(id) != kKindTable) return false;
	const int count = table_column_count_(id);
	if (column < 0 || column >= count) return false;
	MenuWidgetRuntimeState &state = state_of_(id);
	if (!state.has_table_columns) table_resize_records_(id, count); // the records as they stand
	init_record(state.table_columns[static_cast<size_t>(column)], false, width, label, justify,
			vjustify);
	push_table_columns_(id);
	return true;
}

void MenuRuntime::table_add_row(int id, const std::vector<std::string> &cells) {
	MenuWidgetRuntimeState &state = state_of_(id);
	MenuTableRow row;
	row.cells = cells;
	state.table_rows.push_back(std::move(row));
	state.has_table_rows = true;
	push_table_rows_(id);
}

int MenuRuntime::table_insert_row(int id, const std::string &text0, int32_t value0,
		uint32_t flags, int insert_index) {
	if (widget_kind_of(id) != kKindTable) return -1;
	MenuWidgetRuntimeState &state = state_of_(id);
	const int at = menu::table_insert_row(state.table_rows, text0, value0, flags, insert_index);
	state.has_table_rows = true;
	push_table_rows_(id);
	return at;
}

void MenuRuntime::table_set_cell_text(int id, int row, int col, const std::string &text) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (menu::table_set_cell_text(state.table_rows, row, col, table_column_count_(id), text))
		push_table_rows_(id);
}

void MenuRuntime::table_set_cell_value(int id, int row, int col, int32_t value) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (menu::table_set_cell_value(state.table_rows, row, col, table_column_count_(id), value))
		push_table_rows_(id);
}

int32_t MenuRuntime::table_cell_value(int id, int row, int col) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	return state != nullptr ? menu::table_cell_value(state->table_rows, row, col) : 0;
}

void MenuRuntime::table_remove_row(int id, int row) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (row != -1 && (row < 0 || row >= static_cast<int>(state.table_rows.size()))) return;
	menu::table_remove_row(state.table_rows, row);
	state.has_table_rows = true;
	// The first visible row clamps to the count [orig: @0x641c0b..0x641c1a].
	if (state.scroll_row > static_cast<int>(state.table_rows.size())) {
		state.scroll_row = static_cast<int>(state.table_rows.size());
		state.has_scroll_row = true;
	}
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
	return state->table_rows[static_cast<size_t>(row)].cell(col);
}

int32_t MenuRuntime::table_row_state(int id, int row) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state == nullptr || row < 0 || row >= static_cast<int>(state->table_rows.size()))
		return kTableRowDefault;
	return state->table_rows[static_cast<size_t>(row)].state;
}

void MenuRuntime::table_set_row_selected(int id, int row, bool selected) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (menu::table_set_row_selected(state.table_rows, row, selected, table_multiselect_(id)))
		push_table_rows_(id);
}

void MenuRuntime::table_set_row_color(int id, int row, bool enable, uint32_t color) {
	MenuWidgetRuntimeState &state = state_of_(id);
	menu::table_set_row_color(state.table_rows, row, enable, color);
	push_table_rows_(id);
}

std::vector<int> MenuRuntime::table_selected_rows(int id) const {
	std::vector<int> out;
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (state == nullptr) return out;
	for (size_t r = 0; r < state->table_rows.size(); ++r)
		if (state->table_rows[r].state == kTableRowSelected) out.push_back(static_cast<int>(r));
	return out;
}

void MenuRuntime::table_select_row(int id, int row, bool additive) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (row < 0 || row >= static_cast<int>(state.table_rows.size())) return;
	if (additive)
		menu::table_click_select(state.table_rows, row, true);
	else
		menu::table_set_row_selected(state.table_rows, row, true, false);
	push_table_rows_(id);
}

// The stat fill's set-up: the count, then one init per column, each column's label,
// width, justification and sort compare [orig: StatScreen_PopulateStatResultsList
// @ 0x562240 — the count through the vtable +0x6C @0x562302, the inits
// @0x562346..0x56242b]. A count below 1 fails, which sets nothing up.
void MenuRuntime::table_set_columns(int id, const std::vector<MenuTableColumn> &columns) {
	if (widget_kind_of(id) != kKindTable || columns.empty()) return;
	table_resize_records_(id, static_cast<int>(columns.size()));
	MenuWidgetRuntimeState &state = state_of_(id);
	for (size_t i = 0; i < columns.size(); ++i) {
		const MenuTableColumn &c = columns[i];
		init_record(state.table_columns[i], c.numeric_sort, c.width, c.label, c.justify, c.vjustify);
	}
	state.table_sort_keys.assign(std::min<size_t>(std::max<size_t>(columns.size(), 1), 20), -1);
	state.table_sort_column = -1;
	push_table_columns_(id);
}

void MenuRuntime::table_set_column_ascending(int id, int column, bool ascending) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (column < 0 || column >= static_cast<int>(state.table_columns.size())) return;
	state.table_columns[static_cast<size_t>(column)].ascending = ascending;
	const int index = frame_index(id);
	if (index >= 0)
		frame_->set_widget_table_columns(index, true, state.table_columns, state.table_sort_column);
}

void MenuRuntime::table_sort_by_column(int id, int column) {
	MenuWidgetRuntimeState &state = state_of_(id);
	if (!state.has_table_columns) return;
	const int count = static_cast<int>(state.table_columns.size());
	table_push_sort_key(state.table_sort_keys, column, count);
	const std::vector<int> order =
			table_sort_order(state.table_rows, state.table_columns, state.table_sort_keys);
	// The row records move whole: their state (the selection), flags and colour
	// go with them.
	std::vector<MenuTableRow> rows;
	rows.reserve(order.size());
	for (const int from : order) rows.push_back(std::move(state.table_rows[static_cast<size_t>(from)]));
	state.table_rows = std::move(rows);
	if (column != -1) state.table_sort_column = column;
	const int index = frame_index(id);
	if (index >= 0)
		frame_->set_widget_table_columns(index, true, state.table_columns, state.table_sort_column);
	push_table_rows_(id);
}

int MenuRuntime::table_sort_column(int id) const {
	const MenuWidgetRuntimeState *state = saved_state_(id);
	return state != nullptr ? state->table_sort_column : -1;
}

void MenuRuntime::push_table_rows_(int id) {
	const int index = frame_index(id);
	const MenuWidgetRuntimeState *state = saved_state_(id);
	if (index < 0 || state == nullptr) return;
	frame_->set_widget_table_rows(index, state->table_rows);
	frame_->set_widget_selection(index, -1, -1, state->scroll_row);
}

// ---- activation / actions ---------------------------------------------------

void MenuRuntime::activate(int id) {
	const mnu::Window *w = index_.window(id);
	if (w == nullptr) return;
	const uint32_t generation = open_generation_;
	pending_requests_.clear();
	// The class handler's own step for the click event 0x3000001 [orig:
	// CCheckboxWnd_HandleNamedEvent @ 0x64ab80 toggles +0x304;
	// radio_button_on_click @ 0x656cd0 checks; CComboWnd_HandleEvent
	// @ 0x65c190 opens or closes the list, the single open dropdown].
	switch (widget_kind_of(id)) {
		case kKindCheckBox: set_widget_checked(id, !is_widget_checked(id)); break;
		case kKindRadio: select_radio(id); break;
		case kKindCombo:
			if (open_combo_id_ == id)
				close_active_combo_popup();
			else
				open_combo_popup_(id);
			break;
		default: break;
	}
	// The ACTION walk. A cross-file jump copies nothing it needs from `w`.
	const std::string name = w->name;
	if (!walk_rows_(*w, id, name.empty() ? false : true, generation)) return;
	// Up the parents [orig: @ 0x649c5d — the parent's handler with this widget's
	// NAME; an ancestor runs its rows only when that NAME is its own, byte for
	// byte (@ 0x649810), and passes its own NAME on].
	std::string passed = name;
	for (int parent = parent_window_(id); parent >= 0; parent = parent_window_(parent)) {
		const mnu::Window *pw = index_.window(parent);
		if (pw == nullptr) break;
		// walk_rows_ copies the rows first and stops at a document swap, so the
		// window is not read after one.
		if (!passed.empty() && pw->name == passed && !walk_rows_(*pw, parent, true, generation))
			return;
		passed = pw->name;
	}
	// The control callbacks, bound by NAME [orig: CWnd_EmitEventToNamedHandlerAndCallbacks
	// @ 0x646970 runs them after the handler; CUIScene_BindControlCallbacks
	// finds the control with CWnd_FindChildByName, so a nameless one has none].
	if (!name.empty()) {
		MenuEvent e;
		e.kind = MenuEvent::Kind::WidgetActivated;
		e.id = id;
		e.text = name;
		emit_(e);
		if (open_generation_ != generation) return;
	}
	raise_pending_requests_();
}

void MenuRuntime::raise_pending_requests_() {
	std::vector<MenuEvent> requests;
	requests.swap(pending_requests_);
	for (const MenuEvent &request : requests) emit_(request);
}

// [orig: CUIWidget_HandleScriptedAction @0x6497f0 — the rows are prepended as
// they parse (@ 0x6494da..0x6494e3), so the walk runs last-authored first, and
// every row runs; a widget with no NAME runs none (@ 0x649805)]
bool MenuRuntime::walk_rows_(const mnu::Window &w, int owner_id, bool named, uint32_t generation) {
	if (!named) return true;
	const std::vector<mnu::Action> rows = w.actions; // a row may swap the document
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		bool handled = false;
		run_row_(owner_id, *it, handled);
		if (open_generation_ != generation) return false;
	}
	return true;
}

// A row that moves between files is held (pending_requests_) and raised after the
// activation's callbacks: the runtime's document is the file it fired on until
// then. Once one is held, every later SCREEN and POP_SCREEN row is held behind it,
// since retail runs those against the screen the held one leaves current.
void MenuRuntime::run_row_(int owner_id, const mnu::Action &action, bool &handled) {
	switch (action_code(action.type)) {
		case kActionScreen: {
			// [orig: @ 0x649894 — UIScene_LoadAndParseContent(scene, FILE, target)
			// then CUIScene_SelectNodeByName(target, 1)] The menu's own file
			// selects in place; another file is the embedder's (a missing file
			// changes nothing there). No FILE is retail's fault (the editor refuses
			// it): nothing here.
			if (action.file.empty()) return;
			if (pending_requests_.empty() && strutil::iequals(action.file, menu_file_)) {
				handled = navigate_to_screen(action.target);
				return;
			}
			MenuEvent e;
			e.kind = MenuEvent::Kind::MenuRequested;
			e.text = action.file;
			e.text2 = action.target;
			pending_requests_.push_back(std::move(e));
			handled = true;
			return;
		}
		case kActionWindow:
			handled = window_row_(owner_id, action);
			return;
		case kActionUrl:
			url_row_(action);
			handled = true;
			return;
		case kActionFormPost:
			// [orig: @ 0x649a5c — the focus drops before UI_BuildURLAndSubmitRequest,
			// the embedder's]
			drop_focus_();
			service_row_(owner_id, action, kActionFormPost);
			return;
		case kActionPopScreen:
			// [orig: UIScene_PopScreenHistory(scene, 1) @ 0x63c410] The file's own
			// history pops in place; past it the embedder's cross-file history.
			if (pending_requests_.empty() && !nav_stack_.empty()) {
				pop_screen();
			} else {
				MenuEvent e;
				e.kind = MenuEvent::Kind::PopRequested;
				pending_requests_.push_back(std::move(e));
			}
			handled = true;
			return;
		default: {
			// Code 0 (an unknown or missing TYPE), GLB_FILTER, GLB_FILTER_NUM and
			// TAB fall to the jump table's default on activation; the rest are the
			// embedder's service verbs, raised for it as they run.
			const int code = action_code(action.type);
			if (code != kActionNone && code != kActionGlbFilter && code != kActionGlbFilterNum &&
					code != kActionTab)
				service_row_(owner_id, action, code);
			return;
		}
	}
}

// WINDOW [orig: @ 0x6498c8..0x6499c9]: the focus, capture and mouseover drop; the
// target is found on the OWNING screen (the acting widget's; a row run on its own,
// the current screen's) [orig: CNode_ScalarDeletingDestructor @ 0x646bf0 reads
// the root's screen name; UI_FindScreenControl @ 0x63ae80]. HIDE / SHOW write the
// shown flag, a MODAL target closing or opening the popup; with TOGGLE either
// flips it through CWnd_SetShown @ 0x6480e0 (a MODAL target opens the popup when
// shown, closes it when hidden). ENABLE / DISABLE write the enabled flag down the
// target's subtree, with TOGGLE to the opposite of the target's own [orig:
// UIWidget_SetInteractiveRecursive @ 0x6462e0]. Another STATE does nothing.
bool MenuRuntime::window_row_(int owner_id, const mnu::Action &action) {
	drop_focus_();
	const std::string screen = owner_id >= 0 ? widget_screen_of(owner_id) : std::string();
	const int target = find_control(screen, action.target);
	if (target < 0) return false;
	const mnu::Window *tw = index_.window(target);
	const bool modal = tw != nullptr && tw->modal;
	const bool hide = strutil::iequals(action.state, "HIDE");
	const bool show = strutil::iequals(action.state, "SHOW");
	if (hide || show) {
		const bool shown = action.toggle ? !is_widget_shown(target) : show;
		set_widget_shown(target, shown);
		if (modal) set_popup_(shown ? target : -1);
		return true;
	}
	const bool enable = strutil::iequals(action.state, "ENABLE");
	const bool disable = strutil::iequals(action.state, "DISABLE");
	if (enable || disable) {
		const bool enabled = action.toggle ? is_widget_disabled(target) : enable;
		set_interactive_recursive_(target, enabled);
		return true;
	}
	return false;
}

void MenuRuntime::set_interactive_recursive_(int id, bool enabled) {
	set_widget_disabled(id, !enabled);
	if (const MenuDocIndex::Node *node = index_.node(id))
		for (int child : node->child_ids) set_interactive_recursive_(child, enabled);
}

// URL [orig: @ 0x649a1c..0x649a57; UIScene_OpenUrl @ 0x63e2f0]: the focus drops;
// with a FIELD / SOURCE / NAME slot the text of that control on the current
// screen (none there: nothing), else the row's text; trimmed of " \t\n\r"; the
// external browser when EXTERNAL_BROWSER is present or the URL holds
// external_browser=1 or commercial_browser=1 (any case), else retail's in-game
// fetch (the embedder's).
void MenuRuntime::url_row_(const mnu::Action &action) {
	drop_focus_();
	std::string url;
	if (!action.field.empty()) {
		const int control = find_control(std::string(), action.field);
		if (control < 0) return;
		url = get_widget_text(control);
	} else {
		url = action.target;
	}
	const char *const kTrim = " \t\n\r";
	const size_t first = url.find_first_not_of(kTrim);
	url = first == std::string::npos ? std::string() : url.substr(first, url.find_last_not_of(kTrim) - first + 1);
	MenuEvent e;
	e.kind = MenuEvent::Kind::UrlRequested;
	e.text = url;
	e.flag = action.external_browser || contains_ci(url, "external_browser=1") ||
			contains_ci(url, "commercial_browser=1");
	emit_(e);
}

bool MenuRuntime::dispatch_action(const mnu::Action &action) {
	pending_requests_.clear();
	bool handled = false;
	run_row_(-1, action, handled);
	raise_pending_requests_();
	return handled;
}

void MenuRuntime::select_radio(int id) {
	const mnu::Window *w = index_.window(id);
	const int group = w != nullptr ? w->group : 0;
	const int parent = parent_window_(id);
	// The radios beside it (the parent's children, RADIO or RADIOEDIT) of the
	// same nonzero GROUP uncheck, then it checks.
	if (parent >= 0 && group != 0) {
		const std::vector<int> siblings = index_.node(parent)->child_ids;
		for (int other : siblings) {
			if (other == id) continue;
			const int kind = widget_kind_of(other);
			if (kind != kKindRadio && kind != kKindRadioEdit) continue;
			const mnu::Window *ow = index_.window(other);
			if (ow != nullptr && ow->group == group) set_widget_checked(other, false);
		}
	}
	set_widget_checked(id, true);
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
	if (count <= 0 || delta == 0) return;
	const int row = ((selected_row(id) + delta) % count + count) % count;
	select_row(id, row, true);
}

void MenuRuntime::play_widget_state_sound(int id, const std::string &state_token) {
	if (const mnu::Window *w = index_.window(id)) play_sound_(id, *w, state_token);
}

void MenuRuntime::play_sound_(int id, const mnu::Window &w, const std::string &state_token) {
	// The state's slot: the last row of it with a TRIGGER (menu_window_sound).
	const int state = menu_sound_state_of(state_token);
	const mnu::Sound *sound = menu_window_sound(w, state);
	if (sound == nullptr) return;
	MenuEvent e;
	e.kind = MenuEvent::Kind::Sound;
	e.id = id;
	e.value = state;
	e.text = sound->file;
	e.text2 = sound->trigger;
	e.text3 = w.name;
	emit_(e);
}

void MenuRuntime::service_row_(int owner_id, const mnu::Action &action, int code) {
	MenuEvent e;
	e.kind = MenuEvent::Kind::ServiceRequested;
	e.id = owner_id;
	e.value = code;
	e.text = mnu::kActionTypes[code - 1];
	e.text2 = action.target;
	e.text3 = action.file;
	emit_(e);
}

bool MenuRuntime::sound_reached_(int id) const {
	// A widget of the current screen shown up its chain; while a popup is open, one of its subtree
	// alone [orig: CWnd_ProcessMouseEvent @ 0x647a21 returns at once on a hidden widget, and its
	// children go unpumped; CUIScene_EndFrame @ 0x63e600 pumps the open popup alone].
	if (frame_index(id) < 0 || !drawn_(id)) return false;
	if (open_popup_id_ < 0) return true;
	for (int w = id; w >= 0; w = parent_window_(w))
		if (w == open_popup_id_) return true;
	return false;
}

void MenuRuntime::sample_sounds_(int claim, bool button_down) {
	const int id = id_at_index(claim);
	std::vector<MenuSoundPump::Edge> edges;
	// The claim is under the mouse only while visible in the hierarchy (shown and enabled up its
	// chain) [orig: CWnd_ProcessMouseEvent @ 0x647a27 -> CWnd_IsVisibleInHierarchy @ 0x646290].
	sound_pump_.sample(id >= 0, uint64_t(id >= 0 ? id : 0), id >= 0 && visible_in_hierarchy_(id), button_down,
			[this](uint64_t key) { return sound_reached_(int(key)); }, edges);
	for (const MenuSoundPump::Edge &edge : edges)
		play_widget_state_sound(int(edge.key), menu_sound_state_token(edge.state));
}

void MenuRuntime::emit_value_changed_for_(int id, int row) {
	const char *kind = "list";
	switch (widget_kind_of(id)) {
		case kKindCombo: kind = "combo"; break;
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

// ---- the popup and the interaction gate -------------------------------------

bool MenuRuntime::drawn_(int id) const {
	for (int w = id; w >= 0; w = parent_window_(w))
		if (!is_widget_shown(w)) return false;
	return true;
}

void MenuRuntime::sync_popup_() {
	// A drawn MODAL window of the generic draw claims the popup, the last in draw
	// order winning [orig: CUIElement_Draw @ 0x64a8a0 — shown, MODAL (+0x294) and
	// not already the popup: it becomes g_UIOpenPopupWnd; the generic window,
	// GLB_TABLE and GOPHER draw through it]. The claim stays when the window stops
	// drawing; an action or a screen change closes it.
	for (int id : id_of_index_) {
		const mnu::Window *w = index_.window(id);
		if (w == nullptr || !w->modal) continue;
		const int kind = widget_kind_of(id);
		if (kind != kKindWindow && kind != kKindGlbTable && kind != kKindGopher) continue;
		if (drawn_(id)) open_popup_id_ = id;
	}
	if (frame_ == nullptr) return;
	const int index = open_popup_id_ >= 0 ? frame_index(open_popup_id_) : -1;
	if (index != frame_popup_index_) {
		frame_popup_index_ = index;
		frame_->set_open_popup(index);
	}
}

void MenuRuntime::set_popup_(int id) {
	open_popup_id_ = id;
	sync_popup_();
}

// [orig: CWnd_IsVisibleInHierarchy @ 0x646290 — shown, then while enabled (and,
// with a popup open, while there is a parent): the popup itself passes, a root
// passes, a hidden parent fails. So with a popup open only the popup and its
// descendants pass; without one the widget and every ancestor must be shown and
// enabled]
bool MenuRuntime::visible_in_hierarchy_(int id) const {
	if (id < 0 || !is_widget_shown(id)) return false;
	int w = id;
	while (!is_widget_disabled(w)) {
		const int parent = parent_window_(w);
		if (open_popup_id_ >= 0 && parent < 0) return false;
		if (w == open_popup_id_) return true;
		if (parent < 0) return true;
		w = parent;
		if (!is_widget_shown(w)) return false;
	}
	return false;
}

// ---- input: mouse -----------------------------------------------------------

void MenuRuntime::process_mouse(float x, float y, bool button_down, uint32_t now_ms) {
	if (frame_ == nullptr || !frame_->is_configured()) return;
	// The sample's button edge is its message, ahead of the pump.
	if (button_down && !mouse_down_) {
		press_mouse(x, y, now_ms);
		if (frame_ == nullptr) return;
	} else if (!button_down && mouse_down_) {
		release_mouse(x, y);
	}
	move_mouse(x, y, button_down);
	pump_mouse();
}

void MenuRuntime::move_mouse(float x, float y, bool button_down) {
	mouse_x_ = x;
	mouse_y_ = y;
	mouse_down_ = button_down;
	mouse_seen_ = true;
}

bool MenuRuntime::press_mouse(float x, float y, uint32_t now_ms) {
	if (frame_ == nullptr || !frame_->is_configured()) return false;
	sync_popup_();
	move_mouse(x, y, true);
	if (!dropdown_press_(x, y)) press_reach_(x, y, now_ms);
	return true;
}

bool MenuRuntime::release_mouse(float x, float y) {
	if (frame_ == nullptr || !frame_->is_configured()) return false;
	move_mouse(x, y, false);
	frame_->release_mouse();
	return true;
}

bool MenuRuntime::dropdown_press_(float x, float y) {
	if (open_combo_id_ < 0) return false;
	const int combo_index = frame_index(open_combo_id_);
	if (combo_index < 0) {
		open_combo_id_ = -1;
		return false;
	}
	// The dropdown takes the press ahead of every other window [orig:
	// CWnd_DispatchMouseEventToChildren @ 0x647917, g_UIActiveComboWnd's sink first]: its
	// scrollbar's windows first [orig: CListWnd child walk @ 0x643f30, the scrollbar child claims
	// first; parts = CScrollWnd_HandleEvent @ 0x64d050], then a row picks [orig: list_wnd_on_command
	// @ 0x643cb0, the LISTBOX_WND 0x5000001 pick, CComboWnd_HandleEvent @ 0x65c2fd], and a press
	// outside the cell and the list closes it [orig: @ 0x65c261..0x65c2bc, the outside check
	// @ 0x65c290]; a press on the input-dead closed cell does nothing. The press is consumed either
	// way (D-MNU-11). A value its scrollbar changes arrives through on_frame_scroll_value like the
	// main pump's.
	if (frame_->press_popup_mouse(combo_index, x, y)) {
		frame_->set_widget_hover_item(combo_index, -1);
		return true;
	}
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
	return true;
}

void MenuRuntime::pump_mouse() {
	if (frame_ == nullptr || !frame_->is_configured() || !mouse_seen_) return;
	sync_popup_();
	const float x = mouse_x_;
	const float y = mouse_y_;
	const bool button_down = mouse_down_;

	// An open dropdown owns the mouse exclusively [orig: UI_DispatchMouseEvent
	// @0x63ab00 g_UIOpenPopupWnd gate; CComboWnd_HandleEvent @0x65c190; D-MNU-11/12]:
	// its press is press_mouse's. No code flags the combo's list as the popup, so which
	// retail pump serves it is open (docs/mnu/menu-re.md, "Not ported, or open"); this
	// pump stays dropdown-exclusive.
	if (open_combo_id_ >= 0) {
		const int combo_index = frame_index(open_combo_id_);
		if (combo_index < 0) {
			open_combo_id_ = -1;
		} else {
			frame_->set_cursor_state(false, x, y);
			// The window its press holds takes the sample (its scroll_row changes arrive
			// through on_frame_scroll_value like the main pump's).
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
			return;
		}
	}

	// A click inside the frame's pump (on_widget_clicked) may show another screen or open another
	// document: the claim then indexes the screen it left, and the next sample plays its sounds.
	const uint32_t generation = open_generation_;
	const std::string screen = current_screen_;
	const int claim = frame_->process_mouse(x, y, button_down);
	if (frame_ == nullptr) return;
	frame_->set_cursor_state(false, x, y);
	if (open_generation_ == generation && current_screen_ == screen) sample_sounds_(claim, button_down);
	if (claim != last_claim_) {
		on_claim_changed_(last_claim_, claim);
		last_claim_ = claim;
	}
	if (frame_ != nullptr) frame_->apply_claim_cursor();
}

void MenuRuntime::press_reach_(float x, float y, uint32_t now_ms) {
	// Its message reaches the windows under the mouse ahead of the frame's pump [orig:
	// Input_DispatchMouseEvent -> UI_DispatchMouseEvent @ 0x63ab00 on WM_LBUTTONDOWN, the windows
	// walked by CWnd_DispatchMouseEventToChildren @ 0x647900]: the frame runs the scrollbar windows'
	// own (a value they change arrives through on_frame_scroll_value), the widgets' are ours, front to
	// back (MenuFrameCompiler::press_reach, D-MNU-33). One may show another screen.
	const uint32_t pressed_on = open_generation_;
	const std::string pressed_screen = current_screen_;
	const std::vector<MenuPumpWindow> reach = frame_->press_mouse(x, y);
	for (size_t i = 0; i < reach.size(); ++i) {
		if (reach[i].part != 0) continue; // a spin arrow, a scrollbar's window: the frame's
		const bool again = std::find(reach.begin(), reach.begin() + static_cast<std::ptrdiff_t>(i),
								   reach[i]) != reach.begin() + static_cast<std::ptrdiff_t>(i);
		press_(reach[i].index, x, y, now_ms, again);
		if (frame_ == nullptr || open_generation_ != pressed_on || current_screen_ != pressed_screen)
			break;
	}
}

bool MenuRuntime::process_wheel(float x, float y, int steps) {
	if (frame_ == nullptr || !frame_->is_configured()) return false;
	sync_popup_();
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
	// The claim's edges for the embedder's observers; the sounds are the pump's
	// (sample_sounds_).
	if (previous >= 0) {
		const int prev_id = id_at_index(previous);
		if (prev_id >= 0) {
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
			MenuEvent e;
			e.kind = MenuEvent::Kind::HoverChanged;
			e.id = id;
			e.flag = true;
			emit_(e);
		}
	}
}

// A widget's own press handler, one window of the press's reach (shown and enabled at every
// level, inside the open popup while one is open) [orig: CWnd_DispatchMouseEventToChildren
// @ 0x647900 checks +0xE0 and +0xE4 at every level, then the window's own handler @ 0x6479f4].
void MenuRuntime::press_(int index, float x, float y, uint32_t now_ms, bool again) {
	const int id = id_at_index(index);
	if (id < 0 || !visible_in_hierarchy_(id)) return;
	const uint32_t generation = open_generation_;
	switch (widget_kind_of(id)) {
		case kKindEdit:
			// [orig: CEditWnd_HandleInputEvent @ 0x661510 — 0x1000002 focuses
			// unless READONLY (+0x308)]
			focus_edit(id);
			break;
		case kKindList:
		case kKindLanList: {
			// [orig: list_wnd_on_command @ 0x643cb0 — the activation first, then
			// the row under the point]
			activate(id);
			if (open_generation_ != generation || frame_ == nullptr) return;
			const int row = frame_index(id) == index ? frame_->list_row_at(index, x, y) : -1;
			if (row >= 0) list_press_(id, row, now_ms, again);
			break;
		}
		case kKindTable: {
			// [orig: CTableWnd_HandleNamedEvent @ 0x642400 — the activation, then
			// table_hit_test @ 0x63fe90: a row selects when 0 <= row < count; the
			// header strip sorts (not ported: docs/mnu/menu-re.md "Table input")]
			activate(id);
			if (open_generation_ != generation || frame_ == nullptr || frame_index(id) != index) return;
			int row = -1;
			int column = -1;
			if (frame_->table_hit(index, x, y, &row, &column) && row >= 0 &&
					row < table_row_count(id))
				table_press_(id, row, column, now_ms, again);
			break;
		}
		case kKindSpinList:
			// An arrow is a button of its own: it acts on its click. A press on the
			// list itself activates it and steps to the next value [orig:
			// CSpinListWnd_HandleEvent @ 0x64c370 on 0x1000002 / 0x1000004].
			if (frame_->spin_arrow_at(index, x, y) != 0) break;
			activate(id);
			if (open_generation_ != generation) return;
			spin_cycle(id, 1);
			break;
		default:
			break;
	}
}

void MenuRuntime::on_widget_clicked(int index, int part) {
	const int id = id_at_index(index);
	if (id < 0 || frame_ == nullptr || !visible_in_hierarchy_(id)) return;
	if ((part == 1 || part == 2) && widget_kind_of(id) == kKindSpinList) {
		arrow_click_(id, part);
		return;
	}
	// The pump's click plays the SELECTED sound, then the click event [orig:
	// CWnd_ProcessMouseEvent @ 0x647a00 — the state-3 sound, then vtable+28
	// with 0x3000001]: its sound state let go, so the next sample with the
	// mouse still there plays MOUSEIN again (menu_sound.h).
	sound_pump_.click(uint64_t(id));
	play_widget_state_sound(id, "SELECTED");
	activate(id);
}

void MenuRuntime::arrow_click_(int id, int arrow) {
	const mnu::Window *w = index_.window(id);
	if (w == nullptr) return;
	const mnu::WindowPart &part = arrow == 1 ? w->spinup : w->spindown;
	if (!part.present() || part->disabled) return;
	const uint32_t generation = open_generation_;
	pending_requests_.clear();
	// The arrow's own pump and rows (its NAME is SPINLISTWND_UP / _DOWN [orig:
	// CSpinListWnd_CreateUpDownChildren @ 0x64b8b0]), then the spin list's step,
	// then the cross-file requests its rows made.
	play_sound_(id, *part, "SELECTED");
	if (!walk_rows_(*part, id, true, generation)) return;
	spin_cycle(id, arrow == 1 ? 1 : -1);
	if (open_generation_ != generation) return;
	raise_pending_requests_();
}

// The double-click latch both row owners share: true on the second press of
// the same row inside the window (which then re-arms from scratch) — the
// press retail gets as WM_LBUTTONDBLCLK (0x1000004).
bool MenuRuntime::register_click_(int id, int row, uint32_t now_ms) {
	const bool is_double = id == last_click_id_ && row == last_click_row_ &&
			now_ms - last_click_ms_ <= kDoubleClickMs;
	last_click_id_ = id;
	last_click_row_ = row;
	last_click_ms_ = is_double ? 0 : now_ms;
	last_click_double_ = is_double;
	return is_double;
}

// [orig: list_wnd_on_command @ 0x643cb0 — an ITEMS MULTISELECT list (+0x318)
// toggles the row's state between 3 and 0, a single-select list selects it alone;
// then the selection event, 0x5000002 on a double click]
void MenuRuntime::list_press_(int id, int row, uint32_t now_ms, bool again) {
	const bool is_double = again ? last_click_double_ : register_click_(id, row, now_ms);
	const mnu::Window *w = index_.window(id);
	int pick = row;
	if (w != nullptr && w->items.multiselect) {
		std::vector<int> selected = selected_set(id);
		toggle_row(selected, row);
		set_selected_set(id, selected);
		// Retail keeps only the per-row states: the list's selected row is the
		// first row in state 3 by index, none once the toggle emptied the set
		// [orig: UIList_GetSelectedValue @ 0x644660], so a row the click took out
		// neither draws nor reads as selected.
		const auto first = std::min_element(selected.begin(), selected.end());
		pick = first != selected.end() ? *first : -1;
	}
	select_row(id, pick, true); // emits the "list" value change
	if (is_double) {
		MenuEvent e;
		e.kind = MenuEvent::Kind::ListActivated;
		e.id = id;
		e.value = row;
		emit_(e);
	}
}

// The table's own press on a data row [orig: CTableWnd_HandleNamedEvent
// @ 0x642400 — the L-down / double-click arm @0x6424d4..0x6426d4]: a locked
// row takes nothing; the selection write (MULTISELECT toggles the row, a
// single-select table selects it alone), then the cell event 0x8000001 the
// registered handlers read (TableCellClicked: the row's new state and the
// column's cell value) and 0x5000002 on a double click. The header's click
// sorts the columns in retail (CTableWnd_SortByColumn), which is not ported
// here (docs/mnu/menu-re.md "Table input").
void MenuRuntime::table_press_(int id, int row, int column, uint32_t now_ms, bool again) {
	const bool is_double = again ? last_click_double_ : register_click_(id, row, now_ms);
	MenuWidgetRuntimeState &state = state_of_(id);
	if (!menu::table_click_select(state.table_rows, row, table_multiselect_(id))) return;
	push_table_rows_(id);
	MenuEvent cell;
	cell.kind = MenuEvent::Kind::TableCellClicked;
	cell.id = id;
	cell.text = widget_name_of(id);
	cell.value = row;
	cell.column = column;
	cell.state = table_row_state(id, row);
	cell.cell_value = column >= 0 ? table_cell_value(id, row, column) : 0;
	cell.flag = is_double;
	emit_(cell);
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

// ---- focus + keyboard --------------------------------------------------------

void MenuRuntime::focus_edit(int id) {
	const mnu::Window *w = index_.window(id);
	if (w == nullptr || w->readonly) return;
	set_focus_(id);
}

void MenuRuntime::set_focus_(int id) {
	if (focus_id_ == id) return;
	drop_focus_();
	focus_id_ = id;
	const int index = frame_index(id);
	if (index >= 0) {
		frame_->set_widget_focused(index, true);
		// An edit's caret rides characters (code points), not bytes.
		if (widget_kind_of(id) == kKindEdit && frame_->get_widget_caret(index) < 0)
			frame_->set_widget_caret(index, static_cast<int>(strutil::utf8_length(get_widget_text(id))));
	}
}

void MenuRuntime::drop_focus_() {
	if (focus_id_ < 0) return;
	const int id = focus_id_;
	focus_id_ = -1;
	const int index = frame_index(id);
	if (index >= 0) {
		frame_->set_widget_focused(index, false);
		// The edit keeps its text: the store carries it to the next compile.
		if (widget_kind_of(id) == kKindEdit) remember_widget_text(id, frame_->get_widget_text(index));
	}
}

// Enter in an edit [orig: CEditWnd_HandleKeyEvent @ 0x6623a0 case 0xD — the
// focus and the capture clear, the commit event 0x7000002, then the VK_RETURN
// scan with no focus gate (dispatch_key_event @ 0x63ac30, @ 0x6624ef)]. The
// keydown then still runs the edit's GLB filter rows [orig:
// CEditWnd_HandleInputEvent @ 0x661510 falls from the key handler into the
// filter walk], and the scan's winner only clicks on the next pump [orig:
// CWnd_SetHotkeyPressed @ 0x646440], so the rows come before that click, on the
// screen the commit left; the runtime clicks at once, so it runs them first.
void MenuRuntime::commit_edit_(int id) {
	const uint32_t generation = open_generation_;
	drop_focus_();
	emit_edit_changed(id);
	if (open_generation_ != generation) return;
	// The edit's own callback takes the commit event 0x7000002 (the embedder's
	// EditCommitted) [orig: CEditWnd_HandleKeyEvent @0x6623a0 — g_UIFocusWnd = 0
	// @0x66249f, the widget event 0x7000002 @0x6624e3].
	MenuEvent committed;
	committed.kind = MenuEvent::Kind::EditCommitted;
	committed.id = id;
	committed.text = widget_name_of(id);
	emit_(committed);
	if (open_generation_ != generation) return;
	edit_filter_rows_(id);
	if (open_generation_ != generation) return;
	scan_hotkeys_(true, 13);
}

bool MenuRuntime::handle_key(const MenuKeyInput &key) {
	if (frame_ == nullptr || !frame_->is_configured()) return false;
	sync_popup_();
	const uint32_t generation = open_generation_;
	bool consumed = false;
	// WM_KEYDOWN: the virtual rows scan while nothing has the focus, then the key
	// goes to the focused widget once per root window.
	if (key.vk != 0) {
		if (focus_id_ < 0 && scan_hotkeys_(true, key.vk)) consumed = true;
		if (open_generation_ != generation) return true;
		const size_t roots = current_root_count_();
		for (size_t r = 0; r < roots && focus_id_ >= 0; ++r) {
			consumed = true;
			key_down_to_focus_(key);
			if (open_generation_ != generation) return true;
		}
	}
	// WM_CHAR: the character rows the same way (a control character types
	// nothing and no row carries one).
	int ch = key.unicode;
	if (ch == 0 && key.printable_keycode >= 0x20 && key.printable_keycode <= 0x7E) ch = key.printable_keycode;
	if (ch >= 0x20 && ch != 0x7F) {
		if (focus_id_ < 0 && scan_hotkeys_(false, ch)) consumed = true;
		if (open_generation_ != generation) return true;
		const size_t roots = current_root_count_();
		for (size_t r = 0; r < roots && focus_id_ >= 0; ++r) {
			consumed = true;
			char_to_focus_(key.unicode);
			if (open_generation_ != generation) return true;
		}
	}
	return consumed;
}

size_t MenuRuntime::current_root_count_() const {
	const MenuDocIndex::Node *screen = index_.node(current_screen_id());
	return screen != nullptr ? screen->child_ids.size() : 0;
}

// The focused widget's keydown (0x2000001). An edit runs its key handler, then its
// GLB_FILTER rows; every class then runs its TAB rows on Tab [orig:
// CEditWnd_HandleInputEvent @ 0x661510 -> vtable+96 =
// CEditWnd_HandleKeyEvent @ 0x6623a0, the filter rows, then
// CUIWidget_HandleScriptedAction @ 0x6497f0's keydown arm @ 0x649c17].
void MenuRuntime::key_down_to_focus_(const MenuKeyInput &key) {
	const int id = focus_id_;
	const int index = frame_index(id);
	if (index < 0) {
		focus_id_ = -1;
		return;
	}
	if (widget_kind_of(id) == kKindEdit) {
		switch (key.vk) {
			case kEditKeyBackspace:
			case kEditKeyEnter:
			case kEditKeyEnd:
			case kEditKeyHome:
			case kEditKeyLeft:
			case kEditKeyRight:
			case kEditKeyDelete: {
				const int result = frame_->edit_key(index, key.vk, key.shift);
				if (result == static_cast<int>(EditKeyResult::kCommit)) {
					// The commit runs the filter rows itself, ahead of the
					// VK_RETURN click; Enter is not Tab.
					commit_edit_(id);
					return;
				}
				if (result == static_cast<int>(EditKeyResult::kChanged)) emit_edit_changed(id);
				break;
			}
			default: break;
		}
		edit_filter_rows_(id);
	}
	if (key.vk == 9) tab_rows_(id);
}

// The focused widget's typed character (0x2000002): an edit inserts a character
// that is not a control character, then runs its GLB_FILTER rows [orig:
// CEditWnd_HandleInputEvent @ 0x661510 — iscntrl, vtable+88].
void MenuRuntime::char_to_focus_(int unicode) {
	const int id = focus_id_;
	const int index = frame_index(id);
	if (index < 0) {
		focus_id_ = -1;
		return;
	}
	if (widget_kind_of(id) != kKindEdit || unicode < 0x20) return;
	if (frame_->edit_char(index, unicode)) emit_edit_changed(id);
	edit_filter_rows_(id);
}

// [orig: CEditWnd_HandleInputEvent @ 0x661510 — each GLB_FILTER row sends
// event 4 {atol(FIELD), the text} and each GLB_FILTER_NUM row event 5
// {atol(FIELD), atol(the text), TEST} to the current screen's control its text
// names; the rows run last-authored first]
void MenuRuntime::edit_filter_rows_(int id) {
	const mnu::Window *w = index_.window(id);
	if (w == nullptr) return;
	const std::vector<mnu::Action> rows = w->actions;
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		const int code = action_code(it->type);
		if (code != kActionGlbFilter && code != kActionGlbFilterNum) continue;
		const int receiver = find_control(std::string(), it->target);
		if (receiver < 0) continue;
		MenuEvent e;
		e.kind = MenuEvent::Kind::FilterRequested;
		e.id = receiver;
		e.value = static_cast<int>(std::strtol(it->field.c_str(), nullptr, 10));
		e.text = get_widget_text(id);
		e.flag = code == kActionGlbFilterNum;
		e.text2 = it->test.empty() ? std::string("LT") : it->test;
		emit_(e);
	}
}

// [orig: CUIWidget_HandleScriptedAction @ 0x6497f0 keydown with key 9 — each TAB
// row sets g_UIFocusWnd to the current screen's control its text names, with no
// type or visibility check; the rows run last-authored first, so the first
// authored decides]
void MenuRuntime::tab_rows_(int id) {
	const mnu::Window *w = index_.window(id);
	if (w == nullptr || w->name.empty()) return;
	const std::vector<mnu::Action> rows = w->actions;
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		if (action_code(it->type) != kActionTab) continue;
		const int target = find_control(std::string(), it->target);
		if (target >= 0) set_focus_(target);
	}
}

// ---- hotkeys ------------------------------------------------------------------

// The screen's hotkey table [orig: parse_script_block @ 0x63b800 calls
// CWnd_RegisterHotkeysRecursive @ 0x649d90 (vtable+40) on every root in document
// order: a window's own rows, then its children's, pre-order]. A window's rows:
// its HOTKEYs in document order (VIRTUAL by the name, else the text's first
// character), then its label's {hot} mnemonic [orig: CButtonWnd_SetLabel
// @ 0x6572F0 — the byte after the marker, a signed char]. A row whose key is 0 is
// skipped and a (key, widget) pair registers once [orig:
// scene_add_widget_event_callback @ 0x63a8e0].
void MenuRuntime::build_hotkeys_() {
	hotkeys_.clear();
	const mnu::Document *doc = index_.document();
	const bool utf8 = doc != nullptr && doc->source_encoding != mnu::SourceEncoding::CodePage;
	for (size_t i = 0; i < id_of_index_.size(); ++i) {
		const int id = id_of_index_[i];
		const mnu::Window *w = index_.window(id);
		if (w == nullptr) continue;
		const auto add = [this, id](bool virtual_key, int key) {
			if (key == 0) return;
			for (const MenuHotkeyRow &row : hotkeys_)
				if (row.key == key && row.id == id) return;
			hotkeys_.push_back(MenuHotkeyRow{ virtual_key, key, id });
		};
		for (const mnu::Hotkey &hk : w->hotkeys)
			add(hk.virtual_key, hk.virtual_key ? virtual_key_code(hk.value) : first_character(hk.value, utf8));
		if (frame_ != nullptr) {
			const std::string mnemonic = frame_->widget_mnemonic(static_cast<int>(i));
			if (!mnemonic.empty()) add(false, static_cast<int>(static_cast<signed char>(mnemonic[0])));
		}
	}
}

// The scan [orig: UI_DispatchKeyboardEventToChildren @ 0x63ad10 — WM_KEYDOWN
// matches the virtual rows by key, WM_CHAR the character rows (@ 0x63ad63) by
// tolower on both sides (@ 0x63ad78 / @ 0x63ad84); the first row whose widget is
// visible in the hierarchy wins (the CWnd_IsVisibleInHierarchy gate @ 0x63ad90), a
// row that is not is skipped]. The winner is pressed: its next pump clicks it [orig:
// CWnd_SetHotkeyPressed @ 0x646440; CWnd_ProcessMouseEvent @ 0x647a00 takes
// +0xF4 as a click]: the SELECTED sound and the click event, here at once.
bool MenuRuntime::scan_hotkeys_(bool virtual_key, int key) {
	int winner = -1;
	for (const MenuHotkeyRow &row : hotkeys_) {
		if (row.virtual_key != virtual_key) continue;
		if (virtual_key ? row.key != key : fold_key(row.key) != fold_key(key)) continue;
		if (!visible_in_hierarchy_(row.id)) continue;
		winner = row.id;
		break;
	}
	if (winner < 0) return false;
	sound_pump_.click(uint64_t(winner));
	play_widget_state_sound(winner, "SELECTED");
	activate(winner);
	return true;
}

} // namespace opennova::menu
