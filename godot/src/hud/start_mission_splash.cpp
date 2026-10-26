#include "hud/start_mission_splash.h"

namespace godot {

void StartMissionSplash::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_state"), &StartMissionSplash::get_state);
	ClassDB::bind_method(D_METHOD("raise"), &StartMissionSplash::raise);
	ClassDB::bind_method(D_METHOD("exit", "presents"), &StartMissionSplash::exit);
	ClassDB::bind_method(D_METHOD("frame", "mouse_mask", "presents"), &StartMissionSplash::frame);
	ClassDB::bind_method(D_METHOD("draws_overlay"), &StartMissionSplash::draws_overlay);
	BIND_ENUM_CONSTANT(STATE_NONE);
	BIND_ENUM_CONSTANT(STATE_ACTIVE);
	BIND_ENUM_CONSTANT(STATE_CLOSING);
}

} // namespace godot
