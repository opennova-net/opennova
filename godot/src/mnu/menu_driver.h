#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/menu/menu_commands.h>
#include <runtime/menu/mission_screen.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/mod_list.h>
#include <runtime/menu/options_screen.h>
#include <runtime/menu/player_info_avatars.h>
#include <runtime/menu/menu_screen_controller.h>
#include <runtime/menu/menu_shell.h>

#include <memory>
#include <vector>

#include "mnu/mnu_rows.h"

namespace godot {

class MenuAudio;
class ControlsModel;
class PlayerProfiles;
class MissionCatalogRow;
class MenuFrame;
class MnsStyleSheet;
class MnuDocument;
class MusicDirector;
class ResourceRoot;
class RtxtStringFile;
class AvatarDatabase;
class AvatarComboRow;
class DeployListRow;
class EndRoundColumn;
class EndRoundRow;

// The standalone CScrollWnd range of one widget, as the driver's store holds
// it (a value snapshot; MenuDriver.set_widget_scroll_range is the one write
// path, where the original range normalization happens).
class MenuScrollRange : public RefCounted {
	GDCLASS(MenuScrollRange, RefCounted)

	opennova::menu::MenuScrollRangeState value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::menu::MenuScrollRangeState &p_value) { value_ = p_value; }
	int get_minimum() const { return value_.minimum; }
	int get_maximum() const { return value_.maximum; }
	int get_page() const { return value_.page; }
	int get_value() const { return value_.value; }
};

// A screen controller's binding: the engine controller (menu::MenuScreenController) a script
// hands a driver (MenuDriver.add_screen / claim_screen), with the host the controller reads its
// models through. A subclass holds both and relays the device work its controller asks for (a
// preview, an icon) as its own signals.
class MenuScreenBinding : public RefCounted {
	GDCLASS(MenuScreenBinding, RefCounted)

protected:
	static void _bind_methods() {}

public:
	// The engine controller (C++ only); null for none.
	virtual opennova::menu::MenuScreenController *screen_controller() { return nullptr; }
};

// The compiled-menu interaction driver: ONE MenuFrame (the engine draw-list /
// pump surface) over a parsed MnuDocument. Every decision — navigation and
// the back stack, the per-widget state store and its replay, ACTION dispatch,
// radio groups, the spin cycle, the single open dropdown, edit focus and key
// routing — is the engine's opennova::menu::MenuRuntime
// (engine/runtime/menu/menu_runtime.h carries the witnesses); this class is
// its device half: it implements the frame seam over the MenuFrame node,
// reads the marquee data through the resource root, plays the widget sound
// edges, pushes the screen MUSICVAR and relays the runtime's events as
// signals. Widgets go by stable MnuDocument id, valid across screens.
class MenuDriver : public RefCounted {
	GDCLASS(MenuDriver, RefCounted)

	class FrameSeam;
	friend class FrameSeam;

	opennova::menu::MenuRuntime runtime_;
	opennova::menu::MenuFlow flow_;
	opennova::menu::ModList mods_;
	String mods_root_; // the game folder the Mods list's records were scanned in
	opennova::menu::HostDialog host_dialog_;
	opennova::menu::OptionsScreen options_;
	opennova::menu::PlayerInfoAvatars avatars_;
	// The commands the game binds on the open document by control name (wire_commands).
	opennova::menu::MenuCommands commands_;
	std::unique_ptr<FrameSeam> seam_;
	ObjectID frame_id_;
	ObjectID audio_id_;
	ObjectID music_director_id_;
	int music_var_index_ = 0;
	Ref<MnuDocument> doc_;
	Ref<ResourceRoot> root_;
	Ref<MnsStyleSheet> style_;
	// The pointer the driver last made the OS cursor (the claim's CURSOR, or the
	// transparent no-pointer image), by id: the frame's texture stays the frame's
	// to release; and that image (D-MNU-26).
	ObjectID applied_pointer_id_;
	Ref<Image> no_pointer_image_;

