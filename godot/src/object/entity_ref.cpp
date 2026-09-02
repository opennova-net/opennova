#include "object/entity_ref.h"

using namespace godot;

Ref<EntityRef> EntityRef::make(int p_kind, int p_index, int p_bms_id, int p_item_id,
		int p_wire_handle) {
	Ref<EntityRef> ref;
	ref.instantiate();
	ref->kind_ = p_kind;
	ref->index_ = p_index;
	ref->bms_id_ = p_bms_id;
	ref->item_id_ = p_item_id;
	ref->wire_handle_ = p_wire_handle;
	return ref;
}

void EntityRef::_bind_methods() {
#define ENTITY_REF_INT_BIND(m_name, m_default)                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &EntityRef::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &EntityRef::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	ENTITY_REF_INT_FIELDS(ENTITY_REF_INT_BIND)
#undef ENTITY_REF_INT_BIND
	ClassDB::bind_method(D_METHOD("get_attrib2"), &EntityRef::get_attrib2);
	ClassDB::bind_method(D_METHOD("set_attrib2", "value"), &EntityRef::set_attrib2);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "attrib2"), "set_attrib2", "get_attrib2");
	ClassDB::bind_method(D_METHOD("get_position"), &EntityRef::get_position);
	ClassDB::bind_method(D_METHOD("set_position", "position"), &EntityRef::set_position);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "position"), "set_position", "get_position");
	ClassDB::bind_method(D_METHOD("get_graphic"), &EntityRef::get_graphic);
	ClassDB::bind_method(D_METHOD("set_graphic", "graphic"), &EntityRef::set_graphic);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "graphic"), "set_graphic", "get_graphic");
	ClassDB::bind_method(D_METHOD("has_wire_handle"), &EntityRef::has_wire_handle);
	ClassDB::bind_static_method("EntityRef",
			D_METHOD("make", "kind", "index", "bms_id", "item_id", "wire_handle"),
			&EntityRef::make, DEFVAL(0), DEFVAL(-1));
}
