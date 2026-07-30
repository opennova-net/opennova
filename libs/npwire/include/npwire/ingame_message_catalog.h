#ifndef OPENNOVA_NOVAWORLD_INGAME_MESSAGE_CATALOG_H
#define OPENNOVA_NOVAWORLD_INGAME_MESSAGE_CATALOG_H

#include <cstddef>
#include <cstdint>

#include "npwire/ingame_message_id.h"

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
// decoder in libs/npwire/include/npwire/ingame_decode.h.
inline const MsgCatalogEntry *ingame_message_catalog(size_t *count) {
	static const MsgCatalogEntry table[] = {
		// ---- S2C (server -> client) ----
		{'S', s2c::INIT,                      "init",                    MsgCoverage::PrinterOnly, "session-layer init (dispatch stub @0x42E0E0)"},
		{'S', s2c::SYNC_STATE,                "sync-state",              MsgCoverage::PrinterOnly, "u32 sync state @0x425360"},
		{'S', s2c::JOIN_PADDING_PROBE,        "join-padding-probe",      MsgCoverage::Decoded,     "§5.55 decode_join_padding_probe (pos ack; client echoes C2S 0x02 + N random bytes)"},
		{'S', s2c::SYNC_TICK,                 "sync-tick",               MsgCoverage::PrinterOnly, "u8 + 2×u16 sync tick @0x425390"},
		{'S', s2c::SESSION_SLOT_CONFIG,       "session-slot-config",     MsgCoverage::Decoded,     "§5.53 decode_session_slot_config (maxPlayers → slot-table realloc)"},
		{'S', s2c::GAME_START_SIGNAL,         "game-start-signal",       MsgCoverage::PrinterOnly, "u8 flag; nonzero → C2S 0x4E + timer reset @0x42E180"},
		{'S', s2c::SESSION_CONFIG,            "session-config",          MsgCoverage::Decoded,     "§5.54 decode_session_config (51 B; gameType + flag bits)"},
		{'S', s2c::PER_FRAME_UPDATE,          "per-frame-update",        MsgCoverage::Decoded,     "§5.9 decode_frame_update"},
		{'S', s2c::BMS_HEADER,                "BMS-header",              MsgCoverage::PrinterOnly, "§5.4 616-byte header"},
		{'S', s2c::ENTITY_SPAWN_BATCH,        "entity-spawn-batch",      MsgCoverage::Decoded,     "§5.23 decode_organic_spawn_batch"},
		{'S', s2c::POOL_SPAWN,                "pool-spawn",              MsgCoverage::Decoded,     "§5.11 decode_pool_spawn_batch"},
		{'S', s2c::WORLD_STATE_LOAD,          "world-state-load",        MsgCoverage::Decoded,     "§5.29 decode_world_state_load"},
		{'S', s2c::STATIC_ENTITY_BATCH,       "static-entity-batch",     MsgCoverage::Decoded,     "§5.9 decode_static_entity_batch"},
		{'S', s2c::DISCONNECT_UNLOCK,         "disconnect-unlock",       MsgCoverage::PrinterOnly, "len 0; dword_A82358=1 (WaitForDisconnect) @0x4226E0"},
		{'S', s2c::ENTITY_DEATH,              "entity-death",            MsgCoverage::Decoded,     "§5.35 decode_entity_death"},
		{'S', s2c::CHAT_BROADCAST,            "chat-broadcast",          MsgCoverage::Decoded,     "§5.52 decode_chat_broadcast (fan-out of C2S 0x0D)"},
		{'S', s2c::PLAYER_LIST,               "player-list",             MsgCoverage::Decoded,     "§5.20 decode_player_list"},
		{'S', s2c::FULL_ENTITY_SPAWN,         "full-entity-spawn",       MsgCoverage::Decoded,     "§5.46 decode_full_entity_spawn (0x0F reply + 0x40 vehicle-spawn broadcast)"},
		{'S', s2c::SPAWN_ACK_TIMESTAMP,       "spawn-ack-timestamp",     MsgCoverage::Decoded,     "§5.34-style decode_u32_scalar (ts ack of C2S 0x0A → dword_A82360)"},
		{'S', s2c::WAIT_FOR_GAME_START_ACK,   "wait-for-game-start-ack", MsgCoverage::PrinterOnly, "game-start gate"},
		{'S', s2c::NOOP,                      "noop",                    MsgCoverage::PrinterOnly, "empty stub @0x4227F0"},
		{'S', s2c::SPAWN_SUCCESS_GATE,        "spawn-success-gate",      MsgCoverage::PrinterOnly, "§5.2"},
		{'S', s2c::GAME_EVENT,                "game-event",              MsgCoverage::Decoded,     "§5.26 decode_game_event"},
		{'S', s2c::POOL3_SYNC,                "pool3-sync",              MsgCoverage::Decoded,     "§5.12 decode_pool3_sync_batch"},
		{'S', s2c::KILL_SYNC,                 "kill-sync",               MsgCoverage::Decoded,     "§5.26 decode_kill_record"},
		{'S', s2c::CHAR_MINIMAP_UPDATE,       "char-minimap-update",     MsgCoverage::PrinterOnly, "[u8 pool0Idx][u8 team][u8 flags7][u16 packedCharId] → NetId + CharacterEntity rebind @0x427D00 (§5.59)"},
		{'S', s2c::CHAT_HISTORY,              "chat-history",            MsgCoverage::Decoded,     "§5.35 decode_chat_history_entry"},
		{'S', s2c::MISSION_MAP_NAMES,         "mission-map-names",       MsgCoverage::Decoded,     "§5.51 decode_mission_map_names (join burst)"},
		{'S', s2c::ENTITY_CHECKSUM_REQ,       "entity-checksum-req",     MsgCoverage::Decoded,     "§5.35 decode_entity_checksum_request (-> C2S 0x20)"},
		{'S', s2c::LOADOUT_CRC_REQ,           "loadout-crc-req",         MsgCoverage::Decoded,     "§5.35 decode_loadout_crc_request (3 B ammo-def CRC request → C2S 0x21 @0x4311E0)"},
		{'S', s2c::PLAY_SOUND,                "play-sound",              MsgCoverage::Decoded,     "§5.50 decode_play_sound (profile name + optional 3D pos)"},
		{'S', s2c::CHARATTR_CRC_CHALLENGE,    "charattr-crc-challenge",  MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (seed -> C2S 0x1C)"},
		{'S', s2c::ACK_STUB,                  "ack-stub",                MsgCoverage::PrinterOnly, "ack-style stub @0x4226D0"},
		{'S', s2c::CAPTURE_ZONE_STATE,        "capture-zone-state",      MsgCoverage::Decoded,     "§5.19 decode_capture_zone_overlay"},
		{'S', s2c::CHARATTR_PROPERTY_CLEAR,   "charattr-property-clear", MsgCoverage::PrinterOnly, "[u8 propertyId] clears that property across the retained g_CharAttr[16] table (short body -> property 0); IDB NapiNPClientMsg_ClearAnimSlot @0x4254C0 is a MISNOMER -> CharAttr_SetSlotProperty @0x412890"},
		{'S', s2c::INPUT_STATE_FLAGS,         "input-state-flags",       MsgCoverage::Decoded,     "§5.35 decode_input_state_flags"},
		{'S', s2c::TIME_SYNC_PING,            "time-sync-ping",          MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (serverTs -> C2S 0x08)"},
		{'S', s2c::ENTITY_ROUTED,             "entity-routed",           MsgCoverage::PrinterOnly, "§5.36 decode_entity_routed_packet (sub-header; class body partial)"},
		{'S', s2c::TERRAIN_LOAD,              "terrain-load",            MsgCoverage::Decoded,     "§5.37 decode_terrain_load_batch"},
		{'S', s2c::PLAYER_SYNC,               "player-sync",             MsgCoverage::Decoded,     "§5.21 decode_player_sync"},
		{'S', s2c::WEAPON_RELOAD,             "weapon-reload",           MsgCoverage::Decoded,     "§5.35 decode_weapon_reload"},
		{'S', s2c::TARGET_ASSIGNMENT,         "target-assignment",       MsgCoverage::PrinterOnly, "squad/AI order list @0x428570"},
		{'S', s2c::SPAWN_SLOT_TIP,            "spawn-slot-tip",          MsgCoverage::PrinterOnly, "u8 slot; local → tip, else C2S 0x22+0x23 @0x4317B0"},
		{'S', s2c::KILL_BY_SLOT,              "kill-by-slot",            MsgCoverage::Decoded,     "§5.26 decode_batch_kill"},
		{'S', s2c::TEAM_ASSIGN,               "team-assign",             MsgCoverage::Decoded,     "decode_team_assign — [u16 handle][u8 team][u16 netId][u8 animSlot] (the identity pair, zeroed for non-players by the Flags & 0x100 gate @0x506b3d); NapiNPClientMsg_0x050 @0x431910 latches byte_A85B48 for self, writes entity Team otherwise, and re-sends ONE C2S 0x2F with slot 195 raw"},
		{'S', s2c::TEAM_CHANGE_CONFIRM,       "team-change-confirm",     MsgCoverage::PrinterOnly, "write_entity_packet @0x506BB0 for g_team_change_entity_list[idx] (@0x514F10, team-change only); client FIELD-PARSES + rebinds CharacterEntity @0x431BB0 — never sent on a plain join (D-NET-148, §5.59)"},
		{'S', s2c::ZONE_TIMER_WINDOW,         "zone-timer-window",       MsgCoverage::Decoded,     "§5.49 decode_zone_timer_window (capture/takeover HUD)"},
		{'S', s2c::RTT_ECHO,                  "rtt-echo",                MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'S', s2c::SESSION_STATUS,            "session-status",          MsgCoverage::Decoded,     "§5.48 decode_session_status (server/mission names + score rules)"},
		{'S', s2c::DEPLOYED_ITEM,             "deployed-item",           MsgCoverage::Decoded,     "§5.36 decode_deployed_item_spawn"},
		{'S', s2c::WEAPON_LOADOUT,            "weapon-loadout",          MsgCoverage::Decoded,     "§5.30 decode_weapon_loadout"},
		{'S', s2c::EMPTY_SLOT_SWEEP,          "empty-slot-sweep",        MsgCoverage::Decoded,     "decode_destroy_entity_list — [i16 pool0Index]xN RAW pool-0 indices; NapiNPClientMsg_DestroyEntityList @0x429730 (!is_authority) destroys each + PlayerSlot_ClearAndUnlink @0x434730. Reply to C2S 0x32 (D-NET-176)"},
		{'S', s2c::FILE_TRANSFER_CHUNK,       "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', s2c::TICK_SEED,                 "tick-seed",               MsgCoverage::Decoded,     "decode_tick_seed — per-player clock anchor; the host's fire-freshness floor @0x4297c0/@0x5101a0"},
		{'S', s2c::MISSION_DATA_CHUNK,        "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', s2c::WEAPON_RESTRICTIONS,       "weapon-restrictions",     MsgCoverage::PrinterOnly, "count + (slot,restriction) pairs @0x42D4C0"},
		{'S', s2c::LOADED_MODEL_PAGE_REQUEST, "loaded-model-page-request", MsgCoverage::Decoded,   "§5.34 decode_u32_scalar (cursor pages frozen loaded-model rows -> C2S 0x3D)"},
		{'S', s2c::MINIMAP_OVERLAY,           "minimap-overlay",         MsgCoverage::Decoded,     "§5.35 decode_minimap_overlay_batch"},
		{'S', s2c::ROSTER_SYNC,               "roster-sync",             MsgCoverage::Decoded,     "§5.31 decode_roster_sync"},
		{'S', s2c::ZONE_TIMER_VALUE,          "zone-timer-value",        MsgCoverage::Decoded,     "§5.49 decode_zone_timer_value (NOT cinematic camera)"},
		{'S', s2c::SPECTATOR_FLAGS,           "spectator-flags",         MsgCoverage::PrinterOnly, "2 B → g_death_screen_active/dword_24D1DF4 @0x4259E0"},
		{'S', s2c::SERVER_TICK16,             "server-tick16",           MsgCoverage::PrinterOnly, "u16 → dword_24D59FC @0x42D540 [orig sender: NetPacket_WriteServerTick16 @0x510350]"},
		{'S', s2c::SPECTATOR_FLAG,            "spectator-flag",          MsgCoverage::Decoded,     "§5.35 decode_spectator_flag"},
		{'S', s2c::PLAYER_NAME,               "player-name",             MsgCoverage::PrinterOnly, "player name (≤64) → server-info struct @0x429B40"},
		{'S', s2c::FULL_PLAYER_INFO,          "full-player-info",        MsgCoverage::Decoded,     "§5.32 decode_full_player_info"},
		{'S', s2c::SERVER_CONFIG_STRINGS,     "server-config-strings",   MsgCoverage::PrinterOnly, "two cstrings → byte_A86520/byte_A86120 @0x425E20"},
		{'S', s2c::SCORE_DELTA_SOUND,         "score-delta-sound",       MsgCoverage::PrinterOnly, "[i32 score]; positive delta plays tiered hit-confirm sound @0x42A0B0"},
		// ---- C2S (client -> server) ----
		{'C', c2s::JOIN,                      "JOIN",                    MsgCoverage::PrinterOnly, "join request"},
		{'C', c2s::JOIN_FORM_POST,            "join-form-post",          MsgCoverage::PrinterOnly, "§6.4 early-join side-password compare @0x512ED0"},
		{'C', c2s::JOIN_PADDING_ECHO,         "join-padding-echo",       MsgCoverage::PrinterOnly, "§5.55 reply to S2C 0x02: position + N random filler bytes"},
		{'C', c2s::SET_PLAYER_VALUE,          "set-player-value",        MsgCoverage::PrinterOnly, "[i32] -> player entity+372 @0x501BE0"},
		{'C', c2s::FIRED_ROUND,               "fired-round",             MsgCoverage::Decoded,     "§5.16 decode_client_fired_round"},
		{'C', c2s::TIME_SYNC_REPLY,           "time-sync-reply",         MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x43"},
		{'C', c2s::CHECKSUM_RESPONSE,         "checksum-response",       MsgCoverage::PrinterOnly, "client checksum response @0x513200"},
		{'C', c2s::SPAWN_MENU_REQUEST,        "spawn-menu-request",      MsgCoverage::PrinterOnly, "len 0; game state 9 + S2C 0x19 ts ack @0x513260"},
		{'C', c2s::MISSION_FILE_STATUS,       "mission-file-status",     MsgCoverage::PrinterOnly, "§5.4 mission-file status report @0x51AB10"},
		{'C', c2s::ENTITY_UPLINK,             "entity-uplink",           MsgCoverage::Decoded,     "§5.10 decode_player_extended_uplink"},
		{'C', c2s::CHAT_MESSAGE,              "chat-message",            MsgCoverage::Decoded,     "§5.52 decode_chat_uplink (the old 'replication-ack' label was wrong; -> S2C 0x14)"},
		{'C', c2s::RESPAWN_REQUEST,           "respawn-request",         MsgCoverage::PrinterOnly, "[i16 spawnHandle; 0xFFFE=auto team spawn] deploy request @0x519AF0"},
		{'C', c2s::ENTITY_INFO_QUERY,         "entity-info-query",       MsgCoverage::PrinterOnly, "§5.46 [u16 handle] self-heal -> S2C 0x18"},
		{'C', c2s::CHAT,                      "chat",                    MsgCoverage::PrinterOnly, "text chat"},
		{'C', c2s::CHARATTR_CRC_REPLY,        "charattr-crc-reply",      MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x39"},
		{'C', c2s::STANCE_CHANGE,             "stance-change",           MsgCoverage::PrinterOnly, "[i16 actionId] 0xA9 crouch / 0xAA prone / 0xAC stand, sent from the stance key SELECT (@0x4e0d77/@0x4e0df3/@0x4e0e3e) -> NapiNPServerMsg_HandleStanceChange @0x501C60"},
		{'C', c2s::ENTITY_CHECKSUM_REPLY,     "entity-checksum-reply",   MsgCoverage::PrinterOnly, "§5.35 reply to S2C 0x30"},
		{'C', c2s::CHECKSUM_REPLY,            "checksum-reply",          MsgCoverage::Decoded,     "§5.17 decode_client_checksum_reply"},
		{'C', c2s::PLAYER_SYNC_REQUEST,       "player-sync-request",     MsgCoverage::Decoded,     "§5.33 decode_burst_player_sync_request"},
		{'C', c2s::VISIBLE_PLAYERS_REQUEST,   "visible-players-request", MsgCoverage::Decoded,     "§5.33 decode_burst_visible_request"},
		{'C', c2s::LOADOUT_REQUEST,           "loadout-request",         MsgCoverage::Decoded,     "§5.33 decode_burst_loadout_request"},
		{'C', c2s::TEAM_SPAWN_ACK,            "team-spawn-ack",          MsgCoverage::Decoded,     "§5.59 decode_team_spawn_ack ([u16 team_change_index]; deploy/team ack — S2C 0x51 only for a pending team change, D-NET-148)"},
		{'C', c2s::WEAPON_RELOAD_REQUEST,     "weapon-reload-request",   MsgCoverage::Decoded,     "§5.58 decode_weapon_reload (same 4-B body as S2C 0x49; host broadcasts it back @0x514DF0; direction asymmetry vs S2C 0x25, §5.3)"},
		{'C', c2s::VEHICLE_ATTACH_REQUEST,    "vehicle-attach-request",  MsgCoverage::PrinterOnly, "word0 overwritten with requester's own handle -> Entity_ProcessVehicleAttach @0x502390"},
		{'C', c2s::VEHICLE_DETACH_REQUEST,    "vehicle-detach-request",  MsgCoverage::PrinterOnly, "[u16 handle] -> Entity_DetachFromVehicle(entity, entity+364) @0x4FC980"},
		{'C', c2s::RTT_CONSUMED,              "rtt-consumed",            MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'C', c2s::BURST_MEMBER_2D,           "burst-member-2d",         MsgCoverage::PrinterOnly, "§5.33 burst receiver @0x502430"},
		{'C', c2s::LOADOUT_SUBMIT,            "loadout-submit",          MsgCoverage::Decoded,     "§5.56 decode_loadout_submit — byte 0 is TEAM (byte_A85B48), byte 1 the CLASS, then [u32 weaponSlotIndex] + ADM rows -> S2C 0x5A (D-NET-168)"},
		{'C', c2s::EMPTY_SLOT_SWEEP_REQUEST,  "empty-slot-sweep-request", MsgCoverage::Decoded,    "decode_empty_slots_request — no fields read; NapiNPServerMsg_SendEmptySlots @0x51A600 answers S2C 0x5D to the requester only (body builder @0x5160f0). Client sender: the §5.29 0x0F reply burst @0x42e647"},
		{'C', c2s::FILE_CHUNK_REQUEST,        "file-chunk-request-60",   MsgCoverage::PrinterOnly, "§5.28 [u32 id][u32 nextOffset] re-request for S2C 0x60"},
		{'C', c2s::KEEPALIVE,                 "keepalive",               MsgCoverage::PrinterOnly, "§5.44 per-frame housekeeping keepalive (29760-tick cadence); client sender @0x42c1e1 inside Client_ProcessNetworkFrame @0x42c180"},
		{'C', c2s::MISSION_CHUNK_REQUEST,     "file-chunk-request-64",   MsgCoverage::PrinterOnly, "§5.28 [u32 id][u32 nextOffset] re-request for S2C 0x64"},
		{'C', c2s::LOADED_MODEL_PAGE_REPLY,   "loaded-model-page-reply", MsgCoverage::PrinterOnly, "§5.34 frozen loaded-model page reply to S2C 0x68"},
		{'C', c2s::PING,                      "ping",                    MsgCoverage::PrinterOnly, "re-broadcast request -> S2C 0x75 @0x510ED0"},
		{'C', c2s::CLIENT_ACK,                "client-ack",              MsgCoverage::PrinterOnly, "4 B read+discarded; server handler is an empty stub @0x510F30"},
		{'C', c2s::CLIENT_QUALITY,            "client-quality",          MsgCoverage::Decoded,     "§5.33 decode_burst_client_quality"},
		{'C', c2s::GAME_START_ACK,            "game-start-ack",          MsgCoverage::PrinterOnly, "4 B reply to S2C 0x05 game-start signal"},
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
