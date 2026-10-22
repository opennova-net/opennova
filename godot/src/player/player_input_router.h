#pragma once

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstdint>
#include <runtime/controls/player_actions.h>
#include <vector>

#include "hud/hud_chat_entry.h"
#include "hud/hud_toggles.h"
#include "mnu/controls_model.h"
#include "player/key_event_queue.h"
#include "player/player_move_intent.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/inmatch_session_values.h"

namespace godot {

class LocalPlayerPresenter;
class Simulation;

// The local player's input sampling/routing (the former player_input_router.gd,
// ADR 0043 slice G8), a plain member of LocalPlayerPresenter: the
// movement/device sampling into the native controls::PlayerActions table,
// request forwarding, mouse look, and mouse capture/release. The presenter keeps the camera cluster and the
// avatar/viewmodel presentation; the camera MODE is the sim's resolved word
// (the arbiter over the chase preference and the seat), mirrored by the
// presenter -- no key here flips it. The presenter's before_world_tick /
// handle_input stay the externally pinned names and delegate here. The live binding table arrives as the ControlsModel the
// shell's ControlsBindings singleton owns (persistence stays shell-side); a
// null model reads every token released.
//
// The router is also the session's per-update input source (engine
// inmatch::TickInputSource carries the witness): before_world_tick steps the
// devices to the frame's first update and takes the frame's head reads (the
// cursor offset, the menu-screen bit); every update the session runs, a paused one
// too, runs its input frame here: the shell's keyboard pass, then its talk and
// HUD rows (run_update_rows), and an update that runs a tick, the first included,
// samples its movement and action rows at the logic tick it runs as (sample_tick),
// under the gates the frame's head took. A frame that runs no update runs no
// keyboard pass and polls no row: its keys stay in the queue and the row states it
// saw queue, in order, for the next update, as retail's queue keeps each key event
// for the next update's input frame.
class PlayerInputRouter final : public opennova::inmatch::TickInputSource {
public:
	// The world serves the sim; the presenter serves the presentation surfaces
	// this router drives around the sample (fly-camera lock, model lifetime,
	// the head-bone eye).
	void setup(Node *p_world, LocalPlayerPresenter *p_presenter, const Ref<ControlsModel> &p_controls);
	void teardown();

	// The process's keyboard-lock legs, under a display with a keyboard (engine
	// controls::kVkNumLock carries the witness): the start forces Num Lock on, saving its
	// state, and turns Scroll Lock off; the normal exit turns Scroll Lock off, puts Num Lock
	// back and turns Caps Lock off, once the start ran.
	static void start_process_num_lock();
	static void start_process_scroll_lock();
	static void exit_process_key_locks();
	// The Escape key's Player_ResetLocalInput ran past its local-entity test: the next
	// frame's head turns Scroll Lock off (inmatch::Role::reset_local_input's return).
	void owe_scroll_lock_turn_off();

	// Scripted input (probes, automation, the presenter test): the same seven
	// movement bits a device produces, so the sim sees one contract. Null =
	// poll the live bindings again.
	void set_input_override(const Ref<PlayerMoveIntent> &p_intent);

	// `p_menu_open`: a menu screen (INGAME, WEAPON, CMAP) is up, under which
	// no input frame runs (controls::PlayerActionPoll::menu_open). The
	// returned frame names this router as its per-tick source.
	Ref<MissionFrameInput> before_world_tick(double p_delta, bool p_capture_mouse,
			bool p_gameplay_input_active, bool p_menu_open = false);
	// An update's movement and action rows (the frame's first update's included),
	// over the device state the frame's head or the update's run_update_rows
	// stepped to, after that update's keyboard pass.
	opennova::inmatch::InputPacket sample_tick(uint32_t p_logic_tick) override;
	// An update's input frame: the shell's keyboard pass at its head (the callable
	// set_keyboard_pass installs), then its talk and HUD rows; each pause row they
	// dispatch flips `r_pause`, whose word the rows behind it read at the head.
	// `p_new_sample` steps the device to the update's logic tick first (a later
	// update of a running frame).
	void run_update_rows(uint32_t p_logic_tick, bool p_new_sample,
			opennova::inmatch::PauseRowWord &r_pause) override;
	// The update's sample's action-row records (its requests with a code), in order,
	// into the role's event queue, and a drained one back to its arm through the
	// dispatcher's head (engine inmatch::TickInputSource carries the witness).
	void queue_records(opennova::controls::InputEventQueue &r_records) override;
	void dispatch_record(const opennova::controls::InputEventRecord &p_record) override;
	// The shell's keyboard pass over the key queue (its KeyboardPass over the
	// shell's legs), run at each update's input-frame head under no menu screen;
	// it decides itself whether its shell state drains the keys. Kept across a
	// teardown, as the shell installs it once.
	void set_keyboard_pass(const Callable &p_pass) { keyboard_pass_ = p_pass; }
	bool handle_input(const Ref<InputEvent> &p_event, bool p_active);

