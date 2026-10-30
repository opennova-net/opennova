#include "mnu/menu_driver.h"

#include "audio/music_director.h"
#include "mnu/menu_audio.h"
#include "mnu/controls_model.h"
#include "mission/mission_catalog.h"
#include "mnu/menu_frame.h"
#include "object/avatar_database.h"
#include "object/avatar_records.h"
#include "player/player_profiles.h"
#include "mnu/mns_stylesheet.h"
#include "mnu/mnu_document.h"
#include "resource_index/launch_flags.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "hud/hud_overlay.h"
#include "simulation/deploy_rows.h"
#include "simulation/hud_view_records.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <runtime/inmatch/joiner_role.h> // the STAT DONE press's runtime
#include <runtime/inmatch/role_feeds.h> // deploy_table_zones
#include <base/title/title.h>
#include <runtime/inmatch/stat_screen_feed.h>
#include <runtime/menu/menu_commands.h>
#include <runtime/menu/menu_credits.h>
#include <runtime/world/deploy_screen_feed.h>

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_joypad_button.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable.hpp>

using opennova::to_gd;
using opennova::to_std;

namespace godot {

namespace {

std::vector<opennova::menu::MissionChoice> mission_choices(const TypedArray<MissionCatalogRow> &rows) {
	std::vector<opennova::menu::MissionChoice> choices;
	for (int i = 0; i < rows.size(); ++i) {
		const Ref<MissionCatalogRow> row = rows[i];
		if (row.is_valid()) choices.push_back({to_std(row->get_file()), to_std(row->display_text()),
				to_std(row->get_briefing()), static_cast<uint32_t>(row->get_game_type()), {},
				to_std(row->get_title()), static_cast<uint8_t>(row->get_mission_type())});
	}
	return choices;
}

PackedStringArray to_gd_strings(const std::vector<std::string> &p_values) {
	PackedStringArray out;
	for (const std::string &value : p_values) out.push_back(to_gd(value));
	return out;
}

// A menu text for the script. A code-page document's runtime holds the page's
// bytes: an authored text, a typed character, a cell or an item the engine
// fills with roster or gametext bytes (D-MNU-46), and a text a script set,
// which the setters encode (D-MNU-49); so the read is the page's, each byte its
// character (`p_code_page`). A Unicode document's model holds UTF-8 and a
// script's set its bytes, so its read takes the mixed rule, valid UTF-8 as it
// is and anything else as cp1252 (retail_text_to_utf8).
String menu_text_to_gd(const std::string &p_text, bool p_code_page) {
	return p_code_page ? opennova::cp1252_to_gd(p_text)
					   : to_gd(opennova::retail_text_to_utf8(p_text));
}

PackedStringArray menu_texts_to_gd(const std::vector<std::string> &p_values, bool p_code_page) {
	PackedStringArray out;
	for (const std::string &value : p_values) out.push_back(menu_text_to_gd(value, p_code_page));
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
		if (MenuFrame *f = frame())
			f->configure(owner_->doc_, to_gd(screen), owner_->root_, owner_->style_);
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
	void set_widget_text(int index, const std::string &text, int hotkey_pos) override {
		// The runtime's bytes as they are, as the items go (D-MNU-49).
		if (MenuFrame *f = frame()) f->set_widget_text_bytes(index, text, hotkey_pos);
	}
	void set_widget_items(int index, const std::vector<std::string> &items) override {
		// The items as the runtime holds them, bytes and all: a round trip
		// through godot::String parses them as UTF-8, which turns a code-page
		// byte into U+FFFD (D-MNU-46).
		if (MenuFrame *f = frame()) f->set_widget_item_texts(index, items);
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
	void set_widget_disabled_items(int index, const std::vector<uint8_t> &rows) override {
		if (MenuFrame *f = frame()) f->set_widget_disabled_items(index, rows);
	}
	void set_widget_table_rows(int index,
			const std::vector<opennova::menu::MenuTableRow> &rows) override {
		if (MenuFrame *f = frame()) f->set_widget_table_rows(index, rows);
	}
	void set_widget_clip_rect(int index, bool enabled, int left, int top, int right,
			int bottom) override {
		if (MenuFrame *f = frame())
			f->set_widget_clip_rect(index, enabled, Rect2i(left, top, right - left, bottom - top));
	}
	void set_widget_table_columns(int index, bool installed,
			const std::vector<opennova::menu::MenuTableColumn> &columns,
			int sort_column) override {
		if (MenuFrame *f = frame()) f->set_widget_table_columns(index, installed, columns, sort_column);
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
	void set_widget_image(int index, uint32_t states, const std::string &file) override {
		if (MenuFrame *f = frame()) f->set_widget_image(index, states, file);
	}
	void set_widget_rect(int index, int left, int top, int right, int bottom) override {
		if (MenuFrame *f = frame())
			f->set_widget_rect(index, Rect2i(left, top, right - left, bottom - top));
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
		return f != nullptr ? opennova::gd_to_retail_text(f->get_widget_text(index))
							: std::string();
	}
	int item_count(int index) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->item_count(index) : 0;
	}
	std::string item_display_text(int index, int row) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->item_display_text(index, row) : std::string();
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
	std::vector<opennova::menu::MenuPumpWindow> press_mouse(float x, float y) override {
		MenuFrame *f = frame();
		return f != nullptr ? f->press_mouse(Vector2(x, y)) : std::vector<opennova::menu::MenuPumpWindow>();
	}
	int process_mouse(float x, float y, bool button_down) override {
		MenuFrame *f = frame();
		return f != nullptr ? f->process_mouse(Vector2(x, y), button_down) : -1;
	}
	void release_mouse() override {
		if (MenuFrame *f = frame()) f->release_mouse();
	}
	bool press_popup_mouse(int index, float x, float y) override {
		MenuFrame *f = frame();
		return f != nullptr && f->press_popup_mouse(index, Vector2(x, y));
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
		// A claim with no CURSOR loaded shows no pointer: the game hides the
		// system one for good at boot and draws only a screen's CURSOR
		// (docs/mnu/menu-re.md "No CURSOR, no pointer", D-MNU-26).
		if (MenuFrame *f = frame()) {
			const Ref<Texture2D> cursor = f->get_cursor_texture();
			owner_->show_pointer_(
					cursor.is_valid() ? Ref<Resource>(cursor) : owner_->no_pointer_());
		}
	}
	void reset_cursor() override {
		// A screen just configured: no pointer until its claim's CURSOR applies.
		owner_->show_pointer_(owner_->no_pointer_());
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
	bool table_hit(int index, float x, float y, int *row, int *column) const override {
		MenuFrame *f = frame();
		if (f == nullptr) {
			*row = -1;
			*column = -1;
			return false;
		}
		return f->table_hit(index, Vector2(x, y), row, column);
	}
	std::string widget_mnemonic(int index) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->widget_mnemonic(index) : std::string();
	}
	std::string widget_string(int index, const std::string &key) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->widget_string(index, key) : key;
	}
	std::string table_string(const std::string &table, const std::string &key) const override {
		MenuFrame *f = frame();
		return f != nullptr ? f->table_string(table, key) : key;
	}
	void set_open_popup(int index) override {
		if (MenuFrame *f = frame()) f->set_open_popup(index);
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
		// The screen controller holding the document hears an event first, while it still holds
		// it (the open's own events are its controller's open(), its INIT); this device half
		// relays every event after.
		if (wired_ != nullptr && !opening_) wired_->on_event(runtime_, p_event);
		on_runtime_event_(p_event);
	});
	// The single-player screen's controls and the Mods list's descriptions, the engine's name
	// sets (menu_commands.h), as the editor's Try mode sets them (editor/preview/menu_try.cpp).
	flow_.set_mission_controls(opennova::menu::menu_name_set(opennova::menu::MenuNameSet::SinglePlayerLists),
			opennova::menu::menu_name_set(opennova::menu::MenuNameSet::Briefings),
			opennova::menu::menu_name_set(opennova::menu::MenuNameSet::SinglePlayerAccepts));
	mods_.set_descriptions(opennova::menu::menu_name_set(opennova::menu::MenuNameSet::ModDescriptions));
}

MenuDriver::~MenuDriver() {
	runtime_.set_sink(nullptr);
	release_wired_();
}

void MenuDriver::release_wired_() {
	if (wired_ == nullptr) return;
	opennova::menu::MenuScreenController *const released = wired_;
	wired_ = nullptr;
	released->close(runtime_);
}

bool MenuDriver::claim_screen(const Ref<MenuScreenBinding> &p_screen) {
	opennova::menu::MenuScreenController *const controller =
			p_screen.is_valid() ? p_screen->screen_controller() : nullptr;
	if (controller == nullptr || runtime_.document() == nullptr) return false;
	if (wired_ != controller) release_wired_();
	released_ = claimed_;
	claimed_ = p_screen;
	wired_ = controller;
	controller->open(runtime_, runtime_.menu_file(), runtime_.current_screen());
	return true;
}

void MenuDriver::release_screen() {
	// A controller may ask its own release from inside its event (a CANCEL that closes the
	// screen): its binding stays alive until the next claim or release, past the dispatch on the
	// stack.
	released_ = claimed_;
	claimed_.unref();
	release_wired_();
}

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

// The pointer a menu shows where its claim has no CURSOR: a transparent image as the OS
// custom cursor, so the mouse still moves, presses and hits the menu unseen (D-MNU-26).
Ref<Resource> MenuDriver::no_pointer_() {
	if (no_pointer_image_.is_null()) {
		no_pointer_image_ = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
		no_pointer_image_->fill(Color(0, 0, 0, 0));
	}
	return no_pointer_image_;
}

void MenuDriver::show_pointer_(const Ref<Resource> &p_pointer) {
	applied_pointer_id_ =
			p_pointer.is_valid() ? ObjectID(p_pointer->get_instance_id()) : ObjectID();
	Input::get_singleton()->set_custom_mouse_cursor(p_pointer, Input::CURSOR_ARROW);
}

Ref<Resource> MenuDriver::get_applied_pointer() const {
	return Ref<Resource>(Object::cast_to<Resource>(ObjectDB::get_instance(applied_pointer_id_)));
}

String MenuDriver::widget_screen_of(int p_id) const {
	return to_gd(runtime_.widget_screen_of(p_id));
}

bool MenuDriver::open_document(const Ref<MnuDocument> &p_doc, const Ref<ResourceRoot> &p_root,
		const Ref<MnsStyleSheet> &p_style, const String &p_menu_file,
		const String &p_target_screen) {
	doc_ = p_doc;
	root_ = p_root;
	style_ = p_style;
	// The previous document's commands go with it: nothing is bound until the next wire.
	commands_ = opennova::menu::MenuCommands();
	// The STARTUP label's line (D-MNU-76): the launch's game and its installed release, ONE's
	// build and the protocol it speaks (LaunchFlags::version_line).
	runtime_.set_version_line(to_std(LaunchFlags::version_line()));
	opening_ = true;
	const bool shown = runtime_.open_document(doc_.is_valid() ? &doc_->get_native() : nullptr,
			to_std(p_menu_file), to_std(p_target_screen));
	opening_ = false;
	// The previous document's controller lets go: the embedder claims this one for its own.
	released_ = claimed_;
	claimed_.unref();
	release_wired_();
	return shown;
}

PackedStringArray MenuDriver::get_screen_names() const {
	return to_gd_strings(runtime_.screen_names());
}

bool MenuDriver::show_screen(const String &p_name) { return runtime_.show_screen(to_std(p_name)); }
bool MenuDriver::navigate_to_screen(const String &p_name) {
	return runtime_.navigate_to_screen(to_std(p_name));
}
bool MenuDriver::pop_screen() { return runtime_.pop_screen(); }

namespace {
PackedStringArray history_row_to_gd(const opennova::menu::ScreenHistoryRow &p_row) {
	PackedStringArray out;
	out.push_back(to_gd(p_row.file));
	out.push_back(to_gd(p_row.screen));
	return out;
}
} // namespace

void MenuDriver::push_screen_history(const String &p_file, const String &p_screen) {
	runtime_.screen_history().push(to_std(p_file), to_std(p_screen));
}

PackedStringArray MenuDriver::pop_screen_history() {
	opennova::menu::ScreenHistoryRow row;
	return runtime_.screen_history().pop(&row) ? history_row_to_gd(row) : PackedStringArray();
}

void MenuDriver::clear_screen_history() { runtime_.screen_history().clear(); }
void MenuDriver::trim_screen_history() { runtime_.trim_screen_history(); }

int MenuDriver::get_screen_history_depth() const { return runtime_.screen_history().size(); }

Array MenuDriver::get_screen_history() const {
	Array out;
	for (const opennova::menu::ScreenHistoryRow &row : runtime_.screen_history().rows()) {
		Dictionary entry;
		entry["file"] = to_gd(row.file);
		entry["screen"] = to_gd(row.screen);
		entry["mark"] = row.mark;
		out.push_back(entry);
	}
	return out;
}

void MenuDriver::leave_menu_mode() { runtime_.leave_menu_mode(); }

PackedStringArray MenuDriver::return_to_menu_mode() {
	opennova::menu::ScreenHistoryRow row;
	return runtime_.return_to_menu_mode(&row) ? history_row_to_gd(row) : PackedStringArray();
}

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
		case Kind::PopRequested:
			emit_signal("pop_requested");
			break;
		case Kind::UrlRequested:
			emit_signal("url_requested", to_gd(p_event.text), p_event.flag);
			break;
		case Kind::FilterRequested:
			emit_signal("filter_requested", p_event.id, p_event.value,
					menu_text_to_gd(p_event.text, code_page_()), p_event.flag,
					to_gd(p_event.text2));
			break;
		case Kind::Sound:
			play_widget_sound(to_gd(p_event.text2), to_gd(p_event.text));
			break;
		case Kind::ValueChanged:
			emit_signal("widget_value_changed", to_gd(p_event.text), to_gd(p_event.text2),
					p_event.value, menu_text_to_gd(p_event.text3, code_page_()));
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
			// The compiled draw walk gates every widget it draws, the credits roll among them.
			break;
		case Kind::EditCommitted:
			emit_signal("edit_committed", p_event.id, to_gd(p_event.text));
			break;
		case Kind::TableCellClicked:
			emit_signal("table_cell_clicked", p_event.id, to_gd(p_event.text), p_event.value,
					p_event.column, p_event.state, p_event.cell_value, p_event.flag);
			break;
		case Kind::ServiceRequested:
			// The service verbs' rows: the game serves its LAN and NovaWorld screens through the
			// companions' controls by name; an APPMSG row's message is the game's to answer
			// (engine menu_commands.h app_message_command).
			if (p_event.text == "APPMSG") {
				const opennova::menu::MenuCommand command =
						opennova::menu::app_message_command(p_event.text2);
				if (command != opennova::menu::MenuCommand::None)
					emit_signal("app_command_requested",
							String(opennova::menu::menu_command_token(command)));
			}
			break;
	}
}

