#include "player/gameplay_camera.h"

using namespace godot;

void GameplayCamera::set_gameplay_locked(bool p_locked) {
	GDVIRTUAL_CALL(_set_gameplay_locked, p_locked);
}

void GameplayCamera::set_spectator_mode(bool p_active) {
	GDVIRTUAL_CALL(_set_spectator_mode, p_active);
}

void GameplayCamera::_bind_methods() {
	GDVIRTUAL_BIND(_set_gameplay_locked, "locked");
	GDVIRTUAL_BIND(_set_spectator_mode, "active");
	ClassDB::bind_method(D_METHOD("set_gameplay_locked", "locked"), &GameplayCamera::set_gameplay_locked);
	ClassDB::bind_method(D_METHOD("set_spectator_mode", "active"), &GameplayCamera::set_spectator_mode);
}