	MenuFrame *frame_() const;
	Ref<Resource> no_pointer_();
	void show_pointer_(const Ref<Resource> &p_pointer);
	MenuAudio *audio_() const;
	MusicDirector *music_director_() const;
	void on_runtime_event_(const opennova::menu::MenuEvent &p_event);
	bool code_page_() const;
	void seed_marquee_widgets_();
	void on_frame_widget_clicked_(int p_index, int p_part);
	void on_frame_scroll_value_(int p_index, int p_value);

protected:
	static void _bind_methods();

public:
	enum OptionsEffect {
		OPTIONS_CONSUMED = opennova::menu::OptionsScreen::Consumed,
		OPTIONS_COMMIT_PREVIEW = opennova::menu::OptionsScreen::CommitPreview,
		OPTIONS_RESTORE_PREVIEW = opennova::menu::OptionsScreen::RestorePreview,
		OPTIONS_APPLY_CONTROLS = opennova::menu::OptionsScreen::ApplyControls,
		OPTIONS_DISCARD_EDITS = opennova::menu::OptionsScreen::DiscardEdits,
		OPTIONS_RELOAD_CROSSHAIR_ALPHA = opennova::menu::OptionsScreen::ReloadCrosshairAlpha,
	};
	// The player options' word indices (engine options_policy.h OptionsWord).
	enum OptionsWordIndex {
		OPTIONS_WORD_SOUND_FX_VOLUME = static_cast<int>(opennova::menu::OptionsWord::SoundFxVolume),
		OPTIONS_WORD_DIALOG_VOLUME = static_cast<int>(opennova::menu::OptionsWord::DialogVolume),
		OPTIONS_WORD_MUSIC_VOLUME = static_cast<int>(opennova::menu::OptionsWord::MusicVolume),
		OPTIONS_WORD_CROSSHAIR_STYLE =
				static_cast<int>(opennova::menu::OptionsWord::CrosshairStyle),
		OPTIONS_WORD_CROSSHAIR_COLOR =
				static_cast<int>(opennova::menu::OptionsWord::CrosshairColor),
		OPTIONS_WORD_CROSSHAIR_SPREAD =
				static_cast<int>(opennova::menu::OptionsWord::CrosshairSpread),
		OPTIONS_WORD_KEYBOARD_TIPS = static_cast<int>(opennova::menu::OptionsWord::KeyboardTips),
		OPTIONS_WORD_GAMEPLAY_TIPS = static_cast<int>(opennova::menu::OptionsWord::GameplayTips),
		OPTIONS_WORD_ASPECT_MODE = static_cast<int>(opennova::menu::OptionsWord::AspectMode),
		OPTIONS_WORD_OBJECT_POLYDETAIL =
				static_cast<int>(opennova::menu::OptionsWord::ObjectPolyDetail),
		OPTIONS_WORD_TEXFILTER_LEVEL =
				static_cast<int>(opennova::menu::OptionsWord::TexFilterLevel),
		OPTIONS_WORD_PARTICLE_DENSITY =
				static_cast<int>(opennova::menu::OptionsWord::ParticleDensity),
		OPTIONS_WORD_TEXCOMPRESSION_LEVEL =
				static_cast<int>(opennova::menu::OptionsWord::TexCompressionLevel),
		OPTIONS_WORD_TERRAIN_TEXDETAIL =
				static_cast<int>(opennova::menu::OptionsWord::TerrainTexDetail),
		OPTIONS_WORD_OBJECT_TEXDETAIL =
				static_cast<int>(opennova::menu::OptionsWord::ObjectTexDetail),
		OPTIONS_WORD_COUNT = static_cast<int>(opennova::menu::OptionsWord::kCount),
	};
	// The single-player screen's lists, briefing panes and confirms are the engine's name sets
	// (menu_commands.h), set at construction as the editor's Try mode sets them.
	void clear_mission_rows() { flow_.clear_rows(); }
	void seed_mission_list(int p_id, const TypedArray<MissionCatalogRow> &p_rows);
	void select_mission(int p_id, int p_row, const String &p_fallback);
	void activate_mission(int p_id, int p_row) { flow_.activate_mission(p_id, p_row); }
	String get_selected_mission() const;
	void clear_selected_mission() { flow_.clear_selected_mission(); }
	// The Mods list (engine menu::ModList): the descriptions a pick fills are the engine's
	// ModDescriptions name set (set at construction); the list filled with the base game's row and the expansion records of
	// the bound root's folder (scanned once per folder, as the game scans once at boot), the one
	// running highlighted; a row picked; and ACCEPT's pick ("" the base game).
	void seed_mod_list(int p_id);
	void select_mod(int p_id, int p_row) { mods_.select(runtime_, p_id, p_row); }
	String mod_list_pick(int p_id) const;
	bool request_expansion(const String &p_name, const String &p_current, bool p_packed);
	bool has_pending_expansion_reload() const { return flow_.has_pending_expansion(); }
	String take_expansion_reload();
	void seed_host_pool(const TypedArray<MissionCatalogRow> &p_rows);
	void filter_host_missions() { host_dialog_.filter(runtime_); }
	void add_host_missions(const Ref<RtxtStringFile> &p_text);
	void remove_host_missions() { host_dialog_.remove_selected(runtime_); }
	bool can_start_host() const { return host_dialog_.can_start(); }
	PackedStringArray selected_host_missions() const;
	// The SELECTED_MISSIONS Switch cell's click and the rows' launch options
	// (engine menu::HostDialog::toggle_switch / selected_launch_options).
	void toggle_host_mission_switch(int p_row) { host_dialog_.toggle_switch(runtime_, p_row); }
	PackedInt32Array selected_host_launch_options() const;
	// The Options screens over the profile's current record (engine
	// menu::OptionsScreen): the screen's binding records built from the
	// record's table; null profiles = the catalog defaults, no words.
	void prepare_options(const Ref<PlayerProfiles> &p_profiles);
	bool is_options_surface() const { return options_.is_surface(); }
	// The retail Options policy on the surface prepare_options found (engine
	// OptionsScreen::apply_policy), then the record's controls words
	// (OptionsScreen::seed_profile), before the settings owner seeds its values.
	void apply_options_policy(const Ref<PlayerProfiles> &p_profiles);
	// The engine's name set by its token (menu_commands.h: "start", "exit", "return",
	// "restart", "back", "novaworld", "mission_lists", "sp_lists", "briefings", "sp_accepts",
	// "mod_lists", "mod_descriptions"), the one table the editor's Try mode reads too. Empty
	// for another token.
	static PackedStringArray command_names(const String &p_set);
	// The commands the game binds on the open document by control name, in a mission or the
	// front end (engine MenuCommands::wire, run after each open, as the editor's Try mode runs
	// it); the readers below answer from it until the next open (which binds nothing until it
	// is wired) or wire.
	void wire_commands(bool p_in_mission) { commands_.wire(runtime_, p_in_mission); }
	// The command bound to the control `p_name` of the screen `p_screen` (MenuCommands::command_of),
	// as its token ("start_mission", "apply_loadout", ...; menu_commands.h menu_command_token);
	// "" for none.
	String screen_command(const String &p_screen, const String &p_name) const;
	// The delegate that owns the document ("lan", "player_info"; "" the shell's own wiring) and
	// the document's lists the game fills with its missions and with its expansions (widget ids).
	String command_owner() const { return String(commands_.owner().c_str()); }
	PackedInt32Array command_mission_lists() const;
	PackedInt32Array command_mod_lists() const;
	// The menu screen a running mission has up and the main frame's input law under it
	// (engine menu/mission_screen.h carries the witness): whether one is up, whether its
	// input passes through (DEATH or STAT under the menu's pass-through word: the input
	// frame runs), and whether it holds the input frame (controls::PlayerActionPoll::menu_open).
	enum MissionScreen {
		MISSION_SCREEN_NONE = static_cast<int>(opennova::menu::MissionScreen::None),
		MISSION_SCREEN_INGAME = static_cast<int>(opennova::menu::MissionScreen::Ingame),
		MISSION_SCREEN_WEAPON = static_cast<int>(opennova::menu::MissionScreen::Weapon),
		MISSION_SCREEN_COMMAND_MAP = static_cast<int>(opennova::menu::MissionScreen::CommandMap),
		MISSION_SCREEN_VEHICLE = static_cast<int>(opennova::menu::MissionScreen::Vehicle),
		MISSION_SCREEN_STAT = static_cast<int>(opennova::menu::MissionScreen::Stat),
		MISSION_SCREEN_DEATH = static_cast<int>(opennova::menu::MissionScreen::Death),
	};
	static bool mission_screen_open(MissionScreen p_screen) {
		return opennova::menu::mission_screen_open(static_cast<opennova::menu::MissionScreen>(p_screen));
	}
	static bool mission_screen_passes_input(MissionScreen p_screen, bool p_pass_through_word) {
		return opennova::menu::mission_screen_passes_input(
				static_cast<opennova::menu::MissionScreen>(p_screen), p_pass_through_word);
	}
	static bool mission_screen_holds_input(MissionScreen p_screen, bool p_pass_through_word) {
		return opennova::menu::mission_screen_holds_input(
				static_cast<opennova::menu::MissionScreen>(p_screen), p_pass_through_word);
	}
	// The front-end OPTIONS screen, whose every show re-runs the options
	// preparation (engine OptionsScreen::is_front_screen).
	static bool is_front_options_screen(const String &p_screen);
	int activate_options(const Ref<PlayerProfiles> &p_profiles, int p_id);
	void arm_options_remap(int p_id, int p_row) { options_.arm(runtime_, p_id, p_row); }
	// A single press on the controls table's row (engine OptionsScreen::press).
	void press_options_row(int p_id, int p_row) { options_.press(runtime_, p_id, p_row); }
	// The player options' words (OPTIONS_WORD_* order) into their controls, and one edited
	// control's word from its edit's `p_value` (-1 for a click): the words with it written,
	// empty when `p_id` shows none (engine OptionsScreen::seed_words / edit_word,
	// options_policy.h kOptionsWordControls).
	void seed_options_words(const PackedInt32Array &p_words);
	PackedInt32Array options_word_edit(int p_id, int p_value,
			const PackedInt32Array &p_words) const;
	int consume_options_input(const Ref<InputEvent> &p_event);
	void end_options_remap(bool p_refill) { options_.end_remap(runtime_, p_refill); }
	// The Options screen's own binding records' Control-column text (tests and
	// tools read the remap flow through it).
	String options_control_text(int p_action, int p_device) const;
	void show_ingame_main() { opennova::menu::OptionsScreen::show_ingame_main(runtime_); }

