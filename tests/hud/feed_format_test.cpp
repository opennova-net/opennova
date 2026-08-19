// The message-feed line policy [orig: NetPacket_HandleGameEvent @0x426270 ->
// HUD_FormatKillEventMessage @0x422DA0 -> Chat_FormatMessage @0x422C60]:
// the suppression set, the verbose gate, the per-case color table, the
// $A/$B substitution with the STRCND48 bonus re-compose, and the camp keys.
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
// the tip system only [orig: @0x42702E-@0x42716D, @0x427202].
void test_suppression_set() {
    for (uint8_t t : { 50, 51, 52, 53, 58 }) CHECK(feed_event_suppressed(t));
    // Everything that DOES reach the feed must not be suppressed.
    for (uint8_t t : { 4, 24, 38, 43, 45, 54, 56, 59, 60 })
        CHECK(!feed_event_suppressed(t));
}

// The grey branch of the suicide/kill families and the bonus trio return
// without posting when the verbose toggle is off; the friendly-fire trio,
// the killer-less deaths, 24 and 49 post unconditionally
// [orig: the 13 g_MpVerbose2 tests @0x426472..@0x4267C6].
void test_verbose_gate() {
    for (uint8_t t : { 1, 2, 3, 4, 5, 6, 10, 11, 12, 13, 14, 15, 32, 33, 34 })
        CHECK(feed_event_verbose_only(t));
    for (uint8_t t : { 7, 8, 9, 22, 23, 24, 25, 26, 27, 38, 49, 59 })
        CHECK(!feed_event_verbose_only(t));
}

// The kill/death families: white when the local player took part, grey
// 0xFFAFAFAF otherwise [orig: -1 / -5263441 per case, e.g. @0x426539].
void test_own_other_colors() {
    for (uint8_t t : { 1, 4, 10, 13, 22, 24, 25, 26, 49 }) {
        CHECK(feed_event_color(t, /*own=*/true, 0) == kFeedColorWhite);
        CHECK(feed_event_color(t, /*own=*/false, 0) == kFeedColorGrey);
    }
    // The friendly-fire trio is white for EVERYONE [orig: 7/8/9 @0x4265FF
    // pass -1 unconditionally], as are 16/17/18 (g_hudColorTable[0] = -1
    // [orig: HUD_InitTeamColorTable @0x51F245]) and the announcement lines.
    for (uint8_t t : { 7, 8, 9, 16, 17, 18, 27, 31, 35, 36, 37 })
        CHECK(feed_event_color(t, /*own=*/false, 0) == kFeedColorWhite);
    // The multi-kill bonus trio is yellow for everyone [orig: -256 @0x4266C5].
    for (uint8_t t : { 32, 33, 34 })
        CHECK(feed_event_color(t, /*own=*/false, 0) == kFeedColorBonusKill);
}

// Medic lines carry 0xFF008CEE regardless of who took part [orig: the shared
// -16741138 post of cases 38/39/45], as do the SSKB bonus lines
// [orig: 46/47 @0x427A68/@0x427AD8].
void test_medic_color_is_unconditional() {
    for (uint8_t t : { 38, 39, 45, 46, 47 }) {
        CHECK(feed_event_color(t, /*own=*/true, 0) == kFeedColorMedic);
        CHECK(feed_event_color(t, /*own=*/false, 0) == kFeedColorMedic);
    }
}

// The one-off literals [orig: case 40 @0x4263E8 -65536; case 48 @0x427AE5
// -32768] and the PSP/LFP team palette [orig: -16732161 / -65536, e.g. the
// posts @0x427519 (54, blue) and @0x427717 (42, red)].
void test_objective_colors() {
    CHECK(feed_event_color(40, false, 0) == kFeedColorRed);
    CHECK(feed_event_color(48, false, 0) == kFeedColorMortar);
    for (uint8_t t : { 41, 43, 54, 56 })
        CHECK(feed_event_color(t, false, 0) == kFeedColorBlue);
    for (uint8_t t : { 42, 44, 55, 57 })
        CHECK(feed_event_color(t, false, 0) == kFeedColorRed);
    // Camp events color on the team byte [orig: 59 @0x4272D7 / 60 @0x4273DC].
    CHECK(feed_event_color(59, false, 1) == kFeedColorBlue);
    CHECK(feed_event_color(59, false, 2) == kFeedColorRed);
    CHECK(feed_event_color(60, false, 1) == kFeedColorBlue);
    CHECK(feed_event_color(60, false, 2) == kFeedColorRed);
}

