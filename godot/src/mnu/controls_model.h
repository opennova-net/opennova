#pragma once

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <runtime/controls/key_state_reset.h>
#include <runtime/controls/binding_set.h>
#include <runtime/controls/help_screen.h>

#include "devtools/scripted_input.h"

#include <array>

namespace godot {

class PlayerProfiles;

// GDScript-facing wrapper over the portable engine/runtime/controls catalog and
// its LIVE binding records — the game's catalog rows, which the player
// profile's binding table reaches at each session start and at the in-game
// options Accept (apply_profile; the Options screen edits its own copy,
// engine menu::OptionsScreen). Exposes the live keys to the gameplay input
// sampler and the HUD's key labels. The engine logic (catalog, key-name decode,
// binding format, record semantics) lives in engine/runtime/controls
// (binding_set.h carries the [orig:, see docs/mnu/menu-re.md] chain); this class adds only the
// Godot-device seam: the VK <-> Godot Key translation and the device sampling.
class ControlsModel : public RefCounted {
	GDCLASS(ControlsModel, RefCounted)

protected:
	static void _bind_methods();

public:
	opennova::controls::BindingSet &native_bindings() { return bindings_; }
	const opennova::controls::BindingSet &native_bindings() const { return bindings_; }

	enum Device {
		DEVICE_KEYBOARD = 0,
		DEVICE_MOUSE = 1,
		DEVICE_JOYSTICK = 2,
	};
	// The HUDLS key label's binding record base (engine controls
	// kSlotBarKeyRecordBase); the label reads base + the weapon category.
	enum {
		SLOT_BAR_KEY_RECORD_BASE = 200,
	};

	// An Array of PackedStringArray rows, each [class, action, control], for
	// the given device, from the LIVE binding records.
	TypedArray<PackedStringArray> get_rows(int p_device) const;

	// The catalog action behind a visible table row (-1 out of range).
	int action_index_for_row(int p_row) const;
	// The Control-column text for one catalog action on one device.
	String control_text(int p_action, int p_device) const;

	// Remap operations over the live records (see binding_set.h for the
	// witnessed semantics — ctrl/shift/repeat feed the original event flag
	// word; the extended-key flag derives from the VK). assign_godot_key
	// returns false when the key has no VK mapping or the engine rejects it.
	bool assign_godot_key(int p_action, int p_godot_key, bool p_ctrl,
			bool p_shift, bool p_repeat);
	void assign_mouse_mask(int p_action, int p_mask);
	void clear_binding(int p_action, int p_device);
	void restore_defaults();

	// Godot keycodes currently bound to a catalog token (primary then
	// secondary; modifiers not included — display/diagnostic use).
	PackedInt32Array godot_keys_for_token(const String &p_token) const;

