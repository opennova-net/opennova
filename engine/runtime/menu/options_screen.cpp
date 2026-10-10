#include "options_screen.h"
#include "options_policy.h"
#include <base/io/strutil.h>
#include <runtime/profile/profile_controls.h>

namespace opennova::menu {

namespace {

constexpr int kScroll = static_cast<int>(mnu::WindowType::Scroll);

void set_checked(MenuRuntime &menu, const char *name, bool checked) {
	const int id = menu.widget_id(name);
	if (id >= 0) menu.set_widget_checked(id, checked);
}

// UIWidget_SetInteractiveRecursive(enabled) on a named control.
void set_interactive(MenuRuntime &menu, const char *name, bool enabled) {
	const int id = menu.widget_id(name);
	if (id >= 0) menu.set_widget_disabled(id, !enabled);
}

// A checkbox's state into a record dword when the control exists
// [orig: UI_FindScreenControl ... sub_64ACB0 (the checked state)].
template <typename Write>
void read_checked(const MenuRuntime &menu, const char *name, Write write) {
	const int id = menu.widget_id(name);
	if (id >= 0) write(menu.is_widget_checked(id));
}

// The mouse and joystick words into their widgets: the shape both screens'
// inits and the DEFAULTS reseed share [orig: UI_OptionsScreenInit
// @0x554ba0..0x554ca2; UI_PopulateRenderAndAudioSettings @0x55d2b0..0x55d3c2;
// sub_55BD90 @0x55bee8..0x55bfb0].
void seed_mouse_and_joystick(MenuRuntime &menu, const playersav::ProfileRecord &record) {
	set_checked(menu, "INVERT_MOUSE", record.invert_mouse != 0);  // [orig: @0x554bb7]
	const int sensitivity = menu.widget_id("MOUSE_SENSITIVITY");
	MenuScrollRangeState range;
	if (sensitivity >= 0 && menu.widget_kind_of(sensitivity) == kScroll &&
			menu.get_widget_scroll_range(sensitivity, range)) {
		// CScrollWnd_SetScrollPos clamps into the range [orig: @0x554bfc].
		int32_t value = record.mouse_sensitivity;
		if (value < range.minimum) value = range.minimum;
		if (value > range.maximum) value = range.maximum;
		menu.set_widget_scroll_range(sensitivity, range.minimum, range.maximum, range.page, value);
	}
	const bool joystick = record.joystick_enabled != 0;
	set_checked(menu, "ENABLE_JOYSTICK", joystick);                         // [orig: @0x554c21]
	set_checked(menu, "INVERT_JOYSTICK", record.invert_joystick != 0);       // [orig: @0x554c55]
	set_interactive(menu, "INVERT_JOYSTICK", joystick);                      // [orig: @0x554c69]
	set_checked(menu, "ENABLE_FORCE_FEEDBACK", record.force_feedback != 0);  // [orig: @0x554c8e]
	set_interactive(menu, "ENABLE_FORCE_FEEDBACK", joystick);                // [orig: @0x554ca2]
}

} // namespace

int OptionsScreen::control_table(const MenuRuntime &menu) const {
	const int id = menu.widget_id("CONTROL_MAPPING");
	return menu.widget_kind_of(id) == static_cast<int>(mnu::WindowType::Table) ? id : -1;
}

// Presence gates keep same-named controls on SP/session screens out of the
// options handlers. The in-game dialog is the document authoring OPT_ACCEPT.
// [orig: UI_PopulateControlMappingList @0x55c0c0; UI_OptionsScreenInit @0x554800]
void OptionsScreen::prepare(MenuRuntime &menu, const playersav::ProfileRecord *record) {
	end_remap(menu, false);
	bindings_ = record != nullptr ? profile::options_bindings(*record) : controls::BindingSet();
	const int table = control_table(menu);
	has_table_ = table >= 0;
	is_surface_ = has_table_;
	for (const auto &range : kOptionsScrollRanges) {
		if (menu.widget_kind_of(menu.widget_id(range.control)) == kScroll) is_surface_ = true;
	}
	ingame_ = is_surface_ && menu.widget_id("OPT_ACCEPT") >= 0;
	if (has_table_) {
		device_ = controls::Device::Keyboard;
		fill(menu, table);
		const int keyboard = menu.widget_id("KEYBOARD"), mouse = menu.widget_id("MOUSE");
		if (keyboard >= 0) menu.set_widget_checked(keyboard, true);
		if (mouse >= 0) menu.set_widget_checked(mouse, false);
	}
}

// [orig: the slider ranges UI_OptionsScreenInit @0x554800 / UI_PopulateRenderAndAudioSettings
// @0x55c830 install; the VIDEO rows UI_SyncRenderSettingsToWidgets @0x55a140 selects by value; the
// rest options_policy.h's pins, each with its witness there]
void OptionsScreen::apply_policy(MenuRuntime &menu) const {
	if (!is_surface_) return;
	for (const OptionsScrollRange &range : kOptionsScrollRanges) {
		const int id = menu.widget_id(range.control);
		if (id >= 0 && menu.widget_kind_of(id) == kScroll)
			menu.set_widget_scroll_range(id, range.minimum, range.maximum, range.page, range.minimum);
	}
	for (const VideoQualityControl &control : kVideoQualityControls) {
		const int id = menu.widget_id(control.control);
		if (id < 0) continue;
		const int count = menu.item_count(id);
		for (int row = 0; row < count; ++row) {
			if (menu.item_value(id, row) != control.semantic_value) continue;
			menu.select_row(id, row, false);
			break;
		}
		menu.set_widget_disabled(id, true);
	}
	// Gamma is calibration, not a quality rung: the reference profile's, then locked.
	const int gamma = menu.widget_id("GAMMA");
	MenuScrollRangeState range;
	if (gamma >= 0 && menu.widget_kind_of(gamma) == kScroll) {
		if (menu.get_widget_scroll_range(gamma, range))
			menu.set_widget_scroll_range(gamma, range.minimum, range.maximum, range.page, kVideoGammaReference);
		menu.set_widget_disabled(gamma, true);
	}
	// A revision that authors RESOLUTION shows its last (highest) mode, locked.
	const int resolution = menu.widget_id("RESOLUTION");
	if (resolution >= 0) {
		const int count = menu.item_count(resolution);
		if (count > 0) menu.select_row(resolution, count - 1, false);
		menu.set_widget_disabled(resolution, true);
	}
	for (const char *name : kVideoPresetButtons) {
		const int id = menu.widget_id(name);
		if (id >= 0) menu.set_widget_disabled(id, true);
	}
	for (const OptionsForcedCheck &forced : kOptionsForcedChecks) {
		const int id = menu.widget_id(forced.control);
		if (id >= 0) menu.set_widget_checked(id, forced.checked);
	}
	for (const char *name : kOptionsUnsupportedControls) {
		const int id = menu.widget_id(name);
		if (id >= 0) menu.set_widget_disabled(id, true);
	}
}

void OptionsScreen::seed_profile(MenuRuntime &menu, const playersav::ProfileRecord *record) const {
	if (!is_surface_ || record == nullptr) return;
	seed_mouse_and_joystick(menu, *record);
	// Only the in-game dialog's init seeds the auto pair; the front-end
	// OPTIONS init never reads them [orig: UI_OptionsScreenInit @0x554d36 /
	// @0x554d62; UI_PopulateRenderAndAudioSettings has no reference].
	if (!ingame_) return;
	set_checked(menu, "OPTIONS_AUTORELOAD", profile::auto_reload_checked(*record));
	set_checked(menu, "OPTIONS_AUTOMEDIC", profile::auto_medic_checked(*record));
}

// The controls words back into the record, each when its control exists
// [orig: sub_55A710 @0x55ab5f..0x55ac09 (the front ACCEPT);
//  UI_IngameOptionsDialogEventHandler @0x5550de..0x5551b0 and
//  @0x55526a..0x5552bc (the in-game Accept, which alone reads the auto pair)].
void OptionsScreen::accept_words(const MenuRuntime &menu, playersav::ProfileRecord &record,
		bool ingame) const {
	read_checked(menu, "INVERT_MOUSE", [&](bool c) { record.invert_mouse = c ? 1 : 0; });
	const int sensitivity = menu.widget_id("MOUSE_SENSITIVITY");
	MenuScrollRangeState range;
	if (sensitivity >= 0 && menu.widget_kind_of(sensitivity) == kScroll &&
			menu.get_widget_scroll_range(sensitivity, range))
		record.mouse_sensitivity = range.value;  // [orig: sub_64CE60, the scroll position]
	read_checked(menu, "ENABLE_JOYSTICK", [&](bool c) { record.joystick_enabled = c ? 1 : 0; });
	read_checked(menu, "INVERT_JOYSTICK", [&](bool c) { record.invert_joystick = c ? 1 : 0; });
	read_checked(menu, "ENABLE_FORCE_FEEDBACK", [&](bool c) { record.force_feedback = c ? 1 : 0; });
	if (!ingame) return;
	read_checked(menu, "OPTIONS_AUTORELOAD", [&](bool c) { profile::set_auto_reload(record, c); });
	read_checked(menu, "OPTIONS_AUTOMEDIC", [&](bool c) { profile::set_auto_medic(record, c); });
}

// The table mirrors the screen's class/action/control rows. Capture
// blanks only the armed cell, preserving the action's row identity.
// [orig: UI_PopulateControlMappingList @0x55c0c0;
// UI_UpdateControlMappingDisplay @0x55b700]
void OptionsScreen::fill(MenuRuntime &menu, int table, int blank) {
	menu.table_clear_rows(table);
	int i = 0;
	for (const auto &row : bindings_.build_rows(device_)) {
		menu.table_add_row(table, {row.cls, row.action, i++ == blank ? "" : row.control});
	}
}

// [orig: UI_SelectControlsInputDevice @0x55bcd0]
void OptionsScreen::switch_device(MenuRuntime &menu, controls::Device device) {
	end_remap(menu, false);
	device_ = device;
	const int table = control_table(menu);
	if (table >= 0) fill(menu, table);
}

// [orig: UI_IngameOptionsDialogEventHandler @0x554e40 — the Accept's record
// writes and its copy of the screen's records @0x554e74;
// UI_RegisterIngameCallbacks @0x555510, OPT_ACCEPT @0x555597 / OPT_CANCEL @0x5555b5,
// ENABLE_JOYSTICK -> sub_5554B0 @0x5556bc; sub_55A710 (the front ACCEPT) @0x55ab5f..0x55ace5;
// UI_RegisterOptionsCallbacks ENABLE_JOYSTICK -> sub_55B010 @0x55d780;
// OPTIONS DEFAULTS @0x55bd90; CLEAR_KEY @0x55bfd0]
int OptionsScreen::activate(MenuRuntime &menu, playersav::ProfileRecord *record, int id) {
	if (!is_surface_) return None;
	const auto upper = strutil::to_upper(menu.widget_name_of(id));
	// The front BACK is registered on the OPTIONS scene's own BACK control,
	// user_data 0 [orig: UI_RegisterOptionsCallbacks @0x55d629], so the in-game
	// dialog (scene INGAME) has none. The callback rides that control and runs
	// after its ACTION rows [orig: CWnd_EmitEventToNamedHandlerAndCallbacks
	// @0x646970], whatever screen a pop_screen row selected meanwhile
	// [orig: UIScene_PopScreenHistory @0x63c410]: the gate is the activated
	// control's own screen, not the shown one. It writes no word: it puts the
	// saved gamma, music volume and menu sound-effects volume back over the live
	// previews [orig: Options_HandleAcceptOrBack @0x55adcf..0x55ae01], which the
	// shell's options owner does by returning to its entry state.
	if (upper == "BACK" && is_front_screen(menu.widget_screen_of(id))) return DiscardEdits;
	if (upper == "OPT_ACCEPT") {
		if (record != nullptr) {
			profile::store_bindings(bindings_, *record);  // [orig: @0x554e74]
			accept_words(menu, *record, true);
		}
		return CommitPreview | ApplyControls;
	}
	if (upper == "OPT_CANCEL") {
		// Cancel re-runs the screen's init over the record, which the dialog
		// left untouched [orig: @0x5553a7 -> UI_OptionsScreenInit @0x554800].
		prepare(menu, record);
		seed_profile(menu, record);
		return RestorePreview;
	}
	if (upper == "ENABLE_JOYSTICK") {
		// [orig: sub_55B010 @0x55b010 / sub_5554B0 @0x5554b0: the box's state
		//  makes the two joystick rows interactive or not]
		const int id = menu.widget_id("ENABLE_JOYSTICK");
		const bool enabled = id >= 0 && menu.is_widget_checked(id);
		set_interactive(menu, "INVERT_JOYSTICK", enabled);
		set_interactive(menu, "ENABLE_FORCE_FEEDBACK", enabled);
		return None;
	}
	if (!has_table_) return None;
	if (upper == "KEYBOARD") switch_device(menu, controls::Device::Keyboard);
	else if (upper == "MOUSE") switch_device(menu, controls::Device::Mouse);
	else if (upper == "JOYSTICK") switch_device(menu, controls::Device::Joystick);
	else if (upper == "ACCEPT") {
		// The front ACCEPT writes the words and the records into the record
		// and saves nothing [orig: @0x55ab5f..0x55ace5].
		if (record != nullptr) {
			accept_words(menu, *record, false);
			profile::store_bindings(bindings_, *record);
		}
		menu.pop_screen();
	} else if (upper == "DEFAULTS") {
		end_remap(menu, false);
		bindings_.restore_defaults();
		// The record's mouse and joystick words reset at once, then their
		// widgets [orig: @0x55be93..0x55beca, @0x55bee8..0x55bfb0].
		if (record != nullptr) {
			profile::restore_controls_defaults(*record);
			seed_mouse_and_joystick(menu, *record);
		}
		const int table = control_table(menu);
		if (table >= 0) fill(menu, table);
	} else if (upper == "CLEAR_KEY") {
		end_remap(menu, false);
		const int table = control_table(menu);
		if (table < 0) return None;
		const auto selected = menu.table_selected_rows(table);
		const int row = selected.empty() ? -1 : selected.front();
		const int action = bindings_.action_index_for_row(row);
		if (action < 0) return None;
		bindings_.clear(action, device_);
		fill(menu, table);
		menu.table_select_row(table, row, false);
	}
	return None;
}

bool OptionsScreen::is_front_screen(const std::string &screen) {
	return strutil::iequals(screen, "OPTIONS");
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
void OptionsScreen::arm(MenuRuntime &menu, int id, int row) {
	if (!strutil::iequals(menu.widget_name_of(id), "CONTROL_MAPPING") ||
			device_ == controls::Device::Joystick) return;
	const int action = bindings_.action_index_for_row(row);
	if (action < 0) return;
	table_ = id;
	row_ = row;
	action_ = action;
	fill(menu, id, row);
	menu.table_select_row(id, row, false);
}

// Esc restores without assigning. Key releases are swallowed while armed;
// a rejected/unmapped key leaves capture armed. Mouse releases / other
// device events fall through, and an unmappable mouse press ends capture.
// [orig: capture pump @0x55c67c; KeyBinding_HandleKeyAssignment @0x55bb20;
// mouse capture callback @0x55c780]
int OptionsScreen::consume(MenuRuntime &menu, const RemapInput &event) {
	if (action_ < 0) return None;
	if (event.kind == RemapInput::Kind::Key) {
		if (!event.pressed) return Consumed;
		if (event.escape) {
			end_remap(menu, true);
			return Consumed;
		}
		if (event.vk != 0 && bindings_.assign_key(action_, event.vk, event.ctrl, event.shift,
				controls::is_extended_vk(event.vk), event.repeat)) {
			end_remap(menu, true);
		}
		return Consumed;
	}
	if (event.kind == RemapInput::Kind::Mouse && event.pressed &&
			device_ == controls::Device::Mouse) {
		if (event.mouse_mask != 0) bindings_.assign_mouse(action_, event.mouse_mask);
		end_remap(menu, true);
		return Consumed;
	}
	return None;
}

// Screen/document changes cancel the screen-owned capture. Save the row
// before clearing the capture registers, then restore its display if asked.
// [orig: UI_UpdateControlMappingDisplay @0x55b700]
void OptionsScreen::end_remap(MenuRuntime &menu, bool refill) {
	if (action_ < 0) return;
	const int table = table_, row = row_;
	table_ = row_ = action_ = -1;
	if (refill && menu.widget_kind_of(table) == static_cast<int>(mnu::WindowType::Table)) {
		fill(menu, table);
		menu.table_select_row(table, row, false);
	}
}

} // namespace opennova::menu
