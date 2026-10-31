#include <runtime/inmatch/player_input_frame.h>

#include <base/gameprofile/game_type.h>
#include <base/title/title.h> // Title::friendly_tags_skip_mode_1_on_death_screen
#include <runtime/controls/controls.h> // Catalog (the rows' dispatch codes)
#include <runtime/controls/device_messages.h>
#include <runtime/hud/hud_chat_entry.h> // chat_talk_row_token
#include <runtime/world/player_look.h>  // pass_through_close_axis

#include <cmath>
#include <string_view>

namespace opennova::inmatch {

namespace {

// The held USE row's dispatch code (useitem), the action head's key.
constexpr int kUseItemCode = 177;

// The records whose arms are the shell's (PlayerInputShell::record_leg): the special-key chain's
// deploy pick, input event 12, and the escape row's, code 18 (controls::KeyRowEvent and
// PlayerAction::EmbedderRow carry the witness).
constexpr int kDeployPickCode = 12;
constexpr int kEscapeCode = 18;
// The Screen Shot row (code 2): its case writes the frame to a file, the shell's device leg
// (gameprofile/player_files.h screenshot_file_name carries the witness).
constexpr int kScreenShotCode = 2;
// The cheat row's code 40, past the role's gates (out of a session, a local player): the shell's
// weapon-screen toggle (Role::dispatch_record carries the witness).
constexpr int kWeaponScreenCode = 40;

// A talk or HUD row's dispatch code by its token, as the key scan queues it (the catalog row's
// code).
int32_t row_token_code(const controls::Catalog &catalog, const char *token) {
	const controls::ActionDef *def = catalog.for_token(token);
	return def != nullptr ? def->code : -1;
}

} // namespace

int PlayerInputFrame::talk_row_of(int code) const {
	const controls::ActionDef *def = catalog_->for_code(code);
	if (def == nullptr) return -1;
	const std::string_view token(def->token);
	for (int row = 0; row < hud::kChatTalkRowCount; ++row)
		if (token == hud::chat_talk_row_token(row)) return row;
	return -1;
}

int PlayerInputFrame::hud_row_of(int code) {
	for (int row = 0; row < hud::kHudToggleRowCount; ++row)
		if (code == hud::hud_toggle_row_code(row)) return row;
	return -1;
}

bool PlayerInputFrame::shell_record(int code) {
	return code == kDeployPickCode || code == kEscapeCode || code == kScreenShotCode ||
			code == kWeaponScreenCode;
}

void PlayerInputFrame::bind(PlayerInputDevice *device, PlayerInputSession *session,
		PlayerInputShell *shell) {
	device_ = device;
	session_ = session;
	shell_ = shell;
}

void PlayerInputFrame::set_catalog(const controls::Catalog &catalog) {
	// The action table over the catalog, the title's.
	catalog_ = &catalog;
	actions_ = controls::PlayerActions(catalog);
}

void PlayerInputFrame::clear_queues() {
	row_samples_.clear();
	row_records_.clear();
	action_events_.clear();
	sample_records_.clear();
	pump_records_.clear();
}

void PlayerInputFrame::teardown() {
	if (device_ != nullptr) device_->set_scan_gates(false, false);
	clear_queues();
}

PlayerInputFrame::FrameHead PlayerInputFrame::begin_frame(bool presenter, bool has_player,
		bool gameplay_input_active, bool menu_open) {
	FrameHead head;
	// The window's messages were read since the last frame: the keyboard state holds any
	// turn-off sent then (retail pumps them at once after its send). A turn-off the Escape key's
	// reset owes goes out here, ahead of the frame's sample, which then reads the word off.
	scroll_lock_turned_off_ = false;
	if (scroll_lock_turn_off_owed_) {
		scroll_lock_turn_off_owed_ = false;
		scroll_lock_turned_off_ = device_ != nullptr && device_->scroll_lock_turn_off();
	}
	head.sequence = ++frame_sequence_;
	frame_player_ = false;
	frame_gameplay_input_active_ = gameplay_input_active;
	frame_menu_open_ = menu_open;
	if (!presenter) return head;
	// The frame's head steps the devices to the logic tick its first update runs as; the
	// session runs every update's input frame through this frame: its keyboard pass and its
	// talk and HUD rows (run_update_rows), then its movement and action rows (sample_tick).
	head.sourced = true;
	const int64_t logic_tick = session_live() ? session_->facts().logic_tick : -1;
	float look_x = 0.0f;
	float look_y = 0.0f;
	tick_spectating_ = step_devices(logic_tick, look_x, look_y);
	head.spectating = tick_spectating_;
	// The look waits for the next update's sample, a frame that drains none included, as the
	// session kept it for the next tick.
	tick_look_x_ += look_x;
	tick_look_y_ += look_y;
	// The rows' state this frame sees waits for the next update's poll: a frame whose bank
	// drains no update runs no input frame, and its key events stay queued, a release and a new
	// press included (TickInputSource carries the witness).
	queue_row_sample();
	if (!has_player) {
		look_x_ = 0.0f;
		look_y_ = 0.0f;
		return head;
	}
	frame_player_ = true;
	head.player = true;
	// The frame's head reads: the mouse motion as the cursor offset in whole pixels, its
	// sub-pixel rest carried to the next frame, which the session's mouse pump reads in the
	// frame's first update and spreads over the frame's updates (world::MouseAxisPump), and the
	// menu-screen bit the paused updates read.
	if (gameplay_input_active) {
		head.cursor_dx = static_cast<int32_t>(look_x_);
		head.cursor_dy = static_cast<int32_t>(look_y_);
		look_x_ -= static_cast<float>(head.cursor_dx);
		look_y_ -= static_cast<float>(head.cursor_dy);
	} else {
		look_x_ = 0.0f;
		look_y_ = 0.0f;
	}
	head.menu_open = frame_menu_open_;
	return head;
}

bool PlayerInputFrame::step_devices(int64_t logic_tick, float &look_x, float &look_y) {
	const bool live = session_live();
	const PlayerInputSession::Facts facts = live ? session_->facts() : PlayerInputSession::Facts{};
	if (device_ == nullptr) return live && facts.death_screen;
	// The scripted input device (devtools/scripted_input.h) steps on the logic tick this sample
	// is keyed to, ahead of every device read below.
	device_->step_scripted(live, logic_tick, look_x, look_y);
	// The death screen samples the same device input: the engine's filter hands it to the
	// free-fly motor or the chase orbit, and the binding scan admits only the death-screen rows
	// (controls::PlayerActionPoll::death_screen).
	const bool spectating = live && facts.death_screen;
	// The key scan's gates for every poll of this input frame, the HUD's after the world tick
	// too: the death screen, and the scan word, set on a dedicated host and otherwise while the
	// local body is dead (controls::BindingSet::set_scan_gates carries the witness).
	device_->set_scan_gates(spectating, live && (facts.dedicated_host || facts.local_dead));
	return spectating;
}

InputPacket PlayerInputFrame::sample_tick(uint32_t logic_tick) {
	// The device already stands at logic_tick: the frame's head or this update's run_update_rows
	// stepped it, and the update's keyboard pass and rows ran ahead of the action rows.
	(void)logic_tick;
	InputPacket packet;
	packet.sequence = frame_sequence_;
	if (frame_player_) {
		sample(packet, tick_look_x_, tick_look_y_, tick_spectating_);
	} else {
		// No sample takes the action rows' events: they are dropped. Every other row's key-down
		// is a record still (the talk and HUD rows' run with or without a local player, the
		// command prompt a Serve Only host opens, as do the escape row's, the deploy picks with
		// their parameter, and a keylist binding on a row no arm of the poll owns;
		// controls::player_actions_own_code carries the witness).
		for (const controls::KeyRowEvent &event : action_events_)
			if (!event.release && !controls::player_actions_own_code(*catalog_, event.code))
				sample_records_.push_back({event.code, event.param, 0, 0, event.sign});
		action_events_.clear();
	}
	tick_look_x_ = 0.0f;
	tick_look_y_ = 0.0f;
	return packet;
}

void PlayerInputFrame::run_update_rows(uint32_t logic_tick, bool new_sample) {
	// An update's input frame, a paused update's too: its keyboard pass, whose talk, HUD and
	// pause rows' events become records with the action rows' in the update's sample, and the
	// drain dispatches them, the pause row's flip among them (TickInputSource carries the
	// witness). A later update of a running frame steps the device to the logic tick it runs as
	// first.
	if (new_sample) {
		tick_spectating_ = step_devices(logic_tick, tick_look_x_, tick_look_y_);
		queue_row_sample();
	}
	// The menu-screen bit at this update's head: a screen an earlier update's keyboard pass
	// opened (the escape row's INGAME) gates this one; every such screen holds a shell state
	// whose gameplay word reads down.
	if (shell_ != nullptr && shell_->menu_screen_open()) {
		frame_menu_open_ = true;
		frame_gameplay_input_active_ = false;
	}
	// Under a menu screen no input frame runs, so nothing drains the key events: they wait, in
	// order, for the close's reset (reset_input_state) or, under DEATH's exit confirmation, for
	// the first input frame after its NO raises the word.
	if (frame_menu_open_) return;
	// The input frame's head: the shell's keyboard pass drains the key queue, its special-key
	// legs land in this update, and the row events it queues become the update's records in its
	// sample; a frame that runs no update keeps its keys (TickInputSource carries the witness).
	// Its legs may run the input reset, which drops the rows queued so far.
	if (shell_ != nullptr) shell_->keyboard_pass();
	// A later paused update of the same frame reads the state the frame left.
	if (row_samples_.empty()) queue_row_sample();
	// The talk and HUD rows' off-keyboard arms' states in their order, so a press, its release
	// and a new press of a mouse or joystick arm seen by frames that ran no update each reach
	// the rows: each press edge is its row's record, queued ahead of the sample's (a mouse
	// button's click is a record of the update's message pump, ahead of its keyboard pass;
	// TickInputSource carries the witness). The mousescale fire's sign is its arm's (Shift held;
	// controls::BindingSet::fire_signed).
	for (const RowSample &row_sample : row_samples_) {
		const uint32_t talk_fired = row_sample.talk & ~talk_held_;
		const uint32_t hud_fired = row_sample.hud & ~hud_held_;
		talk_held_ = row_sample.talk;
		hud_held_ = row_sample.hud;
		for (int row = 0; row < hud::kChatTalkRowCount; ++row)
			if ((talk_fired & (1u << row)) != 0)
				row_records_.push_back(
						{row_token_code(*catalog_, hud::chat_talk_row_token(row)), 0, 0, 0, false});
		for (int row = 0; row < hud::kHudToggleRowCount; ++row) {
			if ((hud_fired & (1u << row)) == 0) continue;
			const char *token = hud::hud_toggle_row_token(row);
			row_records_.push_back({row_token_code(*catalog_, token), 0, 0, 0,
					device_ != nullptr && device_->fire_signed(token)});
		}
	}
	row_samples_.clear();
}

void PlayerInputFrame::queue_records(controls::InputEventQueue &records) {
	// The update's message pump: a left button's down runs its PowerThrow windup test ahead of
	// the input frame's records (world::local_weapon_left_button_down carries the witness).
	if (left_down_owed_) {
		left_down_owed_ = false;
		if (session_live()) session_->left_button_down();
	}
	// Behind the records already waiting (the console's): the message pump's wheel and click
	// records, the talk and HUD rows' off-keyboard edges, then the sample's rows (its key events
	// in key order, then the action rows' off-keyboard edges); the queue's cap drops the rest.
	for (const controls::InputEventRecord &record : pump_records_) (void)records.push(record);
	pump_records_.clear();
	for (const controls::InputEventRecord &record : row_records_) (void)records.push(record);
	row_records_.clear();
	for (const controls::InputEventRecord &record : sample_records_) (void)records.push(record);
	sample_records_.clear();
}

void PlayerInputFrame::dispatch_record(const controls::InputEventRecord &record) {
	if (!session_live()) return;
	// A talk row's record opens its line (one dispatch per record, an OS repeat's included); a
	// HUD row's runs its toggle with the sign its fire carried, the pause row's behind the role's
	// flip of the session's word (Role::dispatch_record), whose case resets the key queue behind
	// its flip (controls::KeyEventQueue::reset carries the witness; hud_toggles_dispatch and
	// ChatEntry::dispatch_rows the rows').
	const int talk_row = talk_row_of(record.code);
	if (talk_row >= 0) {
		session_->dispatch_talk_rows(1u << talk_row, frame_gameplay_input_active_);
		return;
	}
	const int hud_row = hud_row_of(record.code);
	if (hud_row >= 0) {
		if (dispatch_hud_rows(*session_, 1u << hud_row, record.sign, record.p1) &&
				device_ != nullptr)
			device_->reset_key_queue();
		return;
	}
	// The escape row's record (case 18: the Escape action), the screen shot's (case 2), the
	// weapon-screen toggle's (case 40) and a deploy pick's (case 12, its pick the record's first
	// argument) run the shell's arms at their place in the drain; the escape's and the pick's
	// input reset (the role's memset of the event queue) ends the drain there, and the shell's
	// leg runs the devices' reset at once.
	if (shell_record(record.code)) {
		if (shell_ != nullptr) shell_->record_leg(record.code, record.p1);
		return;
	}
	// The held USE row's record (case 0xB1) past the dispatcher's head by its row's flags (the
	// deploy overlay, the round-over gate and the pause word drop it as the dead bit does, under
	// a pass-through screen's input frame too): its Scroll Lock turn-off and the arm the press
	// latch decides (controls::PlayerActions::use_item_record carries the witness). A dropped
	// record arms nothing, so the hold ends at the frame's tail.
	if (record.code == kUseItemCode) {
		if (!session_->action_head_admits(record.code)) return;
		apply_immediate(actions_.use_item_record(session_->take_use_item_press()));
		return;
	}
	// A movement or look row's record the role took past the head (a typed row's, a queued
	// Last_move's): the sample carries the held rows' movement, so the record arms nothing but
	// the back, forward (fourth argument 0) and strafe cases' Scroll Lock turn-off, once a frame.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 147 @0x4E10C8, case 148 @0x4E10D9, case
	//  151 @0x4E0C57 (Input_DisableScrollLock @0x4E0C66), case 152 @0x4E0C85 (the fourth
	//  argument's test @0x4E0C92, Input_DisableScrollLock @0x4E0C9E), case 153 @0x4E0D66, case
	//  154 @0x4E0E8C, case 155 @0x4E0F5A, case 156 @0x4E1012 (Input_DisableScrollLock
	//  @0x4E1021), case 157 @0x4E104F (Input_DisableScrollLock @0x4E105E), case 158 @0x4E1074,
	//  case 159 @0x4E10B7, case 176 @0x4E0CD5]
	switch (record.code) {
		case 147: case 148: case 151: case 152: case 153: case 154: case 155: case 156:
		case 157: case 158: case 159: case 176: {
			const bool turns_off = record.code == 151 || record.code == 156 ||
					record.code == 157 || (record.code == 152 && record.p3 == 0);
			if (turns_off) {
				controls::PlayerActionFrame frame;
				frame.scroll_lock_off.push_back(record.code);
				apply_immediate(frame);
			}
			return;
		}
		default: break;
	}
	if (const auto request = controls::player_action_request(*catalog_, record)) {
		// The 1st Person Toggle past the head: its first-person arm turns the FP gun bit on, the
		// HUD toggles' flag, whose event the embedder's HUD applies (world/player_view.h
		// player_view_apply_view_action carries the witness).
		if (request->action == controls::PlayerAction::ViewToggle) {
			if (session_->action_head_admits(request->code) && session_->view_toggle() &&
					hud_toggles_ != nullptr) {
				hud_toggles_->showhud_flags |= hud::kShowHudFlagGun;
				hud_toggle_events_.push_back(hud::hud_toggle_event::kGunBitChanged);
			}
			return;
		}
		apply_player_action(*session_, *request);
		return;
	}
	// The session's own records: the exit (3) and the punt by slot (34), a console line's in
	// play.
	(void)session_->dispatch_action_record(record.code, record.p1);
}

void PlayerInputFrame::end_input_frame() {
	// The input frame's tail: the USE hold's release edge (controls::PlayerActions::
	// end_input_frame carries the witness).
	apply_immediate(actions_.end_input_frame(session_live()));
}

void PlayerInputFrame::apply_player_action(PlayerInputSession &session,
		const controls::PlayerActionRequest &request) {
	// A binding row's event passes the dispatcher's head by its row's flags
	// (Role::action_head_admits carries the witness).
	if (request.code >= 0 && !session.action_head_admits(request.code)) return;
	session.run_action(request);
}

void PlayerInputFrame::apply_immediate(const controls::PlayerActionFrame &frame) {
	const bool live = session_live();
	// Retail's auto-run turn-offs: a case that turns its toggle off passes the dispatcher's head
	// by its code; one turn-off per frame, as the keyboard state reads it only once the window's
	// messages are read (controls::PlayerActionFrame::scroll_lock_off).
	for (const int code : frame.scroll_lock_off) {
		if (scroll_lock_turned_off_) break;
		if (code < 0 || (live && session_->action_head_admits(code)))
			scroll_lock_turned_off_ = device_ != nullptr && device_->scroll_lock_turn_off();
	}
	// The arms that run at once (no code): the USE row's opens, its release's mount toggle.
	for (const auto &request : frame.requests) {
		if (request.code >= 0) continue;
		if (request.action == controls::PlayerAction::OpenArmory) {
			armory_requested_ = true;
		} else if (request.action == controls::PlayerAction::OpenVehicleMenu) {
			vehicle_menu_requested_ = true;
		} else if (live) {
			apply_player_action(*session_, request);
		}
	}
}

void PlayerInputFrame::queue_row_event(int code, bool sign, int key) {
	// Every row's event, in key order, for the next device sample: an action row's through its
	// poll, a talk or HUD row's as its record (controls::PlayerActionPoll::embedder_row).
	action_events_.push_back(controls::KeyRowEvent{code, sign, key, false});
}

void PlayerInputFrame::queue_pick_event(int32_t param) {
	// Input event 12 with the pick its first argument (controls::KeyRowEvent::param carries the
	// witness).
	action_events_.push_back(controls::KeyRowEvent{kDeployPickCode, false, 0, false, param});
}

void PlayerInputFrame::run_key_release_pass() {
	if (device_ == nullptr) return;
	for (const controls::KeyRowEvent &event : device_->key_release_pass())
		action_events_.push_back(event);
}

void PlayerInputFrame::queue_row_sample() {
	const RowSample row_sample{talk_rows_down(), hud_rows_down()};
	if (row_samples_.empty() || row_samples_.back().talk != row_sample.talk ||
			row_samples_.back().hud != row_sample.hud)
		row_samples_.push_back(row_sample);
}

uint32_t PlayerInputFrame::talk_rows_down() const {
	uint32_t rows_down = 0;
	if (device_ == nullptr) return rows_down;
	for (int row = 0; row < hud::kChatTalkRowCount; ++row)
		if (device_->held_off_keyboard(hud::chat_talk_row_token(row))) rows_down |= 1u << row;
	return rows_down;
}

uint32_t PlayerInputFrame::hud_rows_down() const {
	uint32_t rows_down = 0;
	if (device_ == nullptr) return rows_down;
	for (int row = 0; row < hud::kHudToggleRowCount; ++row)
		if (device_->held_off_keyboard(hud::hud_toggle_row_token(row))) rows_down |= 1u << row;
	return rows_down;
}

void PlayerInputFrame::sample(InputPacket &packet, float look_x, float look_y, bool spectating) {
	if (device_ == nullptr) return;
	const bool active = frame_gameplay_input_active_;
	const bool live = session_live();
	const platform::InputState state = device_->state();
	// A live UI overlay keeps the world ticking but must actively submit a neutral movement
	// frame: a player who opened the armory while running would otherwise keep running under
	// it. Scripted input (probes, automation) arrives as the device's movement override: the
	// same seven bits a device produces, so the sim sees one contract. u/d ride the LEAN keys
	// there: retail packs lean_left/lean_right as MoveOrder bits 0x40/0x80, and the aircraft
	// mover reads those same two bits as descend/ascend - the collective is the lean pair,
	// overloaded.
	world::PlayerInput movement;
	const bool overridden = device_->movement_override(movement);
	world::PlayerInput &out = packet.movement;
	if (active) {
		if (overridden) {
			out.forward = movement.forward;
			out.back = movement.back;
			out.left = movement.left;
			out.right = movement.right;
			out.lean_left = movement.lean_left;
			out.lean_right = movement.lean_right;
			out.jump = movement.jump;
		} else {
			// The live binding table drives every key (defaults: WASD move, Q/E lean, Space
			// jump; controls k_catalog); jump is momentary (the motor jumps once when grounded).
			// There is no run key: running is the automatic forward-walk promotion in the sim's
			// body selection, suppressed while scoped (world/player_input.h).
			out.forward = device_->pressed("move_forward");
			out.back = device_->pressed("move_back");
			out.left = device_->pressed("strafe_left");
			out.right = device_->pressed("strafe_right");
			out.lean_left = device_->pressed("LeanRoll_left");
			out.lean_right = device_->pressed("LeanRoll_right");
			out.jump = device_->pressed("move_jump");
		}
	}
	out.free_look = active && device_->pressed("FreeLook");
	out.look_up = active && device_->pressed("look_up");
	out.look_down = active && device_->pressed("look_down");
	out.turn_left = active && device_->pressed("turn_left");
	out.turn_right = active && device_->pressed("turn_right");
	// The mouse motion rides the frame's head (begin_frame). The scripted device's look pixels
	// are axis input at their own tick, as onHook's driver adds them to the axis word in that
	// input frame, under the gate real motion meets on its way in (device_event runs only under
	// live gameplay with the mouse captured). A pass-through screen's close owes this sample its
	// cursor offset, axis input of this tick alone (close_pass_through_screen).
	const bool captured = state.cursor_captured;
	if (active && captured) {
		packet.look_delta_x = look_x + close_axis_x_;
		packet.look_delta_y = look_y + close_axis_y_;
	}
	close_axis_x_ = 0.0f;
	close_axis_y_ = 0.0f;
	controls::PlayerActionPoll gate;
	gate.active = active;
	gate.captured = captured;
	gate.simulation_available = live;
	gate.keyboard_captured = device_->keyboard_captured();
	gate.emotes_menu_open = hud_toggles_ != nullptr && hud_toggles_->emotes_menu_open;
	gate.radio_menu_open = hud_toggles_ != nullptr && hud_toggles_->radio_menu_open;
	gate.death_screen = spectating;
	gate.menu_open = frame_menu_open_;
	// The keyboard pass's events since the last sample; the talk, HUD and pause rows', the
	// escape row's and the deploy picks are the embedder's records in key order with the action
	// rows' and the spectator rows' (dispatch_record).
	gate.key_events.swap(action_events_);
	action_events_.clear();
	// The Scroll Lock word is a device the scripted input replaces like the keys.
	const bool scroll_lock = !overridden && !scroll_lock_turned_off_ && state.scroll_lock;
	// The joystick toggle pass's fires behind the keyboard and mouse passes' records
	// (controls::BindingSet::joystick_toggle_pass); no input frame runs under a menu screen.
	if (!frame_menu_open_) {
		const auto joystick = device_->joystick_records(spectating, scroll_lock);
		gate.key_events.insert(gate.key_events.end(), joystick.begin(), joystick.end());
	}
	gate.embedder_row = [this](int code) {
		return talk_row_of(code) >= 0 || hud_row_of(code) >= 0 || shell_record(code);
	};
	gate.scroll_lock_on = scroll_lock;
	const controls::PlayerActionFrame actions = actions_.poll(*device_, gate);
	// Retail's auto-run rides the forward key (controls::PlayerActionFrame::auto_run_forward). A
	// case that turns its toggle off passes the dispatcher's head by its code; one turn-off per
	// frame, as the keyboard state reads it only once the window's messages are read.
	out.forward = out.forward || actions.auto_run_forward;
	apply_immediate(actions);
	// The fire binding's and the ToSpecial keys' state, which their records' arms test live; the
	// fire, reload and ToSpecial rows are records (Role::dispatch_record carries the witness).
	packet.held_action_bits = (actions.fire_held ? HELD_FIRE : 0u) |
			(actions.fire_keys_held ? HELD_FIRE_KEYS : 0u) |
			(actions.to_special_held ? HELD_TO_SPECIAL : 0u) |
			(frame_menu_open_ ? HELD_MENU_OPEN : 0u);
	packet.pressed_action_bits = 0;
	// The scripted fire hold standing for the left button: its press owes the left button's
	// down, a joystick's none (controls::PlayerActionFrame::fire_scripted_left_down carries the
	// witness).
	if (actions.fire_scripted_left_down) left_down_owed_ = true;
	for (const auto &request : actions.requests) {
		// An action row's request is a record the update's drain dispatches in queue order
		// (queue_records, dispatch_record), a talk, HUD, pause or escape row's key event's and a
		// deploy pick's among them (a pick's parameter its first argument), the held USE row's
		// last.
		if (request.code < 0) continue;
		const int32_t first =
				request.action == controls::PlayerAction::EmbedderRow ? request.value : 0;
		sample_records_.push_back({request.code, first, 0, 0, request.sign});
	}
	// The death screen's own rows (the spectate sub-mode and target): their key events are the
	// poll's records in key order; their off-keyboard arms' press edges are records of the
	// update's drain behind the sample's other rows, each past the dispatcher's head at its
	// dispatch (dispatch_record, controls::player_action_request).
	if (spectating && live) {
		for (const auto &request : actions_.poll_spectator(*device_, active))
			sample_records_.push_back({request.code, 0, 0, 0, request.sign});
	}
}

bool PlayerInputFrame::dispatch_hud_rows(PlayerInputSession &session, uint32_t rows_fired,
		bool mouse_scale_negative, int32_t mouse_scale_param) {
	// The HUD's key rows are action bindings like the rest: the input frame's key scan fires
	// them and its drain dispatches them, every tick, so a row pressed for one tick of a
	// multi-tick frame fires on that tick. hudcolor (row 76) defaults to Ctrl+F6 beside
	// huddetail's bare F6 (row 50): the binding sampler's two passes keep them apart (Ctrl+F6
	// fires only hudcolor, F6 only huddetail), and its first match gives a key both rows carry to
	// hudcolor alone (code 10 before 19; controls::BindingSet::pressed_key). showhud (row 27)
	// ships unbound. Ctrl is a binding modifier; Shift or Alt held gates nothing, as the key scan
	// has no such gate (hud::HudKeyEdge carries the witness).
	// [orig: Input_ProcessFrame @0x49D520 (the Input_ProcessPlayerFrame call @0x49D58B, the
	//  drain's Input_HandleActionBinding call @0x49D5D8); the key scan's fire @0x49d42f (the
	//  modifier pass) and @0x49d488 (the fallback); huddetail dispatch @0x4E0601; showhud
	//  dispatch @0x4E0561]
	// The row events are the keyboard pass's key-downs (an OS repeat fires its row again) and
	// this frame's press edges of the mouse, joystick and scripted arms (run_update_rows); the
	// cycles and the window toggles are the HUD's dispatch (hud::hud_toggles_dispatch); the
	// embedder's HUD applies the event words at its frame.
	if (hud_toggles_ == nullptr || !session.available()) return false;
	// Each row's event passes the dispatcher's head by its catalog row's flags (the pre-round
	// timer, the deploy overlay, the round-over gate, the dead operand): the rows the role's head
	// drops this input frame (hud::HudKeyPoll::head_drops carries the witness).
	uint32_t head_drops = 0;
	for (int row = 0; row < hud::kHudToggleRowCount; ++row)
		if (!session.action_head_admits(hud::hud_toggle_row_code(row))) head_drops |= 1u << row;
	// The connection mode's two bits: an authority is any role but a joiner, a session peer any
	// role but a dedicated host. The mousescale row steps by its fire's sign bit (Shift on a
	// modifier-less key).
	const PlayerInputSession::Facts facts = session.facts();
	hud::HudKeyPoll keys;
	hud::hud_key_poll_set_rows(keys, rows_fired);
	keys.active = frame_gameplay_input_active_;
	keys.in_session = facts.mp_session;
	keys.objective_game = game_type::is_objective(facts.game_type);
	keys.local_alive = !facts.local_dead;
	keys.authority = !facts.joiner;
	keys.mp_session_peer = !facts.dedicated_host;
	keys.mouse_scale_negative = mouse_scale_negative;
	keys.head_drops = head_drops;
	// The friendly-tags cycle reads the death-screen latch under the session title's rule
	// (hud::HudKeyPoll::death_screen).
	keys.death_screen = facts.death_screen;
	keys.friendly_tags_skip_mode_1_on_death_screen =
			facts.title != nullptr && facts.title->friendly_tags_skip_mode_1_on_death_screen;
	uint64_t events = hud::hud_toggles_dispatch(*hud_toggles_, keys);
	// A typed mousescale's argument rides above the event bits (hud_toggle_event::
	// kMouseScaleParamShift carries the witness).
	if ((events & hud::hud_toggle_event::kMouseScaleStepped) != 0)
		events |= static_cast<uint64_t>(static_cast<uint32_t>(mouse_scale_param))
				<< hud::hud_toggle_event::kMouseScaleParamShift;
	if (events != 0) hud_toggle_events_.push_back(events);
	// The pause row's flip reaches the session at this update (Session::run_updates); the HUD's
	// later apply of the event finds the session already there.
	return (events & hud::hud_toggle_event::kPauseToggled) != 0;
}

std::vector<uint64_t> PlayerInputFrame::take_hud_toggle_events() {
	std::vector<uint64_t> out;
	out.swap(hud_toggle_events_);
	return out;
}

SpecialKeyOutcome PlayerInputFrame::special_key(PlayerInputSession &session,
		const SpecialKeyInput &input) {
	if (!session.available()) return {};
	// The chain's arms read the HUD's words as the key meets them, so a pick that closed a menu
	// hands the frame's next digit to the next arm.
	hud::HudToggleState no_hud;
	hud::HudToggleState &toggles = hud_toggles_ != nullptr ? *hud_toggles_ : no_hud;
	const SpecialKeyOutcome outcome = session.special_key_chain(toggles, actions_, input);
	if (outcome.digit_pick) apply_player_action(session, *outcome.digit_pick);
	return outcome;
}

bool PlayerInputFrame::take_armory_request() {
	const bool requested = armory_requested_;
	armory_requested_ = false;
	return requested;
}

bool PlayerInputFrame::take_vehicle_menu_request() {
	const bool requested = vehicle_menu_requested_;
	vehicle_menu_requested_ = false;
	return requested;
}

void PlayerInputFrame::reset_input_state() {
	// The reset's head: the key queue, then the key state (controls::KeyEventQueue::reset
	// carries the witness).
	if (device_ != nullptr) {
		device_->reset_key_queue();
		device_->reset_key_state();
	}
	// The reset's event queue memset drops the rows a no-update frame left waiting.
	clear_queues();
	left_down_owed_ = false;
	if (session_live()) session_->clear_deferred_input();
}

void PlayerInputFrame::close_pass_through_screen() {
	// The device read: the cursor's position in the window's client pixels, the coordinates the
	// original's mouse-move handler keeps.
	if (device_ == nullptr) return;
	const platform::InputState state = device_->state();
	int32_t axis[2] = {};
	world::pass_through_close_axis(state.cursor_x, state.cursor_y, axis);
	close_axis_x_ = static_cast<float>(axis[0]);
	close_axis_y_ = static_cast<float>(axis[1]);
}

bool PlayerInputFrame::device_event(const platform::InputEvent &event) {
	switch (event.type) {
		case platform::InputEvent::Type::MouseWheel: {
			if (device_ == nullptr || !session_live()) return false;
			// The message's delta in WHEEL_DELTA units joins the notch remainder
			// (controls::WheelRemainder). Each notch is the wheel's event (256 up, 512 down),
			// whose row the binding walk takes as a button's (a held row's included) and the
			// next update's message pump queues as a record, ahead of that update's keyboard
			// pass, for its drain (queue_records; controls::WheelRemainder,
			// BindingSet::mouse_click_action and TickInputSource carry the witness).
			int notches = wheel_.feed(controls::wheel_delta_units(event.wheel));
			for (; notches > 0; --notches) queue_mouse_event_record(controls::kMouseWheelUpEvent);
			for (; notches < 0; ++notches) queue_mouse_event_record(controls::kMouseWheelDownEvent);
			return true;
		}
		case platform::InputEvent::Type::MouseButton: {
			// A mouse button's message is a click record its next update's pump queues, ahead of
			// that update's keyboard pass: the row its event takes, with the Shift sign or the
			// row's 0x200 bit; the click walk's head gate (the cine editor's open word) never
			// closes in play (controls::BindingSet::mouse_click_action carries the witness). The
			// button's held state stays the binding table's (the fire, the held rows).
			if (device_ == nullptr) return false;
			const uint32_t mouse_event = controls::mouse_button_event(event.button);
			// The left button's down (event 2, not a double click's 0x400) owes its PowerThrow
			// windup test to the next update's message pump.
			if (mouse_event == controls::kMouseLeftDownEvent) left_down_owed_ = true;
			if (mouse_event != 0) queue_mouse_event_record(mouse_event);
			return false;
		}
		case platform::InputEvent::Type::MouseMotion:
			// Mouse-look: the motion events add up the cursor offset the frame's head hands the
			// session in whole pixels; the session's mouse pump spreads it into the axis words
			// the sim's witnessed integer pipeline scales (the sim owns sensitivity, the scoped
			// zoom reduction, Y-invert and the pitch clamps; world/player_look.h MouseAxisPump).
			look_x_ += event.motion.dx;
			look_y_ += event.motion.dy;
			return true;
		default: return false;
	}
}

void PlayerInputFrame::queue_mouse_event_record(uint32_t event) {
	// The row the binding walk takes for the event and the record's sign
	// (controls::BindingSet::mouse_click_record carries the witness).
	const PlayerInputDevice::ClickRecord record = device_->mouse_click_record(event);
	if (record.code >= 0) pump_records_.push_back({record.code, 0, 0, 0, record.sign});
}

void PlayerInputFrame::reset() {
	actions_.reset();
	look_x_ = 0.0f;
	look_y_ = 0.0f;
	close_axis_x_ = 0.0f;
	close_axis_y_ = 0.0f;
	clear_queues();
	tick_look_x_ = 0.0f;
	tick_look_y_ = 0.0f;
}

} // namespace opennova::inmatch
