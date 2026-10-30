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
#include "network/novaworld_client.h"
#include "resource_index/launch_flags.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/deploy_rows.h"
#include "simulation/hud_view_records.h"
#include "util/string_convert.h"

#include <runtime/inmatch/stat_screen_feed.h>
#include <runtime/inmatch/vehicle_menu.h> // the ITEM_DISPLAY preview's view
#include <runtime/world/occlusion.h>      // render_matrix_from_pose
#include <runtime/menu/menu_commands.h>
#include <runtime/menu/menu_credits.h>
#include <runtime/world/deploy_screen_feed.h>

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_joypad_button.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
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
				to_std(row->get_briefing()), static_cast<uint32_t>(row->get_game_type())});
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
			f->configure(owner_->current_document_(), to_gd(screen), owner_->root_, owner_->style_);
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

// ---- the shell's host: the device legs of the engine menu shell --------------------

class MenuDriver::ShellHost : public opennova::menu::MenuShellHost {
	MenuDriver *owner_;

public:
	explicit ShellHost(MenuDriver *p_owner) : owner_(p_owner) {}

	// The visual menu assets load through the VFS by name, so they resolve from PFF archives at
	// runtime; their textures and fonts through the root the frame is given.
	std::shared_ptr<const opennova::mnu::Document> read_menu(const std::string &file) override {
		const Ref<ResourceRoot> &root = owner_->root_;
		if (root.is_null() || file.empty()) return nullptr;
		const PackedByteArray bytes = root->read_file(to_gd(file));
		if (bytes.is_empty()) return nullptr;
		Ref<MnuDocument> doc;
		doc.instantiate();
		if (doc->load_from_bytes(bytes) != OK) return nullptr;
		owner_->shell_documents_[&doc->get_native()] = ObjectID(doc->get_instance_id());
		// The shell's cache holds the Resource for as long as it holds the record.
		auto holder = std::make_shared<Ref<MnuDocument>>(doc);
		return std::shared_ptr<const opennova::mnu::Document>(holder, &(*holder)->get_native());
	}
	std::vector<opennova::menu::MissionChoice> missions() override {
		return mission_choices(MissionCatalog::rows(owner_->root_));
	}
	// The records of the bound root's folder, scanned once per folder, as the game scans once at boot.
	std::vector<opennova::ExpansionRecord> expansions() override {
		const String dir = owner_->root_.is_valid() ? owner_->root_->get_root_dir() : String();
		if (dir != owner_->mods_root_) {
			owner_->mods_root_ = dir;
			owner_->mods_records_.clear();
			if (!dir.is_empty()) owner_->mods_records_ = opennova::vfs_expansion_records(to_std(dir));
		}
		return owner_->mods_records_;
	}
	std::string expansion() override {
		return owner_->root_.is_valid() ? to_std(owner_->root_->get_expansion()) : std::string();
	}
	bool packed_install() override { return owner_->root_.is_valid() && owner_->root_->is_runtime_mount(); }
	bool update_waiting() override { return owner_->update_waiting_; }
	opennova::playersav::ProfileRecord *profile_record() override {
		return owner_->profiles_.is_valid() ? &owner_->profiles_->native().current() : nullptr;
	}
	bool player_words(opennova::menu::OptionsScreen::Words &out) override {
		if (!owner_->player_words_get_.is_valid()) return false;
		const PackedInt32Array words = owner_->player_words_get_.call();
		if (words.size() < static_cast<int64_t>(out.size())) return false;
		for (size_t i = 0; i < out.size(); ++i) out[i] = words[static_cast<int64_t>(i)];
		return true;
	}
	void set_player_words(const opennova::menu::OptionsScreen::Words &words) override {
		if (!owner_->player_words_set_.is_valid()) return;
		PackedInt32Array out;
		for (const int32_t word : words) out.push_back(word);
		owner_->player_words_set_.call(out);
	}
	void leave(opennova::menu::ShellExit exit, opennova::menu::MenuCommand, const std::string &,
			const std::string &text) override {
		owner_->emit_signal("shell_exit_requested", static_cast<int>(exit), to_gd(text));
	}
	bool launch_update(const std::string &) override {
		return owner_->update_launcher_.is_valid() && bool(owner_->update_launcher_.call());
	}
	// The OS browser takes a link with its scheme: a bare authored one (the splash's
	// "www.novalogic.com/...") gains https://, which Windows' shell open infers itself.
	void open_browser(const std::string &url) override {
		String target = to_gd(url);
		if (!(target.begins_with("http://") || target.begins_with("https://"))) target = "https://" + target;
		OS::get_singleton()->shell_open(target);
	}
	void notice(opennova::menu::ShellNotice notice, const std::string &, const std::string &text,
			const std::string &text2) override {
		using Notice = opennova::menu::ShellNotice;
		switch (notice) {
			case Notice::MenuMissing:
				UtilityFunctions::push_warning("MenuShell: could not load menu '", to_gd(text), "'");
				return;
			case Notice::ScreenMissing:
				UtilityFunctions::push_warning("MenuShell: menu '", to_gd(text), "' has no screen '",
						to_gd(text2), "'");
				return;
			case Notice::NothingSelected:
				UtilityFunctions::push_warning("MenuShell: start pressed with no mission selected");
				return;
			case Notice::ExpansionNeedsPackedInstall:
				UtilityFunctions::push_warning(
						"MenuShell: expansions need a packed game install; the loose mount stands");
				return;
			default:
				return;
		}
	}
	void controls_accepted() override { owner_->emit_signal("controls_accepted"); }
	void crosshair_reload() override { owner_->emit_signal("crosshair_reload_requested"); }
	void save_profile() override { owner_->emit_signal("profile_save_requested"); }
	void save_cookie_jar() override { NovaWorldClient::save_cookie_jar(); }
	void forget_gate_response() override { NovaWorldClient::forget_gate_response(); }
	// The reload closes every archive before it mounts the pick, so the pick's text-override table
	// loads as at boot (docs/vfs/vfs-pff-mount-re.md, "Expansions" item 1): the root mounts from
	// cleared, not as the join's switch does over its open archives. A failed mount clears the root,
	// so the previous game is mounted again.
	bool mount_game(const std::string &name, const std::string &previous) override {
		const Ref<ResourceRoot> &root = owner_->root_;
		if (root.is_null()) return false;
		const String dir = root->get_root_dir();
		root->clear();
		if (root->mount_runtime(dir, to_gd(name), LaunchFlags::loose_override_enabled()) == OK) return true;
		UtilityFunctions::push_warning("MenuShell: could not mount expansion '", to_gd(name), "': ",
				root->get_last_error());
		root->mount_runtime(dir, to_gd(previous), LaunchFlags::loose_override_enabled());
		return false;
	}
	void reload_shell_resources() override { owner_->emit_signal("shell_resources_requested"); }
	void game_reloaded() override { owner_->emit_signal("game_reloaded"); }
};

