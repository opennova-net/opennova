#pragma once

#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// A menu text label (port of mnu_label.gd). The builder resolves the RTXT
// string at build time, so at runtime this mostly carries its string_id so the
// editor can address it and a shell can re-resolve text after swapping the text
// resource. Used both as a standalone Static/Label widget's text node and as the
// "Label" child of buttons/checkboxes.
class NovaMnuLabel : public Label {
	GDCLASS(NovaMnuLabel, Label)

private:
	String string_id_;

protected:
	static void _bind_methods();

public:
	void set_string_id(const String &p_id) { string_id_ = p_id; }
	String get_string_id() const { return string_id_; }
};

} // namespace godot
