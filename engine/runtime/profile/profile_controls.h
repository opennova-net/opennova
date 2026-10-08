#pragma once

// The player profile's controls words as the game reads them: the session
// copy's input words (the mouse, the joystick, auto-reload), the binding
// table's apply onto the live records and the Options screen's store back, and
// the auto-reload / auto-medic checkboxes the dialogs show over the record.
// Witness record: docs/playerinfo/player-sav-re.md "The controls words".

#include <cstdint>

#include <formats/playersav/player_sav.h>
#include <runtime/controls/binding_set.h>

namespace opennova::profile {

// The input words a mission start copies out of the current record into the
// globals the simulation reads [orig: Game_ApplySessionSettingsToGlobals
// @0x551500, run by Game_StartMission @0x524662 at every mission start]. The
// same copy writes the mouse look's gate dword_24D2074 = 1 whatever +1420
// holds (@0x551601..0x551606, after the controls apply's copy of +1420 at
// sub_563620 @0x563652), so the mouse is always on in a mission; the
// ENABLE_JOYSTICK word reaches the input layer through apply_controls; +1436
// (dword_24D208C, the joystick's analog channel 1 negate,
// Input_TryTriggerAxisBinding @0x497c0c) and +1432 && +1444 (dword_24D20AC,
// the force-feedback effects' gate, sub_455910 @0x45591a) have no device leg
// in the port.
struct SessionInput {
	// dword_24D2078 <- +1428: the look's Y delta is negated unless set
	// [orig: @0x55160c..0x551612; Input_ProcessMouseAxisBindings @0x4996cf].
	bool invert_mouse = false;
	// dword_24D207C <- +1424, copied unclamped; the look scales by it << 11
	// [orig: @0x551618..0x55161e; @0x4996dd].
	int32_t mouse_sensitivity = 128;
	// g_autoReloadEnabled <- +1524, forced 0 under `/noreload`
	// [orig: @0x551a24..0x551a48; the flag Game_ParseCommandLineAndInit
	//  @0x4a76d7..0x4a76e9 -> dword_B4C4F4].
	bool auto_reload = true;
};

// The session copy's input words from a record. `no_reload` is the `/noreload`
// launch flag.
SessionInput session_input(const playersav::ProfileRecord &record, bool no_reload);

// The in-game options Accept's live writes over the words a session holds:
// the mouse look's two words from the record it just wrote, at once; the
// auto-reload global stands until the next mission start's session copy, its
// one writer [orig: UI_IngameOptionsDialogEventHandler @0x555252..0x55525e
// (dword_24D207C <- +1424), @0x555264..0x555271 (dword_24D2078 <- +1428);
// g_autoReloadEnabled written only @0x551a40/@0x551a48].
SessionInput ingame_accept_input(const playersav::ProfileRecord &record, const SessionInput &live);

// The record's binding table onto the live records: each entry's eight
// binding fields onto the record of the row whose action code the entry's
// row word (+4) names; a row the table does not carry keeps what it held
// [orig: sub_562E60 @0x562e60 -> sub_562DF0 @0x562df0 (row 54 * entry+4: the
//  flag word, +24/+26 the keys, +28/+30 their modifiers, +32 the mouse mask,
//  +34 its modifier, +36 the joystick binding, +37 its modifier)]. The flag
// word the original copies too is the catalog row's own on every entry the
// load keeps (the merge leaves the identity fields the default table's), so
// the port's static flags stand.
void apply_bindings(const playersav::ProfileRecord &record, controls::BindingSet &live);

// The controls apply every session start runs before the mission loads, and
// the in-game options Accept with it: the binding table onto the live records
// and the ENABLE_JOYSTICK word into the joystick gate
// [orig: sub_563620 @0x563620 — +1432 -> dword_24D2088 @0x563664, sub_562E60
//  @0x5636bf; callers SinglePlayer_StartMission @0x561bc1,
//  UI_HandleHostSessionStart @0x556e77, the join's pre-game step sub_5693E0
//  @0x5693ef, the NovaWorld URL host @0x54f30a; the in-game Accept's
//  sub_562E60 @0x554e79 and its +1432 -> dword_24D2088 @0x555158..0x55515e].
void apply_controls(const playersav::ProfileRecord &record, controls::BindingSet &live);

// The Options screen's working records as its init builds them from the
// record's table [orig: UI_BuildKeyBindingLoadoutTable @0x559e50, from
// UI_OptionsScreenInit @0x554dc5 and UI_PopulateRenderAndAudioSettings
// @0x55d503]: a fresh set (the catalog defaults) with the table applied.
controls::BindingSet options_bindings(const playersav::ProfileRecord &record);

// The Options screen's records back into the record's table: each entry, in
// the table's own order, takes the eight binding fields of its row's record;
// the count and the identity fields stand [orig:
// sort_and_copy_weapon_loadout_to_session @0x559d50 — the qsort by the
// entry's table index (+40, sub_559D40 @0x559d40) restores the table order
// before the copy @0x559d90..0x559e2a; callers the front-end ACCEPT
// @0x55ace5 and the in-game Accept @0x554e74].
void store_bindings(const controls::BindingSet &records, playersav::ProfileRecord &record);

// The auto-reload and auto-medic checkboxes over the record. OPTIONS_AUTORELOAD
// shows +1524 as its checked state and writes the box back; OPTIONS_AUTOMEDIC
// shows +1660 == 0 and writes the box INVERTED (checked -> 0, clear -> 1)
// [orig: seeds UI_OptionsScreenInit @0x554d36 / @0x554d62,
//  PlayerInfo_PopulateAllControls @0x56071d / @0x56074c; writes
//  UI_IngameOptionsDialogEventHandler @0x55528c / @0x5552b5..0x5552bc (neg,
//  sbb, add 1), PlayerInfo_SaveFromDialog @0x55ef5f / @0x55ef92].
bool auto_reload_checked(const playersav::ProfileRecord &record);
bool auto_medic_checked(const playersav::ProfileRecord &record);
void set_auto_reload(playersav::ProfileRecord &record, bool checked);
void set_auto_medic(playersav::ProfileRecord &record, bool checked);

// The Options DEFAULTS button's reset of the record's mouse and joystick
// words: +1424 = 128, +1428 / +1432 / +1436 / +1444 = 0, written into the
// record at once (the binding records are the screen's own)
// [orig: sub_55BD90 @0x55be93..0x55beca].
void restore_controls_defaults(playersav::ProfileRecord &record);

}  // namespace opennova::profile