// ---- lifecycle -----------------------------------------------------------------

MenuDriver::MenuDriver() :
		host_(std::make_unique<ShellHost>(this)), seam_(std::make_unique<FrameSeam>(this)) {
	// The runtime's events reach the shell first (its own handling and its screen controller's), then
	// this device half (on_runtime_event_).
	shell_.set_host(host_.get());
	shell_.set_sink([this](const opennova::menu::MenuEvent &p_event) {
		on_runtime_event_(p_event);
	});
}

MenuDriver::~MenuDriver() {
	shell_.set_sink(nullptr);
	shell_.release();
	shell_.set_host(nullptr);
}

Ref<MnuDocument> MenuDriver::current_document_() const {
	const opennova::mnu::Document *native = runtime_.document();
	if (native != nullptr && shell_.document()) {
		const auto found = shell_documents_.find(native);
		if (found != shell_documents_.end())
			return Ref<MnuDocument>(Object::cast_to<MnuDocument>(ObjectDB::get_instance(found->second)));
	}
	return doc_;
}

const opennova::menu::MenuCommands &MenuDriver::commands_() const {
	return shell_.document() ? shell_.commands() : direct_commands_;
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
	direct_commands_ = opennova::menu::MenuCommands();
	// The STARTUP label's line (D-MNU-76): the launch's game and its installed release, ONE's
	// build and the protocol it speaks (LaunchFlags::version_line).
	runtime_.set_version_line(to_std(LaunchFlags::version_line()));
	return shell_.show_document(doc_.is_valid() ? &doc_->get_native() : nullptr,
			to_std(p_menu_file), to_std(p_target_screen));
}

// ---- the menu shell ------------------------------------------------------------------

void MenuDriver::set_shell_sources(const Ref<ResourceRoot> &p_root, const Ref<MnsStyleSheet> &p_style) {
	root_ = p_root;
	style_ = p_style;
}

void MenuDriver::set_player_profiles(const Ref<PlayerProfiles> &p_profiles) {
	profiles_ = p_profiles;
}

