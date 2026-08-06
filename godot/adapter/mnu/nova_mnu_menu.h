#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <audio/sound_selector.h>

#include "audio/nova_sbf_bank.h"
#include "lwf/nova_lwf_data.h"
#include "mns_stylesheet.h"
#include "nova_mnu_document.h"
#include "nova_mnu_widget_common.h"
#include "resource_index/nova_resource_root.h"
#include "rtxt/rtxt_string_file.h"

namespace godot {

class NovaMnuScreen;
class NovaMnuCombo;
class NovaMusicDirector;
class AudioStreamPlayer;
class InputEventKey;

// Runtime menu node: point it at a NovaMnuDocument and it builds the live widget
// tree (one NovaMnuScreen child per screen). Assets resolve through an optional
// NovaResourceRoot / MnsStyleSheet / RtxtStringFile (M4). It is also the menu's
// navigation controller (M5): widgets dispatch their actions here (screen nav +
// stack, window show/hide, pop, quit) and ask it to play sounds. Menu music is
// driven through an optional NovaMusicDirector and sounds through an optional
// NovaSbfBank (PR #29); both are also surfaced as signals so a shell can react
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
	// Menu SFX: each widget <SOUND> names its own .lwf bank file (the element
	// text) and a sound set inside it (TRIGGER). Banks load on first use into
	// lwf_banks_ — the analog of the original's global refcounted bank
	// collection [orig: sound_bank_collection_add_or_ref @ 0x652b40 over
	// dword_31C1704; entries dedup by stricmp on the file name; lifetime = the
	// UI manager @ 0x64af10/0x64afe0]. A failed load caches a null bank so each
	// bad name is tried once (the original leaves the element's bank id 0).
	//
	// sound_profile_ is a shell-set authoring fallback for <SOUND> nodes that
	// carry no file (the original fails the element parse in that case);
	// sound_bank_ is the legacy pre-LWF SBF path, kept only as a last fallback.
	struct LwfBankEntry {
		Ref<NovaLwfData> bank;
		int id = 0; // selector key space; 0 == failed load
	};
	HashMap<String, LwfBankEntry> lwf_banks_;
	int next_lwf_bank_id_ = 1;
	// Per-(bank,set,layer) member selection, sharing the engine's selection
	// machine with the mission sound stack [orig: selection branches
	// @ 0x75cd5c..0x75cdfc, RNG @ 0x85A3DC].
	opennova::audio::SoundSelector sound_selector_;
	// Channel volume for UI sounds, 0..255 [orig: CGameMenu+92, ctor default
	// 0xFF @ 0x63e060, read @ 0x63b670 into the play params @ 0x647bd4].
	int master_volume_ = 255;
	Ref<NovaLwfData> sound_profile_;
	Ref<NovaSbfBank> sound_bank_;
	NovaMusicDirector *music_director_ = nullptr;
	int music_var_index_ = 0;
	String current_screen_;
	bool edit_mode_ = false;
	// Layered on top of edit_mode_ for the ONED "play" preview: keeps the authoring
	// suppressions (music/cursor/all-screens) but re-wires navigators so clicking a
	// tab runs its window show/hide actions. External side effects (quit/url/cross-
	// .mnu jump) are clamped to no-ops so the sandbox cannot reach the editor shell.
	bool interactive_ = false;
	bool build_on_ready_ = true;
	int unresolved_asset_count_ = 0;

	// This menu's own .mnu basename (e.g. "main.mnu"), set by the shell that
	// built it. Shipped screen actions spell same-file jumps with their own
	// filename, so dispatch compares against this (case-insensitive).
	String menu_file_;

	// Within-menu back navigation stack of screen names (cross-.mnu jumps are
	// shell policy, emitted as menu_requested rather than pushed here).
	Vector<String> nav_stack_;

	// Lazily created pooled players for one-shot widget sounds. Round-robin so a
	// rapid hover/click stream does not cut itself off. Survives rebuilds.
	Vector<AudioStreamPlayer *> sound_players_;
	int next_sound_player_ = 0;

	// The combo whose dropdown is currently open -- at most one per menu, the
	// scene-scoped analog of the original's active-combo/open-popup globals
	// [orig: g_ui_active_combo_wnd @ 0x31C16D0, g_ui_open_popup_wnd @ 0x31C16D8].
	// The combo registers on open (closing any previous one) and unregisters on
	// close; screen navigation closes it (docs/mnu/menu-re.md D-MNU-11).
	NovaMnuCombo *active_combo_ = nullptr;

