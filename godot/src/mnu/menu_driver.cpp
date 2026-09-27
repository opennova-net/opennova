#include "mnu/menu_driver.h"

#include "audio/music_director.h"
#include "cbin/cbin_credits_resource.h"
#include "cbin/credits_player.h"
#include "mnu/menu_audio.h"
#include "mnu/controls_model.h"
#include "mission/mission_catalog.h"
#include "mnu/menu_frame.h"
#include "object/weapon_database.h"
#include "object/avatar_database.h"
#include "object/avatar_records.h"
#include "mnu/mns_stylesheet.h"
#include "mnu/mnu_document.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <runtime/menu/loadout_screen.h>

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable.hpp>

using opennova::to_gd;
using opennova::to_std;

namespace godot {

namespace {

opennova::menu::LoadoutAmmoCounts loadout_counts(const Dictionary &p_values) {
	opennova::menu::LoadoutAmmoCounts out;
	const Array keys = p_values.keys();
	for (int i = 0; i < keys.size(); ++i) out[int(keys[i])] = int(p_values[keys[i]]);
	return out;
}

std::vector<int> loadout_indices(const TypedArray<WeaponDef> &p_rows) {
	std::vector<int> indices;
	for (int i = 0; i < p_rows.size(); ++i) {
		const Ref<WeaponDef> row = p_rows[i];
		indices.push_back(row.is_valid() ? row->get_index() : -1);
	}
	return indices;
}

opennova::menu::LoadoutParentIndices loadout_parents(const TypedArray<WeaponDef> &p_rows) {
	opennova::menu::LoadoutParentIndices out{-1, -1, -1};
	const auto indices = loadout_indices(p_rows);
	for (size_t i = 0; i < out.size() && i < indices.size(); ++i) out[i] = indices[i];
	return out;
}

TypedArray<WeaponDef> loadout_defs(const Ref<WeaponDatabase> &p_weapons, const std::vector<int> &p_indices) {
	TypedArray<WeaponDef> out;
	for (int index : p_indices) out.push_back(p_weapons->get_weapon(index));
	return out;
}

std::vector<opennova::menu::MissionChoice> mission_choices(const TypedArray<MissionCatalogRow> &rows) {
	std::vector<opennova::menu::MissionChoice> choices;
	for (int i = 0; i < rows.size(); ++i) {
		const Ref<MissionCatalogRow> row = rows[i];
		if (row.is_valid()) choices.push_back({to_std(row->get_file()), to_std(row->display_text()),
				to_std(row->get_briefing()), static_cast<uint32_t>(row->get_game_type())});
	}
	return choices;
}

std::vector<std::string> to_std_strings(const PackedStringArray &p_values) {
	std::vector<std::string> out;
	out.reserve(static_cast<size_t>(p_values.size()));
	for (int i = 0; i < p_values.size(); ++i) out.push_back(to_std(p_values[i]));
	return out;
}

PackedStringArray to_gd_strings(const std::vector<std::string> &p_values) {
	PackedStringArray out;
	for (const std::string &value : p_values) out.push_back(to_gd(value));
	return out;
}

PackedInt32Array to_gd_ints(const std::vector<int> &p_values) {
	PackedInt32Array out;
	for (int value : p_values) out.push_back(value);
	return out;
}

} // namespace

// ---- MenuScrollRange ---------------------------------------------------------

void MenuScrollRange::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_minimum"), &MenuScrollRange::get_minimum);
	ClassDB::bind_method(D_METHOD("get_maximum"), &MenuScrollRange::get_maximum);
	ClassDB::bind_method(D_METHOD("get_page"), &MenuScrollRange::get_page);
	ClassDB::bind_method(D_METHOD("get_value"), &MenuScrollRange::get_value);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "minimum"), "", "get_minimum");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "maximum"), "", "get_maximum");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "page"), "", "get_page");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "value"), "", "get_value");
}

// ---- the frame seam over the MenuFrame node ----------------------------------

class MenuDriver::FrameSeam : public opennova::menu::MenuFrameSeam {
	MenuDriver *owner_;

	MenuFrame *frame() const { return owner_->frame_(); }

public:
	explicit FrameSeam(MenuDriver *p_owner) : owner_(p_owner) {}