void MenuDriver::set_menu_files(const String &p_main_menu, const String &p_ingame_menu) {
	shell_.set_menu_files(to_std(p_main_menu), to_std(p_ingame_menu));
}

void MenuDriver::set_player_words_source(const Callable &p_get, const Callable &p_set) {
	player_words_get_ = p_get;
	player_words_set_ = p_set;
}

void MenuDriver::set_update_waiting(bool p_waiting) {
	update_waiting_ = p_waiting;
	shell_.apply_update_gate();
}

bool MenuDriver::enter_menu_mode(bool p_root_changed) {
	runtime_.set_version_line(to_std(LaunchFlags::version_line()));
	if (p_root_changed) shell_documents_.clear();
	return shell_.enter_menu_mode(p_root_changed);
}

bool MenuDriver::open_ingame_menu() {
	runtime_.set_version_line(to_std(LaunchFlags::version_line()));
	return shell_.open_ingame();
}

bool MenuDriver::open_menu(const String &p_file, const String &p_screen) {
	runtime_.set_version_line(to_std(LaunchFlags::version_line()));
	return shell_.open_menu(to_std(p_file), to_std(p_screen));
}

String MenuDriver::get_current_menu_file() const { return to_gd(shell_.file()); }

void MenuDriver::add_screen(const Ref<MenuScreenBinding> &p_screen) {
	if (p_screen.is_null() || p_screen->screen_controller() == nullptr) return;
	for (const Ref<MenuScreenBinding> &held : screens_)
		if (held == p_screen) return;
	screens_.push_back(p_screen);
	shell_.add_controller(p_screen->screen_controller());
}

bool MenuDriver::claim_screen(const Ref<MenuScreenBinding> &p_screen) {
	if (p_screen.is_null() || p_screen->screen_controller() == nullptr) return false;
	released_ = claimed_;
	claimed_ = p_screen;
	return shell_.claim(p_screen->screen_controller());
}

void MenuDriver::release_screen() {
	// A controller may ask its own release from inside its event (a CANCEL that closes the screen): its
	// binding stays alive until the next claim or release, past the dispatch on the stack.
	released_ = claimed_;
	claimed_.unref();
	shell_.release();
}

PackedStringArray MenuDriver::get_screen_names() const {
	return to_gd_strings(runtime_.screen_names());
}

bool MenuDriver::show_screen(const String &p_name) { return runtime_.show_screen(to_std(p_name)); }
bool MenuDriver::navigate_to_screen(const String &p_name) {
	return runtime_.navigate_to_screen(to_std(p_name));
}
bool MenuDriver::pop_screen() { return runtime_.pop_screen(); }

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
			// screen controllers' controls by name; an APPMSG row's message the shell answers
			// (engine menu_commands.h app_message_command).
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
	const opennova::mnu::Document *native = runtime_.document();
	return native == nullptr || native->source_encoding == opennova::mnu::SourceEncoding::CodePage;
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
	const Ref<MnuDocument> doc = current_document_();
	return doc.is_valid() ? doc->get_widget_actions(p_id) : TypedArray<MnuActionRow>();
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
	runtime_.set_widget_items(p_id, opennova::world::deploy_list_items(rows));
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
	shell_.poll_options_joystick(ControlsModel::joystick_device_bits());
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

String MenuDriver::get_selected_mission() const { return to_gd(shell_.selected_mission()); }

