#pragma once

#include <cstdint>
#include <vector>

#include <base/platform/input_sample.h>
#include <runtime/controls/input_event_queue.h>
#include <runtime/controls/key_event_queue.h>
#include <runtime/controls/player_actions.h>
#include <runtime/hud/hud_toggles.h>
#include <runtime/inmatch/session.h>
#include <runtime/inmatch/special_key_chain.h>
#include <runtime/world/player_input.h>

namespace opennova::inmatch {

// The local player's device beyond its polled events (PlayerInputFrame::device_event): the live
// binding table's reads (controls::PlayerActionSource: the rows held now under the binding
// scan's two passes), the state snapshot, and the binding table's passes over the device's key
// state. The embedder's device implements it (the Godot backend's binding model and keyboard-lock
// device today; the platform InputDevice once it lands).
class PlayerInputDevice : public controls::PlayerActionSource {
public:
	// The state snapshot as the device reads it now: the cursor, its capture, the lock keys.
	virtual platform::InputState state() const = 0;
	// The key scan's gates for the input frame's polls, the HUD's after the world tick too
	// (controls::BindingSet::set_scan_gates carries the rule and its witness).
	virtual void set_scan_gates(bool death_screen, bool scan_word) = 0;
	// An open text line owns the keyboard (controls::BindingSet::keyboard_captured).
	virtual bool keyboard_captured() const = 0;
	// The click record a mouse event queues (controls::BindingSet::mouse_click_record under the
	// scan's death-screen gate): the row's dispatch code (-1 for none) and the record's sign.
	struct ClickRecord {
		int code = -1;
		bool sign = false;
	};
	virtual ClickRecord mouse_click_record(uint32_t event) const = 0;
	// The input frame's joystick toggle pass over the device's buttons
	// (controls::BindingSet::joystick_toggle_pass): its fires, one event each in the walk's order.
	virtual std::vector<controls::KeyRowEvent> joystick_records(bool death_screen,
			bool scroll_lock) = 0;
	// The keyboard pass's release pass over the key queue's press latch and the key state
	// (controls::BindingSet::release_pass carries the witness).
	virtual std::vector<controls::KeyRowEvent> key_release_pass() = 0;
	// The key queue's ring reset alone (controls::KeyEventQueue::reset carries the witness).
	virtual void reset_key_queue() = 0;
	// The whole input reset's device half behind the ring's: the key state (every key held now
	// reads up until its next key-down; controls::KeyStateReset) and the key-press latch.
	virtual void reset_key_state() = 0;
	// The scripted input device (devtools::ScriptedInput), when one is attached: stepped to the
	// logic tick the input frame runs as (with a session only, `advance`), and its look pixels
	// since the last take. False with no scripted device (the look is left as it is).
	virtual bool step_scripted(bool advance, int64_t logic_tick, float &look_x, float &look_y) = 0;
	// Scripted movement (probes, automation): the seven movement bits a device produces, which
	// replace the movement rows and the Scroll Lock word. False with none set.
	virtual bool movement_override(world::PlayerInput &movement) const = 0;
	// Retail's Scroll Lock turn-off (controls::PlayerActionFrame::scroll_lock_off carries the
	// witness): true when the device sent it.
	virtual bool scroll_lock_turn_off() = 0;
};

// The session the frame's records reach (the embedder's over the running role and kernel).
class PlayerInputSession {
public:
	virtual ~PlayerInputSession() = default;
	// Whether a session runs (a mission is loaded); every other call is made only then.
	virtual bool available() const = 0;
	struct Facts {
		bool death_screen = false; // g_DeathScreenActive
		bool local_dead = false;
		bool dedicated_host = false;
		bool joiner = false;
		bool mp_session = false;
		uint32_t game_type = 0;
		int64_t logic_tick = -1;
	};
	virtual Facts facts() const = 0;
	// The dispatcher's head by the row's flags (Role::action_head_admits carries the witness).
	virtual bool action_head_admits(int code) = 0;
	// The held USE row's press latch (world::LocalPlayer::take_use_item_press).
	virtual controls::UseItemPressArm take_use_item_press() = 0;
	// An action request's arm, past its head (the mount toggle, the seat, the scope, the weapon
	// rows, the stance, the HUD radar and map, NVG, the waypoints, the spectator rows, the voice
	// menus' picks).
	virtual void run_action(const controls::PlayerActionRequest &request) = 0;
	// The session's own records: the exit (3), the punt by slot (34), the punt log (48). False
	// when the code is none of them.
	virtual bool dispatch_action_record(int32_t code, int32_t param) = 0;
	// The left button's down: the PowerThrow windup test (world::local_weapon_left_button_down).
	virtual void left_button_down() = 0;
	// The deferred input queue's clear (the whole input reset's).
	virtual void clear_deferred_input() = 0;
	// A drained talk row record (1 << hud chat row) through the chat line (hud::ChatEntry::
	// dispatch_rows carries the witness), with or without a local player.
	virtual void dispatch_talk_rows(uint32_t rows_fired, bool active) = 0;
	// One key-down through the special-key chain (inmatch::special_key_chain).
	virtual SpecialKeyOutcome special_key_chain(hud::HudToggleState &toggles,
			controls::PlayerActions &actions, const SpecialKeyInput &input) = 0;
};

// The shell's arms (the embedder's game flow).
class PlayerInputShell {
public:
	virtual ~PlayerInputShell() = default;
	// The keyboard pass over the key queue, run at each update's input-frame head under no menu
	// screen (controls::run_keyboard_pass over the shell's legs).
	virtual void keyboard_pass() = 0;
	// The menu-screen bit (a screen with no input frame: INGAME, WEAPON, CMAP, VEHICLE, DEATH's
	// exit confirmation), read at each update's input-frame head.
	virtual bool menu_screen_open() = 0;
	// A drained record whose arm is the shell's: the escape row's (code 18), the screen shot's
	// (code 2), the cheat row 117's weapon-screen toggle (code 40) and the deploy pick's (input
	// event 12, its pick the first argument).
	virtual void record_leg(int32_t code, int32_t param) = 0;
};

// The local player's input frame: retail's per-update input frame over the device's raw sample
// (the polled events and the state snapshot), the session's per-update input source. The frame's
// head steps the devices to the frame's first update and takes the frame's head reads (the
// cursor offset, the menu-screen bit); every update the session runs, a paused one too, runs its
// input frame here: the shell's keyboard pass (run_update_rows), then its movement and action
// rows at the logic tick it runs as (sample_tick), under the gates the frame's head took, whose
// rows (the talk, HUD and pause rows' among the action rows' in key order) become the update's
// records (queue_records) that the role's drain hands back one at a time (dispatch_record). A
// frame that runs no update runs no keyboard pass and polls no row: its keys stay in the queue
// and the row states it saw queue, in order, for the next update, as retail's queue keeps each
// key event for the next update's input frame (TickInputSource carries the witness).
// [orig: Input_ProcessFrame @0x49D520 (the Input_ProcessPlayerFrame call @0x49D58B, the drain
//  @0x49D599..0x49D5E7 with the Input_HandleActionBinding call @0x49D5D8, the release edge
//  @0x49D6C1..0x49D6DC)]
class PlayerInputFrame final : public TickInputSource {
public:
	// The seams, which outlive every call into the frame (the embedder's members).
	void bind(PlayerInputDevice *device, PlayerInputSession *session, PlayerInputShell *shell);
	// The HUD's key toggles, whose F9 Emotes / F10 Radio menus take a digit before the weapon
	// rows do (special_key): the embedder's process-lifetime state. Null = no menus, and no HUD
	// rows dispatched.
	void set_hud_toggles(hud::HudToggleState *toggles) { hud_toggles_ = toggles; }
	// The device's teardown: the scan gates down and every queued row dropped.
	void teardown();

