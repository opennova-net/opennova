#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

// Thin GDScript-facing wrapper over the portable libs/controls catalog. Hands the
// menu shell the Class/Action/Control rows for the Options -> Controls (CONTROL_MAPPING)
// table, per input device. Read-only: rebinding/persistence are not modelled here.
// The engine logic (catalog, key-name decode, binding format) lives in libs/controls
// [orig: UI_PopulateControlMappingList @ 0x55c0c0].
class NovaControlsModel : public RefCounted {
	GDCLASS(NovaControlsModel, RefCounted)

protected:
	static void _bind_methods();

public:
	enum Device {
		DEVICE_KEYBOARD = 0,
		DEVICE_MOUSE = 1,
		DEVICE_JOYSTICK = 2,
	};

	// Returns an Array of PackedStringArray rows, each [class, action, control], for
	// the given device (one of the Device enum values).
	TypedArray<PackedStringArray> get_rows(int p_device) const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaControlsModel::Device);
