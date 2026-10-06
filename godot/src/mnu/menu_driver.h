#pragma once

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
#include <godot_cpp/variant/vector2.hpp>

#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/options_screen.h>
#include <runtime/menu/player_info_avatars.h>

#include <memory>
#include <vector>

#include "mnu/mnu_rows.h"

namespace godot {

class MenuAudio;
class ControlsModel;
class MissionCatalogRow;
class MenuFrame;
class MnsStyleSheet;
class MnuDocument;
class MusicDirector;
class ResourceRoot;
class RtxtStringFile;
class AvatarDatabase;
class AvatarComboRow;
class WeaponDatabase;
class WeaponDef;

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

// The compiled-menu interaction driver: ONE MenuFrame (the engine draw-list /
// pump surface) over a parsed MnuDocument. Every decision — navigation and
// the back stack, the per-widget state store and its replay, ACTION dispatch,
// radio groups, the spin cycle, the single open dropdown, edit focus and key
// routing — is the engine's opennova::menu::MenuRuntime
// (engine/runtime/menu/menu_runtime.h carries the witnesses); this class is
// its device half: it implements the frame seam over the MenuFrame node,
// resolves the screen's text table and marquee data through the resource
// root, mounts the credits scrollers, plays the widget sound edges, pushes
// the screen MUSICVAR and relays the runtime's events as signals. Widgets go
// by stable MnuDocument id, valid across screens.
class MenuDriver : public RefCounted {
	GDCLASS(MenuDriver, RefCounted)

	class FrameSeam;
	friend class FrameSeam;

	opennova::menu::MenuRuntime runtime_;
	opennova::menu::MenuFlow flow_;
	opennova::menu::HostDialog host_dialog_;
	opennova::menu::OptionsScreen options_;
	opennova::menu::PlayerInfoAvatars avatars_;
	std::unique_ptr<FrameSeam> seam_;
	ObjectID frame_id_;
	ObjectID audio_id_;
	ObjectID music_director_id_;
	int music_var_index_ = 0;
	Ref<MnuDocument> doc_;
	Ref<ResourceRoot> root_;
	Ref<MnsStyleSheet> style_;
	Ref<RtxtStringFile> text_;
	// Per-screen RTXT text tables (TEXT_RSRC), cached by lowercased filename.
	HashMap<String, Ref<RtxtStringFile>> text_rsrc_cache_;
	// CBIN credits scrollers mounted over marquee widgets of the current
	// screen. The overlays are frame CHILDREN, outside the compiled draw walk,
	// so the driver re-applies the walk's shown gate whenever widget
	// visibility changes: a hidden tab hides its credits.
	struct CreditsMount {
		ObjectID player;
		int id = -1;
	};
	std::vector<CreditsMount> credits_;

	MenuFrame *frame_() const;
	MenuAudio *audio_() const;
	MusicDirector *music_director_() const;
	void on_runtime_event_(const opennova::menu::MenuEvent &p_event);
	void fill_screen_text_lookup_(Dictionary &r_out);
	Ref<RtxtStringFile> load_text_rsrc_(const String &p_file);
	void seed_marquee_widgets_();
	void clear_credits_();
	void sync_credits_();
	void on_frame_widget_clicked_(int p_index);
	void on_frame_scroll_value_(int p_index, int p_value);

protected:
	static void _bind_methods();

public:
	enum OptionsEffect {
		OPTIONS_CONSUMED = opennova::menu::OptionsScreen::Consumed,
		OPTIONS_PERSIST_BINDINGS = opennova::menu::OptionsScreen::PersistBindings,
		OPTIONS_COMMIT_PREVIEW = opennova::menu::OptionsScreen::CommitPreview,
		OPTIONS_RESTORE_PREVIEW = opennova::menu::OptionsScreen::RestorePreview,
	};
	void set_mission_controls(const PackedStringArray &p_lists,
			const PackedStringArray &p_briefings, const PackedStringArray &p_accepts);
	void clear_mission_rows() { flow_.clear_rows(); }
	void seed_mission_list(int p_id, const TypedArray<MissionCatalogRow> &p_rows);
	void select_mission(int p_id, int p_row, const String &p_fallback);
	void activate_mission(int p_id, int p_row) { flow_.activate_mission(p_id, p_row); }
	String get_selected_mission() const;
	void clear_selected_mission() { flow_.clear_selected_mission(); }
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
	void select_host_location(int p_id, const String &p_country);
	void prepare_options(const Ref<ControlsModel> &p_controls);
	bool is_options_surface() const { return options_.is_surface(); }
	int activate_options(const Ref<ControlsModel> &p_controls, const String &p_name);
	void arm_options_remap(const Ref<ControlsModel> &p_controls, int p_id, int p_row);
	int consume_options_input(const Ref<ControlsModel> &p_controls, const Ref<InputEvent> &p_event);
	void end_options_remap(const Ref<ControlsModel> &p_controls, bool p_refill);
	void show_ingame_main() { opennova::menu::OptionsScreen::show_ingame_main(runtime_); }