	// The frame's head reads (begin_frame).
	struct FrameHead {
		uint64_t sequence = 0;
		// The frame has a presenter: this frame is its updates' tick source.
		bool sourced = false;
		// The frame has a local player to sample.
		bool player = false;
		// The death screen is up (g_DeathScreenActive).
		bool spectating = false;
		// The mouse motion as the cursor offset in whole pixels, its sub-pixel rest carried to
		// the next frame, which the session's mouse pump reads in the frame's first update and
		// spreads over the frame's updates (world::MouseAxisPump).
		int32_t cursor_dx = 0;
		int32_t cursor_dy = 0;
		bool menu_open = false;
	};
	// The frame's head: a Scroll Lock turn-off the Escape key's reset owes goes out ahead of the
	// frame's sample, the devices step to the logic tick the first update runs as, and the gates
	// are set. `presenter` = the embedder has a local-player presenter; `has_player` = it has a
	// player to sample; `menu_open` = a menu screen (INGAME, WEAPON, CMAP) is up, under which no
	// input frame runs (controls::PlayerActionPoll::menu_open).
	FrameHead begin_frame(bool presenter, bool has_player, bool gameplay_input_active,
			bool menu_open);
	// The Escape key's Player_ResetLocalInput ran past its local-entity test: the next frame's
	// head turns Scroll Lock off (Role::reset_local_input's return).
	void owe_scroll_lock_turn_off() { scroll_lock_turn_off_owed_ = true; }

