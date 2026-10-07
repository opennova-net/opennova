#pragma once

#include <cstddef>
#include <cstdint>

#include <net/npwire/ingame_message_id.h>

// Canonical catalog of in-game NAPI message tags (mirrors the §4 dispatch tables
// + §5.x field maps in docs/net/novaworld-net-re.md). Single source of truth for
// opennova-wire's human labels AND the nw_message_coverage CI gate, so a tag can't be
// handled in one place and silently forgotten in the other. Header-only (the data
// is a function-local static) so the app and the test share one definition.
//
// These classes describe wire decoding/printing, not runtime dispatch or gameplay
// fidelity. See docs/net/retail-message-dispatch-audit.md for consumer gaps.
//
// Coverage classes:
//   Decoded     — a structured decode_* exists (`note` names it) and opennova-wire prints
//                 its fields; the coverage test asserts the decoder consumes a
//                 representative body to the byte.
//   PrinterOnly — opennova-wire labels + hex-dumps it; no structured decoder yet.
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
	const char *name;  // short label used by opennova-wire output
	MsgCoverage coverage;
	const char *note;  // Decoded: "§5.x decode_*"; PrinterOnly: section ref; Unhandled: reason
};

// The catalog table + its length (via out-param). The Decoded `note`s name the
// decoder in engine/net/npwire/ingame_decode.h.
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
		{'S', s2c::ENTITY_REMOVE,              "entity-expiry",           MsgCoverage::Decoded,     "decode_entity_remove: [u16 handle], sent before Server_RemoveEntityAndNotify destroys the row @0x50A270"},
		{'S', s2c::ENTITY_DEATH,              "entity-death",            MsgCoverage::Decoded,     "§5.35 decode_entity_death"},
		{'S', s2c::CHAT_BROADCAST,            "chat-broadcast",          MsgCoverage::Decoded,     "§5.52 decode_chat_broadcast (fan-out of C2S 0x0D)"},
		{'S', s2c::PLAYER_LIST,               "player-list",             MsgCoverage::Decoded,     "§5.20 decode_player_list"},
		{'S', s2c::FULL_ENTITY_SPAWN,         "full-entity-spawn",       MsgCoverage::Decoded,     "§5.46 decode_full_entity_spawn (0x0F reply + 0x40 vehicle-spawn broadcast)"},
		{'S', s2c::SPAWN_ACK_TIMESTAMP,       "spawn-ack-timestamp",     MsgCoverage::Decoded,     "§5.34-style decode_u32_scalar (ts ack of C2S 0x0A → dword_A82360 = the C2S 0x28 kill-window MIN, @0x4318db)"},
		{'S', s2c::WAIT_FOR_GAME_START_ACK,   "wait-for-game-start-ack", MsgCoverage::PrinterOnly, "game-start gate: [u32 GetTickCount] → dword_A82364 (@0x425ecb; zeroed at NapiClient_WaitForGameStart entry @0x42cc51) = the 0x4E-continuation C2S 0x28 kill-window MAX (@0x4318e8)"},
		{'S', s2c::NOOP,                      "noop",                    MsgCoverage::PrinterOnly, "empty stub @0x4227F0"},
		{'S', s2c::END_ROUND_HEADER,          "end-round-header",        MsgCoverage::Decoded,     "§5.68 decode_end_round_header; 7-B team form or the non-team top-three-names form, then client requests C2S 0x2B offset 0"},
		{'S', s2c::GAME_EVENT,                "game-event",              MsgCoverage::Decoded,     "§5.26 decode_game_event"},
		{'S', s2c::POOL3_SYNC,                "pool3-sync",              MsgCoverage::Decoded,     "§5.12 decode_pool3_sync_batch"},
		{'S', s2c::SCRIPT_REMOTE_COMMAND,     "script-remote-command",   MsgCoverage::Decoded,     "decode_script_remote_command: [u16 WAC registry index] + operands by that row's types (Text/Filename cstr <= 250, Ssn u16, else u32); WacScript_ExecuteBytecode @0x4F58B0 -> GameMode_DispatchRemoteCommand @0x4F81E0"},
		{'S', s2c::KILL_SYNC,                 "kill-sync",               MsgCoverage::Decoded,     "§5.26 decode_kill_record"},
		{'S', s2c::CHAR_MINIMAP_UPDATE,       "char-minimap-update",     MsgCoverage::PrinterOnly, "[u8 pool0Idx][u8 team][u8 flags7][u16 packedCharId] → NetId + CharacterEntity rebind @0x427D00 (§5.59)"},
		{'S', s2c::CHAT_HISTORY,              "chat-history",            MsgCoverage::Decoded,     "§5.35 decode_chat_history_entry"},
		{'S', s2c::MISSION_MAP_NAMES,         "mission-map-names",       MsgCoverage::Decoded,     "§5.51 decode_mission_map_names (join burst)"},
		{'S', s2c::OBJECTIVE_ENTITY_STATE,    "objective-entity-state",  MsgCoverage::Decoded,     "decode_objective_entity_state: 19-B flag/carryable pose + occupant + ground links; NapiNPClientMsg_0x02F @0x430E10"},
		{'S', s2c::ENTITY_CHECKSUM_REQ,       "entity-checksum-req",     MsgCoverage::Decoded,     "§5.35 decode_entity_checksum_request (-> C2S 0x20)"},
		{'S', s2c::LOADOUT_CRC_REQ,           "loadout-crc-req",         MsgCoverage::Decoded,     "§5.35 decode_loadout_crc_request (3 B ammo-def CRC request → C2S 0x21 @0x4311E0)"},
		{'S', s2c::PLAY_SOUND,                "play-sound",              MsgCoverage::Decoded,     "§5.50 decode_play_sound (profile name + optional 3D pos)"},
		{'S', s2c::CHARATTR_CRC_CHALLENGE,    "charattr-crc-challenge",  MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (seed -> C2S 0x1C)"},
		{'S', s2c::MAP_OVERLAY_RESET,         "map-overlay-reset",       MsgCoverage::PrinterOnly, "empty body; clears the 1160 map-overlay slots and the 251 linked rows, sent last in the host's join tail [orig: NapiNPClientMsg_0x03E @0x4226D0 -> sub_5BE8D0 @0x5BE8D0; Server_OnPlayerJoin @0x51aaee]"},
		{'S', s2c::CAPTURE_ZONE_STATE,        "capture-zone-state",      MsgCoverage::Decoded,     "§5.19 decode_capture_zone_overlay"},
		{'S', s2c::CHARATTR_PROPERTY_CLEAR,   "charattr-property-clear", MsgCoverage::PrinterOnly, "[u8 propertyId] zeroes that property across the retained g_CharAttr[16] table and raises its disable latch (short body -> property 0); NapiNPClientMsg_CharAttrDisableProperty @0x4254C0 (IDB ClearAnimSlot until 2026-10-07) -> CharAttr_SetProperty @0x412890, CharAttr_SetPropertyAllowed @0x4125C0; the host sends 0, 5, 6, 7 at a join (Server_SendCharAttrRestrictionsToPlayer @0x509950)"},
		{'S', s2c::CHARATTR_DISABLED_PROPERTIES,         "charattr-disabled-properties",       MsgCoverage::Decoded,     "§5.35 decode_charattr_disabled_properties: [u16] the authority's packed charattr disable latches -> CharAttr_UnpackDisabledProperties @0x4124D0"},
		{'S', s2c::TIME_SYNC_PING,            "time-sync-ping",          MsgCoverage::Decoded,     "§5.34 decode_u32_scalar (serverTs -> C2S 0x08)"},
		{'S', s2c::ENTITY_ROUTED,             "entity-routed",           MsgCoverage::PrinterOnly, "§5.36 decode_entity_routed_packet (sub-header; class body partial)"},
		{'S', s2c::TERRAIN_LOAD,              "terrain-load",            MsgCoverage::Decoded,     "§5.37 decode_terrain_load_batch"},
		{'S', s2c::PLAYER_SYNC,               "player-sync",             MsgCoverage::Decoded,     "§5.21 decode_player_sync"},
		{'S', s2c::WEAPON_RELOAD,             "weapon-reload",           MsgCoverage::Decoded,     "§5.35 decode_weapon_reload"},
		{'S', s2c::VISIBLE_PLAYERS,           "visible-players",         MsgCoverage::Decoded,     "decode_visible_players / encode_visible_players: [u8 count] + count x {u8 slot, u16 entity handle}, short reads 0 -> the player-slot pointer table g_PlayerSlotPtrTable/Count (the map's bit-5 loop 1, the friendly-tag player walk, Entity_GetDisplayName, the 1 Hz revive countdown) [orig: NapiNPClientMsg_0x04C @0x428570; builder NetPacket_SerializeVisiblePlayersSnapshot @0x506320 from the C2S 0x23 handler @0x514D50]"},
		{'S', s2c::SPAWN_SLOT_NOTICE,         "spawn-slot-notice",       MsgCoverage::Decoded,     "decode_spawn_slot_notice / encode_spawn_slot_notice: [u8 slot] to every in-game slot on a player join (mask 0x80) — the slot's downed state cleared; another slot re-requests C2S 0x22 {slot, 0x1CF7} + C2S 0x23; the own slot raises tip 22 on the death screen [orig: NapiNPClientMsg_HandleSpawnSlot @0x4317B0; sender Server_OnPlayerJoin @0x51a93f..0x51a97a]"},
		{'S', s2c::KILL_BY_SLOT,              "kill-by-slot",            MsgCoverage::Decoded,     "§5.26 decode_batch_kill / encode_batch_kill: one PAGE of the join-window kill list — [u16 resume index (0xFFFF = exhausted)] then every following u16 slot to the body end (<=33 per page); any slot -> Entity_KillBySlotId + the C2S 0x28 continuation {A82360, A82364, resume}; a bare FF FF ends the walk [orig: NapiNPClientMsg_HandleBatchKill @0x431870; page builder Server_CollectValidWeaponSlots @0x516000, predicate NetSync_IsEntityEligibleInWindow @0x507AA0, sender @0x51a5f4 mask 160]"},
		{'S', s2c::TEAM_ASSIGN,               "team-assign",             MsgCoverage::Decoded,     "decode_team_assign — [u16 handle][u8 team][u16 netId][u8 animSlot] (the identity pair, zeroed for non-players by the Flags & 0x100 gate @0x506b3d); NapiNPClientMsg_TeamAssign (0x50) @0x431910 latches byte_A85B48 for self, writes entity Team otherwise, and re-sends ONE C2S 0x2F with slot 195 raw"},
		{'S', s2c::TEAM_CHANGE_CONFIRM,       "team-change-confirm",     MsgCoverage::Decoded,     "decode_team_change_confirm / encode_team_change_confirm: [i16 index] + the 0x50 body, NetPacket_WriteEntityPacket @0x506BB0 for g_TeamChangeEntityList[idx] (@0x514F10, team-change only); the client writes Team off the authority, rebinds a player's NetId/animSlot and queues C2S 0x29 {index + 1} [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0] — never sent on a plain join (D-NET-148, §5.59)"},
		{'S', s2c::DEATH_CAMERA_TARGET,       "death-camera-target",     MsgCoverage::Decoded,     "decode_death_camera_target: victim-only [i32 x][i32 y][i32 z] consumed by Camera_ComputeThirdPersonPositions @0x438B80"},
		{'S', s2c::ZONE_TIMER_WINDOW,         "zone-timer-window",       MsgCoverage::Decoded,     "§5.49 decode_zone_timer_window (capture/takeover HUD)"},
		{'S', s2c::PLAYER_DOWNED_STATE,       "player-downed-state",     MsgCoverage::Decoded,     "decode_player_downed_state: [u16 entity][u8 reviveSeconds|medicRequestBit] @0x429040"},
		{'S', s2c::END_ROUND_STATS,           "end-round-stats",         MsgCoverage::Decoded,     "§5.68 decode_end_round_stats_chunk + decode_end_round_stats (stat.mnu source; pulled 200 B at a time over C2S 0x2B)"},
		{'S', s2c::RTT_ECHO,                  "rtt-echo",                MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'S', s2c::SESSION_STATUS,            "session-status",          MsgCoverage::Decoded,     "§5.48 decode_session_status (server/mission names + score rules) -> the client's g_SessionStatus store (names 31/63, the first 8 pairs with key <= 9), the CMAP RULES text [orig: SessionStatus_ParseFromBuffer @0x530ed0]"},
		{'S', s2c::DEPLOYED_ITEM,             "deployed-item",           MsgCoverage::Decoded,     "§5.36 decode_deployed_item_spawn"},
		{'S', s2c::WEAPON_LOADOUT,            "weapon-loadout",          MsgCoverage::Decoded,     "§5.30 decode_weapon_loadout"},
		{'S', s2c::EMPTY_SLOT_SWEEP,          "empty-slot-sweep",        MsgCoverage::Decoded,     "decode_destroy_entity_list — [i16 pool0Index]xN RAW pool-0 indices; NapiNPClientMsg_DestroyEntityList @0x429730 (!is_authority) destroys each + PlayerSlot_ClearAndUnlink @0x434730. Reply to C2S 0x32 (D-NET-176)"},
		{'S', s2c::FILE_TRANSFER_CHUNK,       "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', s2c::TICK_SEED,                 "tick-seed",               MsgCoverage::Decoded,     "decode_tick_seed — per-player clock anchor; the host's fire-freshness floor @0x4297c0/@0x5101a0"},
		{'S', s2c::MISSION_DATA_CHUNK,        "file-transfer-chunk",     MsgCoverage::Decoded,     "§5.28 decode_file_transfer_chunk"},
		{'S', s2c::WEAPON_RESTRICTIONS,       "weapon-restrictions",     MsgCoverage::PrinterOnly, "count + (slot,restriction) pairs @0x42D4C0"},
		{'S', s2c::LOADED_MODEL_PAGE_REQUEST, "loaded-model-page-request", MsgCoverage::Decoded,   "§5.34 decode_u32_scalar (cursor pages frozen loaded-model rows -> C2S 0x3D)"},
		{'S', s2c::MINIMAP_OVERLAY,           "minimap-overlay",         MsgCoverage::Decoded,     "§5.35 decode_minimap_overlay_batch / encode_minimap_overlay_batch: the host's designation table for the recipient's team, per 14-tick overlay build [orig: NetPacket_SerializeDesignations @0x5116A0 <- Server_SendDesignationsToPlayer @0x517F70]"},
		{'S', s2c::ZONE_PRESENCE_COUNT,       "zone-presence-count",     MsgCoverage::Decoded,     "§5.61 decode_zone_presence_count; active timed-capture rate @0x428FC0"},
		{'S', s2c::SPAWN_WAVE_STATUS,         "spawn-wave-status",       MsgCoverage::Decoded,     "§5.31 decode_spawn_wave_status"},
		{'S', s2c::ZONE_TIMER_VALUE,          "zone-timer-value",        MsgCoverage::Decoded,     "§5.49 decode_zone_timer_value (NOT cinematic camera)"},
		{'S', s2c::SPECTATOR_FLAGS,           "player-slot-state",       MsgCoverage::PrinterOnly, "[spectator bit, team] → g_DeathScreenActive/byte_A85B48 @0x4259E0"},
		{'S', s2c::CLASS_ALLOW_MASK,          "class-allow-mask",        MsgCoverage::PrinterOnly, "u16 → g_HostClassAllowMask @0x24D59FC [orig: NapiNPClientMsg_HandleClassAllowMask @0x42D540; sender @0x510350]"},
		{'S', s2c::NETWORK_QUALITY,            "network-quality",         MsgCoverage::Decoded,     "§5.35 decode_network_quality; 310-tick host CNetQuality broadcast"},
		{'S', s2c::PLAYER_NAME,               "player-name",             MsgCoverage::PrinterOnly, "player name (≤64) → server-info struct @0x429B40"},
		{'S', s2c::FULL_PLAYER_INFO,          "full-player-info",        MsgCoverage::Decoded,     "§5.32 decode_full_player_info"},
		{'S', s2c::SERVER_CONFIG_STRINGS,     "server-config-strings",   MsgCoverage::Decoded,     "decode_server_config_strings: two cstrings → byte_A86520/byte_A86120 @0x425E20 (the client's briefing strings, the CMAP RULES text)"},
		{'S', s2c::SCORE_DELTA_SOUND,         "score-delta-sound",       MsgCoverage::Decoded,     "decode_score_delta_sound — requester-local CRenderState points [i32] @0x5086A0/@0x42A0B0"},
		{'S', s2c::CLIENT_CONSOLE_COMMAND, "client-console-command", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_HandleChatCommand @0x432bc0]"},
		{'S', s2c::RESERVED_NOOP_07, "reserved-noop-07", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPClientMsg_0x007 @0x422730]"},
		{'S', s2c::ENTITY_SLOT_MESSAGE, "entity-slot-message", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x017 @0x4226f0]"},
		{'S', s2c::NETWORK_DELAY, "network-delay", MsgCoverage::PrinterOnly, "i32 network delay, ignored by authority (the IDB RandomSeed name is a misnomer) [orig: NapiNPClientMsg_HandleRandomSeed @0x426080]"},
		{'S', s2c::COUNTDOWN_SYNC, "countdown-sync", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x01F @0x427cb0]"},
		{'S', s2c::EXPLOSION_EFFECT, "explosion-effect", MsgCoverage::Decoded, "decode_explosion_effect: type 0 item explosion; type 2 debris remains inert [orig: NapiNPClientMsg_HandleSpawnEffect @0x430b10]"},
		{'S', s2c::ENTITY_CREATE, "entity-create", MsgCoverage::Unhandled, "the dropped-weapon create; its one producer is the C2S 0x18 handler's non-powerup tail for a dead dropper, which no stock client reaches (D-PWR-5, world/powerup-re.md) [orig: NetPacket_HandleEntityCreate @0x42ec90; NetPacket_SerializeEntityOverlayState @0x503ff0]"},
		{'S', s2c::TEXT_COMMAND, "text-command", MsgCoverage::PrinterOnly, "quoted text command; SETFLASH1, SETCEASEFIRE, SU, GOTO, SPECTATORTARGET [orig: NapiNPClientMsg_HandleTextCommand @0x429e70]"},
		{'S', s2c::GAME_RESET, "game-reset", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_GameReset @0x422800]"},
		{'S', s2c::SPAWN_EFFECT, "spawn-effect", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_SpawnEffect @0x425aa0]"},
		{'S', s2c::DIALOG_LINE, "dialog-line", MsgCoverage::Decoded, "decode_dialog_line / encode_dialog_line: [cstr dialog name][i16 line], the co-op dialog line the authority's playback loaded (mask 0x90); a non-authority plays that line localized by its player class [orig: Dialog_UpdatePlayback @0x44e5a5 -> Server_SendEntityStateToAll @0x50A0D0; NapiNPClientMsg_0x028 @0x425b40 -> Dialog_PlayByNameAndSlot @0x44E3F0]"},
		{'S', s2c::EXIT_SESSION, "exit-session", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x02B @0x427df0]"},
		{'S', s2c::EMOTE_BROADCAST, "emote-broadcast", MsgCoverage::Decoded, "decode_emote_broadcast / encode_emote_broadcast: [u8 emote][u8 raw pool-0 index][u16 0] -> the speaker's emote state 114+emote, the EMO_ voice unless the slot's voice-mute bit, HUD_SetTrackedEntityTarget @0x59D050 (@0x427f5b); session peers only [orig: NapiNPClientMsg_HandleEmote @0x427e90; sender NapiNPServerMsg_HandleEmoteRequest @0x501e00]"},
		{'S', s2c::FORM_FIELD, "form-field", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x02E @0x427f80]"},
		{'S', s2c::FORMATTED_GAME_TEXT, "formatted-game-text", MsgCoverage::Decoded, "decode_formatted_game_text / encode_formatted_game_text: [i8 subtype][cstr text] (+[i8 team] for 1/2): 1 joined (team 0 STRCLI22, types <2/8 STRCLI11, teams 1..4 STRCLI12..15), 2 leaving STRCLI16, 3 the squadron literal, 4 nothing, 5 spectating STRCLI24 — Chat_FormatPlayerTokens $A, SYSTEM ring 0xFFAFAFAF/930 [orig: NapiNPClientMsg_0x032 @0x428060; senders Server_PlayerAdd @0x51d21e (1) / @0x51d18e (3), Server_HandlePlayerDisconnect @0x51b6a8 (2) / @0x51b705 (4), the spectator convert @0x519edc (5)]"},
		{'S', s2c::WAYPOINT_CREATE, "waypoint-create", MsgCoverage::Decoded, "decode_waypoint_create: [cstr name][i32 x][i32 y][i32 z skipped][u8 owner] -> Waypoint_CreateForPlayer @0x4DFCB0 [orig: NapiNPClientMsg_0x033 @0x425fa0]"},
		{'S', s2c::WEAPON_PICKUP, "weapon-pickup", MsgCoverage::Decoded, "decode_weapon_pickup / encode_weapon_pickup: [u16 picker][u16 powerup] (4 B), mask 0x90 reliable, from the authority for every powerup `weapon` grant; the picker's live client lands the row's +0x2B0 weapon and mounts it (world/powerup-re.md) [orig: NapiNPClientMsg_0x035 @0x4261a0 -> sub_4E03D0 @0x4e03d0; Server_BroadcastWeaponOverlayUpdate @0x509fc0]"},
		{'S', s2c::VEHICLE_SPAWN_NOTIFY, "vehicle-spawn-notify", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x036 @0x426120]"},
		{'S', s2c::DOOR_SLOT_ACTION, "door-slot-action", MsgCoverage::Decoded, "decode_door_slot_action / encode_door_slot_action: [u16 handle][i16 state][u8 number] (5 B, short reads 0) -> door row (door_slot + number - 1).state = state, phase 0 for state 0 and 0x10000 for state 2, gated number != 0 && number <= door count; the IDB 'weapon slot' name is a misnomer for the 0xA8A418 door rows [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x431250; senders Server_SendWeaponSlotActionPacket @0x50F9A0 (mask 0x90: FadeEffect_UpdateAll completion @0x44e982 1-based, Entity_ProcessSectionDamageTransition per section @0x43f462 0-based) and the C2S 0x1A reply @0x514c74 (mask 0x30)]"},
		{'S', s2c::WEAPON_SWITCH, "weapon-switch", MsgCoverage::Unhandled, "the post-drop switch [i32 handle][i32 token]; its one producer is the C2S 0x18 handler's tail, which no stock client reaches, and the token it matches is zero unless a drop was sent (D-PWR-5, world/powerup-re.md) [orig: NapiNPClientMsg_HandleWeaponSwitchPacket @0x4260b0; NetPacket_WriteTwoDwords @0x505430]"},
		{'S', s2c::MEDIC_REVIVING, "medic-reviving", MsgCoverage::Decoded, "decode_medic_reviving (empty body, any length accepted like the handler); latches the local entity's +0x1E0 being-revived word (ClientState::local_medic_reviving) — hides the DEATH screen's MEDIC/CALLMEDIC statics; cleared at the local respawn [orig: NapiNPClientMsg_0x03A @0x422680]"},
		{'S', s2c::FORM_POST_REQUEST, "form-post-request", MsgCoverage::PrinterOnly, "no fields read; queues reliable C2S 0x01 with one zero byte [orig: NapiNPClientMsg_0x03B @0x431340]"},
		{'S', s2c::EXIT_SESSION_ALT, "exit-session-alt", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x03D @0x422870]"},
		{'S', s2c::OBJECTIVE_NOTIFICATION, "objective-notification", MsgCoverage::Decoded, "decode_objective_notification: [u8 kind] + kind 0 [i32 slot][i32 is_win][i32 is_active][u8 flag] / kind 1 [i32 team][cstr key] [orig: NapiNPClientMsg_0x03F @0x42bb20; Server_BroadcastEntityActionPacket @0x5080d0]"},
		{'S', s2c::TARGET_LIST, "target-list", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x048 @0x4284b0]"},
		{'S', s2c::TARGET_LIST_PAGE, "target-list-page", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x04F @0x4286c0]"},
		{'S', s2c::WORLD_SYNC_REQUEST, "world-sync-request", MsgCoverage::PrinterOnly, "serializes world-sync state for the active player (the IDB HealthUpdate name is a misnomer) [orig: NapiNPClientMsg_HandleHealthUpdate @0x4322b0]"},
		{'S', s2c::LOAD_SAVED_GAME, "load-saved-game", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x05C @0x425200]"},
		{'S', s2c::RESERVED_NOOP_5E, "reserved-noop-5e", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPClientMsg_0x05E @0x4297b0]"},
		{'S', s2c::RESERVED_NOOP_5F, "reserved-noop-5f", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPClientMsg_0x05F @0x4228f0]"},
		{'S', s2c::MEMORY_CRC_CHALLENGE, "memory-crc-challenge", MsgCoverage::PrinterOnly, "three-dword memory CRC challenge records; reply C2S 0x35; arbitrary retail address space is not modeled [orig: NapiNPClientMsg_0x062 @0x42d200]"},
		{'S', s2c::POOF_TOGGLE, "poof-toggle", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NetMsg_HandlePoofToggle @0x42d450]"},
		{'S', s2c::RESERVED_NOOP_65, "reserved-noop-65", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPClientMsg_0x065 @0x429870]"},
		{'S', s2c::TELEPORT, "teleport", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x067 @0x42d570]"},
		{'S', s2c::CLAN_ROSTER, "clan-roster", MsgCoverage::Decoded, "decode_clan_roster_update / encode_clan_roster_update: [u8 action][u32 accountNetId] + (actions 1 add / 3 walk-reply) [cstr name <=64][cstr tag <=8]; action 2 removes; non-authority only; the node TAG feeds PlayerSlot_SetName (slot dword 8, keyed by the 0x46 field-0x0800 account id in slot dword 15); action 3 re-queues C2S 0x4E {netId} [orig: NapiNPClientMsg_HandlePlayerJoinLeave @0x432510; CLinkedList_FindOrCreateByNetId @0x52B540; PlayerSlotTable_UpdateAllDisplayNames @0x434A00 -> PlayerSlot_SetName @0x4348F0; serializer NetPacket_SerializeMinimapSlot @0x5073B0]"},
		{'S', s2c::TRACKED_PLAYER_VOICE, "tracked-player-voice", MsgCoverage::Decoded, "decode_tracked_player_voice / encode_tracked_player_voice: [u8 event][u8 pool-0 index][i16 location, -1 none]; contextual radio, chat, and tracking; produced by the host's C2S 0x13 radio-call handler [orig: NapiNPClientMsg_HandleEntityDeath @0x430c50; NapiNPServerMsg_HandleRadioCall @0x514330]"},
		{'S', s2c::VEHICLE_SPAWN_AVAILABILITY, "vehicle-spawn-availability", MsgCoverage::Decoded, "decode_vehicle_spawn_availability / encode_vehicle_spawn_availability: [u8 3] then per EntityLimit row [u16 typeId][u8 avail][u8 max] (0xFF/0xFF = unlimited) until u16 0; requester-only reply to C2S 0x42 (mask 32); the client fills the 12-B-stride table @0xA81BC0 [orig: NapiNPClientMsg_HandleWeaponLoadoutList @0x429a30; NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0 <- NapiNPServerMsg_SendWeaponSlotStates @0x510930]"},
		{'S', s2c::SQUAD_JOIN, "squad-join", MsgCoverage::Decoded, "decode_squad_join: [u8 leader][u8 member] -> the roster link, the join line / sound / waypoint push, the leave's foreign-waypoint sweep [orig: NapiNPClientMsg_HandleSquadJoin @0x425600]"},
		{'S', s2c::SQUAD_ORDER, "squad-order", MsgCoverage::Decoded, "decode_squad_order: [u8 kind][cstr] -> the HUD order line (kind < 2); nonempty plays MP_COMMAND1 [orig: NapiNPClientMsg_0x072 @0x425710 -> Team_SetNameByIndex @0x59C2D0 (a misnomer)]"},
		{'S', s2c::FIRETEAM_SET, "fireteam-set", MsgCoverage::Decoded, "decode_fireteam_set: [u8 member][u8 fireteam] -> the roster fireteam, the own-slot HUD_CMAP_SETFIRETEAM line [orig: NapiNPClientMsg_0x073 @0x425770]"},
		{'S', s2c::SQUAD_RECRUITED, "squad-recruited", MsgCoverage::Decoded, "decode_squad_recruited: [u8 recruiter] -> the RECRUIT sound and HUD_CMAP_RECRUIT line [orig: NapiNPClientMsg_PlayerRecruited @0x4258b0]"},
		{'S', s2c::GO_CODE, "go-code", MsgCoverage::Decoded, "decode_go_code: [u8 leader][u8 code] -> the leader's _GC_ sound and HUD_CMAP_GOCODE line [orig: NapiNPClientMsg_0x078 @0x425970]"},
		{'S', s2c::DESTROY_ENTITY, "destroy-entity", MsgCoverage::Decoded, "decode_entity_handle16: [u16 handle] -> Entity_Destroy of that pool row (0xFFFF none; a short body reads 0) [orig: NapiNPClientMsg_0x07C @0x426020]"},
		{'S', s2c::CLIENT_METRICS_REQUEST, "client-metrics-request", MsgCoverage::PrinterOnly, "no fields read; replies with four i32 client metrics in C2S 0x50 [orig: NapiNPClientMsg_0x07D @0x432690]"},
		{'S', s2c::RESERVED_NOOP_7F, "reserved-noop-7f", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPClientMsg_0x07F @0x429e60]"},
		{'S', s2c::SCORE_TRACKER_RESET, "score-tracker-reset", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x080 @0x42a070]"},
		{'S', s2c::SCORE_TRACKER_TIME, "score-tracker-time", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x082 @0x42a0e0]"},
		{'S', s2c::PLAYER_PROFILE_REFRESH, "player-profile-refresh", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPClientMsg_0x083 @0x4326e0]"},
		// ---- C2S (client -> server) ----
		{'C', c2s::JOIN,                      "JOIN",                    MsgCoverage::PrinterOnly, "join request"},
		{'C', c2s::JOIN_FORM_POST,            "join-form-post",          MsgCoverage::PrinterOnly, "§6.4 early-join side-password compare @0x512ED0"},
		{'C', c2s::JOIN_PADDING_ECHO,         "join-padding-echo",       MsgCoverage::PrinterOnly, "§5.55 reply to S2C 0x02: position + N random filler bytes"},
		{'C', c2s::AUTO_MEDIC_PREFERENCE,     "auto-medic-preference",   MsgCoverage::Decoded,     "decode_auto_medic_preference: [i32 disabled] -> playerSlot+372 (NapiNPServerMsg_AutoMedicPreference @0x501BE0; short body stores 0 @0x501C16); source OPTIONS_AUTOMEDIC inverse @0x5549E7/@0x554E40"},
		{'C', c2s::FIRED_ROUND,               "fired-round",             MsgCoverage::Decoded,     "§5.16 decode_client_fired_round"},
		{'C', c2s::TIME_SYNC_REPLY,           "time-sync-reply",         MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x43"},
		{'C', c2s::CHECKSUM_RESPONSE,         "checksum-response",       MsgCoverage::PrinterOnly, "client checksum response @0x513200"},
		{'C', c2s::SPAWN_MENU_REQUEST,        "spawn-menu-request",      MsgCoverage::PrinterOnly, "len 0; game state 9 + S2C 0x19 ts ack @0x513260"},
		{'C', c2s::MISSION_FILE_STATUS,       "mission-file-status",     MsgCoverage::PrinterOnly, "§5.4 mission-file status report @0x51AB10"},
		{'C', c2s::ENTITY_UPLINK,             "entity-uplink",           MsgCoverage::Decoded,     "§5.10 decode_player_extended_uplink"},
		{'C', c2s::CHAT_MESSAGE,              "chat-message",            MsgCoverage::Decoded,     "§5.52 decode_chat_uplink (the old 'replication-ack' label was wrong; -> S2C 0x14)"},
		{'C', c2s::RESPAWN_REQUEST,           "respawn-request",         MsgCoverage::PrinterOnly, "[i16 spawnHandle; 0xFFFE=auto team spawn] deploy request @0x519AF0"},
		{'C', c2s::ENTITY_INFO_QUERY,         "entity-info-query",       MsgCoverage::PrinterOnly, "§5.46 [u16 handle] self-heal -> S2C 0x18"},
		{'C', c2s::MOUNTED_WEAPON_SLOT_SELECT,"mounted-weapon-slot-select", MsgCoverage::Decoded,   "decode_mounted_weapon_slot_selection: action-6 bool-as-i16 -> NapiNPServerMsg_HandleWeaponToggle @0x511A70"},
		{'C', c2s::CHARATTR_CRC_REPLY,        "charattr-crc-reply",      MsgCoverage::PrinterOnly, "§5.34 reply to S2C 0x39"},
		{'C', c2s::STANCE_CHANGE,             "stance-change",           MsgCoverage::PrinterOnly, "[i16 actionId] 0xA9 crouch / 0xAA prone / 0xAC stand, sent from the stance key SELECT (@0x4e0d77/@0x4e0df3/@0x4e0e3e) -> NapiNPServerMsg_HandleStanceChange @0x501C60"},
		{'C', c2s::ENTITY_CHECKSUM_REPLY,     "entity-checksum-reply",   MsgCoverage::PrinterOnly, "§5.35 reply to S2C 0x30"},
		{'C', c2s::CHECKSUM_REPLY,            "checksum-reply",          MsgCoverage::Decoded,     "§5.17/§5.65 decode_client_checksum_reply (reply to S2C 0x31)"},
		{'C', c2s::PLAYER_SYNC_REQUEST,       "player-sync-request",     MsgCoverage::Decoded,     "§5.33 decode_burst_player_sync_request"},
		{'C', c2s::VISIBLE_PLAYERS_REQUEST,   "visible-players-request", MsgCoverage::Decoded,     "§5.33 decode_burst_visible_request"},
		{'C', c2s::LOADOUT_REQUEST,           "loadout-request",         MsgCoverage::Decoded,     "§5.33 decode_burst_loadout_request / encode_burst_loadout_request: [u32 windowMin = the S2C 0x19 value][u32 windowMax = the S2C 0x1A value (0x4E continuation) or the 0x0F session tick (burst)][u16 start] -> one S2C 0x4E kill-list page from `start`, or nothing when the walk is exhausted / spawns suspended (IDB misnomer 'weapon loadout') [orig: NapiNPServerMsg_HandleWeaponLoadoutRequest @0x51A550; senders @0x42e5f7 (burst), @0x4318ff (continuation)]"},
		{'C', c2s::TEAM_SPAWN_ACK,            "team-spawn-ack",          MsgCoverage::Decoded,     "§5.59 decode_team_spawn_ack ([u16 team_change_index]; deploy/team ack — S2C 0x51 only for a pending team change, D-NET-148)"},
		{'C', c2s::WEAPON_RELOAD_REQUEST,     "weapon-reload-request",   MsgCoverage::Decoded,     "§5.58 decode_weapon_reload (same 4-B body as S2C 0x49; host broadcasts it back @0x514DF0; direction asymmetry vs S2C 0x25, §5.3)"},
		{'C', c2s::VEHICLE_ATTACH_REQUEST,    "vehicle-attach-request",  MsgCoverage::PrinterOnly, "word0 overwritten with requester's own handle -> NapiNPServerMsg_HandleVehicleAttach @0x502390 -> Entity_ProcessVehicleAttach @0x435aa0"},
		{'C', c2s::VEHICLE_DETACH_REQUEST,    "vehicle-detach-request",  MsgCoverage::PrinterOnly, "[u16 handle] -> Entity_DetachFromVehicle(entity, entity+364) @0x4FC980"},
		{'C', c2s::END_ROUND_STATS_REQUEST,   "end-round-stats-request", MsgCoverage::Decoded,     "§5.68 decode_end_round_stats_request; [u16 offset] -> requester-only S2C 0x56, max 200 B"},
		{'C', c2s::RTT_CONSUMED,              "rtt-consumed",            MsgCoverage::Decoded,     "§5.34 decode_rtt_sample"},
		{'C', c2s::BURST_MEMBER_2D,           "burst-member-2d",         MsgCoverage::PrinterOnly, "§5.33 burst receiver @0x502430"},
		{'C', c2s::MEDIC_REQUEST,             "medic-request",           MsgCoverage::Decoded,     "decode_medic_request: [u32 entityIndex] (never read by the host) — Server_BroadcastMedicRequest @0x515390: STRSRV_MEDREQ null -> no-op, playerSlot+368 > 0, SpawnWaveList_RemovePlayer, S2C 0x54 [handle][slot+368] to the 0x580 medic set when manual && !latched, S2C 0x14 [2][senderSlot][msg] mask 0x5C0 then 0x20, latch slot+89856, MEDIC_REQUEST 0x34 fan; client Input_HandleActionBinding case 217 @0x49B4B4 (dead local entity, 310-frame latch dword_B76804) (D-NET-108)"},
		{'C', c2s::LOADOUT_SUBMIT,            "loadout-submit",          MsgCoverage::Decoded,     "§5.56 decode_loadout_submit — byte 0 is TEAM (byte_A85B48), byte 1 the CLASS, then [u32 weaponSlotIndex] + ADM rows -> S2C 0x5A (D-NET-168)"},
		{'C', c2s::EMPTY_SLOT_SWEEP_REQUEST,  "empty-slot-sweep-request", MsgCoverage::Decoded,    "decode_empty_slots_request — no fields read; NapiNPServerMsg_SendEmptySlots @0x51A600 answers S2C 0x5D to the requester only (body builder @0x5160f0). Client sender: the §5.29 0x0F reply burst @0x42e647"},
		{'C', c2s::FILE_CHUNK_REQUEST,        "file-chunk-request-60",   MsgCoverage::PrinterOnly, "§5.28 [u32 id][u32 nextOffset] re-request for S2C 0x60"},
		{'C', c2s::KEEPALIVE,                 "keepalive",               MsgCoverage::PrinterOnly, "§5.44 per-frame housekeeping keepalive (29760-tick cadence); client sender @0x42c1e1 inside Client_ProcessNetworkFrame @0x42c180"},
		{'C', c2s::MISSION_CHUNK_REQUEST,     "file-chunk-request-64",   MsgCoverage::PrinterOnly, "§5.28 [u32 id][u32 nextOffset] re-request for S2C 0x64"},
		{'C', c2s::LOADED_MODEL_PAGE_REPLY,   "loaded-model-page-reply", MsgCoverage::PrinterOnly, "§5.34 frozen loaded-model page reply to S2C 0x68"},
		{'C', c2s::PING,                      "ping",                    MsgCoverage::PrinterOnly, "re-broadcast request -> S2C 0x75 @0x510ED0"},
		{'C', c2s::CLIENT_ACK,                "client-ack",              MsgCoverage::PrinterOnly, "4 B read+discarded; server handler is an empty stub @0x510F30"},
		{'C', c2s::CLIENT_QUALITY,            "client-quality",          MsgCoverage::Decoded,     "§5.33 decode_burst_client_quality"},
		{'C', c2s::GAME_START_ACK,            "game-start-ack",          MsgCoverage::Decoded,     "decode_clan_roster_walk_request / encode_clan_roster_walk_request: the clan-roster WALK [u32 afterNetId] — {0} kick when the S2C 0x05 flag is nonzero, {netId} continuation from each S2C 0x6A action 3; the host answers the smallest roster node id > value with 0x6A action 3 (requester only, mask 32) or stays silent (a LAN roster has no nodes) [orig: NapiNPServerMsg_HandleMinimapSlotRequest @0x511210 -> sub_52B190; client kick @0x42e1d9, continuation @0x43266c]"},
		{'C', c2s::ADMIN_NETLOG_COMMAND, "admin-netlog-command", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPServerMsg_Chat @0x5199d0]"},
		{'C', c2s::RESERVED_NOOP_07, "reserved-noop-07", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPServerMsg_0x007 @0x4fc970]"},
		{'C', c2s::RADIO_CALL_REQUEST, "radio-call-request", MsgCoverage::Decoded, "decode_radio_call_request / encode_radio_call_request: [i16 digit] (the host reads the low byte) -> authority, a live non-spectator sender off its slot+380 cooldown: the RAD_ key (flags 6) looked up in the g_RadioCallRules table, the sender's +885/+886 radio latch (event 6 -> 1 / 30), then S2C 0x6D {event, pool-0 index, nearest 2044 location or -1}: no rule -> the sender's team (mask 0x180); a rule -> each same-team state-10 slot its rule type admits (the sender always), unicast; a tracker-3 rule registers a 30 s designation at the sender's aim point (the S2C 0x6B batch); cooldown 4 s [orig: NapiNPServerMsg_HandleRadioCall @0x514330 (ex HandleSectorAction); sender NetPacket_SendRadioCallRequest @0x42C150 from the Radio menu digits @0x49c783..0x49c7a8]"},
		{'C', c2s::EMOTE_REQUEST, "emote-request", MsgCoverage::Decoded, "decode_emote_request / encode_emote_request: [i16 digit] (the host reads the low byte) -> authority, a live non-spectator sender off its slot+376 cooldown: S2C 0x2D {emote, pool-0 index} to every state-10/11 slot within 100 units per axis (the sender included), cooldown 2 s (1 Hz decrement) [orig: NapiNPServerMsg_HandleEmoteRequest @0x501e00; sender NetPacket_SendEmoteRequest @0x42C120 from the Emotes menu digits @0x49c731..0x49c75c]"},
		{'C', c2s::WAYPOINT_SHARE, "waypoint-share", MsgCoverage::Decoded, "decode_waypoint_share: [u8 target][cstr][i32 x][i32 y][i32 z] -> S2C 0x33 to the sender's subordinates (0xFF) or the target [orig: NapiNPServerMsg_HandleChatOrWhisper @0x514850 (a misnomer)]"},
		{'C', c2s::WEAPON_DROP, "weapon-drop", MsgCoverage::Unhandled, "no stock producer (action 50 has no binding row; the S2C 0x36 caller is dead); the retail authority's handler overwrites its own /GS cookie through Entity_InitFromWeaponDef on its stack template and terminates for every powerup-typed body, every one the stock sender writes on a process's first mission; left unhandled (D-PWR-5, world/powerup-re.md) [orig: NapiNPServerMsg_HandleWeaponSpawn @0x51a020]"},
		{'C', c2s::ENTITY_REMOVE_REQUEST, "entity-remove-request", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPServerMsg_0x019 @0x514250]"},
		{'C', c2s::DOOR_SLOT_REQUEST, "door-slot-request", MsgCoverage::Decoded, "decode_door_slot_action / encode_door_slot_action (the S2C 0x37 body): a non-authority's per-section door request [u16 handle][i16 state][u8 number], gated row > -1 && number != 0 && number <= door count; the host's row switch (state 0/3 -> 1 iff value == 1; 1/2 -> value iff number == 3) then S2C 0x37 {handle, row.state, number} to the requester (mask 0x30) [orig: NetPacket_SendWeaponSwitch @0x42D0C0 -> NapiNPServerMsg_HandleVoteUpdate @0x514b20 (both misnomers)]"},
		{'C', c2s::PLAYER_CLASS_SELECT, "player-class-select", MsgCoverage::PrinterOnly, "i32 player class, gated by spectator and dead/preround state; IDB SetFarClip is a misnomer [orig: NapiNPServerMsg_SetFarClip @0x501d90]"},
		{'C', c2s::BAN_PUNT_COMMAND, "ban-punt-command", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPServerMsg_0x024 @0x514dc0]"},
		{'C', c2s::LOAD_SAVED_GAME_REQUEST, "load-saved-game-request", MsgCoverage::PrinterOnly, "no fields read; requester-only S2C 0x5C with empty body [orig: NapiNPServerMsg_0x030 @0x5029b0]"},
		{'C', c2s::RESERVED_NOOP_31, "reserved-noop-31", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPServerMsg_0x031 @0x5024a0]"},
		{'C', c2s::MEMORY_CRC_REPLY, "memory-crc-reply", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPServerMsg_0x035 @0x500df0]"},
		{'C', c2s::RESERVED_NOOP_36, "reserved-noop-36", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPServerMsg_0x036 @0x500e00]"},
		{'C', c2s::RESERVED_NOOP_38, "reserved-noop-38", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPServerMsg_0x038 @0x502510]"},
		{'C', c2s::RESERVED_NOOP_39, "reserved-noop-39", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPServerMsg_0x039 @0x500e20]"},
		{'C', c2s::CLIENT_CRC_VALIDATION, "client-crc-validation", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: Server_HandleClientCRCValidation @0x519110]"},
		{'C', c2s::RESERVED_NOOP_3E, "reserved-noop-3e", MsgCoverage::PrinterOnly, "retail empty handler; no state change or response [orig: NapiNPServerMsg_0x03E @0x500e10]"},
		{'C', c2s::PUNT_VOTE, "punt-vote", MsgCoverage::Decoded, "decode_punt_vote: [u8 target] -> the slot's vote, then Server_ProcessVoteKickResults @0x511400 when voting is on [orig: NapiNPServerMsg_VoteKick @0x518f10]"},
		{'C', c2s::VEHICLE_SPAWN_REQUEST, "vehicle-spawn-request", MsgCoverage::Decoded, "decode_vehicle_spawn_request / encode_vehicle_spawn_request: [u16 sourceHandle][u8 typeIndex] (3 B, short reads 0); the host gates spectator, handle, typeIndex < shared-slot count, the source def's pcvehicle_spawnlist bit, team and the EntityLimit availability, spawns at the 'boat'/'helo' userpoint (else z + 2.0) and fans S2C 0x18 (mask 0x90) [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51c4c0]"},
		{'C', c2s::DEATH_TIMEOUT_RESET, "death-timeout-reset", MsgCoverage::PrinterOnly, "owned player healthMax and death timers reset; IDB ResetPlayerAmmo is a misnomer [orig: NapiNPServerMsg_ResetPlayerAmmo @0x510540]"},
		{'C', c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST, "vehicle-spawn-availability-request", MsgCoverage::Decoded, "decode_vehicle_spawn_availability_request: no fields read; the host answers S2C 0x70 to the requester only (mask 32) when the sender has a player entity [orig: NapiNPServerMsg_SendWeaponSlotStates @0x510930]"},
		{'C', c2s::SQUAD_JOIN_REQUEST, "squad-join-request", MsgCoverage::Decoded, "decode_squad_join_request: [u8 leader] -> the checked link, S2C 0x71 to the member's team [orig: Server_HandleEntitySync @0x510990 (a misnomer)]"},
		{'C', c2s::SQUAD_ORDER_REQUEST, "squad-order-request", MsgCoverage::Decoded, "decode_squad_order_request: [u8 kind][u8 n][cstr][n x u8 slot] -> S2C 0x72 per listed slot [orig: NapiNPServerMsg_HandleChatBroadcast @0x510ae0 (a misnomer)]"},
		{'C', c2s::FIRETEAM_ASSIGN, "fireteam-assign", MsgCoverage::Decoded, "decode_fireteam_assign: [u8 fireteam][u8 n][n x u8 slot] -> the slot fireteam, S2C 0x73 to its team [orig: NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00]"},
		{'C', c2s::SQUAD_RECRUIT, "squad-recruit", MsgCoverage::Decoded, "decode_squad_recruit: [u8 recruiter][u8 target] -> S2C 0x74 to the target [orig: NapiNPServerMsg_HandleVoteKick @0x510d20 (a misnomer)]"},
		{'C', c2s::PLAYER_PROFILE, "player-profile", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: NapiNPServerMsg_0x049_ParsePlayerStatus @0x510f40]"},
		{'C', c2s::GO_CODE, "go-code", MsgCoverage::Decoded, "decode_go_code: [u8 leader][u8 code] -> S2C 0x78 to every in-game slot led by that leader [orig: NapiNPServer_BroadcastPlayerProfileUpdate @0x510dc0 (a misnomer)]"},
		{'C', c2s::TEAM_CHANGE_REQUEST, "team-change-request", MsgCoverage::PrinterOnly, "no body: the death screen's SWAP_TEAMS request -> authority, a non-spectator slot past mpchangeteam_interval, TeamChoose, not unbalancing: team 1 <-> 2 (S2C 0x50), the C2Blue/C2Red S2C 0x14 [7][255] system line, the inline player death, the mpchangeteam_penalty respawn holds [orig: DeathScreen_OnSwapTeams @0x5535b0 -> NetPacket_SendPingRequest @0x42dd90 (a misnomer); NapiNPServerMsg_0x04D_ChangeTeam @0x518f70]"},
		{'C', c2s::WAYPOINT_DELETE, "waypoint-delete", MsgCoverage::Decoded, "decode_entity_handle16: [u16 handle] -> S2C 0x7C to the sender's subordinates [orig: NapiNPServerMsg_0x04F @0x514a40]"},
		{'C', c2s::CLIENT_METRICS, "client-metrics", MsgCoverage::PrinterOnly, "four i32 metrics retained on the bound player slot [orig: NapiNPServerMsg_0x050_UpdatePlayerState @0x5112b0]"},
		{'C', c2s::SPECTATOR_RESPAWN, "spectator-respawn", MsgCoverage::Unhandled, "retail handler; runtime parity remains under audit [orig: Server_ProcessClientRequestSpectatorRespawn @0x51c840]"},
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

// Short label for opennova-wire output, or nullptr for an uncatalogued tag.
inline const char *ingame_message_name(char dir, uint8_t tag) {
	const MsgCatalogEntry *e = lookup_ingame_message(dir, tag);
	return e ? e->name : nullptr;
}

} // namespace opennova