	// Whether the token's binding is held RIGHT NOW: keyboard slots under the
	// dispatcher's two passes (engine BindingSet::pressed_key over Godot's
	// key state by the layout's VK: a row's modifier word must be held, and a
	// modifier-less row yields its key to a held-modifier row that claims it)
	// plus the held-sampleable mouse-mask buttons (wheel masks are
	// impulse-only and never sample). The one gameplay-sampler entry point —
	// samples Godot Input here at the device seam, plus the attached scripted
	// device's held tokens.
	bool is_token_pressed(const String &p_token) const;
	// The same, without the keyboard slots and the mouse buttons: the scripted device's
	// hold and the joystick. A keyboard key fires its row through the key queue's events
	// (the shell's KeyboardPass), a mouse button's through its click records
	// (mouse_click_code; D-CTRL-6).
	bool is_token_held_off_keyboard(const String &p_token) const;
	// Whether the token's binding is held as the fire arm's live test reads it: its key slots
	// by the key state and its mouse buttons (engine BindingSet::binding_active carries the
	// witness), its joystick button, the scripted device's hold.
	bool is_token_binding_active(const char *p_token) const;
	// The scripted device holds the token's row and the row binds the left mouse button: the
	// hold stands for that button (engine controls::PlayerActionSource::scripted_left_button).
	bool is_token_scripted_left_button(const char *p_token) const;
	// The dispatch code a mouse button message's event queues (2 / 4 / 1024 left down /
	// up / double click, 8 / 0x10 / 2048 right, 32 / 64 / 4096 middle), -1 for none, under
	// the death screen's mode gate the key scan holds (engine BindingSet::mouse_click_action
	// carries the witness).
	int mouse_click_code(int p_event) const;
	// The scenario driver's scripted input device (devtools/scripted_input.h)
	// at this seam: a token it holds reads held like a keyboard slot, so an
	// open chat line silences it the same way. Null detaches.
	void set_scripted_input(const Ref<ScriptedInput> &p_input) { scripted_input_ = p_input; }
	Ref<ScriptedInput> get_scripted_input() const { return scripted_input_; }
	// The input frame's step of the attached script at the logic tick it runs
	// as (the frame's head and each later update's; PlayerInputRouter's
	// before_world_tick and run_update_rows). Each code the step starts or stops holding also arrives as
	// its bound key's own press or release event (slot 1, else slot 2), as a
	// keyboard's would: the readers of raw key events (the deploy keys, the
	// held-USE digits) see the script the way the binding scan does. A
	// mouse-bound code holds its token only; no click is synthesized. Each
	// character a char step typed arrives as its key's press carrying the
	// character, then its release, so an open chat line's editor takes it
	// through the shell's chat route as a typed key; outside an open line it
	// reaches only the readers of raw key events (retail's keystroke would
	// also fire its key's binding).
	void advance_scripted_input(int64_t p_logic_tick);
	// The VK (0 = none) of the keyboard slot firing the token RIGHT NOW under
	// those same two passes — the sampler's seam for rules that look at WHICH
	// key fired (the USE hold's digit swallow). Mouse-mask holds report 0.
	int pressed_key_for_token(const String &p_token) const;
	// Whether the token's fire RIGHT NOW carries the sign bit (engine
	// BindingSet::fire_signed carries the rule: Shift on a modifier-less key
	// or a held mouse button) -- the mousescale step's direction.
	bool is_token_fire_signed(const String &p_token) const;
	// The chat line's keyboard capture (engine BindingSet::set_keyboard_captured
	// carries the rule and its witness): no keyboard slot fires while set.
	void set_keyboard_captured(bool p_captured) { bindings_.set_keyboard_captured(p_captured); }
	// The key scan's live gates for this frame's polls (engine
	// BindingSet::set_scan_gates carries the rule and its witness).
	void set_scan_gates(bool p_death_screen, bool p_scan_word) {
		bindings_.set_scan_gates(p_death_screen, p_scan_word);
	}
	// The special-key handler's take: a key-down it consumed never reaches the
	// action rows, so every row bound to that key reads it up until the key
	// is released (engine hud_toggles.h hud_toggles_quit_dialog_key carries
	// the witness).
	void consume_key_press(int p_vk);
	// A VK's held state as the samplers read it (a consumed press reads up).
	bool is_vk_down(int p_vk) const { return vk_down_(p_vk); }
	// The dispatch code of the binding row one dequeued key event fires, -1 for
	// none: the scan over the event's key (KeyEventQueue::last_scan_code) under
	// the key scan's live gates, the modifier words read from the key state now
	// (engine BindingSet::event_row carries the witness).
	int event_code(int p_scan_code) const;
	// Whether that fire carries the sign bit: a fallback-pass fire with Shift held
	// (engine BindingSet::event_row's fallback_signed).
	bool event_signed(int p_scan_code) const;
	// Whether a key event is one the scripted device replayed for its hold (its row
	// fires through the hold, not through the key queue's scan).
	static bool is_scripted_key_event(const Ref<InputEventKey> &p_event);
	// Whether a key bound to the token is down by the key state alone (the
	// reset-aware device state; no scan gate, no keyboard capture, a
	// special-key take included; engine controls::PlayerActionSource::keys_down
	// carries the witness).
	bool is_token_key_down(const char *p_token) const;
	// A VK down by the key state alone (the reset-aware device state; a key the
	// special-key chain took still reads down): the keyboard pass's release pass
	// reads it (engine BindingSet::release_pass carries the witness).
	bool is_key_state_down(int p_vk) const;
	// A menu close's key-state reset: every key held now reads up until its
	// next key-down, which the shell reports through key_down_event for every
	// key press and OS repeat, or its release (engine controls::KeyStateReset
	// carries the witness). The mouse buttons and the joystick hold through it.
	void reset_key_state();
	void key_down_event(int p_godot_key);
	bool is_keyboard_captured() const { return bindings_.keyboard_captured(); }
	// The in-game display string of a token's binding — the death screen's
	// "call a medic" hint formatter (engine: runtime/controls/binding_set.h,
	// format_display_string). "" when unbound.
	String display_text_for_token(const String &p_token) const;
	// The same display string for the binding record read BY ACTION CODE (the
	// start-up re-lay puts the row dispatching `code` at record `code`; engine
	// controls action_for_code): "" when no row carries the code (the zero
	// record), " *" appended when the row's flags carry 0x200. The HUDLS key
	// label reads record 200 + category.
	String display_text_for_action_code(int p_code) const;
	// The live records' revision (engine BindingSet::revision): it moves on
	// every binding write, so a label cache rebuilds only on a change.
	int get_binding_revision() const { return static_cast<int>(bindings_.revision()); }

