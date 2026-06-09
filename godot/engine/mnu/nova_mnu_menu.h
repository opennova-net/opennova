#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "audio/nova_sbf_bank.h"
#include "lwf/nova_lwf_data.h"
#include "mns_stylesheet.h"
#include "nova_mnu_document.h"
#include "resource_index/nova_resource_root.h"
#include "rtxt/rtxt_string_file.h"

namespace godot {

class NovaMnuScreen;
class NovaMusicDirector;
class AudioStreamPlayer;
class InputEventKey;

// Runtime menu node: point it at a NovaMnuDocument and it builds the live widget
// tree (one NovaMnuScreen child per screen). Assets resolve through an optional
// NovaResourceRoot / MnsStyleSheet / RtxtStringFile (M4). It is also the menu's
// navigation controller (M5): widgets dispatch their actions here (screen nav +
// stack, window show/hide, pop, quit) and ask it to play sounds. Menu music is
// driven through an optional NovaMusicDirector and sounds through an optional
// NovaSbfBank (PR #29); both are also surfaced as signals so a host can react
// without the menu owning audio policy.
//
// edit_mode makes the tree inert + click-through and suppresses navigation,
// audio, and cursor side effects so the ONED editor can reuse this exact node as
// a WYSIWYG preview canvas.
class NovaMnuMenu : public Control {
	GDCLASS(NovaMnuMenu, Control)

private:
	Ref<NovaMnuDocument> menu_;
	Ref<NovaResourceRoot> resource_root_;
	Ref<MnsStyleSheet> stylesheet_;
	Ref<RtxtStringFile> text_resource_;
	// Menu SFX come from a .lwf sound profile: the widget's <SOUND> trigger
	// (MOUSE_OVER/CLICK_SELECT/...) names a set whose member points at a loose
	// .wav (resolved through resource_root_). sound_bank_ is the legacy pre-LWF
	// path (an SBF keyed by trigger/file-stem), kept only as a fallback.
	Ref<NovaLwfData> sound_profile_;
	Ref<NovaSbfBank> sound_bank_;
	NovaMusicDirector *music_director_ = nullptr;
	int music_var_index_ = 0;
	// Last music var actually applied; -1 == none yet. Dedups repeated values so
	// re-showing a screen does not retrigger its track (mirrors menu_manager.gd's
	// _current_music_var). Reset on each build().
	int current_music_var_ = -1;
	String current_screen_;
	bool edit_mode_ = false;
	// Layered on top of edit_mode_ for the ONED "play" preview: keeps the authoring
	// suppressions (music/cursor/all-screens) but re-wires navigators so clicking a
	// tab runs its window show/hide actions. External side effects (quit/url/cross-
	// .mnu jump) are clamped to no-ops so the sandbox cannot reach the editor host.
	bool interactive_ = false;
	bool build_on_ready_ = true;
	int unresolved_asset_count_ = 0;

	// Within-menu back navigation stack of screen names (cross-.mnu jumps are
	// host policy, emitted as menu_requested rather than pushed here).
	Vector<String> nav_stack_;

	// Lazily created pooled players for one-shot widget sounds. Round-robin so a
	// rapid hover/click stream does not cut itself off. Survives rebuilds.
	Vector<AudioStreamPlayer *> sound_players_;
	int next_sound_player_ = 0;

	void apply_screen_visibility();
	NovaMnuScreen *find_screen(const String &p_name) const;
	// Pre-order search for a widget tagged meta "mnu_hotkey" matching p_vk
	// (VK_RETURN/VK_ENTER treated as equivalent).
	Node *find_hotkey_target(Node *p_node, const String &p_vk) const;
	bool trigger_hotkey_target(Node *p_target);
	bool has_screen(const String &p_name) const;
	void on_screen_shown(const String &p_name);
	void apply_music_for_screen(NovaMnuScreen *p_screen);
	void ensure_sound_pool();
	// Resolve a trigger to a set in sound_profile_ and play its first member's
	// .wav through the pool; returns false (so the caller can fall back) when no
	// profile, no matching set, or the wav can't be resolved/decoded.
	bool play_profile_sound(const String &p_trigger);
	// Decode a resolved member dict's .wav via the resource root and play it on
	// the next pooled player with the member's pitch/volume. Returns true if it played.
	bool play_member_sound(const Dictionary &p_member);
	// Legacy SBF-keyed playback (pre-LWF). Returns true if it played.
	bool play_sbf_sound(const String &p_trigger, const String &p_file);
	// Nulls music_director_ if the host frees it out from under us (the menu does
	// not own the director, so its pointer can otherwise dangle).
	void on_director_exiting();

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _unhandled_key_input(const Ref<InputEvent> &p_event) override;
	// Script-callable adapter behind _unhandled_key_input: maps a key event to its
	// VK name and routes it (gated on edit_mode + visibility). Returns true if a
	// hotkey handled it. Exposed so tests can drive the real adapter path (the
	// engine virtual itself is not script-callable).
	bool handle_key_input(const Ref<InputEventKey> &p_key);

