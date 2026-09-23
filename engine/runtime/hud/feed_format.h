#pragma once

#include <runtime/hud/game_text_lookup.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::hud {

// Coarse classification of a 0x1E event_type, derived structurally from the
// handler's switch [orig: 0x426270]. Drives the viewer's kill-feed styling.
enum class GameEventKind : uint8_t {
	Other = 0,     // single-actor canned / misc HUD message
	Kill,          // attacker killed victim (the kill feed proper)
	Objective,     // flag / capture / zone control / camp events
	// A death with NO killer: suicide (1/2/3, the emitter picks 1+rand(3)) and
	// the killer-less deaths (22/23/25/26 "$A is dead/died/drowned."). The
	// handler leaves the victim/aux slots LITERAL ZERO on these
	// [orig: GameEvent_PlayerDeath @0x516DD0 leaves v41/v42 = 0], so a consumer
	// that reads them charges the death to entity 0 (the listen server's own
	// player) — they must be ignored for this kind.
	SelfDeath,
	// The medic lines, drawn in their own light blue 0xFF008CEE and NOT kills
	// [orig: cases 38 "$B has revived $A." @0x42640F / 45 "…medical
	// attention…" @0x426442 / the emitterless 39 @0x426456 — one shared post].
	Medic,
};
GameEventKind game_event_kind(uint8_t event_type);

// The witnessed "Canned Msg" string key (e.g. "STRCND04") an event_type maps to,
// or nullptr when the type has none. Faithful to the 0x426270 switch.
const char *game_event_strcnd_key(uint8_t event_type);

// THE FEED LINE POLICY — how one S2C 0x1E game event becomes the sentence and
// the color retail posts to its message feed
// [orig: NetPacket_HandleGameEvent @0x426270 -> HUD_FormatKillEventMessage
//  @0x422DA0 -> Chat_FormatMessage @0x422C60 -> Chat_AddMessageChannel2 @0x4987F0].
//
// The strings themselves are the game's own: every line is a "Canned Msg"
// template out of gametext with `$A`/`$B` substituted, never text we compose.
// The embedder supplies the resolved template and the actor names (it owns the
// string table and the roster); this header owns the WITNESSED RULES — which
// types are suppressed, which are gated on the verbose toggle, how the
// substitution runs, and which color each line carries.

// Packed ARGB feed colors — every value is a literal the 0x426270 handler
// passes to the sink for at least one event type.
inline constexpr uint32_t kFeedColorWhite = 0xFFFFFFFFu;    // -1, the default line
inline constexpr uint32_t kFeedColorGrey = 0xFFAFAFAFu;     // -5263441, a kill the local player had no part in
inline constexpr uint32_t kFeedColorBlue = 0xFF00AFFFu;     // -16732161, every BLUE-team objective line
inline constexpr uint32_t kFeedColorRed = 0xFFFF0000u;      // -65536, every RED-team objective line (and type 40)
inline constexpr uint32_t kFeedColorMedic = 0xFF008CEEu;    // -16741138, the medic pair + the SSKB bonus pair
inline constexpr uint32_t kFeedColorBonusKill = 0xFFFFFF00u; // -256, the multi-kill bonus trio 32/33/34
inline constexpr uint32_t kFeedColorMortar = 0xFFFF8000u;   // -32768, type 48's mortar-designation line

// True when the event formats a string but posts NOTHING to the feed — the
// handler builds the WPNames line and returns without calling the sink
// [orig: 50/51/52/53 format-and-return @0x42702E/@0x427084/@0x4270DA/@0x42716D],
// or routes only to the tip system
// [orig: 58 @0x427202 -> CTipSystem_HandleEvent 17 @0x427286].
bool feed_event_suppressed(uint8_t event_type);

// True when a line the local player took no part in posts only while the MP
// verbose toggle is on: the grey branch of the suicide/kill families returns
// without posting when `!g_MpVerbose2` [orig: types 1-6/10-15 e.g. @0x426472
// (case 1) and @0x426547 (case 4); the bonus trio 32/33/34 @0x42668C]. The
// toggle lives at g_MpVerbose2 @0x24D2154 — seeded from the session settings
// [orig: apply_session_settings_to_globals @0x551D0F] and flipped by the
// keybind that announces STRMISC_VERBOSE_ON/OFF [orig: @0x49B78F].
bool feed_event_verbose_only(uint8_t event_type);

// The line's packed ARGB — a structural translation of the per-case color the
// 0x426270 switch hands the sink. `own` marks an event the local player took
// part in (attacker or victim); `team` is the camp events' team byte
// (1 = blue, 2 = red).
//   * kill/death families 1-6, 10-15, 22-26, 49: own white / other grey
//   * friendly-fire trio 7/8/9 and 16-18 (g_hudColorTable[0] = -1
//     [orig: HUD_InitTeamColorTable @0x51F245]) and 27-31/35-37: white
//   * multi-kill bonus 32/33/34: yellow [orig: push -256 @0x4266C5]
//   * medic pair 38/45 (+ the emitterless 39) and SSKB 46/47: 0xFF008CEE
//   * PSP/LFP blue 41/43/54/56 vs red 42/44/55/57 [orig: -16732161 / -65536,
//     e.g. @0x427519/@0x427717]; type 40 red; type 48 mortar orange
//   * camp 59/60: blue or red by the team byte [orig: @0x4272D7/@0x4273DC]
uint32_t feed_event_color(uint8_t event_type, bool own, uint8_t team);