// Marquee DATASOURCEs: every DATASOURCE of a marquee_wnd loads and appends its
// credits into the compiled roll, a text config and a CBIN one alike (the witness
// lives at the engine home, menu_credits.h).
void MenuDriver::seed_marquee_widgets_() {
	MenuFrame *frame = frame_();
	if (root_.is_null() || frame == nullptr) return;
	for (int id : runtime_.current_screen_ids()) {
		const opennova::mnu::Window *window = runtime_.index().window(id);
		if (window == nullptr || window->type != opennova::mnu::WindowType::Marquee) continue;
		opennova::menu::MarqueeCredits credits;
		bool loaded = false;
		for (const std::string &source : window->datasources) {
			if (source.empty()) continue;
			const PackedByteArray bytes = root_->read_file(to_gd(source).get_file());
			if (bytes.is_empty()) continue;
			if (opennova::menu::marquee_load_credits(bytes.ptr(), static_cast<size_t>(bytes.size()),
						credits, [frame](const std::string &name) {
							return frame->texture_loads(to_gd(name));
						}))
				loaded = true;
		}
		const int index = runtime_.frame_index(id);
		if (index >= 0 && loaded) frame->set_widget_marquee(index, credits);
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

Rect2 MenuDriver::widget_local_rect(int p_id) const {
	const int index = frame_index(p_id);
	MenuFrame *frame = frame_();
	if (index < 0 || frame == nullptr) return Rect2();
	return frame->widget_local_rect(index);
}

void MenuDriver::set_widget_rect(int p_id, const Rect2i &p_rect) {
	runtime_.set_widget_rect(p_id, p_rect.position.x, p_rect.position.y,
			p_rect.position.x + p_rect.size.x, p_rect.position.y + p_rect.size.y);
}

void MenuDriver::focus_widget(int p_id) {
	runtime_.focus_edit(p_id);
}

Vector2 MenuDriver::design_scale() const {
	// The scale pair of the fixed authoring design space (the witness lives at the
	// engine home, engine/runtime/menu menu_frame.h menu_scale_x/y).
	MenuFrame *frame = frame_();
	if (frame == nullptr) return Vector2(1.0f, 1.0f);
	const Vector2 size = frame->get_size();
	if (size.x > 1.0f && size.y > 1.0f)
		return Vector2(opennova::menu::menu_scale_x(size.x), opennova::menu::menu_scale_y(size.y));
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
// The open document is the page's (or none is open): its texts read byte by byte.
bool MenuDriver::code_page_() const {
	return !doc_.is_valid() ||
			doc_->get_native().source_encoding == opennova::mnu::SourceEncoding::CodePage;
}

String MenuDriver::set_widget_text(int p_id, const String &p_text) {
	return menu_text_to_gd(runtime_.set_widget_text(p_id, opennova::gd_to_retail_text(p_text)),
			code_page_());
}
String MenuDriver::get_widget_text(int p_id) const {
	return menu_text_to_gd(runtime_.get_widget_text(p_id), code_page_());
}
void MenuDriver::set_widget_items(int p_id, const PackedStringArray &p_items) {
	std::vector<std::string> items;
	items.reserve(static_cast<size_t>(p_items.size()));
	for (int64_t i = 0; i < p_items.size(); ++i)
		items.push_back(opennova::gd_to_retail_text(p_items[i]));
	runtime_.set_widget_items(p_id, items);
}
PackedStringArray MenuDriver::get_widget_items(int p_id) const {
	return menu_texts_to_gd(runtime_.get_widget_items(p_id), code_page_());
}
int MenuDriver::item_count(int p_id) const { return runtime_.item_count(p_id); }
String MenuDriver::item_text(int p_id, int p_row) const {
	return menu_text_to_gd(runtime_.item_text(p_id, p_row), code_page_());
}
String MenuDriver::item_value(int p_id, int p_row) const {
	return menu_text_to_gd(runtime_.item_value(p_id, p_row), code_page_());
}
TypedArray<MnuActionRow> MenuDriver::widget_actions(int p_id) const {
	return doc_.is_valid() ? doc_->get_widget_actions(p_id) : TypedArray<MnuActionRow>();
}
void MenuDriver::select_row_by_value(int p_id, const String &p_value, bool p_emit) {
	runtime_.select_row_by_value(p_id, opennova::gd_to_retail_text(p_value), p_emit);
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

int MenuDriver::table_row_count(int p_id) const { return runtime_.table_row_count(p_id); }
String MenuDriver::table_cell_text(int p_id, int p_row, int p_col) const {
	return menu_text_to_gd(runtime_.table_cell_text(p_id, p_row, p_col), code_page_());
}
void MenuDriver::table_select_row(int p_id, int p_row, bool p_additive) {
	runtime_.table_select_row(p_id, p_row, p_additive);
}
int MenuDriver::table_cell_value(int p_id, int p_row, int p_col) const {
	return runtime_.table_cell_value(p_id, p_row, p_col);
}
PackedInt32Array MenuDriver::table_selected_rows(int p_id) const {
	PackedInt32Array out;
	for (int row : runtime_.table_selected_rows(p_id)) out.push_back(row);
	return out;
}
void MenuDriver::set_widget_clip_rect(int p_id, const Rect2i &p_rect) {
	runtime_.set_widget_clip_rect(p_id, true, p_rect.position.x, p_rect.position.y,
			p_rect.position.x + p_rect.size.x, p_rect.position.y + p_rect.size.y);
}

int MenuDriver::table_sort_column(int p_id) const {
	return runtime_.table_sort_column(p_id);
}

// The stat RESULTLIST filled the way StatScreen_PopulateStatResultsList fills it: the
// engine's fill (inmatch stat_screen_feed.h, where the witness lives) over the
// records' own values, so the NAME and Squad cells take the roster's bytes.
void MenuDriver::fill_stat_results(int p_id, const TypedArray<EndRoundColumn> &p_columns,
		const TypedArray<EndRoundRow> &p_rows, int p_local_slot) {
	std::vector<opennova::inmatch::StatScreenColumn> columns;
	columns.reserve(static_cast<size_t>(p_columns.size()));
	for (int i = 0; i < p_columns.size(); ++i) {
		const Ref<EndRoundColumn> column = p_columns[i];
		if (column.is_valid()) columns.push_back(column->value());
	}
	std::vector<opennova::inmatch::StatScreenRow> rows;
	rows.reserve(static_cast<size_t>(p_rows.size()));
	for (int i = 0; i < p_rows.size(); ++i) {
		const Ref<EndRoundRow> row = p_rows[i];
		if (row.is_valid()) rows.push_back(row->value());
	}
	opennova::inmatch::fill_stat_results(runtime_, p_id, columns, rows, p_local_slot);
}

namespace {

opennova::menu::OptionsScreen::Words options_words_of(const PackedInt32Array &p_words) {
	opennova::menu::OptionsScreen::Words words{};
	for (size_t i = 0; i < words.size() && static_cast<int64_t>(i) < p_words.size(); ++i)
		words[i] = p_words[static_cast<int64_t>(i)];
	return words;
}

} // namespace

void MenuDriver::seed_options_words(const PackedInt32Array &p_words) {
	options_.seed_words(runtime_, options_words_of(p_words));
}

PackedInt32Array MenuDriver::options_word_edit(int p_id, int p_value,
		const PackedInt32Array &p_words) const {
	opennova::menu::OptionsScreen::Words words = options_words_of(p_words);
	PackedInt32Array out;
	if (!options_.edit_word(runtime_, p_id, p_value, words)) return out;
	for (const int32_t word : words) out.push_back(word);
	return out;
}

void MenuDriver::filter_stat_results(int p_id, int p_tab) {
	opennova::inmatch::stat_screen_filter_rows(runtime_, p_id, p_tab);
}

// The death screen's SPAWNPOINTS_LIST from the compiled rows: the engine's items
// (world deploy_screen_feed.h deploy_list_items: the tags stripped, each row in
// the code page's bytes, an occupant's roster name as the roster holds it).
void MenuDriver::set_deploy_list_items(int p_id, const TypedArray<DeployListRow> &p_rows) {
	std::vector<opennova::world::DeployListRow> rows;
	rows.reserve(static_cast<size_t>(p_rows.size()));
	for (int i = 0; i < p_rows.size(); ++i) {
		const Ref<DeployListRow> row = p_rows[i];
		if (row.is_valid()) rows.push_back(row->value());
	}
	opennova::world::fill_deploy_spawn_list(runtime_, p_id, rows);
}

bool MenuDriver::hide_stat_done(const Ref<Simulation> &p_sim) {
	if (p_sim.is_null()) return false;
	return opennova::inmatch::stat_screen_hide_done(runtime_, p_sim->role_view().title);
}

bool MenuDriver::show_stat_done(const Ref<Simulation> &p_sim) {
	if (p_sim.is_null()) return false;
	const opennova::inmatch::RoleView view = p_sim->role_view();
	const bool hold = view.runtime != nullptr && view.runtime->state().round_reset_hold;
	return opennova::inmatch::stat_screen_show_done(runtime_, view.title, hold);
}

bool MenuDriver::stat_done_pressed(const Ref<Simulation> &p_sim) {
	if (p_sim.is_null() || !p_sim->is_joiner() || p_sim->joiner_role_ == nullptr) return false;
	return opennova::inmatch::stat_screen_done_pressed(p_sim->role_view().title,
			p_sim->joiner_role_->client_runtime());
}

bool MenuDriver::fill_deploy_spawn_table(const Ref<Simulation> &p_sim, Object *p_hud,
		const Ref<RtxtStringFile> &p_gametext) {
	const opennova::hud::HudLayout defaults;
	const HudOverlay *hud = Object::cast_to<HudOverlay>(p_hud);
	const opennova::hud::HudLayout &layout = hud != nullptr ? hud->layout() : defaults;
	std::vector<opennova::world::DeployTableZone> zones;
	int team = 0;
	if (p_sim.is_valid() && p_sim->kernel_) {
		const opennova::inmatch::RoleView view = p_sim->role_view();
		opennova::inmatch::deploy_table_zones(view, p_sim->deploy_zone_registry(),
				layout.stance_middle, layout.stance_bad, game_text_lookup(p_gametext), zones);
		team = opennova::inmatch::deploy_list_team(view);
	}
	return opennova::world::fill_deploy_spawn_table(runtime_,
			opennova::world::build_deploy_table_rows(team, zones));
}

Vector2i MenuDriver::deploy_table_click(int p_id, int p_row, int p_state,
		bool p_double_click) const {
	const opennova::world::DeployTableClick click =
			opennova::world::deploy_table_click(runtime_, p_id, p_row, p_state, p_double_click);
	return Vector2i(click.pan_zone_index, click.pick_value);
}

bool MenuDriver::fill_deploy_team_lists(const Ref<Simulation> &p_sim,
		const Ref<RtxtStringFile> &p_gametext) {
	if (p_sim.is_null() || !p_sim->kernel_) return false;
	const opennova::inmatch::RoleView view = p_sim->role_view();
	std::vector<opennova::world::DeployTeamListPlayer> players;
	if (!opennova::inmatch::deploy_team_list_players(view, players)) return false;
	opennova::world::fill_deploy_team_lists(runtime_, opennova::world::deploy_team_lists(players,
			opennova::inmatch::deploy_list_team(view), game_text_lookup(p_gametext)));
	return true;
}

void MenuDriver::apply_deploy_activation(const Ref<Simulation> &p_sim,
		const Ref<RtxtStringFile> &p_gametext) {
	if (p_sim.is_null() || !p_sim->kernel_) return;
	opennova::world::apply_deploy_activation(runtime_, opennova::inmatch::deploy_activation(
			p_sim->role_view(), game_text_lookup(p_gametext)));
}

bool MenuDriver::install_deploy_table_painter(const Ref<Simulation> &p_sim) {
	MenuFrame *frame = frame_();
	const int table = runtime_.find_screen_control("DEATH", "SPAWNPOINTS_TABLE");
	const int index = frame_index(table);
	if (frame == nullptr || index < 0) return false;
	const ObjectID driver_id(get_instance_id());
	const ObjectID sim_id = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
	// The zone labels draw in the HUD label face (world::deploy_table_paint).
	frame->set_hud_label_face(true);
	frame->set_table_cell_painter(index,
			[driver_id, sim_id, table](const opennova::menu::MenuTableCellEvent &event,
					opennova::menu::MenuTableCellCanvas &canvas) {
				std::string name;
				MenuDriver *driver = Object::cast_to<MenuDriver>(ObjectDB::get_instance(driver_id));
				Simulation *sim = Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id));
				if (event.row >= 0 && event.column == 1 && driver != nullptr && sim != nullptr &&
						sim->kernel_) {
					const std::optional<int32_t> value =
							driver->runtime().table_cell_record(table, event.row, 1);
					if (value.has_value())
						name = opennova::world::deploy_table_zone_name(sim->kernel_->world,
								sim->deploy_zone_registry(), *value);
				}
				opennova::world::deploy_table_paint(event, canvas, name);
			});
	return true;
}

bool MenuDriver::deploy_map_moved() {
	return opennova::world::deploy_map_moved(runtime_);
}

// ---- activation / actions --------------------------------------------------------

void MenuDriver::activate(int p_id) { runtime_.activate(p_id); }
void MenuDriver::spin_cycle(int p_id, int p_delta) { runtime_.spin_cycle(p_id, p_delta); }

void MenuDriver::enable_class_rows(int p_id, int p_class_allow_mask) {
	opennova::menu::enable_class_rows(runtime_, p_id, static_cast<uint32_t>(p_class_allow_mask));
}
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

void MenuDriver::on_frame_widget_clicked_(int p_index, int p_part) {
	runtime_.on_widget_clicked(p_index, p_part);
}

void MenuDriver::on_frame_scroll_value_(int p_index, int p_value) {
	runtime_.on_frame_scroll_value(p_index, p_value);
}

void MenuDriver::process_mouse(const Vector2 &p_position, bool p_button_down) {
	runtime_.process_mouse(p_position.x, p_position.y, p_button_down);
}

void MenuDriver::move_mouse(const Vector2 &p_position, bool p_button_down) {
	runtime_.move_mouse(p_position.x, p_position.y, p_button_down);
}

bool MenuDriver::press_mouse(const Vector2 &p_position, bool p_double_click) {
	return runtime_.press_mouse(p_position.x, p_position.y, p_double_click);
}

bool MenuDriver::release_mouse(const Vector2 &p_position) {
	return runtime_.release_mouse(p_position.x, p_position.y);
}

void MenuDriver::pump_mouse() {
	// The device's pointer, frame-local as the events are, until this menu's first message: the
	// pump and the claim's CURSOR from the first frame (D-MNU-79).
	MenuFrame *frame = frame_();
	if (frame != nullptr && frame->is_inside_tree()) {
		const Vector2 at = frame->get_local_mouse_position();
		runtime_.adopt_mouse_state(at.x, at.y,
				Input::get_singleton()->is_mouse_button_pressed(MOUSE_BUTTON_LEFT));
	}
	runtime_.pump_mouse();
	// The menu frame's capture pump: an armed row on the joystick's page polls the device's
	// state every frame (engine OptionsScreen::poll_joystick), so a button already down when
	// the row armed is captured too.
	if (options_.capturing_joystick())
		(void)options_.poll_joystick(runtime_, ControlsModel::joystick_device_bits());
}

bool MenuDriver::take_mouse_event(const Ref<InputEvent> &p_event) {
	// The input sampling the game's mouse callback does, each event as its message (engine
	// MenuRuntime::move_mouse, press_mouse, release_mouse); the pump is pump_mouse's, once a frame.
	if (p_event.is_null()) return false;
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		move_mouse(motion->get_position(), motion->get_button_mask().has_flag(MOUSE_BUTTON_MASK_LEFT));
		return false;
	}
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_null() || button->get_button_index() != MOUSE_BUTTON_LEFT) return false;
	// The platform's double click (Godot's Windows backend sets it from WM_LBUTTONDBLCLK),
	// as retail takes the OS's (engine MenuRuntime::press_mouse).
	if (button->is_pressed()) press_mouse(button->get_position(), button->is_double_click());
	else release_mouse(button->get_position());
	return true;
}

