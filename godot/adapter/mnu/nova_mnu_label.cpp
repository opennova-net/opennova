#include "nova_mnu_label.h"

using namespace godot;

void NovaMnuLabel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_string_id", "id"), &NovaMnuLabel::set_string_id);
	ClassDB::bind_method(D_METHOD("get_string_id"), &NovaMnuLabel::get_string_id);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "string_id"), "set_string_id", "get_string_id");
}
