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
#include <runtime/menu/menu_runtime.h>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace opennova::inmatch;
namespace gamecfg = opennova::gamecfg;

namespace {
constexpr int kJo = opennova::gameprofile::GAME_JO;
constexpr int kDfx2 = opennova::gameprofile::GAME_DFX2;

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
    TEST_EXPECT(apply_host_dialog_control(dfx2, "MAX_PLAYERS", "99"));
    TEST_EXPECT(dfx2.mp_max_players == 99);
    TEST_EXPECT(apply_host_dialog_control(dfx2, "MAX_PLAYERS", "250"));
    TEST_EXPECT(dfx2.mp_max_players == 100 && host_dialog_value(dfx2, "MAX_PLAYERS") == "100");
    dfx2.mp_max_players = 250;
    populate_host_dialog(dfx2);
    TEST_EXPECT(dfx2.mp_max_players == 100);
    TEST_EXPECT(session_player_cap(0, kDfx2) == 1 && session_player_cap(100, kDfx2) == 100);
    TEST_EXPECT(session_player_cap(251, kDfx2) == 251 && session_player_cap(300, kDfx2) == 251);
    TEST_EXPECT(host_player_slot_limit(100, false, kDfx2) == 101);
    TEST_EXPECT(host_player_slot_limit(251, false, kDfx2) == 252);
    TEST_EXPECT(host_player_slot_limit(252, false, kDfx2) == 251);
    GameConfig config;
    config.title = kDfx2;
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
    read_untouched_host_dialog(cfg, &doc);
    TEST_EXPECT(cfg.game_name == "NightOps");
    TEST_EXPECT(cfg.country == "US");
    TEST_EXPECT(cfg.sv_punkbuster == 0 && cfg.replay == 0 && cfg.enable_ai == 0);
    TEST_EXPECT(cfg.time_limit == 25 && cfg.mp_max_players == 64 && cfg.dedicated == 1);
    TEST_EXPECT(cfg.mp_novaworld_host_lan_only == 5);
    // Three letters match no two-letter row: the screen stands on row 0, whose empty name
    // copies its NUL alone.
    cfg.country = "USA";
    cfg.replay = 1;
    read_untouched_host_dialog(cfg, &doc);
    TEST_EXPECT(cfg.country.empty() && cfg.replay == 1);
    cfg.country = "de";
    read_untouched_host_dialog(cfg, &doc);
    TEST_EXPECT(cfg.country == "DE");
    // No screen: the clamp alone.
    cfg.sv_punkbuster = 1;
    cfg.replay = 2;
    cfg.mp_max_players = 80;
    read_untouched_host_dialog(cfg, nullptr);
    TEST_EXPECT(cfg.sv_punkbuster == 1 && cfg.replay == 2 && cfg.mp_max_players == 64);
    // The LAN screen: the open, the player's edit, START's read; PunkBuster's spin is disabled.
    auto screen = std::make_unique<opennova::menu::MenuRuntime>();
    TEST_EXPECT(screen->open_document(&doc, std::string(), std::string(kHostDialogScreen)));
    populate_host_screen(*screen, cfg);
    const int name = screen->find_screen_control(std::string(kHostDialogScreen), "GAME_NAME");
    const int punkbuster = screen->find_screen_control(std::string(kHostDialogScreen), "SERVER_PUNKBUSTER");
    TEST_EXPECT(name >= 0 && screen->get_widget_text(name) == "NightOps");
    TEST_EXPECT(punkbuster >= 0 && screen->is_widget_disabled(punkbuster));
    screen->set_widget_text(name, "Renamed");
    read_host_screen(*screen, cfg);
    TEST_EXPECT(cfg.game_name == "Renamed" && cfg.sv_punkbuster == 0 && cfg.replay == 0);
    return 0;
}
} // namespace

int main() {
    if (title_caps() != 0) return 1;
    if (untouched_dialog() != 0) return 1;
    gamecfg::GameCfg cfg;
    const auto read = [&](const char* name, const char* value) {
        return apply_host_dialog_control(cfg, name, value);
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
    populate_host_dialog(cfg);
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
        TEST_EXPECT(apply_host_dialog_control(copy, name, shown(name)));
        TEST_EXPECT(host_dialog_value(copy, name) == shown(name));
    }
    return 0;
}