	// The capture button->mask translation (0 = unmappable). The witnessed
	// mask values are the engine's kMouse* constants (controls/binding_set.h
	// carries the citation); this seam only maps Godot's MouseButton onto
	// them.
	static int mouse_mask_from_godot_button(int p_button);

	// VK <-> Godot Key translation (0 when unmappable).
	static int vk_from_godot_key(int p_godot_key);
	static int godot_key_from_vk(int p_vk);

	// The controls apply over the player profile's current record: its binding
	// table onto these live records and its ENABLE_JOYSTICK word into the
	// joystick gate (engine profile::apply_controls). The shell runs it at
	// every session start and at the in-game options Accept. Null: nothing.
	void apply_profile(const Ref<PlayerProfiles> &p_profiles);
	bool is_joystick_enabled() const { return bindings_.joystick_enabled(); }

	// A snapshot of the live records: token -> [primary, secondary,
	// primary_mod, secondary_mod, mouse_mask, joy_button, mouse_mod, joy_mod].
	// Tests and probes set and restore the live records through it; nothing
	// persists it (the records' home is the player profile's table).
	Dictionary save_blob() const;
	void load_blob(const Dictionary &p_blob);

	// The F1 key-binding help pages over the live records
	// (engine controls/help_screen.h): rebuilt at mission start, the page
	// keys walk them, and the HUD reads the current page's text.
	void build_help_screen();
	void cycle_help_page(bool p_forward);
	String get_help_title() const;
	String get_help_page_line() const;
	PackedStringArray get_help_keys() const;
	PackedStringArray get_help_texts() const;
	static String help_footer();

private:
	// The key state by the layout's VK under the special-key takes.
	bool vk_down_(int p_vk) const;
	// p_binding_active: the keyboard and mouse legs by BindingSet::binding_active (the slots
	// and buttons raw) in place of the dispatcher's walk.
	bool token_pressed_(const String &p_token, bool p_keyboard, bool p_mouse = true,
			bool p_binding_active = false) const;

	opennova::controls::BindingSet bindings_;
	opennova::controls::HelpScreen help_screen_;
	mutable std::array<bool, 256> consumed_vks_{};
	mutable opennova::controls::KeyStateReset key_reset_;
	Ref<ScriptedInput> scripted_input_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::ControlsModel::Device);
