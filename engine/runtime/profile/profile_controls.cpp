// The player profile's controls words — see <runtime/profile/profile_controls.h>
// and docs/playerinfo/player-sav-re.md.

#include <runtime/profile/profile_controls.h>

#include <runtime/controls/controls.h>

namespace opennova::profile {
namespace {

// The live record of the row an entry names: the row whose action code is the
// entry's row word, the slot the boot's re-lay put it in [orig:
// KeyBinding_SortBySequentialId @0x498260; sub_562DF0 @0x562df4 reads +4].
int row_for_entry(const playersav::BindingEntry &entry) {
	const controls::ActionDef *row = controls::action_for_code(entry.index);
	// The live set keeps one record per catalog entry, in the catalog's order.
	return row != nullptr ? static_cast<int>(row - controls::catalog(nullptr)) : -1;
}

}  // namespace

SessionInput session_input(const playersav::ProfileRecord &record, bool no_reload) {
	SessionInput out;
	out.invert_mouse = record.invert_mouse != 0;        // [orig: @0x551612]
	out.mouse_sensitivity = record.mouse_sensitivity;    // [orig: @0x55161e]
	// [orig: g_autoReloadEnabled = +1524 @0x551a40; `/noreload` -> 0 @0x551a48]
	out.auto_reload = !no_reload && record.auto_reload != 0;
	return out;
}

SessionInput ingame_accept_input(const playersav::ProfileRecord &record, const SessionInput &live) {
	SessionInput out = live;
	out.mouse_sensitivity = record.mouse_sensitivity;  // [orig: @0x55525e]
	out.invert_mouse = record.invert_mouse != 0;       // [orig: @0x555271]
	return out;
}

void apply_bindings(const playersav::ProfileRecord &record, controls::BindingSet &live) {
	for (const playersav::BindingEntry &entry : record.bindings) {
		const int row = row_for_entry(entry);
		if (row < 0) continue;  // retail __debugbreak()s past 768 rows (@0x562dfe)
		controls::BindingRecord rec;
		// [orig: sub_562DF0 @0x562e0f..0x562e59]
		rec.primary = entry.primary;
		rec.secondary = entry.secondary;
		rec.primary_mod = entry.primary_mod;
		rec.secondary_mod = entry.secondary_mod;
		rec.mouse_mask = entry.mouse_mask;
		rec.mouse_mod = entry.mouse_mod;
		rec.joy_button = entry.joy_button;
		rec.joy_mod = entry.joy_mod;
		live.set_record(row, rec);
	}
}

void apply_controls(const playersav::ProfileRecord &record, controls::BindingSet &live) {
	live.set_joystick_enabled(record.joystick_enabled != 0);  // [orig: @0x563664]
	apply_bindings(record, live);                              // [orig: @0x5636bf]
}

controls::BindingSet options_bindings(const playersav::ProfileRecord &record) {
	controls::BindingSet out;
	apply_bindings(record, out);
	return out;
}

void store_bindings(const controls::BindingSet &records, playersav::ProfileRecord &record) {
	for (playersav::BindingEntry &entry : record.bindings) {
		const controls::BindingRecord *rec = records.record(row_for_entry(entry));
		if (rec == nullptr) continue;
		// [orig: @0x559dd5..0x559e21]
		entry.primary = rec->primary;
		entry.secondary = rec->secondary;
		entry.primary_mod = rec->primary_mod;
		entry.secondary_mod = rec->secondary_mod;
		entry.mouse_mask = rec->mouse_mask;
		entry.mouse_mod = rec->mouse_mod;
		entry.joy_button = rec->joy_button;
		entry.joy_mod = rec->joy_mod;
	}
}

bool auto_reload_checked(const playersav::ProfileRecord &record) {
	return record.auto_reload != 0;  // [orig: the dword as the state @0x554d36]
}

bool auto_medic_checked(const playersav::ProfileRecord &record) {
	return record.auto_medic_off == 0;  // [orig: `cmp +1660, 0; setz` @0x56074c]
}

void set_auto_reload(playersav::ProfileRecord &record, bool checked) {
	record.auto_reload = checked ? 1 : 0;  // [orig: @0x55528c]
}

void set_auto_medic(playersav::ProfileRecord &record, bool checked) {
	record.auto_medic_off = checked ? 0 : 1;  // [orig: neg/sbb/add 1 @0x5552b5..0x5552bc]
}

void restore_controls_defaults(playersav::ProfileRecord &record) {
	// [orig: sub_55BD90 — +1424 = 0x80 @0x55be93, +1428 @0x55bea2, +1432
	//  @0x55beae, +1436 @0x55beba, +1444 @0x55beca = 0]
	record.mouse_sensitivity = 0x80;
	record.invert_mouse = 0;
	record.joystick_enabled = 0;
	record.invert_joystick = 0;
	record.force_feedback = 0;
}

}  // namespace opennova::profile
