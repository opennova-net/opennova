#include "simulation/load_warning_box.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/os.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace godot;

opennova::mission::LoadWarningAnswer load_warning_box(
		const std::string &p_caption, const std::string &p_text) {
	using opennova::mission::LoadWarningAnswer;
	DisplayServer *display = DisplayServer::get_singleton();
	if (display == nullptr || display->get_name() == "headless")
		return LoadWarningAnswer::Continue;
	Input *input = Input::get_singleton();
	const Input::MouseMode mode =
			input != nullptr ? input->get_mouse_mode() : Input::MOUSE_MODE_VISIBLE;
	if (input != nullptr)
		input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
#ifdef _WIN32
	// The engine's mission::LoadWarningBox carries the box's witness (its type and its answers).
	const HWND window =
			reinterpret_cast<HWND>(display->window_get_native_handle(DisplayServer::WINDOW_HANDLE));
	const bool ok = MessageBoxA(window, p_text.c_str(), p_caption.c_str(),
							MB_OKCANCEL | MB_ICONEXCLAMATION) == IDOK;
#else
	// No OK/Cancel box off Windows: the alert's one button carries the load on.
	OS::get_singleton()->alert(String::utf8(p_text.c_str()), String::utf8(p_caption.c_str()));
	const bool ok = true;
#endif
	if (input != nullptr)
		input->set_mouse_mode(mode);
	return ok ? LoadWarningAnswer::Continue : LoadWarningAnswer::Quit;
}
