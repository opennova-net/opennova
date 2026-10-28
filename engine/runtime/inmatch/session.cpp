#include <runtime/inmatch/session.h>

#include <base/io/perf_clock.h>
#include <runtime/controls/controls.h> // action_code (the console's by-code dispatch)
#include <runtime/hud/hud_frame.h> // HudFrameCompiler::kRadarGate*
#include <runtime/hud/hud_server_status.h> // main_frame_draws_scene (the scene gate)
#include <runtime/hud/hud_toggles.h> // HudToggleState::server_status_view
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/main_loop_clock.h> // main_loop_tick_count_clock (the frame lock's clock)
#include <runtime/world/tick_count_clock.h> // frame_tick_count_ms (the frame lock clock)
#include <runtime/inmatch/mission_exit.h> // kMissionExitReset (the quit row's exit)
#include <runtime/inmatch/mission_rotation.h> // HostRotation::last_game (the LastGame row)
#include <runtime/inmatch/napi_np_server_ctx.h> // the server rows' context
#include <runtime/inmatch/server_admin_command.h> // the netdelay / resetgames / tod rows
#include <runtime/inmatch/server_ban_lists.h> // Server_BanPlayerSlot (the Ban row)
#include <runtime/inmatch/role_feeds.h> // step_hud_radar
#include <runtime/inmatch/spectator_session.h>
#include <runtime/mission/mission_kernel.h>

#include <array>
#include <cstdio>
#include <string>
#include <utility>

