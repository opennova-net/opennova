#include "world/world_entity_proxy.h"

using namespace godot;

void WorldEntityProxy::_bind_methods() {
	BIND_ENUM_CONSTANT(UNPLACED);
	BIND_ENUM_CONSTANT(STATIC_INSTANCE);
	BIND_ENUM_CONSTANT(MODEL);
	ClassDB::bind_method(D_METHOD("get_kind"), &WorldEntityProxy::get_kind);
	ClassDB::bind_method(D_METHOD("get_index"), &WorldEntityProxy::get_index);
	ClassDB::bind_method(D_METHOD("get_bms_id"), &WorldEntityProxy::get_bms_id);
	ClassDB::bind_method(D_METHOD("get_item_id"), &WorldEntityProxy::get_item_id);
	ClassDB::bind_method(D_METHOD("get_graphic"), &WorldEntityProxy::get_graphic);
	ClassDB::bind_method(D_METHOD("get_local_bounds"), &WorldEntityProxy::get_local_bounds);
	ClassDB::bind_method(D_METHOD("get_representation"), &WorldEntityProxy::get_representation);
}