	bool is_configured() const override {
		MenuFrame *f = frame();
		return f != nullptr && f->is_configured();
	}
	void configure_screen(const std::string &screen) override {
		if (MenuFrame *f = frame()) {
			Dictionary text_lookup;
			owner_->fill_screen_text_lookup_(text_lookup);
			f->configure(owner_->doc_, to_gd(screen), owner_->root_, owner_->style_, text_lookup);
		}
	}
	void screen_configured() override {
		owner_->seed_marquee_widgets_();
		reset_cursor();
	}
	void set_widget_shown_override(int index, bool shown) override {
		if (MenuFrame *f = frame()) f->set_widget_shown_override(index, shown);
	}
	void set_widget_disabled(int index, bool disabled) override {
		if (MenuFrame *f = frame()) f->set_widget_disabled(index, disabled);
	}
	void set_widget_checked(int index, bool checked) override {
		if (MenuFrame *f = frame()) f->set_widget_checked(index, checked);
	}
	void set_widget_text(int index, const std::string &text) override {
		if (MenuFrame *f = frame()) f->set_widget_text(index, to_gd(text));
	}
	void set_widget_items(int index, const std::vector<std::string> &items) override {
		if (MenuFrame *f = frame()) f->set_widget_items(index, to_gd_strings(items));
	}
	void set_widget_selection(int index, int selected, int hover, int scroll_row) override {
		if (MenuFrame *f = frame()) f->set_widget_selection(index, selected, hover, scroll_row);
	}
	void set_widget_scroll_range(int index, int minimum, int maximum, int page,
			int value) override {
		if (MenuFrame *f = frame()) f->set_widget_scroll_range(index, minimum, maximum, page, value);
	}
	void set_widget_selected_set(int index, const std::vector<int> &rows) override {
		if (MenuFrame *f = frame()) f->set_widget_selected_set(index, to_gd_ints(rows));
	}
	void set_widget_table_rows(int index,
			const std::vector<std::vector<std::string>> &rows) override {
		MenuFrame *f = frame();
		if (f == nullptr) return;
		TypedArray<PackedStringArray> out;
		for (const std::vector<std::string> &row : rows) out.push_back(to_gd_strings(row));
		f->set_widget_table_rows(index, out);
	}
	void set_widget_hover_item(int index, int row) override {
		if (MenuFrame *f = frame()) f->set_widget_hover_item(index, row);
	}
	void set_widget_popup_open(int index, bool open) override {
		if (MenuFrame *f = frame()) f->set_widget_popup_open(index, open);
	}
	void set_widget_focused(int index, bool focused) override {
		if (MenuFrame *f = frame()) f->set_widget_focused(index, focused);
	}
	void set_widget_caret(int index, int caret) override {
		if (MenuFrame *f = frame()) f->set_widget_caret(index, caret);
	}
	int get_widget_caret(int index) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->get_widget_caret(index) : -1;
	}
	std::string get_widget_text(int index) const override {
		MenuFrame *f = frame();
		return f != nullptr ? to_std(f->get_widget_text(index)) : std::string();
	}
	int item_count(int index) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->item_count(index) : 0;
	}
	bool is_widget_disabled(int index) const override {
		MenuFrame *f = frame();
		return f != nullptr && f->is_widget_disabled(index);
	}
	opennova::menu::MenuRectF widget_rect(int index) const override {
		opennova::menu::MenuRectF out;
		if (MenuFrame *f = frame()) {
			const Rect2 rect = f->widget_rect(index);
			out.x = rect.position.x;
			out.y = rect.position.y;
			out.w = rect.size.x;
			out.h = rect.size.y;
		}
		return out;
	}
	void design_scale(float &sx, float &sy) const override {
		const Vector2 scale = owner_->design_scale();
		sx = scale.x;
		sy = scale.y;
	}
	int process_mouse(float x, float y, bool button_down) override {
		MenuFrame *f = frame();
		return f != nullptr ? f->process_mouse(Vector2(x, y), button_down) : -1;
	}
	bool process_popup_mouse(int index, float x, float y, bool button_down) override {
		MenuFrame *f = frame();
		return f != nullptr && f->process_popup_mouse(index, Vector2(x, y), button_down);
	}
	bool process_mouse_wheel(float x, float y, int steps) override {
		MenuFrame *f = frame();
		return f != nullptr && f->process_mouse_wheel(Vector2(x, y), steps);
	}
	void set_cursor_state(bool visible, float x, float y) override {
		if (MenuFrame *f = frame()) f->set_cursor_state(visible, Vector2(x, y));
	}
	void apply_claim_cursor() override {
		// The retail cursor rides the claim as the OS custom cursor — the ONE
		// live cursor (both drawn showed the compiled one trailing by a pump
		// frame; the frame's emit_cursor stays for surfaces without an OS cursor).
		if (MenuFrame *f = frame())
			Input::get_singleton()->set_custom_mouse_cursor(f->get_cursor_texture(),
					Input::CURSOR_ARROW);
	}
	void reset_cursor() override {
		Input::get_singleton()->set_custom_mouse_cursor(Ref<Resource>(), Input::CURSOR_ARROW);
	}
	int combo_popup_row_at(int index, float x, float y) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->combo_popup_row_at(index, Vector2(x, y)) : -1;
	}
	bool combo_popup_contains(int index, float x, float y) const override {
		MenuFrame *f = frame();
		return f != nullptr && f->combo_popup_contains(index, Vector2(x, y));
	}
	int list_row_at(int index, float x, float y) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->list_row_at(index, Vector2(x, y)) : -1;
	}
	int spin_arrow_at(int index, float x, float y) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->spin_arrow_at(index, Vector2(x, y)) : 0;
	}
	int table_row_at(int index, float x, float y) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->table_row_at(index, Vector2(x, y)) : -1;
	}
	int hotkey_widget(const std::string &key, bool virtual_key) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->hotkey_widget(to_gd(key), virtual_key) : -1;
	}
	bool edit_char(int index, int unicode) override {
		MenuFrame *f = frame();
		return f != nullptr && f->edit_char(index, unicode);
	}
	int edit_key(int index, int key, bool shift) override {
		MenuFrame *f = frame();
		return f != nullptr ? f->edit_key(index, key, shift) : 0;
	}
};

// ---- lifecycle -----------------------------------------------------------------

MenuDriver::MenuDriver() : seam_(std::make_unique<FrameSeam>(this)) {
	runtime_.set_sink([this](const opennova::menu::MenuEvent &p_event) {
		on_runtime_event_(p_event);
	});
}

MenuDriver::~MenuDriver() = default;

MenuFrame *MenuDriver::frame_() const {
	return frame_id_.is_null() ? nullptr
							   : Object::cast_to<MenuFrame>(ObjectDB::get_instance(frame_id_));
}

MenuAudio *MenuDriver::audio_() const {
	return audio_id_.is_null() ? nullptr
							   : Object::cast_to<MenuAudio>(ObjectDB::get_instance(audio_id_));
}

MusicDirector *MenuDriver::music_director_() const {
	return music_director_id_.is_null()
			? nullptr
			: Object::cast_to<MusicDirector>(ObjectDB::get_instance(music_director_id_));
}

void MenuDriver::attach(MenuFrame *p_frame, MenuAudio *p_audio) {
	frame_id_ = p_frame != nullptr ? ObjectID(p_frame->get_instance_id()) : ObjectID();
	audio_id_ = p_audio != nullptr ? ObjectID(p_audio->get_instance_id()) : ObjectID();
	runtime_.set_frame(p_frame != nullptr ? seam_.get() : nullptr);
	if (p_frame == nullptr) return;
	const Callable clicked(this, "_on_frame_widget_clicked");
	if (!p_frame->is_connected("widget_clicked", clicked))
		p_frame->connect("widget_clicked", clicked);
	const Callable scrolled(this, "_on_frame_scroll_value");
	if (!p_frame->is_connected("scroll_value_changed", scrolled))
		p_frame->connect("scroll_value_changed", scrolled);
}

void MenuDriver::set_music_director(MusicDirector *p_director) {
	music_director_id_ =
			p_director != nullptr ? ObjectID(p_director->get_instance_id()) : ObjectID();
}

String MenuDriver::get_menu_file() const { return to_gd(runtime_.menu_file()); }
String MenuDriver::get_current_screen() const { return to_gd(runtime_.current_screen()); }

bool MenuDriver::open_document(const Ref<MnuDocument> &p_doc, const Ref<ResourceRoot> &p_root,
		const Ref<MnsStyleSheet> &p_style, const Ref<RtxtStringFile> &p_text,
		const String &p_menu_file, const String &p_target_screen) {
	doc_ = p_doc;
	root_ = p_root;
	style_ = p_style;
	text_ = p_text;
	text_rsrc_cache_.clear();
	return runtime_.open_document(doc_.is_valid() ? &doc_->get_native() : nullptr,
			to_std(p_menu_file), to_std(p_target_screen));
}