namespace opennova::inmatch {

// The session banks real time through the retail frame-time bank
// (world::TickAccumulator: the 500 ms clamp, the 7/8 EMA, 4 ms drain quanta
// with a logic tick on every fourth, the mission-start re-base below) and
// dispatches one role tick per logic tick, matching the original
// outer/logic-loop split [orig: Game_MainLoop @ 0x52b630
// -> Game_ProcessMainFrame @ 0x5263f0].

namespace {

SessionError invalid_transition(const char *message) {
	return {SessionErrorCode::InvalidTransition, message};
}

// An update's tail leg (Session::set_update_tail), run as the update ends whichever way its
// body leaves it, unless the update's tick ended the session.
struct UpdateTail {
	const std::function<void()> *leg;
	~UpdateTail() {
		if (leg != nullptr && *leg) (*leg)();
	}
	void cancel() { leg = nullptr; }
};

} // namespace

void Session::set_frame_lock_clock(world::FrameLockClock *clock) { frame_lock_clock_ = clock; }

int32_t Session::bank_frame(double elapsed, const FrameInput &input, uint32_t &lock_stamp_ms,
		int32_t &lock_sleeps) {
	lock_stamp_ms = 0;
	lock_sleeps = 0;
	if (!input.frame_lock || input.frame_stamp_ms == 0) return accumulator_.bank(elapsed);
	world::FrameLock lock;
	lock.clock = frame_lock_clock_ != nullptr ? frame_lock_clock_ : &main_loop_tick_count_clock();
	lock.stamp_ms = input.frame_stamp_ms;
	const int32_t due = accumulator_.bank(elapsed, &lock);
	if (lock.sleeps > 0) lock_stamp_ms = lock.stamp_ms;
	lock_sleeps = lock.sleeps;
	return due;
}

Session::Session(Role &role) : role_(&role), kind_(role.kind()) {}

void Role::apply_input(const TickInput &input) {
	mission::MissionKernel &kernel = *kernel_;
	ClientRuntime *runtime = client_runtime();
	const bool joiner = kind() == RoleKind::Joiner;
	// A joiner's input frame runs in its game loop alone: the join screen, the
	// mission start's load and its final wait run no main frame, so no press of
	// theirs reaches a sender (ClientRuntime::in_game_loop).
	// [orig: Game_ProcessMainFrame @0x5263F0 - Input_ProcessFrame @0x52661D;
	//  Game_StartMission @0x524360 - the final wait @0x526295..0x5262E5]
	if (joiner && (runtime == nullptr || !runtime->in_game_loop())) return;
	// The input pass's head drops the orbit-yaw bits before this tick's
	// dispatch can raise them again [orig: Input_ProcessFrame
	// `and g_InputActionBits, 0FFFFFFAFh` @0x49d52b].
	kernel.world.script.input_action_bits &= ~0x50u;
	// The spectate state the free-fly motor reads, ahead of the pack and the
	// entity update (a joiner restamps it after its receive).
	stamp_spectator_motor(kernel, runtime, joiner);
	// The update's records in queue order: the console's queued actions first, then the
	// device passes' (the key scan's rows, the Respawn key and the medic call among them,
	// in their key order), all ahead of the mouse-axis events, the toggles, the held rows
	// and the deferred dispatches, which the frame applies below. The session's updates
	// drained already (TickInput::drained).
	if (!input.drained) drain_input_events(input.source, input.player.held_action_bits);
	if (kernel.world.spectator.death_screen && runtime != nullptr) {
		apply_death_screen_input(kernel, *runtime, joiner, input.player);
		return;
	}
	const world::PlayerInput &movement = input.player.movement;
	// A reset in this frame's drain (Player_ResetLocalInput -> Input_ResetAllState, the
	// Respawn key's or a console-queued one's) memsets the event queue in the middle of the
	// drain, so the frame's later events never dispatch (the look, the weapon rows, the
	// ToSpecial and deferred dispatches), and it clears the key state, so a key held
	// through it reads up (its held rows queue nothing) until its next key-down; the
	// devices' own reset is the embedder's (take_input_reset)
	// (D-NET-560). [orig: Player_ResetLocalInput @0x4B2B80 - Input_ResetAllState
	//  @0x4B2B99; Input_ResetAllState @0x497ED0 - the queue's memset @0x497EE6,
	//  Input_ClearKeyState; Input_ProcessFrame @0x49D520 - the drain's count test
	//  @0x49D5E0, the loop @0x49D5E7; Input_ProcessPlayerFrame @0x49D4C0 - the key rows
	//  @0x49D4C6, the mouse axis @0x49D4D8, the toggles @0x49D4DE; Input_FlushDeferredEvents
	//  @0x49D591; Input_ProcessAnalogBindings @0x4995F0]
	if (input_queue_reset_) {
		kernel.local.set_movement_keys(false, false, false, false, false, false, false);
		kernel.local.set_view_keys(false, false, false, false, false);
		kernel.local.set_weapon_input(false, false, false);
		return;
	}
	// The held rows (the analog list: the movement, lean, jump, look and FreeLook rows,
	// flags 0x0C000C05 / 0x2C000C05) queue their event every frame a key is down, and
	// each passes the dispatcher's head by its row's flags, so a dead local player, the
	// pre-round timer, the deploy overlay and the round-over gate drop the frame's held
	// keys (V-L2-s8 defect 2's 0x400 rows).
	// [orig: Input_ProcessAnalogBindings @0x4995F0 - Input_QueueEvent(row code)
	//  @0x499660; Input_HandleActionBinding @0x49AD40 - the head @0x49AD8D..0x49AE23]
	const auto held = [this](bool down, int row) {
		return down && action_head_admits(controls::action_code(row));
	};
	kernel.local.set_movement_keys(held(movement.forward, 2), held(movement.back, 3),
			held(movement.left, 4), held(movement.right, 5), held(movement.lean_left, 6),
			held(movement.lean_right, 7), held(movement.jump, 8));
	kernel.local.set_view_keys(held(movement.free_look, 97), held(movement.look_up, 13),
			held(movement.look_down, 12), held(movement.turn_left, 14),
			held(movement.turn_right, 15));
	// The mouse look rows (flags 0x0C100425 / 0x0C200425) queue their axis as action
	// events, which pass the dispatcher's head: a dead local player, the pre-round
	// timer, the deploy overlay and the round-over gate drop the frame's look
	// (V-LI-s1 FOUND 5). [orig: Input_TryTriggerMouseAxisBinding @0x499130 - the row's
	//  0x100000 / 0x200000 tests @0x49913D / @0x499164, Input_QueueEvent @0x499156;
	//  Input_HandleActionBinding @0x49AD40 - the head @0x49AD8A..0x49AE16]
	// Each axis is its own row's event (row 0 the X, row 1 the Y).
	constexpr int kLookXRow = 0;
	constexpr int kLookYRow = 1;
	const float look_x = input.player.look_delta_x != 0.0f &&
			action_head_admits(controls::action_code(kLookXRow)) ? input.player.look_delta_x : 0.0f;
	const float look_y = input.player.look_delta_y != 0.0f &&
			action_head_admits(controls::action_code(kLookYRow)) ? input.player.look_delta_y : 0.0f;
	if (look_x != 0.0f || look_y != 0.0f) kernel.local.look(look_x, look_y);
	// The trigger; the reload rides its record's arm (dispatch_record), PRESSED_RELOAD a
	// sourceless frame's latch (PressedAction).
	const bool fire_held = (input.player.held_action_bits & HELD_FIRE) != 0;
	const bool fire_pressed = (input.player.pressed_action_bits & PRESSED_FIRE) != 0;
	kernel.local.set_weapon_input(fire_held, fire_pressed,
			(input.player.pressed_action_bits & PRESSED_RELOAD) != 0);
	// A sourceless frame (the embedder's frame value: no device, so no records) carries the
	// fire row as its bits alone. A PowerThrow def's windup then runs as a key's would: the
	// press bit is the press leg's start, the hold's end the release leg's throw
	// (LocalPlayer::fire_record); a device's windup is its records' alone. The def is the
	// EquippedSlot's, which the commit that stored the slot mounted (LocalPlayerWeapon::def)
	// [orig: Input_HandleActionBinding_0 @0x4E08E2 / @0x4E08F2 / @0x4E08FD].
	if (input.source == nullptr &&
			(kernel.local.weapon.def.flags & world::weapon_flag::kPowerThrow) != 0) {
		if (fire_pressed)
			(void)kernel.local.fire_record(true, true);
		else if (!fire_held)
			(void)kernel.local.fire_record(false, true);
	}
}

void Role::observe_frame_rate(int32_t fps) {
	if (ClientRuntime *runtime = client_runtime()) runtime->set_observed_frame_rate(fps);
}

bool Role::request_medic() {
	mission::MissionKernel &kernel = *kernel_;
	// The session/entity gates: a replica runtime and a local entity; the
	// dead-bit and cooldown gates are the local player's.
	if (client_runtime() == nullptr || !kernel.world.cached.local_player.valid()) return false;
	const bool local_dead = kind() == RoleKind::Joiner
			? client_runtime()->local_player_dead() : kernel.local.local_player_dead();
	if (!kernel.local.medic_request_allowed(local_dead)) return false;
	if (!send_medic_request()) return false;
	kernel.local.stamp_medic_request();
	return true;
}

bool Role::action_head_admits(int32_t code) {
	mission::MissionKernel &kernel = *kernel_;
	ClientRuntime *runtime = client_runtime();
	const bool joiner = kind() == RoleKind::Joiner;
	// The head reads the local entity, which a joiner has from its mission start's
	// Player_InitPlayer whether or not L has spawned (D-NET-562) [orig:
	// Game_StartMission @0x524360 - Player_InitPlayer @0x525BBC]. A dedicated host
	// in a session takes its operand as the constant 2 without reading it, so its
	// rows pass the head with no player of ours (a Serve Only host's lockgame, help
	// and the status page's score list) [orig: Input_HandleActionBinding @0x49AD40 -
	// the non-peer authority's 2 @0x49AD52..0x49AD74; the entity's Flags & 2 only on
	// the other arm @0x49AD76..0x49AD7E].
	const world::World &w = kernel.world;
	const bool dedicated_operand =
			w.rules.mp_session && w.rules.logic_authority && !w.rules.mp_session_peer;
	if (joiner ? runtime == nullptr || !runtime->local_entity_bound()
	           : !dedicated_operand && !w.cached.local_player.valid())
		return false;
	// The flags word of the binding record the code names (the catalog row whose code it
	// is); a code no row carries reads the zero record (D-NET-710, V-L2-s8).
	// [orig: Input_HandleActionBinding @0x49AD40 - dword_8159AC[code * 0x6C] @0x49AD8D;
	//  KeyBinding_SortBySequentialId @0x498260]
	const controls::ActionDef *row = controls::action_for_code(code);
	const uint32_t flags = row != nullptr ? row->flags : 0u;
	// a6: on the authority the constant 2 on a dedicated host in a session, else the dead
	// bit (LocalPlayer::action_dispatch_drops, D-NET-433); a joiner's replica dead bit.
	// [orig: @0x49AD52..0x49AD6F (the constant), @0x49AD76..0x49AD7E (Flags & 2)]
	const bool local_dead = joiner ? runtime->local_player_dead()
			: kernel.local.action_dispatch_drops();
	const bool round_over = joiner ? runtime->state().spawn_success_gate
			: kernel.world.match.outcome().ended;
	// 0x400's operand is the local client's pre-round timer, whose one writer is the
	// client's 0x0A handler: a joiner's replica copy, the listen host's own client's copy
	// (World::CachedFrameState::client_preround_timer), which a dedicated host never
	// writes (V-L2-s8).
	// [orig: `cmp g_ClientPreRoundTimer, 0` @0x49ADBB; its one store
	//  NapiNPClientMsg_0x00A @0x430064]
	const uint32_t preround = joiner ? kernel.world.preround_delay_seconds
			: kernel.world.cached.client_preround_timer;
	// The gates in the head's order; bit 0's third gate is the single-player pause word,
	// which a paused update's sample meets (its action rows poll while the pause row's
	// pause holds; the out-of-session round-over leg is the same drop).
	// [orig: Input_HandleActionBinding @0x49AD40 - bit 0 with a6 @0x49AD95..0x49ADA0;
	//  0x40 with !a6 @0x49ADA6..0x49ADAD; 0x400 @0x49ADB3..0x49ADC2; 0x8000000 with
	//  g_DeployScreenActive @0x49ADC8..0x49ADD7; the pause @0x49ADDD..0x49ADE8;
	//  g_RoundOver @0x49ADEE..0x49AE16; 0x10 in a session @0x49AE1C..0x49AE23]
	if ((flags & 0x1u) != 0 && local_dead) return false;
	if ((flags & 0x40u) != 0 && !local_dead) return false;
	if ((flags & 0x400u) != 0 && preround != 0) return false;
	if ((flags & 0x8000000u) != 0 && runtime != nullptr &&
			runtime->state().deploy_overlay_active)
		return false;
	if ((flags & 0x1u) != 0 && w.rules.pause_word) return false;
	if ((flags & 0x1u) != 0 && round_over) return false;
	return !((flags & 0x10u) != 0 && kernel.world.rules.mp_session);
}

bool Role::request_respawn() {
	mission::MissionKernel &kernel = *kernel_;
	// The row's head, its flags 0x0C000C01. None of the head's filters takes code 55
	// (the death screen's, the stub, the cine's, the troop class's default arm).
	// [orig: Input_HandleActionBinding @0x49AD40 - DeathScreen_FilterInputAction
	//  @0x49AE4C, Input_NullActionFilter @0x49AE68, Cine_FilterInputAction @0x49AE84, the
	//  def+0x170 callback @0x49AEB6 (Input_HandleActionBinding_0's default @0x4E13F9)]
	constexpr int kRespawnRow = 68;
	if (!action_head_admits(controls::action_code(kRespawnRow))) return false;
	// The input reset, then in a session on a peer the bodiless 0x41
	// [orig: case 55 -- Player_ResetLocalInput @0x49B20B, is_in_session @0x49B210,
	//  is_mp_session_peer @0x49B21D, NetPacket_SendRequestRespawn @0x49B22A].
	(void)reset_local_input(); // the device's turn-off rides the row's code
	return kernel.world.rules.mp_session && send_respawn_request();
}

bool Role::request_stance(int stance) {
	if (stance < 0 || stance > 2) return false;
	// The stance rows 9..11 carry flags 0x0C000C01: the head drops a select for a dead
	// local player, in the pre-round, under the deploy overlay and at round over, a
	// key's and a console-queued one's alike; past it the def callback's arm
	// (send_stance_request) (D-NET-710, V-L2-s7).
	// [orig: Input_HandleActionBinding @0x49AD40 - the head @0x49AD8A..0x49AE16, the
	//  def+0x170 callback @0x49AEB6 -> Input_HandleActionBinding_0 @0x4E0420 - cases 169
	//  @0x4E0D77, 170 @0x4E0DF3, 172 @0x4E0E3E; the catalog rows 9 / 10 / 11
	//  @0x815D74 / @0x815DE0 / @0x815E4C]
	constexpr int kStanceRows[3] = {11, 10, 9}; // stand 172, crouch 169, prone 170
	if (!action_head_admits(controls::action_code(kStanceRows[stance]))) return false;
	return send_stance_request(stance);
}

void Role::flush_stance_requests() {
	// [orig: Player_MountWeaponSlot @0x4DFA40 - CNapiNetwork_QueueReliableMessage(0x1D, 1, 0)
	//  @0x4DFC30 at the request, the stance key's own message (Input_HandleActionBinding_0
	//  @0x4E0420 - @0x4E0DCA / @0x4E0DE7)]
	if (kernel_ == nullptr) return;
	std::vector<uint16_t> &requests = kernel_->world.out.stance_requests;
	for (const uint16_t action_id : requests) (void)queue_stance_action(action_id);
	requests.clear();
}

void Role::store_mission_exit(int32_t reason) {
	if (kernel_ != nullptr) kernel_->world.mission_exit_reason = reason;
}

bool Role::input_frame_runs() {
	const ClientRuntime *runtime = client_runtime();
	return kind() != RoleKind::Joiner || (runtime != nullptr && runtime->in_game_loop());
}

void Role::drain_input_events(TickInputSource *source, uint32_t held_action_bits,
		bool pause_flips) {
	// [orig: Input_ProcessFrame @0x49D520 - the count test @0x49D599, Input_DequeueEvent
	//  @0x49D5BA, Input_HandleActionBinding @0x49D5D8, the count test @0x49D5E0 / the loop
	//  @0x49D5E7; Game_ProcessMainFrame @0x5263F0 - UI_IsMenuScreenOpen @0x52660D skips
	//  Input_ProcessFrame @0x52661D; Game_StartMission @0x524360 - the final wait
	//  @0x526295..0x5262E5 runs no main frame]
	input_queue_reset_ = false;
	if (!input_frame_runs()) return;
	if ((held_action_bits & HELD_MENU_OPEN) != 0) return;
	// The frame's head zeroes the input axis words ahead of the drain, whose movement records
	// and the held rows behind them write them again (D-NET-1319) [orig: Input_ProcessFrame
	//  @0x49D520 - @0x49D54E..0x49D578].
	kernel_->local.zero_axis_words();
	pause_flips_ = pause_flips;
	to_special_held_ = (held_action_bits & HELD_TO_SPECIAL) != 0;
	fire_held_ = (held_action_bits & HELD_FIRE) != 0;
	fire_keys_held_ = (held_action_bits & HELD_FIRE_KEYS) != 0;
	// Input_FlushDeferredEvents: each deferred dispatch (ToSpecial's, kept by a reload or a
	// draw in progress; the fire row's, an automatic weapon's re-queue) is queued again in
	// order behind the update's records, its first two arguments its own and its third the
	// deferred marker 0xFFFFEEEE, under the queue's cap, the deferred queue emptied; it
	// passes the head again at its dispatch, which drops it for good [orig:
	// Input_ProcessFrame @0x49D520 - Input_ProcessPlayerFrame @0x49D58B, then
	// Input_FlushDeferredEvents @0x49D591; Input_FlushDeferredEvents @0x497AC0 - the count
	// test @0x497AC0, `push 0FFFFEEEEh` @0x497AE1, Input_QueueEvent @0x497AE9, the shift
	// @0x497B04..0x497B21].
	constexpr int32_t kDeferredMarker = static_cast<int32_t>(0xFFFFEEEEu);
	std::vector<world::LocalPlayer::DeferredInputEvent> deferred;
	deferred.swap(kernel_->local.deferred_events);
	for (const world::LocalPlayer::DeferredInputEvent &event : deferred)
		(void)input_events_.push({event.code, event.p1, event.p2, kDeferredMarker, false});
	controls::InputEventRecord record;
	while (input_events_.pop(record)) {
		dispatch_record(record, source);
		// A record's mount request queues its 0x1D at once, ahead of a later record's
		// sends (mount_queue_switch_out).
		flush_stance_requests();
	}
	pause_flips_ = false;
	// The frame's tail behind the drain: the USE row's release edge (TickInputSource::
	// end_input_frame) [orig: Input_ProcessFrame @0x49D520 - @0x49D6C1..0x49D6DC].
	if (source != nullptr) source->end_input_frame();
}

void Role::dispatch_record(const controls::InputEventRecord &record, TickInputSource *source) {
	const int32_t code = record.code;
	// Input_HandleActionBinding's switch by the record's code: the Respawn row
	// (catalog row 68, code 55) and the medic call (row 64 MedicReq, code 217) are
	// the role's arms, behind their own head gates; every other code is the
	// embedder's (its screens' and devices' arms), whatever queued the record: a key's,
	// a console line's local CRC arm or its echo's walk (by any row's token, so any
	// catalog code) all reach the one dispatcher (D-NET-1302).
	// [orig: Input_HandleActionBinding @0x49AD40 - case 55 @0x49B20B, case 217
	//  @0x49B4B4; the catalog codes word_8159A8 + 108 * row; Input_ProcessFrame @0x49D520 -
	//  the drain's one Input_HandleActionBinding call @0x49D5D8; NapiClientCmd_ParseAndExecute
	//  @0x4329C0 - the token walk @0x432A64..0x432A94 over all 768 records, the CRC walk
	//  @0x432AA9..0x432B3E, Input_QueueEvent(record, ..) @0x432B4C / @0x432BA1;
	//  Client_ProcessLocalCommand @0x520BF0 - Input_QueueEvent @0x520E5B]
	constexpr int kRespawnRow = 68;
	constexpr int kMedicRequestRow = 64;
	// The "quit" row 78 (code 4, flags 0x04000000: no head gate) stores exit reason 2 and
	// nothing else; the main frame's tail takes it to the Post Menu, whose reason-2 arm resets
	// the session and ends the game (PostMissionRoute::exits_game).
	// [orig: Input_HandleActionBinding @0x49AD40 - case 4 @0x49AF4C; the catalog row 78
	//  @0x817A90; Game_ProcessMainFrame @0x5263F0 - @0x526860..0x526867]
	constexpr int kQuitRow = 78;
	if (code == controls::action_code(kQuitRow)) {
		store_mission_exit(kMissionExitReset);
		return;
	}
	if (code == controls::action_code(kRespawnRow)) {
		(void)request_respawn();
		return;
	}
	if (code == controls::action_code(kMedicRequestRow)) {
		(void)request_medic();
		return;
	}
	// The stance selects, catalog rows 9 (170 prone), 10 (169 crouch) and 11 (172 stand)
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - cases 169 @0x4E0D77, 170 @0x4E0DF3,
	//  172 @0x4E0E3E].
	constexpr int kProneRow = 9;
	constexpr int kCrouchRow = 10;
	constexpr int kStandRow = 11;
	if (code == controls::action_code(kCrouchRow)) {
		(void)request_stance(1);
		return;
	}
	if (code == controls::action_code(kProneRow)) {
		(void)request_stance(2);
		return;
	}
	if (code == controls::action_code(kStandRow)) {
		(void)request_stance(0);
		return;
	}
	// The reload row (catalog row 47 magazine, code 211 = 0xD3, flags 0x0C000801) past the
	// head: the case's request on the slot equipped at this dispatch, under its binocular,
	// full-magazine and pool gates (world::local_weapon_request_reload), so it queues RELOAD
	// ahead of the switch or fire records behind it in the drain. A second record in the same
	// input frame finds the reload's 4 stored and queues nothing.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 0xD3 @0x4E1278..0x4E12F3 ->
	//  WeaponSlot_RequestReload @0x53F110 (the next action's test @0x53F11E..0x53F12D, the
	//  store @0x53F12F)]
	constexpr int kReloadRow = 47;
	if (code == controls::action_code(kReloadRow)) {
		if (action_head_admits(code)) (void)kernel_->local.request_reload();
		return;
	}
	// ToSpecial (catalog row 37, code 220 = 0xDC, flags 0x8C000801) past the head (a dead
	// local player, the pause, the deploy hold and the round over drop it, a deferred one
	// for good): one dispatch with the keys' live state, which a reload or a draw in progress
	// defers to the next input frame's flush (LocalPlayer::deferred_events). (The press
	// scan's own dead gate, @0x49D330..0x49D339, takes the same press first.)
	// [orig: Input_HandleActionBinding @0x49AD40 - the head @0x49AD8A..0x49AE16 (bit 0
	//  with g_RoundOver @0x49ADFD in a session, @0x49AE16 out of one);
	//  Input_HandleActionBinding_0 @0x4E0420 - case 0xDC @0x4E1161..0x4E1181;
	//  Input_QueueDeferredEvent @0x4993E0 (the 32 cap @0x4993E8)]
	constexpr int kToSpecialRow = 37;
	if (code == controls::action_code(kToSpecialRow)) {
		if (action_head_admits(code)) kernel_->local.dispatch_to_special(to_special_held_);
		return;
	}
	// The fire row (catalog row 43 attack_1, code 149 = 0x95, flags 0x8C400C01) past the head:
	// the case drops the record whole under the binocular view or a cease fire, then tests the
	// binding live (the update's sample, HELD_FIRE). Held, the press leg latches the weapon
	// pump's fire request; up, the release leg, which throws a running PowerThrow windup
	// (LocalPlayer::fire_record). So a click whose button came up before its record's update
	// fires nothing (D-NET-1000), and a windup throws only at a record that reads the
	// binding up: none under a cease fire or an open text line (whose release pass queues no
	// event). A PowerThrow def's press leg requests no fire: it starts the windup only when the
	// binding's device is the keyboard (HELD_FIRE_KEYS), a mouse press's windup being its
	// left-down's (world::local_weapon_left_button_down), a joystick's none. A deferred record
	// (an automatic weapon's re-queue, LocalPlayer::deferred_events) dispatches as a fresh one,
	// so the refire meets every gate a press does.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 0x95: g_BinocularsViewActive
	//  @0x4E07B5, g_InCeaseFire @0x4E07C2, Input_IsBindingActive(0x95) @0x4E07D9, the release
	//  leg @0x4E07E9, the press leg @0x4E08DC (the PowerThrow sign bit @0x4E08FD, the device
	//  test @0x4E090B)]
	// An ordinary def's press leg runs its own gates there (world::local_weapon_press_fire:
	// the WAC latch and BlockFire, OnlyFireScoped, OnlyFireLocked's dry click, the seats).
	constexpr int kFireRow = 43;
	if (code == controls::action_code(kFireRow)) {
		if (!action_head_admits(code) || kernel_->world.rules.cease_fire) return;
		(void)kernel_->local.fire_record(fire_held_, fire_keys_held_);
		return;
	}
	// The pause row (catalog row 70, code 25; the declaration carries the witness): past
	// the head, out of a session, the flip and the audio stop; the debug transport's step
	// flips no word of the session's (pause_flips_). The embedder then takes the record (its
	// HUD's copy of the word, the key-queue reset), a device's and a console's alike.
	constexpr int kPauseRow = 70;
	if (code == controls::action_code(kPauseRow)) {
		if (pause_flips_ && kind() == RoleKind::SinglePlayer && action_head_admits(code)) {
			world::World &world = kernel_->world;
			world.rules.pause_word = !world.rules.pause_word;
			if (world.rules.pause_word) world.out.audio_channels_shutdown = true;
		}
		if (source != nullptr) source->dispatch_record(record);
		return;
	}
	// The cheat row 116 (code 123, flags 0x10; the console's out-of-session command CRC
	// queues it) past the head, whose 0x10 test drops it in a session: the troop class
	// callback's case flips the local cheat word's 0x800 bit, the AI target walks' and the
	// weapon validator's skip-the-local-player arm (World::rules.ai_rules_skip_local_player),
	// and takes the record. So no session ever sets the bit, and each mission start's fresh
	// world clears it as Game_StartMission's store does (World::rules carries that cite).
	// [orig: Input_HandleActionBinding @0x49AD40 - the 0x10 test @0x49AE1C..0x49AE23, the
	//  def+0x170 callback @0x49AEB6; Input_HandleActionBinding_0 @0x4E0420 - case 123
	//  `xor g_DebugCheatFlags, 800h` @0x4E07A4]
	constexpr int kAiIgnoresPlayerCheatRow = 116;
	if (code == controls::action_code(kAiIgnoresPlayerCheatRow)) {
		if (action_head_admits(code)) {
			world::World &world = kernel_->world;
			world.rules.ai_rules_skip_local_player = !world.rules.ai_rules_skip_local_player;
		}
		return;
	}
	// The sibling cheat rows past the same head (flags 0x10, out of a session only), each
	// case of the troop class callback over the local player (D-NET-1290):
	// - row 113's 120 refreshes its weapon tables (LocalPlayer::refresh_weapon_overlay);
	// - row 114's 121 flips the cheat word's 0x100 bit, the infinite ammo
	//   (World::rules.ignore_weapon_ammo_cost), refreshes the tables, and while the bit is up
	//   posts Misc STRMISC_INFAMMO to the SYSTEM ring in white;
	// - row 115's 122 stores 0x4000 in its health word.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - g_LocalPlayerEntity @0x4E0434; case 120
	//  Entity_UpdateWeaponOverlayFrameState called @0x4E0737; case 121 Cheat_ToggleInfiniteAmmo
	//  called @0x4E0746 (`xor g_DebugCheatFlags, 100h` @0x4DC9C5,
	//  Entity_UpdateWeaponOverlayFrameState called @0x4DC9D0), `test g_DebugCheatFlags, 100h`
	//  @0x4E074B, GameText_GetString("Misc", "STRMISC_INFAMMO") @0x4E076C,
	//  Chat_AddMessageChannel2(text, -1, 930) @0x4E0775; case 122 `mov word ptr [eax+11Eh],
	//  4000h` @0x4E0784]
	constexpr int kWeaponRefreshCheatRow = 113;
	constexpr int kInfiniteAmmoCheatRow = 114;
	constexpr int kHealthCheatRow = 115;
	if (code == controls::action_code(kWeaponRefreshCheatRow)) {
		if (action_head_admits(code)) kernel_->local.refresh_weapon_overlay();
		return;
	}
	if (code == controls::action_code(kInfiniteAmmoCheatRow)) {
		if (action_head_admits(code)) {
			world::World &world = kernel_->world;
			world.rules.ignore_weapon_ammo_cost = !world.rules.ignore_weapon_ammo_cost;
			kernel_->local.refresh_weapon_overlay();
			if (world.rules.ignore_weapon_ammo_cost)
				world.out.hud_chat_lines.push_back({"Misc", "STRMISC_INFAMMO", 0xFFFFFFFFu, true});
		}
		return;
	}
	if (code == controls::action_code(kHealthCheatRow)) {
		if (action_head_admits(code)) {
			world::World &world = kernel_->world;
			// The one health word, which the port keeps on the registry row and the AI
			// motor's mirror of entity+286 (the scripted Tele's store writes both).
			if (world::Entity *player = world.registry.get(world.cached.local_player)) {
				player->health = 0x4000;
				if (world::AiEntity *body = world.ai.for_handle(player->handle))
					body->health = 0x4000;
			}
		}
		return;
	}
	// The Debug row 118's 74 (flags 0: no head gate, so a session's echo walk reaches it too)
	// flips the cheat word's 0x400 bit, the frame-rate overlay (World::rules.frame_rate_overlay).
	// [orig: Input_HandleActionBinding @0x49AD40 - case 74 `xor g_DebugCheatFlags, 400h`
	//  @0x49B937]
	constexpr int kFrameRateRow = 118;
	if (code == controls::action_code(kFrameRateRow)) {
		if (action_head_admits(code)) {
			world::World &world = kernel_->world;
			world.rules.frame_rate_overlay = !world.rules.frame_rate_overlay;
		}
		return;
	}
	// The Last_move row 16's 425 past the head (flags 0x0C000405): the troop class callback
	// queues the forward row's record, its fourth argument 1 (no Scroll Lock turn-off), which
	// joins this drain (D-NET-1319).
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - `cmp 425` @0x4E046F,
	//  Input_QueueEvent(152, 0, 0, 1) called @0x4E13ED]
	constexpr int kLastMoveRow = 16;
	if (code == controls::action_code(kLastMoveRow)) {
		if (action_head_admits(code)) (void)input_events_.push({152, 0, 0, 1, false});
		return;
	}
	// The movement and look rows as records (a typed row's, a queued Last_move's), each its
	// troop class callback case past the head (world::LocalPlayer::apply_movement_case): the
	// bit form ORs its input-flag bit, which the fold carries beside the held keys' (the look
	// rows' AbsorbPitch refusal rides the fold), forward and back writing the X axis word with
	// it; a nonzero first argument writes the row's axis word instead (D-NET-1319). Their
	// g_DebugCheatFlags 0x20 gate is dead (controls::PlayerActionFrame::scroll_lock_off carries
	// that witness). The record then goes on to the embedder, whose device owes the Scroll Lock
	// turn-off the back, forward (fourth argument 0) and strafe cases run.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - 151: Input_DisableScrollLock called
	//  @0x4E0C66; 152: the fourth argument @0x4E0C92, Input_DisableScrollLock called @0x4E0C9E;
	//  154: Entity_CheckWeaponSeatFlags @0x4E0EA5; 155: @0x4E0F73; 156: Input_DisableScrollLock
	//  called @0x4E1021; 157: Input_DisableScrollLock called @0x4E105E]
	if (world::LocalPlayer::movement_case_bit(code) != 0) {
		if (!action_head_admits(code)) return;
		kernel_->local.apply_movement_case(code, record.p1);
		if (source != nullptr) source->dispatch_record(record);
		return;
	}
	// The scopescale row 74's 49, the troop class callback's case past the head (flags
	// 0x04000000): the session's scope-scale word steps by the record's argument and sign
	// (world::player_look_scope_scale), then the Misc toast STRMISC_SCOPESCALE with the word as
	// its $A goes to the SYSTEM ring in white (D-NET-1318).
	// [orig: Input_HandleActionBinding @0x49AD40 - the def+0x170 callback @0x49AEB6;
	//  Input_HandleActionBinding_0 @0x4E0420 - case 49 @0x4E056D, sprintf("%i") called @0x4E05BE,
	//  GameText_GetString("Misc", "STRMISC_SCOPESCALE") called @0x4E05E0, Chat_FormatPlayerTokens
	//  called @0x4E05E9, Chat_AddMessageChannel2(.., -1, 930) called @0x4E05F2]
	constexpr int kScopeScaleRow = 74;
	if (code == controls::action_code(kScopeScaleRow)) {
		if (action_head_admits(code)) {
			const int32_t scale = world::player_look_scope_scale(kernel_->local.look_settings,
					record.p1, record.sign);
			world::HudChatLine line{"Misc", "STRMISC_SCOPESCALE", 0xFFFFFFFFu, true};
			line.text = std::to_string(scale);
			kernel_->world.out.hud_chat_lines.push_back(std::move(line));
		}
		return;
	}
	if (dispatch_server_record(record)) return;
	// The cheat row 117's 40 opens the single player's weapon screen: out of a session, with a
	// local player, the embedder's toggle (a menu screen up closes, else weapon.mnu's WEAPON
	// opens), so the record goes on to the embedder past those gates.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 40 UI_OpenWeaponScreenSinglePlayer
	//  called @0x49B7E7; UI_OpenWeaponScreenSinglePlayer @0x424390 - `cmp is_in_session, 0`
	//  @0x424390, the tail @0x4DDCE0 (g_LocalPlayerEntity), UI_IsMenuScreenOpen @0x4DDCE9,
	//  UI_CloseMenuScreen @0x4DDCF2, UI_OpenMenuScreen("weapon.mnu", "WEAPON") @0x4DDD03,
	//  g_WeaponScreenOpen = 1 @0x4DDD0B]
	constexpr int kWeaponScreenCheatRow = 117;
	if (code == controls::action_code(kWeaponScreenCheatRow) &&
			(!action_head_admits(code) || kernel_->world.rules.mp_session ||
					!kernel_->world.cached.local_player.valid()))
		return;
	// The embedder's one dispatch (TickInputSource::dispatch_record), a console record's and a
	// key's alike; an update with no device (a dedicated host, a probe's direct frame) has none.
	if (source != nullptr) source->dispatch_record(record);
}

bool Role::dispatch_server_record(const controls::InputEventRecord &record) {
	constexpr int kNextFlagRow = 52;   // code 32, flags 0x0C000801
	constexpr int kPuntCrcRow = 85;    // code 47, flags 0x04000000
	constexpr int kSaveScoresRow = 88; // code 38, flags 0x04000000
	constexpr int kNetDelayRow = 89;   // code 103, flags 0x04000000
	constexpr int kResetGamesRow = 92; // code 106, flags 0x04000000
	constexpr int kBanRow = 93;        // code 107, flags 0x04000000
	constexpr int kLastGameRow = 94;   // code 108, flags 0x04000000
	constexpr int kTodRow = 95;        // code 438, flags 0
	constexpr int kTodRateRow = 96;    // code 497, flags 0
	const int32_t code = record.code;
	const auto is = [code](int row) { return code == controls::action_code(row); };
	if (!is(kNextFlagRow) && !is(kPuntCrcRow) && !is(kSaveScoresRow) && !is(kNetDelayRow) &&
			!is(kResetGamesRow) && !is(kBanRow) && !is(kLastGameRow) && !is(kTodRow) &&
			!is(kTodRateRow))
		return false;
	if (!action_head_admits(code)) return true;
	world::World &world = kernel_->world;
	const bool in_session = world.rules.mp_session;
	const bool authority = world.rules.logic_authority;
	const bool peer = world.rules.mp_session_peer;
	NapiNPServerCtx *ctx = server_ctx();
	// nextflag: the record's sign (Shift) steps backward, else forward, over the selection
	// list by the local player's team (WaypointTrack::cycle_by_team carries the walk); a
	// joiner's game type is the session's, which its replica holds.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 32 `test ebp, ebp` @0x49B6A7,
	//  Spectator_CycleTargetByTeam(1) called @0x49B6AD, (-1) @0x49B6BC]
	if (is(kNextFlagRow)) {
		const world::Entity *local = kernel_->local.player();
		if (local == nullptr) return true;
		world::WaypointCycleContext cycle = world.waypoint_context();
		const ClientRuntime *runtime = client_runtime();
		if (kind() == RoleKind::Joiner && runtime != nullptr) cycle.game_type = runtime->game_type();
		world.script.waypoints.cycle_by_team(record.sign, cycle, local->team);
		return true;
	}
	// savescores: the process word flips (ClientProcessState::save_scores) and the SYSTEM
	// ring takes the literal line in white, no gate.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 38 `xor g_SaveScores, 1` @0x49B7B3,
	//  Chat_AddMessageChannel2("Save Scores: On", -1, 930) @0x49B7C8, ("Save Scores: Off")
	//  @0x49B7DA]
	if (is(kSaveScoresRow)) {
		ClientProcessState *process = process_state();
		if (process == nullptr) return true;
		process->save_scores = !process->save_scores;
		world::HudChatLine line;
		line.argb = 0xFFFFFFFFu;
		line.system_ring = true;
		line.text = process->save_scores ? "Save Scores: On" : "Save Scores: Off";
		world.out.hud_chat_lines.push_back(std::move(line));
		return true;
	}
	// PuntCRC: the process's eight counters (ClientProcessState::punt_crc_counters) in one
	// literal SYSTEM line in white, no gate.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 47, Debug_PrintScoreCounters called
	//  @0x49B829; Debug_PrintScoreCounters @0x4FD6A0 - "1:%i 2:%i 3:%i 4:%i 5:%i 6:%i 7:%i 8:%i"
	//  @0x4FD6F3, Chat_AddMessageChannel2(.., -1, 930) called @0x4FD704]
	if (is(kPuntCrcRow)) {
		const ClientProcessState *process = process_state();
		if (process == nullptr) return true;
		const std::array<int32_t, 8> &n = process->punt_crc_counters;
		char text[128];
		std::snprintf(text, sizeof(text), "1:%i 2:%i 3:%i 4:%i 5:%i 6:%i 7:%i 8:%i", n[0], n[1],
				n[2], n[3], n[4], n[5], n[6], n[7]);
		world::HudChatLine line;
		line.argb = 0xFFFFFFFFu;
		line.system_ring = true;
		line.text = text;
		world.out.hud_chat_lines.push_back(std::move(line));
		return true;
	}
	// netdelay: the record's first argument, gated inside (Server_SetNetworkDelay)
	// [orig: Input_HandleActionBinding @0x49AD40 - case 103, Server_SetNetworkDelay called
	//  @0x49BB73].
	if (is(kNetDelayRow)) {
		if (ctx != nullptr) (void)Server_SetNetworkDelay(*ctx, record.p1);
		return true;
	}
	// resetgames: in a session on the authority.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 106, the gates @0x49BB80..0x49BB94,
	//  Server_ForceRoundEndAndClearState called @0x49BB9A]
	if (is(kResetGamesRow)) {
		if (in_session && authority && ctx != nullptr) Server_ForceRoundEndAndClearState(*ctx, world);
		return true;
	}
	// Ban: the record's first argument, the slot, gated inside (Server_BanPlayerSlot)
	// [orig: Input_HandleActionBinding @0x49AD40 - case 107, Server_BanPlayerSlot called
	//  @0x49BBA5]
	if (is(kBanRow)) {
		if (ctx != nullptr) (void)Server_BanPlayerSlot(*ctx, record.p1);
		return true;
	}
	// LastGame: in a session on the authority the rotation's last-game word flips, and the
	// Misc toast STRMISC_LASTGAME_ON with the word now set, else STRMISC_LASTGAME_OFF, goes to
	// the SYSTEM ring in white.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 108, the gates @0x49BBE4..0x49BBF8,
	//  Server_ToggleLastGame called @0x49BBFE (`xor g_LastGameToggle, 1` @0x4FE885), the keys
	//  pushed @0x49BC0E / @0x49BC15,
	//  GameText_GetString("Misc", ..) @0x49BC57, Chat_AddMessageChannel2(.., -1, 930)
	//  @0x49BC60]
	if (is(kLastGameRow)) {
		if (!in_session || !authority || ctx == nullptr || ctx->rotation == nullptr) return true;
		ctx->rotation->last_game = !ctx->rotation->last_game;
		world.out.hud_chat_lines.push_back({"Misc",
				ctx->rotation->last_game ? "STRMISC_LASTGAME_ON" : "STRMISC_LASTGAME_OFF",
				0xFFFFFFFFu, true});
		return true;
	}
	// tod: on an authority that is not a session peer (a dedicated host, in a session or
	// not), the record's first argument as HHMM sets the clock word, unclamped
	// (environment_time_from_hhmm), then the tile cache's reset and the environment cube's
	// full refresh (WeatherState::reset_time_caches).
	// [orig: Input_HandleActionBinding @0x49AD40 - case 438, the gates @0x49BDAB..0x49BDBF, the
	//  packing @0x49BDC5..0x49BE42, Environment_SetCurrentTime called @0x49BE43,
	//  Terrain_ResetTileCache called @0x49BE4B, EnvCube_ForceFullRefresh called @0x49BE50]
	if (is(kTodRow)) {
		if (!authority || peer) return true;
		world.weather.set_current_time(environment_time_from_hhmm(record.p1));
		world.weather.reset_time_caches();
		return true;
	}
	// todrate: the same gates and a nonzero argument; the clock's advance per tick is
	// 0x18000000 / (62 * argument), the product 32-bit and the divide signed. A product that
	// wraps to 0 (the argument -2^31) divides by zero in retail, which ends the process; here
	// it stores nothing.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 497, the gates @0x49BE5A..0x49BE76, the
	//  product @0x49BE7C..0x49BE89, `idiv ecx` @0x49BE8B, the store @0x49BE8D]
	if (is(kTodRateRow)) {
		if (!authority || peer || record.p1 == 0) return true;
		const auto divisor = static_cast<int32_t>(static_cast<uint32_t>(record.p1) * 62u);
		if (divisor == 0) return true;
		world.weather.set_tod_advance_per_tick(static_cast<uint32_t>(0x18000000 / divisor));
		return true;
	}
	return true;
}

bool Role::reset_local_input() {
	// Player_ResetLocalInput: the local entity and its def, then Input_DisableScrollLock
	// (the embedder's device, on this return: D-NET-563), Input_ResetAllState (the deferred queue here; the key
	// state and the key-press latches the embedder's devices, take_input_reset; the event
	// queue's memset, which ends the frame's drain, and apply_input's input_queue_reset_),
	// then g_InputFlags, the pack and the idle snap (LocalPlayer::reset_key_input)
	// (D-NET-560). [orig: Player_ResetLocalInput @0x4B2B80 - the entity and def tests
	//  @0x4B2B85..0x4B2B8D, Input_DisableScrollLock @0x4B2B91, Input_ResetAllState
	//  @0x4B2B99, @0x4B2B9E..0x4B2BC2]
	mission::MissionKernel &kernel = *kernel_;
	const world::Entity *local = kernel.local.player();
	if (local == nullptr) {
		// A joiner's local entity is its own from the mission start's Player_InitPlayer
		// on (ClientRuntime::local_entity_bound; a spectator's alike), before L spawns at
		// the name-match: the device and queue legs reset from then, and the key-input
		// leg waits for L's body (D-NET-560, V-L2-s6 FOUND 4). [orig: Player_InitPlayer
		//  @0x4E15F0 - the store @0x4E17EB]
		const ClientRuntime *runtime = client_runtime();
		if (kind() != RoleKind::Joiner || runtime == nullptr || !runtime->local_entity_bound())
			return false;
	} else if (!local->has_item_def) {
		return false;
	}
	input_events_.reset();
	kernel.local.clear_deferred_input();
	input_reset_pending_ = true;
	input_queue_reset_ = true;
	kernel.local.reset_key_input();
	return true;
}

void Role::reset_all_input_state() {
	// [orig: Input_ResetAllState @0x497ED0 - the event queue's memset @0x497EE6, the
	//  deferred queue's @0x497EF7, the key-press latch @0x497F08 (the devices')]
	input_events_.reset();
	kernel_->local.clear_deferred_input();
	input_reset_pending_ = true;
	input_queue_reset_ = true;
}

bool Role::request_deployment_pick(uint16_t wire_value) {
	mission::MissionKernel &kernel = *kernel_;
	ClientRuntime *runtime = client_runtime();
	if (runtime == nullptr) return false;
	// The deploy screen is the game loop's, as every input sender is
	// (ClientRuntime::in_game_loop).
	if (kind() == RoleKind::Joiner && !runtime->in_game_loop()) return false;
	// a6 is the head's dead read the Respawn key's gate takes (the local
	// entity's Flags & 2; a HostOnly host's constant 2 has no client here):
	// the authority's own body, a joiner's replica read (D-NET-562's window).
	// Under permanent death (the S2C 0x08 bit 15) a dead player does nothing
	// with spectators off (bit 13) and otherwise queues the reliable 0x51
	// [i16 0xFFFF] in place of the pick.
	// [orig: Input_HandleActionBinding -- a6 @0x49AD52..0x49AD98; case 12
	//  @0x49B024..0x49B03C, the convert @0x49B065..0x49B0B9, the pick from
	//  @0x49B0C3; g_SessionPermanentDeath / g_SessionSpectatorsAllowed stored by
	//  NapiNPClientMsg_HandleSessionConfig @0x428386 / @0x42838C]
	const bool local_dead = kind() == RoleKind::Joiner ? runtime->local_player_dead()
			: kernel.local.local_player_dead();
	const replication::ClientState &cs = runtime->state();
	const bool convert = local_dead && cs.permanent_death;
	if (convert && !cs.spectators_allowed) return false;
	// The shared head of both send arms, none of it on the spectators-off arm above:
	// Input_ResetAllState (the deferred queue here; the key state and the key-press
	// latches are the embedder's devices, through take_input_reset), the epilog
	// screen's input event 83, which no handler takes (no catalog record carries
	// code 83; the head's filters (the death screen's, the stub, the cine's, the
	// troop class's 58..119 default) pass it and the dispatcher's switch sends
	// 75..99 to its default), then Dialog_ResetAll (D-NET-685).
	// [orig: Input_HandleActionBinding @0x49AD40 - the spectators-off jump @0x49B03C,
	//  Input_ResetAllState @0x49B042, the epilog test and Input_QueueEvent(0x53)
	//  @0x49B047..0x49B058, Dialog_ResetAll @0x49B060, the hold @0x49B067; event 83's
	//  filters DeathScreen_FilterInputAction @0x52AD50 (the call @0x49AE4C),
	//  Input_NullActionFilter (@0x49AE68), Cine_FilterInputAction @0x56FC30 (@0x49AE84),
	//  the def+0x170 callback Input_HandleActionBinding_0 @0x4E0420 (@0x49AEB6)]
	// The queue's memset ends the drain the action rides (drain_input_events; D-NET-685,
	// V-L2-s7) [orig: Input_ResetAllState @0x497ED0 - the event queue's memset @0x497EE6].
	input_events_.reset();
	kernel.local.clear_deferred_input();
	input_reset_pending_ = true;
	input_queue_reset_ = true;
	kernel.world.script.dialog.reset();
	if (convert) return runtime->queue_spectator_convert();
	return runtime->queue_deployment_pick(wire_value);
}

world::LocalViewSessionInputs Role::view_session_inputs_for(
		const ClientRuntime *runtime, bool joiner, bool local_dead, bool in_session) {
	// What the arbiter reads from the session: the net layer sits above the
	// world group, so its client state crosses as plain values.
	world::LocalViewSessionInputs s;
	// The session word is the launch's network type, not the replica fold:
	// the SP launch sets type 0, so is_in_session reads 0 for the whole SP
	// mission although its in-process listen server folds a loopback client.
	// [orig: CNapiNetwork_SetNetworkType @0x4c4a50 stores +0x58 = (type in
	//  1..3) @0x4c4a85; SinglePlayer_StartMission passes 0 @0x561bce
	//  (`xor ebx, ebx` @0x561b30); the readers here: Render_ProcessMainSceneFrame
	//  @0x5ca22d (the forced first person) and @0x5ca8f6 (the dead-in-session
	//  distortion skip)]
	s.in_session = in_session;
	s.joiner = joiner;
	// The client-local death-screen latch: the 0x0A flags1 bit-0 edges every
	// role's view folds (the listen host's own loopback included)
	// [orig: g_DeathScreenActive, NapiNPClientMsg_0x00A @0x42ff88..0x43002b].
	s.hud_hit_feedback_frames = runtime != nullptr ? runtime->state().hud_hit_feedback_frames : 0;
	if (runtime) {
		s.hud_service.preround_seconds = runtime->state().preround_delay_seconds;
		s.armory_reuse_seconds = runtime->state().armory_reuse_seconds;
		s.hud_service.reload_seconds = runtime->state().vehicle_reload_seconds;
		s.hud_service.owned_zone_mask = runtime->state().owned_zone_mask;
		// Keep link order: an equal-distance designation retains the first.
		// [orig: SpawnPoint_FindNearestByTypeAndTeam @0x5BBF42..0x5BBFC3]
		const auto &state = runtime->state();
		for (const auto &link : state.minimap.linked) {
			if (!link.active || !link.remaining_ticks || link.type != 1)
				continue;
			const auto *owner = state.find(link.handle);
			s.hud_designations.push_back(
					{ link.x, link.y, link.radius_q16, uint8_t(owner ? owner->team : 0) });
		}
	}
	s.death_screen_active = runtime != nullptr && runtime->state().death_screen_active;
	s.death_screen_submode = runtime != nullptr ? runtime->state().death_screen_submode : 0;
	if (runtime != nullptr && runtime->state().spectate_target != 0xFFFF) {
		const replication::ClientState &cs = runtime->state();
		s.spectate_target_key = world::kCameraTrackedTargetKey | cs.spectate_target;
		const replication::ClientEntityState *row = cs.find(cs.spectate_target);
		s.spectate_target_dead =
				row != nullptr && row->state_flags_known && (row->state_flags & 2u) != 0;
	}
	s.end_round_known = runtime != nullptr && runtime->state().end_round.known;
	// [orig: NapiNPClientMsg_0x01D @0x430840 -> g_EndRoundWinnerTeam]
	s.end_round_winner_team = runtime != nullptr && runtime->state().end_round.header_known
			? static_cast<int32_t>(runtime->state().end_round.header.winner_team)
			: 0;
	s.local_dead = local_dead;
	s.death_camera_target_known = runtime != nullptr;
	if (runtime != nullptr) {
		const replication::ClientDeathCameraTarget &t = runtime->state().death_camera;
		s.death_camera_target[0] = t.x;
		s.death_camera_target[1] = t.y;
		s.death_camera_target[2] = t.z;
	}
	return s;
}

int64_t Session::now_us() {
	return static_cast<int64_t>(io::perf_now_us());
}

TransitionResult Session::transition(State to) {
	TransitionResult out;
	out.from = state_;
	out.to = to;
	if (state_ == to) {
		out.code = TransitionCode::NoOp;
		return out;
	}
	set_state(to);
	out.code = TransitionCode::Applied;
	return out;
}

void Session::set_state(State to) {
	state_ = to;
	// The pause word the action rows' head reads: a single-player session's Paused
	// state (the pause row's, the in-game menu's and the transport's pause alike).
	if (role_ != nullptr && role_->kernel() != nullptr)
		role_->kernel()->world.rules.pause_word = to == State::Paused;
}

TransitionResult Session::rejected(
		TransitionCode code, SessionError error) const {
	TransitionResult out;
	out.code = code;
	out.from = state_;
	out.to = state_;
	out.error = std::move(error);
	return out;
}

TransitionResult Session::configure_role(Role &role) {
	if (state_ != State::Unloaded &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("role can change only while unloaded or failed"));
	}
	if (role_ == &role && kind_ == role.kind()) return rejected(TransitionCode::NoOp);
	role_ = &role;
	kind_ = role.kind();
	return {TransitionCode::Applied, state_, state_, {}};
}