bool MenuDriver::process_wheel(const Vector2 &p_position, int p_steps) {
	return runtime_.process_wheel(p_position.x, p_position.y, p_steps);
}

bool MenuDriver::handle_key_input(const Ref<InputEventKey> &p_event) {
	if (p_event.is_null() || p_event->is_echo() || !p_event->is_pressed()) return false;
	// The Godot key as Windows delivers it: WM_KEYDOWN's virtual-key code (the
	// keypad Enter is VK_RETURN there), then WM_CHAR's character.
	opennova::menu::MenuKeyInput key;
	const godot::Key keycode = p_event->get_keycode();
	key.vk = keycode == KEY_KP_ENTER ? 0x0D : ControlsModel::vk_from_godot_key(static_cast<int>(keycode));
	key.unicode = static_cast<int>(p_event->get_unicode());
	key.printable_keycode = static_cast<int>(keycode);
	key.shift = p_event->is_shift_pressed();
	return runtime_.handle_key(key);
}

void MenuDriver::close_active_combo_popup() { runtime_.close_active_combo_popup(); }
bool MenuDriver::is_combo_popup_open(int p_id) const { return runtime_.is_combo_popup_open(p_id); }
int MenuDriver::get_focused_widget() const { return runtime_.focused_widget(); }

void MenuDriver::tick(int64_t p_time_ms) {
	if (MenuFrame *frame = frame_()) frame->set_time_ms(p_time_ms);
}

