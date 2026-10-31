#include "host_settings.h"

#include <base/io/crt_ftol.h>
#include <formats/mnu/mnu.h>
#include <net/protocol/protocol.h> // the availability words (DFX2-10)
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/hud/hud_game_text.h> // hud_sprintf (the CRT sprintf over a localized format)

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace opennova::inmatch {
namespace {

int32_t edit_integer(const std::string& value) {
    // The CRT strtol at radix 10 on a signed 32-bit long even when the port's long is
    // wider, its white space the locale's (0xA0 included; io::retail_strtol, D-NET-384).
    // [orig: CEditWnd_GetIntValue @ 0x6575d0 — strtol(text, &end, 10) @0x6575e5]
    return io::retail_strtol(value.c_str(), 10);
}

struct FlagControl {
    std::string_view control;
    uint32_t bit;
    bool inverted;
};
constexpr FlagControl kFlagControls[] = {
    {"TEAM_FF", 0x200u, true}, {"FRIENDLY_TAG", 0x400u, true},
    {"FF_WARNING", 0x8u, true}, {"TEAM_CHOOSE", 0x4u, false},
    {"CLAYMORE_PREF", 0x8000u, false}, {"TRACERS", 0x1u, true},
};

} // namespace

const std::vector<std::string_view>& host_dialog_controls() {
    // [orig: HostDialog_ReadSettings @ 0x555940]
    static const std::vector<std::string_view> controls = {
        "GAME_NAME", "SERVER_PASSWORD", "SERVER_MESSAGE", "SERVERTYPE",
        "GAME_LOCATION", "SERVER_PUNKBUSTER", "SERVER_LANONLY", "CONNECTIONSPEED",
        "REPLAY", "DELAY", "RESPAWN", "TIME", "KILL_LIMIT", "MAX_SCORE", "MAX_KOTH",
        "MAX_PLAYERS", "MAX_FF_KILLS", "TAKEOVER_TIME", "LFP_TAKEOVER", "TEAM_FF",
        "FRIENDLY_TAG", "FF_WARNING", "TEAM_CHOOSE", "CLAYMORE_PREF", "TRACERS",
        "ALLOW_SPECTATORS", "ALLOW_AI", "BLUE_PW", "RED_PW", "SPECTATOR_PW",
        "TOD_CONTINUITY",
    };
    return controls;
}

bool apply_host_dialog_control(gamecfg::GameCfg& cfg, const Title& title, std::string_view control,
                               const std::string& value) {
    // The text edits by their capacities, literal byte counts (no invented -1)
    // [orig: HostDialog_ReadSettings @0x555940 - GAME_NAME @0x555967 (32),
    //  SERVER_PASSWORD @0x555990 (17), SERVER_MESSAGE @0x5559BC (128), BLUE_PW
    //  @0x555E7F, RED_PW @0x555EA8, SPECTATOR_PW @0x555ED1 (17 each);
    //  CStaticWnd_GetLabelText @0x657570]; the location's item, its length clamped into 1..3
    // [orig: strncpy(country_3C5, item, count) @0x555A72]. A label shorter than its cap copies
    // whole with its NUL; one of the cap or longer copies its first cap bytes with none
    // [orig: CStaticWnd_GetLabelText - the strlen + 1 against the cap @0x657597, strncpy
    // @0x6575BF], where retail's field reads on into the next one (GAME_NAME's 32 into
    // country_3C5, the unbounded-copy class of D-GAMECFG-1): the block holds the 32.
    if (control == "GAME_NAME") { cfg.game_name = value.substr(0, 32); return true; }
    if (control == "SERVER_PASSWORD") { cfg.mp_host_game_password = value.substr(0, 17); return true; }
    if (control == "SERVER_MESSAGE") { cfg.servermsg = value.substr(0, 128); return true; }
    if (control == "BLUE_PW") { cfg.mp_host_side_password_a = value.substr(0, 17); return true; }
    if (control == "RED_PW") { cfg.mp_host_side_password_b = value.substr(0, 17); return true; }
    if (control == "SPECTATOR_PW") { cfg.mp_host_spectator_password = value.substr(0, 17); return true; }
    if (control == "GAME_LOCATION") {
        // strncpy with the item's length clamped into 1..3 as the count: a 1..3 character item's
        // bytes over the word's with no NUL, so a shorter item keeps the old word's tail ("DE"
        // over "USA" reads "DEA"); an empty item copies its NUL alone, so the word reads "".
        // [orig: HostDialog_ReadSettings @0x555A1B..0x555A72 - the length against 3 @0x555A1B,
        //  against 1 @0x555A30, `mov ecx, 1` @0x555A35, `mov ecx, 3` @0x555A51, the length
        //  @0x555A58..0x555A69, strncpy(country_3C5, item, count) @0x555A72]
        const size_t count = std::clamp<size_t>(value.size(), 1, 3);
        const size_t copied = std::min(count, value.size());
        if (cfg.country.size() < copied) cfg.country.resize(copied);
        cfg.country.replace(0, copied, value, 0, copied);
        // strncpy pads with NULs up to the count: an item shorter than the count ends the word.
        if (copied < count) cfg.country.resize(copied);
        return true;
    }
    const int32_t number = edit_integer(value);
    // The six attribute spins onto their mpattrib bits, four of them inverted
    // [orig: @0x555CBC..0x555DE8].
    for (const FlagControl& flag : kFlagControls) {
        if (control != flag.control) continue;
        if ((number != 0) != flag.inverted) cfg.mpattrib = static_cast<int32_t>(static_cast<uint32_t>(cfg.mpattrib) | flag.bit);
        else cfg.mpattrib = static_cast<int32_t>(static_cast<uint32_t>(cfg.mpattrib) & ~flag.bit);
        return true;
    }
    // [orig: SERVERTYPE dedicatedServer_400 @0x5559DE, SERVER_PUNKBUSTER @0x555A9B,
    //  SERVER_LANONLY hostLanOnly_340 @0x555AC0, CONNECTIONSPEED nwiSpType_540
    //  @0x555AE5, REPLAY @0x555B0A, DELAY startDelay_410 @0x555B31, RESPAWN
    //  respawnTimeout_47C @0x555B58, TIME timeLimit_470 @0x555B7F, KILL_LIMIT
    //  maxKills_408 @0x555BA6, MAX_SCORE maxScore_40C @0x555BCD, MAX_KOTH
    //  kothLimit_474 @0x555BF4, MAX_PLAYERS maxPlayers_3F4 @0x555C1B with the
    //  screen cap @0x555C25..0x555C2D (dfx2.exe's 100 @0x553dd0), MAX_FF_KILLS
    //  @0x555C54, TAKEOVER_TIME teamChangeTime_4C0 @0x555C7B, LFP_TAKEOVER
    //  @0x555CA0, ALLOW_SPECTATORS @0x555E0A..0x555E23, ALLOW_AI enableAi_A14
    //  @0x555E53, TOD_CONTINUITY @0x555EF3]
    if (control == "SERVERTYPE") cfg.dedicated = number;
    else if (control == "SERVER_PUNKBUSTER") cfg.sv_punkbuster = number;
    else if (control == "SERVER_LANONLY") cfg.mp_novaworld_host_lan_only = number;
    else if (control == "CONNECTIONSPEED") cfg.nwisptype = number;
    else if (control == "REPLAY") cfg.replay = number;
    else if (control == "DELAY") cfg.startdelay = number;
    else if (control == "RESPAWN") cfg.timeout = number;
    else if (control == "TIME") cfg.time_limit = number;
    else if (control == "KILL_LIMIT") cfg.max_kills = number;
    else if (control == "MAX_SCORE") cfg.max_score = number;
    else if (control == "MAX_KOTH") cfg.koth_limit = number;
    else if (control == "MAX_PLAYERS")
        cfg.mp_max_players = std::min(number, title_player_caps(title).host_screen);
    else if (control == "MAX_FF_KILLS") cfg.numallowablefriendlykills = number;
    else if (control == "TAKEOVER_TIME") cfg.teamchange_time = number;
    else if (control == "LFP_TAKEOVER") cfg.mp_lfp_takeoverspeed = number;
    else if (control == "ALLOW_SPECTATORS") {
        if (number == 0) cfg.mp_num_spectators_max = 0;
        else if (cfg.mp_num_spectators_max == 0) cfg.mp_num_spectators_max = -1;
    } else if (control == "ALLOW_AI") cfg.enable_ai = number != 0 ? 1 : 0;
    else if (control == "TOD_CONTINUITY") cfg.mp_tod_continuity = number;
    else return false;
    return true;
}

void populate_host_dialog(gamecfg::GameCfg& cfg, const Title& title) {
    // [orig: UI_PopulateHostSettingsFromConfig @0x556328..0x55632A, g_HostScreenMaxPlayers]
    // [orig: @0x5544cd (dfx2.exe)]
    cfg.mp_max_players = std::min(cfg.mp_max_players, title_player_caps(title).host_screen);
}

std::string host_dialog_value(const gamecfg::GameCfg& cfg, std::string_view control) {
    // Text edits by SetText, numeric edits by SetIntValue, spins by
    // SelectItemByValue; the four inverted flag spins show the bit CLEAR as 1.
    // [orig: UI_PopulateHostSettingsFromConfig @0x555FE0 - GAME_NAME @0x556015,
    //  SERVERTYPE @0x55608B, SERVER_LANONLY @0x5560B2, GAME_LOCATION strnicmp(country,
    //  item, 3) @0x5560FC, SERVER_PUNKBUSTER @0x55614E, CONNECTIONSPEED @0x556187,
    //  REPLAY @0x556222, DELAY @0x55624E, TIME @0x5562A0, MAX_SCORE @0x5562F2,
    //  MAX_PLAYERS @0x556358, LFP_TAKEOVER @0x5563B2, TEAM_FF (flags & 0x200) == 0
    //  @0x5563E1, FRIENDLY_TAG @0x556439, FF_WARNING @0x556468, TRACERS @0x5564EE,
    //  ALLOW_SPECTATORS maxSpectators != 0 @0x55651A, ALLOW_AI @0x556541,
    //  TOD_CONTINUITY @0x5565E3]
    if (control == "GAME_NAME") return cfg.game_name;
    if (control == "SERVER_PASSWORD") return cfg.mp_host_game_password;
    if (control == "SERVER_MESSAGE") return cfg.servermsg;
    if (control == "BLUE_PW") return cfg.mp_host_side_password_a;
    if (control == "RED_PW") return cfg.mp_host_side_password_b;
    if (control == "SPECTATOR_PW") return cfg.mp_host_spectator_password;
    if (control == "GAME_LOCATION") return cfg.country;
    const auto number = [](int64_t value) { return std::to_string(value); };
    for (const FlagControl& flag : kFlagControls) {
        if (control != flag.control) continue;
        return number(((static_cast<uint32_t>(cfg.mpattrib) & flag.bit) != 0) != flag.inverted ? 1 : 0);
    }
    if (control == "SERVERTYPE") return number(cfg.dedicated);
    if (control == "SERVER_PUNKBUSTER") return number(cfg.sv_punkbuster);
    if (control == "SERVER_LANONLY") return number(cfg.mp_novaworld_host_lan_only);
    if (control == "CONNECTIONSPEED") return number(cfg.nwisptype);
    if (control == "REPLAY") return number(cfg.replay);
    if (control == "DELAY") return number(cfg.startdelay);
    if (control == "RESPAWN") return number(cfg.timeout);
    if (control == "TIME") return number(cfg.time_limit);
    if (control == "KILL_LIMIT") return number(cfg.max_kills);
    if (control == "MAX_SCORE") return number(cfg.max_score);
    if (control == "MAX_KOTH") return number(cfg.koth_limit);
    if (control == "MAX_PLAYERS") return number(cfg.mp_max_players);
    if (control == "MAX_FF_KILLS") return number(cfg.numallowablefriendlykills);
    if (control == "TAKEOVER_TIME") return number(cfg.teamchange_time);
    if (control == "LFP_TAKEOVER") return number(cfg.mp_lfp_takeoverspeed);
    if (control == "ALLOW_SPECTATORS") return number(cfg.mp_num_spectators_max != 0 ? 1 : 0);
    if (control == "ALLOW_AI") return number(cfg.enable_ai);
    if (control == "TOD_CONTINUITY") return number(cfg.mp_tod_continuity);
    return std::string();
}

namespace {

// A control on the title's own host screen (Title::host_screen), as both walks and the restriction
// lists find it: -1 when the screen lacks it (the walk skips it) or the title names no screen.
// [orig: UI_FindScreenControl @0x63AE80 - "MULTI_PLAYER_HOST" at each control of
//  UI_PopulateHostSettingsFromConfig @0x555FE0 (GAME_NAME @0x556010) and HostDialog_ReadSettings
//  @0x555940 (GAME_NAME @0x555962), and @0x556611 / @0x556712]
// [orig: "NW_MULTI_PLAYER" at each control of UI_PopulateHostSettingsFromConfig @0x554190
//  (GAME_NAME @0x5541ae .. TOD_CONTINUITY @0x554781) and HostDialog_ReadSettings @0x553af0
//  (GAME_NAME @0x553b00 .. TOD_CONTINUITY @0x554093), and @0x5547c1 / @0x5548c2 (dfx2.exe)]
int host_screen_control(const menu::MenuRuntime& screen, const Title& title, std::string_view control) {
    if (title.host_screen == nullptr) return -1;
    return screen.find_screen_control(std::string(title.host_screen), std::string(control));
}

mnu::WindowType host_control_kind(const menu::MenuRuntime& screen, int id) {
    return static_cast<mnu::WindowType>(screen.widget_kind_of(id));
}

} // namespace

void populate_host_screen(menu::MenuRuntime& screen, gamecfg::GameCfg& cfg, const Title& title) {
    // The clamp lands in the block ahead of MAX_PLAYERS' show, the one word it moves.
    populate_host_dialog(cfg, title);
    for (std::string_view control : host_dialog_controls()) {
        const int id = host_screen_control(screen, title, control);
        if (id < 0) continue;
        std::string value = host_dialog_value(cfg, control);
        if (control == "SERVER_PUNKBUSTER") {
            // No pb\pbcl.dll: the row of value 0, and the spin made inert.
            // [orig: UI_PopulateHostSettingsFromConfig @0x555FE0 (the File_CheckExists
            //  ("pb\\pbcl.dll") call @0x55613A, SpinList_SelectItemByValue(0) @0x556157,
            //  UIWidget_SetInteractiveRecursive(0) @0x556160)]
            value = "0";
            screen.set_widget_disabled(id, true);
        }
        switch (host_control_kind(screen, id)) {
            case mnu::WindowType::SpinList:
                // [orig: SpinList_SelectItemByValue @0x64BA50 (row 0 on a miss @0x64BA82);
                //  GAME_LOCATION: UI_PopulateHostSettingsFromConfig @0x555FE0 (the
                //  CSpinListWnd_GetItemCount call @0x5560D7, each row's name by the
                //  CSpinListWnd_GetItemNameByIndex call @0x5560E7 against the country by
                //  _strnicmp(country_3C5, name, 3) @0x5560FC, the first match selected by the
                //  CSpinListWnd_SetSelectedIndex call @0x556114, none: the row stays)]
                if (control == "GAME_LOCATION") menu::HostDialog::select_location(screen, id, value);
                else screen.select_row_by_value(id, value, false);
                break;
            case mnu::WindowType::CheckBox:
                screen.set_widget_checked(id, value != "0");
                break;
            default:
                // The edits' text set, SetIntValue's "%d" through it for the numeric ones
                // [orig: UI_PopulateHostSettingsFromConfig @0x555FE0 (GAME_NAME's vtable+0x4C
                //  call @0x556015); CEditWnd_SetText @0x661900 (CButtonWnd_SetLabel, the {hot}
                //  cut, @0x661912); CEditWnd_SetIntValue @0x657520 (sprintf "%d" @0x657540, the
                //  set @0x657558)].
                screen.set_widget_text(id, value);
                break;
        }
    }
    // The recommendation label past CONNECTIONSPEED: menutxt.bin's MAX_PLAYERS (the key on
    // a miss) as the sprintf format of the connection speed's player count, set as the
    // label's text. It reads only nwisptype, which no other control's show moves.
    // [orig: UI_PopulateHostSettingsFromConfig @0x555FE0 - UI_FindScreenControl
    //  ("STATIC_MAXPLAYERS_RECOMMENDATION") @0x55619C, UIStringTable_LookupAndDup("menutxt.bin",
    //  "MAX_PLAYERS") @0x5561BC, UI_MapSliderIndexToMaxPlayers(nwiSpType_540) @0x5561CF, sprintf
    //  @0x5561DB, the SetText vtable+0x4C call @0x5561F1, the free @0x5561F8]
    const int label = host_screen_control(screen, title, "STATIC_MAXPLAYERS_RECOMMENDATION");
    if (label >= 0) {
        const std::string format = screen.table_string("menutxt.bin", "MAX_PLAYERS");
        screen.set_widget_text(label, hud::hud_sprintf(format,
                map_slider_index_to_max_players(cfg.nwisptype,
                        title_player_caps(title).host_screen)));
    }
}

int32_t map_slider_index_to_max_players(int32_t slider_index, int32_t host_screen_cap) {
    // [orig: UI_MapSliderIndexToMaxPlayers @0x555F00 - the switch @0x555F0B, the cap test
    //  @0x555F1F, row 18 the cap @0x555F86]
    static constexpr int32_t kRowPlayers[18] = {1, 2, 2, 2, 3, 3, 4, 5, 6, 8, 9, 11, 14, 16,
                                                21, 23, 26, 32};
    if (slider_index == 18) return host_screen_cap;
    const int32_t players = slider_index >= 0 && slider_index < 18 ? kRowPlayers[slider_index] : 0;
    return players > host_screen_cap ? host_screen_cap : players;
}

void read_host_screen(const menu::MenuRuntime& screen, gamecfg::GameCfg& cfg, const Title& title) {
    for (std::string_view control : host_dialog_controls()) {
        const int id = host_screen_control(screen, title, control);
        if (id < 0) continue;
        std::string value = screen.get_widget_text(id);
        switch (host_control_kind(screen, id)) {
            case mnu::WindowType::SpinList: {
                // The selected row's value, 0 out of range (item_value's ""), GAME_LOCATION's
                // its name [orig: CSpinListWnd_GetSelectedValue @0x64BAF0 (item+4 @0x64BB11);
                // CSpinListWnd_GetSelectedName @0x64BB20; HostDialog_ReadSettings @0x555940
                // (the SERVERTYPE read @0x5559DE and each spin's after it, the GAME_LOCATION
                // name call @0x5559FF)]
                const int row = screen.selected_row(id);
                value = control == "GAME_LOCATION" ? screen.item_text(id, row) : screen.item_value(id, row);
                break;
            }
            case mnu::WindowType::CheckBox:
                value = screen.is_widget_checked(id) ? "1" : "0";
                break;
            default:
                break;
        }
        apply_host_dialog_control(cfg, title, control, value);
    }
}

namespace {

// A Game.bin "Menu" token, "??KEY??" when the table lacks it.
// [orig: TextResource_GetStringWithFallback @0x562EE0 (the "??%s??" miss)]
std::string menu_token(const hud::GameTextLookup& menu_ui, const std::string& key) {
    const std::string miss = "??" + key + "??";
    return hud::game_text(menu_ui, "Menu", key.c_str(), miss.c_str());
}

// A weapon word's cell: -1 and the mission word MISSION, 0 NEVER, the armory-only word ARMORY,
// any other ALWAYS (gamecfg::weapon_availability_name's names, as the tokens spell them).
// [orig: HostDialog_PopulateWeaponRestrictionList @0x556600 - the switch @0x556655]
std::string weapon_word_token(const hud::GameTextLookup& menu_ui, int32_t word,
                              const gamecfg::ServerSettingsTitle& words) {
    std::string key = gamecfg::weapon_availability_name(word, words);
    for (char& c : key) c = static_cast<char>(c - 'a' + 'A');
    return menu_token(menu_ui, key);
}

// A class word's cell: 0 NEVER, 2 MISSION, any other ALWAYS.
// [orig: HostDialog_PopulateClassListCombo @0x556700 - @0x5567C1..0x5567FE]
std::string class_word_token(const hud::GameTextLookup& menu_ui, int32_t word) {
    return menu_token(menu_ui, word == 0 ? "NEVER" : word == 2 ? "MISSION" : "ALWAYS");
}

constexpr int kRestrictionCell = 1; // the token's column [orig: CTableWnd_SetCellText(.., 1) @0x5566BF]
constexpr size_t kClassWordCount = gamecfg::kClassAvailabilityCount;

} // namespace

gamecfg::ServerSettingsTitle server_settings_title(const Title& title) {
    // The host screen's cap (DFX2-8) and the words of the protocol the title speaks (DFX2-10).
    const ProtocolInMatch& in_match = protocol_in_match(title_protocol(title.code));
    gamecfg::ServerSettingsTitle out;
    out.table = title.config_table;
    out.max_players = title_player_caps(title).host_screen;
    out.armory_word = in_match.s2c_66.armory_only_availability;
    out.mission_word = in_match.s2c_66.mission_availability();
    return out;
}

gamecfg::ServerSettingsTitle server_settings_title(std::string_view title_code) {
    const Title* title = title_by_code(title_code);
    return server_settings_title(title != nullptr ? *title : default_title());
}

void populate_host_weapon_list(menu::MenuRuntime& screen, const gamecfg::GameCfg& cfg, const Title& title,
                               const gamecfg::WeaponRoster& weapons, const hud::GameTextLookup& gametext,
                               const hud::GameTextLookup& menu_ui) {
    // [orig: HostDialog_PopulateWeaponRestrictionList @0x556600 - UI_FindScreenControl
    //  ("MULTI_PLAYER_HOST", "WEAPON_LIST") @0x556611, CTableWnd_RemoveRow(-1) @0x556625, the
    //  table walk @0x55663A..0x5566D3 (a row's loadout_selectable @0x556640, its WepDes name, the
    //  text WeaponDef_ParseProperty's loadout_menu_textid resolves, none without one),
    //  CTableWnd_AddRow(name, index, 0, -1) @0x5566B2, CTableWnd_SetCellText(row, token, 1)
    //  @0x5566BF]
    // [orig: HostDialog_PopulateWeaponRestrictionList @0x5547b0 - NW_MULTI_PLAYER's WEAPON_LIST
    //  @0x5547c1, the token's switch @0x554805 (dfx2.exe)]
    const int list = host_screen_control(screen, title, "WEAPON_LIST");
    if (list < 0) return;
    const gamecfg::ServerSettingsTitle words = server_settings_title(title);
    screen.table_clear_rows(list);
    for (size_t i = 0; i < weapons.size() && i < gamecfg::kWeaponAvailabilityCount; ++i) {
        if (weapons[i].loadout_selectable == 0) continue;
        const std::string& textid = weapons[i].loadout_menu_textid;
        const std::string name = textid.empty()
                ? std::string()
                : hud::game_text(gametext, hud::kGameTextWepDes, textid.c_str(), "");
        const int row = screen.table_insert_row(list, name, static_cast<int32_t>(i), 0, -1);
        screen.table_set_cell_text(list, row, kRestrictionCell,
                weapon_word_token(menu_ui, cfg.weapon_availability[i], words));
    }
}

void populate_host_class_list(menu::MenuRuntime& screen, const gamecfg::GameCfg& cfg, const Title& title,
                              const hud::GameTextLookup& menu_ui) {
    // [orig: HostDialog_PopulateClassListCombo @0x556700 - UI_FindScreenControl("MULTI_PLAYER_HOST",
    //  "CLASS_LIST") @0x556712, CTableWnd_RemoveRow(-1) @0x556728, classes 5..9 @0x556731, the
    //  CHARCLASS_* names @0x556741..0x556798, CTableWnd_AddRow(name, class, 0, -1) @0x5567B5, the
    //  token @0x5567C1..0x5567FE, CTableWnd_SetCellText(row, token, 1) @0x556814]
    // [orig: HostDialog_PopulateClassListCombo @0x5548b0 - NW_MULTI_PLAYER's CLASS_LIST @0x5548c2,
    //  classes 5..9 @0x5548e1 (dfx2.exe)]
    static constexpr const char* kClassKeys[] = {"CHARCLASS_MEDIC", "CHARCLASS_SNIPER",
        "CHARCLASS_GUNNER", "CHARCLASS_RIFLEMAN", "CHARCLASS_ENGINEER"};
    const int list = host_screen_control(screen, title, "CLASS_LIST");
    if (list < 0) return;
    screen.table_clear_rows(list);
    for (int cls = gamecfg::kClassMedic; cls <= gamecfg::kClassEngineer; ++cls) {
        const int row = screen.table_insert_row(list,
                menu_token(menu_ui, kClassKeys[cls - gamecfg::kClassMedic]), cls, 0, -1);
        screen.table_set_cell_text(list, row, kRestrictionCell,
                class_word_token(menu_ui, cfg.class_availability[cls]));
    }
}

bool host_restriction_list_pressed(menu::MenuRuntime& screen, menu::HostDialog& dialog,
                                   gamecfg::GameCfg& cfg, const Title& title, std::string_view widget, int row,
                                   int column, const hud::GameTextLookup& menu_ui) {
    if (column != kRestrictionCell) return false;
    const bool weapons = widget == "WEAPON_LIST";
    if (!weapons && widget != "CLASS_LIST") return false;
    const int list = host_screen_control(screen, title, widget);
    if (list < 0 || row < 0 || row >= screen.table_row_count(list)) return false;
    const int32_t index = screen.table_row_value(list, row);
    if (weapons) {
        // [orig: HostDialog_WeaponRestrictionCycleHandler @0x5576c0 - the cell event's press forms
        //  and column 1 @0x5576D3..0x5576EE, CTableWnd_GetRowValue @0x55771A, the step past 3 to 0
        //  @0x55772F..0x557731, the token's switch @0x55774B, CTableWnd_SetCellText @0x5577A6]
        // [orig: @0x5558a0 - NW_MULTI_PLAYER's WEAPON_LIST @0x5558db, CTableWnd_GetRowValue
        //  @0x5558fa, the step past 2 to 0 @0x55590f..0x555911, the token's switch @0x55592b
        //  (dfx2.exe)]
        // The step's last word is the title's mission word, past which it goes back to NEVER:
        // Jointops.exe's 3 (ARMORY 2 on the way), dfx2.exe's 2, so a dfx2.exe press never reaches its
        // ARMORY 3 (WPN_ARMORY alone sets it).
        if (index < 0 || index >= static_cast<int32_t>(gamecfg::kWeaponAvailabilityCount)) return false;
        const gamecfg::ServerSettingsTitle words = server_settings_title(title);
        int32_t& word = cfg.weapon_availability[index];
        if (++word > words.mission_word) word = 0;
        screen.table_set_cell_text(list, row, kRestrictionCell, weapon_word_token(menu_ui, word, words));
        return true;
    }
    // [orig: HostDialog_OnClassAvailabilityListClick @0x5577d0 - the press forms and column 1
    //  @0x5577E3, the event's own list @0x557805, CTableWnd_GetRowValue @0x557815, the step past 2
    //  to 0 @0x55782C..0x55782E, the token @0x557839..0x557880, CTableWnd_SetCellText @0x557897,
    //  the START gate @0x5578B1..0x557907]
    // [orig: HostDialog_OnWeaponRestrictionListClick @0x5559b0 (a misnomer: CLASS_LIST's, registered
    //  @0x556339) - CTableWnd_GetRowValue @0x5559f5, the step past 2 to 0 @0x555a0c,
    //  NW_MULTI_PLAYER's START_GAME gate @0x555a91 (dfx2.exe)]
    if (index < 0 || index >= static_cast<int32_t>(kClassWordCount)) return false;
    int32_t& word = cfg.class_availability[index];
    if (++word > 2) word = 0;
    screen.table_set_cell_text(list, row, kRestrictionCell, class_word_token(menu_ui, word));
    dialog.set_class_words(screen, cfg.class_availability, kClassWordCount);
    return true;
}

bool host_restriction_radio_pressed(menu::MenuRuntime& screen, menu::HostDialog& dialog,
                                    gamecfg::GameCfg& cfg, const Title& title,
                                    const gamecfg::WeaponRoster& weapons, std::string_view control,
                                    const hud::GameTextLookup& menu_ui) {
    // [orig: UI_RegisterHostSettingsCallbacks @0x5580f0 - CLASS_ALWAYS 1, CLASS_NEVER 0,
    //  CLASS_MISSION 2 to HostDialog_SetAllClassAvailability; WPN_ALWAYS 1, WPN_NEVER 0,
    //  WPN_ARMORY 2, WPN_MISSION 3 to HostDialog_SetAllWeaponAvailability @0x557910]
    // [orig: @0x5562c0 - CLASS_ALWAYS 1 @0x556357, CLASS_NEVER 0 @0x556375, CLASS_MISSION 2
    //  @0x556393 to HostDialog_SetAllWeaponRestrictions @0x555bf0; WPN_ALWAYS 1 @0x5563b1,
    //  WPN_NEVER 0 @0x5563cf, WPN_ARMORY 3 @0x5563ed, WPN_MISSION 2 @0x55640b to @0x555af0
    //  (dfx2.exe)]
    // WPN_ARMORY and WPN_MISSION set the title's armory-only and mission words.
    struct Radio {
        std::string_view control;
        bool weapons;
        int32_t word;
    };
    const gamecfg::ServerSettingsTitle words = server_settings_title(title);
    const Radio radios[] = {
        {"CLASS_ALWAYS", false, 1}, {"CLASS_NEVER", false, 0}, {"CLASS_MISSION", false, 2},
        {"WPN_ALWAYS", true, 1}, {"WPN_NEVER", true, 0}, {"WPN_ARMORY", true, words.armory_word},
        {"WPN_MISSION", true, words.mission_word},
    };
    const Radio* radio = nullptr;
    for (const Radio& r : radios)
        if (r.control == control) radio = &r;
    if (radio == nullptr) return false;
    if (radio->weapons) {
        // [orig: HostDialog_SetAllWeaponAvailability @0x557910 - every loadout_selectable row's word
        //  @0x55792F..0x55794B, the value's token @0x557956, every WEAPON_LIST row's cell
        //  @0x5579B9..0x5579DD]
        // [orig: @0x555af0 - every row's word @0x555b0f..0x555b2b, the token's switch @0x555b36,
        //  NW_MULTI_PLAYER's WEAPON_LIST @0x555b99, every row's cell @0x555bac..0x555bbd (dfx2.exe)]
        for (size_t i = 0; i < weapons.size() && i < gamecfg::kWeaponAvailabilityCount; ++i)
            if (weapons[i].loadout_selectable != 0) cfg.weapon_availability[i] = radio->word;
        const int list = host_screen_control(screen, title, "WEAPON_LIST");
        if (list >= 0) {
            const std::string token = weapon_word_token(menu_ui, radio->word, words);
            for (int row = 0; row < screen.table_row_count(list); ++row)
                screen.table_set_cell_text(list, row, kRestrictionCell, token);
        }
        return true;
    }
    // [orig: HostDialog_SetAllClassAvailability @0x557a10 - the ten class words @0x557A23..
    //  0x557A50, the token @0x557A70 / @0x557A96, every CLASS_LIST row's cell @0x557AB5..0x557AD9,
    //  the START gate @0x557AFC..0x557B4C]
    // [orig: HostDialog_SetAllWeaponRestrictions @0x555bf0 (a misnomer: the class words') - the ten
    //  words @0x555c03..0x555c30, NW_MULTI_PLAYER's CLASS_LIST @0x555c95, its START_GAME gate
    //  @0x555cdc (dfx2.exe)]
    std::fill(std::begin(cfg.class_availability), std::end(cfg.class_availability), radio->word);
    const int list = host_screen_control(screen, title, "CLASS_LIST");
    if (list >= 0) {
        const std::string token = class_word_token(menu_ui, radio->word);
        for (int row = 0; row < screen.table_row_count(list); ++row)
            screen.table_set_cell_text(list, row, kRestrictionCell, token);
    }
    dialog.set_class_words(screen, cfg.class_availability, kClassWordCount);
    return true;
}

uint32_t session_point_limit(int32_t cfg_limit) {
    // [orig: Game_ApplySessionSettingsToGlobals @0x551B67..0x551B75 (g_ScoreLimit
    // from maxKills_408), @0x551B84..0x551B8C (g_KillLimit from maxScore_40C)]
    return cfg_limit == 500 ? kSessionNoPointLimit : static_cast<uint32_t>(cfg_limit);
}

uint32_t session_koth_limit_minutes(int32_t cfg_limit) {
    // [orig: Game_ApplySessionSettingsToGlobals @0x551CD2..0x551CDF
    // (g_KothLimitMinutes from kothLimit_474)]
    return cfg_limit <= 0 ? kSessionNoKothLimitMinutes : static_cast<uint32_t>(cfg_limit);
}

TitlePlayerCaps title_player_caps(const Title& title) {
    // The session's ceiling is one past the title's most (Jointops.exe's 65, dfx2.exe's 251), the
    // host screen's its own word: the Title's (DFX2-8). [orig: g_HostScreenMaxPlayers @0x83C79C
    // (64)] [orig: dfx2.exe g_HostMaxPlayersCap @0x835f0c (100)]
    TitlePlayerCaps caps;
    caps.session = static_cast<uint32_t>(title.max_players + 1);
    caps.host_screen = title.max_players_host_screen;
    return caps;
}

uint32_t host_player_slot_limit(int32_t player_limit, bool serve_and_play,
                                int32_t network_connect_type, const Title& title) {
    // Dedicated hosting reserves the extra host slot in the live slot limit
    // (dword_24D211C); the session's advertised and admitted cap
    // (GameConfig::max_players) never carries it. The 65 ceiling tests the
    // PRE-increment cap (a dedicated 65 publishes 66) and applies only for
    // network connect type 1 (game.cfg's `networkconnecttype`, 1 by default and
    // as the NovaWorld screen writes it; the LAN screen writes 2, under which a
    // cap past the ceiling stands). The apply has no lower clamp: a blank
    // cap publishes 0 (1 dedicated). In session, though, the session's
    // creation clamps the cap into 1..65 before the apply runs, so the ceiling
    // never cuts and a blank cap publishes 1 (2 dedicated): session_player_cap,
    // which both hosts run first. The same live count gates BMS placements.
    // The ceiling is the title's: 65, or dfx2.exe's 251 (TitlePlayerCaps).
    // [orig: Game_ApplySessionSettingsToGlobals @0x551b26..0x551b48 - the type test
    //  `cmp networkConnectType_480, esi; jnz` @0x551B3B..0x551B41, the cap test
    //  @0x551B43..0x551B46, the store @0x551B48; CNapiGameSession_BuildAndCreateSession
    //  @0x56955D..0x56956C; Config_SetDefaults @0x54d1d4 (networkConnectType_480 = 1);
    //  Server_InitNewRoundState @ 0x51c8e0] [orig: Game_ApplySessionSettingsToGlobals - the
    //  type test `cmp channelType, esi; jnz` @0x54ED8B..0x54ED91, the cap @0x54ed93..0x54ed9a
    //  (dfx2.exe)]
    const int32_t ceiling = static_cast<int32_t>(title_player_caps(title).session);
    int64_t total = static_cast<int64_t>(player_limit) + (serve_and_play ? 0 : 1);
    if (network_connect_type == 1 && player_limit > ceiling) total = ceiling;
    // The limit is unsigned: a negative total (no session create clamped the
    // cap) reads as 0.
    return static_cast<uint32_t>(std::max<int64_t>(total, 0));
}

uint32_t GameConfig::player_slot_limit() const {
    // A config no session titled (the host-equivalent defaults a joiner's or the single player's
    // admission reads) has no title ceiling to apply: its cap plus the dedicated slot.
    if (title == nullptr)
        return static_cast<uint32_t>(max_players) + (dedicated_server ? 1u : 0u);
    return host_player_slot_limit(static_cast<int32_t>(max_players), !dedicated_server,
                                  network_connect_type, *title);
}

int32_t session_player_cap(int32_t player_limit, const Title& title) {
    // [orig: CNapiGameSession_BuildAndCreateSession @0x569567 (0x41)] [orig:
    //  @0x567707 (0xFB) (dfx2.exe)]
    const int32_t session_cap = static_cast<int32_t>(title_player_caps(title).session);
    if (player_limit < 1) return 1;
    return player_limit > session_cap ? session_cap : player_limit;
}

std::string mission_session_title(const rtxt::File *text_override, const std::string &map_file) {
    // [orig: Game_ApplySessionSettingsToGlobals @0x552112..0x55219E]
    const size_t dot = map_file.rfind('.');
    if (text_override != nullptr && dot != std::string::npos) {
        static const rtxt::File kNoTable;
        if (const rtxt::Entry *entry =
                rtxt::find_with_override(text_override, kNoTable, "TITLE", map_file.substr(0, dot))) {
            const size_t cr = entry->text.find('\r');
            const size_t count = cr == std::string::npos || cr > 31 ? 31 : cr;
            return entry->text.substr(0, count);
        }
    }
    return map_file.substr(0, 31);
}

} // namespace opennova::inmatch
