#pragma once

#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;
class NovaMnuScroll;

// A multi-line MNU text field (type="multiline_edit"). Subclasses TextEdit. The
// parsed READONLY flag maps to editable=false; a host can flip it at runtime via
// set_readonly(). The .mnu seeds STRING text; a host drives content with TextEdit's
// set_text()/get_text(). On edit it notifies the owning menu's widget_value_changed.
//
// In edit_mode the builder forces editable=false and it connects no signals.
class NovaMnuMultilineEdit : public TextEdit {
	GDCLASS(NovaMnuMultilineEdit, TextEdit)

private:
	MnuWidgetBehavior behavior_;
	NovaMnuScroll *authored_scrollbar_ = nullptr;
	bool readonly_ = false;
	bool runtime_enabled_ = true;

	void on_text_changed();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_readonly(bool p_readonly) {
		readonly_ = p_readonly;
		set_editable(runtime_enabled_ && !readonly_ && !behavior_.edit_mode);
	}
	bool get_readonly() const { return readonly_; }
	void set_runtime_enabled(bool p_enabled) {
		runtime_enabled_ = p_enabled;
		set_editable(runtime_enabled_ && !readonly_ && !behavior_.edit_mode);
	}
	void set_authored_scrollbar(NovaMnuScroll *p_scrollbar) {
		authored_scrollbar_ = p_scrollbar;
	}
};

} // namespace godot