void MenuDriver::seed_mission_list(int p_id, const TypedArray<MissionCatalogRow> &p_rows) {
	flow_.seed_missions(runtime_, p_id, mission_choices(p_rows));
}

void MenuDriver::select_mission(int p_id, int p_row, const String &p_fallback) {
	flow_.select_mission(runtime_, p_id, p_row, to_std(p_fallback));
}

String MenuDriver::get_selected_mission() const { return to_gd(flow_.selected_mission()); }

Vector2i MenuDriver::get_selected_campaign() const {
	const opennova::mission_catalog::CampaignWords &words = flow_.selected_campaign();
	return Vector2i(words.campaign, words.campaign_mission);
}

bool MenuDriver::activate_single_player_tab(const String &p_name) {
	return flow_.activate_tab(runtime_, to_std(p_name));
}

void MenuDriver::set_single_player_source(const Ref<PlayerProfiles> &p_profiles,
		const Ref<RtxtStringFile> &p_menu_text) {
	const opennova::Title &title = ResourceRoot::session_title(root_);
	flow_.set_title(&title);
	if (!title.sp_campaign_tabs) return;
	sp_menu_text_ = p_menu_text;
	std::string def;
	if (root_.is_valid()) {
		const PackedByteArray bytes = root_->read_file(to_gd(opennova::campaign::kCampaignFile));
		def.assign(reinterpret_cast<const char *>(bytes.ptr()), static_cast<size_t>(bytes.size()));
	}
	opennova::campaign::CompletionTable progress{};
	if (p_profiles.is_valid()) {
		const auto &table = p_profiles->native().current().campaign_complete;
		for (size_t c = 0; c < progress.size() && c < table.size(); ++c)
			for (size_t m = 0; m < progress[c].size() && m < table[c].size(); ++m)
				progress[c][m] = table[c][m];
	}
	flow_.set_campaign_source(std::move(def),
			sp_menu_text_.is_valid() ? &sp_menu_text_->get_native() : nullptr, progress);
}

