#pragma once

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#include <cstdint>
#include <runtime/inmatch/player_input_frame.h>

#include "hud/hud_chat_entry.h"
#include "hud/hud_toggles.h"
#include "mnu/controls_model.h"
#include "player/key_event_queue.h"
#include "player/player_move_intent.h"
#include "player/special_key_outcome.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/inmatch_session_values.h"

namespace godot {

class LocalPlayerPresenter;
class Simulation;

// The local player's input device and its seams, a plain member of LocalPlayerPresenter. The
// input frame itself is the engine's (inmatch::PlayerInputFrame: the record queues and their
// order, the talk and HUD rows' edges, the drained records' arms, the mouse and wheel events,
// the head-drop mask and the role bits); this binding only samples Godot's devices into the
// engine's raw input sample (player/input_device.h) and implements the frame's seams: the
// device (the live binding table, ControlsModel, the key queue and the keyboard-lock device),
// the session (Simulation, re-resolved per use) and the shell (the GDScript shell's legs). It
// also keeps the presenter's device work around the frame's head: the fly-camera lock, the
// models' lifetime and the mouse capture. A null binding model reads every token released.
class PlayerInputRouter final {
public:
	PlayerInputRouter();
	// The engine frame holds this router's seams by address.
	PlayerInputRouter(const PlayerInputRouter &) = delete;
	PlayerInputRouter &operator=(const PlayerInputRouter &) = delete;

	// The world serves the sim; the presenter serves the presentation surfaces this router
	// drives around the sample (fly-camera lock, model lifetime).
	void setup(Node *p_world, LocalPlayerPresenter *p_presenter, const Ref<ControlsModel> &p_controls);
	void teardown();

	// The process's keyboard-lock legs, under a display with a keyboard (engine
	// controls::kVkNumLock carries the witness): the start forces Num Lock on, saving its
	// state, and turns Scroll Lock off; the normal exit turns Scroll Lock off, puts Num Lock
	// back and turns Caps Lock off, once the start ran.
	static void start_process_num_lock();
	static void start_process_scroll_lock();
	static void exit_process_key_locks();
	// The Escape key's reset owes the next frame's head a Scroll Lock turn-off
	// (inmatch::PlayerInputFrame::owe_scroll_lock_turn_off).
	void owe_scroll_lock_turn_off() { frame_.owe_scroll_lock_turn_off(); }

	// Scripted input (probes, automation, the presenter test): the same seven movement bits a
	// device produces, so the sim sees one contract. Null = poll the live bindings again.
	void set_input_override(const Ref<PlayerMoveIntent> &p_intent);

	// The frame's head (inmatch::PlayerInputFrame::begin_frame); the returned frame names the
	// engine's input frame as its per-tick source. `p_menu_open`: a menu screen (INGAME,
	// WEAPON, CMAP) is up, under which no input frame runs.
	Ref<MissionFrameInput> before_world_tick(double p_delta, bool p_capture_mouse,
			bool p_gameplay_input_active, bool p_menu_open = false);
	// The shell's keyboard pass over the key queue (its KeyboardPass over the shell's legs),
	// run at each update's input-frame head under no menu screen. Kept across a teardown, as
	// the shell installs it once.
	void set_keyboard_pass(const Callable &p_pass) { shell_.keyboard_pass_ = p_pass; }
	// The shell's menu-screen bit, read again at each update's input-frame head. Kept across a
	// teardown; unset, the frame head's read holds for the frame.
	void set_menu_screen_probe(const Callable &p_probe) { shell_.menu_screen_probe_ = p_probe; }
	// The shell's arms for the records it owns, called as (code, first argument): the escape
	// row's (code 18), the screen shot's (code 2), the cheat row 117's weapon-screen toggle
	// (code 40) and the deploy pick's (input event 12, its pick). Kept across a teardown;
	// unset, those records do nothing.
	void set_record_leg(const Callable &p_leg) { shell_.record_leg_ = p_leg; }
	// One Godot input event under live gameplay (inmatch::PlayerInputFrame::device_event).
	bool handle_input(const Ref<InputEvent> &p_event, bool p_active);

