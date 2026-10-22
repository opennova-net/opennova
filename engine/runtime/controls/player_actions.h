#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <runtime/controls/input_event_queue.h>
#include <runtime/controls/key_event_queue.h>

namespace opennova::controls {

// The embedder resolves bindings against its devices. A keyboard match returns
// its VK; a mouse/joystick match has no keyboard VK. Digits are physical keys,
// independent of remapping and modifier claims.
class PlayerActionSource {
public:
	virtual ~PlayerActionSource() = default;
	virtual bool pressed(const char *token) const = 0;
	virtual int pressed_key(const char *token) const = 0;
	virtual bool digit_down(int digit) const = 0;
	// The sign bit the fire of the token's row carries: Shift held on a
	// modifier-less key's fire (the fallback pass) or a held mouse button,
	// never a fire through the row's held modifier (the modifier pass) or a
	// joystick button (controls::BindingSet::fire_signed).
	// [orig: Input_ProcessKeyboardEvents @0x49d452..0x49d470 (the fallback's
	//  sign) vs @0x49d41e..0x49d42f (the modifier pass, unsigned);
	//  Input_TryDispatchBindingByWeaponType @0x499180..0x4991a4;
	//  Input_TryTriggerJoystickButton @0x497B7A / @0x497BA9 (unsigned)]
	virtual bool fire_signed(const char *token) const = 0;
	// Whether a key bound to the token's row is down by the key state alone:
	// no scan gate, no keyboard capture, a special-key take included. The
	// ToSpecial handler reads its keys raw to tell its press from its release.
	// [orig: Input_HandleActionBinding_0 case 0xDC @0x4E1161..0x4E1181]
	virtual bool keys_down(const char *token) const { return pressed(token); }
	// The token held by an arm the key queue does not carry: the mouse buttons,
	// the joystick and the scripted device's hold. A keyboard key fires an event
	// row once per queued key event (PlayerActionPoll::key_events); these arms
	// keep their press edges. A source with no key queue reads every arm here.
	virtual bool held_off_keyboard(const char *token) const { return pressed(token); }
};

enum class PlayerAction {
	ToggleMount,
	SelectSeat,
	ToggleScope,
	WeaponCategory,
	WeaponCycle,
	Stance,
	ScopeZero,
	RadarZoom,
	MapCycle,
	Binoculars,
	NightVision,
	NvgGain,
	WaypointCycle, // value -1 = backward (the Shift direction bit)
	// The death screen's spectator rows (value = the dispatch code 500 / 501
	// / 502; replication::ClientReplicaPipeline::spectate_action).
	Spectate,
	// A digit while the Emotes / Radio menu is open: value 1..10 (the 0 key
	// is 10). The embedder sends it (C2S 0x14 / C2S 0x13) and closes that
	// menu (hud::hud_toggles_close_voice_menu).
	EmotePick,
	RadioPick,
	// The USE-ITEM armory arm's open (weapon.mnu WEAPON): the embedder's shell
	// opens the screen.
	OpenArmory,
	// The USE-ITEM vehicle-loadout arm's open (vehicle.mnu VEHICLE): the
	// embedder's shell opens the screen over inmatch::VehicleMenu.
	OpenVehicleMenu,
	// The Respawn row (catalog row 68, Ctrl+R, code 55) and the medic call (row 64
	// MedicReq, code 217): records whose arms the role owns (inmatch::Role::dispatch_record:
	// request_respawn, request_medic), past their own head gates there.
	Respawn,
	MedicRequest,
	// A key event of a row whose arm the embedder owns (PlayerActionPoll::embedder_row:
	// the talk, HUD and pause rows): its record, in key order with the action rows'.
	EmbedderRow,
};

// The arm a frame's held USE takes, as the embedder's press latch decides it
// (world::LocalPlayer::take_use_item_press): the mount chain's hold, the
// armory's open, the vehicle screen's open (a vehicle-loadout volume whose
// ground's team byte is 0 or the player's), or a press the arm takes with
// nothing to open (a latched or refused armory press, a vehicle bay of another
// team).
// [orig: Input_HandleActionBinding_0 case 0xB1 -- the team test
//  @0x4E0AD8..0x4E0AEB, UI_OpenMenuScreen("vehicle.mnu", "VEHICLE") @0x4E0AFD]
enum class UseItemPressArm : uint8_t { kMountChain, kOpenArmory, kOpenVehicleMenu, kTaken };

struct PlayerActionRequest {
	PlayerAction action = PlayerAction::ToggleMount;
	int value = 0;
	// The dispatch code of the binding row whose queued event carries the request,
	// which the dispatcher's head gates by that row's flags before the arm runs
	// (inmatch::Role::action_head_admits); -1 for an arm the input frame calls
	// directly, past no head: the held-USE digit's seat pick, the USE release's
	// mount toggle, the Emotes / Radio menus' digit picks.
	// [orig: Input_ProcessKeyboardEvents -> Input_QueueEvent -> Input_HandleActionBinding
	//  @0x49AD40 (the head @0x49AD8D..0x49AE23); Input_HandleSpecialKeys @0x49c5c0 -
	//  the seat index @0x49c6e6..0x49c6ed, Entity_FindAvailableSeat (the call @0x49c702), the
	//  menu picks @0x49c745 / @0x49c791;
	//  Input_ProcessFrame @0x49d520 - the release edge @0x49d6c1..0x49d6dc]
	// A request with a code is a record of the input frame's event queue: the embedder
	// queues it (InputEventRecord: the code, the arguments 0, the sign) and the update's
	// drain dispatches it in queue order (inmatch::Role::drain_input_events); one with
	// none runs at once.
	int code = -1;
	// The sign bit its record carries: the fire's Shift sign (PlayerActionSource::fire_signed;
	// KeyRowEvent::sign), which NextWaypoint reads.
	bool sign = false;
};

// The request a drained record of an action row names: the row's request by its code,
// NextWaypoint's direction from the record's sign. None for a code no action row carries.
// [orig: Input_HandleActionBinding @0x49AD40 (the switch by the record's code; case 23
//  @0x49B3DE..0x49B44D reads the sign); Input_HandleActionBinding_0 @0x4E0420]
std::optional<PlayerActionRequest> player_action_request(const InputEventRecord &record);

struct PlayerActionPoll {
	bool active = false;
	bool captured = false;
	bool simulation_available = false;
	// An open text line owns the keyboard (BindingSet::set_keyboard_captured):
	// the held-USE digits go to the line, not the seats.
	bool keyboard_captured = false;
	// The F9 Emotes / F10 Radio menus' open words as the HUD toggles hold them
	// this frame (hud::HudToggleState): a digit goes to the open menu.
	bool emotes_menu_open = false;
	bool radio_menu_open = false;
	// The death screen is up (g_DeathScreenActive): the binding scan admits
	// only the rows whose mode word carries bit 2, so the fire, reload and
	// medic rows and the weapon/seat/scope rows stay silent
	// [orig: Input_IsBindingActiveForMode @0x497ea0].
	bool death_screen = false;
	// A menu screen (INGAME, WEAPON, CMAP, VEHICLE, or DEATH with its exit
	// confirmation's word down) is up: retail runs no input frame under it, so
	// the poll neither ages a latch nor dispatches; DEATH and STAT under their
	// pass-through word run the whole frame (world::pass_through_close_axis
	// carries the key witness). A USE hold
	// latched when the menu opened therefore ends on the first frame after
	// the close, whose key-state reset reads the key up: the release edge's
	// mount toggle runs then, unless a seat pick consumed the hold.
	// [orig: Game_ProcessMainFrame -- the menu branch @0x52660D, the input
	//  frame only in the no-menu branch @0x52661D; Input_ProcessFrame's latch
	//  aging @0x49D57F..0x49D585 and release edge @0x49D6C1..0x49D6DC]
	bool menu_open = false;
	// The Scroll Lock word the device reads every input frame: the keyboard state's VK_SCROLL
	// byte, nonzero when the toggle is on or the key is held (retail's auto-run, D-NET-563).
	// [orig: Input_IsScrollLockOn @0x760E10 - GetKeyboardState @0x760E28, `KeyState[145] != 0`
	//  @0x760E57]
	bool scroll_lock_on = false;
	// The held USE row's arm, asked once on every frame either of its keys is
	// down: the row is a held row (flags 0x0C000805, bit 4), which the analog
	// pass queues each frame, so case 0xB1 runs each frame the key is held.
	// Unset = the mount chain.
	// [orig: Input_ProcessAnalogBindings @0x4995F0, run from
	//  Input_ProcessPlayerFrame @0x49D50C]
	std::function<UseItemPressArm()> take_use_item_arm;
	// The keyboard pass's events for the action rows since the last poll, in their
	// queue order: each scanned key-down (an OS auto-repeat included) fires its row
	// once, and the release pass's events dispatch ToSpecial's release. Each queued
	// event is its own record (no merge of equal ones), and the drain dispatches every
	// record in order; the keyboard pass runs ahead of the mouse and toggle passes, so
	// its events dispatch first. (The embedder routes the talk and HUD rows' events
	// to its update's drain on the same rule.) A digit's event is the special-key
	// chain's while the USE hold or an Emotes / Radio menu holds it, so no row fires
	// for it. The rows' off-keyboard arms keep their press edges
	// (PlayerActionSource::held_off_keyboard).
	// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the release pass @0x49d249..
	//  0x49d2b9, the press scan's queue @0x49d42f / @0x49d488; Input_QueueEvent
	//  @0x4979C0; Input_HandleSpecialKeys @0x49c5c0 - the digit tests @0x49c6e0 /
	//  @0x49c73f / @0x49c78b; Input_ProcessFrame @0x49d520 - the drain's dispatch
	//  @0x49d5d8; Input_ProcessPlayerFrame @0x49d4c0 - the keyboard pass @0x49d4c6
	//  ahead of the mouse and toggle passes @0x49d4d8 / @0x49d4de]
	std::vector<KeyRowEvent> key_events;
	// The codes of the rows among key_events whose arms the embedder owns (its talk, HUD
	// and pause rows): each such key-down is a record in key order with the action rows'
	// (PlayerAction::EmbedderRow), under none of this table's gates (the rows' own arms and
	// the dispatcher's head gate them at the drain), a digit the special-key chain took
	// excepted. Unset = none.
	// [orig: Input_ProcessKeyboardEvents @0x49D1F0 (Input_QueueEvent per scanned key-down
	//  @0x49D42F / @0x49D488); Input_ProcessFrame @0x49D520 (the drain @0x49D599..0x49D5E7)]
	std::function<bool(int code)> embedder_row;
};

struct PlayerActionFrame {
	bool fire_held = false;
	bool fire_edge = false;
	// The reload and ToSpecial rows reach the session as one edge per sample. Retail
	// dispatches every record, but a second one in the same input frame changes nothing:
	// the reload request takes only a next action of 0, 1 or 11 and stores the reload's 4,
	// and ToSpecial's second press or release is a no-op. These edges dispatch in the
	// tick's input prologue behind the drained records, so their order against the action
	// rows' records is D-CTRL-6's next row. The Respawn and MedicReq rows are records in
	// `requests` (PlayerAction::Respawn / MedicRequest), one per event in key order: the
	// Respawn case's Player_ResetLocalInput memsets the event queue ahead of its 0x41 (a
	// local entity with a def), which ends the drain, and the medic call's cooldown latches.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 (case 0xD3 -> WeaponSlot_RequestReload
	//  @0x53F110, the next action's test @0x53F11E..0x53F12D, the store @0x53F12F);
	//  Input_HandleActionBinding @0x49AD40 (case 55: Player_ResetLocalInput @0x49B20B ahead of
	//  NetPacket_SendRequestRespawn @0x49B22A); Player_ResetLocalInput @0x4B2B80 (the entity
	//  and def tests @0x4B2B85..0x4B2B8D, Input_ResetAllState @0x4B2B99)]
	bool reload_edge = false;
	// ToSpecial (catalog row 37, default F, dispatch 220): its keys' live state
	// and a dispatch: its scanned key-down's event (a press, an OS repeat
	// included), the release pass's event (the row's flags carry the release bit
	// 0x80000000), or an edge of its off-keyboard arm; the handler tells a press
	// from a release by the keys' state. Two in one input frame run one dispatch:
	// the handler's second press or second release changes nothing.
	// [orig: Input_ProcessKeyboardEvents @0x49D249..0x49D2B9 (release) / @0x49D42F
	//  (press); row flags 0x8C000801; Input_HandleActionBinding_0 case 0xDC
	//  @0x4E1161..0x4E1181]
	bool to_special_held = false;
	bool to_special_edge = false;
	// Retail's auto-run (D-NET-563): this frame's forward event (code 152 with its fourth
	// argument 1, which a forward key's own event carries as 0), queued every input frame the
	// Scroll Lock word reads on, and by the held Last_move row (catalog row 16, code 425,
	// default key Scroll Lock), whose case queues the same event. The embedder ORs it into the
	// forward key; it rides the forward key's path from there (the input flags' forward bit).
	// [orig: Input_ProcessPlayerFrame @0x49D4C0 - Input_IsScrollLockOn (the call @0x49D4E6) ->
	//  Input_QueueEvent(152, 0, 1, 1) @0x49D4FA, ahead of the capture-mode test @0x49D509;
	//  Input_HandleActionBinding_0 @0x4E0420 - code 425 @0x4E046F -> Input_QueueEvent(152, 0, 0,
	//  1) @0x4E13E3..0x4E13ED; case 152 @0x4E0C85 - the fourth argument's test @0x4E0C92, the
	//  forward bit `g_InputFlags |= 2` @0x4E0CBE]
	bool auto_run_forward = false;
	// The dispatch codes whose cases turn the Scroll Lock toggle off this frame
	// (Input_DisableScrollLock), each through the dispatcher's head by its own code; -1 is the
	// USE release tail, past no head. A held forward key's own event (its fourth argument 0),
	// the back and strafe keys, the held USE row and the Respawn press; the embedder's device
	// turns the toggle off. The case's 0x20 gate on dword_24C1930 is dead (no writer sets the
	// bit: Game_StartMission's stores @0x525B25 / @0x525B5C / @0x525C3B and the 0x400 / 0x100 /
	// 0x800 toggles @0x49B937 / @0x4DC9C5 / @0x4E07A4 are its only writes).
	// The device's turn-off sends a Scroll Lock press and release (VK and scan 0x91, the
	// release with KEYEVENTF_KEYUP and time 10) only when the keyboard state's byte is nonzero,
	// then reads the window's messages; the process's init and shutdown turn it off too.
	// [orig: Input_HandleActionBinding_0 - case 151 @0x4E0C66, case 152 @0x4E0C9E, case 156
	//  @0x4E1021, case 157 @0x4E105E, case 0xB1 @0x4E0A84; Input_HandleActionBinding case 55
	//  @0x49B20B -> Player_ResetLocalInput @0x4B2B80 (the call @0x4B2B91); Input_ProcessFrame -
	//  the USE release tail @0x49D6C5; Input_DisableScrollLock @0x7610F0 - the byte test
	//  @0x761116..0x76111D, the inputs @0x76111F..0x761166, SendInput @0x76116E, the pump
	//  @0x76117A; Game_InitSubsystems (the call @0x4A7260); Game_ShutdownSubsystems (the call
	//  @0x4A53B7)]
	std::vector<int> scroll_lock_off;
	// In order: the USE special-key / release arms (no code: they run at once), then the
	// action rows' records (a code: the embedder queues them for the update's drain), the
	// keyboard pass's events first, then the off-keyboard arms' press edges.
	std::vector<PlayerActionRequest> requests;
};

// The WM_MOUSEWHEEL accumulator in front of the wheel binding rows: each
// message's signed delta (WHEEL_DELTA 120 per notch, positive away from the
// user) joins a persistent remainder, and every whole +120 dispatches one
// wheel-up event (the 0x400 binding mask) while every whole -120 dispatches one
// wheel-down event (0x800); the rest carries into the next message.
// [orig: Input_DispatchMouseEvent @ 0x761470 -- `sar eax,10h` / `add eax,
//  g_MouseState.wheelRemainder` @ 0x7614AB..0x7614AE, the +120 walk firing
//  event 0x100 @ 0x761575..0x7615AA, the -120 walk firing 0x200
//  @ 0x7615AC..0x7615DB; the events map to masks 0x400 / 0x800 in
//  Input_TryDispatchBindingByWeaponType @ 0x4992FC / @ 0x499311]
inline constexpr int32_t kWheelDelta = 120;
class WheelRemainder {
public:
	// Returns the signed whole-notch count this delta completes.
	int feed(int32_t delta);

private:
	int32_t remainder_ = 0;
};

// The keyboard locks the process forces, the embedder's device work. The command line's init
// saves Num Lock's state and forces it on; the subsystems' init, after game.cfg's load (whose
// mpreset exit leaves by crt_exit, running no shutdown), turns Scroll Lock off; the
// subsystems' shutdown at the main loop's end, after the cursor's show, turns Scroll Lock off,
// puts Num Lock back as the start found it and turns Caps Lock off. A forced state is a press
// and release of the key (VK and scan alike, the release KEYEVENTF_KEYUP with time 10) sent
// only when the keyboard state's byte for it, nonzero while the toggle is on or the key is
// held, disagrees with the state wanted; the window's messages are read after it. The start's
// Scroll Lock and Caps Lock reads reach no reader: the exit pushes them as arguments its
// callees ignore. The Escape key's Player_ResetLocalInput turns Scroll Lock off too
// (inmatch::Role::reset_local_input's return; PlayerActionFrame::scroll_lock_off carries the
// device's turn-off).
// [orig: WinMain @0x763A80 (Game_ParseCommandLineAndInit @0x763B5B, then Game_Run @0x763BA9);
//  Game_ParseCommandLineAndInit @0x4A7310 (Input_IsScrollLockOn @0x4A7362 into dword_B49844
//  @0x4A7367; Input_IsCapsLockOn @0x4A736C; Input_IsNumLockOn @0x4A7376 into
//  g_SavedNumLockState @0x4A7381; Input_SetNumLockState(1) @0x4A7386); Game_Run @0x4A7FB0
//  (Game_LoadConfig @0x4A7FBB, Game_InitSubsystems @0x4A7FD5, Game_SaveConfig @0x4A7FFF,
//  Game_ShutdownSubsystems @0x4A8004); Game_LoadConfig @0x551480 (mpreset's crt_exit
//  @0x5514A1..0x5514AC); Game_InitSubsystems
//  @0x4A6CD0 (Input_DisableScrollLock @0x4A7260); Game_ShutdownSubsystems @0x4A5300
//  (Cursor_Show @0x4A53A2, dword_B49844 pushed @0x4A53A7..0x4A53AC,
//  Input_DisableScrollLock @0x4A53B7, Input_SetNumLockState(g_SavedNumLockState)
//  @0x4A53BC..0x4A53C3, Input_ReleaseCapsLockIfActive @0x4A53CF); Input_IsNumLockOn @0x760ED0
//  (the byte compare @0x760F10); Input_SetNumLockState @0x761240 (Input_SetKeyState(0x90, state)
//  @0x76124A); Input_SetKeyState @0x760F60 (the byte against the state wanted
//  @0x760FA6..0x760FB2, SendInput @0x761002, the pump @0x761009);
//  Input_ReleaseCapsLockIfActive @0x7611A0 (the VK_CAPITAL byte compare @0x7611C6, SendInput
//  @0x76121B, the pump @0x761224); Input_DisableScrollLock @0x7610F0 (no read of its argument)]
inline constexpr int kVkCapital = 0x14;
inline constexpr int kVkNumLock = 0x90;
inline constexpr int kVkScroll = 0x91;

// Owns the action table's gates and latches across frames and mission rebuilds.
// A host without a player (including spectator mode) does not poll this state.
// Request refusal and all gameplay effects belong to the simulation.
class PlayerActions {
public:
	PlayerActions();
	PlayerActionFrame poll(const PlayerActionSource &source, const PlayerActionPoll &gate);
	// The presenter reset clears trigger, stance and scope-zero edges only;
	// switch/HUD/medic/USE state survives it, just as it survives host teardown.
	void reset();
	// Deferred until after the next poll's fresh-USE-press reset.
	void consume_use_hold();
	// The USE hold the special-key chain's held-USE arm reads: the hold the last poll's USE
	// row latched, which the next input frame's head ages into the word its keyboard pass
	// reads, so a key the embedder's chain takes between two polls meets it. While it
	// holds, the arm takes a digit (the seat pick, poll()) and passes every other key to
	// the chain's tail: the Emotes, Radio and round-over arms, the Tab board's page keys
	// and the status page's keys never run for it.
	// [orig: Input_ProcessFrame @0x49d520 - g_UseItemHoldPrev = g_UseItemHoldLatch
	//  @0x49d57f, the latch cleared @0x49d585, ahead of the keyboard pass
	//  (Input_ProcessPlayerFrame @0x49d58b); Input_HandleActionBinding_0 case 0xB1's
	//  latch @0x4e0b71; Input_HandleSpecialKeys @0x49c5c0 - the held-USE arm's test
	//  @0x49c6d8 (its clear word on to the Emotes arm @0x49c731), a non-digit's jump to
	//  the tail @0x49c6e0 -> @0x49c9c9]
	bool use_held() const { return use_latched_; }
	// The death screen's spectator rows, the catalog's mode-2 rows 110..112
	// (CycleSpectatorMode, IncSpectatorTarget, DecSpectatorTarget): the binding
	// scan admits them only while the death screen is up, so the embedder
	// polls them there while gameplay input is active: one request per queued
	// key event in `events` (the poll's key_events), in their order, then the
	// press edges of the rows' off-keyboard arms.
	// [orig: Input_IsBindingActiveForMode @0x497ea0 — row +8 bit 2 on the
	//  death screen; rows @0x818810 / @0x81887C / @0x8188E8 -> codes 500..502]
	std::vector<PlayerActionRequest> poll_spectator(const PlayerActionSource &source, bool active,
			const std::vector<KeyRowEvent> &events = {});

private:
	struct RowLatch { bool down = false; };
	std::vector<RowLatch> rows_;
	bool fire_was_held_ = false;
	bool reload_was_down_ = false;
	bool medic_was_down_ = false;
	bool respawn_was_down_ = false;
	// ToSpecial's off-keyboard arm, whose edges dispatch both ways.
	bool to_special_off_down_ = false;
	bool use_latched_ = false;
	bool use_held_prev_ = false;
	bool use_hold_consumed_ = false;
	bool use_consume_pending_ = false;
	bool use_digit_was_down_[10] = {};
	// This frame's menu digit arm: a menu was open while gameplay input ran.
	bool menu_digits_ = false;
	bool spectator_was_down_[3] = {};

	void sample_use(const PlayerActionSource &source, const PlayerActionPoll &gate,
			PlayerActionFrame &frame);
	// A key event the special-key chain took ahead of the binding scan: a digit
	// while the USE hold (the hold the last poll latched) or an open Emotes / Radio
	// menu holds the chain, whose arm picks the seat or the menu entry from the
	// digit's key state (sample_use), so no row fires for its event. The menus are
	// read as the sample found them: retail's arm runs per dequeued key and its pick
	// closes the menu at once, so a second digit in the same input frame reaches the
	// scan (D-CTRL-6's next row: the digit arms per key event in the keyboard pass)
	// [orig: Input_HandleSpecialKeys @0x49C5C0 - the Emotes pick's send @0x49C752 and
	//  the word's clear @0x49C75C, the Radio pick's @0x49C79E / @0x49C7A8].
	bool chain_takes_key(const KeyRowEvent &event) const;
};

// Only the seven currently routed wheel actions. The host admits a pressed
// wheel event with active gameplay, a player and a simulation; wheel events
// neither require mouse capture nor advance the polled key latches.
std::optional<PlayerActionRequest> player_wheel_action(std::string_view token);

} // namespace opennova::controls