	// Drop the trigger latches (the presenter's reset-state path). The
	// switch-key latches are deliberately NOT reset: a category key held across
	// a mission reload must not fire a spurious switch edge on the first new-sim
	// frame.
	void reset();
	void release_mouse_capture();
	// A game screen frees the captured cursor: the free menu cursor starts at the
	// re-centre point in the window's client pixels, where every gameplay update
	// left the cursor (engine world::kCursorCentreX / kCursorCentreY carry the
	// witness), the frame close_pass_through_screen reads.
	void release_capture_for_screen();
	// The shell consumed the live USE-ITEM hold for a chord of its own (the
	// debug pick rides Shift+F6; the tools window opening mid-hold): the
	// release edge then runs no mount toggle. Applied after this frame's
	// fresh-press reset so a same-frame chord sticks.
	void consume_use_hold();
	// The USE hold the special-key chain's held-USE arm reads
	// (controls::PlayerActions::use_held carries the witness).
	bool is_use_held() const { return actions_.use_held(); }
	// The held USE row's armory arm took its open on the last frame's poll
	// (controls::PlayerAction::OpenArmory); reading clears it. The shell
	// opens weapon.mnu's WEAPON screen.
	bool take_armory_request();
	// The vehicle-loadout arm took its open on the last frame's poll
	// (controls::PlayerAction::OpenVehicleMenu); reading clears it. The shell
	// opens vehicle.mnu's VEHICLE screen.
	bool take_vehicle_menu_request();
	// A menu close's input reset: the key queue (KeyEventQueue::reset), the key
	// state (ControlsModel::reset_key_state), the key-press latch
	// (KeyEventQueue::clear_press_latch) and the deferred queue
	// (Simulation::clear_deferred_input).
	void reset_input_state();
	// The device's key queue, one per process: the shell feeds it every key-down in
	// every state (push_key_event) and each input frame's keyboard pass drains it
	// (the shell's KeyboardPass); every input reset the shell runs resets it.
	Ref<KeyEventQueue> key_events();
	// One key-down, an OS repeat included, into the queue with Ctrl and Shift as
	// the key state holds them (KeyEventQueue::push_key).
	void push_key_event(const Ref<InputEventKey> &p_event);
	// One row event the keyboard pass's scan queued (the shell's KeyboardPass):
	// the row's dispatch code, the sign bit its fire carries and the event's key
	// (its scan code). The pass runs at an update's input-frame head: a talk or HUD
	// row's event dispatches behind it in that input frame (run_update_rows), an
	// action row's in the update's sample, whose action poll fires it
	// (controls::PlayerActionPoll::key_events carries the witness).
	void queue_row_event(int p_code, bool p_signed, int p_key);
	// The keyboard pass's release pass, ahead of its first dequeue: each key whose
	// press latch is set and that reads up queues its release row's event for the
	// next device sample (engine controls::BindingSet::release_pass carries the
	// witness).
	void run_key_release_pass();
	// A pass-through screen closed (death.mnu's DEATH, stat.mnu's STAT): its
	// close's input frame takes the menu cursor's offset from the re-centre
	// point as one update's axis input, read here from the cursor's
	// client-area position (engine world::pass_through_close_axis carries the
	// witness). The next sample takes it.
	void close_pass_through_screen();
	// The HUD's key toggles, whose F9 Emotes / F10 Radio menus take a digit
	// before the weapon rows do (controls::PlayerActions' menu arms); the HUD
	// presenter hands its process-lifetime instance over. Null = no menus, and
	// no HUD rows polled here.
	void set_hud_toggles(const Ref<HudToggles> &p_toggles) { hud_toggles_ = p_toggles; }
	// The HUD toggle rows' event words (HudToggles::poll) of the input frames
	// since the last read, in their order; reading clears them. The HUD
	// presenter applies them at its frame.
	PackedInt64Array take_hud_toggle_events();
	// The HUD's chat line and the gametext its prompts read: the talk rows
	// (the command prompt among them) are polled here in every input frame.
	// The HUD presenter hands its process-lifetime instance over. Null = no
	// talk rows polled.
	void set_hud_chat(const Ref<HudChatEntry> &p_chat, const Ref<RtxtStringFile> &p_gametext);

private:
	Ref<Simulation> sim() const;
	LocalPlayerPresenter *presenter() const;
	// The token's binding held RIGHT NOW (released with no model).
	bool pressed(const char *p_token) const;
	// An input frame's head at the logic tick it runs as: the scripted device
	// stepped to it (its look pixels taken) and the key scan's gates set.
	// Returns whether the death screen is up.
	bool begin_input_frame(const Ref<Simulation> &p_sim, int64_t p_logic_tick,
			Vector2 &r_scripted_look);
	// An update's device sample into `p_input` (movement, view keys, look, the
	// action rows and their requests), under the frame's gates.
	void sample_input_frame(const Ref<MissionFrameInput> &p_input, const Vector2 &p_scripted_look,
			bool p_spectating);
	// The input frame's HUD toggle row events (`p_rows_fired`, 1 << HudToggles::Row)
	// through the HUD's toggles, ahead of a later update's action rows, which
	// read the menus the rows open. True when the pause row flipped the pause word.
	bool poll_hud_toggles(const Ref<Simulation> &p_sim, int p_rows_fired,
			bool p_mouse_scale_negative);
	// The input frame's talk row events (`p_rows_fired`, 1 << HudChatEntry row)
	// through the HUD's chat line, with or without a local player.
	void poll_talk_rows(const Ref<Simulation> &p_sim, int p_rows_fired);
	// The rows' mouse, joystick and scripted arms held RIGHT NOW, one bit each
	// (ControlsModel::is_token_held_off_keyboard): a keyboard key fires its row
	// through the key queue's events.
	int hud_rows_down() const;
	int talk_rows_down() const;
	void read_input_state(bool &r_forward, bool &r_back, bool &r_left, bool &r_right,
			bool &r_lean_left, bool &r_lean_right, bool &r_jump) const;

