// engine/runtime/profile — the player profile's in-memory image: the fresh
// record's seeds, the binding merge, the load and the save, the screens' name
// rules, the session steps and the controls words the game reads
// (profile_controls.h). Fixtures are built in code; the fresh record's tables
// are pinned against the program's own static data (digests read once from the
// pinned JO:CA Jointops.exe, SHA-256 9a1035440a53af2057ce0995ac42dced840d3b9f
// d53c04dc86041a962b84fe57), the controls words' offsets against the
// instructions that read and write them, and the shipped default weapon.sav
// (common/default_weapon_sav.h) loads and saves in memory.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/default_weapon_sav.h"
#include "common/test_expect.h"
#include <formats/playersav/player_sav.h>
#include <formats/playersav/weapon_sav.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/controls/controls.h>
#include <runtime/controls/binding_set.h>
#include <runtime/profile/player_profiles.h>
#include <runtime/profile/profile_controls.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>

using namespace opennova;
using opennova::profile::PlayerProfiles;
using opennova::profile::ProfileDefaults;

namespace {

// The title the profiles are Joint Operations' (its files' magic, Title::profile_magic).
const Title &kJo = games::jo_title();

ProfileDefaults defaults() {
    ProfileDefaults d;
    d.bindings = profile::default_binding_table();
    d.macros = profile::default_macros(nullptr, nullptr);
    d.avatars[0] = {0, 0, 0x0200};
    d.avatars[1] = {7, 0, 0x8207};
    return d;
}

rtxt::File macros_table(const std::vector<std::pair<std::string, std::string>> &rows) {
    rtxt::File f;
    f.sections.push_back(rtxt::Section{"Macros", static_cast<uint32_t>(rows.size())});
    for (const auto &row : rows) {
        rtxt::Entry e;
        e.key = row.first;
        e.text = row.second;
        e.section_index = 0;
        f.entries.push_back(e);
    }
    return f;
}

// --- 1. the default binding table [orig: KeyBinding_BuildFilteredTable @0x54c2b0] ---

int test_default_binding_table() {
    const std::vector<playersav::BindingEntry> table = profile::default_binding_table();
    // Every catalog row with the 0x4000000 flag: 109 in JO:CA (the shipped
    // profiles' count).
    TEST_EXPECT(table.size() == 109);
    for (size_t i = 1; i < table.size(); ++i) TEST_EXPECT(table[i - 1].id < table[i].id);
    const playersav::BindingEntry &command = table[0];
    // The row's token, NUL-terminated inside the row [orig: @0x54c37a..0x54c38d].
    TEST_EXPECT(command.id == 1 && command.index == 1 && command.token == "command");
    TEST_EXPECT(command.flags == 0x05000800u && command.modes == 3 && command.action_class == 10);
    TEST_EXPECT(command.help == 10001 && command.primary == 0xC0);
    bool scope = false;
    bool hudcolor = false;
    for (const playersav::BindingEntry &e : table) {
        TEST_EXPECT((e.flags & 0x4000000u) != 0);
        if (e.token == "scope") {
            scope = e.id == 6 && e.primary == 0xBF && e.mouse_mask == 0x2 && e.help == 2015;
        }
        if (e.token == "hudcolor") hudcolor = e.primary == 0x75 && e.primary_mod == 17;
        TEST_EXPECT(e.token != "null" && e.token != "tod" && e.token != "todrate");
    }
    TEST_EXPECT(scope && hudcolor);
    TEST_EXPECT(table.back().token == "DecSpectatorTarget" && table.back().id == 502);
    return 0;
}

// --- 2. the macros [orig: @0x54bcb5..0x54bccc over TextResource_GetStringWithFallback] ---

int test_default_macros() {
    const auto missing = profile::default_macros(nullptr, nullptr);
    TEST_EXPECT(missing[0] == "??MACRO_0??" && missing[9] == "??MACRO_9??");
    const rtxt::File menu = macros_table({{"MACRO_0", "I'm on offense"},
            {"MACRO_1", std::string(50, 'x')}, {"MACRO_9", "I need a medic"}});
    const rtxt::File override_table = macros_table({{"MACRO_9", "Medic!"}});
    const auto from = profile::default_macros(&override_table, &menu);
    TEST_EXPECT(from[0] == "I'm on offense");
    TEST_EXPECT(from[1] == std::string(39, 'x'));
    TEST_EXPECT(from[2] == "??MACRO_2??");
    TEST_EXPECT(from[9] == "Medic!");
    return 0;
}

// --- 3. the fresh record [orig: PlayerProfile_InitDefaults @0x54bb40] ---------------

int test_init_defaults() {
    ProfileDefaults d = defaults();
    d.joystick_caps = 1 | 4;
    playersav::ProfileRecord record;
    record.name = "stale";
    record.sp_no_char_abilities = 1;
    playersav::Record weapons;
    profile::init_defaults(record, weapons, d, kJo);
    TEST_EXPECT(record.name.empty() && record.flags == playersav::kRecordFlagNoName);
    TEST_EXPECT(record.sp_no_char_abilities == 0 && record.sp_gps_icons == 1);
    TEST_EXPECT(record.intro_pending == -1 && record.mouse_sensitivity == 128);
    TEST_EXPECT(record.word_1468 == 127 && record.auto_reload == 1 && record.view_mode == 1);
    TEST_EXPECT(record.joystick_cap_1 == 1 && record.joystick_cap_4 == 1 && record.joystick_cap_2 == 0);
    TEST_EXPECT(record.bindings.size() == d.bindings.size());
    TEST_EXPECT(record.macros[0] == "??MACRO_0??");
    TEST_EXPECT(weapons.blue.player_class == 8 && weapons.red.player_class == 8);
    TEST_EXPECT(weapons.red.avatar_a == 7 && weapons.red.avatar_packed == 0x8207);
    TEST_EXPECT(weapons.blue.avatar_packed == 0x0200);
    TEST_EXPECT(weapons.single_player.entries.size() == 8);
    TEST_EXPECT(weapons.red.pages[4].entries[0].name == "WPN_AK74AUTO");

    // The two inner joystick seeds hang off the first [orig: @0x54bc64].
    d.joystick_caps = 2 | 4;
    profile::init_defaults(record, weapons, d, kJo);
    TEST_EXPECT(record.joystick_cap_1 == 0 && record.joystick_cap_2 == 0 && record.joystick_cap_4 == 0);
    return 0;
}

// --- 4. the merge [orig: KeyBinding_MergeSavedBindings @0x54c3f0] -----------------------

int test_merge_bindings() {
    const std::vector<playersav::BindingEntry> table = profile::default_binding_table();
    std::vector<playersav::BindingEntry> saved;
    playersav::BindingEntry forward;
    for (const auto &e : table)
        if (e.token == "move_forward") forward = e;
    forward.token = "MOVE_FORWARD";  // the token compares without case
    forward.primary = 0x49;
    forward.flags = 0;  // the identity stays the default's
    saved.push_back(forward);
    playersav::BindingEntry gone;
    gone.id = 999;
    gone.token = "gone";
    gone.primary = 1;
    saved.push_back(gone);
    playersav::BindingEntry wrong_id = forward;
    wrong_id.id = 151;  // move_back's code with move_forward's token: no match
    wrong_id.primary = 0x51;
    saved.push_back(wrong_id);
    const std::vector<playersav::BindingEntry> merged = profile::merge_bindings(table, saved);
    TEST_EXPECT(merged.size() == table.size());
    for (size_t i = 0; i < merged.size(); ++i) {
        if (merged[i].token == "move_forward") {
            TEST_EXPECT(merged[i].primary == 0x49 && merged[i].flags == table[i].flags);
            TEST_EXPECT(merged[i].token == "move_forward");
        } else {
            TEST_EXPECT(merged[i].primary == table[i].primary);
        }
    }
    return 0;
}

// --- 5. the load and the save [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0;
//        PlayerProfile_SaveToFiles @0x54be00] ------------------------------------

int test_load_without_files() {
    PlayerProfiles p(kJo);
    p.select_slot(3);
    PlayerProfiles::LoadInput in;
    in.user_name = "cdouglass";
    p.load(in, defaults());
    TEST_EXPECT(p.first_run());
    TEST_EXPECT(p.current_slot() == 3);  // the slot survives the load
    TEST_EXPECT(p.player_sav().slots[0].name == "cdouglass");
    TEST_EXPECT(p.player_sav().slots[0].flags == 0);
    for (size_t s = 1; s < playersav::kProfileSlots; ++s) {
        TEST_EXPECT(p.player_sav().slots[s].name.empty());
        TEST_EXPECT(p.player_sav().slots[s].flags == playersav::kRecordFlagNoName);
    }
    // A name the 16-byte buffer cannot hold leaves the field empty.
    in.user_name = std::string(16, 'u');
    p.load(in, defaults());
    TEST_EXPECT(p.player_sav().slots[0].name.empty() && p.player_sav().slots[0].flags == 0);

    // The save: both files' 16-byte header, player.sav's trailer.
    const std::vector<uint8_t> player = p.player_sav_bytes();
    const std::vector<uint8_t> weapon = p.weapon_sav_bytes();
    TEST_EXPECT(player.size() == playersav::kPlayerSavBytes);
    TEST_EXPECT(weapon.size() == playersav::kHeaderBytes + playersav::kProfileSlots * playersav::kRecordBytes);
    TEST_EXPECT(std::equal(player.begin(), player.begin() + 16, weapon.begin()));
    return 0;
}

int test_load_with_files() {
    // A player.sav whose slot 2 is named, flags the header and carries a
    // remapped binding beside one the defaults lack.
    playersav::PlayerSav file;
    file.flags = 1;
    file.extra = 0x03000000u;
    for (playersav::ProfileRecord &r : file.slots) r.bindings = profile::default_binding_table();
    file.slots[2].name = "Sandman";
    file.slots[2].flags = 0;
    file.slots[2].sp_no_weapon_recoil = 1;
    file.slots[2].bindings[0].primary = 0x41;
    playersav::BindingEntry extra;
    extra.id = 700;
    extra.token = "removed";
    file.slots[2].bindings.push_back(extra);
    const std::vector<uint8_t> player = playersav::write(file, kJo.profile_magic);
    playersav::File weapons = playersav::make_defaults();
    weapons.slots[2].blue.player_class = 6;
    const std::vector<uint8_t> weapon = playersav::write(weapons, kJo.profile_magic);

    PlayerProfiles p(kJo);
    p.select_slot(2);
    PlayerProfiles::LoadInput in;
    in.player_sav = &player;
    in.weapon_sav = &weapon;
    p.load(in, defaults());
    TEST_EXPECT(!p.first_run());
    TEST_EXPECT(p.current().name == "Sandman" && p.current().sp_no_weapon_recoil == 1);
    TEST_EXPECT(p.current().bindings.size() == 109);  // merged into the defaults
    TEST_EXPECT(p.current().bindings[0].primary == 0x41);
    TEST_EXPECT(p.current_weapons().blue.player_class == 6);
    // The file's record 0 overrides the account name.
    TEST_EXPECT(p.player_sav().slots[0].name.empty());

    // The header: the flag latches, the byte accumulates on every load.
    std::vector<uint8_t> saved = p.player_sav_bytes();
    TEST_EXPECT(saved[8] == 1 && saved[15] == 3);
    p.load(in, defaults());
    saved = p.player_sav_bytes();
    TEST_EXPECT(saved[8] == 1 && saved[15] == 6);
    TEST_EXPECT(p.weapon_sav_bytes()[15] == 6);

    // A bad header leaves every default, yet its extra byte still adds.
    std::vector<uint8_t> bad = player;
    bad[0] = 'X';
    in.player_sav = &bad;
    in.weapon_sav = nullptr;
    PlayerProfiles q(kJo);
    q.load(in, defaults());
    TEST_EXPECT(q.player_sav().slots[2].name.empty());
    TEST_EXPECT(q.player_sav_bytes()[15] == 3 && q.player_sav_bytes()[8] == 0);

    // A short file leaves the seeds past its end.
    std::vector<uint8_t> shorter(player.begin(), player.begin() + playersav::kHeaderBytes +
                                 2 * playersav::kPlayerRecordBytes + 100);
    in.player_sav = &shorter;
    PlayerProfiles r(kJo);
    r.select_slot(2);
    r.load(in, defaults());
    TEST_EXPECT(r.current().name == "Sandman");  // the first 100 bytes of record 2
    TEST_EXPECT(r.current().sp_no_weapon_recoil == 0);  // past the end: the seed
    return 0;
}

// --- 6. the screens and the session ------------------------------------------------

int test_name_rules() {
    PlayerProfiles p(kJo);
    PlayerProfiles::LoadInput in;
    p.load(in, defaults());
    p.select_slot(1);
    // [orig: PlayerInfo_HandlePlayerNameChange @0x55fbb0]
    TEST_EXPECT(!p.rename("Bob!"));
    TEST_EXPECT(p.current().flags == playersav::kRecordFlagNoName);
    TEST_EXPECT(p.rename("Bob.Smith-2"));
    TEST_EXPECT(p.current().name == "Bob.Smith-2" && p.current().flags == 0);
    TEST_EXPECT(p.rename(""));
    TEST_EXPECT(p.current().name.empty() && p.current().flags == 0);
    TEST_EXPECT(p.rename("ABCDEFGHIJKLMNOPQRS"));
    TEST_EXPECT(p.current().name == "ABCDEFGHIJKLMNOP");
    // [orig: save_player_info_from_dialog @0x55efa8..0x55f039]
    p.commit_name("Sandman");
    TEST_EXPECT(p.current().name == "Sandman" && p.current().flags == 0);
    p.commit_name(" Sandman");
    TEST_EXPECT(p.current().name.empty() && p.current().flags == playersav::kRecordFlagNoName);
    p.commit_name("Sandman ");
    TEST_EXPECT(p.current().name.empty());
    p.commit_name("");
    TEST_EXPECT(p.current().name.empty() && p.current().flags == playersav::kRecordFlagNoName);
    p.commit_name("ABCDEFGHIJKLMNOPQRS");
    TEST_EXPECT(p.current().name == "ABCDEFGHIJKLMNO");
    // The PLAYERNAME edit's notice runs the rule over the label once: a kept
    // text (or none) names the record and clears the no-name bit, a refused
    // last character is cut and returned for the edit's set, the cut's own set
    // reaching no handler [orig: CEditWnd_SetText @0x661900 ->
    // PlayerInfo_HandlePlayerNameChange @0x55fc35..0x55fc6d, @0x55fc91,
    // @0x55fc9b; CWnd_EmitEventToNamedHandlerAndCallbacks @0x646990..0x6469a2].
    p.current().name = "Ghost";
    p.current().flags = playersav::kRecordFlagNoName;
    TEST_EXPECT(p.name_changed("Ghost") == "Ghost");
    TEST_EXPECT(p.current().name == "Ghost" && p.current().flags == 0);
    p.current().flags = playersav::kRecordFlagNoName;
    TEST_EXPECT(p.name_changed("") == "");
    TEST_EXPECT(p.current().name.empty() && p.current().flags == 0);
    p.current().name = "Bob";
    p.current().flags = playersav::kRecordFlagNoName;
    TEST_EXPECT(p.name_changed("Bob!?") == "Bob!");
    TEST_EXPECT(p.current().name == "Bob" && p.current().flags == playersav::kRecordFlagNoName);
    // The edit holds the name as code-page bytes, one per character (D-NET-661):
    // a refused cp1252 'é' or '€' last character goes as its one byte, and the
    // two bytes of a UTF-8 'é' are two characters ("Ã©"), of which only the last
    // comes off.
    TEST_EXPECT(p.name_changed("Jos\xE9") == "Jos");
    TEST_EXPECT(p.name_changed("A\x80") == "A");
    TEST_EXPECT(p.name_changed("Ab\xC3\xA9") == "Ab\xC3");
    TEST_EXPECT(p.current().name == "Bob");
    p.current().name.clear();
    p.current().flags = 0;
    TEST_EXPECT(p.name_changed("!#") == "!");
    TEST_EXPECT(p.current().name.empty() && p.current().flags == 0);
    // The rule reads the label as the edit's text set left it: the set cut
    // the first {hot} marker (menu::MenuRuntime::set_widget_text, ctest
    // menu_runtime), so a later marker is name text, and the cut is returned
    // whole for the set to cut its next marker [orig: CButtonWnd_SetLabel
    // @0x661912 from CEditWnd_SetText; the cut's set @0x55fc6d].
    p.current().flags = playersav::kRecordFlagNoName;
    TEST_EXPECT(p.name_changed("AB{hot}C") == "AB{hot}C");
    TEST_EXPECT(p.current().name == "AB{hot}C" && p.current().flags == 0);
    // "{HOT}" is literal: its '}' is refused and cut, the record kept.
    TEST_EXPECT(p.name_changed("{HOT}") == "{HOT");
    TEST_EXPECT(p.name_changed("AB{hot}!") == "AB{hot}");
    TEST_EXPECT(p.current().name == "AB{hot}C");
    // [orig: PlayerInfo_OnDeleteQueryYes @0x561400]
    p.current().sp_no_scope_drift = 1;
    p.current_weapons().red.player_class = 5;
    p.reset_current(defaults());
    TEST_EXPECT(p.current().name.empty() && p.current().flags == playersav::kRecordFlagNoName);
    TEST_EXPECT(p.current().sp_no_scope_drift == 0 && p.current_weapons().red.player_class == 8);
    return 0;
}

int test_voice_override() {
    PlayerProfiles p(kJo);
    PlayerProfiles::LoadInput in;
    p.load(in, defaults());
    playersav::ProfileRecord &r = p.current();
    TEST_EXPECT(profile::voice_override(r, 0) == 0 && profile::voice_override(r, 1) == 0);
    // [orig: PlayerInfo_HandlePlayerVoiceSelect @0x56004c]: the shown side's byte alone, +1533 for red.
    profile::set_voice_override(r, 1, 7);
    TEST_EXPECT(r.voice[1] == 7 && r.voice[0] == 0);
    TEST_EXPECT(profile::voice_override(r, 1) == 7 && profile::voice_override(r, 0) == 0);
    profile::set_voice_override(r, 0, 2);
    TEST_EXPECT(r.voice[0] == 2 && r.voice[1] == 7);
    // The value's low byte (`mov cl, [eax+10h]` @0x56003e).
    profile::set_voice_override(r, 0, 0x10A);
    TEST_EXPECT(r.voice[0] == 0x0A);
    // No third side.
    profile::set_voice_override(r, 2, 5);
    profile::set_voice_override(r, -1, 5);
    TEST_EXPECT(r.voice[0] == 0x0A && r.voice[1] == 7 && profile::voice_override(r, 2) == 0);
    // The pair rides player.sav at +1532 / +1533.
    const std::vector<uint8_t> bytes = p.player_sav_bytes();
    PlayerProfiles reread(kJo);
    PlayerProfiles::LoadInput again;
    again.player_sav = &bytes;
    reread.load(again, defaults());
    TEST_EXPECT(profile::voice_override(reread.current(), 0) == 0x0A);
    TEST_EXPECT(profile::voice_override(reread.current(), 1) == 7);
    return 0;
}

int test_session_steps() {
    PlayerProfiles p(kJo);
    PlayerProfiles::LoadInput in;
    p.load(in, defaults());
    // [orig: SinglePlayer_StartMission @0x561b99]
    p.record_mission_start(-1);
    TEST_EXPECT(p.current().last_campaign == -1);
    // [orig: PlayerProfile_SaveRoundEndProgress]: outside a session the round must be over.
    p.current().word_1460 = 5;
    p.begin_session();
    p.current().word_1460 = 9;
    TEST_EXPECT(!p.record_round_end(false, false, 2, 3, true));
    TEST_EXPECT(p.current().word_1460 == 9);
    TEST_EXPECT(p.record_round_end(false, true, 2, 3, true));
    TEST_EXPECT(p.current().word_1460 == 5);
    TEST_EXPECT(p.current().campaign_complete[2][3] == 1 && p.current().campaigns_won == 1);
    TEST_EXPECT(p.record_round_end(false, true, -1, 0, true));
    TEST_EXPECT(p.current().campaigns_won == 1);
    TEST_EXPECT(p.record_round_end(true, false, 2, 4, true));  // a session always goes on
    TEST_EXPECT(p.current().campaign_complete[2][4] == 0);
    // A session start's class clamp stays in the record, each side on its own, and
    // the next save writes it (D-PLAYERINFO-34) [orig: Game_ApplySessionSettingsToGlobals
    // @0x5516d0..0x5516ec].
    p.current_weapons().blue.player_class = 4;
    p.current_weapons().red.player_class = 0xFF;
    p.clamp_session_classes();
    TEST_EXPECT(p.current_weapons().blue.player_class == 5);
    TEST_EXPECT(p.current_weapons().red.player_class == 9);
    p.current_weapons().blue.player_class = 7;
    p.clamp_session_classes();
    TEST_EXPECT(p.current_weapons().blue.player_class == 7);
    {
        const std::vector<uint8_t> weapons = p.weapon_sav_bytes();
        PlayerProfiles reread(kJo);
        PlayerProfiles::LoadInput again;
        again.weapon_sav = &weapons;
        reread.load(again, defaults());
        TEST_EXPECT(reread.current_weapons().red.player_class == 9);
    }
    // [orig: Game_PlayIntroVideos @0x56385e..0x563876]
    p.clear_intro_pending();
    for (const playersav::ProfileRecord &r : p.player_sav().slots) TEST_EXPECT(r.intro_pending == 0);
    return 0;
}

// --- 6b. the controls words the game reads (runtime/profile/profile_controls.h) ----

int test_controls_words() {
    PlayerProfiles p(kJo);
    PlayerProfiles::LoadInput in;
    p.load(in, defaults());
    playersav::ProfileRecord &r = p.current();
    // The session copy [orig: Game_ApplySessionSettingsToGlobals @0x551612,
    // @0x55161e, @0x551a40/@0x551a48]: a fresh record's words, then edited ones.
    profile::SessionInput s = profile::session_input(r, false);
    TEST_EXPECT(!s.invert_mouse && s.mouse_sensitivity == 128 && s.auto_reload);
    r.invert_mouse = 1;
    r.mouse_sensitivity = 300;
    r.auto_reload = 0;
    s = profile::session_input(r, false);
    TEST_EXPECT(s.invert_mouse && s.mouse_sensitivity == 300 && !s.auto_reload);
    r.auto_reload = 1;
    TEST_EXPECT(!profile::session_input(r, true).auto_reload);  // `/noreload`
    // The scopescale word: +1480 plus 0x80 (D-NET-1318) [orig:
    // Game_ApplySessionSettingsToGlobals @0x551500 - @0x551624..0x551630].
    TEST_EXPECT(s.scope_scale == 0x80 + r.word_1480);
    r.word_1480 = 50;
    TEST_EXPECT(profile::session_input(r, false).scope_scale == 0xB2);
    // The in-game Accept writes the two mouse words at once and leaves the
    // auto-reload global to the next session copy [orig: @0x55525e, @0x555271].
    profile::SessionInput held;
    held.auto_reload = false;
    const profile::SessionInput accepted = profile::ingame_accept_input(r, held);
    TEST_EXPECT(accepted.invert_mouse && accepted.mouse_sensitivity == 300 && !accepted.auto_reload);

    // A fresh record's table onto the live records changes nothing: the default
    // table is the catalog's own defaults, row for row.
    {
        controls::BindingSet fresh;
        const controls::BindingSet applied = profile::options_bindings(r);
        for (int i = 0; i < static_cast<int>(fresh.size()); ++i) {
            const controls::BindingRecord *a = fresh.record(i);
            const controls::BindingRecord *b = applied.record(i);
            TEST_EXPECT(a && b && a->primary == b->primary && a->secondary == b->secondary &&
                        a->primary_mod == b->primary_mod && a->secondary_mod == b->secondary_mod &&
                        a->mouse_mask == b->mouse_mask && a->mouse_mod == b->mouse_mod &&
                        a->joy_button == b->joy_button && a->joy_mod == b->joy_mod);
        }
    }
    // The table onto the live records [orig: KeyBinding_ApplyProfileBindings -> KeyBinding_ApplyProfileEntry]: a row
    // the table names takes the entry's fields, the joystick gate the word.
    controls::BindingSet live;
    const int forward = live.index_of_token("move_forward");
    TEST_EXPECT(forward >= 0);
    for (playersav::BindingEntry &e : r.bindings) {
        if (e.token != "move_forward") continue;
        e.primary = 'Y';
        e.secondary = 0;
        e.mouse_mask = 0x10;
    }
    TEST_EXPECT(!live.joystick_enabled());
    r.joystick_enabled = 1;
    profile::apply_controls(r, live);
    TEST_EXPECT(live.joystick_enabled());
    const controls::BindingRecord *rec = live.record(forward);
    TEST_EXPECT(rec != nullptr && rec->primary == 'Y' && rec->secondary == 0 && rec->mouse_mask == 0x10);
    r.joystick_enabled = 0;
    profile::apply_controls(r, live);
    TEST_EXPECT(!live.joystick_enabled());

    // The Options screen's records [orig: UI_BuildKeyBindingLoadoutTable
    // @0x559e50] and their store back [orig: @0x559d50]: the count and the
    // identity fields stand, the eight binding fields follow the records.
    controls::BindingSet options = profile::options_bindings(r);
    TEST_EXPECT(options.record(forward)->primary == 'Y');
    TEST_EXPECT(options.assign_key(forward, 'K', false, false, false, false));
    const size_t count = r.bindings.size();
    profile::store_bindings(options, r);
    TEST_EXPECT(r.bindings.size() == count);
    for (const playersav::BindingEntry &e : r.bindings) {
        if (e.token != "move_forward") continue;
        TEST_EXPECT(e.primary == 'Y' && e.secondary == 'K' && e.mouse_mask == 0x10);  // the empty slot
        TEST_EXPECT(e.index == controls::action_code(forward) && e.id == e.index);
    }
    // A record that round-trips through the file keeps the stored table.
    std::vector<uint8_t> image(playersav::kPlayerRecordBytes, 0);
    playersav::write_record(r, image.data());
    controls::BindingSet reread = profile::options_bindings(playersav::read_record(image.data()));
    TEST_EXPECT(reread.record(forward)->secondary == 'K');

    // The two checkboxes [orig: @0x554d36, @0x56074c; @0x55528c, @0x5552b5].
    r.auto_reload = 1;
    r.auto_medic_off = 0;
    TEST_EXPECT(profile::auto_reload_checked(r) && profile::auto_medic_checked(r));
    profile::set_auto_reload(r, false);
    profile::set_auto_medic(r, false);
    TEST_EXPECT(r.auto_reload == 0 && r.auto_medic_off == 1);
    TEST_EXPECT(!profile::auto_reload_checked(r) && !profile::auto_medic_checked(r));
    profile::set_auto_medic(r, true);
    TEST_EXPECT(r.auto_medic_off == 0);
    r.auto_medic_off = 7;  // any nonzero word shows the box clear
    TEST_EXPECT(!profile::auto_medic_checked(r));

    // DEFAULTS [orig: Options_OnDefaultsButton @0x55be93..0x55beca].
    r.mouse_sensitivity = 5;
    r.invert_mouse = r.joystick_enabled = r.invert_joystick = r.force_feedback = 1;
    profile::restore_controls_defaults(r);
    TEST_EXPECT(r.mouse_sensitivity == 128 && r.invert_mouse == 0 && r.joystick_enabled == 0);
    TEST_EXPECT(r.invert_joystick == 0 && r.force_feedback == 0);
    return 0;
}

// --- 7. the fresh record against the program's own static data ------------------

// FNV-1a 64 over each binding entry's fields, little-endian in declaration order
// (id u16, index i32, flags, modes, action_class, help u32, primary, secondary,
// primary_mod, secondary_mod, mouse_mask, mouse_mod u16, joy_button, joy_mod u8),
// then its token and a NUL.
uint64_t binding_table_digest(const std::vector<playersav::BindingEntry> &table) {
    uint64_t h = io::kFnv1a64Offset;
    const auto put = [&h](uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) h = io::fnv1a64_byte(h, static_cast<uint8_t>(v >> (8 * i)));
    };
    for (const playersav::BindingEntry &e : table) {
        put(e.id, 2);
        put(static_cast<uint32_t>(e.index), 4);
        put(e.flags, 4);
        put(e.modes, 4);
        put(e.action_class, 4);
        put(e.help, 4);
        put(e.primary, 2);
        put(e.secondary, 2);
        put(e.primary_mod, 2);
        put(e.secondary_mod, 2);
        put(e.mouse_mask, 2);
        put(e.mouse_mod, 2);
        put(e.joy_button, 1);
        put(e.joy_mod, 1);
        h = io::fnv1a64_bytes(h, e.token.data(), e.token.size());
        h = io::fnv1a64_byte(h, 0);
    }
    return h;
}

