// npruntime_host_settings: the MULTI_PLAYER_HOST screen over the process's cfg block
// (runtime/inmatch/host_settings.h): START's read of each control into the block, the
// populate's values from the block and its cap clamp, the title caps, and the session cap
// and slot-limit rules.
// [orig: HostDialog_ReadSettings @0x555940; UI_PopulateHostSettingsFromConfig @0x555FE0;
//  CNapiGameSession_BuildAndCreateSession @0x56955D..0x56956C; Game_ApplySessionSettingsToGlobals
//  @0x551B26..0x551B48]
#include <runtime/inmatch/host_settings.h>
#include "common/test_expect.h"
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_state_frame.h>
#include <runtime/menu/menu_text_tables.h>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace opennova::inmatch;
namespace gamecfg = opennova::gamecfg;

namespace {
// The titles whose caps the screen and the session take (base/title/title.h max_players).
const opennova::Title &kJo = *opennova::title_by_code("jo");
const opennova::Title &kDfx2 = *opennova::title_by_code("dfx2");

// dfx2.exe's caps (docs/titles/dfx2-vs-jo-re.md DFX2-8): the host screen 100, the session create,
// the apply's ceiling and the slot table 251, the same code over other words; Jointops.exe's stand.
// [orig: HostDialog_ReadSettings @0x553af0, g_HostMaxPlayersCap @0x835f0c;
//  CNapiGameSession_BuildAndCreateSession @0x567707; Game_ApplySessionSettingsToGlobals @0x54ed93
//  (dfx2.exe)]
int title_caps() {
    TEST_EXPECT(title_player_caps(kJo).session == 65 && title_player_caps(kJo).host_screen == 64);
    TEST_EXPECT(title_player_caps(kDfx2).session == 251 && title_player_caps(kDfx2).host_screen == 100);
    gamecfg::GameCfg dfx2;
    dfx2.table = gamecfg::ConfigTable::Dfx2;
    TEST_EXPECT(apply_host_dialog_control(dfx2, kDfx2, "MAX_PLAYERS", "99"));
    TEST_EXPECT(dfx2.mp_max_players == 99);
    TEST_EXPECT(apply_host_dialog_control(dfx2, kDfx2, "MAX_PLAYERS", "250"));
    TEST_EXPECT(dfx2.mp_max_players == 100 && host_dialog_value(dfx2, "MAX_PLAYERS") == "100");
    dfx2.mp_max_players = 250;
    populate_host_dialog(dfx2, kDfx2);
    TEST_EXPECT(dfx2.mp_max_players == 100);
    TEST_EXPECT(session_player_cap(0, kDfx2) == 1 && session_player_cap(100, kDfx2) == 100);
    TEST_EXPECT(session_player_cap(251, kDfx2) == 251 && session_player_cap(300, kDfx2) == 251);
    TEST_EXPECT(host_player_slot_limit(100, false, kDfx2) == 101);
    TEST_EXPECT(host_player_slot_limit(251, false, kDfx2) == 252);
    TEST_EXPECT(host_player_slot_limit(252, false, kDfx2) == 251);
    GameConfig config;
    config.set_title(kDfx2);
    config.max_players = 251;
    config.dedicated_server = true;
    TEST_EXPECT(config.player_slot_limit() == 252);
    return 0;
}

namespace mnu = opennova::mnu;

mnu::Window control(const char* name, mnu::WindowType type,
                    std::vector<std::pair<const char*, const char*>> rows = {}) {
    mnu::Window window;
    window.name = name;
    window.type = type;
    window.items.present = !rows.empty();
    for (const auto& [text, value] : rows) {
        mnu::Item row;
        row.text = text;
        row.value = value;
        window.items.items.push_back(row);
    }
    return window;
}

// A MULTI_PLAYER_HOST as the shipped multiplayer menu authors it: the edits, the spins with their
// value= rows (an <ITEM> with none holds 0), GAME_LOCATION's two-letter rows behind an empty one,
// no SERVER_LANONLY.
mnu::Document host_dialog_document() {
    mnu::Window main = control("MAIN", mnu::WindowType::Window);
    main.children = {
        control("GAME_NAME", mnu::WindowType::Edit),
        control("SERVERTYPE", mnu::WindowType::SpinList, {{"HOST AND PLAY", ""}, {"HOST ONLY", "1"}}),
        control("GAME_LOCATION", mnu::WindowType::SpinList, {{"", "33"}, {"DE", "8"}, {"US", "32"}}),
        control("SERVER_PUNKBUSTER", mnu::WindowType::SpinList, {{"NO", ""}, {"YES", "1"}}),
        control("REPLAY", mnu::WindowType::SpinList, {{"NEXT", ""}, {"REPLAY", "1"}}),
        control("TIME", mnu::WindowType::Edit),
        control("MAX_PLAYERS", mnu::WindowType::Edit),
        control("ALLOW_AI", mnu::WindowType::SpinList, {{"NO", ""}, {"YES", "1"}}),
    };
    mnu::Screen screen;
    screen.name = "MULTI_PLAYER_HOST";
    screen.roots.push_back(main);
    mnu::Document doc;
    doc.screens.push_back(screen);
    return doc;
}

// The dialog left untouched gives the block back as its screen shows it: a word the screen can
// show stays, a spin's word no row holds comes back as row 0's value, GAME_LOCATION comes back as
// the matched row's own name (its case, its length) or, matching none, as the row the screen
// stands on, PunkBuster as 0 with no pb\pbcl.dll, the cap clamped; a control the screen lacks
// keeps its word, and with no screen only the clamp writes. The edits' text set cuts a {hot}.
// [orig: UI_InitHostSettingsDialog @0x558960 - UI_PopulateHostSettingsFromConfig @0x5589C7;
//  UI_PopulateHostSettingsFromConfig - the location match @0x5560D7..0x556114, PunkBuster
//  @0x55613A..0x556160, the clamp @0x556322..0x55632A; SpinList_SelectItemByValue @0x64BA50 (row
//  0 on a miss @0x64BA82); CButtonWnd_SetLabel @0x6572F0 (the {hot} cut @0x657451..0x657480);
//  UI_HandleHostSessionStart - HostDialog_ReadSettings @0x556E36; HostDialog_ReadSettings -
//  CSpinListWnd_GetSelectedValue @0x5559DE, the location's strncpy @0x555A72]
// The screen opened over `doc` (null: no screen, every control lacking) and START pressed with
// nothing changed: the populate, then the read.
void open_and_start(gamecfg::GameCfg& cfg, const mnu::Document* doc) {
    auto screen = std::make_unique<opennova::menu::MenuRuntime>();
    if (doc != nullptr) screen->open_document(doc, std::string(), std::string(kHostDialogScreen));
    populate_host_screen(*screen, cfg, kJo);
    read_host_screen(*screen, cfg, kJo);
}

int untouched_dialog() {
    const mnu::Document doc = host_dialog_document();
    gamecfg::GameCfg cfg;
    cfg.game_name = "Night{hot}Ops";
    cfg.country = "us";
    cfg.sv_punkbuster = 1;
    cfg.replay = 2;
    cfg.time_limit = 25;
    cfg.mp_max_players = 80;
    cfg.enable_ai = 2;
    cfg.dedicated = 1;
    cfg.mp_novaworld_host_lan_only = 5;
    open_and_start(cfg, &doc);
    TEST_EXPECT(cfg.game_name == "NightOps");
    TEST_EXPECT(cfg.country == "US");
    TEST_EXPECT(cfg.sv_punkbuster == 0 && cfg.replay == 0 && cfg.enable_ai == 0);
    TEST_EXPECT(cfg.time_limit == 25 && cfg.mp_max_players == 64 && cfg.dedicated == 1);
    TEST_EXPECT(cfg.mp_novaworld_host_lan_only == 5);
    // Three letters match no two-letter row: the screen stands on row 0, whose empty name
    // copies its NUL alone.
    cfg.country = "USA";
    cfg.replay = 1;
    open_and_start(cfg, &doc);
    TEST_EXPECT(cfg.country.empty() && cfg.replay == 1);
    cfg.country = "de";
    open_and_start(cfg, &doc);
    TEST_EXPECT(cfg.country == "DE");
    // No screen: the clamp alone.
    cfg.sv_punkbuster = 1;
    cfg.replay = 2;
    cfg.mp_max_players = 80;
    open_and_start(cfg, nullptr);
    TEST_EXPECT(cfg.sv_punkbuster == 1 && cfg.replay == 2 && cfg.mp_max_players == 64);
    // The LAN screen: the open, the player's edit, START's read; PunkBuster's spin is disabled.
    auto screen = std::make_unique<opennova::menu::MenuRuntime>();
    TEST_EXPECT(screen->open_document(&doc, std::string(), std::string(kHostDialogScreen)));
    populate_host_screen(*screen, cfg, kJo);
    const int name = screen->find_screen_control(std::string(kHostDialogScreen), "GAME_NAME");
    const int punkbuster = screen->find_screen_control(std::string(kHostDialogScreen), "SERVER_PUNKBUSTER");
    TEST_EXPECT(name >= 0 && screen->get_widget_text(name) == "NightOps");
    TEST_EXPECT(punkbuster >= 0 && screen->is_widget_disabled(punkbuster));
    screen->set_widget_text(name, "Renamed");
    read_host_screen(*screen, cfg, kJo);
    TEST_EXPECT(cfg.game_name == "Renamed" && cfg.sv_punkbuster == 0 && cfg.replay == 0);
    return 0;
}
// The connection speed's recommended player count: rows 0..17 the table under the title's
// screen cap, row 18 the cap, any other row 0; the populate shows it in
// STATIC_MAXPLAYERS_RECOMMENDATION through menutxt.bin's MAX_PLAYERS format (the key itself on a
// miss), and nothing reads it back (D-NET-1169).
// [orig: UI_MapSliderIndexToMaxPlayers @0x555F00 (the switch @0x555F0B, the cap @0x555F1F,
//  row 18 @0x555F86); UI_PopulateHostSettingsFromConfig @0x555FE0 - @0x55619C..0x5561F8;
//  UIStringTable_LookupAndDup @0x63B290 (the key's copy on a miss @0x63B2F0)]
// [orig: UI_MapSliderIndexToMaxPlayers @0x5540B0 (dfx2.exe), the same table under its cap]
int max_players_recommendation() {
    const int32_t rows[18] = {1, 2, 2, 2, 3, 3, 4, 5, 6, 8, 9, 11, 14, 16, 21, 23, 26, 32};
    for (int32_t i = 0; i < 18; ++i) TEST_EXPECT(map_slider_index_to_max_players(i, 64) == rows[i]);
    TEST_EXPECT(map_slider_index_to_max_players(18, 64) == 64);
    TEST_EXPECT(map_slider_index_to_max_players(18, 100) == 100);
    TEST_EXPECT(map_slider_index_to_max_players(17, 16) == 16 && map_slider_index_to_max_players(13, 16) == 16);
    TEST_EXPECT(map_slider_index_to_max_players(19, 64) == 0 && map_slider_index_to_max_players(-1, 64) == 0);

    mnu::Document doc = host_dialog_document();
    doc.screens[0].roots[0].children.push_back(control("CONNECTIONSPEED", mnu::WindowType::SpinList,
            {{"28.8", ""}, {"LAN", "18"}}));
    doc.screens[0].roots[0].children.push_back(
            control("STATIC_MAXPLAYERS_RECOMMENDATION", mnu::WindowType::Static));
    opennova::menu::MenuTextTables tables;
    auto menutxt = std::make_shared<opennova::rtxt::File>();
    menutxt->sections.push_back({"Menu", 1});
    menutxt->entries.push_back({"MAX_PLAYERS", "Up to %d players", {}, 0});
    tables.set_table("menutxt.bin", menutxt);
    opennova::menu::MenuStateFrame frame;
    frame.set_configure([&](const std::string& name, opennova::menu::MenuFrameCompiler& compiler) {
        const mnu::Screen* found = name.empty() ? doc.first_screen() : doc.find_screen(name);
        compiler.set_text_tables(&tables);
        compiler.configure(found);
        return found != nullptr;
    });
    auto screen = std::make_unique<opennova::menu::MenuRuntime>();
    screen->set_frame(&frame);
    TEST_EXPECT(screen->open_document(&doc, std::string(), std::string(kHostDialogScreen)));
    const int label = screen->find_screen_control(std::string(kHostDialogScreen),
                                                  "STATIC_MAXPLAYERS_RECOMMENDATION");
    TEST_EXPECT(label >= 0);
    gamecfg::GameCfg cfg;
    cfg.nwisptype = 13;
    populate_host_screen(*screen, cfg, kJo);
    TEST_EXPECT(screen->get_widget_text(label) == "Up to 16 players");
    cfg.nwisptype = 18;
    populate_host_screen(*screen, cfg, kJo);
    TEST_EXPECT(screen->get_widget_text(label) == "Up to 64 players");
    gamecfg::GameCfg dfx2;
    dfx2.table = gamecfg::ConfigTable::Dfx2;
    dfx2.nwisptype = 18;
    populate_host_screen(*screen, dfx2, kDfx2);
    TEST_EXPECT(screen->get_widget_text(label) == "Up to 100 players");
    // A frame with no menutxt.bin: the key itself, sprintf'd as the format.
    auto bare = std::make_unique<opennova::menu::MenuRuntime>();
    TEST_EXPECT(bare->open_document(&doc, std::string(), std::string(kHostDialogScreen)));
    cfg.nwisptype = 13;
    populate_host_screen(*bare, cfg, kJo);
    const int bare_label = bare->find_screen_control(std::string(kHostDialogScreen),
                                                     "STATIC_MAXPLAYERS_RECOMMENDATION");
    TEST_EXPECT(bare_label >= 0 && bare->get_widget_text(bare_label) == "MAX_PLAYERS");
    return 0;
}
// The restriction lists over the block: WEAPON_LIST a row per loadout_selectable weapon (its WepDes
// name, "" with no textid; its value the table row), CLASS_LIST the five classes; column 1 each
// word's Game.bin token, "??KEY??" on a miss. A column-1 press steps the word (weapons past 3 to 0,
// classes past 2 to 0), a radio sets every word; START stays off while every class word is 0.
// [orig: HostDialog_PopulateWeaponRestrictionList @0x556600; HostDialog_PopulateClassListCombo
//  @0x556700; HostDialog_WeaponRestrictionCycleHandler @0x5576c0;
//  HostDialog_OnClassAvailabilityListClick @0x5577d0; HostDialog_SetAllWeaponAvailability
//  @0x557910; HostDialog_SetAllWeaponRestrictions @0x557a10; UI_RegisterHostSettingsCallbacks
//  @0x5580f0; HostDialog_AddRemoveSelectedMissions @0x557c10 - the class words @0x557F24..0x557F43]
int restriction_lists() {
    const auto table = [](const char* name, int columns) {
        mnu::Window w = control(name, mnu::WindowType::Table);
        w.table_data.column.has_count = true;
        w.table_data.column.count = columns;
        return w;
    };
    mnu::Document doc = host_dialog_document();
    auto& children = doc.screens[0].roots[0].children;
    children.push_back(table("WEAPON_LIST", 2));
    children.push_back(table("CLASS_LIST", 2));
    children.push_back(table("SELECTED_MISSIONS", 3));
    children.push_back(control("START_GAME", mnu::WindowType::Button));
    for (const char* radio : {"WPN_NEVER", "WPN_MISSION", "CLASS_NEVER", "CLASS_ALWAYS"})
        children.push_back(control(radio, mnu::WindowType::Radio));
    opennova::menu::MenuRuntime screen;
    TEST_EXPECT(screen.open_document(&doc, std::string(), std::string(kHostDialogScreen)));
    const auto find = [&](const char* name) {
        return screen.find_screen_control(std::string(kHostDialogScreen), name);
    };
    const opennova::hud::GameTextLookup gametext = [](const char* section, const char* key,
                                                       const char* fallback) {
        return std::string(section) == "WepDes" && std::string(key) == "WD_M16" ? std::string("M16A2")
                                                                               : std::string(fallback);
    };
    const opennova::hud::GameTextLookup menu_ui = [](const char* section, const char* key,
                                                      const char* fallback) {
        const std::string k = key;
        if (std::string(section) != "Menu") return std::string(fallback);
        if (k == "NEVER") return std::string("Never");
        if (k == "ALWAYS") return std::string("Always");
        if (k == "MISSION") return std::string("Mission");
        if (k == "CHARCLASS_MEDIC") return std::string("Medic");
        return std::string(fallback);
    };
    const gamecfg::WeaponRoster weapons = {
        {"None", 0, ""}, {"WPN_M16", 1, "WD_M16"}, {"WPN_KNIFE", 0, "WD_KNIFE"}, {"WPN_X", 1, ""}};
    gamecfg::GameCfg cfg = gamecfg::defaults();
    cfg.weapon_availability[3] = 2;
    cfg.class_availability[gamecfg::kClassSniper] = 2;
    cfg.class_availability[gamecfg::kClassGunner] = 0;
    populate_host_weapon_list(screen, cfg, kJo, weapons, gametext, menu_ui);
    populate_host_class_list(screen, cfg, menu_ui);
    const int list = find("WEAPON_LIST");
    TEST_EXPECT(screen.table_row_count(list) == 2);
    TEST_EXPECT(screen.table_cell_text(list, 0, 0) == "M16A2" && screen.table_row_value(list, 0) == 1);
    TEST_EXPECT(screen.table_cell_text(list, 0, 1) == "Mission"); // the default 3
    TEST_EXPECT(screen.table_cell_text(list, 1, 0).empty() && screen.table_row_value(list, 1) == 3);
    TEST_EXPECT(screen.table_cell_text(list, 1, 1) == "??ARMORY??"); // the token missing
    const int classes = find("CLASS_LIST");
    TEST_EXPECT(screen.table_row_count(classes) == 5 && screen.table_row_value(classes, 0) == 5);
    TEST_EXPECT(screen.table_cell_text(classes, 0, 0) == "Medic" &&
                screen.table_cell_text(classes, 1, 0) == "??CHARCLASS_SNIPER??");
    TEST_EXPECT(screen.table_cell_text(classes, 0, 1) == "Always" &&
                screen.table_cell_text(classes, 1, 1) == "Mission" &&
                screen.table_cell_text(classes, 2, 1) == "Never");

    opennova::menu::HostDialog dialog;
    // A press steps the word: column 1 only, past 3 to 0.
    TEST_EXPECT(!host_restriction_list_pressed(screen, dialog, cfg, kJo, "WEAPON_LIST", 0, 0, menu_ui));
    TEST_EXPECT(host_restriction_list_pressed(screen, dialog, cfg, kJo, "WEAPON_LIST", 0, 1, menu_ui));
    TEST_EXPECT(cfg.weapon_availability[1] == 0 && screen.table_cell_text(list, 0, 1) == "Never");
    TEST_EXPECT(host_restriction_list_pressed(screen, dialog, cfg, kJo, "WEAPON_LIST", 0, 1, menu_ui));
    TEST_EXPECT(cfg.weapon_availability[1] == 1 && screen.table_cell_text(list, 0, 1) == "Always");
    // A class steps past 2 to 0.
    TEST_EXPECT(host_restriction_list_pressed(screen, dialog, cfg, kJo, "CLASS_LIST", 1, 1, menu_ui));
    TEST_EXPECT(cfg.class_availability[gamecfg::kClassSniper] == 0);
    TEST_EXPECT(!host_restriction_list_pressed(screen, dialog, cfg, kJo, "SELECTED_MISSIONS", 0, 1, menu_ui));
    // The radios: every loadout_selectable weapon, every class word.
    TEST_EXPECT(host_restriction_radio_pressed(screen, dialog, cfg, kJo, weapons, "WPN_MISSION", menu_ui));
    TEST_EXPECT(cfg.weapon_availability[1] == 3 && cfg.weapon_availability[3] == 3 &&
                cfg.weapon_availability[2] == 3 && screen.table_cell_text(list, 1, 1) == "Mission");
    TEST_EXPECT(host_restriction_radio_pressed(screen, dialog, cfg, kJo, weapons, "WPN_NEVER", menu_ui));
    TEST_EXPECT(cfg.weapon_availability[1] == 0 && cfg.weapon_availability[2] == 3);
    // Every class word 0: START off whatever the missions; one set: START follows them.
    const int start = find("START_GAME");
    TEST_EXPECT(host_restriction_radio_pressed(screen, dialog, cfg, kJo, weapons, "CLASS_NEVER", menu_ui));
    TEST_EXPECT(cfg.class_availability[0] == 0 && cfg.class_availability[9] == 0 && !dialog.can_start());
    TEST_EXPECT(screen.is_widget_disabled(start) && screen.table_cell_text(classes, 4, 1) == "Never");
    TEST_EXPECT(host_restriction_radio_pressed(screen, dialog, cfg, kJo, weapons, "CLASS_ALWAYS", menu_ui));
    TEST_EXPECT(cfg.class_availability[0] == 1 && screen.is_widget_disabled(start)); // no mission yet
    TEST_EXPECT(!host_restriction_radio_pressed(screen, dialog, cfg, kJo, weapons, "START_GAME", menu_ui));
    return 0;
}
} // namespace

