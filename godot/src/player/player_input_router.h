#pragma once

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstdint>
#include <runtime/controls/player_actions.h>
#include <vector>

#include "hud/hud_chat_entry.h"
#include "hud/hud_toggles.h"
#include "mnu/controls_model.h"
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
// inmatch::TickInputSource carries the witness): before_world_tick's sample is
// the frame's first update's movement and action rows, each later update the
// frame banks samples the devices again at the logic tick it runs as, under the
// gates the frame's sample took, and every update the session runs, a paused one
// too, polls its talk and HUD rows (run_update_rows). A frame that runs no update
// polls no row; the row states it saw queue, in order, for the next update's poll,
// as retail's queue keeps each key event for the next update's drain.
class PlayerInputRouter final : public opennova::inmatch::TickInputSource {
public:
	// The world serves the sim; the presenter serves the presentation surfaces
	// this router drives around the sample (fly-camera lock, model lifetime,
	// the head-bone eye).
	void setup(Node *p_world, LocalPlayerPresenter *p_presenter, const Ref<ControlsModel> &p_controls);
	void teardown();

	// Scripted input (probes, automation, the presenter test): the same seven
	// movement bits a device produces, so the sim sees one contract. Null =
	// poll the live bindings again.
	void set_input_override(const Ref<PlayerMoveIntent> &p_intent);

	// `p_menu_open`: a menu screen (INGAME, WEAPON, CMAP) is up, under which
	// no input frame runs (controls::PlayerActionPoll::menu_open). The
	// returned frame names this router as its per-tick source.
	Ref<MissionFrameInput> before_world_tick(double p_delta, bool p_capture_mouse,
			bool p_gameplay_input_active, bool p_menu_open = false);
	// A later tick's movement and action rows of the frame before_world_tick
	// sampled, over the device state its run_update_rows stepped to.
	opennova::inmatch::InputPacket sample_tick(uint32_t p_logic_tick) override;
	// An update's talk and HUD rows: true when its pause row flipped the
	// single-player pause word. `p_new_sample` steps the device to the update's
	// logic tick first (a later update of a running frame).
	bool run_update_rows(uint32_t p_logic_tick, bool p_new_sample) override;
	bool handle_input(const Ref<InputEvent> &p_event, bool p_active);

	// Drop the trigger latches (the presenter's reset-state path). The
	// switch-key latches are deliberately NOT reset: a category key held across
	// a mission reload must not fire a spurious switch edge on the first new-sim
	// frame.
	void reset();
	void release_mouse_capture();
	// The shell consumed the live USE-ITEM hold for a chord of its own (the
	// debug pick rides Shift+F6; the tools window opening mid-hold): the
	// release edge then runs no mount toggle. Applied after this frame's
	// fresh-press reset so a same-frame chord sticks.
	void consume_use_hold();
	// The held USE row's armory arm took its open on the last frame's poll
	// (controls::PlayerAction::OpenArmory); reading clears it. The shell
	// opens weapon.mnu's WEAPON screen.
	bool take_armory_request();
	// The vehicle-loadout arm took its open on the last frame's poll
	// (controls::PlayerAction::OpenVehicleMenu); reading clears it. The shell
	// opens vehicle.mnu's VEHICLE screen.
	bool take_vehicle_menu_request();
	// A menu close's input reset: the key state (ControlsModel::reset_key_state),
	// the key-press latch (controls::PlayerActions::clear_key_press_latches)
	// and the deferred queue (Simulation::clear_deferred_input).
	void reset_input_state();
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
	// The input frame's device sample into `p_input` (movement, view keys,
	// look, the action rows and their requests), under the frame's gates.
	void sample_input_frame(const Ref<MissionFrameInput> &p_input, const Vector2 &p_scripted_look,
			bool p_spectating);
	// The input frame's HUD toggle rows (`p_rows_down`, 1 << HudToggles::Row)
	// through the HUD's toggles, ahead of a later update's action rows, which
	// read the menus the rows open. True when the pause row flipped the pause word.
	bool poll_hud_toggles(const Ref<Simulation> &p_sim, int p_rows_down);
	// The input frame's talk rows (`p_rows_down`, 1 << HudChatEntry row) through
	// the HUD's chat line, with or without a local player.
	void poll_talk_rows(const Ref<Simulation> &p_sim, int p_rows_down);
	// The rows' bindings held RIGHT NOW, one bit each.
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
	// The gates the frame's sample took, which its later ticks' samples keep:
	// whether the frame sampled a player at all, and the shell's live/menu words.
	bool frame_player_ = false;
	bool frame_gameplay_input_active_ = false;
	bool frame_menu_open_ = false;
	// The talk and HUD rows' states the samples since the last update's poll saw,
	// in order, each kept when it differs from the one before: the update polls
	// each in turn, as the drain dispatches the queued key events.
	struct RowSample {
		int talk = 0;
		int hud = 0;
	};
	std::vector<RowSample> row_samples_;
	void queue_row_sample();
	// A later update's device step (run_update_rows): the look pixels and the
	// death screen its sample_tick reads.
	Vector2 tick_scripted_look_;
	bool tick_spectating_ = false;
};

} // namespace godot
