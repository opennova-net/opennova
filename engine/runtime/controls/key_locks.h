#pragma once

#include <base/platform/input_device.h>

// The keyboard locks the process forces, over the platform's input device
// (platform::InputDevice::set_lock_key: a forced state is the key's press and release, sent only
// where the keyboard state's word disagrees with the state wanted). The command line's init saves
// Num Lock's state and forces it on (the engine frontend's boot leg, frontend::BootLegs::
// force_num_lock); the subsystems' init, past game.cfg's read, turns Scroll Lock off; the
// subsystems' shutdown at the main loop's end turns Scroll Lock off, puts Num Lock back as the
// start found it and turns Caps Lock off. The start's Scroll Lock and Caps Lock reads reach no
// reader, so none is kept. controls::kVkNumLock (player_actions.h) carries the whole witness,
// D-NET-563; the Escape key's turn-off and the binding cases' are the input frame's
// (inmatch::PlayerInputDevice::scroll_lock_turn_off).
// [orig: Game_ParseCommandLineAndInit @0x4A7310 - Input_IsNumLockOn @0x4A7376 into
//  g_SavedNumLockState @0x4A7381, Input_SetNumLockState(1) @0x4A7386; Game_InitSubsystems
//  @0x4A6CD0 - Input_DisableScrollLock @0x4A7260; Game_ShutdownSubsystems @0x4A5300 -
//  Input_DisableScrollLock @0x4A53B7, Input_SetNumLockState(g_SavedNumLockState)
//  @0x4A53BC..0x4A53C3, Input_ReleaseCapsLockIfActive @0x4A53CF; Input_SetKeyState @0x760F60 -
//  the byte against the state wanted @0x760FA6..0x760FB2, SendInput @0x761002]
namespace opennova::controls {

class ProcessKeyLocks {
public:
	// `input` null: no keyboard (a headless process), nothing read or forced.
	explicit ProcessKeyLocks(platform::InputDevice *input) : input_(input) {}

	// The command line's init: Num Lock's state saved, then the lock forced on.
	void command_line_init() {
		started_ = true;
		saved_num_lock_ = input_ != nullptr && input_->state().num_lock;
		if (input_ != nullptr) (void)input_->set_lock_key(platform::LockKey::NumLock, true);
	}
	// The subsystems' init: Scroll Lock off, ahead of any input frame.
	void subsystems_init() {
		if (input_ != nullptr) (void)input_->set_lock_key(platform::LockKey::ScrollLock, false);
	}
	// The subsystems' shutdown, once the start ran: Scroll Lock off, Num Lock as the start found
	// it, Caps Lock off.
	void subsystems_shutdown() {
		if (!started_) return;
		started_ = false;
		if (input_ == nullptr) return;
		(void)input_->set_lock_key(platform::LockKey::ScrollLock, false);
		(void)input_->set_lock_key(platform::LockKey::NumLock, saved_num_lock_);
		(void)input_->set_lock_key(platform::LockKey::CapsLock, false);
	}

	bool started() const { return started_; }
	// g_SavedNumLockState: Num Lock's word the start found.
	bool saved_num_lock() const { return saved_num_lock_; }

private:
	platform::InputDevice *input_ = nullptr;
	bool started_ = false;
	bool saved_num_lock_ = false;
};

} // namespace opennova::controls