TransitionResult Session::begin_connect() {
	if (kind_ != RoleKind::Joiner) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("only a joiner can enter Connecting"));
	}
	if (state_ != State::Unloaded &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("connect requires Unloaded or Failed"));
	}
	last_error_ = {};
	reset_bank();
	return transition(State::Connecting);
}

TransitionResult Session::begin_load() {
	if (state_ != State::Unloaded &&
			state_ != State::Connecting &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("load requires Unloaded, Connecting, or Failed"));
	}
	last_error_ = {};
	reset_bank();
	return transition(State::Loading);
}

TransitionResult Session::complete_load() {
	if (state_ != State::Loading) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("load completion requires Loading"));
	}
	last_error_ = {};
	reset_bank();
	// The mission starts inside the Game Loop mode's initialize, and the loop
	// reads its clock only after that returns, so the load is never banked
	// [orig: Game Loop mode @0x82F340 -> Game_StartMission; Game_MainLoop
	// clock read @0x52B75C]. The start also arms three frames whose render
	// time is never banked [orig: Game_StartMission @0x525e1f].
	start_rebase_frames_ = kStartRebaseFrames;
	rebase_clock_ = true;
	return transition(State::Running);
}

TransitionResult Session::fail(SessionError error) {
	if (!error) error = {SessionErrorCode::TickFailed, "session failed"};
	last_error_ = error;
	reset_bank();
	TransitionResult out = transition(State::Failed);
	out.error = std::move(error);
	return out;
}