// $A/$B substitution inserts nothing of its own — all spacing is the
// template's [orig: Chat_FormatMessage @0x422C60].
void test_substitution() {
    CHECK(feed_format_line("$A killed $B.", "SPAGHETTI", "Belsman") ==
          "SPAGHETTI killed Belsman.");
    CHECK(feed_format_line("$B has revived $A.", "elk road", "A-99") ==
          "A-99 has revived elk road.");
    // The needle match is case-insensitive
    // [orig: String_ReplaceAllCaseInsensitive @0x422970 toupper compare].
    CHECK(feed_format_line("$a killed $b.", "K", "V") == "K killed V.");
    // A template with no tokens is returned verbatim; a lone '$' survives.
    CHECK(feed_format_line("Round over.", "x", "y") == "Round over.");
    CHECK(feed_format_line("cost: $5", "x", "y") == "cost: $5");
    // Empty names substitute empty — the template's spacing still stands.
    CHECK(feed_format_line("$A killed $B.", "", "") == " killed .");
    // The passes are SEQUENTIAL over the whole line: a "$B" inserted by the
    // $A pass is consumed by the $B pass [orig: two full
    // String_ReplaceAllCaseInsensitive walks @0x422CB4 then @0x422CD6].
    CHECK(feed_format_line("$A hi", "$B", "V") == "V hi");
    // Within one pass the scan resumes after the inserted text — a name
    // containing its own needle does not loop [orig: cursor +=
    // strlen(replacement) @0x422ADD].
    CHECK(feed_format_line("$A hi", "$A", "V") == "$A hi");
}

// The STRCND48 bonus re-compose fires only when the aux actor resolved to the
// local player [orig: sprintf(haystack, STRCND48, format, extra) @0x422CA2,
// gated on the empty extra string @0x422C84].
void test_bonus_recompose() {
    CHECK(feed_format_line("$A killed $B.", "K", "V", "ME", "%s - Bonus for %s") ==
          "K killed V. - Bonus for ME");
    CHECK(feed_format_line("$A killed $B.", "K", "V", "", "%s - Bonus for %s") ==
          "K killed V.");
    CHECK(feed_format_line("$A killed $B.", "K", "V", "ME", "") ==
          "K killed V.");
}

// The camp key gets its team suffix client-side; team 1/2 only. The WPNames
// level key is built from the wire index PLUS ONE [orig: sprintf(key,
// "STRWPNAME%03d", v141 + 1) @0x4272EC/@0x4273F1], and the camp template's %s
// takes that WPNames string [orig: sprintf @0x427327/@0x42736B].
void test_camp_keys_and_line() {
    CHECK(feed_camp_key(59, 1) == "STRCND_FULLYCAMPED_BLUE");
    CHECK(feed_camp_key(59, 2) == "STRCND_FULLYCAMPED_RED");
    CHECK(feed_camp_key(60, 1) == "STRCND_LOSTCAMP_BLUE");
    CHECK(feed_camp_key(60, 2) == "STRCND_LOSTCAMP_RED");
    CHECK(feed_camp_key(59, 0).empty());   // no else branch in retail
    CHECK(feed_camp_key(59, 3).empty());
    CHECK(feed_camp_key(4, 1).empty());    // not a camp event
    CHECK(feed_camp_wpname_key(0) == "STRWPNAME001");
    CHECK(feed_camp_wpname_key(11) == "STRWPNAME012");
    CHECK(feed_format_camp_line("Joint Ops team has fully camped '%s'",
              "North Village") == "Joint Ops team has fully camped 'North Village'");
    CHECK(feed_format_camp_line("no token", "x") == "no token");
}

// The witnessed classification corrections: the medic trio is not a kill, and
// killer-less deaths are their own kind (their victim/aux slots are zero on
// the wire) [orig: the 0x426270 cases; GameEvent_PlayerDeath @0x516DD0].
void test_game_event_classification() {
    for (uint8_t t : { 38, 39, 45 })
        CHECK(game_event_kind(t) == GameEventKind::Medic);
    for (uint8_t t : { 1, 2, 3, 22, 23, 25, 26 })
        CHECK(game_event_kind(t) == GameEventKind::SelfDeath);
    for (uint8_t t : { 4, 10, 24, 32, 49 })
        CHECK(game_event_kind(t) == GameEventKind::Kill);
    for (uint8_t t : { 43, 56, 59, 60 })
        CHECK(game_event_kind(t) == GameEventKind::Objective);
    // The canned-key table is unchanged by the reclassification.
    CHECK(std::string(game_event_strcnd_key(38)) == "STRCND42");
    CHECK(std::string(game_event_strcnd_key(39)) == "STRCND43");
    CHECK(std::string(game_event_strcnd_key(45)) == "STRCND45");
    CHECK(game_event_strcnd_key(19) == nullptr);   // team/gametype-keyed at runtime
}

} // namespace

int main() {
    test_suppression_set();
    test_verbose_gate();
    test_own_other_colors();
    test_medic_color_is_unconditional();
    test_objective_colors();
    test_substitution();
    test_bonus_recompose();
    test_camp_keys_and_line();
    test_game_event_classification();
    if (failures == 0) std::printf("feed_format_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