void MenuDriver::seed_host_pool(const TypedArray<MissionCatalogRow> &p_rows) {
	host_dialog_.seed(runtime_, mission_choices(p_rows));
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

String MenuDriver::screen_command(const String &p_screen, const String &p_name) const {
	return String(opennova::menu::menu_command_token(
			commands_().command_of(to_std(p_screen), to_std(p_name))));
}

String MenuDriver::options_control_text(int p_action, int p_device) const {
	return String::utf8(shell_.options().bindings().control_text(p_action,
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
	return shell_.consume_options_input(input);
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

namespace {
// A Godot length as 16.16, truncated (the engine's box words).
int32_t preview_q16(real_t p_value) {
	return static_cast<int32_t>(static_cast<double>(p_value) * 65536.0);
}
} // namespace

Vector3 MenuDriver::vehicle_preview_eye(const AABB &p_bounds) {
	const Vector3 lo = p_bounds.position;
	const Vector3 hi = p_bounds.position + p_bounds.size;
	const int32_t box_min[3] = {preview_q16(lo.x), preview_q16(lo.y), preview_q16(lo.z)};
	const int32_t box_max[3] = {preview_q16(hi.x), preview_q16(hi.y), preview_q16(hi.z)};
	const opennova::inmatch::VehiclePreviewView view =
			opennova::inmatch::vehicle_preview_view(box_min, box_max, 0);
	// Render float (x south, y up, z east) to Godot (x east, y up, z south).
	return Vector3(view.eye[2], view.eye[1], view.eye[0]);
}

Basis MenuDriver::vehicle_preview_model_basis(int64_t p_tick_ms) {
	const int32_t zero[3] = {0, 0, 0};
	const opennova::inmatch::VehiclePreviewView view = opennova::inmatch::vehicle_preview_view(
			zero, zero, static_cast<uint32_t>(p_tick_ms));
	const opennova::world::RenderMatrix pose = opennova::world::render_matrix_from_pose(
			zero, static_cast<int32_t>(view.yaw_bam), 0, 0);
	// The pose is row-major for row vectors (v' = v M), so its column-vector basis is M's
	// transpose; the render-to-Godot axis map swaps x and z on both sides.
	const int swap[3] = {2, 1, 0};
	Basis basis;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			basis.rows[i][j] = pose.m[4 * swap[j] + swap[i]];
	return basis;
}

float MenuDriver::vehicle_preview_fov() {
	return opennova::inmatch::VehiclePreviewView().fov_deg;
}

Color MenuDriver::vehicle_preview_clear_color() {
	const uint32_t argb = opennova::inmatch::kVehiclePreviewClear;
	return Color(static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(argb & 0xFFu) / 255.0f);
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
	ClassDB::bind_static_method("MenuDriver", D_METHOD("vehicle_preview_eye", "bounds"),
			&MenuDriver::vehicle_preview_eye);
	ClassDB::bind_static_method("MenuDriver", D_METHOD("vehicle_preview_model_basis", "tick_ms"),
			&MenuDriver::vehicle_preview_model_basis);
	ClassDB::bind_static_method("MenuDriver", D_METHOD("vehicle_preview_fov"),
			&MenuDriver::vehicle_preview_fov);
	ClassDB::bind_static_method("MenuDriver", D_METHOD("vehicle_preview_clear_color"),
			&MenuDriver::vehicle_preview_clear_color);
	ClassDB::bind_method(D_METHOD("preview_player_info_voice", "db", "voice"), &MenuDriver::preview_player_info_voice);
	ClassDB::bind_method(D_METHOD("get_selected_mission"), &MenuDriver::get_selected_mission);
	ClassDB::bind_method(D_METHOD("has_pending_expansion_reload"), &MenuDriver::has_pending_expansion_reload);
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
	ClassDB::bind_method(D_METHOD("is_options_surface"), &MenuDriver::is_options_surface);
	ClassDB::bind_method(D_METHOD("wire_commands", "in_mission"), &MenuDriver::wire_commands);
	ClassDB::bind_method(D_METHOD("screen_command", "screen", "name"), &MenuDriver::screen_command);
	ClassDB::bind_method(D_METHOD("command_owner"), &MenuDriver::command_owner);
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
	ClassDB::bind_method(D_METHOD("consume_options_input", "event"), &MenuDriver::consume_options_input);
	ClassDB::bind_method(D_METHOD("options_control_text", "action", "device"),
			&MenuDriver::options_control_text);
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
	ClassDB::bind_method(D_METHOD("get_screen_history_depth"),
			&MenuDriver::get_screen_history_depth);
	ClassDB::bind_method(D_METHOD("get_screen_history"), &MenuDriver::get_screen_history);

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
	ClassDB::bind_method(D_METHOD("set_deploy_list_items", "id", "rows"),
			&MenuDriver::set_deploy_list_items);
	ClassDB::bind_method(D_METHOD("table_cell_value", "id", "row", "col"),
			&MenuDriver::table_cell_value);
	ClassDB::bind_method(D_METHOD("table_selected_rows", "id"), &MenuDriver::table_selected_rows);
	ClassDB::bind_method(D_METHOD("set_widget_clip_rect", "id", "rect"),
			&MenuDriver::set_widget_clip_rect);

	ClassDB::bind_method(D_METHOD("activate", "id"), &MenuDriver::activate);
	ClassDB::bind_method(D_METHOD("spin_cycle", "id", "delta"), &MenuDriver::spin_cycle);
	ClassDB::bind_method(D_METHOD("is_item_enabled", "id", "row"), &MenuDriver::is_item_enabled);
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

	// The engine menu shell.
	BIND_ENUM_CONSTANT(SHELL_START_MISSION);
	BIND_ENUM_CONSTANT(SHELL_EXIT);
	BIND_ENUM_CONSTANT(SHELL_RETURN_TO_MENU);
	BIND_ENUM_CONSTANT(SHELL_RESTART);
	BIND_ENUM_CONSTANT(SHELL_RESUME);
	BIND_ENUM_CONSTANT(SHELL_NOVAWORLD);
	ClassDB::bind_method(D_METHOD("set_shell_sources", "root", "style"), &MenuDriver::set_shell_sources);
	ClassDB::bind_method(D_METHOD("set_menu_files", "main_menu", "ingame_menu"), &MenuDriver::set_menu_files);
	ClassDB::bind_method(D_METHOD("set_player_profiles", "profiles"), &MenuDriver::set_player_profiles);
	ClassDB::bind_method(D_METHOD("set_player_words_source", "get", "set"),
			&MenuDriver::set_player_words_source);
	ClassDB::bind_method(D_METHOD("set_update_launcher", "launch"), &MenuDriver::set_update_launcher);
	ClassDB::bind_method(D_METHOD("set_update_waiting", "waiting"), &MenuDriver::set_update_waiting);
	ClassDB::bind_method(D_METHOD("enter_menu_mode", "root_changed"), &MenuDriver::enter_menu_mode);
	ClassDB::bind_method(D_METHOD("leave_menu_mode"), &MenuDriver::leave_menu_mode);
	ClassDB::bind_method(D_METHOD("open_ingame_menu"), &MenuDriver::open_ingame_menu);
	ClassDB::bind_method(D_METHOD("open_menu", "file", "screen"), &MenuDriver::open_menu, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("is_in_game"), &MenuDriver::is_in_game);
	ClassDB::bind_method(D_METHOD("get_current_menu_file"), &MenuDriver::get_current_menu_file);
	ClassDB::bind_method(D_METHOD("frame_tail"), &MenuDriver::frame_tail);
	ClassDB::bind_method(D_METHOD("add_screen", "screen"), &MenuDriver::add_screen);
	ClassDB::bind_method(D_METHOD("claim_screen", "screen"), &MenuDriver::claim_screen);
	ClassDB::bind_method(D_METHOD("release_screen"), &MenuDriver::release_screen);

	// The frame's signals land here (the pump's click edge and its CScrollWnd
	// interaction result).
	ClassDB::bind_method(D_METHOD("_on_frame_widget_clicked", "index", "part"),
			&MenuDriver::on_frame_widget_clicked_);
	ClassDB::bind_method(D_METHOD("_on_frame_scroll_value", "index", "value"),
			&MenuDriver::on_frame_scroll_value_);

	ADD_SIGNAL(MethodInfo("screen_changed", PropertyInfo(Variant::STRING, "screen_name")));
	// What leaves the menu (SHELL_*): `text` the mission's file for SHELL_START_MISSION, what ends
	// the process for SHELL_EXIT.
	ADD_SIGNAL(MethodInfo("shell_exit_requested", PropertyInfo(Variant::INT, "exit"),
			PropertyInfo(Variant::STRING, "text")));
	// The Mods pick's reload: the shell's resources (the text tables, the stylesheets, the menu music,
	// the backdrop movies) read again from the new mount, then the menu booted anew over it.
	ADD_SIGNAL(MethodInfo("shell_resources_requested"));
	ADD_SIGNAL(MethodInfo("game_reloaded"));
	// A Mods pick that switches the game saves the player profile first.
	ADD_SIGNAL(MethodInfo("profile_save_requested"));
	// The in-game options' Accept wrote the controls words and the bindings into the current record;
	// on a dialog holding XHAIR_APPEARANCE it reloads the crosshair art in alpha mode.
	ADD_SIGNAL(MethodInfo("controls_accepted"));
	ADD_SIGNAL(MethodInfo("crosshair_reload_requested"));
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
	// A press on a table's data row, after the table's own selection write
	// (the 0x8000001 cell event): the row's new state and the column's cell
	// value; `double_click` for the double-click form.
	ADD_SIGNAL(MethodInfo("table_cell_clicked", PropertyInfo(Variant::INT, "id"),
			PropertyInfo(Variant::STRING, "widget_name"), PropertyInfo(Variant::INT, "row"),
			PropertyInfo(Variant::INT, "column"), PropertyInfo(Variant::INT, "state"),
			PropertyInfo(Variant::INT, "cell_value"), PropertyInfo(Variant::BOOL, "double_click")));
}

} // namespace godot