void Session::latch_input(const FrameInput &input) {
	latch_packet(input.player);
	latest_camera_ = input.camera;
}

void Session::latch_packet(const InputPacket &packet) {
	// A fire press an earlier sample latched and no tick consumed dispatches at
	// the next tick's input frame, where the attack row's case tests its binding
	// live: a binding the newer sample reads up takes the release leg, which
	// fires nothing outside a PowerThrow windup (D-NET-1000) [orig:
	// Input_HandleActionBinding_0 @0x4E0420 (case 0x95's Input_IsBindingActive(149)
	// call @0x4E07D9, the release leg @0x4E07E9..0x4E07F1); Input_IsBindingActive
	// @0x498320 (the key slots @0x498338 / @0x498348, the mouse wParam @0x498368)].
	if ((packet.held_action_bits & HELD_FIRE) == 0)
		pending_input_.pressed_action_bits &= ~PRESSED_FIRE;
	pending_input_.movement = packet.movement;
	pending_input_.cursor_dx += packet.cursor_dx;
	pending_input_.cursor_dy += packet.cursor_dy;
	pending_input_.look_delta_x += packet.look_delta_x;
	pending_input_.look_delta_y += packet.look_delta_y;
	pending_input_.held_action_bits = packet.held_action_bits;
	pending_input_.pressed_action_bits |= packet.pressed_action_bits;
	pending_input_.sequence = packet.sequence;
}

