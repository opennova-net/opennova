#include "player/gameplay_camera.h"

using namespace godot;

void GameplayCamera::set_gameplay_locked(bool p_locked) {
	GDVIRTUAL_CALL(_set_gameplay_locked, p_locked);
}

void GameplayCamera::_bind_methods() {
	GDVIRTUAL_BIND(_set_gameplay_locked, "locked");
	ClassDB::bind_method(D_METHOD("set_gameplay_locked", "locked"), &GameplayCamera::set_gameplay_locked);
}
