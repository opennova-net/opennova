#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <runtime/controls/binding_set.h>

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
	// witnessed semantics — ctrl/shift/repeat feed the original event flag
	// word; the extended-key flag derives from the VK). assign_godot_key
	// returns false when the key has no VK mapping or the engine rejects it.
	bool assign_godot_key(int p_action, int p_godot_key, bool p_ctrl,
			bool p_shift, bool p_repeat);
	void assign_mouse_mask(int p_action, int p_mask);
	void clear_binding(int p_action, int p_device);
	void restore_defaults();

	// Godot keycodes currently bound to a catalog token (primary then
	// secondary; modifiers not included — display/diagnostic use).
	PackedInt32Array godot_keys_for_token(const String &p_token) const;

	// Whether the token's binding is held RIGHT NOW: keyboard slots (their
	// modifier word gating Ctrl-/Shift- combos) plus the held-sampleable
	// mouse-mask buttons (wheel masks are impulse-only and never sample).
	// The one gameplay-sampler entry point — samples Godot Input here at the
	// device seam.
	bool is_token_pressed(const String &p_token) const;
	// The in-game display string of a token's binding — the death screen's
	// "call a medic" hint formatter (engine controls format_display_string;
	// retail KeyBinding_FormatDisplayString @0x496bd0). "" when unbound.
	String display_text_for_token(const String &p_token) const;

	// The capture button->mask translation (0 = unmappable). The witnessed
	// mask values are the engine's kMouse* constants (controls/binding_set.h
	// carries the citation); this seam only maps Godot's MouseButton onto
	// them.
	static int mouse_mask_from_godot_button(int p_button);

	// VK <-> Godot Key translation (0 when unmappable).
	static int vk_from_godot_key(int p_godot_key);
	static int godot_key_from_vk(int p_vk);

	// Persistence blob: token -> [primary, secondary, primary_mod,
	// secondary_mod, mouse_mask, joy_button]. The shell owns where it lives.
	Dictionary save_blob() const;
	void load_blob(const Dictionary &p_blob);

private:
	opennova::controls::BindingSet bindings_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::ControlsModel::Device);
