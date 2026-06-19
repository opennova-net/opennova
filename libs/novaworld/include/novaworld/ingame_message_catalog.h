#ifndef OPENNOVA_NOVAWORLD_INGAME_MESSAGE_CATALOG_H
#define OPENNOVA_NOVAWORLD_INGAME_MESSAGE_CATALOG_H

#include <cstddef>
#include <cstdint>

// Canonical catalog of in-game NAPI message tags (mirrors the §4 dispatch tables
// + §5.x field maps in docs/net/novaworld-net-re.md). Single source of truth for
// nw_pp's human labels AND the nw_message_coverage CI gate, so a tag can't be
// handled in one place and silently forgotten in the other. Header-only (the data
// is a function-local static) so the app and the test share one definition.
//
// Coverage classes:
//   Decoded     — a structured decode_* exists (`note` names it) and nw_pp prints
//                 its fields; the coverage test asserts the decoder consumes a
//                 representative body to the byte.
//   PrinterOnly — nw_pp labels + hex-dumps it; no structured decoder yet.
//   Unhandled   — a known tag not yet characterized (the grill tail); `note` is the
//                 reason. Carried here so new work surfaces it instead of guessing.

namespace opennova {

enum class MsgCoverage : uint8_t {
	Decoded,
	PrinterOnly,
	Unhandled,
};

struct MsgCatalogEntry {
	char        dir;   // 'S' = server->client, 'C' = client->server
	uint8_t     tag;   // NAPI message id
	const char *name;  // short label used by nw_pp output
	MsgCoverage coverage;
	const char *note;  // Decoded: "§5.x decode_*"; PrinterOnly: section ref; Unhandled: reason
};

// The catalog table + its length (via out-param). The Decoded `note`s name the
// decoder in libs/novaworld/include/novaworld/ingame_decode.h.
inline const MsgCatalogEntry *ingame_message_catalog(size_t *count) {
	static const MsgCatalogEntry table[] = {
		// ---- S2C (server -> client) ----
		{'S', 0x00, "init",                    MsgCoverage::PrinterOnly, "session init"},
		{'S', 0x02, "post-handshake",          MsgCoverage::PrinterOnly, "§5.2 burst"},
		{'S', 0x0A, "per-frame-update",        MsgCoverage::Decoded,     "§5.9 decode_frame_update"},
		{'S', 0x0B, "BMS-header",              MsgCoverage::PrinterOnly, "§5.4 616-byte header"},
		{'S', 0x0C, "entity-spawn-batch",      MsgCoverage::Decoded,     "§5.23 decode_organic_spawn_batch"},
		{'S', 0x0D, "pool-spawn",              MsgCoverage::Decoded,     "§5.11 decode_pool_spawn_batch"},
		{'S', 0x0F, "world-state-load",        MsgCoverage::Decoded,     "§5.29 decode_world_state_load"},
		{'S', 0x10, "static-entity-batch",     MsgCoverage::Decoded,     "§5.9 decode_static_entity_batch"},
		{'S', 0x16, "player-list",             MsgCoverage::Decoded,     "§5.20 decode_player_list"},
		{'S', 0x1A, "wait-for-game-start-ack", MsgCoverage::PrinterOnly, "game-start gate"},
		{'S', 0x1D, "spawn-success-gate",      MsgCoverage::PrinterOnly, "§5.2"},
		{'S', 0x1E, "game-event",              MsgCoverage::Decoded,     "§5.26 decode_game_event"},
		{'S', 0x20, "pool3-sync",              MsgCoverage::Decoded,     "§5.12 decode_pool3_sync_batch"},
		{'S', 0x39, "anim-crc-challenge",      MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (seed -> C2S 0x1C)"},
		{'S', 0x43, "time-sync-ping",          MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (serverTs -> C2S 0x08)"},
		{'S', 0x26, "kill-sync",               MsgCoverage::Decoded,     "§5.26 decode_kill_record"},
		{'S', 0x40, "capture-zone-state",      MsgCoverage::Decoded,     "§5.19 decode_capture_zone_overlay"},
		{'S', 0x45, "terrain-load",            MsgCoverage::PrinterOnly, "terrain stream"},
		{'S', 0x46, "player-sync",             MsgCoverage::Decoded,     "§5.21 decode_player_sync"},
		{'S', 0x4E, "kill-by-slot",            MsgCoverage::Decoded,     "§5.26 decode_batch_kill"},
		{'S', 0x57, "rtt-echo",                MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'S', 0x5A, "weapon-loadout",          MsgCoverage::Decoded,     "§5.30 decode_weapon_loadout"},
		{'S', 0x68, "entity-index-request",    MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (startIdx -> C2S 0x3D)"},
		{'S', 0x60, "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', 0x61, "session-key",             MsgCoverage::PrinterOnly, "SCRK exchange"},
		{'S', 0x64, "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', 0x6E, "roster-sync",             MsgCoverage::Decoded,     "§5.31 decode_roster_sync"},
		{'S', 0x6F, "cinematic-camera",        MsgCoverage::PrinterOnly, "cutscene camera"},
		{'S', 0x7B, "full-player-info",        MsgCoverage::Decoded,     "§5.32 decode_full_player_info"},
		// ---- C2S (client -> server) ----
		{'C', 0x00, "JOIN",                    MsgCoverage::PrinterOnly, "join request"},
		{'C', 0x06, "fired-round",             MsgCoverage::Decoded,     "§5.16 decode_client_fired_round"},
		{'C', 0x08, "time-sync-reply",         MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x43"},
		{'C', 0x0C, "entity-uplink",           MsgCoverage::Decoded,     "§5.10 decode_player_extended_uplink"},
		{'C', 0x0D, "replication-ack",         MsgCoverage::PrinterOnly, "spawn ack"},
		{'C', 0x0F, "spawn-query",             MsgCoverage::PrinterOnly, "§5.9 len-2 spawn-point query"},
		{'C', 0x16, "chat",                    MsgCoverage::PrinterOnly, "text chat"},
		{'C', 0x1C, "anim-crc-reply",          MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x39"},
		{'C', 0x21, "checksum-reply",          MsgCoverage::Decoded,     "§5.17 decode_client_checksum_reply"},
		{'C', 0x22, "player-sync-request",     MsgCoverage::Decoded,     "§5.33 decode_burst_player_sync_request"},
		{'C', 0x23, "visible-players-request", MsgCoverage::Decoded,     "§5.33 decode_burst_visible_request"},
		{'C', 0x28, "loadout-request",         MsgCoverage::Decoded,     "§5.33 decode_burst_loadout_request"},
		{'C', 0x29, "entity-request",          MsgCoverage::Decoded,     "§5.33 decode_burst_entity_request"},
		{'C', 0x2C, "rtt-consumed",            MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'C', 0x3D, "entity-index-reply",      MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x68"},
		{'C', 0x47, "ping",                    MsgCoverage::PrinterOnly, "observed len=0"},
		{'C', 0x48, "client-ack",              MsgCoverage::PrinterOnly, "observed 4B"},
		{'C', 0x4C, "client-quality",          MsgCoverage::Decoded,     "§5.33 decode_burst_client_quality"},
	};
	if (count) *count = sizeof(table) / sizeof(table[0]);
	return table;
}

// Look up an entry by direction + tag, or nullptr if the tag isn't catalogued.
inline const MsgCatalogEntry *lookup_ingame_message(char dir, uint8_t tag) {
	size_t n = 0;
	const MsgCatalogEntry *t = ingame_message_catalog(&n);
	for (size_t i = 0; i < n; ++i)
		if (t[i].dir == dir && t[i].tag == tag)
			return &t[i];
	return nullptr;
}

// Short label for nw_pp output, or nullptr for an uncatalogued tag.
inline const char *ingame_message_name(char dir, uint8_t tag) {
	const MsgCatalogEntry *e = lookup_ingame_message(dir, tag);
	return e ? e->name : nullptr;
}

} // namespace opennova

#endif // OPENNOVA_NOVAWORLD_INGAME_MESSAGE_CATALOG_H
