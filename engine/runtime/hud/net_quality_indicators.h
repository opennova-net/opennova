#pragma once

// THE CONNECTION INDICATORS: the three small icons a multiplayer HUD draws in
// its top-left corner, and the per-frame state behind them. Retail keeps both
// in one CNetQuality object, g_NetQuality @0x82BF88: the quality LEVEL the
// 62-frame metrics fold stores (the byte the C2S 0x4C report carries), three
// ramped level channels, the T/R link-error flags and the NovaWorld link fade.
//
//   neticon2.tga, 12x12 at (4, 4): the connection quality. Level 2 fades in
//     its yellow band, level 3 its red band; level 1 (good) draws nothing.
//   neticon1.tga, 24x12 at (20, 4): the T/R pair. A peer asking us for a
//     resend lights T (outgoing), our own missing-sequence request or a
//     3-second receive silence lights R (incoming); it blinks for 93 frames,
//     then fades out green.
//   neticon3.tga, 8x8 at (52, 4): the NovaWorld N, while the NovaWorld UDP
//     session is in use: it blinks red/green while that session is not
//     hosting-or-playing ready and fades out once it is.
//
// [orig: CNetQuality_SetLevel @0x4c3060; CNetQuality_UpdateIndicators
//  @0x4c3090; CNetQuality_DrawIndicators @0x4c3200; CNetQuality_SetLinkErrorFlag
//  (ex SetCheatFlag) @0x4c34f0; CNetQuality_Reset @0x4c58c0; the static
//  construction @0x793880 (two inlined resets)]. The positions (+0x40..+0x54)
// are the hudpos layout's (HudLayout::net_indicator_pos); the draw is
// HudFrameCompiler::emit_net_quality_indicators. Net-agnostic: the embedder
// (inmatch::ClientRuntime) feeds the level, the flags and the NovaWorld facts.

#include <cstdint>

namespace opennova::hud {

// The display half of g_NetQuality, dword by dword.
struct NetQualityIndicators {
	int32_t level = 0;              // +0x00 0..4 (CNetQuality_SetLevel's clamp)
	bool level_changed = false;     // +0x04 byte: raised on a changed level; no reader
	int32_t level1_alpha = 0;       // +0x08 the level-1 channel (ramped, never drawn)
	int32_t level2_alpha = 0;       // +0x0C the level-2 channel: neticon2 band 1 (yellow)
	int32_t level3_alpha = 0;       // +0x10 the level-3 channel: neticon2 band 2 (red)
	int32_t ramp_target = 255;      // +0x14 the ramp ceiling, rewritten 255 every frame
	// +0x18 is written 4 by the reset and read by nothing.
	uint32_t link_error_bits = 0;   // +0x1C bit 0 = T (outgoing), bit 1 = R (incoming)
	int32_t link_error_countdown = 0; // +0x20 155 frames per raised flag
	int32_t link_error_alpha = 0;   // +0x24 neticon1's alpha
	int32_t link_error_blink_timer = 0; // +0x28 reload 10
	int32_t link_error_blink = 0;   // +0x2C 1 shows band 0 (both green)
	uint32_t flag_cooldown_until_ms = 0; // +0x30 flags 1..3 wait for the clock to reach it
	int32_t novaworld_alpha = 0;    // +0x34 neticon3's alpha
	int32_t novaworld_blink_timer = 0; // +0x38 reload 10
	int32_t novaworld_blink = 0;    // +0x3C 1 shows band 1 (red)
};

// The link-error flag types [orig: CNetQuality_SetLinkErrorFlag @0x4c34f0].
inline constexpr int kNetLinkErrorOutgoing = 1; // the peer requested a resend
inline constexpr int kNetLinkErrorIncoming = 2; // a missing-sequence request / receive silence
inline constexpr int kNetLinkErrorClear = 4;    // S2C 0x0F on a joiner: clear + 10 s cooldown
inline constexpr int32_t kNetLinkErrorFrames = 155;       // [orig: @0x4c351d]
inline constexpr uint32_t kNetLinkErrorCooldownMs = 10000; // [orig: @0x4c353a]
// neticon1 shows the error band only while at least this many frames remain
// [orig: `cmp [esi+20h], 3Eh; jge` @0x4c3380].
inline constexpr int32_t kNetLinkErrorBandFrames = 62;

// What the NovaWorld N icon reads besides the object: the network type, the
// NovaWorld UDP (NWU) session being in use, and that session's state flags.
// [orig: g_NapiNPCtx.transport_mode == NetworkType_NovaWorld (1);
//  dword_B5FD2C = `dword_B5F928 > 0 || dword_B4C71C > 0` (gate-response or
//  command-line NWU gate addresses; CNapiGateManager_ProcessResponse
//  @0x4cf525..0x4cf541); byte_B60100 = the NWU game session's flags
//  (session+0x120), whose bits 2 and 8 CGameSession_SetState @0x4ce140 sets
//  together in its states 4..8 (verified, hosting, playing)]
struct NovaWorldLinkFacts {
	bool novaworld = false;         // transport_mode == NovaWorld
	bool nwu_in_use = false;        // dword_B5FD2C
	uint8_t nwu_session_flags = 0;  // byte_B60100
	// The session's hosting/playing word (session+0x128, dword_B60108): the
	// main frame's NovaWorld exit reads it (inmatch/novaworld_link.h).
	int32_t nwu_session_role = 0;
};

// Both bits 2 and 8 of the NWU session flags: the session is in state 4..8
// [orig: `test al, 2` / `test al, 8` @0x4c3185..0x4c318b].
inline bool nwu_session_ready(uint8_t flags) { return (flags & 2u) != 0 && (flags & 8u) != 0; }

// The reset: every dword to its start value and the flag cooldown to `now_ms`
// (open at once) [orig: CNetQuality_Reset @0x4c58c0 — also run by the mission
// start @0x5243b4, the session create @0x4c9cfc and the client connection
// @0x4ca3f7; the positions it seeds are the layout's defaults].
void net_quality_reset(NetQualityIndicators &q, uint32_t now_ms);

// Clamp to 0..4, store, raise the change byte on a change [orig:
// CNetQuality_SetLevel @0x4c3060]. Returns the stored level.
int32_t net_quality_set_level(NetQualityIndicators &q, int32_t level);

// One link-error flag at the clock `now_ms` [orig: CNetQuality_SetLinkErrorFlag
// @0x4c34f0]: 1..3 OR into the bits and restart the 155-frame countdown once
// the clock has reached the cooldown; 4 clears the bits, the countdown and the
// alpha and arms the cooldown 10 s out; any other value does nothing.
void net_quality_set_flag(NetQualityIndicators &q, int flag_type, uint32_t now_ms);

// One frame of the indicators, run every main frame while in a session
// [orig: CNetQuality_UpdateIndicators @0x4c3090, called @0x52668d under
// `is_in_session` @0x526686].
void net_quality_update_indicators(NetQualityIndicators &q, const NovaWorldLinkFacts &nw);

// What the HUD compile reads: the session gate, the N icon's draw gate and the
// object itself.
struct HudNetQualityState {
	// g_NapiNPCtx.is_in_session [orig: @0x4c3210]
	bool in_session = false;
	// transport_mode == NovaWorld && dword_B5FD2C [orig: @0x4c33d0..0x4c33f0]
	bool novaworld_icon = false;
	NetQualityIndicators indicators;
};

} // namespace opennova::hud
