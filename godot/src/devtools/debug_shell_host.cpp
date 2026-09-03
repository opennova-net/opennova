#include "devtools/debug_shell_host.h"

namespace godot {

GameWorld *DebugShellHost::world() {
	GameWorld *out = nullptr;
	if (GDVIRTUAL_CALL(_world, out)) {
		return out;
	}
	return nullptr;
}

MissionRoot *DebugShellHost::runtime() {
	MissionRoot *out = nullptr;
	if (GDVIRTUAL_CALL(_runtime, out)) {
		return out;
	}
	return nullptr;
}

LocalPlayerPresenter *DebugShellHost::player_presenter() {
	LocalPlayerPresenter *out = nullptr;
	if (GDVIRTUAL_CALL(_player_presenter, out)) {
		return out;
	}
	return nullptr;
}

Viewport *DebugShellHost::viewport() {
	Viewport *out = nullptr;
	if (GDVIRTUAL_CALL(_viewport, out)) {
		return out;
	}
	return nullptr;
}

Error DebugShellHost::return_to_menu() {
	Error out = ERR_UNAVAILABLE;
	if (GDVIRTUAL_CALL(_return_to_menu, out)) {
		return out;
	}
	return ERR_UNAVAILABLE;
}

bool DebugShellHost::has_debug_authority() {
	bool out = false;
	if (GDVIRTUAL_CALL(_has_debug_authority, out)) {
		return out;
	}
	return false;
}

void DebugShellHost::_bind_methods() {
	GDVIRTUAL_BIND(_world);
	GDVIRTUAL_BIND(_runtime);
	GDVIRTUAL_BIND(_player_presenter);
	GDVIRTUAL_BIND(_viewport);
	GDVIRTUAL_BIND(_return_to_menu);
	GDVIRTUAL_BIND(_has_debug_authority);
	ClassDB::bind_method(D_METHOD("world"), &DebugShellHost::world);
	ClassDB::bind_method(D_METHOD("runtime"), &DebugShellHost::runtime);
	ClassDB::bind_method(D_METHOD("player_presenter"), &DebugShellHost::player_presenter);
	ClassDB::bind_method(D_METHOD("viewport"), &DebugShellHost::viewport);
	ClassDB::bind_method(D_METHOD("return_to_menu"), &DebugShellHost::return_to_menu);
	ClassDB::bind_method(D_METHOD("has_debug_authority"), &DebugShellHost::has_debug_authority);
}

} // namespace godot