TickInput Session::merged_tick_input(
		const FrameInput &input, bool consume_one_shots) {
	TickInput out;
	out.camera = latest_camera_;
	out.loaded_model_list_count = input.loaded_model_list_count;
	out.player = pending_input_;
	out.consume_one_shots = consume_one_shots;
	if (!consume_one_shots) {
		out.player.look_delta_x = 0.0f;
		out.player.look_delta_y = 0.0f;
		out.player.pressed_action_bits = 0;
	}
	return out;
}

void Session::consume_pending_one_shots() {
	pending_input_.look_delta_x = 0.0f;
	pending_input_.look_delta_y = 0.0f;
	pending_input_.pressed_action_bits = 0;
}

void Session::step_mouse_pump(int32_t update, int32_t due, bool menu_open, int32_t (&axis)[2]) {
	const int32_t cursor[2] = {pending_input_.cursor_dx, pending_input_.cursor_dy};
	if (update == 0) pending_input_.cursor_dx = pending_input_.cursor_dy = 0;
	mouse_pump_.step(update == 0, update + 1 == due, due, menu_open, cursor, axis);
}

// The frame's updates. Retail's update runs, in order: the head (the mouse pump on
// the frame's first update; behind the pause word's first test the dialog step and
// `add g_CurrentTick`; Game_TickHudFrameCounters, paused or not), the input frame
// (Input_ProcessFrame under no menu screen, its drain dispatching the queued key
// events, the pause row's case 25 among them), then the body: Server_TickUpdate
// unpaused only, Entity_UpdateAllEntities (which returns on the pause at its
// entry), the tracer pool, the third-person camera, the weather, the weapon walk
// behind the second pause test, the 62-update second, the exit tests and the
// /PROFILE frame. So the update whose input frame raises the pause has already
// advanced the tick and stepped its HUD counters unpaused, and runs as a paused
// update from its input frame on, as does every later update of the frame; the
// update whose input frame lowers it advanced no tick and stepped its counters
// paused, and runs its body unpaused. Ours' logic tick steps at the end of a
// tick's entity pass, so World::logic_tick at a tick's head already equals
// retail's g_CurrentTick after the head's add: the pausing update leaves it as
// it stands (the frozen tick is the pausing update's own) and the resuming
// update's tick ends with the step retail's next head takes. The dialog step,
// which the shell runs at the end of each tick, stands for the next update's
// head step on the same reading, so the pausing update runs no tick for it.
// [orig: Game_ProcessMainFrame @0x5263F0 (the pump @0x52640E..0x526508; the pause
//  word's test @0x5265A0, Dialog_UpdatePlayback @0x5265AF, `add g_CurrentTick`
//  @0x5265B4; Game_TickHudFrameCounters @0x5265D5; Input_ProcessFrame @0x52661D;
//  Server_TickUpdate behind the pause test @0x5266AE; Entity_UpdateAllEntities
//  @0x52674B; the second pause test @0x526779; the 62-update second
//  @0x5267AB..0x5267DF); Entity_UpdateAllEntities @0x4C2100 (the pause return
//  @0x4C2103); Input_HandleActionBinding @0x49AD40 (case 25 @0x49B520: the
//  in-session return @0x49B527, `xor dword_A87050, 1` @0x49B52D,
//  Input_ResetKeyQueue @0x49B53E)]
FrameOutcome Session::run_updates(int32_t due, const FrameInput &input, bool transport) {
	FrameOutcome out;
	out.status = FrameStatus::Ok;
	out.state = state_;
	const int64_t tick_start = now_us();
	mission::MissionKernel *kernel = role_ != nullptr ? role_->kernel() : nullptr;
	TickInputSource *source = kernel != nullptr ? input.tick_source : nullptr;
	// The pause word flips only out of a session (case 25's return @0x49B527).
	const bool flips_apply = !transport && kind_ == RoleKind::SinglePlayer;
	// The transport's forced step runs the role's tick as a running update does,
	// the action rows' head with it: the pause word is down for its tick.
	if (transport && kernel != nullptr) kernel->world.rules.pause_word = false;
	bool consume_one_shots = true;
	// Whether the update before ran a tick: a later update then steps the device
	// to the logic tick it runs as; the frame's first update and a paused one read
	// the device state the frame's head stepped.
	bool previous_ran = false;
	for (int32_t i = 0; i < due; ++i) {
		++out.perf.updates;
		// The embedder's update legs (set_update_head / set_update_tail): the head now, the tail
		// as this update ends.
		if (!transport && update_head_) update_head_();
		UpdateTail tail{transport ? nullptr : &update_tail_};
		if (state_ == State::Paused && !transport) {
			// A paused update: the mouse pump heads it (no input frame consumes the
			// axis under a menu screen; under the pause row's pause the head drops
			// the look rows), the HUD counters step paused ahead of its input frame,
			// then the input frame, whose pause row may lower the word.
			const bool menu_open = (input.player.held_action_bits & HELD_MENU_OPEN) != 0;
			int32_t axis[2] = {};
			step_mouse_pump(i, due, menu_open, axis);
			if (kernel == nullptr) continue;
			world::World &world = kernel->world;
			world.hud_frames.step(/*menu_paused=*/true, world.epilog_screen_active());
			// The update's keyboard pass, then its sample (with no source, the frame's).
			if (source != nullptr) source->run_update_rows(world.logic_tick, false);
			InputPacket sample =
					source != nullptr ? source->sample_tick(world.logic_tick) : input.player;
			// The sample's records join the queue behind the console's, and the drain
			// dispatches them in order: each pause row flips the word, the records behind
			// it meeting the head with it (a paused update's bit-0 rows drop, its other
			// rows run, and the one-shots it leaves drop with it, as no tick consumes
			// them); the update resumes when the drain leaves the word down
			// (TickInputSource carries the witness).
			if (source != nullptr) queue_update_records(*source);
			role_->drain_input_events(source, sample.held_action_bits, flips_apply);
			if (world.rules.pause_word) {
				run_paused_update_legs();
				previous_ran = false;
				continue;
			}
			// The resuming update: its body runs unpaused over that sample (the cursor
			// excluded: the paused head pumped it), with no tick step of its own at the
			// head and its counters already stepped.
			set_state(State::Running);
			sample.cursor_dx = sample.cursor_dy = 0;
			latch_packet(sample);
			latest_camera_ = input.camera;
			consume_one_shots = true;
			TickInput tick_input = merged_tick_input(input, true);
			tick_input.source = source;
			tick_input.drained = true;
			tick_input.head_stepped = true;
			if (!menu_open) {
				tick_input.player.look_delta_x += static_cast<float>(axis[0]);
				tick_input.player.look_delta_y += static_cast<float>(axis[1]);
			}
			tick_input.last_tick_of_batch = i + 1 == due;
			TickOutcome tick = run_one_tick(tick_input);
			if (tick.terminal()) {
				out.status = tick.status == TickStatus::SessionLost
						? FrameStatus::SessionLost : FrameStatus::Fatal;
				out.error = tick.error;
				fail(tick.error ? tick.error : SessionError{
						SessionErrorCode::TickFailed, "mission tick failed"});
				tail.cancel();
				break;
			}
			previous_ran = tick.ran();
			if (!tick.ran()) continue;
			out.ticks.push_back(std::move(tick));
			consume_pending_one_shots();
			consume_one_shots = false;
			count_update_into_second();
			continue;
		}
		// A running update's input frame: its keyboard pass, then its movement and
		// action rows at the logic tick it runs as (the frame's first update's too, over
		// the device state the frame's head stepped), which replace the held state with
		// edges of their own to consume, then the drain of its records
		// (TickInputSource carries the witness).
		const bool new_sample = previous_ran && kernel != nullptr;
		const int64_t rows_start = now_us();
		if (source != nullptr) source->run_update_rows(kernel->world.logic_tick, new_sample);
		if (source != nullptr) {
			latch_packet(source->sample_tick(kernel->world.logic_tick));
			queue_update_records(*source);
			consume_one_shots = true;
		}
		out.perf.input_rows_us += now_us() - rows_start;
		TickInput tick_input = merged_tick_input(input, consume_one_shots);
		tick_input.source = source;
		// The mouse pump's axis input for this tick (world::MouseAxisPump
		// carries the witness): the frame's first tick reads the cursor offset
		// the frame banked, every tick takes its spread share; under a menu
		// screen no input frame consumes it.
		const bool menu_open = (tick_input.player.held_action_bits & HELD_MENU_OPEN) != 0;
		int32_t axis[2] = {};
		step_mouse_pump(i, due, menu_open, axis);
		// The update's drain, ahead of its body: a role tick's input prologue reads its
		// outcome (TickInput::drained).
		if (kernel != nullptr) {
			const int64_t drain_start = now_us();
			role_->drain_input_events(source, tick_input.player.held_action_bits, flips_apply);
			out.perf.input_drain_us += now_us() - drain_start;
			tick_input.drained = true;
		}
		// The drain left the word raised (an odd count of pause rows): the records behind
		// the flip met the action rows' head with it set, and the update runs paused.
		const bool pauses = flips_apply && kernel != nullptr && kernel->world.rules.pause_word;
		if (pauses) {
			// The pausing update: the pending one-shots drop, as the action rows
			// behind the pause row meet the head's pause gate (their bit 0) [orig:
			// Input_HandleActionBinding @0x49AD40 (@0x49ADDD..0x49ADE8)]; the head's
			// HUD counters stepped unpaused before the input frame, and the body runs
			// paused: no role tick (no server tick, entity update or weapon walk),
			// only the paused legs.
			set_state(State::Paused);
			consume_pending_one_shots();
			consume_one_shots = false;
			world::World &world = kernel->world;
			world.hud_frames.step(/*menu_paused=*/false, world.epilog_screen_active());
			run_paused_update_legs();
			previous_ran = false;
			continue;
		}
		if (!menu_open) {
			tick_input.player.look_delta_x += static_cast<float>(axis[0]);
			tick_input.player.look_delta_y += static_cast<float>(axis[1]);
		}
		// [orig: Game_MainLoop @0x52ba21..0x52ba3a -- dword_24E0E80 = 1 when
		//  less than one 16 ms tick of backlog remains after this quantum, 0
		//  while catching up (and always 0 under g_CineFixedStepMode, which
		//  the port does not model)]
		tick_input.last_tick_of_batch = i + 1 == due;
		TickOutcome tick = run_one_tick(tick_input);
		out.perf.input_apply_us += tick.input_us;
		if (tick.terminal()) {
			out.status = tick.status == TickStatus::SessionLost
					? FrameStatus::SessionLost : FrameStatus::Fatal;
			out.error = tick.error;
			fail(tick.error ? tick.error : SessionError{
					SessionErrorCode::TickFailed, "mission tick failed"});
			tail.cancel();
			break;
		}
		previous_ran = tick.ran();
		if (!tick.ran()) continue;
		out.ticks.push_back(std::move(tick));
		if (consume_one_shots) {
			consume_pending_one_shots();
			consume_one_shots = false;
		}
		// The transport's forced step is no main-loop update.
		if (!transport) count_update_into_second();
		// A joiner's final wait ended in this update: the mode's initialize returns,
		// and what this frame banked is never drained (Session::advance resets it).
		if (role_->take_final_wait_end()) {
			final_wait_ended_ = true;
			break;
		}
	}
	if (transport && kernel != nullptr) kernel->world.rules.pause_word = state_ == State::Paused;
	out.state = state_;
	out.perf.tick_us = now_us() - tick_start;
	out.perf.ticks = out.ticks_run();
	return out;
}