	// TickInputSource: an update's input frame.
	InputPacket sample_tick(uint32_t logic_tick) override;
	void run_update_rows(uint32_t logic_tick, bool new_sample) override;
	void queue_records(controls::InputEventQueue &records) override;
	void dispatch_record(const controls::InputEventRecord &record) override;
	void end_input_frame() override;

	// One polled device event under live gameplay with a player (the embedder gates the rest):
	// the mouse motion adds up the cursor offset, a button's message and each whole wheel notch
	// queue their click record for the next update's message pump. True when the event is the
	// frame's (the motion, a wheel turn).
	bool device_event(const platform::InputEvent &event);
	// One row event the keyboard pass's scan queued: the row's dispatch code, the sign bit its
	// fire carries and the event's key. The update's sample takes the events in key order, an
	// action row's through its poll, a talk or HUD row's as its record
	// (controls::PlayerActionPoll::key_events and embedder_row carry the witness).
	void queue_row_event(int code, bool sign, int key);
	// The special-key chain's deploy pick (input event 12, its parameter the record's first
	// argument), in key order with the rows' events (controls::KeyRowEvent carries the witness).
	void queue_pick_event(int32_t param);
	// The keyboard pass's release pass, ahead of its first dequeue.
	void run_key_release_pass();
	// A pass-through screen closed (death.mnu's DEATH, stat.mnu's STAT): its close's input frame
	// takes the menu cursor's offset from the re-centre point as one update's axis input
	// (world::pass_through_close_axis carries the witness). The next sample takes it.
	void close_pass_through_screen();
	// A menu close's input reset: the key queue, the key state, the press latch, every queued
	// row and record, and the session's deferred queue.
	void reset_input_state();
	// Drop the trigger latches (the presenter's reset-state path); the switch latches and the
	// wheel remainder survive it.
	void reset();

	// A drained HUD toggle row record (`rows_fired`, 1 << hud::HudToggleRow) through the HUD's
	// toggles, whose menus the later action rows read; a mousescale event carries the record's
	// first argument above its event bits. True when the pause row flipped the HUD's pause word.
	bool dispatch_hud_rows(PlayerInputSession &session, uint32_t rows_fired,
			bool mouse_scale_negative, int32_t mouse_scale_param);
	// The HUD toggle rows' event words of the input frames since the last read, in order;
	// reading clears them.
	std::vector<uint64_t> take_hud_toggle_events();
	// One key-down through the special-key chain over a session: a digit arm's pick runs at once,
	// past no head.
	SpecialKeyOutcome special_key(PlayerInputSession &session, const SpecialKeyInput &input);