void MenuDriver::seed_mod_list(int p_id) {
	const String dir = root_.is_valid() ? root_->get_root_dir() : String();
	if (dir != mods_root_) {
		mods_root_ = dir;
		std::vector<opennova::ExpansionRecord> records;
		if (!dir.is_empty()) records = opennova::vfs_expansion_records(to_std(dir));
		mods_.set_records(std::move(records));
	}
	mods_.populate(runtime_, p_id, root_.is_valid() ? to_std(root_->get_expansion()) : std::string(),
			ResourceRoot::session_title(root_).mod_list_base);
}

String MenuDriver::mod_list_pick(int p_id) const { return to_gd(mods_.pick(runtime_, p_id)); }

bool MenuDriver::request_expansion(const String &p_name, const String &p_current, bool p_packed) {
	return flow_.request_expansion(to_std(p_name), to_std(p_current), p_packed) !=
			opennova::menu::MenuFlow::ExpansionPick::NeedsPackedRoot;
}

String MenuDriver::take_expansion_reload() { return to_gd(flow_.take_expansion_reload()); }

void MenuDriver::seed_host_pool(const TypedArray<MissionCatalogRow> &p_rows) {
	host_dialog_.seed(runtime_, mission_choices(p_rows), ResourceRoot::session_title(root_));
}

void MenuDriver::add_host_missions(const Ref<RtxtStringFile> &p_text) {
	host_dialog_.add_selected(runtime_, game_text_lookup(p_text));
}

PackedStringArray MenuDriver::selected_host_missions() const {
	return to_gd_strings(host_dialog_.selected_missions());
}

PackedInt32Array MenuDriver::selected_host_launch_options() const {
	PackedInt32Array out;
	for (const int32_t option : host_dialog_.selected_launch_options()) out.push_back(option);
	return out;
}

bool MenuDriver::is_front_options_screen(const String &p_screen) {
	return opennova::menu::OptionsScreen::is_front_screen(to_std(p_screen));
}

String MenuDriver::screen_command(const String &p_screen, const String &p_name) const {
	return String(opennova::menu::menu_command_token(
			commands_.command_of(to_std(p_screen), to_std(p_name))));
}

PackedInt32Array MenuDriver::command_mission_lists() const {
	PackedInt32Array out;
	for (const int id : commands_.mission_lists()) out.push_back(id);
	return out;
}

PackedInt32Array MenuDriver::command_mod_lists() const {
	PackedInt32Array out;
	for (const int id : commands_.mod_lists()) out.push_back(id);
	return out;
}

PackedStringArray MenuDriver::command_names(const String &p_set) {
	opennova::menu::MenuNameSet set = opennova::menu::MenuNameSet::kCount;
	if (!opennova::menu::menu_name_set_from_token(to_std(p_set), set)) return PackedStringArray();
	return to_gd_strings(opennova::menu::menu_name_set(set));
}