void Session::queue_update_records(TickInputSource &source) {
	if (role_->input_frame_runs()) {
		source.queue_records(role_->input_events());
		return;
	}
	controls::InputEventQueue dropped;
	source.queue_records(dropped);
}

// One fixed tick: the shared input prologue through the role, the role's
// tick, then the outcome and the shell's observer.
TickOutcome Session::run_one_tick(const TickInput &input) {
	TickOutcome out;
	if (role_ == nullptr || role_->kernel() == nullptr) {
		out.status = TickStatus::Fatal;
		out.error = {SessionErrorCode::TickFailed, "no role is bound to the session"};
		return out;
	}
	role_->kernel()->world.rules.last_tick_of_batch = input.last_tick_of_batch;
	role_->kernel()->world.rules.update_head_stepped = input.head_stepped;
	const int64_t input_start = now_us();
	role_->apply_input(input);
	const int64_t tick_start = now_us();
	out.input_us = tick_start - input_start;
	role_->run_tick(input);
	// The lose cine's edit fade scaled the main loop's smoothing history from
	// inside this update. Retail's store lands mid-drain, after the bank's EMA
	// and before the frame's later updates, which count their quanta off the
	// bank register and never re-read the word; its one reader is the next
	// frame's EMA. So the scales apply here, after the tick that raised them and
	// ahead of the frame's next tick and the next frame's bank, in their call
	// order (world::TickAccumulator::scale_smoothing_history carries the witness)
	// [orig: Game_MainLoop @0x52B630 (the EMA's store @0x52B866, the drain
	//  @0x52BA08..0x52BA6B, the EMA's read @0x52B845); Game_ProcessMainFrame
	//  @0x5263F0 (Entity_UpdateAllEntities @0x52674B) -> Cinematic_EpilogUpdate
	//  (the call @0x4C2230) -> CCineEventEditFade_Start @0x5726C9 /
	//  CCineEventEditFade_Finish @0x572775 -> Game_ScaleSmoothedFrameTime @0x52B5D0].
	std::vector<float> &scales = role_->kernel()->world.out.frame_time_scales;
	for (const float factor : scales) accumulator_.scale_smoothing_history(factor);
	scales.clear();
	out.tick_us = now_us() - tick_start;
	out.net_us = role_->last_net_us();
	out.logic_tick = static_cast<int32_t>(role_->kernel()->world.logic_tick);
	if (observer_ != nullptr) observer_->after_tick();
	SessionError lost;
	if (role_->session_lost(lost)) {
		out.status = TickStatus::SessionLost;
		out.error = lost;
		return out;
	}
	out.status = TickStatus::Ran;
	if (observer_ != nullptr && !observer_->accept_tick(out)) {
		out.status = TickStatus::SessionLost;
		out.error = {SessionErrorCode::SessionLost, "the tick observer cancelled the batch"};
	}
	return out;
}