PackedStringArray MenuDriver::get_screen_names() const {
	return to_gd_strings(runtime_.screen_names());
}

bool MenuDriver::show_screen(const String &p_name) { return runtime_.show_screen(to_std(p_name)); }
bool MenuDriver::navigate_to_screen(const String &p_name) {
	return runtime_.navigate_to_screen(to_std(p_name));
}
bool MenuDriver::pop_screen() { return runtime_.pop_screen(); }

// The runtime's events, relayed synchronously: observers may re-enter the
// driver from a signal (a cross-.mnu jump swaps the document).
void MenuDriver::on_runtime_event_(const opennova::menu::MenuEvent &p_event) {
	using Kind = opennova::menu::MenuEvent::Kind;
	switch (p_event.kind) {
		case Kind::ScreenChanged:
			emit_signal("screen_changed", to_gd(p_event.text));
			break;
		case Kind::MusicVar:
			if (MusicDirector *director = music_director_())
				director->set_var(music_var_index_, p_event.value);
			break;
		case Kind::MenuRequested:
			emit_signal("menu_requested", to_gd(p_event.text), to_gd(p_event.text2));
			break;
		case Kind::QuitRequested:
			emit_signal("quit_requested");
			break;
		case Kind::UrlRequested:
			emit_signal("url_requested", to_gd(p_event.text));
			break;
		case Kind::Sound:
			play_widget_sound(to_gd(p_event.text2), to_gd(p_event.text));
			break;
		case Kind::ValueChanged:
			emit_signal("widget_value_changed", to_gd(p_event.text), to_gd(p_event.text2),
					p_event.value, to_gd(p_event.text3));
			break;
		case Kind::WidgetActivated:
			emit_signal("widget_activated", p_event.id, to_gd(p_event.text));
			break;
		case Kind::ListActivated:
			emit_signal("list_activated", p_event.id, p_event.value);
			break;
		case Kind::HoverChanged:
			emit_signal("widget_hover_changed", p_event.id, p_event.flag);
			break;
		case Kind::ShownChanged:
			sync_credits_();
			break;
	}
}

// The id->text table for String/Item type=="id" lookups: the screen's own
// TEXT_RSRC (loaded through the VFS, cached) wins, else the shell-provided
// text resource.
void MenuDriver::fill_screen_text_lookup_(Dictionary &r_out) {
	Ref<RtxtStringFile> table = text_;
	const opennova::mnu::Screen *screen = runtime_.index().screen(runtime_.current_screen_id());
	if (screen != nullptr && !screen->text_rsrc.empty()) {
		const Ref<RtxtStringFile> loaded = load_text_rsrc_(to_gd(screen->text_rsrc));
		if (loaded.is_valid()) table = loaded;
	}
	if (table.is_null()) return;
	// First-match-wins across sections (the engine-faithful flat lookup).
	for (int section = 0; section < table->get_section_count(); ++section) {
		const String section_name = table->get_section_name(section);
		const PackedStringArray keys = table->get_section_keys(section);
		for (int i = 0; i < keys.size(); ++i) {
			const String token = keys[i];
			if (!r_out.has(token))
				r_out[token] = table->get_string_in_section(section_name, token);
		}
	}
}

Ref<RtxtStringFile> MenuDriver::load_text_rsrc_(const String &p_file) {
	const String key = p_file.to_lower();
	if (const Ref<RtxtStringFile> *cached = text_rsrc_cache_.getptr(key)) return *cached;
	Ref<RtxtStringFile> loaded;
	if (root_.is_valid()) {
		const PackedByteArray bytes = root_->read_file(p_file.get_file());
		if (!bytes.is_empty()) {
			loaded.instantiate();
			if (loaded->load_from_byte_array(bytes) != OK) loaded.unref();
		}
	}
	text_rsrc_cache_[key] = loaded;
	return loaded;
}

// Marquee DATASOURCE routing: a marquee_wnd DATASOURCE is either a
// CBIN-encrypted credits config (ENV scroll settings + TEXT entries — routed
// to the dedicated CreditsPlayer scroller) or plain text fed to the compiled
// roll (the retail loader is CMarqueeWnd_LoadCreditsFromIni; the CBIN scroller
// is godot/src/cbin).
void MenuDriver::seed_marquee_widgets_() {
	clear_credits_();
	MenuFrame *frame = frame_();
	if (root_.is_null() || frame == nullptr) return;
	for (int id : runtime_.current_screen_ids()) {
		const opennova::mnu::Window *window = runtime_.index().window(id);
		if (window == nullptr || window->type != opennova::mnu::WindowType::Marquee ||
				window->datasource.empty())
			continue;
		const PackedByteArray bytes = root_->read_file(to_gd(window->datasource).get_file());
		if (bytes.is_empty()) continue;
		const Ref<CbinCreditsResource> credits = CbinCreditsResource::from_cbin_bytes(bytes);
		if (credits.is_valid()) {
			const Rect2 rect = widget_frame_rect(id);
			CreditsPlayer *player = memnew(CreditsPlayer);
			player->set_name("Credits");
			player->set_credits_resource(credits);
			player->set_autoplay(true);
			player->set_position(rect.position);
			player->set_size(rect.size);
			player->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			frame->add_child(player);
			credits_.push_back({ ObjectID(player->get_instance_id()), id });
			continue;
		}
		const String text = bytes.get_string_from_ascii();
		const int index = runtime_.frame_index(id);
		if (index >= 0 && !text.is_empty())
			frame->set_widget_marquee_lines(index, text.replace("\r\n", "\n").split("\n"));
	}
	sync_credits_();
}

void MenuDriver::clear_credits_() {
	for (const CreditsMount &mount : credits_) {
		if (CreditsPlayer *player =
						Object::cast_to<CreditsPlayer>(ObjectDB::get_instance(mount.player)))
			player->queue_free();
	}
	credits_.clear();
}

// Re-apply the draw walk's shown gate (widget + ancestors + overrides —
// MenuFrame.is_widget_shown) to every mounted overlay.
void MenuDriver::sync_credits_() {
	MenuFrame *frame = frame_();
	for (const CreditsMount &mount : credits_) {
		CreditsPlayer *player =
				Object::cast_to<CreditsPlayer>(ObjectDB::get_instance(mount.player));
		if (player == nullptr) continue;
		const int index = runtime_.frame_index(mount.id);
		player->set_visible(frame != nullptr && index >= 0 && frame->is_widget_shown(index));
	}
}