namespace {

// The profile's current record, or null with no profile.
opennova::playersav::ProfileRecord *current_record(const Ref<PlayerProfiles> &p_profiles) {
	return p_profiles.is_valid() ? &p_profiles->native().current() : nullptr;
}

} // namespace

void MenuDriver::prepare_options(const Ref<PlayerProfiles> &p_profiles) {
	// The working records follow the profile's title's binding catalog (the launch's).
	options_.prepare(runtime_, current_record(p_profiles),
			opennova::controls::title_catalog(p_profiles.is_valid()
							? p_profiles->native().title()
							: ResourceRoot::session_title(Ref<ResourceRoot>())));
}

void MenuDriver::apply_options_policy(const Ref<PlayerProfiles> &p_profiles) {
	options_.apply_policy(runtime_);
	options_.apply_title(runtime_, ResourceRoot::session_title(root_));
	options_.seed_profile(runtime_, current_record(p_profiles));
}

int MenuDriver::activate_options(const Ref<PlayerProfiles> &p_profiles, int p_id) {
	return options_.activate(runtime_, current_record(p_profiles), p_id);
}

String MenuDriver::options_control_text(int p_action, int p_device) const {
	return String::utf8(options_.bindings().control_text(p_action,
			static_cast<opennova::controls::Device>(p_device)).c_str());
}

int MenuDriver::consume_options_input(const Ref<InputEvent> &p_event) {
	if (p_event.is_null()) return 0;
	opennova::menu::RemapInput input;
	const Ref<InputEventKey> key = p_event;
	const Ref<InputEventMouseButton> mouse = p_event;
	const Ref<InputEventJoypadButton> pad = p_event;
	if (key.is_valid()) {
		input.kind = opennova::menu::RemapInput::Kind::Key;
		input.pressed = key->is_pressed();
		// The layout's VK (Godot's keycode), as every key leg reads it; a synthetic event with
		// none falls back to its physical key.
		const Key code = key->get_keycode() != Key::KEY_NONE ? key->get_keycode()
				: key->get_physical_keycode();
		input.escape = code == Key::KEY_ESCAPE;
		input.vk = ControlsModel::vk_from_godot_key(static_cast<int>(code));
		input.ctrl = key->is_ctrl_pressed();
		input.shift = key->is_shift_pressed();
		input.repeat = key->is_echo();
	} else if (mouse.is_valid()) {
		input.kind = opennova::menu::RemapInput::Kind::Mouse;
		input.pressed = mouse->is_pressed();
		input.mouse_mask = static_cast<uint16_t>(ControlsModel::mouse_mask_from_godot_button(
				static_cast<int>(mouse->get_button_index())));
	} else if (pad.is_valid()) {
		// Swallowed while the joystick's page captures: the capture is the pump's poll
		// (pump_mouse, engine OptionsScreen::poll_joystick).
		input.kind = opennova::menu::RemapInput::Kind::Joystick;
		input.pressed = pad->is_pressed();
	}
	return options_.consume(runtime_, input);
}

int MenuDriver::update_player_info_avatars(const Ref<AvatarDatabase> &p_db, int p_change,
		int p_value, int p_team, int p_voice, const Ref<RtxtStringFile> &p_gameui,
		const Ref<RtxtStringFile> &p_menutxt) {
	const opennova::avatars::AvatarsFile empty{};
	return avatars_.update(runtime_, p_db.is_valid() ? p_db->native_file() : empty,
			static_cast<opennova::menu::PlayerInfoAvatars::Change>(p_change), p_value, p_team, p_voice,
			game_text_lookup(p_gameui), game_text_lookup(p_menutxt), ResourceRoot::session_title(root_));
}

String MenuDriver::player_info_combo_list_event() const {
	return String(opennova::menu::player_info_combo_list_event(ResourceRoot::session_title(root_)));
}