	enum AvatarChange {
		AVATAR_TEAM = int(opennova::menu::PlayerInfoAvatars::Change::Team),
		AVATAR_NATIONALITY = int(opennova::menu::PlayerInfoAvatars::Change::Nationality),
		AVATAR_DIVISION = int(opennova::menu::PlayerInfoAvatars::Change::Division),
		AVATAR_COMBO = int(opennova::menu::PlayerInfoAvatars::Change::Combo),
		AVATAR_VOICE = int(opennova::menu::PlayerInfoAvatars::Change::Voice),
	};
	int update_player_info_avatars(const Ref<AvatarDatabase> &p_db, int p_change,
			int p_value, int p_team, int p_voice, const Ref<RtxtStringFile> &p_gameui,
			const Ref<RtxtStringFile> &p_menutxt);
	PackedInt32Array player_info_nationality_rows() const;
	int player_info_avatar_nationality() const { return avatars_.nationality; }
	int player_info_avatar_division() const { return avatars_.division; }
	bool player_info_avatar_preview_changed() const { return avatars_.preview_changed; }
	Ref<AvatarComboRow> player_info_avatar_combo(const Ref<AvatarDatabase> &p_db) const;
	// The VEHICLE screen's ITEM_DISPLAY preview (engine inmatch::vehicle_preview_view): the eye
	// for a model of `p_bounds` (its centre at the origin), the model's turn at `p_tick_ms`
	// (the engine's render-space pose matrix, its x / z swapped into Godot's), the field across
	// the rect's width in degrees, and the clear colour.
	static Vector3 vehicle_preview_eye(const AABB &p_bounds);
	static Basis vehicle_preview_model_basis(int64_t p_tick_ms);
	static float vehicle_preview_fov();
	static Color vehicle_preview_clear_color();
	void preview_player_info_voice(const Ref<AvatarDatabase> &p_db, int p_voice);