// Substitute the actor names into a "Canned Msg" template — the ported
// Chat_FormatMessage [orig: @0x422C60]. When `extra` (the aux actor's name,
// filled only when that actor is the local player) and `bonus_tmpl` (the
// STRCND48 "%s - Bonus for %s" template) are both non-empty, the line is first
// re-composed through the bonus template [orig: sprintf @0x422CA2]. `$A` and
// `$B` are then replaced with the attacker and victim names in that order,
// case-insensitively, every occurrence, with the scan resuming after each
// inserted name [orig: String_ReplaceAllCaseInsensitive @0x422970 — cursor +=
// strlen(replacement) @0x422ADD]. Every space comes from the template; nothing
// is inserted around the names.
std::string feed_format_line(const std::string &tmpl, const std::string &attacker,
                             const std::string &victim,
                             const std::string &extra = std::string(),
                             const std::string &bonus_tmpl = std::string());

// THE PLAYER-CHAT CHANNEL TABLE — where an S2C 0x14 line lands and which
// colour it carries, by its channel byte [orig: Chat_DispatchToChannel
// @0x42b910 — the 0..0xE switch @0x42b95d/@0x42ba17; every colour is a
// g_hudColorTable entry written by HUD_InitTeamColorTable @0x51f245..0x51f2b3].
enum class ChatSink : uint8_t {
	System = 0,   // Chat_AddMessageChannel2 — the SYSTEM ring [orig: @0x42bb0c]
	Chat = 1,     // Chat_AddMessageChannel1 — the CHAT ring
	Queue = 2,    // channel 8: CMessageQueue_Enqueue @0x42bab8 (not a ring)
	Channel3 = 3, // channel 14: Chat_AddMessageChannel3 @0x42bb01 (unported ring)
};
// The g_hudColorTable immediates the dispatcher reads [orig: @0x51f245..0x51f2b3].
inline constexpr uint32_t kHudColorWhite = 0xFFFFFFFFu;     // table[0]
inline constexpr uint32_t kHudColorGreen = 0xFF00FF00u;     // table[1]  dword_24C183C
inline constexpr uint32_t kHudColorLightBlue = 0xFF80A0FFu; // table[3]  g_hudColorLightBlue
inline constexpr uint32_t kHudColorYellow = 0xFFF0F000u;    // table[4]  `color` @0x24C1848
inline constexpr uint32_t kHudColorSalmon = 0xFFFF5050u;    // table[5]  dword_24C184C
inline constexpr uint32_t kHudColorMagenta = 0xFFFF40FFu;   // table[6]  dword_24C1850
inline constexpr uint32_t kHudColorOrange = 0xFFFF8020u;    // table[7]  dword_24C1854
inline constexpr uint32_t kHudColorCyan = 0xFF00EAE7u;      // table[10] dword_24C1860
// channel -> sink: 0 and every channel outside 1..14 post to the SYSTEM ring
// (the switch default @0x42baf8 -> @0x42bb0c); 8 enqueues; 14 is channel 3;
// the rest are CHAT. Channel 13 additionally re-targets the HUD tracked
// entity at the sender [orig: @0x42b9e8..0x42ba09] — a presenter leg.
ChatSink chat_channel_sink(int channel);
// channel -> colour: 1/4/5 light blue, 2 green, 3 yellow, 7 orange, 9 salmon,
// 11 cyan, 12 magenta, 0/6/10/13/14/default white.
uint32_t chat_channel_color(int channel);

// Compose a camp line: the template's `%s` takes the camp level's WPNames
// string [orig: sprintf(msg, GameText("Canned Msg", key),
// GameText("WPNames", wpname_key)) @0x427327/@0x42736B, posted @0x42737F].
std::string feed_format_camp_line(const std::string &tmpl,
                                  const std::string &wpname);

// Camp events 59/60 build their key CLIENT-side: the base key carries no team
// token, and the handler appends the team suffix before the lookup
// [orig: case 59 @0x4272D7 / case 60 @0x4273DC — GameText_GetString("Canned
//  Msg", "STRCND_{FULLYCAMPED,LOSTCAMP}_{BLUE,RED}")]. Retail emits for team
// 1/2 only (there is no else branch), so an out-of-range team yields an empty
// key and the line is dropped. Returns "" when the type is not a camp event.
std::string feed_camp_key(uint8_t event_type, uint8_t team);

// The two camp events, the only 0x1E types whose slots are not actors: the
// attacker byte is the LEVEL and the victim byte the TEAM (see feed_camp_key).
inline constexpr uint8_t kFeedEventFullyCamped = 59;
inline constexpr uint8_t kFeedEventLostCamp = 60;
inline bool feed_event_is_camp(uint8_t event_type) {
	return event_type == kFeedEventFullyCamped || event_type == kFeedEventLostCamp;
}