// ---- addressing / state --------------------------------------------------------

int MenuDriver::widget_id(const String &p_name) const { return runtime_.widget_id(to_std(p_name)); }
String MenuDriver::widget_name_of(int p_id) const { return to_gd(runtime_.widget_name_of(p_id)); }
int MenuDriver::widget_kind_of(int p_id) const { return runtime_.widget_kind_of(p_id); }
bool MenuDriver::has_widget(const String &p_name) const { return widget_id(p_name) >= 0; }
int MenuDriver::frame_index(int p_id) const { return runtime_.frame_index(p_id); }

Rect2 MenuDriver::widget_frame_rect(int p_id) const {
	const int index = frame_index(p_id);
	MenuFrame *frame = frame_();
	if (index < 0 || frame == nullptr) return Rect2();
	const Rect2 design = frame->widget_rect(index);
	const Vector2 scale = design_scale();
	return Rect2(design.position * scale, design.size * scale);
}

Vector2 MenuDriver::design_scale() const {
	// The fixed authoring design space (the witness lives at the engine home,
	// engine/runtime/menu menu_frame.h kMenuDesignWidth/Height).
	MenuFrame *frame = frame_();
	if (frame == nullptr) return Vector2(1.0f, 1.0f);
	const Vector2 size = frame->get_size();
	if (size.x > 1.0f && size.y > 1.0f)
		return Vector2(size.x / static_cast<float>(MenuFrame::DESIGN_WIDTH),
				size.y / static_cast<float>(MenuFrame::DESIGN_HEIGHT));
	return Vector2(1.0f, 1.0f);
}

void MenuDriver::set_widget_shown(int p_id, bool p_shown) { runtime_.set_widget_shown(p_id, p_shown); }
bool MenuDriver::is_widget_shown(int p_id) const { return runtime_.is_widget_shown(p_id); }
void MenuDriver::set_widget_disabled(int p_id, bool p_disabled) {
	runtime_.set_widget_disabled(p_id, p_disabled);
}
bool MenuDriver::is_widget_disabled(int p_id) const { return runtime_.is_widget_disabled(p_id); }
void MenuDriver::set_widget_checked(int p_id, bool p_checked) {
	runtime_.set_widget_checked(p_id, p_checked);
}
bool MenuDriver::is_widget_checked(int p_id) const { return runtime_.is_widget_checked(p_id); }
void MenuDriver::set_widget_text(int p_id, const String &p_text) {
	runtime_.set_widget_text(p_id, to_std(p_text));
}
String MenuDriver::get_widget_text(int p_id) const { return to_gd(runtime_.get_widget_text(p_id)); }
void MenuDriver::set_widget_items(int p_id, const PackedStringArray &p_items) {
	runtime_.set_widget_items(p_id, to_std_strings(p_items));
}
PackedStringArray MenuDriver::get_widget_items(int p_id) const {
	return to_gd_strings(runtime_.get_widget_items(p_id));
}
int MenuDriver::item_count(int p_id) const { return runtime_.item_count(p_id); }
String MenuDriver::item_text(int p_id, int p_row) const {
	return to_gd(runtime_.item_text(p_id, p_row));
}
String MenuDriver::item_value(int p_id, int p_row) const {
	return to_gd(runtime_.item_value(p_id, p_row));
}
TypedArray<MnuActionRow> MenuDriver::widget_actions(int p_id) const {
	return doc_.is_valid() ? doc_->get_widget_actions(p_id) : TypedArray<MnuActionRow>();
}
void MenuDriver::select_row_by_value(int p_id, const String &p_value, bool p_emit) {
	runtime_.select_row_by_value(p_id, to_std(p_value), p_emit);
}
void MenuDriver::select_row(int p_id, int p_row, bool p_emit) {
	runtime_.select_row(p_id, p_row, p_emit);
}
int MenuDriver::selected_row(int p_id) const { return runtime_.selected_row(p_id); }
void MenuDriver::set_widget_scroll_range(int p_id, int p_minimum, int p_maximum, int p_page,
		int p_value) {
	runtime_.set_widget_scroll_range(p_id, p_minimum, p_maximum, p_page, p_value);
}
Ref<MenuScrollRange> MenuDriver::get_widget_scroll_range(int p_id) const {
	opennova::menu::MenuScrollRangeState state;
	Ref<MenuScrollRange> out;
	if (!runtime_.get_widget_scroll_range(p_id, state)) return out;
	out.instantiate();
	out->assign(state);
	return out;
}

void MenuDriver::table_add_row(int p_id, const PackedStringArray &p_cells) {
	runtime_.table_add_row(p_id, to_std_strings(p_cells));
}
void MenuDriver::table_clear_rows(int p_id) { runtime_.table_clear_rows(p_id); }
int MenuDriver::table_row_count(int p_id) const { return runtime_.table_row_count(p_id); }
String MenuDriver::table_cell_text(int p_id, int p_row, int p_col) const {
	return to_gd(runtime_.table_cell_text(p_id, p_row, p_col));
}
void MenuDriver::table_select_row(int p_id, int p_row, bool p_additive) {
	runtime_.table_select_row(p_id, p_row, p_additive);
}

// ---- activation / actions --------------------------------------------------------

void MenuDriver::activate(int p_id) { runtime_.activate(p_id); }
void MenuDriver::spin_cycle(int p_id, int p_delta) { runtime_.spin_cycle(p_id, p_delta); }
String MenuDriver::spin_value_attr(int p_id) const { return item_value(p_id, selected_row(p_id)); }

bool MenuDriver::dispatch_action_row(const Ref<MnuActionRow> &p_action) {
	return p_action.is_valid() && runtime_.dispatch_action(p_action->native());
}

void MenuDriver::play_widget_sound(const String &p_trigger, const String &p_file) {
	// The widget sound edge as resolved from the SOUND table; observable
	// without banks, which the seam tests need.
	emit_signal("sound_requested", p_file, p_trigger);
	if (MenuAudio *audio = audio_()) audio->play_widget_sound(p_trigger, p_file);
}

// ---- input -------------------------------------------------------------------------

void MenuDriver::on_frame_widget_clicked_(int p_index) {
	runtime_.on_widget_clicked(p_index,
			static_cast<uint32_t>(Time::get_singleton()->get_ticks_msec()),
			Input::get_singleton()->is_key_pressed(KEY_CTRL));
}

void MenuDriver::on_frame_scroll_value_(int p_index, int p_value) {
	runtime_.on_frame_scroll_value(p_index, p_value);
}