bool MenuDriver::player_info_preview_hovers() const {
	return ResourceRoot::session_title(root_).player_info_preview_hover;
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
	ClassDB::bind_method(D_METHOD("seed_player_info_avatars", "nationality", "division"),
			&MenuDriver::seed_player_info_avatars);
	ClassDB::bind_method(D_METHOD("player_info_combo_list_event"), &MenuDriver::player_info_combo_list_event);
	ClassDB::bind_method(D_METHOD("player_info_preview_hovers"), &MenuDriver::player_info_preview_hovers);
	ClassDB::bind_method(D_METHOD("player_info_nationality_rows"), &MenuDriver::player_info_nationality_rows);
	ClassDB::bind_method(D_METHOD("player_info_avatar_nationality"), &MenuDriver::player_info_avatar_nationality);
	ClassDB::bind_method(D_METHOD("player_info_avatar_division"), &MenuDriver::player_info_avatar_division);
	ClassDB::bind_method(D_METHOD("player_info_avatar_preview_changed"), &MenuDriver::player_info_avatar_preview_changed);
	ClassDB::bind_method(D_METHOD("player_info_avatar_combo", "db"), &MenuDriver::player_info_avatar_combo);
	ClassDB::bind_method(D_METHOD("preview_player_info_voice", "db", "voice"), &MenuDriver::preview_player_info_voice);
	ClassDB::bind_method(D_METHOD("clear_mission_rows"), &MenuDriver::clear_mission_rows);
	ClassDB::bind_method(D_METHOD("seed_mission_list", "id", "rows"), &MenuDriver::seed_mission_list);
	ClassDB::bind_method(D_METHOD("select_mission", "id", "row", "fallback"), &MenuDriver::select_mission);
	ClassDB::bind_method(D_METHOD("activate_mission", "id", "row"), &MenuDriver::activate_mission);
	ClassDB::bind_method(D_METHOD("get_selected_mission"), &MenuDriver::get_selected_mission);
	ClassDB::bind_method(D_METHOD("get_selected_campaign"), &MenuDriver::get_selected_campaign);
	ClassDB::bind_method(D_METHOD("set_single_player_source", "profiles", "menu_text"),
			&MenuDriver::set_single_player_source);
	ClassDB::bind_method(D_METHOD("select_campaign", "id", "row"), &MenuDriver::select_campaign);
	ClassDB::bind_method(D_METHOD("activate_single_player_tab", "name"),
			&MenuDriver::activate_single_player_tab);
	ClassDB::bind_method(D_METHOD("clear_selected_mission"), &MenuDriver::clear_selected_mission);
	ClassDB::bind_method(D_METHOD("seed_mod_list", "id"), &MenuDriver::seed_mod_list);
	ClassDB::bind_method(D_METHOD("select_mod", "id", "row"), &MenuDriver::select_mod);
	ClassDB::bind_method(D_METHOD("mod_list_pick", "id"), &MenuDriver::mod_list_pick);
	ClassDB::bind_method(D_METHOD("request_expansion", "name", "current", "packed"), &MenuDriver::request_expansion);
	ClassDB::bind_method(D_METHOD("has_pending_expansion_reload"), &MenuDriver::has_pending_expansion_reload);
	ClassDB::bind_method(D_METHOD("take_expansion_reload"), &MenuDriver::take_expansion_reload);
	ClassDB::bind_method(D_METHOD("seed_host_pool", "rows"), &MenuDriver::seed_host_pool);
	ClassDB::bind_method(D_METHOD("filter_host_missions"), &MenuDriver::filter_host_missions);
	ClassDB::bind_method(D_METHOD("add_host_missions", "text"), &MenuDriver::add_host_missions);
	ClassDB::bind_method(D_METHOD("remove_host_missions"), &MenuDriver::remove_host_missions);
	ClassDB::bind_method(D_METHOD("can_start_host"), &MenuDriver::can_start_host);
	ClassDB::bind_method(D_METHOD("selected_host_missions"), &MenuDriver::selected_host_missions);
	ClassDB::bind_method(D_METHOD("toggle_host_mission_switch", "row"),
			&MenuDriver::toggle_host_mission_switch);
	ClassDB::bind_method(D_METHOD("selected_host_launch_options"),
			&MenuDriver::selected_host_launch_options);
	ClassDB::bind_method(D_METHOD("prepare_options", "profiles"), &MenuDriver::prepare_options);
	ClassDB::bind_method(D_METHOD("is_options_surface"), &MenuDriver::is_options_surface);
	ClassDB::bind_method(D_METHOD("apply_options_policy", "profiles"), &MenuDriver::apply_options_policy);
	ClassDB::bind_static_method("MenuDriver", D_METHOD("command_names", "set"), &MenuDriver::command_names);
	ClassDB::bind_method(D_METHOD("claim_screen", "screen"), &MenuDriver::claim_screen);
	ClassDB::bind_method(D_METHOD("release_screen"), &MenuDriver::release_screen);
	ClassDB::bind_method(D_METHOD("wire_commands", "in_mission"), &MenuDriver::wire_commands);
	ClassDB::bind_method(D_METHOD("screen_command", "screen", "name"), &MenuDriver::screen_command);
	ClassDB::bind_method(D_METHOD("command_owner"), &MenuDriver::command_owner);
	ClassDB::bind_method(D_METHOD("command_mission_lists"), &MenuDriver::command_mission_lists);
	ClassDB::bind_method(D_METHOD("command_mod_lists"), &MenuDriver::command_mod_lists);
	ClassDB::bind_static_method("MenuDriver", D_METHOD("is_front_options_screen", "screen"),
			&MenuDriver::is_front_options_screen);
	BIND_ENUM_CONSTANT(MISSION_SCREEN_NONE);
	BIND_ENUM_CONSTANT(MISSION_SCREEN_INGAME);
	BIND_ENUM_CONSTANT(MISSION_SCREEN_WEAPON);
	BIND_ENUM_CONSTANT(MISSION_SCREEN_COMMAND_MAP);
	BIND_ENUM_CONSTANT(MISSION_SCREEN_VEHICLE);
	BIND_ENUM_CONSTANT(MISSION_SCREEN_STAT);
	BIND_ENUM_CONSTANT(MISSION_SCREEN_DEATH);
	ClassDB::bind_static_method("MenuDriver", D_METHOD("mission_screen_open", "screen"),
			&MenuDriver::mission_screen_open);
	ClassDB::bind_static_method("MenuDriver",
			D_METHOD("mission_screen_passes_input", "screen", "pass_through_word"),
			&MenuDriver::mission_screen_passes_input);
	ClassDB::bind_static_method("MenuDriver",
			D_METHOD("mission_screen_holds_input", "screen", "pass_through_word"),
			&MenuDriver::mission_screen_holds_input);
	ClassDB::bind_method(D_METHOD("activate_options", "profiles", "id"), &MenuDriver::activate_options);
	ClassDB::bind_method(D_METHOD("arm_options_remap", "id", "row"), &MenuDriver::arm_options_remap);
	ClassDB::bind_method(D_METHOD("press_options_row", "id", "row"),
			&MenuDriver::press_options_row);
	ClassDB::bind_method(D_METHOD("consume_options_input", "event"), &MenuDriver::consume_options_input);
	ClassDB::bind_method(D_METHOD("end_options_remap", "refill"), &MenuDriver::end_options_remap);
	ClassDB::bind_method(D_METHOD("options_control_text", "action", "device"),
			&MenuDriver::options_control_text);
	ClassDB::bind_method(D_METHOD("show_ingame_main"), &MenuDriver::show_ingame_main);
	BIND_ENUM_CONSTANT(OPTIONS_CONSUMED);
	BIND_ENUM_CONSTANT(OPTIONS_COMMIT_PREVIEW);
	BIND_ENUM_CONSTANT(OPTIONS_RESTORE_PREVIEW);
	BIND_ENUM_CONSTANT(OPTIONS_APPLY_CONTROLS);
	BIND_ENUM_CONSTANT(OPTIONS_DISCARD_EDITS);
	BIND_ENUM_CONSTANT(OPTIONS_RELOAD_CROSSHAIR_ALPHA);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_SOUND_FX_VOLUME);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_DIALOG_VOLUME);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_MUSIC_VOLUME);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_CROSSHAIR_STYLE);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_CROSSHAIR_COLOR);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_CROSSHAIR_SPREAD);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_KEYBOARD_TIPS);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_GAMEPLAY_TIPS);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_ASPECT_MODE);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_OBJECT_POLYDETAIL);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_TEXFILTER_LEVEL);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_PARTICLE_DENSITY);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_TEXCOMPRESSION_LEVEL);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_TERRAIN_TEXDETAIL);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_OBJECT_TEXDETAIL);
	BIND_ENUM_CONSTANT(OPTIONS_WORD_COUNT);
	ClassDB::bind_method(D_METHOD("seed_options_words", "words"), &MenuDriver::seed_options_words);
	ClassDB::bind_method(D_METHOD("options_word_edit", "id", "value", "words"),
			&MenuDriver::options_word_edit);
	ClassDB::bind_method(D_METHOD("attach", "frame", "audio"), &MenuDriver::attach);
	ClassDB::bind_method(D_METHOD("set_music_director", "director"),
			&MenuDriver::set_music_director);
	ClassDB::bind_method(D_METHOD("set_music_var_index", "index"),
			&MenuDriver::set_music_var_index);
	ClassDB::bind_method(D_METHOD("get_frame"), &MenuDriver::get_frame);
	ClassDB::bind_method(D_METHOD("get_menu_file"), &MenuDriver::get_menu_file);
	ClassDB::bind_method(D_METHOD("get_current_screen"), &MenuDriver::get_current_screen);
	ClassDB::bind_method(D_METHOD("get_applied_pointer"), &MenuDriver::get_applied_pointer);
	ClassDB::bind_method(D_METHOD("widget_screen_of", "id"), &MenuDriver::widget_screen_of);
	ClassDB::bind_method(D_METHOD("open_document", "doc", "root", "style", "menu_file",
								 "target_screen"),
			&MenuDriver::open_document, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("document"), &MenuDriver::document);
	ClassDB::bind_method(D_METHOD("get_screen_names"), &MenuDriver::get_screen_names);
	ClassDB::bind_method(D_METHOD("show_screen", "name"), &MenuDriver::show_screen);
	ClassDB::bind_method(D_METHOD("navigate_to_screen", "name"), &MenuDriver::navigate_to_screen);
	ClassDB::bind_method(D_METHOD("pop_screen"), &MenuDriver::pop_screen);
	ClassDB::bind_method(D_METHOD("push_screen_history", "file", "screen"),
			&MenuDriver::push_screen_history);
	ClassDB::bind_method(D_METHOD("pop_screen_history"), &MenuDriver::pop_screen_history);
	ClassDB::bind_method(D_METHOD("clear_screen_history"), &MenuDriver::clear_screen_history);
	ClassDB::bind_method(D_METHOD("trim_screen_history"), &MenuDriver::trim_screen_history);
	ClassDB::bind_method(D_METHOD("get_screen_history_depth"),
			&MenuDriver::get_screen_history_depth);
	ClassDB::bind_method(D_METHOD("get_screen_history"), &MenuDriver::get_screen_history);
	ClassDB::bind_method(D_METHOD("leave_menu_mode"), &MenuDriver::leave_menu_mode);
	ClassDB::bind_method(D_METHOD("return_to_menu_mode"), &MenuDriver::return_to_menu_mode);

	ClassDB::bind_method(D_METHOD("widget_id", "name"), &MenuDriver::widget_id);
	ClassDB::bind_method(D_METHOD("widget_name_of", "id"), &MenuDriver::widget_name_of);
	ClassDB::bind_method(D_METHOD("widget_kind_of", "id"), &MenuDriver::widget_kind_of);
	ClassDB::bind_method(D_METHOD("has_widget", "name"), &MenuDriver::has_widget);
	ClassDB::bind_method(D_METHOD("frame_index", "id"), &MenuDriver::frame_index);
	ClassDB::bind_method(D_METHOD("widget_frame_rect", "id"), &MenuDriver::widget_frame_rect);
	ClassDB::bind_method(D_METHOD("widget_local_rect", "id"), &MenuDriver::widget_local_rect);
	ClassDB::bind_method(D_METHOD("set_widget_rect", "id", "rect"), &MenuDriver::set_widget_rect);
	ClassDB::bind_method(D_METHOD("focus_widget", "id"), &MenuDriver::focus_widget);

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

	ClassDB::bind_method(D_METHOD("table_row_count", "id"), &MenuDriver::table_row_count);
	ClassDB::bind_method(D_METHOD("table_cell_text", "id", "row", "col"),
			&MenuDriver::table_cell_text);
	ClassDB::bind_method(D_METHOD("table_select_row", "id", "row", "additive"),
			&MenuDriver::table_select_row, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("table_sort_column", "id"), &MenuDriver::table_sort_column);
	ClassDB::bind_method(D_METHOD("fill_stat_results", "id", "columns", "rows", "local_slot"),
			&MenuDriver::fill_stat_results);
	ClassDB::bind_method(D_METHOD("filter_stat_results", "id", "tab"),
			&MenuDriver::filter_stat_results);
	ClassDB::bind_method(D_METHOD("hide_stat_done", "sim"), &MenuDriver::hide_stat_done);
	ClassDB::bind_method(D_METHOD("show_stat_done", "sim"), &MenuDriver::show_stat_done);
	ClassDB::bind_method(D_METHOD("stat_done_pressed", "sim"), &MenuDriver::stat_done_pressed);
	ClassDB::bind_method(D_METHOD("set_deploy_list_items", "id", "rows"),
			&MenuDriver::set_deploy_list_items);
	ClassDB::bind_method(D_METHOD("fill_deploy_spawn_table", "sim", "hud", "gametext"),
			&MenuDriver::fill_deploy_spawn_table);
	ClassDB::bind_method(D_METHOD("deploy_table_click", "id", "row", "state", "double_click"),
			&MenuDriver::deploy_table_click);
	ClassDB::bind_method(D_METHOD("deploy_map_moved"), &MenuDriver::deploy_map_moved);
	ClassDB::bind_method(D_METHOD("install_deploy_table_painter", "sim"),
			&MenuDriver::install_deploy_table_painter);
	ClassDB::bind_method(D_METHOD("apply_deploy_activation", "sim", "gametext"),
			&MenuDriver::apply_deploy_activation);
	ClassDB::bind_method(D_METHOD("fill_deploy_team_lists", "sim", "gametext"),
			&MenuDriver::fill_deploy_team_lists);
	ClassDB::bind_method(D_METHOD("table_cell_value", "id", "row", "col"),
			&MenuDriver::table_cell_value);
	ClassDB::bind_method(D_METHOD("table_selected_rows", "id"), &MenuDriver::table_selected_rows);
	ClassDB::bind_method(D_METHOD("set_widget_clip_rect", "id", "rect"),
			&MenuDriver::set_widget_clip_rect);

	ClassDB::bind_method(D_METHOD("activate", "id"), &MenuDriver::activate);
	ClassDB::bind_method(D_METHOD("spin_cycle", "id", "delta"), &MenuDriver::spin_cycle);
	ClassDB::bind_method(D_METHOD("is_item_enabled", "id", "row"), &MenuDriver::is_item_enabled);
	ClassDB::bind_method(D_METHOD("enable_class_rows", "id", "class_allow_mask"),
			&MenuDriver::enable_class_rows);
	ClassDB::bind_method(D_METHOD("spin_value_attr", "id"), &MenuDriver::spin_value_attr);
	ClassDB::bind_method(D_METHOD("dispatch_action_row", "action"),
			&MenuDriver::dispatch_action_row);
	ClassDB::bind_method(D_METHOD("play_widget_sound", "trigger", "file"),
			&MenuDriver::play_widget_sound);

	ClassDB::bind_method(D_METHOD("process_mouse", "position", "button_down"),
			&MenuDriver::process_mouse);
	ClassDB::bind_method(D_METHOD("pump_mouse"), &MenuDriver::pump_mouse);
	ClassDB::bind_method(D_METHOD("take_mouse_event", "event"), &MenuDriver::take_mouse_event);
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
	ClassDB::bind_method(D_METHOD("_on_frame_widget_clicked", "index", "part"),
			&MenuDriver::on_frame_widget_clicked_);
	ClassDB::bind_method(D_METHOD("_on_frame_scroll_value", "index", "value"),
			&MenuDriver::on_frame_scroll_value_);

	ADD_SIGNAL(MethodInfo("screen_changed", PropertyInfo(Variant::STRING, "screen_name")));
	ADD_SIGNAL(MethodInfo("menu_requested", PropertyInfo(Variant::STRING, "file"),
			PropertyInfo(Variant::STRING, "target_screen")));
	// POP_SCREEN with the document's own history empty: the shell's cross-file
	// history pops, else nothing happens.
	ADD_SIGNAL(MethodInfo("pop_requested"));
	// A URL action: the trimmed URL, and whether retail opens the external
	// browser (EXTERNAL_BROWSER, or external_browser=1 / commercial_browser=1).
	ADD_SIGNAL(MethodInfo("url_requested", PropertyInfo(Variant::STRING, "url"),
			PropertyInfo(Variant::BOOL, "external")));
	// GLB_FILTER / GLB_FILTER_NUM from an edit's keys, to the control that
	// receives it (a GLB_TABLE, whose runtime the shell does not host).
	ADD_SIGNAL(MethodInfo("filter_requested", PropertyInfo(Variant::INT, "receiver_id"),
			PropertyInfo(Variant::INT, "column"), PropertyInfo(Variant::STRING, "text"),
			PropertyInfo(Variant::BOOL, "numeric"), PropertyInfo(Variant::STRING, "test")));
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
	// An edit's Enter commit, after the focus released (event 0x7000002).
	ADD_SIGNAL(MethodInfo("edit_committed", PropertyInfo(Variant::INT, "id"),
			PropertyInfo(Variant::STRING, "widget_name")));
	// An APPMSG row's message the game answers, as its command token ("update";
	// engine menu_commands.h app_message_command).
	ADD_SIGNAL(MethodInfo("app_command_requested", PropertyInfo(Variant::STRING, "command")));
	// A press on a table's data row, after the table's own selection write
	// (the 0x8000001 cell event): the row's new state and the column's cell
	// value; `double_click` for the double-click form.
	ADD_SIGNAL(MethodInfo("table_cell_clicked", PropertyInfo(Variant::INT, "id"),
			PropertyInfo(Variant::STRING, "widget_name"), PropertyInfo(Variant::INT, "row"),
			PropertyInfo(Variant::INT, "column"), PropertyInfo(Variant::INT, "state"),
			PropertyInfo(Variant::INT, "cell_value"), PropertyInfo(Variant::BOOL, "double_click")));
}

} // namespace godot
