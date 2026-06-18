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
		{'S', 0x0F, "world-state-load",        MsgCoverage::PrinterOnly, "§5.8 ~624B, fragmented"},
		{'S', 0x10, "static-entity-batch",     MsgCoverage::Decoded,     "§5.9 decode_static_entity_batch"},
		{'S', 0x16, "player-list",             MsgCoverage::Decoded,     "§5.20 decode_player_list"},
		{'S', 0x1A, "wait-for-game-start-ack", MsgCoverage::PrinterOnly, "game-start gate"},
		{'S', 0x1D, "spawn-success-gate",      MsgCoverage::PrinterOnly, "§5.2"},
		{'S', 0x1E, "game-event",              MsgCoverage::Decoded,     "§5.26 decode_game_event"},
		{'S', 0x20, "pool3-sync",              MsgCoverage::Decoded,     "§5.12 decode_pool3_sync_batch"},
		{'S', 0x26, "kill-sync",               MsgCoverage::Decoded,     "§5.26 decode_kill_record"},
		{'S', 0x40, "capture-zone-state",      MsgCoverage::Decoded,     "§5.19 decode_capture_zone_overlay"},
		{'S', 0x45, "terrain-load",            MsgCoverage::PrinterOnly, "terrain stream"},
		{'S', 0x46, "player-sync",             MsgCoverage::Decoded,     "§5.21 decode_player_sync"},
		{'S', 0x4E, "kill-by-slot",            MsgCoverage::Decoded,     "§5.26 decode_batch_kill"},
		{'S', 0x57, "rtt-echo",                MsgCoverage::PrinterOnly, "latency probe"},
		{'S', 0x5A, "weapon-loadout",          MsgCoverage::PrinterOnly, "§4 resets dword_81474C"},
		{'S', 0x60, "mission-announce",        MsgCoverage::PrinterOnly, "§5.28 SERVERNAME/MISSIONNAME"},
		{'S', 0x61, "session-key",             MsgCoverage::PrinterOnly, "SCRK exchange"},
		{'S', 0x64, "mission-chunk",           MsgCoverage::PrinterOnly, "§5.28 streamed; payload codec deferred"},
		{'S', 0x6E, "roster-sync",             MsgCoverage::Unhandled,   "team/squad roster sync (0x429880) — not yet field-mapped"},
		{'S', 0x6F, "cinematic-camera",        MsgCoverage::PrinterOnly, "cutscene camera"},
		{'S', 0x7B, "full-player-info",        MsgCoverage::PrinterOnly, "extended player record"},
		// ---- C2S (client -> server) ----
		{'C', 0x00, "JOIN",                    MsgCoverage::PrinterOnly, "join request"},
		{'C', 0x06, "fired-round",             MsgCoverage::Decoded,     "§5.16 decode_client_fired_round"},
		{'C', 0x0C, "entity-uplink",           MsgCoverage::Decoded,     "§5.10 decode_player_extended_uplink"},
		{'C', 0x0D, "replication-ack",         MsgCoverage::PrinterOnly, "spawn ack"},
		{'C', 0x0F, "spawn-query",             MsgCoverage::PrinterOnly, "§5.9 len-2 spawn-point query"},
		{'C', 0x16, "chat",                    MsgCoverage::PrinterOnly, "text chat"},
		{'C', 0x21, "checksum-reply",          MsgCoverage::Decoded,     "§5.17 decode_client_checksum_reply"},
		{'C', 0x22, "burst",                   MsgCoverage::PrinterOnly, "burst ack"},
		{'C', 0x23, "burst",                   MsgCoverage::PrinterOnly, "burst ack"},
		{'C', 0x28, "burst",                   MsgCoverage::PrinterOnly, "batch-kill ack"},
		{'C', 0x29, "burst",                   MsgCoverage::PrinterOnly, "spawn-confirm"},
		{'C', 0x2C, "rtt-consumed",            MsgCoverage::PrinterOnly, "latency reply"},
		{'C', 0x47, "ping",                    MsgCoverage::PrinterOnly, "observed len=0"},
		{'C', 0x48, "client-ack",              MsgCoverage::PrinterOnly, "observed 4B"},
		{'C', 0x4C, "client-state-byte",       MsgCoverage::PrinterOnly, "observed 1B =0x01"},
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