void MenuDriver::process_mouse(const Vector2 &p_position, bool p_button_down) {
	runtime_.process_mouse(p_position.x, p_position.y, p_button_down);
}

bool MenuDriver::process_wheel(const Vector2 &p_position, int p_steps) {
	return runtime_.process_wheel(p_position.x, p_position.y, p_steps);
}

bool MenuDriver::handle_key_input(const Ref<InputEventKey> &p_event) {
	if (p_event.is_null() || p_event->is_echo() || !p_event->is_pressed()) return false;
	// Godot key -> the runtime's key classes (the edit VK codes live at the
	// engine home, engine/runtime/menu menu_edit.h kEditKey*).
	opennova::menu::MenuKeyInput key;
	using Key = opennova::menu::MenuKeyInput::Key;
	const godot::Key keycode = p_event->get_keycode();
	switch (keycode) {
		case KEY_ESCAPE: key.key = Key::Escape; break;
		case KEY_ENTER:
		case KEY_KP_ENTER: key.key = Key::Enter; break;
		case KEY_BACKSPACE: key.key = Key::Backspace; break;
		case KEY_END: key.key = Key::End; break;
		case KEY_HOME: key.key = Key::Home; break;
		case KEY_LEFT: key.key = Key::Left; break;
		case KEY_RIGHT: key.key = Key::Right; break;
		case KEY_DELETE: key.key = Key::Delete; break;
		default: break;
	}
	key.unicode = static_cast<int>(p_event->get_unicode());
	key.printable_keycode = static_cast<int>(keycode);
	key.shift = p_event->is_shift_pressed();
	return runtime_.handle_key(key,
			static_cast<uint32_t>(Time::get_singleton()->get_ticks_msec()),
			Input::get_singleton()->is_key_pressed(KEY_CTRL));
}

void MenuDriver::close_active_combo_popup() { runtime_.close_active_combo_popup(); }
bool MenuDriver::is_combo_popup_open(int p_id) const { return runtime_.is_combo_popup_open(p_id); }
int MenuDriver::get_focused_widget() const { return runtime_.focused_widget(); }

void MenuDriver::tick(int64_t p_time_ms) {
	if (MenuFrame *frame = frame_()) frame->set_time_ms(p_time_ms);
}

void MenuDriver::set_mission_controls(const PackedStringArray &p_lists,
		const PackedStringArray &p_briefings, const PackedStringArray &p_accepts) {
	flow_.set_mission_controls(to_std_strings(p_lists), to_std_strings(p_briefings),
			to_std_strings(p_accepts));
}

void MenuDriver::seed_mission_list(int p_id, const TypedArray<MissionCatalogRow> &p_rows) {
	flow_.seed_missions(runtime_, p_id, mission_choices(p_rows));
}

void MenuDriver::select_mission(int p_id, int p_row, const String &p_fallback) {
	flow_.select_mission(runtime_, p_id, p_row, to_std(p_fallback));
}

String MenuDriver::get_selected_mission() const { return to_gd(flow_.selected_mission()); }

bool MenuDriver::request_expansion(const String &p_name, const String &p_current, bool p_packed) {
	return flow_.request_expansion(to_std(p_name), to_std(p_current), p_packed) !=
			opennova::menu::MenuFlow::ExpansionPick::NeedsPackedRoot;
}

String MenuDriver::take_expansion_reload() { return to_gd(flow_.take_expansion_reload()); }

void MenuDriver::seed_host_pool(const TypedArray<MissionCatalogRow> &p_rows) {
	host_dialog_.seed(runtime_, mission_choices(p_rows));
}

void MenuDriver::add_host_missions(const Ref<RtxtStringFile> &p_text) {
	host_dialog_.add_selected(runtime_, game_text_lookup(p_text));
}

PackedStringArray MenuDriver::selected_host_missions() const {
	return to_gd_strings(host_dialog_.selected_missions());
}

void MenuDriver::select_host_location(int p_id, const String &p_country) {
	opennova::menu::HostDialog::select_location(runtime_, p_id, to_std(p_country));
}

void MenuDriver::prepare_options(const Ref<ControlsModel> &p_controls) {
	if (p_controls.is_valid()) options_.prepare(runtime_, p_controls->native_bindings());
}

int MenuDriver::activate_options(const Ref<ControlsModel> &p_controls, const String &p_name) {
	return p_controls.is_valid() ? options_.activate(runtime_, p_controls->native_bindings(),
			to_std(p_name)) : 0;
}

void MenuDriver::arm_options_remap(const Ref<ControlsModel> &p_controls, int p_id, int p_row) {
	if (p_controls.is_valid()) options_.arm(runtime_, p_controls->native_bindings(), p_id, p_row);
}

int MenuDriver::consume_options_input(const Ref<ControlsModel> &p_controls,
		const Ref<InputEvent> &p_event) {
	if (p_controls.is_null() || p_event.is_null()) return 0;
	opennova::menu::RemapInput input;
	const Ref<InputEventKey> key = p_event;
	const Ref<InputEventMouseButton> mouse = p_event;
	if (key.is_valid()) {
		input.kind = opennova::menu::RemapInput::Kind::Key;
		input.pressed = key->is_pressed();
		input.escape = key->get_physical_keycode() == Key::KEY_ESCAPE;
		input.vk = ControlsModel::vk_from_godot_key(static_cast<int>(key->get_physical_keycode()));
		input.ctrl = key->is_ctrl_pressed();
		input.shift = key->is_shift_pressed();
		input.repeat = key->is_echo();
	} else if (mouse.is_valid()) {
		input.kind = opennova::menu::RemapInput::Kind::Mouse;
		input.pressed = mouse->is_pressed();
		input.mouse_mask = static_cast<uint16_t>(ControlsModel::mouse_mask_from_godot_button(
				static_cast<int>(mouse->get_button_index())));
	}
	return options_.consume(runtime_, p_controls->native_bindings(), input);
}

void MenuDriver::end_options_remap(const Ref<ControlsModel> &p_controls, bool p_refill) {
	if (p_controls.is_valid()) options_.end_remap(runtime_, p_controls->native_bindings(), p_refill);
}

// ---- loadout screen adapters --------------------------------------------------------

int MenuDriver::fill_player_info_ammo(const Ref<WeaponDatabase> &p_weapons, const String &p_control,
		int p_parent, int p_primary, int p_secondary, int p_type, const Ref<RtxtStringFile> &p_text) {
	const opennova::def::DefWeaponsFile empty{};
	return opennova::menu::player_info_fill_ammo(runtime_, p_weapons.is_valid() ? p_weapons->native_file() : empty, to_std(p_control),
			p_parent, p_primary, p_secondary, p_type, game_text_lookup(p_text));
}

