// The lock keys over the platform (D-NET-563): the platform input device's lock-key set
// (platform::InputDevice::set_lock_key, the null keyboard's toggle) and the engine's two callers,
// the process's forced locks (controls::ProcessKeyLocks, the frontend's boot leg its Num Lock) and
// the input frame's Scroll Lock turn-off (inmatch::PlatformInputDevice::scroll_lock_turn_off).
//
//   set       a lock's set toggles only where its word disagrees with the state wanted, and says
//             whether it toggled; each lock is its own word.
//   process   the command line's init saves Num Lock's word and forces it on (through
//             frontend::boot's leg, ahead of game.cfg's read); the subsystems' init turns Scroll
//             Lock off; the shutdown turns Scroll Lock off, puts Num Lock back and turns Caps Lock
//             off, once and only after the start; no keyboard reads and forces nothing.
//   turn-off  the input frame's device sends the Scroll Lock turn-off through the platform only
//             while the word reads on, and none without a platform device.
// [orig: Input_SetKeyState @0x760F60 - the byte against the state wanted @0x760FA6..0x760FB2;
//  Input_DisableScrollLock @0x7610F0 - the byte test @0x76111D; Game_ParseCommandLineAndInit
//  @0x4A7310 - Input_SetNumLockState(1) @0x4A7386; Game_InitSubsystems - Input_DisableScrollLock
//  @0x4A7260; Game_ShutdownSubsystems @0x4A5300 - @0x4A53B7, @0x4A53BC..0x4A53C3, @0x4A53CF]
#include "null_backend/null_platform.h"

#include <runtime/controls/binding_set.h>
#include <runtime/controls/key_locks.h>
#include <runtime/frontend/frontend.h>
#include <runtime/inmatch/platform_input_device.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
namespace nb = opennova::null_backend;
namespace pf = opennova::platform;

static int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

bool sets_are(const nb::NullInputDevice &input, const std::vector<nb::NullInputDevice::LockSet> &want) {
	const auto &got = input.lock_sets();
	if (got.size() != want.size()) return false;
	for (size_t i = 0; i < got.size(); ++i)
		if (got[i].key != want[i].key || got[i].on != want[i].on) return false;
	return true;
}

void set_leg() {
	nb::NullPlatform backend;
	pf::InputDevice &input = *backend.record().input;
	backend.input().set_lock_state(false, false, true);
	CHECK(!input.set_lock_key(pf::LockKey::ScrollLock, true)); // already on: nothing sent
	CHECK(input.set_lock_key(pf::LockKey::ScrollLock, false));
	CHECK(!input.state().scroll_lock);
	CHECK(!input.set_lock_key(pf::LockKey::ScrollLock, false)); // off now: nothing sent
	CHECK(input.set_lock_key(pf::LockKey::NumLock, true));
	CHECK(input.state().num_lock && !input.state().caps_lock);
	CHECK(sets_are(backend.input(), { { pf::LockKey::ScrollLock, false }, { pf::LockKey::NumLock, true } }));
}

void process_leg() {
	using LS = nb::NullInputDevice::LockSet;
	// A keyboard found with Num Lock off, Scroll Lock and Caps Lock on.
	{
		nb::NullPlatform backend;
		backend.input().set_lock_state(true, false, true);
		controls::ProcessKeyLocks locks(backend.record().input);
		std::vector<const char *> trace;
		frontend::BootLegs boot;
		boot.force_num_lock = [&] {
			trace.push_back("num_lock");
			locks.command_line_init();
		};
		boot.read_game_config = [&] {
			trace.push_back("game_cfg");
			// The read comes after the force: Num Lock is on by then.
			CHECK(backend.input().state().num_lock);
			return false;
		};
		CHECK(frontend::boot(boot));
		CHECK(trace.size() == 2 && std::string(trace[0]) == "num_lock");
		CHECK(locks.started() && !locks.saved_num_lock());
		CHECK(sets_are(backend.input(), { LS{ pf::LockKey::NumLock, true } }));
		locks.subsystems_init();
		CHECK(!backend.input().state().scroll_lock);
		locks.subsystems_shutdown();
		const pf::InputState end = backend.input().state();
		CHECK(!end.num_lock && !end.scroll_lock && !end.caps_lock);
		CHECK(sets_are(backend.input(),
				{ LS{ pf::LockKey::NumLock, true }, LS{ pf::LockKey::ScrollLock, false },
						LS{ pf::LockKey::NumLock, false }, LS{ pf::LockKey::CapsLock, false } }));
		// Once: a second shutdown forces nothing.
		locks.subsystems_shutdown();
		CHECK(backend.input().lock_sets().size() == 4);
	}
	// Num Lock found on: forced on sends nothing, and the shutdown leaves it on.
	{
		nb::NullPlatform backend;
		backend.input().set_lock_state(false, true, false);
		controls::ProcessKeyLocks locks(backend.record().input);
		locks.command_line_init();
		CHECK(locks.saved_num_lock());
		locks.subsystems_init();
		locks.subsystems_shutdown();
		CHECK(backend.input().lock_sets().empty() && backend.input().state().num_lock);
	}
	// No start, no shutdown: the locks stay as they stand.
	{
		nb::NullPlatform backend;
		backend.input().set_lock_state(true, false, true);
		controls::ProcessKeyLocks locks(backend.record().input);
		locks.subsystems_shutdown();
		CHECK(backend.input().lock_sets().empty());
	}
	// No keyboard: nothing read or forced.
	{
		controls::ProcessKeyLocks locks(nullptr);
		locks.command_line_init();
		locks.subsystems_init();
		locks.subsystems_shutdown();
		CHECK(!locks.saved_num_lock());
	}
}

void turn_off_leg() {
	nb::NullPlatform backend;
	backend.input().set_lock_state(false, false, true);
	controls::BindingSet bindings;
	inmatch::PlatformInputDevice device(bindings, backend.record().input);
	CHECK(device.scroll_lock_turn_off());
	CHECK(!backend.input().state().scroll_lock);
	CHECK(!device.scroll_lock_turn_off()); // the word reads off: nothing sent
	CHECK(backend.input().lock_sets().size() == 1);
	inmatch::PlatformInputDevice none(bindings, nullptr);
	CHECK(!none.scroll_lock_turn_off());
}

} // namespace

int main() {
	set_leg();
	process_leg();
	turn_off_leg();
	if (failures != 0) {
		std::printf("null_backend_key_locks: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("null_backend_key_locks: ok");
	return 0;
}
