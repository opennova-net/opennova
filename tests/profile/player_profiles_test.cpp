// engine/runtime/profile — the player profile's in-memory image: the fresh
// record's seeds, the binding merge, the load and the save, the screens' name
// rules, the session steps and the controls words the game reads
// (profile_controls.h). Fixtures are built in code; the retail legs
// check the fresh record's tables against the installed program's own static
// data (Jointops.exe, read only) and load and save the install's weapon.sav in
// memory (docs/asset-gated-tests.md).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/pe_image.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include <formats/playersav/player_sav.h>
#include <formats/playersav/weapon_sav.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/controls/controls.h>
#include <runtime/controls/binding_set.h>
#include <runtime/profile/player_profiles.h>
#include <runtime/profile/profile_controls.h>

using namespace opennova;
using opennova::profile::PlayerProfiles;
using opennova::profile::ProfileDefaults;

namespace {

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
    profile::init_defaults(record, weapons, d);
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
    profile::init_defaults(record, weapons, d);
    TEST_EXPECT(record.joystick_cap_1 == 0 && record.joystick_cap_2 == 0 && record.joystick_cap_4 == 0);
    return 0;
}

// --- 4. the merge [orig: merge_weapon_slot_params @0x54c3f0] -----------------------

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
    PlayerProfiles p;
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
    const std::vector<uint8_t> player = playersav::write(file);
    playersav::File weapons = playersav::make_defaults();
    weapons.slots[2].blue.player_class = 6;
    const std::vector<uint8_t> weapon = playersav::write(weapons);

    PlayerProfiles p;
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
    PlayerProfiles q;
    q.load(in, defaults());
    TEST_EXPECT(q.player_sav().slots[2].name.empty());
    TEST_EXPECT(q.player_sav_bytes()[15] == 3 && q.player_sav_bytes()[8] == 0);

    // A short file leaves the seeds past its end.
    std::vector<uint8_t> shorter(player.begin(), player.begin() + playersav::kHeaderBytes +
                                 2 * playersav::kPlayerRecordBytes + 100);
    in.player_sav = &shorter;
    PlayerProfiles r;
    r.select_slot(2);
    r.load(in, defaults());
    TEST_EXPECT(r.current().name == "Sandman");  // the first 100 bytes of record 2
    TEST_EXPECT(r.current().sp_no_weapon_recoil == 0);  // past the end: the seed
    return 0;
}

// --- 6. the screens and the session ------------------------------------------------

int test_name_rules() {
    PlayerProfiles p;
    PlayerProfiles::LoadInput in;
    p.load(in, defaults());
    p.select_slot(1);
    // [orig: handle_player_name_change @0x55fbb0]
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
    // [orig: sub_561400 @0x561400]
    p.current().sp_no_scope_drift = 1;
    p.current_weapons().red.player_class = 5;
    p.reset_current(defaults());
    TEST_EXPECT(p.current().name.empty() && p.current().flags == playersav::kRecordFlagNoName);
    TEST_EXPECT(p.current().sp_no_scope_drift == 0 && p.current_weapons().red.player_class == 8);
    return 0;
}

int test_session_steps() {
    PlayerProfiles p;
    PlayerProfiles::LoadInput in;
    p.load(in, defaults());
    // [orig: SinglePlayer_StartMission @0x561b99]
    p.record_mission_start(-1);
    TEST_EXPECT(p.current().last_campaign == -1);
    // [orig: sub_54D6A0]: outside a session the round must be over.
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
    // [orig: Game_PlayIntroVideos @0x56385e..0x563876]
    p.clear_intro_pending();
    for (const playersav::ProfileRecord &r : p.player_sav().slots) TEST_EXPECT(r.intro_pending == 0);
    return 0;
}

// --- 6b. the controls words the game reads (runtime/profile/profile_controls.h) ----

int test_controls_words() {
    PlayerProfiles p;
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
    // The table onto the live records [orig: sub_562E60 -> sub_562DF0]: a row
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

    // DEFAULTS [orig: sub_55BD90 @0x55be93..0x55beca].
    r.mouse_sensitivity = 5;
    r.invert_mouse = r.joystick_enabled = r.invert_joystick = r.force_feedback = 1;
    profile::restore_controls_defaults(r);
    TEST_EXPECT(r.mouse_sensitivity == 128 && r.invert_mouse == 0 && r.joystick_enabled == 0);
    TEST_EXPECT(r.invert_joystick == 0 && r.force_feedback == 0);
    return 0;
}