	// The shell consumed the live USE hold for a chord of its own.
	void consume_use_hold() { actions_.consume_use_hold(); }
	bool use_held() const { return actions_.use_held(); }
	// The held USE row's armory / vehicle-loadout arm took its open; reading clears it.
	bool take_armory_request();
	bool take_vehicle_menu_request();

	// Which records' arms are whose (the embedder's dispatch and the poll's embedder rows).
	static int talk_row_of(int code);
	static int hud_row_of(int code);
	static bool shell_record(int code);

private:
	struct RowSample {
		uint32_t talk = 0;
		uint32_t hud = 0;
	};

	bool session_live() const { return session_ != nullptr && session_->available(); }
	bool step_devices(int64_t logic_tick, float &look_x, float &look_y);
	void queue_row_sample();
	uint32_t talk_rows_down() const;
	uint32_t hud_rows_down() const;
	void sample(InputPacket &packet, float look_x, float look_y, bool spectating);
	void apply_immediate(const controls::PlayerActionFrame &frame);
	void apply_player_action(PlayerInputSession &session,
			const controls::PlayerActionRequest &request);
	void queue_mouse_event_record(uint32_t event);
	void clear_queues();

	PlayerInputDevice *device_ = nullptr;
	PlayerInputSession *session_ = nullptr;
	PlayerInputShell *shell_ = nullptr;
	hud::HudToggleState *hud_toggles_ = nullptr;
	controls::PlayerActions actions_;
	std::vector<uint64_t> hud_toggle_events_;
	bool armory_requested_ = false;
	bool vehicle_menu_requested_ = false;
	// The wheel notch remainder. Retail clears it only at process start, so it survives the
	// reset-state path like the switch latches.
	controls::WheelRemainder wheel_;
	// The mouse motion since the frame head's take, in pixels.
	float look_x_ = 0.0f;
	float look_y_ = 0.0f;
	// The axis input a pass-through screen's close owes the next sample.
	float close_axis_x_ = 0.0f;
	float close_axis_y_ = 0.0f;
	uint64_t frame_sequence_ = 0;
	// The gates the frame's head took, which its updates' samples keep.
	bool frame_player_ = false;
	bool frame_gameplay_input_active_ = false;
	bool frame_menu_open_ = false;
	// The talk and HUD rows' off-keyboard holds the samples since the last update's poll saw,
	// in order, each kept when it differs from the one before: the update takes each one's
	// press edges in turn, as the drain dispatches the queued events.
	std::vector<RowSample> row_samples_;
	// The off-keyboard holds the last edge read, which a press edge leaves behind.
	uint32_t talk_held_ = 0;
	uint32_t hud_held_ = 0;
	// The talk and HUD rows' off-keyboard press edges of the update's input frame, as records,
	// until queue_records.
	std::vector<controls::InputEventRecord> row_records_;
	// The keyboard pass's row events (its scan's and its release pass's) since the last device
	// sample, in order, the talk and HUD rows' among the action rows'.
	std::vector<controls::KeyRowEvent> action_events_;
	// The last sample's action-row records, in order, until queue_records.
	std::vector<controls::InputEventRecord> sample_records_;
	// The wheel notches' and the mouse buttons' click records since the last update's message
	// pump, in order, queued ahead of the sample's.
	std::vector<controls::InputEventRecord> pump_records_;
	// A left button's down since the last update's message pump (or the scripted fire hold's
	// press): its PowerThrow windup test runs at that pump.
	bool left_down_owed_ = false;
	// The device step of the frame's head or of a later update: the look pixels and the death
	// screen the update's sample_tick reads.
	float tick_look_x_ = 0.0f;
	float tick_look_y_ = 0.0f;
	bool tick_spectating_ = false;
	// This display frame sent the Scroll Lock turn-off (D-NET-563): the keyboard state reads it
	// only once the window's messages are read, so the frame's later samples read the word off
	// and send no second one.
	bool scroll_lock_turned_off_ = false;
	bool scroll_lock_turn_off_owed_ = false;
};

} // namespace opennova::inmatch
