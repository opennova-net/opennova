#pragma once

#include "menu_runtime.h"
#include <formats/playersav/player_sav.h>
#include <runtime/controls/binding_set.h>

namespace opennova::menu {

// Device events have already crossed the Godot/OS key and button translation.
struct RemapInput {
	enum class Kind { Other, Key, Mouse };
	Kind kind = Kind::Other;
	bool pressed = false, escape = false, ctrl = false, shift = false, repeat = false;
	int vk = 0;
	uint16_t mouse_mask = 0;
};

// The Options screens over the current player.sav record. The screen keeps
// its own binding records, built from the record's table when it opens, which
// the remap flow edits; the front-end ACCEPT writes the controls words and the
// records back into the record, the in-game Accept the same plus the
// auto-reload / auto-medic pair, and leaves the live apply, the save and the
// device preview to the shell (the ApplyControls / CommitPreview effects).
// Nothing reaches the game's live bindings or the session's words before an
// Accept (and a front-end one waits for the next mission start's controls
// apply). A null record (no profile) leaves the words alone and the records
// the catalog defaults.
// [orig: UI_BuildKeyBindingLoadoutTable @0x559e50 (the records, from
//  UI_OptionsScreenInit @0x554dc5 and UI_PopulateRenderAndAudioSettings
//  @0x55d503); sub_55A710 @0x55ab5f..0x55ace5 (the front ACCEPT);
//  UI_IngameOptionsDialogEventHandler @0x554e40 (the in-game Accept)]
class OptionsScreen {
public:
	enum Effect {
		None = 0,
		Consumed = 1,
		// The in-game Accept: the shell takes its preview baseline, and
		// (ApplyControls) applies the record onto the live bindings and the
		// session's live words, then saves the profile [orig: @0x554e79,
		// @0x555158..0x55515e, @0x555252..0x555271, @0x55530b].
		CommitPreview = 4,
		RestorePreview = 8,
		ApplyControls = 16,
	};
	void prepare(MenuRuntime &menu, const playersav::ProfileRecord *record);
	bool is_surface() const { return is_surface_; }
	// The retail Options policy (options_policy.h) on the surface prepare() found, before any
	// settings owner seeds its values: every witnessed slider's range, its value at the minimum;
	// the VIDEO rows pinned to the one renderer path OpenNova ports and locked, GAMMA at its
	// reference and locked, RESOLUTION's last row locked, the preset buttons disabled; the
	// controls not serviced yet locked, showing their forced checks (D-MNU-21). Nothing off the
	// surface.
	void apply_policy(MenuRuntime &menu) const;
	// The record's controls words into their widgets, after apply_policy installed the
	// MOUSE_SENSITIVITY range the seed clamps into: INVERT_MOUSE, MOUSE_SENSITIVITY,
	// ENABLE_JOYSTICK, INVERT_JOYSTICK and ENABLE_FORCE_FEEDBACK (those two interactive only
	// with the joystick enabled), and on the in-game dialog OPTIONS_AUTORELOAD and
	// OPTIONS_AUTOMEDIC [orig: UI_OptionsScreenInit @0x554ba0..0x554d6e;
	// UI_PopulateRenderAndAudioSettings @0x55d2b0..0x55d3c2]. Nothing off the surface or
	// without a record.
	void seed_profile(MenuRuntime &menu, const playersav::ProfileRecord *record) const;
	int activate(MenuRuntime &menu, playersav::ProfileRecord *record, const std::string &name);
	void arm(MenuRuntime &menu, int id, int row);
	int consume(MenuRuntime &menu, const RemapInput &event);
	void end_remap(MenuRuntime &menu, bool refill);
	// The screen's own binding records (the remap flow's).
	const controls::BindingSet &bindings() const { return bindings_; }
	static void show_ingame_main(MenuRuntime &menu);

private:
	int control_table(const MenuRuntime &menu) const;
	void fill(MenuRuntime &menu, int table, int blank = -1);
	void switch_device(MenuRuntime &menu, controls::Device device);
	// The controls words from the widgets into the record; the in-game Accept
	// also reads the auto pair.
	void accept_words(const MenuRuntime &menu, playersav::ProfileRecord &record, bool ingame) const;
	controls::BindingSet bindings_;
	bool has_table_ = false, is_surface_ = false, ingame_ = false;
	controls::Device device_ = controls::Device::Keyboard;
	int table_ = -1, row_ = -1, action_ = -1;
};

} // namespace opennova::menu