int main() {
    if (title_caps() != 0) return 1;
    if (untouched_dialog() != 0) return 1;
    if (max_players_recommendation() != 0) return 1;
    if (restriction_lists() != 0) return 1;
    gamecfg::GameCfg cfg;
    const auto read = [&](const char* name, const char* value) {
        return apply_host_dialog_control(cfg, kJo, name, value);
    };
    // The numeric edits read with strtol prefixes into their block words as typed: the
    // session's limit sentinels are the apply's, not the screen's.
    TEST_EXPECT(read("TIME", "  +45 minutes"));
    TEST_EXPECT(read("RESPAWN", "8"));
    TEST_EXPECT(read("MAX_KOTH", "-1"));
    TEST_EXPECT(read("KILL_LIMIT", "500"));
    TEST_EXPECT(read("MAX_SCORE", "65000"));
    TEST_EXPECT(cfg.time_limit == 45 && cfg.timeout == 8 && cfg.koth_limit == -1);
    TEST_EXPECT(cfg.max_kills == 500 && cfg.max_score == 65000);
    TEST_EXPECT(session_point_limit(cfg.max_kills) == kSessionNoPointLimit &&
                session_koth_limit_minutes(cfg.koth_limit) == kSessionNoKothLimitMinutes);
    TEST_EXPECT(read("TIME", ""));
    TEST_EXPECT(cfg.time_limit == 0);
    // Toggling a control touches exactly its own mpattrib bit, including inverted ones.
    cfg.mpattrib = static_cast<int32_t>(0xA0000000u);
    for (const char* name : {"TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TRACERS"})
        TEST_EXPECT(read(name, "0"));
    TEST_EXPECT(read("TEAM_CHOOSE", "1"));
    TEST_EXPECT(read("CLAYMORE_PREF", "1"));
    TEST_EXPECT(static_cast<uint32_t>(cfg.mpattrib) == 0xA000860Du);
    for (const char* name : {"TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TRACERS"})
        TEST_EXPECT(read(name, "1"));
    TEST_EXPECT(read("TEAM_CHOOSE", "0"));
    TEST_EXPECT(read("CLAYMORE_PREF", "0"));
    TEST_EXPECT(static_cast<uint32_t>(cfg.mpattrib) == 0xA0000000u);
    TEST_EXPECT(read("ALLOW_SPECTATORS", "1"));
    TEST_EXPECT(cfg.mp_num_spectators_max == -1);
    cfg.mp_num_spectators_max = 7;
    TEST_EXPECT(read("ALLOW_SPECTATORS", "1"));
    TEST_EXPECT(cfg.mp_num_spectators_max == 7);
    TEST_EXPECT(read("ALLOW_SPECTATORS", "0"));
    TEST_EXPECT(cfg.mp_num_spectators_max == 0);
    TEST_EXPECT(read("SERVER_PASSWORD", "abcdefghijklmnopqr"));
    TEST_EXPECT(cfg.mp_host_game_password == "abcdefghijklmnopq");
    TEST_EXPECT(read("SERVER_MESSAGE", "Join us"));
    TEST_EXPECT(cfg.servermsg == "Join us");
    TEST_EXPECT(read("MAX_PLAYERS", "99"));
    TEST_EXPECT(cfg.mp_max_players == 64);
    TEST_EXPECT(read("SERVERTYPE", "1"));
    TEST_EXPECT(cfg.dedicated == 1 && host_player_slot_limit(cfg.mp_max_players, false, kJo) == 65);
    TEST_EXPECT(read("MAX_PLAYERS", ""));
    // No lower clamp: a blank cap publishes 0, or 1 as the dedicated slot alone;
    // the 65 ceiling tests the pre-increment cap, so a dedicated 65 publishes 66.
    TEST_EXPECT(cfg.mp_max_players == 0 && host_player_slot_limit(cfg.mp_max_players, false, kJo) == 1);
    TEST_EXPECT(host_player_slot_limit(0, true, kJo) == 0 && host_player_slot_limit(-3, true, kJo) == 0);
    TEST_EXPECT(host_player_slot_limit(65, false, kJo) == 66 && host_player_slot_limit(65, true, kJo) == 65);
    TEST_EXPECT(host_player_slot_limit(66, false, kJo) == 65 && host_player_slot_limit(99, true, kJo) == 65);
    // In session the create clamps the cap into 1..65 first, so a blank cap
    // publishes 1 (2 dedicated) and a dedicated 65+ the ceiling's 66.
    // [orig: CNapiGameSession_BuildAndCreateSession @0x56955D..0x56956C]
    TEST_EXPECT(session_player_cap(0, kJo) == 1 && session_player_cap(-3, kJo) == 1);
    TEST_EXPECT(session_player_cap(1, kJo) == 1 && session_player_cap(65, kJo) == 65);
    TEST_EXPECT(session_player_cap(66, kJo) == 65 && session_player_cap(99, kJo) == 65);
    TEST_EXPECT(host_player_slot_limit(session_player_cap(0, kJo), true, kJo) == 1 &&
                host_player_slot_limit(session_player_cap(0, kJo), false, kJo) == 2);
    TEST_EXPECT(host_player_slot_limit(session_player_cap(99, kJo), false, kJo) == 66);
    TEST_EXPECT(read("TAKEOVER_TIME", "20"));
    TEST_EXPECT(read("LFP_TAKEOVER", "3"));
    TEST_EXPECT(cfg.teamchange_time == 20 && cfg.mp_lfp_takeoverspeed == 3);
    TEST_EXPECT(read("ALLOW_AI", "5") && cfg.enable_ai == 1);
    TEST_EXPECT(!read("UNKNOWN", "1"));
    // The populate: what the host screen shows from the block, the limits as they stand.
    // [orig: UI_PopulateHostSettingsFromConfig @0x555FE0]
    const auto shown = [&](std::string_view name) { return host_dialog_value(cfg, name); };
    TEST_EXPECT(shown("SERVER_PASSWORD") == "abcdefghijklmnopq" && shown("SERVER_MESSAGE") == "Join us");
    TEST_EXPECT(shown("RESPAWN") == "8" && shown("TIME") == "0" && shown("MAX_KOTH") == "-1");
    TEST_EXPECT(shown("KILL_LIMIT") == "500" && shown("MAX_SCORE") == "65000");
    TEST_EXPECT(shown("SERVERTYPE") == "1" && shown("MAX_PLAYERS") == "0");
    TEST_EXPECT(shown("TAKEOVER_TIME") == "20" && shown("LFP_TAKEOVER") == "3");
    TEST_EXPECT(shown("ALLOW_SPECTATORS") == "0" && shown("ALLOW_AI") == "1");
    // The inverted flag spins show a CLEAR bit as 1; the direct ones the bit itself.
    for (const char* name : {"TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TRACERS"})
        TEST_EXPECT(shown(name) == "1");
    TEST_EXPECT(shown("TEAM_CHOOSE") == "0" && shown("CLAYMORE_PREF") == "0");
    // The populate clamps the block's cap to the screen's 64 before it shows it
    // [orig: @0x556328..0x55632A].
    cfg.mp_max_players = 99;
    TEST_EXPECT(shown("MAX_PLAYERS") == "99");
    populate_host_dialog(cfg, kJo);
    TEST_EXPECT(cfg.mp_max_players == 64 && shown("MAX_PLAYERS") == "64");
    TEST_EXPECT(read("SERVERTYPE", "0") && shown("SERVERTYPE") == "0");
    TEST_EXPECT(read("GAME_LOCATION", "USA") && shown("GAME_LOCATION") == "USA");
    // The count is the item's length clamped into 1..3: a shorter item copies its own bytes
    // alone, no NUL: "DE" over "USA" leaves "DEA"; an empty item copies its NUL alone (the
    // count's floor of 1), so the word reads "" [orig: HostDialog_ReadSettings
    // @0x555A1B..0x555A72, `mov ecx, 1` @0x555A35].
    TEST_EXPECT(read("GAME_LOCATION", "DE") && cfg.country == "DEA");
    TEST_EXPECT(read("GAME_LOCATION", "USA") && cfg.country == "USA");
    TEST_EXPECT(read("GAME_LOCATION", "") && cfg.country.empty() && shown("GAME_LOCATION").empty());
    TEST_EXPECT(read("GAME_LOCATION", "USA") && cfg.country == "USA");
    TEST_EXPECT(read("GAME_NAME", "Night Ops") && shown("GAME_NAME") == "Night Ops");
    // GAME_NAME's GetText(serverName_3A5, 0x20) copies a label of 32 or more bytes 32 deep, no
    // NUL [orig: HostDialog_ReadSettings @0x55595B..0x555967; CStaticWnd_GetLabelText
    // @0x657570, the strncpy(dest, label, 32) @0x6575BF]; 31 bytes copy whole with theirs.
    TEST_EXPECT(read("GAME_NAME", std::string(40, 'n').c_str()) && cfg.game_name == std::string(32, 'n'));
    TEST_EXPECT(read("GAME_NAME", std::string(31, 'n').c_str()) && cfg.game_name == std::string(31, 'n'));
    TEST_EXPECT(read("GAME_NAME", "Night Ops"));
    TEST_EXPECT(read("CONNECTIONSPEED", "5") && read("MAX_FF_KILLS", "3"));
    TEST_EXPECT(shown("CONNECTIONSPEED") == "5" && shown("MAX_FF_KILLS") == "3");
    TEST_EXPECT(cfg.nwisptype == 5 && cfg.numallowablefriendlykills == 3);
    TEST_EXPECT(shown("UNKNOWN").empty());
    // Every dialog control round-trips through its own populate value.
    for (std::string_view name : host_dialog_controls()) {
        gamecfg::GameCfg copy = cfg;
        TEST_EXPECT(apply_host_dialog_control(copy, kJo, name, shown(name)));
        TEST_EXPECT(host_dialog_value(copy, name) == shown(name));
    }
    return 0;
}