	// Drop the trigger latches (the presenter's reset-state path).
	void reset() { frame_.reset(); }
	void release_mouse_capture();
	// A game screen frees the captured cursor: the free menu cursor starts at the re-centre
	// point in the window's client pixels, where every gameplay update left the cursor (engine
	// world::kCursorCentreX / kCursorCentreY carry the witness).
	void release_capture_for_screen();
	// The shell consumed the live USE-ITEM hold for a chord of its own.
	void consume_use_hold() { frame_.consume_use_hold(); }
	bool is_use_held() const { return frame_.use_held(); }
	// One key-down through the special-key chain over the session (the shell's world view's;
	// null = this router's world's): its VK and the KeyPress gametext keys' VKs.
	Ref<SpecialKeyOutcome> special_key(const Ref<Simulation> &p_session, int p_vk, int p_yes_vk,
			int p_no_vk, int p_restart_vk);
	bool take_armory_request() { return frame_.take_armory_request(); }
	bool take_vehicle_menu_request() { return frame_.take_vehicle_menu_request(); }
	// A menu close's input reset (inmatch::PlayerInputFrame::reset_input_state).
	void reset_input_state() { frame_.reset_input_state(); }
	// The device's key queue, one per process: the shell feeds it every key-down in every
	// state (push_key_event) and each input frame's keyboard pass drains it.
	Ref<KeyEventQueue> key_events();
	// One key-down, an OS repeat included, into the queue with Ctrl and Shift as the key state
	// holds them (engine controls::queued_key carries the witness).
	void push_key_event(const Ref<InputEventKey> &p_event);
	void queue_row_event(int p_code, bool p_signed, int p_key) {
		frame_.queue_row_event(p_code, p_signed, p_key);
	}
	void queue_pick_event(int p_param) { frame_.queue_pick_event(p_param); }
	void run_key_release_pass() { frame_.run_key_release_pass(); }
	void close_pass_through_screen() { frame_.close_pass_through_screen(); }
	// The HUD's key toggles; the HUD presenter hands its process-lifetime instance over. Null =
	// no menus, and no HUD rows dispatched.
	void set_hud_toggles(const Ref<HudToggles> &p_toggles);
	// The HUD toggle rows' event words of the input frames since the last read, in order;
	// reading clears them. The HUD presenter applies them at its frame.
	PackedInt64Array take_hud_toggle_events();
	// A HUD toggle row record (`p_rows_fired`, 1 << HudToggles::Row) through the HUD's toggles
	// over `p_sim` (inmatch::PlayerInputFrame::dispatch_hud_rows). True when the pause row
	// flipped the HUD's pause word.
	bool poll_hud_toggles(const Ref<Simulation> &p_sim, int p_rows_fired,
			bool p_mouse_scale_negative, int32_t p_mouse_scale_param = 0);
	// The HUD's chat line and the gametext its prompts read: the talk rows dispatch through it.
	void set_hud_chat(const Ref<HudChatEntry> &p_chat, const Ref<RtxtStringFile> &p_gametext);

private:
	// The live binding table and the device reads (the frame's PlayerInputDevice).
	class Device final : public opennova::inmatch::PlayerInputDevice {
	public:
		explicit Device(PlayerInputRouter &p_router) : router_(p_router) {}
		bool pressed(const char *p_token) const override;
		int pressed_key(const char *p_token) const override;
		bool fire_signed(const char *p_token) const override;
		bool keys_down(const char *p_token) const override;
		bool binding_active(const char *p_token) const override;
		bool key_slots_down(const char *p_token) const override;
		bool held_off_keyboard(const char *p_token) const override;
		bool scripted_left_button(const char *p_token) const override;
		opennova::platform::InputState state() const override;
		void set_scan_gates(bool p_death_screen, bool p_scan_word) override;
		bool keyboard_captured() const override;
		ClickRecord mouse_click_record(uint32_t p_event) const override;
		std::vector<opennova::controls::KeyRowEvent> joystick_records(bool p_death_screen,
				bool p_scroll_lock) override;
		std::vector<opennova::controls::KeyRowEvent> key_release_pass() override;
		void reset_key_queue() override;
		void reset_key_state() override;
		bool step_scripted(bool p_advance, int64_t p_logic_tick, float &r_look_x,
				float &r_look_y) override;
		bool movement_override(opennova::world::PlayerInput &r_movement) const override;
		bool scroll_lock_turn_off() override;

	private:
		PlayerInputRouter &router_;
	};

	// The session the records reach (the frame's PlayerInputSession): the router's world's sim,
	// re-resolved per use, or a given one.
	class Session final : public opennova::inmatch::PlayerInputSession {
	public:
		// `p_fixed`: the session the call names (it outlives this seam), else the world's.
		explicit Session(PlayerInputRouter &p_router, const Ref<Simulation> *p_fixed = nullptr) :
				router_(p_router), fixed_(p_fixed) {}
		bool available() const override;
		Facts facts() const override;
		bool action_head_admits(int p_code) override;
		opennova::controls::UseItemPressArm take_use_item_press() override;
		void run_action(const opennova::controls::PlayerActionRequest &p_request) override;
		bool dispatch_action_record(int32_t p_code, int32_t p_param) override;
		void left_button_down() override;
		void clear_deferred_input() override;
		void dispatch_talk_rows(uint32_t p_rows_fired, bool p_active) override;
		opennova::inmatch::SpecialKeyOutcome special_key_chain(
				opennova::hud::HudToggleState &p_toggles, opennova::controls::PlayerActions &p_actions,
				const opennova::inmatch::SpecialKeyInput &p_input) override;

	private:
		Ref<Simulation> sim() const;
		PlayerInputRouter &router_;
		const Ref<Simulation> *fixed_;
	};

	// The GDScript shell's legs (the frame's PlayerInputShell).
	class Shell final : public opennova::inmatch::PlayerInputShell {
	public:
		void keyboard_pass() override;
		bool menu_screen_open() override;
		void record_leg(int32_t p_code, int32_t p_param) override;
		Callable keyboard_pass_;
		Callable menu_screen_probe_;
		Callable record_leg_;
	};

	Ref<Simulation> sim() const;
	LocalPlayerPresenter *presenter() const;

	ObjectID world_id_;
	ObjectID presenter_id_;
	Ref<ControlsModel> controls_;
	Ref<PlayerMoveIntent> input_override_;
	Ref<HudToggles> hud_toggles_;
	Ref<HudChatEntry> hud_chat_;
	Ref<RtxtStringFile> chat_gametext_;
	Ref<KeyEventQueue> key_events_;
	Device device_;
	Session session_;
	Shell shell_;
	opennova::inmatch::PlayerInputFrame frame_;
};

} // namespace godot
