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
    TEST_EXPECT(player_limit == 0 && host_player_slot_limit(player_limit, serve_and_play) == 1);
    TEST_EXPECT(read("TAKEOVER_TIME", "20"));
    TEST_EXPECT(read("LFP_TAKEOVER", "3"));
    TEST_EXPECT(config.capture_duration_seconds == 20 && config.capture_speed_setting == 3);
    TEST_EXPECT(!read("UNKNOWN", "1"));
    return 0;
}
