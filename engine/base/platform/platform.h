#pragma once

#include <base/platform/audio_device.h>
#include <base/platform/clock.h>
#include <base/platform/files.h>
#include <base/platform/input_device.h>
#include <base/platform/os.h>
#include <base/platform/window.h>

// The platform the app hands the engine (ADR 0077 d1, d2): one backend's devices, each an
// interface of this directory, owned by the backend and outliving every engine object that holds
// this record. The engine owns everything above them (the loop, the input frame, the mix, the
// game flow); a backend implements them and nothing else: SDL3 (backends/sdl3, apps/game), the
// headless test backend (tests/null_backend), Godot's bindings while the Godot build ships.
//
// The SDL3 backend (backends/sdl3) builds this record and opennova-game (apps/game) takes it, as
// tests/null_backend's headless backend does; the R3 engine frontend (runtime/frontend) takes it
// over from the app shell.
//
// The design follows sokol_app's and SDL3's: one platform per process, its window, input, time,
// files, OS services and audio reached through it.
namespace opennova::platform {

struct Platform {
	// Always present.
	Clock *clock = nullptr;
	Files *files = nullptr;
	Os *os = nullptr;
	// Null where the process has none: a headless host (opennova-serve) has no window, no input
	// device and no audio output.
	Window *window = nullptr;
	InputDevice *input = nullptr;
	AudioDevice *audio = nullptr;
};

} // namespace opennova::platform