bool Session::draws_scene(const hud::HudToggleState *toggles) const {
	// The role carries the two process words: is_mp_session_peer is clear on a dedicated host
	// alone, is_authority on a joiner alone.
	return renders() &&
			hud::main_frame_draws_scene(kind_ != RoleKind::DedicatedHost, kind_ != RoleKind::Joiner,
					toggles != nullptr && toggles->server_status_view);
}

void Session::step_cine_render_frame() {
	// The rendered frame's cine pass, after the frame's logic updates: the SP
	// end-of-round cine marks its frame drawn, or stops past its last event
	// (world/epilog_cine.h) [orig: Game_MainLoop's Game_ProcessMainFrame
	// drain, then Render_ProcessMainSceneFrame @0x5CADFC -> Cine_RenderOrEndPlayback].
	if (role_ == nullptr || role_->kernel() == nullptr) return;
	role_->kernel()->world.epilog.render_pass();
}

void Session::run_paused_update_legs() {
	// Under the pause retail's updates still run, their pause-gated legs held:
	// each steps the main-frame counter (the session and HUD counters wait on the
	// pause, as the tick does; run_updates steps them ahead of the input frame),
	// and the weather tick, which the update runs ahead of its pause test, so the
	// time of day and the weather keep stepping through the pause [orig:
	// Game_ProcessMainFrame @0x5263F0 -- `cmp dword_A87050, 0` @0x5265A0 around the
	// tick's ++g_CurrentTick, Game_TickHudFrameCounters @0x5265D5 every update,
	// Environment_UpdateWeatherTick @0x57E9B0 called @0x526774 ahead of the pause test
	// @0x526779; the entity update's own pause return, Entity_UpdateAllEntities
	// @0x4C2100 (@0x4C2103)].
	if (role_ == nullptr || role_->kernel() == nullptr) return;
	mission::MissionKernel &kernel = *role_->kernel();
	world::World &world = kernel.world;
	// The paused update's other unconditional legs, in its order: the whole
	// pending-sound pass (the fire-sound slots count down, then the incoming-lock
	// tone drains) and the trigger holds count down, the tracer pool ages and
	// drains its channels (the entity update that appends to them holds), the
	// third-person camera's update runs its orbit legs and eases its follow anchor
	// (LocalPlayer::run_paused_camera_update), and the update counts toward the
	// 62-update second. Its tip countdown steps with the
	// main-frame counter (hud_toggles_tip_frames) and its message rings' lives run
	// on that counter too, so both age through the pause already [orig:
	// Game_ProcessMainFrame @0x5263F0 (the Sound_TickPendingSlots call @0x526697,
	// the Sound_TickTriggerHolds call @0x52669C, the CEffectEmitterPool_Tick call
	// @0x526758, the CTipSystem_TickCountdown call @0x526762, the
	// ThirdPersonCamera_Update call @0x52676F behind its tracked-entity test
	// @0x526767, the Input_DecrementCooldownTimers call @0x5267A6, the 62-update second
	// @0x5267AB..0x5267DF, each outside the pause word's tests @0x5265A0 /
	// @0x526779); Sound_TickPendingSlots @0x529310 (the g_LockOnMeToneTicks drain
	// @0x5293A5..0x5293AF); CEffectEmitterPool_Tick @0x5DB830;
	// Sound_TickTriggerHolds @0x5292D0].
	world.tick_pending_sound_slots();
	world.round_sim.trails.tick();
	kernel.local.run_paused_camera_update();
	kernel.tick_weather();
	count_update_into_second();
	// The /PROFILE frame leg past the exit tests, at the frozen tick
	// [orig: Game_ProcessMainFrame @0x526879..0x5268E7].
	role_->write_profile_frame();
}

void Session::count_update_into_second() {
	// Every logic update counts toward the 62-update second; each second
	// publishes the frames rendered since [orig: Game_ProcessMainFrame
	// @0x5267AB..0x5267DF].
	if (++second_update_count_ >= io::kTicksPerSecondInt) {
		second_update_count_ -= io::kTicksPerSecondInt;
		frames_last_second_ = frames_rendered_;
		frames_rendered_ = 0;
	}
}

void Session::count_rendered_frame() {
	// The frame's render counts, paused or not, and the host's status page takes
	// the statistics [orig: GameLoop_RenderFrame @0x521CF9].
	++frames_rendered_;
	if (role_ != nullptr)
		role_->observe_frame_statistics(frames_last_second_, accumulator_.cpu_percent());
}

RoleView Session::frame_role_view() const {
	RoleView view;
	view.kernel = role_ != nullptr ? role_->kernel() : nullptr;
	view.runtime = role_ != nullptr ? role_->client_runtime() : nullptr;
	view.joiner = kind_ == RoleKind::Joiner;
	return view;
}

bool Session::hud_pass_called(const FrameInput &input, const FrameOutcome &out) const {
	// Inside the world block, past its world-skip word, at its own call site
	// (inmatch::hud_overlays_called carries the witness) [orig: Render_ProcessMainSceneFrame
	//  @0x5CA620..0x5CA640; @0x5CAB2B..0x5CAD04].
	if (out.world_skipped || role_ == nullptr || role_->kernel() == nullptr) return false;
	return hud_overlays_called(frame_role_view(),
			input.hud_toggles != nullptr && input.hud_toggles->help_open);
}

void Session::step_map_overlay_death_clear(const FrameOutcome &out) {
	// The world block's map overlay leg, past the HUD pass and the sun veil: an open map
	// closes for a body with the dead bit, once per drawn frame that draws its world (a
	// frame that skips it, and the status page, run no world block) [orig:
	// Render_ProcessMainSceneFrame - the world-skip jump @0x5CA620..0x5CA640, the HUD pass's
	// return to 0x5CAB95 @0x5CAD09, the sun veil @0x5CAC4B, `cmp g_MapOverlayMode, 0`
	// @0x5CAC50, `test byte ptr [ecx+24h], 2` @0x5CAC63, the clear @0x5CAC6D].
	if (out.world_skipped || role_ == nullptr || role_->kernel() == nullptr) return;
	hud::HudMapControl &map = role_->kernel()->local.hud_map_control;
	if (map.mode == 0) return;
	if (local_player_dead(frame_role_view())) map.on_local_player_dead();
}

void Session::step_scene_head(const FrameInput &input, FrameOutcome &out) {
	mission::MissionKernel *kernel = role_ != nullptr ? role_->kernel() : nullptr;
	// A frame inside a joiner's final wait runs no render callback (renders), so none of its
	// legs below.
	out.rendered = renders();
	// The render callback's first leg, the listener update, asks a session peer's cine
	// whether it plays, whatever the callback draws next, so a cine past its last event
	// stops under the status page too [orig: GameLoop_RenderFrame @0x52131E ->
	//  Audio_UpdateListenerFromView @0x43B400 - is_mp_session_peer @0x43B40A, the
	//  Cine_IsPlaybackActive call @0x43B40C].
	if (out.rendered && kernel != nullptr && kind_ != RoleKind::DedicatedHost)
		(void)kernel->world.epilog.playback_active();
	out.scene_drawn = draws_scene(input.hud_toggles);
	// The scene frame's head word: a playing cine, or the command map over the menu
	// [orig: Render_ProcessMainSceneFrame - the Cine_IsPlaybackActive call @0x5CA171,
	//  UI_IsMenuScreenOpen @0x5CA17F and g_CmapScreenOpen @0x5CA188, ebx @0x5CA17D / @0x5CA190].
	bool skipped = false;
	if (out.scene_drawn && kernel != nullptr) {
		const bool cine = kernel->world.epilog.playback_active();
		skipped = cine || input.command_map_open;
	}
	out.world_skipped = skipped;
	if (kernel != nullptr) kernel->local.view.world_skipped = skipped;
}

void Session::step_hud_scene_cursor() {
	// The scene frame reads the HUD target cursor ahead of its own HUD pass: the weapon
	// inset's name takes the last pass's build (world::HudTargetCursor)
	// [orig: Render_ProcessMainSceneFrame @0x5CA0F0 - the Render_WeaponInsetScene call
	//  @0x5CA949, the HUD_RenderAllOverlays calls @0x5CACE3 / @0x5CAD04].
	if (role_ == nullptr || role_->kernel() == nullptr) return;
	role_->kernel()->local.note_hud_scene_head();
}

void Session::step_hud_radar_frame() {
	if (role_ == nullptr || role_->kernel() == nullptr) return;
	static_assert(kHudRadarGatesDefault == hud::HudFrameCompiler::kRadarGatePass,
			"the default gates run the pass");
	ClientRuntime *runtime = role_->client_runtime();
	// The pass's spawn-success early-out [orig: g_RoundOver
	// @0x5a8084], live; the hud_detail-3 early-out and the map site are the
	// embedder's HUD's.
	const bool spawn_gate = runtime != nullptr && runtime->state().spawn_success_gate;
	// The pass's head builds the entity-info record past its first three returns, ahead of
	// the level-3 one and the radar legs (build_hud_entity_info carries the witness).
	build_hud_entity_info(*role_->kernel(), spawn_gate);
	const bool pass_runs =
			(hud_radar_gates_ & hud::HudFrameCompiler::kRadarGatePass) != 0u && !spawn_gate;
	const bool map_site = (hud_radar_gates_ & hud::HudFrameCompiler::kRadarGateMapSite) != 0u;
	step_hud_radar(*role_->kernel(), runtime, pass_runs, map_site, state_ == State::Paused,
			hud_radar_);
}