	MenuDriver();
	~MenuDriver() override;

	void attach(MenuFrame *p_frame, MenuAudio *p_audio);
	void set_music_director(MusicDirector *p_director);
	void set_music_var_index(int p_index) { music_var_index_ = p_index; }
	MenuFrame *get_frame() const { return frame_(); }
	String get_menu_file() const;
	String get_current_screen() const;
	// The OS cursor the menu last applied: the claim's CURSOR texture, or with
	// none the transparent no-pointer image (D-MNU-26); null before any.
	Ref<Resource> get_applied_pointer() const;
	// The screen holding the widget `p_id` (engine MenuRuntime::widget_screen_of).
	String widget_screen_of(int p_id) const;

	// Bind a parsed document and show `target_screen` (empty = the first
	// screen). Rebuilds every per-document cache; runtime widget state is dropped.
	// Every string table a screen reads is its windows' TEXT_RSRC (the frame
	// loads them); the process's text-override table is tried first (the engine's
	// rtxt::text_override; the lookup order is the engine's, menu_text_tables.h).
	bool open_document(const Ref<MnuDocument> &p_doc, const Ref<ResourceRoot> &p_root,
			const Ref<MnsStyleSheet> &p_style, const String &p_menu_file,
			const String &p_target_screen);
	Ref<MnuDocument> document() const { return doc_; }
	PackedStringArray get_screen_names() const;
	bool show_screen(const String &p_name);
	bool navigate_to_screen(const String &p_name);
	bool pop_screen();

