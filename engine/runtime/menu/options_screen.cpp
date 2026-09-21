#include "options_screen.h"
#include "options_policy.h"
#include <base/io/strutil.h>

namespace opennova::menu {

int OptionsScreen::control_table(const MenuRuntime &menu) const {
	const int id = menu.widget_id("CONTROL_MAPPING");
	return menu.widget_kind_of(id) == static_cast<int>(mnu::WindowType::Table) ? id : -1;
}

// Presence gates keep same-named controls on SP/session screens out of the
// options handlers. game.mnu has sliders but no remapping table.
// [orig: UI_PopulateControlMappingList @0x55c0c0; options_screen_init @0x554800]
void OptionsScreen::prepare(MenuRuntime &menu, const controls::BindingSet &bindings) {
	end_remap(menu, bindings, false);
	const int table = control_table(menu);
	has_table_ = table >= 0;
	is_surface_ = has_table_;
	for (const auto &range : kOptionsScrollRanges) {
		if (menu.widget_kind_of(menu.widget_id(range.control)) ==
				static_cast<int>(mnu::WindowType::Scroll)) is_surface_ = true;
	}
	if (has_table_) {
		device_ = controls::Device::Keyboard;
		fill(menu, bindings, table);
		const int keyboard = menu.widget_id("KEYBOARD"), mouse = menu.widget_id("MOUSE");
		if (keyboard >= 0) menu.set_widget_checked(keyboard, true);
		if (mouse >= 0) menu.set_widget_checked(mouse, false);
	}
}

// The table mirrors the live catalog's class/action/control rows. Capture
// blanks only the armed cell, preserving the action's row identity.
// [orig: UI_PopulateControlMappingList @0x55c0c0;
// update_control_mapping_display @0x55b700]
void OptionsScreen::fill(MenuRuntime &menu, const controls::BindingSet &bindings,
		int table, int blank) {
	menu.table_clear_rows(table);
	int i = 0;
	for (const auto &row : bindings.build_rows(device_)) {
		menu.table_add_row(table, {row.cls, row.action, i++ == blank ? "" : row.control});
	}
}

// [orig: UI_SelectControlsInputDevice @0x55bcd0]
void OptionsScreen::switch_device(MenuRuntime &menu, const controls::BindingSet &bindings,
		controls::Device device) {
	end_remap(menu, bindings, false);
	device_ = device;
	const int table = control_table(menu);
	if (table >= 0) fill(menu, bindings, table);
}

// The shell applies the snapshot copy/update after the returned preview
// request, before showing MAIN_WRAPPER. Storage remains its device leg.
// [orig: ingame_options_dialog_event_handler @0x554e40;
// UI_RegisterIngameCallbacks @0x555510, OPT_ACCEPT @0x555597 / OPT_CANCEL @0x5555b5;
// OPTIONS DEFAULTS @0x55bd90; CLEAR_KEY @0x55bfd0]
int OptionsScreen::activate(MenuRuntime &menu, controls::BindingSet &bindings,
		const std::string &name) {
	if (!is_surface_) return None;
	const auto upper = strutil::to_upper(name);
	if (upper == "OPT_ACCEPT") return CommitPreview;
	if (upper == "OPT_CANCEL") return RestorePreview;
	if (!has_table_) return None;
	if (upper == "KEYBOARD") switch_device(menu, bindings, controls::Device::Keyboard);
	else if (upper == "MOUSE") switch_device(menu, bindings, controls::Device::Mouse);
	else if (upper == "JOYSTICK") switch_device(menu, bindings, controls::Device::Joystick);
	else if (upper == "ACCEPT") menu.pop_screen();
	else if (upper == "DEFAULTS") {
		end_remap(menu, bindings, false);
		bindings.restore_defaults();
		const int table = control_table(menu);
		if (table >= 0) fill(menu, bindings, table);
		return PersistBindings;
	} else if (upper == "CLEAR_KEY") {
		end_remap(menu, bindings, false);
		const int table = control_table(menu);
		if (table < 0) return None;
		const auto selected = menu.table_selected_rows(table);
		const int row = selected.empty() ? -1 : selected.front();
		const int action = bindings.action_index_for_row(row);
		if (action < 0) return None;
		bindings.clear(action, device_);
		fill(menu, bindings, table);
		menu.table_select_row(table, row, false);
		return PersistBindings;
	}
	return None;
}

void OptionsScreen::show_ingame_main(MenuRuntime &menu) {
	const int main = menu.widget_id("MAIN_WRAPPER"), options = menu.widget_id("OPTIONS_WRAPPER");
	if (main >= 0 && options >= 0) {
		menu.set_widget_shown(main, true);
		menu.set_widget_shown(options, false);
	}
}

// The port arms on the driver's list activation (double-click); joystick
// capture remains refused under D-CTRL-1, while its device page is served.
// [orig: UI_ControlsRemapArmHandler @0x55d560; CTableWnd_SetCellText @0x63edf0]
void OptionsScreen::arm(MenuRuntime &menu, const controls::BindingSet &bindings,
		int id, int row) {
	if (!strutil::iequals(menu.widget_name_of(id), "CONTROL_MAPPING") ||
			device_ == controls::Device::Joystick) return;
	const int action = bindings.action_index_for_row(row);
	if (action < 0) return;
	table_ = id;
	row_ = row;
	action_ = action;
	fill(menu, bindings, id, row);
	menu.table_select_row(id, row, false);
}

// Esc restores without assigning. Key releases are swallowed while armed;
// a rejected/unmapped key leaves capture armed. Mouse releases / other
// device events fall through, and an unmappable mouse press ends capture.
// [orig: capture pump @0x55c67c; KeyBinding_HandleKeyAssignment @0x55bb20;
// mouse capture callback @0x55c780]
int OptionsScreen::consume(MenuRuntime &menu, controls::BindingSet &bindings,
		const RemapInput &event) {
	if (action_ < 0) return None;
	if (event.kind == RemapInput::Kind::Key) {
		if (!event.pressed) return Consumed;
		if (event.escape) {
			end_remap(menu, bindings, true);
			return Consumed;
		}
		if (event.vk != 0 && bindings.assign_key(action_, event.vk, event.ctrl, event.shift,
				controls::is_extended_vk(event.vk), event.repeat)) {
			end_remap(menu, bindings, true);
			return Consumed | PersistBindings;
		}
		return Consumed;
	}
	if (event.kind == RemapInput::Kind::Mouse && event.pressed &&
			device_ == controls::Device::Mouse) {
		if (event.mouse_mask != 0) bindings.assign_mouse(action_, event.mouse_mask);
		end_remap(menu, bindings, true);
		return Consumed | (event.mouse_mask != 0 ? PersistBindings : None);
	}
	return None;
}

// Screen/document changes cancel the screen-owned capture. Save the row
// before clearing the capture registers, then restore its display if asked.
// [orig: update_control_mapping_display @0x55b700]
void OptionsScreen::end_remap(MenuRuntime &menu, const controls::BindingSet &bindings,
		bool refill) {
	if (action_ < 0) return;
	const int table = table_, row = row_;
	table_ = row_ = action_ = -1;
	if (refill && menu.widget_kind_of(table) == static_cast<int>(mnu::WindowType::Table)) {
		fill(menu, bindings, table);
		menu.table_select_row(table, row, false);
	}
}

} // namespace opennova::menu
