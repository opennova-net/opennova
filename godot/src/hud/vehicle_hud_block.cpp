#include "hud/vehicle_hud_block.h"

#include <cstdio>

using namespace godot;

namespace {

// The block's own field widths: 16-byte sid, 32-byte names.
void copy_fixed(char *p_dst, size_t p_cap, const String &p_src) {
	const CharString utf8 = p_src.utf8();
	snprintf(p_dst, p_cap, "%s", utf8.get_data());
}

} // namespace

String VehicleHudBlock::get_sid() const { return String::utf8(block_.sid); }
void VehicleHudBlock::set_sid(const String &p_value) { copy_fixed(block_.sid, sizeof(block_.sid), p_value); }
String VehicleHudBlock::get_icon() const { return String::utf8(block_.icon); }
void VehicleHudBlock::set_icon(const String &p_value) { copy_fixed(block_.icon, sizeof(block_.icon), p_value); }
String VehicleHudBlock::get_interface_texture() const { return String::utf8(block_.interface_texture); }
void VehicleHudBlock::set_interface_texture(const String &p_value) {
	copy_fixed(block_.interface_texture, sizeof(block_.interface_texture), p_value);
}
String VehicleHudBlock::get_static_texture() const { return String::utf8(block_.static_texture); }
void VehicleHudBlock::set_static_texture(const String &p_value) {
	copy_fixed(block_.static_texture, sizeof(block_.static_texture), p_value);
}
Vector2i VehicleHudBlock::get_driver() const { return Vector2i(block_.driver_x, block_.driver_y); }
void VehicleHudBlock::set_driver(const Vector2i &p_value) {
	block_.driver_x = p_value.x;
	block_.driver_y = p_value.y;
}

void VehicleHudBlock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_sid"), &VehicleHudBlock::get_sid);
	ClassDB::bind_method(D_METHOD("set_sid", "value"), &VehicleHudBlock::set_sid);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "sid"), "set_sid", "get_sid");
	ClassDB::bind_method(D_METHOD("get_icon"), &VehicleHudBlock::get_icon);
	ClassDB::bind_method(D_METHOD("set_icon", "value"), &VehicleHudBlock::set_icon);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "icon"), "set_icon", "get_icon");
	ClassDB::bind_method(D_METHOD("get_interface_texture"), &VehicleHudBlock::get_interface_texture);
	ClassDB::bind_method(D_METHOD("set_interface_texture", "value"), &VehicleHudBlock::set_interface_texture);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "interface_texture"), "set_interface_texture", "get_interface_texture");
	ClassDB::bind_method(D_METHOD("get_static_texture"), &VehicleHudBlock::get_static_texture);
	ClassDB::bind_method(D_METHOD("set_static_texture", "value"), &VehicleHudBlock::set_static_texture);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "static_texture"), "set_static_texture", "get_static_texture");
	ClassDB::bind_method(D_METHOD("get_driver"), &VehicleHudBlock::get_driver);
	ClassDB::bind_method(D_METHOD("set_driver", "value"), &VehicleHudBlock::set_driver);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "driver"), "set_driver", "get_driver");
	ClassDB::bind_method(D_METHOD("get_emplace_count"), &VehicleHudBlock::get_emplace_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "emplace_count", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_emplace_count");
	ClassDB::bind_method(D_METHOD("get_seat_count"), &VehicleHudBlock::get_seat_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seat_count", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_seat_count");
}
