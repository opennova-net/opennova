#include "world/post_mission_route.h"

using namespace godot;

void PostMissionRoute::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_keep_session"), &PostMissionRoute::get_keep_session);
	ClassDB::bind_method(D_METHOD("set_keep_session", "value"), &PostMissionRoute::set_keep_session);
	ClassDB::bind_method(D_METHOD("get_error"), &PostMissionRoute::get_error);
	ClassDB::bind_method(D_METHOD("set_error", "value"), &PostMissionRoute::set_error);
	ClassDB::bind_method(D_METHOD("get_error_key"), &PostMissionRoute::get_error_key);
	ClassDB::bind_method(D_METHOD("set_error_key", "value"), &PostMissionRoute::set_error_key);
	ClassDB::bind_method(D_METHOD("get_error_text"), &PostMissionRoute::get_error_text);
	ClassDB::bind_method(D_METHOD("set_error_text", "value"), &PostMissionRoute::set_error_text);
	ClassDB::bind_method(D_METHOD("get_connection_error"), &PostMissionRoute::get_connection_error);
	ClassDB::bind_method(D_METHOD("set_connection_error", "value"),
			&PostMissionRoute::set_connection_error);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "keep_session"), "set_keep_session", "get_keep_session");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "error"), "set_error", "get_error");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error_key"), "set_error_key", "get_error_key");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error_text"), "set_error_text", "get_error_text");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "connection_error", PROPERTY_HINT_RESOURCE_TYPE,
						 "ConnectionError"),
			"set_connection_error", "get_connection_error");
}
