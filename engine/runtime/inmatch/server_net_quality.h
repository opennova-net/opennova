#pragma once

#include <cstdint>

#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The C2S 0x2C RETURN leg (echo flag clear): the host measures `now - sent`,
// zeroes it for its own local slot, pushes it through the eleven-dword ring
// (cursor aliasing included), and applies the min/max-ping strike policy —
// strictly over 20 consecutive violations punts the player with chat code
// 36 "minping" / 37 "maxping". `in_session` is g_NapiNPCtx.is_in_session;
// the local slot and a joiner whose DB join tag is nonzero (NetPlayer+216 =
// NapiNetConfig::db; stock clients send 0) are exempt from the policy.
// [orig: NapiNPServerMsg_HandlePingResponse @0x515070 — measure @0x515116,
//  local zero @0x515127, ring @0x515141..0x515155, policy @0x515171..0x51521A]
void Server_RecordPingSample(const GameConfig &config, NapiNPConnection &conn,
		uint32_t sent_ms, uint32_t now_ms, bool in_session);

// The mean of the ten ring samples (the send-window ping term's per-slot input).
// [orig: CNetQuality_UpdateMetrics @0x4C53FA — the +100368.. sum / 0xA]
uint32_t Server_PlayerAveragePingMs(const NapiNPConnection &conn);

// C2S 0x4C: clamp the reported level to 0..4, store it, and dirty the slot
// only when it changed. [orig: NapiNPServerMsg_0x04C @0x5111B0 -> sub_5006E0 @0x5006E0]
void Server_StoreClientQuality(NapiNPConnection &conn, uint8_t reported);

// The 1 Hz S2C 0x46 quality resend: inside the periodic block, gated
// in-session / no pre-round / no spawn gate, a persistent slot cursor walks
// the roster and re-sends field 0x0400 for every active, dirty, connected
// slot — at most eight per second, the cursor carrying over. Mask 128.
// [orig: Server_TickUpdate @0x51DE79..0x51DF4A; g_WeaponBroadcastSlotCursor]
void Server_EmitQualityResends(NapiNPServerCtx &ctx, const world::World &world);

// The host CNetQuality SEND window: every 62 frames while in session, sample
// frame-rate pressure (the main loop's FR counter, ctx.stats_avg_fps), the mean of
// every eligible slot's ping ring averaged over those slots, and the summed
// loss counters (unmodeled: 0), then publish the folded 0..255 quality as the
// S2C 0x79 byte. Eligible = an active, non-local slot whose control age has
// matured (slot+383 >= 1860). [orig: Game_ProcessMainFrame @0x52658B under the
//  dword_24D1DDC 62-frame countdown; CNetQuality_UpdateMetrics @0x4C52C0 send
//  window @0x4C531B..0x4C55DA; the 0x79 read of +0x0C @0x51E3C1]
void Server_SampleHostNetQuality(NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
