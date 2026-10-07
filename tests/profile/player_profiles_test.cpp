// engine/runtime/profile — the player profile's in-memory image: the fresh
// record's seeds, the binding merge, the load and the save, the screens' name
// rules and the session steps. Fixtures are built in code; the optional last
// leg loads the install's own player.sav (read only) and saves it in memory
// (docs/asset-gated-tests.md).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include <formats/playersav/player_sav.h>
#include <formats/playersav/weapon_sav.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/controls/controls.h>
#include <runtime/profile/player_profiles.h>

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

// --- 7. optional: the install's own player.sav, loaded and saved in memory -----------

int test_retail_file() {
    const std::string sav = retail::player_sav();
    if (sav.empty()) return retail::skip_leg("OPENNOVA_JO_DIR carrying a retail player.sav (corpus leg)");
    std::vector<uint8_t> bytes;
    if (!test_io::read_file(sav.c_str(), bytes)) {
        std::fprintf(stderr, "retail player.sav found but unreadable: %s\n", sav.c_str());
        return 1;
    }
    playersav::PlayerSav file;
    TEST_EXPECT(playersav::read(bytes.data(), bytes.size(), file));
    // The shipped binding table is the default table this catalog builds: the
    // same rows, codes, flags, help ids, defaults and tokens.
    const std::vector<playersav::BindingEntry> table = profile::default_binding_table();
    size_t remapped = 0;
    for (const playersav::ProfileRecord &r : file.slots) {
        TEST_EXPECT(r.bindings.size() == table.size());
        for (size_t i = 0; i < table.size(); ++i) {
            const playersav::BindingEntry &a = r.bindings[i];
            const playersav::BindingEntry &b = table[i];
            TEST_EXPECT(a.id == b.id && a.index == b.index && a.token == b.token);
            TEST_EXPECT(a.flags == b.flags && a.modes == b.modes && a.action_class == b.action_class);
            TEST_EXPECT(a.help == b.help);
            if (a.primary != b.primary || a.secondary != b.secondary || a.primary_mod != b.primary_mod ||
                a.mouse_mask != b.mouse_mask || a.mouse_mod != b.mouse_mod ||
                a.joy_button != b.joy_button || a.joy_mod != b.joy_mod)
                ++remapped;
        }
    }
    // The load and the save in memory: with an unremapped table the profile
    // writes the file it read, byte for byte.
    PlayerProfiles p;
    PlayerProfiles::LoadInput in;
    in.player_sav = &bytes;
    p.load(in, defaults());
    if (remapped == 0) TEST_EXPECT(p.player_sav_bytes() == bytes);
    std::printf("retail player.sav: %zu bindings a record, %zu remapped; load+save %s\n", table.size(),
                remapped, p.player_sav_bytes() == bytes ? "byte-identical" : "differs");
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
        {"retail_file", test_retail_file},
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