	ObjectID world_id_;
	ObjectID presenter_id_;
	Ref<ControlsModel> controls_;
	Ref<PlayerMoveIntent> input_override_;
	Ref<HudToggles> hud_toggles_;
	std::vector<int64_t> hud_toggle_events_;
	Ref<HudChatEntry> hud_chat_;
	Ref<RtxtStringFile> chat_gametext_;
	Ref<KeyEventQueue> key_events_;
	opennova::controls::PlayerActions actions_;
	bool armory_requested_ = false;
	bool vehicle_menu_requested_ = false;
	// The wheel notch remainder. Retail clears it only at process start, so it
	// survives the reset-state path like the switch latches.
	opennova::controls::WheelRemainder wheel_;
	Vector2 look_delta_;
	// The axis input a pass-through screen's close owes the next sample.
	Vector2 close_axis_;
	int64_t frame_sequence_ = 0;
	// The gates the frame's head took, which its updates' samples keep: whether
	// the frame has a player to sample at all, and the shell's live/menu words.
	bool frame_player_ = false;
	bool frame_gameplay_input_active_ = false;
	bool frame_menu_open_ = false;
	// The talk and HUD rows' off-keyboard holds the samples since the last update's
	// poll saw, in order, each kept when it differs from the one before: the update
	// takes each one's press edges in turn, as the drain dispatches the queued events.
	struct RowSample {
		int talk = 0;
		int hud = 0;
	};
	std::vector<RowSample> row_samples_;
	void queue_row_sample();
	// The off-keyboard holds the last edge read, which a press edge leaves behind.
	int talk_held_ = 0;
	int hud_held_ = 0;
	// The keyboard pass's talk and HUD row events since the last update's input
	// frame, in order.
	struct RowEvent {
		int code = -1;
		bool sign = false;
	};
	std::vector<RowEvent> row_events_;
	// The keyboard pass's action row events (its scan's and its release pass's)
	// since the last device sample, in order.
	std::vector<opennova::controls::KeyRowEvent> action_events_;
	void queue_key_row_event(const opennova::controls::KeyRowEvent &p_event);
	// The last sample's action-row records, in order, until the session hands the role's
	// event queue to queue_records.
	std::vector<opennova::controls::InputEventRecord> sample_records_;
	// The device step of the frame's head or of a later update (run_update_rows): the
	// look pixels and the death screen the update's sample_tick reads.
	Vector2 tick_scripted_look_;
	bool tick_spectating_ = false;
	// The shell's keyboard pass (set_keyboard_pass).
	Callable keyboard_pass_;
	// This display frame sent the Scroll Lock turn-off (D-NET-563): the keyboard state reads
	// it only once the window's messages are read, so the frame's later samples read the word
	// off and send no second one.
	bool scroll_lock_turned_off_ = false;
	// The Escape key's reset owes the next frame's head a Scroll Lock turn-off.
	bool scroll_lock_turn_off_owed_ = false;
};

} // namespace godot