// FNV-1a 64 over a kit page: each entry's name and a NUL, then its three
// numbers as little-endian i32s.
uint64_t kit_page_digest(const playersav::KitPage &page) {
    uint64_t h = io::kFnv1a64Offset;
    const auto put32 = [&h](int32_t v) {
        for (int i = 0; i < 4; ++i) h = io::fnv1a64_byte(h, static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    };
    for (const playersav::KitEntry &e : page.entries) {
        h = io::fnv1a64_bytes(h, e.name.data(), e.name.size());
        h = io::fnv1a64_byte(h, 0);
        put32(e.ammo_primary);
        put32(e.ammo_secondary);
        put32(e.flags);
    }
    return h;
}

// The default binding table as the program builds it from its static catalog:
// the boot's sort puts the row whose id is i at place i [orig:
// KeyBinding_SortBySequentialId @0x498260, rows from 0x8159A8 to the bound
// @0x4982a4], then the walk over 768 places keeps each row flagged 0x4000000,
// at most 190 [orig: KeyBinding_BuildFilteredTable @0x54c2b0..0x54c3a5]: 109
// entries, read once from the pinned program into this digest.
constexpr size_t kProgramBindingEntries = 109;
constexpr uint64_t kProgramBindingTableDigest = UINT64_C(0xF822C6DB461D2A12);

int test_program_binding_table() {
    const std::vector<playersav::BindingEntry> table = profile::default_binding_table();
    TEST_EXPECT(table.size() == kProgramBindingEntries);
    TEST_EXPECT(binding_table_digest(table) == kProgramBindingTableDigest);
    return 0;
}

// The eleven default kit pages the fresh weapon record copies [orig:
// PlayerProfile_InitDefaults @0x54bb40], each a static double-NUL blob of
// (name, ammo, ammo, flags) quads [orig: Buffer_CopyUntilDoubleNull's source
// blobs], read once from the pinned program into these digests.
struct ProgramKitPage {
    uint32_t va;
    size_t entries;
    uint64_t digest;
};
constexpr ProgramKitPage kProgramSinglePlayerPage = {0x833BF8, 8, UINT64_C(0xEAFAC9532B3B6FA2)};
constexpr ProgramKitPage kProgramBluePages[5] = {
    {0x8353F8, 8, UINT64_C(0x482808AD8DE0E40F)}, {0x8363F8, 7, UINT64_C(0x5EC94FD329F6A478)},
    {0x8373F8, 7, UINT64_C(0x57938392AF3A0D04)}, {0x8393F8, 7, UINT64_C(0x81BC3067E26EE3E1)},
    {0x83A3F8, 7, UINT64_C(0x37786A67C3B02B01)},
};
constexpr ProgramKitPage kProgramRedPages[5] = {
    {0x835BF8, 8, UINT64_C(0xE4672FE97B594585)}, {0x836BF8, 7, UINT64_C(0x4BCD9741B57B04B2)},
    {0x837BF8, 7, UINT64_C(0xE87F81250371CFB3)}, {0x839BF8, 7, UINT64_C(0xD5386517F140B5A1)},
    {0x83ABF8, 7, UINT64_C(0x54D39615DD2597F1)},
};
// The profile-less spawn kit copies the single-player page whole: both sites
// push its address [orig: push 0x833BF8 @0x5246be and @0x5519da].
constexpr uint8_t kPushSinglePlayerPage[5] = {0x68, 0xF8, 0x3B, 0x83, 0x00};

bool page_matches(const playersav::KitPage &page, const ProgramKitPage &pin) {
    return page.entries.size() == pin.entries && kit_page_digest(page) == pin.digest;
}

int test_program_kit_pages() {
    TEST_EXPECT(page_matches(playersav::default_single_player_page(), kProgramSinglePlayerPage));
    const uint32_t pushed = static_cast<uint32_t>(kPushSinglePlayerPage[1]) |
                            (static_cast<uint32_t>(kPushSinglePlayerPage[2]) << 8) |
                            (static_cast<uint32_t>(kPushSinglePlayerPage[3]) << 16) |
                            (static_cast<uint32_t>(kPushSinglePlayerPage[4]) << 24);
    TEST_EXPECT(kPushSinglePlayerPage[0] == 0x68 && pushed == kProgramSinglePlayerPage.va);
    for (uint8_t i = 0; i < 5; ++i) {
        const uint8_t klass = static_cast<uint8_t>(playersav::kMinPlayerClass + i);
        TEST_EXPECT(page_matches(playersav::default_kit_page(playersav::SideId::Blue, klass), kProgramBluePages[i]));
        TEST_EXPECT(page_matches(playersav::default_kit_page(playersav::SideId::Red, klass), kProgramRedPages[i]));
    }
    return 0;
}

// --- 7b. the controls words' readers and writers in the program -----------------

// The instructions profile_controls.h ports, as the pinned program holds them:
// each reads or writes a record word at the displacement it encodes, which must
// be the offset the record layout gives that word (and the DEFAULTS store's
// immediate the fresh record's seed). The joystick dispatch's gate
// (`cmp dword_24D2088, 0` @0x499481) addresses the global the session copy
// fills, not a record word, so it has no offset to check.
int test_program_controls_code() {
    struct Site {
        uint32_t va;
        std::vector<uint8_t> bytes;
        size_t disp_at;    // where the instruction's disp32 sits
        const char *word;  // the record word it addresses
    };
    const Site sites[] = {
        // The session copy: +1432, +1428, +1424, +1524.
        {0x5515c0, {0x8B, 0x88, 0x98, 0x05, 0x00, 0x00}, 2, "joystick_enabled"},  // mov ecx, [eax+598h]
        {0x55160c, {0x8B, 0x88, 0x94, 0x05, 0x00, 0x00}, 2, "invert_mouse"},      // mov ecx, [eax+594h]
        {0x551618, {0x8B, 0x90, 0x90, 0x05, 0x00, 0x00}, 2, "mouse_sensitivity"}, // mov edx, [eax+590h]
        {0x551a3a, {0x8B, 0x91, 0xF4, 0x05, 0x00, 0x00}, 2, "auto_reload"},       // mov edx, [ecx+5F4h]
        // The in-game Accept's inverted OPTIONS_AUTOMEDIC store to +1660.
        {0x5552b5, {0xF7, 0xD8, 0x1B, 0xC0, 0x83, 0xC0, 0x01, 0x89, 0x82, 0x7C, 0x06, 0x00, 0x00}, 9,
         "auto_medic_off"},
        // C2S 0x03's body: the current record's +1660, raw.
        {0x42a40b, {0xA1, 0xFC, 0x10, 0x55, 0x02, 0x56, 0x8B, 0xB0, 0x7C, 0x06, 0x00, 0x00}, 8, "auto_medic_off"},
        // DEFAULTS: +1424 = 0x80.
        {0x55be93, {0xC7, 0x82, 0x90, 0x05, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00}, 2, "mouse_sensitivity"},
    };
    const auto le32 = [](const std::vector<uint8_t> &b, size_t at) {
        return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
               (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
    };
    for (const Site &site : sites) {
        const playersav::RecordWord *word = playersav::find_record_word(site.word);
        TEST_EXPECT(word != nullptr && site.disp_at + 4 <= site.bytes.size());
        if (word == nullptr || site.disp_at + 4 > site.bytes.size()) continue;
        TEST_EXPECT(le32(site.bytes, site.disp_at) == word->offset);
    }
    // The DEFAULTS store's immediate is the fresh record's sensitivity.
    TEST_EXPECT(static_cast<int32_t>(le32(sites[6].bytes, 6)) == playersav::ProfileRecord{}.mouse_sensitivity);
    return 0;
}

// --- 8. the shipped default weapon.sav, loaded and saved in memory ---------------

int test_default_weapon_sav() {
    const std::vector<uint8_t> bytes = playersav::write(test_weapon_sav::default_file(), kJo.profile_magic);
    TEST_EXPECT(test_weapon_sav::fnv1a64(bytes) == test_weapon_sav::kDefaultWeaponSavFnv1a64);
    PlayerProfiles p(kJo);
    PlayerProfiles::LoadInput in;
    in.weapon_sav = &bytes;
    p.load(in, defaults());
    // The records the file holds come back out as they went in; the header's
    // extra byte is the profile's own (player.sav's accumulated count).
    const std::vector<uint8_t> saved = p.weapon_sav_bytes();
    TEST_EXPECT(saved.size() == bytes.size());
    TEST_EXPECT(saved.size() > 16 && std::equal(saved.begin(), saved.begin() + 8, bytes.begin()));
    TEST_EXPECT(saved.size() == bytes.size() && std::equal(saved.begin() + 16, saved.end(), bytes.begin() + 16));
    return 0;
}

// --- 9. the title's magic (DFX2-4) ------------------------------------------------
// Both files open with the title's magic: dfx2.exe's load gates on "2XFD" and its save writes
// it, so each game refuses the other's files and keeps its seeds [orig: PlayerProfile_SaveToFiles
// @0x549080 - @0x5490ae; PlayerProfile_LoadAllFromDisk @0x54c720 - @0x54c7dc, the gates @0x54c800
// and @0x54c93f (dfx2.exe)].

int test_title_magic() {
    PlayerProfiles jo(kJo);
    PlayerProfiles::LoadInput none;
    jo.load(none, defaults());
    jo.select_slot(1);
    jo.current().name = "Sandman";
    jo.current_weapons().blue.player_class = 6;
    const std::vector<uint8_t> jo_player = jo.player_sav_bytes();
    const std::vector<uint8_t> jo_weapon = jo.weapon_sav_bytes();
    TEST_EXPECT(std::memcmp(jo_player.data(), "FPBC0211", 8) == 0);
    TEST_EXPECT(std::memcmp(jo_weapon.data(), "FPBC0211", 8) == 0);

    const Title &dfx2 = games::dfx2_title();
    PlayerProfiles d(dfx2);
    d.select_slot(1);
    PlayerProfiles::LoadInput in;
    in.player_sav = &jo_player;
    in.weapon_sav = &jo_weapon;
    d.load(in, defaults());
    // JO's files are there, and refused: the seeds stand.
    TEST_EXPECT(!d.first_run());
    TEST_EXPECT(d.current().name.empty() && d.current_weapons().blue.player_class == 8);
    d.current().name = "Sandman";
    d.current_weapons().blue.player_class = 6;
    const std::vector<uint8_t> dfx2_player = d.player_sav_bytes();
    const std::vector<uint8_t> dfx2_weapon = d.weapon_sav_bytes();
    TEST_EXPECT(std::memcmp(dfx2_player.data(), "2XFD0211", 8) == 0);
    TEST_EXPECT(std::memcmp(dfx2_weapon.data(), "2XFD0211", 8) == 0);

    // DFX2's files read back under its own title and are refused under JO's.
    in.player_sav = &dfx2_player;
    in.weapon_sav = &dfx2_weapon;
    PlayerProfiles back(dfx2);
    back.select_slot(1);
    back.load(in, defaults());
    TEST_EXPECT(back.current().name == "Sandman" && back.current_weapons().blue.player_class == 6);
    PlayerProfiles refused(kJo);
    refused.select_slot(1);
    refused.load(in, defaults());
    TEST_EXPECT(refused.current().name.empty() && refused.current_weapons().blue.player_class == 8);
    return 0;
}

// --- 10. the title's record 0 name ------------------------------------------------
// dfx2.exe names the fresh record 0 "Bravo 1" where Jointops.exe reads the account
// [orig: PlayerProfile_LoadAllFromDisk @0x54c720 - @0x54c770..0x54c78a (dfx2.exe)]; player.sav's
// record 0 still reads over it.

int test_title_first_name() {
    PlayerProfiles d(games::dfx2_title());
    PlayerProfiles::LoadInput in;
    in.user_name = "cdouglass";
    d.load(in, defaults());
    TEST_EXPECT(d.player_sav().slots[0].name == "Bravo 1" && d.player_sav().slots[0].flags == 0);
    TEST_EXPECT(d.player_sav().slots[1].name.empty());
    in.user_name = std::string(16, 'u');
    d.load(in, defaults());
    TEST_EXPECT(d.player_sav().slots[0].name == "Bravo 1");
    d.player_sav().slots[0].name = "Sandman";
    const std::vector<uint8_t> player = d.player_sav_bytes();
    in.player_sav = &player;
    PlayerProfiles back(games::dfx2_title());
    back.load(in, defaults());
    TEST_EXPECT(back.player_sav().slots[0].name == "Sandman");
    return 0;
}

// --- 11. the title's kit pages (DFX2-29) ----------------------------------------------
// dfx2.exe's fresh weapon record takes its own pages: the single-player page at 0x832368, and on
// each side classes 7 and 8 the XM4AUTO and M4AUTO pages, classes 5, 6 and 9 empty pages (static
// buffers nothing writes) [orig: PlayerProfile_InitDefaults @0x548db0 - @0x548f67,
// @0x548f94..0x548fe0 (side 1), @0x548ff9..0x549045 (side 0) (dfx2.exe)].

int test_title_kit_pages() {
    const auto names = [](const playersav::KitPage &page) {
        std::string out;
        for (const playersav::KitEntry &e : page.entries) out += e.name + " ";
        return out;
    };
    playersav::ProfileRecord record;
    playersav::Record weapons;
    profile::init_defaults(record, weapons, defaults(), games::dfx2_title());
    TEST_EXPECT(names(weapons.single_player) ==
            "WPN_M4AUTO WPN_M4 WPN_MK23 WPN_AT4 WPN_GRENADEFB WPN_GRENADEHE WPN_GRENADESM WPN_KNIFE ");
    const std::string xm4 = "WPN_XM4AUTO WPN_XM4 WPN_XMK23 WPN_XAT4 WPN_GRENADEFB WPN_GRENADEHE "
                            "WPN_GRENADESM WPN_KNIFE ";
    const std::string m4 = "WPN_M4AUTO WPN_MK23 WPN_SATCHEL_CHARGE WPN_GRENADEFB WPN_GRENADEHE "
                           "WPN_GRENADESM WPN_KNIFE ";
    for (const playersav::Side *side : {&weapons.blue, &weapons.red}) {
        TEST_EXPECT(side->pages[0].entries.empty() && side->pages[1].entries.empty());
        TEST_EXPECT(names(side->pages[2]) == xm4);
        TEST_EXPECT(names(side->pages[3]) == m4);
        TEST_EXPECT(side->pages[4].entries.empty());
        TEST_EXPECT(side->player_class == 8);
    }
    TEST_EXPECT(weapons.blue.pages[2].entries[0].ammo_primary == -1);
    // A DFX2 profile's load seeds every record so.
    PlayerProfiles p(games::dfx2_title());
    PlayerProfiles::LoadInput none;
    p.load(none, defaults());
    TEST_EXPECT(names(p.weapon_sav().slots[4].red.pages[3]) == m4);
    TEST_EXPECT(p.weapon_sav().slots[0].blue.pages[0].entries.empty());
    // JO's stay the format's.
    profile::init_defaults(record, weapons, defaults(), kJo);
    TEST_EXPECT(weapons.red.pages[4].entries[0].name == "WPN_AK74AUTO");
    TEST_EXPECT(weapons.single_player.entries[2].name == "WPN_colt45");
    // A fresh weapon record (the session start's and the 0x2F's with no weapon.sav) is the same
    // weapon half over a cleared record: JO's the format's defaults, dfx2.exe's its own pages, no
    // title's no page, each with both class bytes 8 and the avatar bytes 0.
    // [orig: PlayerProfile_InitDefaults @0x54bb40; @0x548db0 (dfx2.exe)]
    const playersav::Record jo = profile::fresh_weapon_record(&kJo);
    const playersav::Record shipped = playersav::make_defaults().slots[0];
    TEST_EXPECT(names(jo.single_player) == names(shipped.single_player));
    for (size_t i = 0; i < playersav::kKitPagesPerSide; ++i) {
        TEST_EXPECT(names(jo.blue.pages[i]) == names(shipped.blue.pages[i]));
        TEST_EXPECT(names(jo.red.pages[i]) == names(shipped.red.pages[i]));
    }
    const playersav::Record dfx2 = profile::fresh_weapon_record(&games::dfx2_title());
    TEST_EXPECT(names(dfx2.red.pages[3]) == m4 && names(dfx2.blue.pages[2]) == xm4 &&
            dfx2.red.pages[4].entries.empty());
    const playersav::Record untitled = profile::fresh_weapon_record(nullptr);
    TEST_EXPECT(untitled.single_player.entries.empty() && untitled.red.pages[3].entries.empty());
    for (const playersav::Record *r : {&jo, &dfx2, &untitled}) {
        TEST_EXPECT(r->blue.player_class == 8 && r->red.player_class == 8);
        TEST_EXPECT(r->blue.avatar_a == 0 && r->red.avatar_packed == 0);
    }
    return 0;
}

}  // namespace

int main() {
    struct Case {
        const char *name;
        int (*fn)();
    };
    const Case cases[] = {
        {"default_binding_table", test_default_binding_table},
        {"default_macros", test_default_macros},
        {"init_defaults", test_init_defaults},
        {"merge_bindings", test_merge_bindings},
        {"load_without_files", test_load_without_files},
        {"load_with_files", test_load_with_files},
        {"name_rules", test_name_rules},
        {"voice_override", test_voice_override},
        {"session_steps", test_session_steps},
        {"controls_words", test_controls_words},
        {"program_binding_table", test_program_binding_table},
        {"program_kit_pages", test_program_kit_pages},
        {"program_controls_code", test_program_controls_code},
        {"default_weapon_sav", test_default_weapon_sav},
        {"title_magic", test_title_magic},
        {"title_first_name", test_title_first_name},
        {"title_kit_pages", test_title_kit_pages},
    };
    int failures = 0;
    for (const Case &c : cases) {
        if (c.fn() != 0) {
            std::fprintf(stderr, "FAILED: %s\n", c.name);
            ++failures;
        } else {
            std::printf("ok: %s\n", c.name);
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d player_profiles test case(s) failed\n", failures);
        return 1;
    }
    std::printf("player_profiles: all cases passed\n");
    return 0;
}