	// --- the screen history across menu files and a mission (engine
	// menu/screen_history.h, kept by the runtime across open_document) ---
	// A row is [file, screen]; an empty array is no row.
	void push_screen_history(const String &p_file, const String &p_screen);
	PackedStringArray pop_screen_history();
	void clear_screen_history();
	void trim_screen_history();
	int get_screen_history_depth() const;
	// The rows bottom first, each {file, screen, mark}.
	Array get_screen_history() const;
	// The menu mode's leave (a mission's start) and its re-entry, which answers
	// the row to show again.
	void leave_menu_mode();
	PackedStringArray return_to_menu_mode();

	// --- widget addressing / state (doc-id keyed) ---
	int widget_id(const String &p_name) const;
	String widget_name_of(int p_id) const;
	int widget_kind_of(int p_id) const;
	bool has_widget(const String &p_name) const;
	int frame_index(int p_id) const;
	// The widget's rect in the frame Control's local coordinates (the design
	// rect scaled by the frame's current size); zero when off-screen.
	Rect2 widget_frame_rect(int p_id) const;
	Vector2 design_scale() const;
	// The widget's own design rect relative to its parent's origin
	// (CWnd_GetRect), and its replacement (CWnd_SetRect).
	Rect2 widget_local_rect(int p_id) const;
	void set_widget_rect(int p_id, const Rect2i &p_rect);

	void set_widget_shown(int p_id, bool p_shown);
	bool is_widget_shown(int p_id) const;
	void set_widget_disabled(int p_id, bool p_disabled);
	bool is_widget_disabled(int p_id) const;
	void set_widget_checked(int p_id, bool p_checked);
	bool is_widget_checked(int p_id) const;
	// The text set; returns the label it stored (engine
	// MenuRuntime::set_widget_text), the script's own text, not the frame's.
	String set_widget_text(int p_id, const String &p_text);
	String get_widget_text(int p_id) const;
	void set_widget_items(int p_id, const PackedStringArray &p_items);
	PackedStringArray get_widget_items(int p_id) const;
	int item_count(int p_id) const;
	String item_text(int p_id, int p_row) const;
	String item_value(int p_id, int p_row) const;
	TypedArray<MnuActionRow> widget_actions(int p_id) const;
	void select_row_by_value(int p_id, const String &p_value, bool p_emit);
	void select_row(int p_id, int p_row, bool p_emit);
	int selected_row(int p_id) const;
	void set_widget_scroll_range(int p_id, int p_minimum, int p_maximum, int p_page,
			int p_value);
	// Current standalone scroll state, or null until seeded.
	Ref<MenuScrollRange> get_widget_scroll_range(int p_id) const;

