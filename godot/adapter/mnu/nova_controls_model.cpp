#include "nova_controls_model.h"

#include "controls/controls.h"

using namespace godot;

TypedArray<PackedStringArray> NovaControlsModel::get_rows(int p_device) const {
	opennova::controls::Device dev = opennova::controls::Device::Keyboard;
	if (p_device == DEVICE_MOUSE) {
		dev = opennova::controls::Device::Mouse;
	} else if (p_device == DEVICE_JOYSTICK) {
		dev = opennova::controls::Device::Joystick;
	}

	TypedArray<PackedStringArray> out;
	const std::vector<opennova::controls::ControlRow> rows = opennova::controls::build_rows(dev);
	for (const opennova::controls::ControlRow &r : rows) {
		PackedStringArray cells;
		cells.push_back(String::utf8(r.cls.c_str()));
		cells.push_back(String::utf8(r.action.c_str()));
		cells.push_back(String::utf8(r.control.c_str()));
		out.push_back(cells);
	}
	return out;
}

void NovaControlsModel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_rows", "device"), &NovaControlsModel::get_rows);

	BIND_ENUM_CONSTANT(DEVICE_KEYBOARD);
	BIND_ENUM_CONSTANT(DEVICE_MOUSE);
	BIND_ENUM_CONSTANT(DEVICE_JOYSTICK);
}
