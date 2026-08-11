#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "controls/binding_set.h"

namespace godot {

// GDScript-facing wrapper over the portable engine/runtime/controls catalog and
// its LIVE binding records. Hands the menu shell the Class/Action/Control rows
// for the Options -> Controls (CONTROL_MAPPING) table, applies the witnessed
// remap operations (assign / clear / defaults), and exposes the live keys to
// the gameplay input sampler. The engine logic (catalog, key-name decode,
// binding format, record semantics) lives in engine/runtime/controls
// (binding_set.h carries the [orig:] chain); this class adds only the
// Godot-device seam: the VK <-> Godot Key translation and the Variant blob
// the shell persists.
class ControlsModel : public RefCounted {
	GDCLASS(ControlsModel, RefCounted)

protected:
	static void _bind_methods();

public:
	enum Device {
		DEVICE_KEYBOARD = 0,
		DEVICE_MOUSE = 1,
		DEVICE_JOYSTICK = 2,
	};

	// An Array of PackedStringArray rows, each [class, action, control], for
	// the given device, from the LIVE binding records.
	TypedArray<PackedStringArray> get_rows(int p_device) const;

	// The catalog action behind a visible table row (-1 out of range).
	int action_index_for_row(int p_row) const;
	// The Control-column text for one catalog action on one device.
	String control_text(int p_action, int p_device) const;

	// Remap operations over the live records (see binding_set.h for the
	// witnessed semantics). assign_godot_key returns false when the key has
	// no VK mapping or the engine rejects it.
	bool assign_godot_key(int p_action, int p_godot_key, bool p_repeat);
	void assign_mouse_mask(int p_action, int p_mask);
	void clear_binding(int p_action, int p_device);
	void restore_defaults();

	// Godot keycodes currently bound to a catalog token (for the gameplay
	// input sampler; primary then secondary).
	PackedInt32Array godot_keys_for_token(const String &p_token) const;

	// VK <-> Godot Key translation (0 when unmappable).
	static int vk_from_godot_key(int p_godot_key);
	static int godot_key_from_vk(int p_vk);

	// Persistence blob: token -> [primary, secondary, primary_ext,
	// secondary_ext, mouse_mask, joy_button]. The shell owns where it lives.
	Dictionary save_blob() const;
	void load_blob(const Dictionary &p_blob);

private:
	opennova::controls::BindingSet bindings_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::ControlsModel::Device);
