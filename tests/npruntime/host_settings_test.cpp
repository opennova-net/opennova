#include <runtime/inmatch/host_settings.h>
#include "common/test_expect.h"
#include <limits>

using namespace opennova::inmatch;

int main() {
    GameConfig config;
    apply_fresh_host_rule_defaults(config);
    int32_t player_limit = 32;
    bool serve_and_play = true;
    const auto read = [&](const char* name, const char* value) {
        return apply_host_dialog_control(config, player_limit, serve_and_play, name, value);
    };
    // Accept's edit reader uses strtol prefixes; live limits preserve their
    // separate sentinels and the TIME / MAX_KOTH name swap.
    TEST_EXPECT(read("TIME", "  +45 minutes"));
    TEST_EXPECT(read("RESPAWN", "8"));
    TEST_EXPECT(read("MAX_KOTH", "0"));
    TEST_EXPECT(read("KILL_LIMIT", "500"));
    TEST_EXPECT(read("MAX_SCORE", "500"));
    TEST_EXPECT(config.respawn_time == 45 && config.respawn_timeout == 8);
    TEST_EXPECT(config.time_limit_minutes == 0x2222222u);
    TEST_EXPECT(config.score_limit == 65000 && config.max_score == 65000);
    TEST_EXPECT(read("MAX_KOTH", "-1"));
    TEST_EXPECT(config.time_limit_minutes == 0x2222222u);
    TEST_EXPECT(read("MAX_KOTH", "500"));
    TEST_EXPECT(read("KILL_LIMIT", "501"));
    TEST_EXPECT(read("MAX_SCORE", "499"));
    TEST_EXPECT(config.time_limit_minutes == 500 && config.score_limit == 501 && config.max_score == 499);
    TEST_EXPECT(read("TIME", ""));
    TEST_EXPECT(config.respawn_time == 0);
    // Toggling a control touches exactly its own bit, including inverted ones.
    config.mp_attributes = 0xA0000000u;
    for (const char* name : {"TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TRACERS"})
        TEST_EXPECT(read(name, "0"));
    TEST_EXPECT(read("TEAM_CHOOSE", "1"));
    TEST_EXPECT(read("CLAYMORE_PREF", "1"));
    TEST_EXPECT(config.mp_attributes == 0xA000860Du);
    for (const char* name : {"TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TRACERS"})
        TEST_EXPECT(read(name, "1"));
    TEST_EXPECT(read("TEAM_CHOOSE", "0"));
    TEST_EXPECT(read("CLAYMORE_PREF", "0"));
    TEST_EXPECT(config.mp_attributes == 0xA0000000u);
    TEST_EXPECT(read("ALLOW_SPECTATORS", "1"));
    TEST_EXPECT(config.spectator_slots == -1);
    config.spectator_slots = 7;
    TEST_EXPECT(read("ALLOW_SPECTATORS", "1"));
    TEST_EXPECT(config.spectator_slots == 7);
    TEST_EXPECT(read("ALLOW_SPECTATORS", "0"));
    TEST_EXPECT(config.spectator_slots == 0);
    TEST_EXPECT(read("SERVER_PASSWORD", "abcdefghijklmnopqr"));
    TEST_EXPECT(config.server_password == "abcdefghijklmnopq");
    TEST_EXPECT(read("SERVER_MESSAGE", "Join us"));
    TEST_EXPECT(config.custom_text == "Join us");
    TEST_EXPECT(read("MAX_PLAYERS", "99"));
    TEST_EXPECT(player_limit == 64 && host_player_slot_limit(player_limit, serve_and_play) == 64);
    TEST_EXPECT(read("SERVERTYPE", "1"));
    TEST_EXPECT(!serve_and_play && host_player_slot_limit(player_limit, serve_and_play) == 65);
    TEST_EXPECT(read("MAX_PLAYERS", ""));
    // No lower clamp: a blank cap publishes 0, or 1 as the dedicated slot alone;
    // the 65 ceiling tests the pre-increment cap, so a dedicated 65 publishes 66.
    TEST_EXPECT(player_limit == 0 && host_player_slot_limit(player_limit, serve_and_play) == 1);
    TEST_EXPECT(host_player_slot_limit(0, true) == 0 && host_player_slot_limit(-3, true) == 0);
    TEST_EXPECT(host_player_slot_limit(65, false) == 66 && host_player_slot_limit(65, true) == 65);
    TEST_EXPECT(host_player_slot_limit(66, false) == 65 && host_player_slot_limit(99, true) == 65);
    TEST_EXPECT(read("TAKEOVER_TIME", "20"));
    TEST_EXPECT(read("LFP_TAKEOVER", "3"));
    TEST_EXPECT(config.capture_duration_seconds == 20 && config.capture_speed_setting == 3);
    TEST_EXPECT(!read("UNKNOWN", "1"));
    // The populate inverse: what the host screen shows for the current request.
    // [orig: UI_PopulateHostSettingsFromConfig @ 0x555fe0]
    const auto shown = [&](std::string_view name) {
        return host_dialog_value(config, player_limit, serve_and_play, name);
    };
    TEST_EXPECT(shown("SERVER_PASSWORD") == "abcdefghijklmnopq" && shown("SERVER_MESSAGE") == "Join us");
    TEST_EXPECT(shown("RESPAWN") == "8" && shown("TIME") == "0" && shown("MAX_KOTH") == "500");
    TEST_EXPECT(shown("KILL_LIMIT") == "501" && shown("MAX_SCORE") == "499");
    TEST_EXPECT(shown("SERVERTYPE") == "1" && shown("MAX_PLAYERS") == "0");
    TEST_EXPECT(shown("TAKEOVER_TIME") == "20" && shown("LFP_TAKEOVER") == "3");
    TEST_EXPECT(shown("ALLOW_SPECTATORS") == "0" && shown("ALLOW_AI") == "1");
    // The inverted flag spins show a CLEAR bit as 1; the direct ones the bit itself.
    for (const char* name : {"TEAM_FF", "FRIENDLY_TAG", "FF_WARNING", "TRACERS"})
        TEST_EXPECT(shown(name) == "1");
    TEST_EXPECT(shown("TEAM_CHOOSE") == "0" && shown("CLAYMORE_PREF") == "0");
    // The sentinels map back to their dialog numbers; MAX_PLAYERS shows at most 64.
    TEST_EXPECT(read("KILL_LIMIT", "500") && read("MAX_SCORE", "500") && read("MAX_KOTH", "0"));
    TEST_EXPECT(shown("KILL_LIMIT") == "500" && shown("MAX_SCORE") == "500" && shown("MAX_KOTH") == "0");
    player_limit = 99;
    TEST_EXPECT(shown("MAX_PLAYERS") == "64");
    TEST_EXPECT(read("SERVERTYPE", "0") && shown("SERVERTYPE") == "0");
    TEST_EXPECT(read("GAME_LOCATION", "USA") && shown("GAME_LOCATION") == "USA");
    TEST_EXPECT(read("GAME_NAME", "Night Ops") && shown("GAME_NAME") == "Night Ops");
    TEST_EXPECT(read("CONNECTIONSPEED", "5") && read("MAX_FF_KILLS", "3"));
    TEST_EXPECT(shown("CONNECTIONSPEED") == "5" && shown("MAX_FF_KILLS") == "3");
    TEST_EXPECT(shown("UNKNOWN").empty());
    // Every dialog control round-trips through its own populate value.
    for (std::string_view name : host_dialog_controls()) {
        GameConfig copy = config;
        int32_t copy_limit = player_limit;
        bool copy_serve = serve_and_play;
        TEST_EXPECT(apply_host_dialog_control(copy, copy_limit, copy_serve, name, shown(name)));
        TEST_EXPECT(host_dialog_value(copy, copy_limit, copy_serve, name) == shown(name));
    }
    return 0;
}