void MenuDriver::fill_armory_ammo(const Ref<WeaponDatabase> &p_weapons, const String &p_control,
		int p_parent, const String &p_current_name, int p_current_clips, const Ref<RtxtStringFile> &p_text) {
	const opennova::def::DefWeaponsFile empty{};
	opennova::menu::armory_fill_ammo(runtime_, p_weapons.is_valid() ? p_weapons->native_file() : empty, to_std(p_control),
			p_parent, to_std(p_current_name), p_current_clips, game_text_lookup(p_text));
}

TypedArray<WeaponDef> MenuDriver::fill_player_info_grenades(const Ref<WeaponDatabase> &p_weapons,
		int p_class_mask, int p_team_mask, const Dictionary &p_counts, const Ref<RtxtStringFile> &p_text) {
	const opennova::def::DefWeaponsFile empty{};
	return loadout_defs(p_weapons, opennova::menu::player_info_fill_grenades(runtime_,
			p_weapons.is_valid() ? p_weapons->native_file() : empty, p_class_mask, p_team_mask, loadout_counts(p_counts), game_text_lookup(p_text)));
}

TypedArray<WeaponDef> MenuDriver::fill_armory_grenades(const Ref<WeaponDatabase> &p_weapons,
		int p_class_mask, int p_team_mask, const Array &p_current, const Callable &p_availability,
		const Ref<RtxtStringFile> &p_text) {
	if (p_weapons.is_null()) return {};
	std::vector<opennova::playersav::KitEntry> current;
	for (int i = 0; i < p_current.size(); ++i) {
		const Dictionary row = p_current[i];
		opennova::playersav::KitEntry entry;
		entry.name = to_std(String(row.get("name", "")));
		entry.ammo_primary = int(row.get("ammo_primary", -1));
		current.push_back(std::move(entry));
	}
	opennova::menu::WeaponAvailability availability;
	if (p_availability.is_valid())
		availability = [p_availability](const std::string &name) { return int(p_availability.call(to_gd(name))); };
	return loadout_defs(p_weapons, opennova::menu::armory_fill_grenades(runtime_, p_weapons->native_file(),
			p_class_mask, p_team_mask, current, availability, game_text_lookup(p_text)));
}

double MenuDriver::player_info_loadout_weight(const Ref<WeaponDatabase> &p_weapons,
		const TypedArray<WeaponDef> &p_parents, const TypedArray<WeaponDef> &p_grenades,
		const Dictionary &p_primary, const Dictionary &p_secondary) const {
	if (p_weapons.is_null()) return 0.0;
	return opennova::menu::player_info_screen_weight(runtime_, p_weapons->native_file(),
			loadout_parents(p_parents), loadout_indices(p_grenades), loadout_counts(p_primary), loadout_counts(p_secondary));
}

double MenuDriver::armory_loadout_weight(const Ref<WeaponDatabase> &p_weapons,
		const TypedArray<WeaponDef> &p_parents, const TypedArray<WeaponDef> &p_grenades) const {
	if (p_weapons.is_null()) return 0.0;
	return opennova::menu::armory_screen_weight(runtime_, p_weapons->native_file(),
			loadout_parents(p_parents), loadout_indices(p_grenades));
}

int MenuDriver::update_player_info_avatars(const Ref<AvatarDatabase> &p_db, int p_change,
		int p_value, int p_team, int p_voice, const Ref<RtxtStringFile> &p_gameui,
		const Ref<RtxtStringFile> &p_menutxt) {
	const opennova::avatars::AvatarsFile empty{};
	return avatars_.update(runtime_, p_db.is_valid() ? p_db->native_file() : empty,
			static_cast<opennova::menu::PlayerInfoAvatars::Change>(p_change), p_value, p_team, p_voice,
			game_text_lookup(p_gameui), game_text_lookup(p_menutxt));
}

PackedInt32Array MenuDriver::player_info_nationality_rows() const {
	PackedInt32Array rows;
	for (int index : avatars_.nationalities) rows.push_back(index);
	return rows;
}

Ref<AvatarComboRow> MenuDriver::player_info_avatar_combo(const Ref<AvatarDatabase> &p_db) const {
	if (p_db.is_null()) return {};
	const auto *combo = avatars_.selected_combo(runtime_, p_db->native_file());
	if (!combo) return {};
	const auto &division = p_db->native_file().nationalities[avatars_.nationality].divisions[avatars_.division];
	return p_db->get_combo(avatars_.nationality, avatars_.division, static_cast<int>(combo - division.combos));
}

void MenuDriver::preview_player_info_voice(const Ref<AvatarDatabase> &p_db, int p_voice) {
	const opennova::avatars::AvatarsFile empty{};
	const std::string trigger = avatars_.voice_preview_trigger(runtime_,
			p_db.is_valid() ? p_db->native_file() : empty, p_voice);
	if (!trigger.empty()) play_widget_sound(to_gd(trigger), opennova::menu::kPlayerInfoVoiceBank);
}

// ---- bindings ----------------------------------------------------------------------