	void apply_screen_visibility();
	NovaMnuScreen *find_screen(const String &p_name) const;
	// Pre-order search for the first visible widget whose ordered accelerator
	// list matches. Virtual and character bindings are kept separate because
	// "VK_RETURN" and the literal text "V" have different input semantics.
	Node *find_hotkey_target(Node *p_node, const String &p_key, bool p_virtual) const;
	bool trigger_hotkey_target(Node *p_target);
	bool has_screen(const String &p_name) const;
	void on_screen_shown(const String &p_name);
	void apply_music_for_screen(NovaMnuScreen *p_screen);
	void ensure_sound_pool();
	// Load (or fetch from cache) the .lwf bank a <SOUND> element names. Returns
	// null (and r_bank_id 0) when the file is empty/unresolvable.
	Ref<NovaLwfData> resolve_sound_bank(const String &p_file, int &r_bank_id);
	// Find p_trigger's set in the bank (case-insensitive, like the original's
	// stricmp lookup) and play every layer: one member per layer via the shared
	// selection machine, with the witnessed volume/pitch composition.
	// [orig: SoundBank_FindTriggerByName @ 0x75be90 ->
	//  SoundBank_PlayTriggerEntries @ 0x75ccd0]
	bool play_lwf_set(const Ref<NovaLwfData> &p_bank, int p_bank_id, const String &p_trigger);
	// Decode a member dict's .wav via the resource root and play it on the next
	// pooled player at the given channel volume (0..255) and pitch scale.
	bool play_member_sound(const Dictionary &p_member, int p_vol255, double p_pitch_scale);
	// Legacy SBF-keyed playback (pre-LWF). Returns true if it played.
	bool play_sbf_sound(const String &p_trigger, const String &p_file);
	// Nulls music_director_ if the shell frees it out from under us (the menu does
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

	// Authoring-fallback SFX profile for <SOUND> nodes without a bank file.
	// Widget sounds normally resolve their own bank by file name (see
	// lwf_banks_); the original engine has no profile concept.
	void set_sound_profile(const Ref<NovaLwfData> &p_profile);
	Ref<NovaLwfData> get_sound_profile() const { return sound_profile_; }

	// UI channel volume, 0..255 [orig: CGameMenu+92, default 0xFF]. With no
	// layer distances (every shipped menu layer) this IS the playback volume.
	void set_master_volume(int p_volume);
	int get_master_volume() const { return master_volume_; }

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

	// The menu's own .mnu basename for self-file screen actions (see menu_file_).
	void set_menu_file(const String &p_file) { menu_file_ = p_file; }
	String get_menu_file() const { return menu_file_; }

	// --- Build ---
	// build() emits screen_changed / music_changed for the initial screen, so a
	// shell that wants those cues should connect before assigning `menu` (or set
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
	// Cross-.mnu jump: emits menu_requested for the shell to service.
	void navigate_to_menu(const String &p_file, const String &p_target_screen);
	void quit_game();
	// Show/hide/enable/disable a named descendant window inside the current
	// screen. TOGGLE inverts the property selected by STATE.
	bool handle_window_action(const String &p_target, const String &p_state,
			bool p_toggle = false);
	// Route a virtual-key hotkey (e.g. "VK_ESCAPE") to the matching widget in the
	// current screen and fire its actions. Returns true if handled. Inert in
	// edit_mode. Public so the input adapter and tests both reach it.
	bool handle_hotkey(const String &p_vk);
	// Route a literal Unicode accelerator (case-insensitive). Kept separate from
	// handle_hotkey so a non-VIRTUAL "VK_ESCAPE" remains literal text.
	bool handle_character_hotkey(const String &p_character);
	// Route one widget action by its MNU verb (screen/window/pop/quit). Emits
	// action_dispatched. Returns true if the action was handled.
	bool dispatch_action(const String &p_type, const String &p_target,
			const String &p_file, const String &p_window_state);
	// Complete native action path used by built widgets. Unlike the script
	// compatibility wrapper above, this retains TOGGLE and shell-owned fields.
	bool dispatch_widget_action(const MnuActionData &p_action);
	void clear_navigation_stack();

	// --- Audio (M5) ---
	// Always emits sound_requested(file, trigger). When not in edit_mode and a
	// sound bank is set, resolves an entry (by trigger name, else the file stem)
	// and plays it through the pool.
	void play_widget_sound(const String &p_trigger, const String &p_file);

	// --- Combo dropdown exclusivity ---
	// One dropdown open per menu [orig: single-open toggle in combobox_handle_event
	// @ 0x65c190 (@ 0x65c210); the open list owns all mouse input via
	// g_ui_open_popup_wnd]. register closes the previously active combo's popup;
	// unregister is identity-checked so stale closes are harmless.
	void register_open_combo(NovaMnuCombo *p_combo);
	void unregister_open_combo(NovaMnuCombo *p_combo);
	// Close the open dropdown, if any. Screen navigation calls this the way the
	// original clears the popup/capture globals on a screen switch
	// [orig: CUIScene_SelectNodeByName @ 0x63b6b0]. Returns true if one closed.
	bool close_active_combo_popup();

	// --- Aggregate widget value/selection relay (M9) ---
	// Interactive data widgets (list/combo/spinlist/table/edit/...) call this from
	// their own change handlers so a shell that holds only the menu can react to any
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
