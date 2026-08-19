#pragma once

#include <cstdint>
#include <string>

namespace opennova::hud {

// THE FEED LINE POLICY — how one S2C 0x1E game event becomes the sentence and
// the color retail posts to its message feed
// [orig: NetPacket_HandleGameEvent @0x426270 -> HUD_FormatKillEventMessage
//  @0x422DA0 -> Chat_FormatMessage @0x422C60 -> Chat_AddDebugMessage @0x4987F0].
//
// The strings themselves are the game's own: every line is a "Canned Msg"
// template out of gametext with `$A`/`$B` substituted, never text we compose.
// The embedder supplies the resolved template and the two names (it owns the
// string table and the roster); this header owns the WITNESSED RULES — which
// types are suppressed, how the substitution runs, and which color each line
// carries.

// Packed ARGB feed colors [orig: the palette writer @0x51f240 unless noted].
inline constexpr uint32_t kFeedColorOwnKill = 0xFFFFFFFFu;   // the followed player's kill
inline constexpr uint32_t kFeedColorOtherKill = 0xFFA0A0A0u; // everyone else's
inline constexpr uint32_t kFeedColorTeamBlue = 0xFF80A0FFu;
inline constexpr uint32_t kFeedColorTeamRed = 0xFFFF5050u;
// The medic pair carries its own light blue [orig: 0xFF008CEE @0x426270].
inline constexpr uint32_t kFeedColorMedic = 0xFF008CEEu;
// Camp events 59/60 pass LITERAL colors to the sink, NOT the palette
// [orig: case 59 @0x62172/@0x62179, case 60 @0x62197/@0x62204].
inline constexpr uint32_t kFeedColorCampBlue = 0xFF01A3FFu;
inline constexpr uint32_t kFeedColorCampRed = 0xFFFF0000u;

// True when the event formats a string but posts NOTHING to the feed — the
// handler builds the WPNames line and returns without calling the sink
// [orig: 50/51/52/53 format-and-return @0x62051-0x62084], or routes only to
// the tip system [orig: 58 -> CTipSystem_HandleEvent 17 @0x62147].
bool feed_event_suppressed(uint8_t event_type);

// The line's packed ARGB. `own` marks an event the followed player took part
// in (its kill line is white, everyone else's grey [orig: white 0xFFFFFFFF /
// 0xFFA0A0A0 @0x51f240]); `team` is the camp events' team byte (1 = blue,
// 2 = red). A STRCND key naming BLUE/RED picks the team palette.
uint32_t feed_event_color(uint8_t event_type, const char *strcnd_key, bool own,
                          uint8_t team);

// Substitute the actor names into a "Canned Msg" template
// [orig: Chat_FormatMessage @0x422C60 — `$A` = attacker, `$B` = victim]. Every
// space in the result comes from the template; nothing is inserted here.
std::string feed_format_line(const std::string &tmpl, const std::string &attacker,
                             const std::string &victim);

// Camp events 59/60 build their key CLIENT-side: the base key carries no team
// token, and the handler appends the team suffix before the lookup
// [orig: case 59 @0x62165 / case 60 @0x62190 — GameText_GetString("Canned Msg",
//  "STRCND_{FULLYCAMPED,LOSTCAMP}_{BLUE,RED}")]. Retail emits for team 1/2
// only (there is no else branch), so an out-of-range team yields an empty key
// and the line is dropped. Returns "" when the type is not a camp event.
std::string feed_camp_key(uint8_t event_type, uint8_t team);

} // namespace opennova::hud