	int table_row_count(int p_id) const;
	String table_cell_text(int p_id, int p_row, int p_col) const;
	void table_select_row(int p_id, int p_row, bool p_additive);
	int table_sort_column(int p_id) const;
	// The end-of-round stat table (StatScreen_PopulateStatResultsList, witnessed in
	// inmatch stat_screen_feed.h): the engine's column set installed (the RESULTLIST authors no
	// HEADER), the rows with their team colours, the sort on the first stat column, descending, and
	// after it the first row whose slot is `p_local_slot` (the local player entity's slot id,
	// EndRoundState.get_local_slot) selected (inmatch stat_screen_feed.h).
	void fill_stat_results(int p_id, const TypedArray<EndRoundColumn> &p_columns,
			const TypedArray<EndRoundRow> &p_rows, int p_local_slot);
	// A stat tab's filter over the filled table: the rows shown or hidden by their team
	// (inmatch stat_screen_feed.h stat_screen_filter_rows carries the witness).
	void filter_stat_results(int p_id, int p_tab);
	// The death screen's SPAWNPOINTS_LIST items from its compiled rows (engine
	// world deploy_screen_feed.h deploy_list_items), in the menu's code page.
	void set_deploy_list_items(int p_id, const TypedArray<DeployListRow> &p_rows);
	// The CTableWnd reads (engine menu_table_row.h).
	int table_cell_value(int p_id, int p_row, int p_col) const;
	PackedInt32Array table_selected_rows(int p_id) const;
	// CWnd_SetClipRect (absolute design units).
	void set_widget_clip_rect(int p_id, const Rect2i &p_rect);
	// The engine runtime, for native screen companions (C++ only).
	opennova::menu::MenuRuntime &runtime() { return runtime_; }

	// --- activation / actions ---
	void activate(int p_id);
	void spin_cycle(int p_id, int p_delta);
	String spin_value_attr(int p_id) const;
	// A spin list's row's enabled state (MenuRuntime::is_item_enabled).
	bool is_item_enabled(int p_id, int p_row) const { return runtime_.is_item_enabled(p_id, p_row); }
	// One ACTION row run as the current screen's (MenuRuntime::dispatch_action).
	bool dispatch_action_row(const Ref<MnuActionRow> &p_action);
	// Direct play seam (voice preview etc.); emits sound_requested always.
	void play_widget_sound(const String &p_trigger, const String &p_file);

	// --- input (the shell's _gui_input owners forward here) ---
	// The mouse as the game takes it (engine MenuRuntime): each event as its message arrives
	// (move_mouse, press_mouse, release_mouse; take_mouse_event samples one), the pump once a
	// frame after them (pump_mouse), which reads the device's mouse before this menu's first
	// message (MenuRuntime::adopt_mouse_state, D-MNU-79). process_mouse is one whole sample (a
	// scripted pump).
	void process_mouse(const Vector2 &p_position, bool p_button_down);
	void move_mouse(const Vector2 &p_position, bool p_button_down);
	// `p_double_click`: the platform's double click (InputEventMouseButton::double_click).
	bool press_mouse(const Vector2 &p_position, bool p_double_click = false);
	bool release_mouse(const Vector2 &p_position);
	void pump_mouse();
	// A mouse event as its message: a motion's point and left button, the left button's press or
	// release. True for a left-button event (the caller accepts it).
	bool take_mouse_event(const Ref<InputEvent> &p_event);
	bool process_wheel(const Vector2 &p_position, int p_steps);
	bool handle_key_input(const Ref<InputEventKey> &p_event);
	void close_active_combo_popup();
	bool is_combo_popup_open(int p_id) const;
	int get_focused_widget() const;
	// Give an edit the keyboard focus (UI_SetFocusWnd; read-only refuses).
	void focus_widget(int p_id);

	// Advance the blink/marquee clock; the shell's _process forwards its clock.
	void tick(int64_t p_time_ms);

	// --- screen controllers (engine menu_screen_controller.h, through menu::MenuShell) ---
	// A delegate the shell hands every document it opens whose owner (menu::MenuCommands::owner)
	// is the controller's owner(): its open() runs after the open's wiring, the runtime's events
	// reach it while it holds the document, its close() runs when another document opens.
	void add_screen(const Ref<MenuScreenBinding> &p_screen);
	// The document open now (a screen a mission opens by name, opened with open_document) is
	// `p_screen`'s: its open() runs now with the open file and screen, and it holds the document
	// until release_screen() or the next open. False with no document open or no controller.
	bool claim_screen(const Ref<MenuScreenBinding> &p_screen);
	void release_screen();
	// The engine shell (C++ only).
	opennova::menu::MenuShell &shell() { return shell_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MenuDriver::OptionsEffect);
VARIANT_ENUM_CAST(godot::MenuDriver::OptionsWordIndex);
VARIANT_ENUM_CAST(godot::MenuDriver::AvatarChange);
VARIANT_ENUM_CAST(godot::MenuDriver::MissionScreen);
