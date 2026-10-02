#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <formats/mnu/mnu.h>

namespace godot {

// One authored <SOUND> row of a widget: `file` is the .lwf profile and
// `trigger` names a set in it (MOUSE_OVER / CLICK_SELECT / ...); `state` is
// the widget state token the driver matches (mousein / selected / ...).
class MnuSoundRow : public RefCounted {
	GDCLASS(MnuSoundRow, RefCounted)

	opennova::mnu::Sound value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mnu::Sound &p_value) { value_ = p_value; }

	String get_state() const;
	String get_trigger() const;
	String get_file() const;
};

// One authored <ACTION> row of a widget, as the document parsed it: the
// navigation / window verbs the driver executes and the retail shell-owned
// GLB / LAN / form / app-message verbs it hands to the shell by name.
class MnuActionRow : public RefCounted {
	GDCLASS(MnuActionRow, RefCounted)

	opennova::mnu::Action value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mnu::Action &p_value) { value_ = p_value; }
	// The parsed row, for the engine dispatch (menu::MenuRuntime::dispatch_action).
	const opennova::mnu::Action &native() const { return value_; }
	// The test-side constructor: a row with the fields the driver dispatches on.
	static Ref<MnuActionRow> make(const String &p_type, const String &p_target,
			const String &p_state, bool p_toggle, const String &p_file);

	String get_type() const;
	String get_target() const;
	String get_state() const;
	String get_file() const;
	// FIELD / SOURCE / NAME: the one slot, and the spelling it was authored with.
	String get_field() const;
	String get_field_attr() const;
	String get_test() const;
	int get_target_form() const { return value_.target_form; }
	bool is_toggle() const { return value_.toggle; }
	bool is_external_browser() const { return value_.external_browser; }
};

} // namespace godot