void MenuDriver::_bind_methods() {
	BIND_ENUM_CONSTANT(AVATAR_TEAM);
	BIND_ENUM_CONSTANT(AVATAR_NATIONALITY);
	BIND_ENUM_CONSTANT(AVATAR_DIVISION);
	BIND_ENUM_CONSTANT(AVATAR_COMBO);
	BIND_ENUM_CONSTANT(AVATAR_VOICE);
	ClassDB::bind_method(D_METHOD("update_player_info_avatars", "db", "change", "value", "team", "voice", "gameui", "menutxt"), &MenuDriver::update_player_info_avatars);
	ClassDB::bind_method(D_METHOD("player_info_nationality_rows"), &MenuDriver::player_info_nationality_rows);
	ClassDB::bind_method(D_METHOD("player_info_avatar_nationality"), &MenuDriver::player_info_avatar_nationality);
	ClassDB::bind_method(D_METHOD("player_info_avatar_division"), &MenuDriver::player_info_avatar_division);
	ClassDB::bind_method(D_METHOD("player_info_avatar_preview_changed"), &MenuDriver::player_info_avatar_preview_changed);
	ClassDB::bind_method(D_METHOD("player_info_avatar_combo", "db"), &MenuDriver::player_info_avatar_combo);
	ClassDB::bind_method(D_METHOD("preview_player_info_voice", "db", "voice"), &MenuDriver::preview_player_info_voice);
	ClassDB::bind_method(D_METHOD("fill_player_info_ammo", "weapons", "control", "parent", "primary", "secondary", "type", "text"), &MenuDriver::fill_player_info_ammo);
	ClassDB::bind_method(D_METHOD("fill_armory_ammo", "weapons", "control", "parent", "current_name", "current_clips", "text"), &MenuDriver::fill_armory_ammo);
	ClassDB::bind_method(D_METHOD("fill_player_info_grenades", "weapons", "class_mask", "team_mask", "counts", "text"), &MenuDriver::fill_player_info_grenades);
	ClassDB::bind_method(D_METHOD("fill_armory_grenades", "weapons", "class_mask", "team_mask", "current", "availability", "text"), &MenuDriver::fill_armory_grenades);
	ClassDB::bind_method(D_METHOD("player_info_loadout_weight", "weapons", "parents", "grenades", "primary", "secondary"), &MenuDriver::player_info_loadout_weight);
	ClassDB::bind_method(D_METHOD("armory_loadout_weight", "weapons", "parents", "grenades"), &MenuDriver::armory_loadout_weight);
	ClassDB::bind_method(D_METHOD("set_mission_controls", "lists", "briefings", "accepts"), &MenuDriver::set_mission_controls);
	ClassDB::bind_method(D_METHOD("clear_mission_rows"), &MenuDriver::clear_mission_rows);
	ClassDB::bind_method(D_METHOD("seed_mission_list", "id", "rows"), &MenuDriver::seed_mission_list);
	ClassDB::bind_method(D_METHOD("select_mission", "id", "row", "fallback"), &MenuDriver::select_mission);
	ClassDB::bind_method(D_METHOD("activate_mission", "id", "row"), &MenuDriver::activate_mission);
	ClassDB::bind_method(D_METHOD("get_selected_mission"), &MenuDriver::get_selected_mission);
	ClassDB::bind_method(D_METHOD("clear_selected_mission"), &MenuDriver::clear_selected_mission);
	ClassDB::bind_method(D_METHOD("request_expansion", "name", "current", "packed"), &MenuDriver::request_expansion);
	ClassDB::bind_method(D_METHOD("has_pending_expansion_reload"), &MenuDriver::has_pending_expansion_reload);
	ClassDB::bind_method(D_METHOD("take_expansion_reload"), &MenuDriver::take_expansion_reload);
	ClassDB::bind_method(D_METHOD("seed_host_pool", "rows"), &MenuDriver::seed_host_pool);
	ClassDB::bind_method(D_METHOD("filter_host_missions"), &MenuDriver::filter_host_missions);
	ClassDB::bind_method(D_METHOD("add_host_missions", "text"), &MenuDriver::add_host_missions);
	ClassDB::bind_method(D_METHOD("remove_host_missions"), &MenuDriver::remove_host_missions);
	ClassDB::bind_method(D_METHOD("can_start_host"), &MenuDriver::can_start_host);
	ClassDB::bind_method(D_METHOD("selected_host_missions"), &MenuDriver::selected_host_missions);
	ClassDB::bind_method(D_METHOD("select_host_location", "id", "country"), &MenuDriver::select_host_location);
	ClassDB::bind_method(D_METHOD("prepare_options", "controls"), &MenuDriver::prepare_options);
	ClassDB::bind_method(D_METHOD("is_options_surface"), &MenuDriver::is_options_surface);
	ClassDB::bind_method(D_METHOD("activate_options", "controls", "name"), &MenuDriver::activate_options);
	ClassDB::bind_method(D_METHOD("arm_options_remap", "controls", "id", "row"), &MenuDriver::arm_options_remap);
	ClassDB::bind_method(D_METHOD("consume_options_input", "controls", "event"), &MenuDriver::consume_options_input);
	ClassDB::bind_method(D_METHOD("end_options_remap", "controls", "refill"), &MenuDriver::end_options_remap);
	ClassDB::bind_method(D_METHOD("show_ingame_main"), &MenuDriver::show_ingame_main);
	BIND_ENUM_CONSTANT(OPTIONS_CONSUMED);
	BIND_ENUM_CONSTANT(OPTIONS_PERSIST_BINDINGS);
	BIND_ENUM_CONSTANT(OPTIONS_COMMIT_PREVIEW);
	BIND_ENUM_CONSTANT(OPTIONS_RESTORE_PREVIEW);
	ClassDB::bind_method(D_METHOD("attach", "frame", "audio"), &MenuDriver::attach);
	ClassDB::bind_method(D_METHOD("set_music_director", "director"),
			&MenuDriver::set_music_director);
	ClassDB::bind_method(D_METHOD("set_music_var_index", "index"),
			&MenuDriver::set_music_var_index);
	ClassDB::bind_method(D_METHOD("get_frame"), &MenuDriver::get_frame);
	ClassDB::bind_method(D_METHOD("get_menu_file"), &MenuDriver::get_menu_file);
	ClassDB::bind_method(D_METHOD("get_current_screen"), &MenuDriver::get_current_screen);
	ClassDB::bind_method(D_METHOD("open_document", "doc", "root", "style", "text", "menu_file",
								 "target_screen"),
			&MenuDriver::open_document, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("document"), &MenuDriver::document);
	ClassDB::bind_method(D_METHOD("get_screen_names"), &MenuDriver::get_screen_names);
	ClassDB::bind_method(D_METHOD("show_screen", "name"), &MenuDriver::show_screen);
	ClassDB::bind_method(D_METHOD("navigate_to_screen", "name"), &MenuDriver::navigate_to_screen);
	ClassDB::bind_method(D_METHOD("pop_screen"), &MenuDriver::pop_screen);

	ClassDB::bind_method(D_METHOD("widget_id", "name"), &MenuDriver::widget_id);
	ClassDB::bind_method(D_METHOD("widget_name_of", "id"), &MenuDriver::widget_name_of);
	ClassDB::bind_method(D_METHOD("widget_kind_of", "id"), &MenuDriver::widget_kind_of);
	ClassDB::bind_method(D_METHOD("has_widget", "name"), &MenuDriver::has_widget);
	ClassDB::bind_method(D_METHOD("frame_index", "id"), &MenuDriver::frame_index);
	ClassDB::bind_method(D_METHOD("widget_frame_rect", "id"), &MenuDriver::widget_frame_rect);

	ClassDB::bind_method(D_METHOD("set_widget_shown", "id", "shown"), &MenuDriver::set_widget_shown);
	ClassDB::bind_method(D_METHOD("is_widget_shown", "id"), &MenuDriver::is_widget_shown);
	ClassDB::bind_method(D_METHOD("set_widget_disabled", "id", "disabled"),
			&MenuDriver::set_widget_disabled);
	ClassDB::bind_method(D_METHOD("is_widget_disabled", "id"), &MenuDriver::is_widget_disabled);
	ClassDB::bind_method(D_METHOD("set_widget_checked", "id", "checked"),
			&MenuDriver::set_widget_checked);
	ClassDB::bind_method(D_METHOD("is_widget_checked", "id"), &MenuDriver::is_widget_checked);
	ClassDB::bind_method(D_METHOD("set_widget_text", "id", "text"), &MenuDriver::set_widget_text);
	ClassDB::bind_method(D_METHOD("get_widget_text", "id"), &MenuDriver::get_widget_text);
	ClassDB::bind_method(D_METHOD("set_widget_items", "id", "items"), &MenuDriver::set_widget_items);
	ClassDB::bind_method(D_METHOD("get_widget_items", "id"), &MenuDriver::get_widget_items);
	ClassDB::bind_method(D_METHOD("item_count", "id"), &MenuDriver::item_count);
	ClassDB::bind_method(D_METHOD("item_text", "id", "row"), &MenuDriver::item_text);
	ClassDB::bind_method(D_METHOD("item_value", "id", "row"), &MenuDriver::item_value);
	ClassDB::bind_method(D_METHOD("widget_actions", "id"), &MenuDriver::widget_actions);
	ClassDB::bind_method(D_METHOD("select_row_by_value", "id", "value", "emit"),
			&MenuDriver::select_row_by_value, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("select_row", "id", "row", "emit"), &MenuDriver::select_row,
			DEFVAL(true));
	ClassDB::bind_method(D_METHOD("selected_row", "id"), &MenuDriver::selected_row);
	ClassDB::bind_method(D_METHOD("set_widget_scroll_range", "id", "minimum", "maximum", "page",
								 "value"),
			&MenuDriver::set_widget_scroll_range);
	ClassDB::bind_method(D_METHOD("get_widget_scroll_range", "id"),
			&MenuDriver::get_widget_scroll_range);

	ClassDB::bind_method(D_METHOD("table_add_row", "id", "cells"), &MenuDriver::table_add_row);
	ClassDB::bind_method(D_METHOD("table_clear_rows", "id"), &MenuDriver::table_clear_rows);
	ClassDB::bind_method(D_METHOD("table_row_count", "id"), &MenuDriver::table_row_count);
	ClassDB::bind_method(D_METHOD("table_cell_text", "id", "row", "col"),
			&MenuDriver::table_cell_text);
	ClassDB::bind_method(D_METHOD("table_select_row", "id", "row", "additive"),
			&MenuDriver::table_select_row, DEFVAL(false));

	ClassDB::bind_method(D_METHOD("activate", "id"), &MenuDriver::activate);
	ClassDB::bind_method(D_METHOD("spin_cycle", "id", "delta"), &MenuDriver::spin_cycle);
	ClassDB::bind_method(D_METHOD("spin_value_attr", "id"), &MenuDriver::spin_value_attr);
	ClassDB::bind_method(D_METHOD("dispatch_action_row", "action"),
			&MenuDriver::dispatch_action_row);
	ClassDB::bind_method(D_METHOD("play_widget_sound", "trigger", "file"),
			&MenuDriver::play_widget_sound);

	ClassDB::bind_method(D_METHOD("process_mouse", "position", "button_down"),
			&MenuDriver::process_mouse);
	ClassDB::bind_method(D_METHOD("process_wheel", "position", "steps"),
			&MenuDriver::process_wheel);
	ClassDB::bind_method(D_METHOD("handle_key_input", "event"), &MenuDriver::handle_key_input);
	ClassDB::bind_method(D_METHOD("close_active_combo_popup"),
			&MenuDriver::close_active_combo_popup);
	ClassDB::bind_method(D_METHOD("is_combo_popup_open", "id"), &MenuDriver::is_combo_popup_open);
	ClassDB::bind_method(D_METHOD("get_focused_widget"), &MenuDriver::get_focused_widget);
	ClassDB::bind_method(D_METHOD("tick", "time_ms"), &MenuDriver::tick);

	// The frame's signals land here (the pump's click edge and its CScrollWnd
	// interaction result).
	ClassDB::bind_method(D_METHOD("_on_frame_widget_clicked", "index"),
			&MenuDriver::on_frame_widget_clicked_);
	ClassDB::bind_method(D_METHOD("_on_frame_scroll_value", "index", "value"),
			&MenuDriver::on_frame_scroll_value_);

	ADD_SIGNAL(MethodInfo("screen_changed", PropertyInfo(Variant::STRING, "screen_name")));
	ADD_SIGNAL(MethodInfo("menu_requested", PropertyInfo(Variant::STRING, "file"),
			PropertyInfo(Variant::STRING, "target_screen")));
	ADD_SIGNAL(MethodInfo("quit_requested"));
	ADD_SIGNAL(MethodInfo("url_requested", PropertyInfo(Variant::STRING, "url")));
	// The widget sound edge as resolved from the SOUND table (trigger, bank);
	// the MenuAudio player plays it.
	ADD_SIGNAL(MethodInfo("sound_requested", PropertyInfo(Variant::STRING, "file"),
			PropertyInfo(Variant::STRING, "trigger")));
	ADD_SIGNAL(MethodInfo("widget_value_changed", PropertyInfo(Variant::STRING, "widget_name"),
			PropertyInfo(Variant::STRING, "kind"), PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::STRING, "value")));
	// Click-level activation of a widget (button/goto/checkbox/radio...) — the
	// named-control seam the shell and companions wire launch/quit policy to.
	ADD_SIGNAL(MethodInfo("widget_activated", PropertyInfo(Variant::INT, "id"),
			PropertyInfo(Variant::STRING, "widget_name")));
	// List double-click (the ItemList item_activated equivalent).
	ADD_SIGNAL(MethodInfo("list_activated", PropertyInfo(Variant::INT, "id"),
			PropertyInfo(Variant::INT, "row")));
	// The pump's claim moved between widgets (hover edges; PLAYER_PREVIEW zoom).
	ADD_SIGNAL(MethodInfo("widget_hover_changed", PropertyInfo(Variant::INT, "id"),
			PropertyInfo(Variant::BOOL, "hovered")));
}

} // namespace godot