	int fill_player_info_ammo(const Ref<WeaponDatabase> &p_weapons, const String &p_control,
			int p_parent, int p_primary, int p_secondary, int p_type, const Ref<RtxtStringFile> &p_text);
	void fill_armory_ammo(const Ref<WeaponDatabase> &p_weapons, const String &p_control,
			int p_parent, const String &p_current_name, int p_current_clips, const Ref<RtxtStringFile> &p_text);
	TypedArray<WeaponDef> fill_player_info_grenades(const Ref<WeaponDatabase> &p_weapons,
			int p_class_mask, int p_team_mask, const Dictionary &p_counts, const Ref<RtxtStringFile> &p_text);
	TypedArray<WeaponDef> fill_armory_grenades(const Ref<WeaponDatabase> &p_weapons,
			int p_class_mask, int p_team_mask, const Array &p_current, const Callable &p_availability,
			const Ref<RtxtStringFile> &p_text);
	double player_info_loadout_weight(const Ref<WeaponDatabase> &p_weapons,
			const TypedArray<WeaponDef> &p_parents, const TypedArray<WeaponDef> &p_grenades,
			const Dictionary &p_primary, const Dictionary &p_secondary) const;
	double armory_loadout_weight(const Ref<WeaponDatabase> &p_weapons,
			const TypedArray<WeaponDef> &p_parents, const TypedArray<WeaponDef> &p_grenades) const;

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
	void preview_player_info_voice(const Ref<AvatarDatabase> &p_db, int p_voice);

	MenuDriver();
	~MenuDriver() override;

	void attach(MenuFrame *p_frame, MenuAudio *p_audio);
	void set_music_director(MusicDirector *p_director);
	void set_music_var_index(int p_index) { music_var_index_ = p_index; }
	MenuFrame *get_frame() const { return frame_(); }
	String get_menu_file() const;
	String get_current_screen() const;

	// Bind a parsed document and show `target_screen` (empty = the first
	// screen). Rebuilds every per-document cache; runtime widget state is dropped.
	bool open_document(const Ref<MnuDocument> &p_doc, const Ref<ResourceRoot> &p_root,
			const Ref<MnsStyleSheet> &p_style, const Ref<RtxtStringFile> &p_text,
			const String &p_menu_file, const String &p_target_screen);
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
	void set_widget_text(int p_id, const String &p_text);
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
	// A row owner's first visible row (a list / table's, a multiline edit's
	// first line; the multiline edit's SetText resets it to 0, docs/interface/
	// hud-re.md "The windowed map views").
	void set_scroll_row(int p_id, int p_row);

	// The populate's column layout (CTableWnd's column-count resize, then one
	// InitRow per column; engine MenuRuntime::table_set_column_count /
	// table_init_column carry the witnesses).
	bool table_set_column_count(int p_id, int p_count);
	bool table_init_column(int p_id, int p_column, int p_width, const String &p_label,
			int p_justify, int p_vjustify);
	void table_add_row(int p_id, const PackedStringArray &p_cells);
	void table_clear_rows(int p_id);
	int table_row_count(int p_id) const;
	String table_cell_text(int p_id, int p_row, int p_col) const;
	void table_select_row(int p_id, int p_row, bool p_additive);
	// The CTableWnd operations (engine menu_table_row.h).
	int table_insert_row(int p_id, const String &p_text0, int p_value0, int p_flags,
			int p_insert_index);
	void table_set_cell_text(int p_id, int p_row, int p_col, const String &p_text);
	void table_set_cell_value(int p_id, int p_row, int p_col, int p_value);
	int table_cell_value(int p_id, int p_row, int p_col) const;
	void table_remove_row(int p_id, int p_row);
	int table_row_state(int p_id, int p_row) const;
	void table_set_row_selected(int p_id, int p_row, bool p_selected);
	PackedInt32Array table_selected_rows(int p_id) const;
	// CWnd_SetClipRect (absolute design units) and its removal.
	void set_widget_clip_rect(int p_id, const Rect2i &p_rect);
	void clear_widget_clip_rect(int p_id);
	// The engine runtime, for native screen companions (C++ only).
	opennova::menu::MenuRuntime &runtime() { return runtime_; }

	// --- activation / actions ---
	void activate(int p_id);
	void spin_cycle(int p_id, int p_delta);
	String spin_value_attr(int p_id) const;
	bool dispatch_action_row(const Ref<MnuActionRow> &p_action);
	// Direct play seam (voice preview etc.); emits sound_requested always.
	void play_widget_sound(const String &p_trigger, const String &p_file);

	// --- input (the shell's _gui_input owners forward here) ---
	void process_mouse(const Vector2 &p_position, bool p_button_down);
	bool process_wheel(const Vector2 &p_position, int p_steps);
	bool handle_key_input(const Ref<InputEventKey> &p_event);
	void close_active_combo_popup();
	bool is_combo_popup_open(int p_id) const;
	int get_focused_widget() const;
	// Give an edit the keyboard focus (UI_SetFocusWnd; read-only refuses).
	void focus_widget(int p_id);

	// Advance the blink/marquee clock; the shell's _process forwards its clock.
	void tick(int64_t p_time_ms);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MenuDriver::OptionsEffect);
VARIANT_ENUM_CAST(godot::MenuDriver::AvatarChange);
