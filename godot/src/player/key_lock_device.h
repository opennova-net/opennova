#pragma once

// The keyboard-lock device: the Windows keyboard state's lock bytes and the forced lock
// state retail sends with SendInput (D-NET-563, D-NET-NEW-LI-9; engine
// controls::PlayerActionPoll::scroll_lock_on, controls::kVkNumLock and
// hud::chat_entry_event::kCapsLockRelease carry the witnesses). A platform without a
// keyboard state, or a run with no keyboard (the headless display), reads every lock off
// and forces none.

namespace godot::key_lock_device {

// The key's keyboard-state byte is nonzero: its toggle is on or the key is held.
bool on(int p_vk);
// A press and release of the key (VK and scan alike, the release KEYEVENTF_KEYUP with time
// 10) when that byte disagrees with `p_on`. True when it sent them.
bool set(int p_vk, bool p_on);

} // namespace godot::key_lock_device
