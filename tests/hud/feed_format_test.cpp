// The message-feed line policy [orig: NetPacket_HandleGameEvent @0x426270 ->
// HUD_FormatKillEventMessage @0x422DA0 -> Chat_FormatMessage @0x422C60]:
// the suppression set, the color rules (medic, camp literals, team palette,
// own-vs-other kill), $A/$B substitution, and the camp key's team suffix.
// Also pins the witnessed 0x1E classification corrections in npwire.
#include <cstdio>
#include <string>

#include <hud/feed_format.h>
#include <npwire/ingame_decode.h>

using namespace opennova;
using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// The four LFP result types format a string but post nothing, and 58 goes to
// the tip system only [orig: @0x62051-0x62084, @0x62147].
void test_suppression_set() {
    for (uint8_t t : { 50, 51, 52, 53, 58 }) CHECK(feed_event_suppressed(t));
    // Everything that DOES reach the feed must not be suppressed.
    for (uint8_t t : { 4, 24, 38, 43, 45, 54, 56, 59, 60 })
        CHECK(!feed_event_suppressed(t));
}

// Medic lines carry 0xFF008CEE regardless of who took part [orig: @0x426270].
void test_medic_color_is_unconditional() {
    CHECK(feed_event_color(38, "STRCND42", /*own=*/true, 0) == kFeedColorMedic);
    CHECK(feed_event_color(45, "STRCND45", /*own=*/false, 0) == kFeedColorMedic);
}

// Camp events use LITERAL colors keyed on the team byte, not the palette
// [orig: case 59 @0x62172/@0x62179, case 60 @0x62197/@0x62204].
void test_camp_colors_are_literal() {
    CHECK(feed_event_color(59, "STRCND_FULLYCAMPED", false, 1) == kFeedColorCampBlue);
    CHECK(feed_event_color(59, "STRCND_FULLYCAMPED", false, 2) == kFeedColorCampRed);
    CHECK(feed_event_color(60, "STRCND_LOSTCAMP", false, 1) == kFeedColorCampBlue);
}

// A team-named canned key takes the team palette; a plain kill is white for
// the followed player and grey for everyone else.
void test_team_and_kill_colors() {
    CHECK(feed_event_color(41, "STRCND_PSP_BLUEWARNING", false, 0) == kFeedColorTeamBlue);
    CHECK(feed_event_color(42, "STRCND_PSP_REDWARNING", false, 0) == kFeedColorTeamRed);
    CHECK(feed_event_color(4, "STRCND04", /*own=*/true, 0) == kFeedColorOwnKill);
    CHECK(feed_event_color(4, "STRCND04", /*own=*/false, 0) == kFeedColorOtherKill);
}

// $A/$B substitution inserts nothing of its own — all spacing is the template's.
void test_substitution() {
    CHECK(feed_format_line("$A killed $B.", "SPAGHETTI", "Belsman") ==
          "SPAGHETTI killed Belsman.");
    CHECK(feed_format_line("$B has revived $A.", "elk road", "A-99") ==
          "A-99 has revived elk road.");
    // A template with no tokens is returned verbatim; a lone '$' survives.
    CHECK(feed_format_line("Round over.", "x", "y") == "Round over.");
    CHECK(feed_format_line("cost: $5", "x", "y") == "cost: $5");
    // Empty names substitute empty — the template's spacing still stands.
    CHECK(feed_format_line("$A killed $B.", "", "") == " killed .");
}

// The camp key gets its team suffix client-side; team 1/2 only.
void test_camp_key() {
    CHECK(feed_camp_key(59, 1) == "STRCND_FULLYCAMPED_BLUE");
    CHECK(feed_camp_key(59, 2) == "STRCND_FULLYCAMPED_RED");
    CHECK(feed_camp_key(60, 1) == "STRCND_LOSTCAMP_BLUE");
    CHECK(feed_camp_key(60, 2) == "STRCND_LOSTCAMP_RED");
    CHECK(feed_camp_key(59, 0).empty());   // no else branch in retail
    CHECK(feed_camp_key(59, 3).empty());
    CHECK(feed_camp_key(4, 1).empty());    // not a camp event
}

// The witnessed classification corrections: the medic pair is not a kill, and
// killer-less deaths are their own kind (their victim/aux slots are zero on
// the wire) [orig: 0x426270 cases; GameEvent_PlayerDeath @0x516DD0].
void test_game_event_classification() {
    CHECK(game_event_kind(38) == GameEventKind::Medic);
    CHECK(game_event_kind(45) == GameEventKind::Medic);
    for (uint8_t t : { 1, 2, 3, 22, 23, 25, 26 })
        CHECK(game_event_kind(t) == GameEventKind::SelfDeath);
    for (uint8_t t : { 4, 10, 24, 32, 49 })
        CHECK(game_event_kind(t) == GameEventKind::Kill);
    for (uint8_t t : { 43, 56, 59, 60 })
        CHECK(game_event_kind(t) == GameEventKind::Objective);
    // 39 has no emitter anywhere in the image — it stays Other.
    CHECK(game_event_kind(39) == GameEventKind::Other);
    // The canned-key table is unchanged by the reclassification.
    CHECK(std::string(game_event_strcnd_key(38)) == "STRCND42");
    CHECK(std::string(game_event_strcnd_key(45)) == "STRCND45");
    CHECK(game_event_strcnd_key(19) == nullptr);   // team/gametype-keyed at runtime
}

} // namespace

int main() {
    test_suppression_set();
    test_medic_color_is_unconditional();
    test_camp_colors_are_literal();
    test_team_and_kill_colors();
    test_substitution();
    test_camp_key();
    test_game_event_classification();
    if (failures == 0) std::printf("feed_format_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
