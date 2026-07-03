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
		{'S', 0x00, "init",                    MsgCoverage::PrinterOnly, "session-layer init (dispatch stub @0x42E0E0)"},
		{'S', 0x01, "sync-state",              MsgCoverage::PrinterOnly, "u32 sync state @0x425360"},
		{'S', 0x02, "join-padding-probe",      MsgCoverage::Decoded,     "§5.55 decode_join_padding_probe (pos ack; client echoes C2S 0x02 + N random bytes)"},
		{'S', 0x03, "sync-tick",               MsgCoverage::PrinterOnly, "u8 + 2×u16 sync tick @0x425390"},
		{'S', 0x04, "session-slot-config",     MsgCoverage::Decoded,     "§5.53 decode_session_slot_config (maxPlayers → slot-table realloc)"},
		{'S', 0x05, "game-start-signal",       MsgCoverage::PrinterOnly, "u8 flag; nonzero → C2S 0x4E + timer reset @0x42E180"},
		{'S', 0x08, "session-config",          MsgCoverage::Decoded,     "§5.54 decode_session_config (51 B; gameType + flag bits)"},
		{'S', 0x0A, "per-frame-update",        MsgCoverage::Decoded,     "§5.9 decode_frame_update"},
		{'S', 0x0B, "BMS-header",              MsgCoverage::PrinterOnly, "§5.4 616-byte header"},
		{'S', 0x0C, "entity-spawn-batch",      MsgCoverage::Decoded,     "§5.23 decode_organic_spawn_batch"},
		{'S', 0x0D, "pool-spawn",              MsgCoverage::Decoded,     "§5.11 decode_pool_spawn_batch"},
		{'S', 0x0F, "world-state-load",        MsgCoverage::Decoded,     "§5.29 decode_world_state_load"},
		{'S', 0x10, "static-entity-batch",     MsgCoverage::Decoded,     "§5.9 decode_static_entity_batch"},
		{'S', 0x11, "disconnect-unlock",       MsgCoverage::PrinterOnly, "len 0; dword_A82358=1 (WaitForDisconnect) @0x4226E0"},
		{'S', 0x13, "entity-death",            MsgCoverage::Decoded,     "§5.35 decode_entity_death"},
		{'S', 0x14, "chat-broadcast",          MsgCoverage::Decoded,     "§5.52 decode_chat_broadcast (fan-out of C2S 0x0D)"},
		{'S', 0x16, "player-list",             MsgCoverage::Decoded,     "§5.20 decode_player_list"},
		{'S', 0x18, "full-entity-spawn",       MsgCoverage::Decoded,     "§5.46 decode_full_entity_spawn (0x0F reply + 0x40 vehicle-spawn broadcast)"},
		{'S', 0x19, "spawn-ack-timestamp",     MsgCoverage::Decoded,     "§5.34-style decode_u32_scalar (ts ack of C2S 0x0A → dword_A82360)"},
		{'S', 0x1A, "wait-for-game-start-ack", MsgCoverage::PrinterOnly, "game-start gate"},
		{'S', 0x1C, "noop",                    MsgCoverage::PrinterOnly, "empty stub @0x4227F0"},
		{'S', 0x1D, "spawn-success-gate",      MsgCoverage::PrinterOnly, "§5.2"},
		{'S', 0x1E, "game-event",              MsgCoverage::Decoded,     "§5.26 decode_game_event"},
		{'S', 0x20, "pool3-sync",              MsgCoverage::Decoded,     "§5.12 decode_pool3_sync_batch"},
		{'S', 0x26, "kill-sync",               MsgCoverage::Decoded,     "§5.26 decode_kill_record"},
		{'S', 0x29, "char-minimap-update",     MsgCoverage::PrinterOnly, "[u8 pool0Idx][u8 team][u8 flags7][u16 packedCharId] → NetId + CharacterEntity rebind @0x427D00 (§5.59)"},
		{'S', 0x2A, "chat-history",            MsgCoverage::Decoded,     "§5.35 decode_chat_history_entry"},
		{'S', 0x2C, "mission-map-names",       MsgCoverage::Decoded,     "§5.51 decode_mission_map_names (join burst)"},
		{'S', 0x30, "entity-checksum-req",     MsgCoverage::Decoded,     "§5.35 decode_entity_checksum_request (-> C2S 0x20)"},
		{'S', 0x31, "loadout-crc-req",         MsgCoverage::PrinterOnly, "3 B CRC request → C2S 0x21 @0x4311E0"},
		{'S', 0x34, "play-sound",              MsgCoverage::Decoded,     "§5.50 decode_play_sound (profile name + optional 3D pos)"},
		{'S', 0x39, "anim-crc-challenge",      MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (seed -> C2S 0x1C)"},
		{'S', 0x3E, "ack-stub",                MsgCoverage::PrinterOnly, "ack-style stub @0x4226D0"},
		{'S', 0x40, "capture-zone-state",      MsgCoverage::Decoded,     "§5.19 decode_capture_zone_overlay"},
		{'S', 0x42, "input-state-flags",       MsgCoverage::Decoded,     "§5.35 decode_input_state_flags"},
		{'S', 0x43, "time-sync-ping",          MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (serverTs -> C2S 0x08)"},
		{'S', 0x44, "entity-routed",           MsgCoverage::PrinterOnly, "§5.36 decode_entity_routed_packet (sub-header; class body partial)"},
		{'S', 0x45, "terrain-load",            MsgCoverage::Decoded,     "§5.37 decode_terrain_load_batch"},
		{'S', 0x46, "player-sync",             MsgCoverage::Decoded,     "§5.21 decode_player_sync"},
		{'S', 0x49, "weapon-reload",           MsgCoverage::Decoded,     "§5.35 decode_weapon_reload"},
		{'S', 0x4C, "target-assignment",       MsgCoverage::PrinterOnly, "squad/AI order list @0x428570"},
		{'S', 0x4D, "spawn-slot-tip",          MsgCoverage::PrinterOnly, "u8 slot; local → tip, else C2S 0x22+0x23 @0x4317B0"},
		{'S', 0x4E, "kill-by-slot",            MsgCoverage::Decoded,     "§5.26 decode_batch_kill"},
		{'S', 0x50, "team-assign",             MsgCoverage::PrinterOnly, "[u16 handle][u8 team][u16 spawnPoint][u8 squadLeader] @0x431910"},
		{'S', 0x51, "team-change-confirm",     MsgCoverage::PrinterOnly, "write_entity_packet @0x506BB0 for g_team_change_entity_list[idx] (@0x514F10, team-change only); client FIELD-PARSES + rebinds CharacterEntity @0x431BB0 — never sent on a plain join (D-NET-148, §5.59)"},
		{'S', 0x53, "zone-timer-window",       MsgCoverage::Decoded,     "§5.49 decode_zone_timer_window (capture/takeover HUD)"},
		{'S', 0x57, "rtt-echo",                MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'S', 0x58, "session-status",          MsgCoverage::Decoded,     "§5.48 decode_session_status (server/mission names + score rules)"},
		{'S', 0x59, "deployed-item",           MsgCoverage::Decoded,     "§5.36 decode_deployed_item_spawn"},
		{'S', 0x5A, "weapon-loadout",          MsgCoverage::Decoded,     "§5.30 decode_weapon_loadout"},
		{'S', 0x5D, "destroy-list",            MsgCoverage::PrinterOnly, "clean despawn [i16 slot]xN (D-NET-80)"},
		{'S', 0x60, "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', 0x61, "session-key",             MsgCoverage::PrinterOnly, "SCRK exchange"},
		{'S', 0x64, "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', 0x66, "weapon-restrictions",     MsgCoverage::PrinterOnly, "count + (slot,restriction) pairs @0x42D4C0"},
		{'S', 0x68, "entity-index-request",    MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (startIdx -> C2S 0x3D)"},
		{'S', 0x6B, "minimap-overlay",         MsgCoverage::Decoded,     "§5.35 decode_minimap_overlay_batch"},
		{'S', 0x6E, "roster-sync",             MsgCoverage::Decoded,     "§5.31 decode_roster_sync"},
		{'S', 0x6F, "zone-timer-value",        MsgCoverage::Decoded,     "§5.49 decode_zone_timer_value (NOT cinematic camera)"},
		{'S', 0x75, "spectator-flags",         MsgCoverage::PrinterOnly, "2 B → g_death_screen_active/dword_24D1DF4 @0x4259E0"},
		{'S', 0x76, "u16-var",                 MsgCoverage::PrinterOnly, "u16 → dword_24D59FC @0x42D540"},
		{'S', 0x79, "spectator-flag",          MsgCoverage::Decoded,     "§5.35 decode_spectator_flag"},
		{'S', 0x7A, "player-name",             MsgCoverage::PrinterOnly, "player name (≤64) → server-info struct @0x429B40"},
		{'S', 0x7B, "full-player-info",        MsgCoverage::Decoded,     "§5.32 decode_full_player_info"},
		{'S', 0x7E, "server-config-strings",   MsgCoverage::PrinterOnly, "two cstrings → byte_A86520/byte_A86120 @0x425E20"},
		{'S', 0x81, "score-delta-sound",       MsgCoverage::PrinterOnly, "[i32 score]; positive delta plays tiered hit-confirm sound @0x42A0B0"},
		// ---- C2S (client -> server) ----
		{'C', 0x00, "JOIN",                    MsgCoverage::PrinterOnly, "join request"},
		{'C', 0x01, "join-form-post",          MsgCoverage::PrinterOnly, "§6.4 early-join side-password compare @0x512ED0"},
		{'C', 0x02, "join-padding-echo",       MsgCoverage::PrinterOnly, "§5.55 reply to S2C 0x02: position + N random filler bytes"},
		{'C', 0x03, "set-player-value",        MsgCoverage::PrinterOnly, "[i32] -> player entity+372 @0x501BE0"},
		{'C', 0x06, "fired-round",             MsgCoverage::Decoded,     "§5.16 decode_client_fired_round"},
		{'C', 0x08, "time-sync-reply",         MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x43"},
		{'C', 0x09, "checksum-response",       MsgCoverage::PrinterOnly, "client checksum response @0x513200"},
		{'C', 0x0A, "spawn-menu-request",      MsgCoverage::PrinterOnly, "len 0; game state 9 + S2C 0x19 ts ack @0x513260"},
		{'C', 0x0B, "mission-file-status",     MsgCoverage::PrinterOnly, "§5.4 mission-file status report @0x51AB10"},
		{'C', 0x0C, "entity-uplink",           MsgCoverage::Decoded,     "§5.10 decode_player_extended_uplink"},
		{'C', 0x0D, "chat-message",            MsgCoverage::Decoded,     "§5.52 decode_chat_uplink (the old 'replication-ack' label was wrong; -> S2C 0x14)"},
		{'C', 0x0E, "respawn-request",         MsgCoverage::PrinterOnly, "[i16 spawnHandle; 0xFFFE=auto team spawn] deploy request @0x519AF0"},
		{'C', 0x0F, "entity-info-query",       MsgCoverage::PrinterOnly, "§5.46 [u16 handle] self-heal -> S2C 0x18"},
		{'C', 0x16, "chat",                    MsgCoverage::PrinterOnly, "text chat"},
		{'C', 0x1C, "anim-crc-reply",          MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x39"},
		{'C', 0x20, "entity-checksum-reply",   MsgCoverage::PrinterOnly, "§5.35 reply to S2C 0x30"},
		{'C', 0x21, "checksum-reply",          MsgCoverage::Decoded,     "§5.17 decode_client_checksum_reply"},
		{'C', 0x22, "player-sync-request",     MsgCoverage::Decoded,     "§5.33 decode_burst_player_sync_request"},
		{'C', 0x23, "visible-players-request", MsgCoverage::Decoded,     "§5.33 decode_burst_visible_request"},
		{'C', 0x28, "loadout-request",         MsgCoverage::Decoded,     "§5.33 decode_burst_loadout_request"},
		{'C', 0x29, "team-spawn-ack",          MsgCoverage::Decoded,     "§5.59 decode_team_spawn_ack ([u16 team_change_index]; deploy/team ack — S2C 0x51 only for a pending team change, D-NET-148)"},
		{'C', 0x25, "weapon-reload-request",   MsgCoverage::Decoded,     "§5.58 decode_weapon_reload (same 4-B body as S2C 0x49; host broadcasts it back @0x514DF0; direction asymmetry vs S2C 0x25, §5.3)"},
		{'C', 0x26, "vehicle-attach-request",  MsgCoverage::PrinterOnly, "word0 overwritten with requester's own handle -> Entity_ProcessVehicleAttach @0x502390"},
		{'C', 0x27, "vehicle-detach-request",  MsgCoverage::PrinterOnly, "[u16 handle] -> Entity_DetachFromVehicle(entity, entity+364) @0x4FC980"},
		{'C', 0x2C, "rtt-consumed",            MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'C', 0x2D, "burst-member-2d",         MsgCoverage::PrinterOnly, "§5.33 burst receiver @0x502430"},
		{'C', 0x2F, "loadout-submit",          MsgCoverage::Decoded,     "§5.56 decode_loadout_submit (class/type + ADM slots -> S2C 0x5A)"},
		{'C', 0x32, "burst-member-32",         MsgCoverage::PrinterOnly, "§5.33 burst receiver @0x51A600"},
		{'C', 0x33, "file-chunk-request-60",   MsgCoverage::PrinterOnly, "§5.28 [u32 id][u32 nextOffset] re-request for S2C 0x60"},
		{'C', 0x37, "file-chunk-request-64",   MsgCoverage::PrinterOnly, "§5.28 [u32 id][u32 nextOffset] re-request for S2C 0x64"},
		{'C', 0x3D, "entity-index-reply",      MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x68"},
		{'C', 0x47, "ping",                    MsgCoverage::PrinterOnly, "re-broadcast request -> S2C 0x75 @0x510ED0"},
		{'C', 0x48, "client-ack",              MsgCoverage::PrinterOnly, "4 B read+discarded; server handler is an empty stub @0x510F30"},
		{'C', 0x4C, "client-quality",          MsgCoverage::Decoded,     "§5.33 decode_burst_client_quality"},
		{'C', 0x4E, "game-start-ack",          MsgCoverage::PrinterOnly, "4 B reply to S2C 0x05 game-start signal"},
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