// --- 7. retail: the fresh record against the installed program's static data -------

uint32_t le(const std::vector<uint8_t> &b, size_t at, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint32_t>(b[at + i]) << (8 * i);
    return v;
}

// The default binding table as the program builds it from its static catalog:
// the boot's sort puts the row whose id is i at place i [orig:
// KeyBinding_SortBySequentialId @0x498260, rows from 0x8159A8 to the bound
// @0x4982a4], then the walk over 768 places keeps each row flagged 0x4000000,
// at most 190 [orig: KeyBinding_BuildFilteredTable @0x54c2b0..0x54c3a5].
bool program_binding_table(const pe::Image &image, std::vector<playersav::BindingEntry> &out) {
    constexpr uint32_t kCatalog = 0x8159A8;
    constexpr size_t kStride = 108;
    uint32_t bound = 0;
    std::vector<uint8_t> t;
    if (!image.u32(0x4982a5, bound) || bound <= kCatalog || (bound - kCatalog) % kStride != 0 ||
        !image.read(kCatalog, bound - kCatalog, t))
        return false;
    const size_t rows = t.size() / kStride;
    const auto id_of = [&](size_t r) { return static_cast<int>(static_cast<int16_t>(le(t, r * kStride, 2))); };
    for (size_t i = 0; i < rows; ++i) {
        const int id = id_of(i);
        if (id >= 768 || id == static_cast<int>(i)) continue;
        for (size_t j = 0; j < rows; ++j) {
            if (id_of(j) != static_cast<int>(i)) continue;
            std::swap_ranges(t.begin() + static_cast<std::ptrdiff_t>(i * kStride),
                             t.begin() + static_cast<std::ptrdiff_t>((i + 1) * kStride),
                             t.begin() + static_cast<std::ptrdiff_t>(j * kStride));
            break;
        }
    }
    out.clear();
    for (size_t place = 0; place < 768 && place < rows && out.size() < playersav::kBindingCapacity; ++place) {
        const size_t r = place * kStride;
        const uint32_t flags = le(t, r + 4, 4);
        if ((flags & 0x4000000u) == 0) continue;
        playersav::BindingEntry e;
        e.id = static_cast<uint16_t>(le(t, r, 2));
        e.index = static_cast<int32_t>(place);
        e.flags = flags;
        e.modes = le(t, r + 8, 4);
        e.action_class = t[r + 12];
        e.help = le(t, r + 16, 4);
        e.primary = static_cast<uint16_t>(le(t, r + 20, 2));
        e.secondary = static_cast<uint16_t>(le(t, r + 22, 2));
        e.mouse_mask = static_cast<uint16_t>(le(t, r + 24, 2));
        e.joy_button = t[r + 26];
        e.primary_mod = static_cast<uint16_t>(le(t, r + 28, 2));
        e.secondary_mod = static_cast<uint16_t>(le(t, r + 30, 2));
        e.mouse_mod = static_cast<uint16_t>(le(t, r + 32, 2));
        e.joy_mod = t[r + 34];
        // The row's token, NUL-terminated inside the row [orig: @0x54c37a..0x54c38d].
        for (size_t c = r + 75; c < r + kStride && t[c] != 0; ++c) e.token.push_back(static_cast<char>(t[c]));
        out.push_back(std::move(e));
    }
    return true;
}

// A static default kit page: NUL-separated (name, ammo, ammo, flags) quads that
// end at an empty string [orig: Buffer_CopyUntilDoubleNull's source blobs].
bool program_kit_page(const pe::Image &image, uint32_t va, playersav::KitPage &out) {
    out.entries.clear();
    std::vector<std::string> fields;
    for (uint32_t at = va;;) {
        std::string field = image.c_string(at);
        if (field.empty()) break;
        at += static_cast<uint32_t>(field.size()) + 1;
        fields.push_back(std::move(field));
        if (fields.size() > 64) return false;
    }
    if (fields.size() % 4 != 0) return false;
    for (size_t i = 0; i < fields.size(); i += 4) {
        playersav::KitEntry e;
        e.name = fields[i];
        e.ammo_primary = std::atoi(fields[i + 1].c_str());
        e.ammo_secondary = std::atoi(fields[i + 2].c_str());
        e.flags = std::atoi(fields[i + 3].c_str());
        out.entries.push_back(std::move(e));
    }
    return true;
}