// The WPNames key for a camp event's level slot: the wire attacker byte is the
// level index and the key is built from index PLUS ONE
// [orig: sprintf(key, "STRWPNAME%03d", v141 + 1) @0x4272EC/@0x4273F1 — unlike
//  the LFP cases 50-53/58, which use the raw index].
std::string feed_camp_wpname_key(uint8_t level_index);

// The MP verbose toggle: a line the local player took no part in posts only
// while it is on. Retail seeds it on from the session settings; the keybind
// that flips it (STRMISC_VERBOSE_ON/OFF) is unported, so the seed stands
// [orig: g_MpVerbose2 @0x24D2154, seeded verbose-on from the session settings
//  @0x551D0F; the flip keybind @0x49B78F].
inline constexpr bool kMpVerboseDefault = true;

// One folded 0x1E game event as the feed reads it: the wire slots plus the
// GameEventKind byte (replication::ClientGameEvent, minus the position, without
// the net include).
struct FeedEventInput {
	uint8_t event_type = 0;
	uint8_t attacker_index = 0xFF;
	uint8_t victim_index = 0xFF;
	uint8_t aux_index = 0xFF;
	uint8_t kind = 0;
	int16_t pos_x = 0; // 46/47 carry the signed bonus count here.
};

// One feed row — everything the presenter needs to post the line: the compose
// form it branches on (`camp`), the "Canned Msg" key, the resolved actor
// names, the aux actor's name when the local player earned the bonus, the
// camp level's WPNames key, the packed ARGB color and whether the local
// player took part.
struct FeedRow {
	uint8_t event_type = 0;
	uint8_t kind = 0;    // GameEventKind
	bool camp = false;
	bool own = false;
	bool announce = false;
	bool attacker_is_value = false;
	bool victim_is_value = false;
	std::string key;
	std::string attacker;    // empty on camp rows
	std::string victim;      // empty on camp rows
	std::string extra;       // the aux actor's name, only when it is the local player
	std::string wpname_key;  // camp rows only
	uint32_t color = kFeedColorWhite;
};

// The roster lookup a row resolves actor names through: a wire index (a pool-0
// INDEX) -> display name, "" when unknown. Never called for 0xFF.
struct FeedActor {
	std::string name;
	uint8_t team = 0;
};
using FeedActorLookup = std::function<FeedActor(uint8_t)>;
struct FeedContext {
	uint16_t self_handle = 0xFFFF;
	bool mp_verbose = kMpVerboseDefault;
	uint32_t game_type = 0;
};

// Fold this frame's game events into feed rows — one per line the original
// posts to its message feed. `self_handle` is the local player's entity
// handle (0xFFFF = none; a pool-0 INDEX on the wire is the handle
// (0<<12)|index, so the slots compare directly). Suppressed types never
// surface; a camp event whose team is outside 1/2, or a type keyed at runtime
// by team/gametype (no STRCND key), draws nothing.
// [orig: NetPacket_HandleGameEvent @0x426270 — the LFP result set formats and
//  returns @0x42702E-@0x42716D; 58 posts to the tip system only @0x427202]
void feed_event_rows(const FeedEventInput *events, std::size_t count,
                     const FeedContext &context,
                     const FeedActorLookup &actor_of, std::vector<FeedRow> &out);

// Resolve empty actor names without substituting "Unknown" for numeric/empty values.
// [orig: HUD_FormatKillEventMessage @ 0x422DA0; sub_422D00 @ 0x422D00]
std::string feed_format_row(const FeedRow &, const std::string &tmpl,
        const std::string &unknown, const std::string &bonus_tmpl, const std::string &wpname);

// One row resolved against gametext and formatted: the "Canned Msg" template
// for the row's key ("" -> nothing posts), a camp row's WPNames string, the
// STRCND48 bonus template when the local player earned the bonus, and the
// missing-actor fallback [orig: HUD_FormatKillEventMessage null-entity paths
// @0x422DDA/@0x422E91 -> GameText_GetString("Client", "STRCLI01") = "Unknown"].
std::string feed_row_line(const FeedRow &, const GameTextLookup &gametext);

// Retained involved-event line; expiry clears its clock but preserves the death-screen text.
// [orig: NetPacket_HandleGameEvent @ 0x427B8B; HUD_DrawKillAnnounceBanner @ 0x59DC90]
struct KillAnnouncement {
	std::string text;
	uint32_t tick = 0;
	void record(const std::string &line, uint32_t now);
	bool visible(uint32_t now) const;
	void expire(uint32_t now);
};

// Strip retail's inline text markup (`<cRRGGBB>` colour, `<b>` bold — every
// `<...>` run) from a string: the byte walk that drops each '<'..'>' span and
// keeps an unterminated '<' tail as-is [orig: Chat_StripHtmlTags @0x4983f0 —
// the chat senders sanitize with it; a compiled list that cannot draw the
// markup shows the same stripped text].
std::string strip_inline_tags(const std::string &text);

} // namespace opennova::hud
