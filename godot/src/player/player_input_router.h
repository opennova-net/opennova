#pragma once

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstdint>

#include "mnu/controls_model.h"
#include "player/player_move_intent.h"
#include "simulation/inmatch_session_values.h"

namespace godot {

class LocalPlayerPresenter;
class Simulation;

// The local player's input sampling/routing (the former player_input_router.gd,
// ADR 0043 slice G8), a plain member of LocalPlayerPresenter: the
// movement-state sampling into the sim, the weapon trigger/switch edge
// latches, the gameplay keys (B/N/NVG/stance), mouse look, and mouse
// capture/release. The presenter keeps the camera cluster and the
// avatar/viewmodel presentation; the camera MODE is the sim's resolved word
// (the arbiter over the chase preference and the seat), mirrored by the
// presenter -- no key here flips it. The presenter's before_world_tick /
// handle_key_input / handle_input stay the externally pinned names and
// delegate here. The live binding table arrives as the ControlsModel the
// shell's ControlsBindings singleton owns (persistence stays shell-side); a
// null model reads every token released.
class PlayerInputRouter {
public:
	PlayerInputRouter();

	// The world serves the sim; the presenter serves the presentation surfaces
	// this router drives around the sample (fly-camera lock, model lifetime,
	// the head-bone eye).
	void setup(Node *p_world, LocalPlayerPresenter *p_presenter, const Ref<ControlsModel> &p_controls);
	void teardown();

	// Scripted input (probes, automation, the presenter test): the same seven
	// movement bits a device produces, so the sim sees one contract. Null =
	// poll the live bindings again.
	void set_input_override(const Ref<PlayerMoveIntent> &p_intent);

	Ref<MissionFrameInput> before_world_tick(double p_delta, bool p_capture_mouse,
			bool p_gameplay_input_active);
	bool handle_key_input(const Ref<InputEvent> &p_event, bool p_active);
	bool handle_input(const Ref<InputEvent> &p_event, bool p_active);

	// Drop the trigger latches (the presenter's reset-state path). The
	// switch-key latches are deliberately NOT reset: a category key held across
	// a mission reload must not fire a spurious switch edge on the first new-sim
	// frame.
	void reset();
	void release_mouse_capture();

private:
	Ref<Simulation> sim() const;
	LocalPlayerPresenter *presenter() const;
	// The token's binding held RIGHT NOW (released with no model).
	bool pressed(const char *p_token) const;
	void sample_weapon_input(const Ref<MissionFrameInput> &p_frame_input, bool p_gameplay_input_active);
	void send_weapon_switch_input(bool p_captured);
	void sample_hud_input(bool p_active);
	void request_stance(int p_stance);
	void read_input_state(bool &r_forward, bool &r_back, bool &r_left, bool &r_right,
			bool &r_lean_left, bool &r_lean_right, bool &r_jump) const;

	ObjectID world_id_;
	ObjectID presenter_id_;
	Ref<ControlsModel> controls_;
	Ref<PlayerMoveIntent> input_override_;
	bool fire_was_held_ = false;
	bool reload_was_down_ = false;
	bool scope_was_down_ = false;
	bool medic_was_down_ = false;
	Vector2 look_delta_;
	int64_t frame_sequence_ = 0;
	// The manual weapon-switch keys -- the retail defaults from the shipped
	// binding catalog: rows 28-36 Knife '1' / Secondary '2' / Primary '3' /
	// Flashbang '4' / FragGrenade '5' / SmokeGrenade '6' / Accessory '7' /
	// Detonator '8' / medpack '9' fire the category actions 201-209
	// (categories 1..9), rows 39/40 cycleweaponP '[' / cycleweaponN ']' cycle
	// prev/next [orig: input cases 200-210 @ 0x4e1144 ->
	// Player_SwitchToWeaponByHandle((action-200)*65); cases 212/214 ->
	// Player_CycleWeaponSlot @ 0x4dfe70; engine/runtime/controls k_catalog rows].
	PackedStringArray weapon_category_tokens_;
	int category_was_down_ = 0;
	bool seat_was_down_[10] = {};
	bool cycle_prev_was_down_ = false;
	bool cycle_next_was_down_ = false;
	bool radar_out_was_down_ = false;
	bool radar_in_was_down_ = false;
	bool map_toggle_was_down_ = false;
};

} // namespace godot