bool same_page(const playersav::KitPage &a, const playersav::KitPage &b) {
    if (a.entries.size() != b.entries.size()) return false;
    for (size_t i = 0; i < a.entries.size(); ++i) {
        const playersav::KitEntry &x = a.entries[i];
        const playersav::KitEntry &y = b.entries[i];
        if (x.name != y.name || x.ammo_primary != y.ammo_primary || x.ammo_secondary != y.ammo_secondary ||
            x.flags != y.flags)
            return false;
    }
    return true;
}

int test_retail_program() {
    const std::string exe = retail::jointops_exe();
    if (exe.empty()) return retail::skip_leg("OPENNOVA_JO_DIR carrying Jointops.exe");
    pe::Image image;
    if (!image.open(exe)) {
        std::fprintf(stderr, "Jointops.exe found but not a readable PE32 image: %s\n", exe.c_str());
        return 1;
    }
    // The default binding table, entry for entry.
    std::vector<playersav::BindingEntry> program;
    TEST_EXPECT(program_binding_table(image, program));
    const std::vector<playersav::BindingEntry> table = profile::default_binding_table();
    TEST_EXPECT(program.size() == table.size());
    for (size_t i = 0; i < std::min(program.size(), table.size()); ++i) {
        const playersav::BindingEntry &a = program[i];
        const playersav::BindingEntry &b = table[i];
        TEST_EXPECT(a.id == b.id && a.index == b.index && a.token == b.token);
        TEST_EXPECT(a.flags == b.flags && a.modes == b.modes && a.action_class == b.action_class);
        TEST_EXPECT(a.help == b.help);
        TEST_EXPECT(a.primary == b.primary && a.secondary == b.secondary);
        TEST_EXPECT(a.primary_mod == b.primary_mod && a.secondary_mod == b.secondary_mod);
        TEST_EXPECT(a.mouse_mask == b.mouse_mask && a.mouse_mod == b.mouse_mod);
        TEST_EXPECT(a.joy_button == b.joy_button && a.joy_mod == b.joy_mod);
    }
    // The eleven default kit pages the fresh weapon record copies [orig:
    // PlayerProfile_InitDefaults @0x54bb40]; the profile-less spawn kit copies
    // the single-player one whole [orig: push 0x833BF8 @0x5246be and @0x5519da].
    playersav::KitPage page;
    TEST_EXPECT(program_kit_page(image, 0x833BF8, page) &&
                same_page(page, playersav::default_single_player_page()));
    const uint8_t push_sp[] = {0x68, 0xF8, 0x3B, 0x83, 0x00};
    std::vector<uint8_t> b;
    TEST_EXPECT(image.read(0x5246be, 5, b) && std::memcmp(b.data(), push_sp, 5) == 0);
    TEST_EXPECT(image.read(0x5519da, 5, b) && std::memcmp(b.data(), push_sp, 5) == 0);
    const uint32_t blue[5] = {0x8353F8, 0x8363F8, 0x8373F8, 0x8393F8, 0x83A3F8};
    const uint32_t red[5] = {0x835BF8, 0x836BF8, 0x837BF8, 0x839BF8, 0x83ABF8};
    for (uint8_t i = 0; i < 5; ++i) {
        const uint8_t klass = static_cast<uint8_t>(playersav::kMinPlayerClass + i);
        TEST_EXPECT(program_kit_page(image, blue[i], page) &&
                    same_page(page, playersav::default_kit_page(playersav::SideId::Blue, klass)));
        TEST_EXPECT(program_kit_page(image, red[i], page) &&
                    same_page(page, playersav::default_kit_page(playersav::SideId::Red, klass)));
    }
    std::printf("Jointops.exe: the %zu-entry default binding table and the 11 default kit pages match\n",
                program.size());
    return 0;
}