FrameOutcome Session::advance(const FrameInput &input) {
	FrameOutcome out;
	out.state = state_;
	if (state_ == State::Paused) {
		// The paused frame banks its time as every main frame does, and its updates
		// run the one update loop: the paused legs (the lock tone's word drains
		// there) until an update's pause row lowers the word (run_updates). The frame
		// then renders and runs its HUD pass, where the menu pause keeps the lock
		// tone unregistered. The main loop has no pause gate on its render callback or
		// its re-base read, so a paused frame past a re-based render banks only the time
		// since that render, as a running one does [orig: Game_MainLoop @0x52BAA7..0x52BAC6
		// (the render callback's call @0x52BAC6), the re-base read @0x52BAC8..0x52BAD2].
		const double elapsed = rebase_clock_ && input.since_render_seconds >= 0.0
				? input.since_render_seconds : input.delta_seconds;
		rebase_clock_ = false;
		uint32_t lock_stamp_ms = 0;
		int32_t lock_sleeps = 0;
		const int64_t bank_start = now_us();
		const int32_t due = bank_frame(elapsed, input, lock_stamp_ms, lock_sleeps);
		const int64_t bank_us = now_us() - bank_start;
		// The mouse pump heads every main frame, ahead of the pause word's test, so its
		// words hold what the paused frames pumped: the paused frame's cursor waits for
		// its first update as a running frame's does, and no input frame consumes the
		// axis (a menu screen skips the input frame; under the pause row's pause the
		// dispatcher head drops the look rows, whose flags carry bit 0) [orig:
		// Game_ProcessMainFrame @0x5263F0 (the pump @0x52640E..0x526508, the pause word's
		// test @0x5265A0); Input_HandleActionBinding @0x49AD40 (the pause gate
		// @0x49ADDD..0x49ADE8)].
		pending_input_.cursor_dx += input.player.cursor_dx;
		pending_input_.cursor_dy += input.player.cursor_dy;
		out = run_updates(due, input, /*transport=*/false);
		out.frame_stamp_ms = lock_stamp_ms;
		out.perf.bank_us = bank_us;
		out.perf.lock_sleeps = lock_sleeps;
		// A frame whose updates all stayed paused ran no tick.
		if (out.ticks.empty() && !out.terminal()) out.status = FrameStatus::NotRunning;
		last_perf_ = out.perf;
		const int64_t render_start = now_us();
		// The render callback's choice after the drain (draws_scene, with the head's cine and
		// world-skip legs); both arms count.
		step_scene_head(input, out);
		count_rendered_frame();
		// The scene frame draws the in-game menu, then reaches its clear and its count-down
		// with no pause test, so the three frames past a start (the SP restart's, a menu
		// opened inside them) present black and re-base under the menu too
		// [orig: GameLoop_RenderFrame - the scene arm @0x521CE8; Render_ProcessMainSceneFrame
		//  - UI_IsMenuScreenOpen @0x5CAE13, the menu @0x5CAE31, the clear @0x5CAE62..0x5CAE94,
		//  the count-down @0x5CAF08, the re-base flag @0x5CAF0E].
		if (out.scene_drawn && start_rebase_frames_ > 0) {
			out.start_blank = true;
			--start_rebase_frames_;
			rebase_clock_ = true;
		}
		if (!out.terminal() && out.scene_drawn) {
			step_hud_scene_cursor();
			out.hud_pass_called = hud_pass_called(input, out);
			if (out.hud_pass_called) step_hud_radar_frame();
			step_map_overlay_death_clear(out);
			step_cine_render_frame();
		}
		out.perf.render_legs_us = now_us() - render_start;
		return out;
	}
	if (state_ != State::Running) {
		out.status = FrameStatus::NotRunning;
		out.rendered = renders();
		out.scene_drawn = draws_scene(input.hud_toggles);
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	// The frame's start stamp, the main loop's clock: the read the frame banked (the embedder's
	// frame stamp, else a read now), which the lock restamps when it slept
	// [orig: Game_MainLoop @0x52B630 - GetTickCount @0x52B798, kept @0x52B7AE; the lock's
	//  restamp @0x52B8D7].
	uint32_t frame_start_ms =
			input.frame_stamp_ms != 0 ? input.frame_stamp_ms : world::frame_tick_count_ms();
	// A mission-start frame re-based the clock once it had rendered, so this frame banks only
	// the time since that render: the embedder's main-loop clock read after the render
	// (FrameInput::since_render_seconds) [orig: Game_MainLoop @0x52bac8..0x52bad2, GetTickCount
	// @0x52BAD0].
	const double elapsed = rebase_clock_ && input.since_render_seconds >= 0.0
			? input.since_render_seconds : input.delta_seconds;
	rebase_clock_ = false;
	uint32_t lock_stamp_ms = 0;
	int32_t lock_sleeps = 0;
	const int64_t bank_start = now_us();
	const int32_t due = bank_frame(elapsed, input, lock_stamp_ms, lock_sleeps);
	// A lock that slept restamped the frame's start with its last read, which the CPU share
	// measures its work from [orig: Game_MainLoop @0x52B8D7, the share's read
	// @0x52BA9B..0x52BAA1].
	if (lock_stamp_ms != 0) frame_start_ms = lock_stamp_ms;
	// The FR counter is published before the drain runs this frame's ticks
	// [orig: Game_MainLoop g_StatsAvgFps store @0x52B98F, drain @0x52BA08].
	if (role_ != nullptr) {
		role_->observe_frame_rate(accumulator_.average_fps());
		role_->observe_cpu_share(accumulator_.cpu_percent());
	}
	const int64_t bank_us = now_us() - bank_start;
	out = run_updates(due, input, /*transport=*/false);
	out.frame_stamp_ms = lock_stamp_ms;
	out.perf.bank_us = bank_us;
	out.perf.lock_sleeps = lock_sleeps;
	// The CPU share's inputs: this frame's work from its start stamp to the
	// end of the drain, and the updates the drain ran, a paused one too [orig:
	// @0x52ba4f; @0x52ba9b..0x52baa1].
	accumulator_.record_frame_work(world::frame_tick_count_ms() - frame_start_ms, out.perf.updates);
	const int64_t render_start = now_us();
	// The render callback reads its view word after the drain and draws the scene frame or the
	// status page in its place (draws_scene, with the head's cine and world-skip legs); each
	// arm's render counts.
	step_scene_head(input, out);
	// Each update counted toward the 62-update second inside the loop; this
	// frame's render counts now (a frame inside the final wait renders none).
	if (out.rendered) count_rendered_frame();
	// A joiner's mission start ends at its final wait, not at the load: the start arms
	// its three frames ahead of that wait, which renders nothing, so they are the first
	// three drawn after the S2C 0x0F; the mode's initialize returns only then, and the
	// main loop resets its bank, smoothing and phase and reads its clock after it. The
	// frames that ran the wait's passes (ours do; they render nothing, renders) bank nothing
	// of what follows. The frame whose drain took the 0x0F renders the main loop's first
	// frame past that reset, so the three arm ahead of its count-down below and its render
	// takes the first: the next three frames bank only the time since a render.
	// [orig: Game_StartMission @0x524360 - g_MissionStartBlankFrameCount = 3 @0x525E1F, the
	//  final wait @0x526295..0x5262E5 (no render); Render_ProcessMainSceneFrame @0x5CAEFF..0x5CAF0E
	//  (the count-down @0x5CAF08); Game_MainLoop @0x52B630 - the mode-init reset
	//  @0x52B70F..0x52B757 (the smoothing @0x52B71B), the clock read @0x52B766]
	if (final_wait_ended_) {
		final_wait_ended_ = false;
		accumulator_.reset();
		start_rebase_frames_ = kStartRebaseFrames;
		rebase_clock_ = true;
	}
	// Each of the first frames drawn after the mission start counts down and
	// raises the re-base flag, the scene frame's tail: a frame the status page
	// stands in for counts none down [orig: Render_ProcessMainSceneFrame
	// @0x5caeff..0x5caf0e, the scene arm @0x521CE8 alone].
	if (out.scene_drawn && start_rebase_frames_ > 0) {
		// The frame presents black: its clear reads the count ahead of the count-down
		// (FrameOutcome::start_blank carries the witness).
		out.start_blank = true;
		--start_rebase_frames_;
		rebase_clock_ = true;
	}
	last_perf_ = out.perf;
	// The frame's render follows the drain: the scene frame's HUD pass where its call site
	// calls it (hud_pass_called), the world block's map overlay clear, then its cine pass;
	// the status page runs none, so under it the radar ages nothing, and a frame that skips
	// its world runs no HUD pass and no clear
	// [orig: Game_MainLoop's Game_ProcessMainFrame drain,
	// then the render's HUD_RenderAllOverlays and cine pass; Render_ProcessMainSceneFrame
	// @0x5CAD04 (@0x5CACE3 for a chase view from a control seat) -> HUD_RenderAllOverlays,
	// inside the world block @0x5CA620..0x5CA640 jumps past, the clear @0x5CAC6D, then
	// @0x5CADFC -> Cine_RenderOrEndPlayback, the scene arm @0x521CE8 alone].
	if (!out.terminal() && out.scene_drawn) {
		step_hud_scene_cursor();
		out.hud_pass_called = hud_pass_called(input, out);
		if (out.hud_pass_called) step_hud_radar_frame();
		step_map_overlay_death_clear(out);
		step_cine_render_frame();
	}
	out.perf.render_legs_us = now_us() - render_start;
	return out;
}

FrameOutcome Session::step_once(const FrameInput &input) {
	FrameOutcome out;
	out.state = state_;
	if (state_ != State::Paused ||
			kind_ != RoleKind::SinglePlayer) {
		out.status = FrameStatus::NotRunning;
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	out = run_updates(1, input, /*transport=*/true);
	last_perf_ = out.perf;
	if (!out.terminal()) step_cine_render_frame();
	return out;
}

FrameOutcome Session::drive_one(const FrameInput &input) {
	FrameOutcome out;
	out.state = state_;
	const bool local_paused = state_ == State::Paused &&
			kind_ == RoleKind::SinglePlayer;
	if (state_ != State::Running && !local_paused) {
		out.status = FrameStatus::NotRunning;
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	out = run_updates(1, input, /*transport=*/true);
	last_perf_ = out.perf;
	if (!out.terminal()) step_cine_render_frame();
	return out;
}

TransitionResult Session::pause() {
	if (kind_ != RoleKind::SinglePlayer) {
		return rejected(TransitionCode::RejectedForNetworkRole,
				{SessionErrorCode::NetworkRoleLocked,
						"network sessions cannot pause"});
	}
	if (state_ == State::Paused)
		return rejected(TransitionCode::NoOp);
	if (state_ != State::Running) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("pause requires Running"));
	}
	// The frame-time bank runs on through the pause: g_TickPhase is written only by
	// the main loop's mode init and its drain, g_FrameTimeSmoothedFp4 only by the
	// mode init, the EMA's store, the cine fade's scale and an unreferenced clear, so
	// no pause restarts the phase, the smoothing history or the FR counter
	// (D-NET-1005). The pending one-shots drop: no input frame consumes them
	// under the menu and its close resets the queue. [orig: Game_MainLoop @0x52B630
	//  (the mode init @0x52B715 / @0x52B71B, the EMA's read @0x52B845 and store
	//  @0x52B866, the drain's phase @0x52BA6B); Game_ScaleSmoothedFrameTime @0x52B5D0
	//  (the store @0x52B606, from CCineEventEditFade_Start @0x5726C9 /
	//  CCineEventEditFade_Finish @0x572775); Game_ClearSmoothedFrameTime @0x52B620 (no
	//  xref)]
	consume_pending_one_shots();
	// The in-game menu's paused frames run the mouse pump under the menu: the
	// paused frame's first update under the menu zeroes the shares and its
	// two-tap clears the previous one (run_updates' paused update), so all six
	// words are zero by the resume; the pause row's paused frames pump the
	// cursor, overwriting them at their first update. Retail runs at least one
	// such update between a pause and its resume; the zero here stands in for a
	// pause that banks no paused update [orig: Game_ProcessMainFrame @0x5263F0
	// (the UI_IsMenuScreenOpen call @0x52642F, the zeroes @0x526438..0x52644D,
	// the two-tap @0x5264E6..0x526508)].
	mouse_pump_ = {};
	return transition(State::Paused);
}

TransitionResult Session::resume() {
	if (state_ == State::Running)
		return rejected(TransitionCode::NoOp);
	if (state_ != State::Paused) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("resume requires Paused"));
	}
	// The bank runs on (pause() carries the witness); the pending one-shots drop.
	consume_pending_one_shots();
	return transition(State::Running);
}


TransitionResult Session::close() {
	if (state_ == State::Unloaded)
		return rejected(TransitionCode::NoOp);
	const State from = state_;
	set_state(State::Stopping);
	if (role_ != nullptr) role_->close();
	reset_bank();
	start_rebase_frames_ = 0;
	rebase_clock_ = false;
	last_error_ = {};
	hud_radar_gates_ = kHudRadarGatesDefault;
	hud_radar_ = {};
	set_state(State::Unloaded);
	return {TransitionCode::Applied, from, state_, {}};
}

void Session::reset_bank() {
	// [orig: Game_MainLoop @0x52B630 (the mode init @0x52B715..0x52B757)]
	accumulator_.reset();
	pending_input_ = {};
	latest_camera_ = {};
	last_perf_ = {};
}

} // namespace opennova::inmatch