	// --- Properties ---
	void set_menu(const Ref<NovaMnuDocument> &p_menu);
	Ref<NovaMnuDocument> get_menu() const { return menu_; }

	void set_resource_root(const Ref<NovaResourceRoot> &p_root);
	Ref<NovaResourceRoot> get_resource_root() const { return resource_root_; }

	void set_stylesheet(const Ref<MnsStyleSheet> &p_sheet);
	Ref<MnsStyleSheet> get_stylesheet() const { return stylesheet_; }

	void set_text_resource(const Ref<RtxtStringFile> &p_text);
	Ref<RtxtStringFile> get_text_resource() const { return text_resource_; }

	// Menu SFX profile (menu.lwf). When set, hover/click resolve through it
	// (set-by-trigger -> member -> .wav) instead of the legacy SBF bank.
	void set_sound_profile(const Ref<NovaLwfData> &p_profile);
	Ref<NovaLwfData> get_sound_profile() const { return sound_profile_; }

	void set_sound_bank(const Ref<NovaSbfBank> &p_bank);
	Ref<NovaSbfBank> get_sound_bank() const { return sound_bank_; }

	void set_music_director(NovaMusicDirector *p_director);
	NovaMusicDirector *get_music_director() const { return music_director_; }

	void set_music_var_index(int p_index) { music_var_index_ = p_index; }
	int get_music_var_index() const { return music_var_index_; }

	void set_current_screen(const String &p_name);
	String get_current_screen() const { return current_screen_; }

	void set_edit_mode(bool p_edit);
	bool get_edit_mode() const { return edit_mode_; }

	// Toggle the interactive "play" preview (only meaningful with edit_mode on).
	// Rebuilds so the navigators re-wire and authored window visibility resets.
	void set_interactive(bool p_on);
	bool get_interactive() const { return interactive_; }

	void set_build_on_ready(bool p_value) { build_on_ready_ = p_value; }
	bool get_build_on_ready() const { return build_on_ready_; }

	// --- Build ---
	// build() emits screen_changed / music_changed for the initial screen, so a
	// host that wants those cues should connect before assigning `menu` (or set
	// the exported `menu` property in a scene and connect before the node's
	// _ready runs build()).
	void build();
	void clear();
	PackedStringArray get_screen_names() const;

	// --- Navigation controller (M5; ports of menu_manager.gd) ---
	// Show a screen without touching the back stack (used by set_current_screen
	// and the default build); returns false for an unknown screen.
	bool show_screen(const String &p_name);
	// Push the current screen and navigate to p_name (the in-menu "forward" move).
	bool navigate_to_screen(const String &p_name);
	// Pop back to the previous screen; with an empty stack this emits
	// quit_requested and returns false.
	bool pop_screen();
	// Cross-.mnu jump: emits menu_requested for the host to service.
	void navigate_to_menu(const String &p_file, const String &p_target_screen);
	void quit_game();
	// Show/hide/toggle a named descendant window inside the current screen.
	bool handle_window_action(const String &p_target, const String &p_state);
	// Route a virtual-key hotkey (e.g. "VK_ESCAPE") to the matching widget in the
	// current screen and fire its actions. Returns true if handled. Inert in
	// edit_mode. Public so the input adapter and tests both reach it.
	bool handle_hotkey(const String &p_vk);
	// Route one widget action by its MNU verb (screen/window/pop/quit). Emits
	// action_dispatched. Returns true if the action was handled.
	bool dispatch_action(const String &p_type, const String &p_target,
			const String &p_file, const String &p_window_state);
	void clear_navigation_stack();

	// --- Audio (M5) ---
	// Always emits sound_requested(file, trigger). When not in edit_mode and a
	// sound bank is set, resolves an entry (by trigger name, else the file stem)
	// and plays it through the pool.
	void play_widget_sound(const String &p_trigger, const String &p_file);

	// --- Aggregate widget value/selection relay (M9) ---
	// Interactive data widgets (list/combo/spinlist/table/edit/...) call this from
	// their own change handlers so a host that holds only the menu can react to any
	// widget without connecting each one. Mirrors how actions/sounds funnel here.
	// `kind` is the widget family ("list", "combo", "spinlist", "edit", ...), `index`
	// the selected/changed row (-1 when not row-based), `value` the chosen value/text.
	void notify_widget_value(const String &p_widget_name, const String &p_kind,
			int p_index, const String &p_value);

	// Count of distinct concrete asset names (textures/fonts) that could not be
	// resolved during the last build. The editor surfaces this; 0 means every
	// referenced asset loaded (or there were none).
	int get_unresolved_asset_count() const { return unresolved_asset_count_; }
};

} // namespace godot