// --- 7b. retail: the controls words' readers and writers in the installed program ---

// The instructions profile_controls.h ports, read from the installed
// Jointops.exe: each reads or writes the record offset the port names.
int test_retail_controls_code() {
    const std::string exe = retail::jointops_exe();
    if (exe.empty()) return retail::skip_leg("OPENNOVA_JO_DIR carrying Jointops.exe");
    pe::Image image;
    if (!image.open(exe)) {
        std::fprintf(stderr, "Jointops.exe found but not a readable PE32 image: %s\n", exe.c_str());
        return 1;
    }
    struct Site {
        uint32_t va;
        std::vector<uint8_t> bytes;
    };
    const Site sites[] = {
        // The session copy: +1432, +1428, +1424, +1524.
        {0x5515c0, {0x8B, 0x88, 0x98, 0x05, 0x00, 0x00}},  // mov ecx, [eax+598h]
        {0x55160c, {0x8B, 0x88, 0x94, 0x05, 0x00, 0x00}},  // mov ecx, [eax+594h]
        {0x551618, {0x8B, 0x90, 0x90, 0x05, 0x00, 0x00}},  // mov edx, [eax+590h]
        {0x551a3a, {0x8B, 0x91, 0xF4, 0x05, 0x00, 0x00}},  // mov edx, [ecx+5F4h]
        // The joystick dispatch's gate: cmp dword_24D2088, 0.
        {0x499481, {0x83, 0x3D, 0x88, 0x20, 0x4D, 0x02, 0x00}},
        // The in-game Accept's inverted OPTIONS_AUTOMEDIC store to +1660.
        {0x5552b5, {0xF7, 0xD8, 0x1B, 0xC0, 0x83, 0xC0, 0x01, 0x89, 0x82, 0x7C, 0x06, 0x00, 0x00}},
        // C2S 0x03's body: the current record's +1660, raw.
        {0x42a40b, {0xA1, 0xFC, 0x10, 0x55, 0x02, 0x56, 0x8B, 0xB0, 0x7C, 0x06, 0x00, 0x00}},
        // DEFAULTS: +1424 = 0x80.
        {0x55be93, {0xC7, 0x82, 0x90, 0x05, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00}},
    };
    for (const Site &site : sites) {
        std::vector<uint8_t> b;
        TEST_EXPECT(image.read(site.va, site.bytes.size(), b) && b == site.bytes);
    }
    std::printf("Jointops.exe: the controls words' %zu reader and writer sites match\n",
                sizeof(sites) / sizeof(sites[0]));
    return 0;
}

// --- 8. retail: the install's weapon.sav, loaded and saved in memory ---------------

int test_retail_weapon_sav() {
    const std::string sav = retail::weapon_sav();
    if (sav.empty()) return retail::skip_leg("OPENNOVA_JO_DIR carrying a retail weapon.sav");
    std::vector<uint8_t> bytes;
    if (!test_io::read_file(sav.c_str(), bytes)) {
        std::fprintf(stderr, "retail weapon.sav found but unreadable: %s\n", sav.c_str());
        return 1;
    }
    PlayerProfiles p;
    PlayerProfiles::LoadInput in;
    in.weapon_sav = &bytes;
    p.load(in, defaults());
    // The records the file holds come back out as they went in; the header's
    // extra byte is the profile's own (player.sav's accumulated count).
    const std::vector<uint8_t> saved = p.weapon_sav_bytes();
    TEST_EXPECT(saved.size() == bytes.size());
    TEST_EXPECT(saved.size() > 16 && std::equal(saved.begin(), saved.begin() + 8, bytes.begin()));
    TEST_EXPECT(saved.size() == bytes.size() && std::equal(saved.begin() + 16, saved.end(), bytes.begin() + 16));
    std::printf("retail weapon.sav: %zu bytes; the profile's load and save keep every record\n", bytes.size());
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
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
        {"session_steps", test_session_steps},
        {"controls_words", test_controls_words},
        {"retail_program", test_retail_program},
        {"retail_controls_code", test_retail_controls_code},
        {"retail_weapon_sav", test_retail_weapon_sav},
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
