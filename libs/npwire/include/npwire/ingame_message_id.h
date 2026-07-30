#ifndef OPENNOVA_NPWIRE_INGAME_MESSAGE_ID_H
#define OPENNOVA_NPWIRE_INGAME_MESSAGE_ID_H

#include <cstdint>

// In-game NAPI msg_id bytes, direction-scoped per docs/net/novaworld-net-re.md §5.3:
// the S2C and C2S handlers for the same byte implement DIFFERENT protocol meanings —
// never compare a tag against the other direction's namespace, and never `using
// namespace` these (the s2c::/c2s:: qualifier at the use site IS the direction
// annotation). Names derive mechanically from the ingame_message_catalog.h labels
// (label uppercased, hyphen -> underscore); the catalog rows initialize from these
// constants, so the two cannot drift. Meaning, coverage, and per-tag witness notes
// stay in the catalog + the §4 dispatch tables / §5.x field maps of
// docs/net/novaworld-net-re.md — the authorities these names come from.
// [orig: S2C table g_np_msginfo_client @0x82AE28; C2S table g_np_msginfo_server @0x82B5D8]
//
// The high-table (settings) control tags H:0x00..0x03 / full tag 0x100..0x103 are NOT
// in-game msg ids and do not live here — they sit beside the full_tag machinery in
// npwire/protocol_message.h.

namespace opennova::s2c { // server -> client [orig: g_np_msginfo_client @0x82AE28]

inline constexpr uint8_t INIT = 0x00;                       // session-layer init
inline constexpr uint8_t SYNC_STATE = 0x01;                 // u32 sync state
inline constexpr uint8_t JOIN_PADDING_PROBE = 0x02;         // §5.55 pos ack; client echoes c2s::JOIN_PADDING_ECHO
inline constexpr uint8_t SYNC_TICK = 0x03;                  // u8 + 2xu16 sync tick
inline constexpr uint8_t SESSION_SLOT_CONFIG = 0x04;        // §5.53 maxPlayers -> slot-table realloc
inline constexpr uint8_t GAME_START_SIGNAL = 0x05;          // nonzero -> c2s::GAME_START_ACK + timer reset
inline constexpr uint8_t SESSION_CONFIG = 0x08;             // §5.54 51 B; gameType + flag bits
inline constexpr uint8_t PER_FRAME_UPDATE = 0x0A;           // §5.9 per-frame local-player + world-state update
inline constexpr uint8_t BMS_HEADER = 0x0B;                 // §5.4 616-byte mission header blob
inline constexpr uint8_t ENTITY_SPAWN_BATCH = 0x0C;         // §5.23 pool-0 organic spawn batch
inline constexpr uint8_t POOL_SPAWN = 0x0D;                 // §5.11 pool-1 entity spawn batch
inline constexpr uint8_t WORLD_STATE_LOAD = 0x0F;           // §5.29 world-state load
inline constexpr uint8_t STATIC_ENTITY_BATCH = 0x10;        // §5.9 pool-2 static entity batch
inline constexpr uint8_t DISCONNECT_UNLOCK = 0x11;          // len 0; WaitForDisconnect unlock
inline constexpr uint8_t ENTITY_DEATH = 0x13;               // §5.35 entity death
inline constexpr uint8_t CHAT_BROADCAST = 0x14;             // §5.52 fan-out of c2s::CHAT_MESSAGE
inline constexpr uint8_t PLAYER_LIST = 0x16;                // §5.20 player list
inline constexpr uint8_t FULL_ENTITY_SPAWN = 0x18;          // §5.46 reply to c2s::ENTITY_INFO_QUERY + vehicle-spawn broadcast
inline constexpr uint8_t SPAWN_ACK_TIMESTAMP = 0x19;        // ts ack of c2s::SPAWN_MENU_REQUEST
inline constexpr uint8_t WAIT_FOR_GAME_START_ACK = 0x1A;    // game-start gate
inline constexpr uint8_t NOOP = 0x1C;                       // empty stub
inline constexpr uint8_t SPAWN_SUCCESS_GATE = 0x1D;         // §5.2
inline constexpr uint8_t GAME_EVENT = 0x1E;                 // §5.26 game event
inline constexpr uint8_t POOL3_SYNC = 0x20;                 // §5.12 pool-3 marker/waypoint sync batch
inline constexpr uint8_t KILL_SYNC = 0x26;                  // §5.26 kill record
inline constexpr uint8_t CHAR_MINIMAP_UPDATE = 0x29;        // §5.59 NetId + CharacterEntity rebind
inline constexpr uint8_t CHAT_HISTORY = 0x2A;               // §5.35 chat history entry
inline constexpr uint8_t MISSION_MAP_NAMES = 0x2C;          // §5.51 server name + map file (join burst)
inline constexpr uint8_t ENTITY_CHECKSUM_REQ = 0x30;        // §5.35 -> c2s::ENTITY_CHECKSUM_REPLY
inline constexpr uint8_t LOADOUT_CRC_REQ = 0x31;            // §5.35 ammo-def CRC request -> c2s::CHECKSUM_REPLY
inline constexpr uint8_t PLAY_SOUND = 0x34;                 // §5.50 sound profile + optional 3D pos (retired misnomer: "GotoTeleport")
inline constexpr uint8_t CHARATTR_CRC_CHALLENGE = 0x39;     // §5.34 seed -> c2s::CHARATTR_CRC_REPLY
inline constexpr uint8_t ACK_STUB = 0x3E;                   // ack-style stub
inline constexpr uint8_t CAPTURE_ZONE_STATE = 0x40;         // §5.19 capture-zone overlay
inline constexpr uint8_t CHARATTR_PROPERTY_CLEAR = 0x41;    // clears a CharAttr property across the table
inline constexpr uint8_t INPUT_STATE_FLAGS = 0x42;          // §5.35 input state flags
inline constexpr uint8_t TIME_SYNC_PING = 0x43;             // §5.34 serverTs -> c2s::TIME_SYNC_REPLY
inline constexpr uint8_t ENTITY_ROUTED = 0x44;              // §5.36 entity-routed packet
inline constexpr uint8_t TERRAIN_LOAD = 0x45;               // §5.37 terrain load batch
inline constexpr uint8_t PLAYER_SYNC = 0x46;                // §5.21 player sync
inline constexpr uint8_t WEAPON_RELOAD = 0x49;              // §5.35 reload echo of c2s::WEAPON_RELOAD_REQUEST (retired misnomer: "camera_sync")
inline constexpr uint8_t TARGET_ASSIGNMENT = 0x4C;          // squad/AI order list
inline constexpr uint8_t SPAWN_SLOT_TIP = 0x4D;             // u8 slot tip
inline constexpr uint8_t KILL_BY_SLOT = 0x4E;               // §5.26 batch kill (retired misnomer: "BatchSpawn")
inline constexpr uint8_t TEAM_ASSIGN = 0x50;                // identity pair latch + team write
inline constexpr uint8_t TEAM_CHANGE_CONFIRM = 0x51;        // §5.59 team-change only, never on plain join (D-NET-148)
inline constexpr uint8_t ZONE_TIMER_WINDOW = 0x53;          // §5.49 capture/takeover HUD window
inline constexpr uint8_t RTT_ECHO = 0x57;                   // §5.34 rtt sample
inline constexpr uint8_t SESSION_STATUS = 0x58;             // §5.48 server/mission names + score rules (retired misnomer: "TerrainTexDef")
inline constexpr uint8_t DEPLOYED_ITEM = 0x59;              // §5.36 deployed-item spawn
inline constexpr uint8_t WEAPON_LOADOUT = 0x5A;             // §5.30 weapon loadout page
inline constexpr uint8_t EMPTY_SLOT_SWEEP = 0x5D;           // raw pool-0 index destroy list; reply to c2s::EMPTY_SLOT_SWEEP_REQUEST (D-NET-176)
inline constexpr uint8_t FILE_TRANSFER_CHUNK = 0x60;        // §5.28 chunked transfer, server-info channel; re-request c2s::FILE_CHUNK_REQUEST
inline constexpr uint8_t TICK_SEED = 0x61;                  // per-player clock anchor, fire-freshness floor (retired misnomer: "session key")
inline constexpr uint8_t MISSION_DATA_CHUNK = 0x64;         // §5.28 chunked transfer, mission-metadata channel; same wire shape as
                                                            // FILE_TRANSFER_CHUNK (one decoder serves both; identifiers split by the
                                                            // witnessed handler pair); re-request c2s::MISSION_CHUNK_REQUEST
inline constexpr uint8_t WEAPON_RESTRICTIONS = 0x66;        // count + (slot,restriction) pairs
inline constexpr uint8_t LOADED_MODEL_PAGE_REQUEST = 0x68;  // §5.34 cursor -> c2s::LOADED_MODEL_PAGE_REPLY
inline constexpr uint8_t MINIMAP_OVERLAY = 0x6B;            // §5.35 minimap overlay batch
inline constexpr uint8_t ROSTER_SYNC = 0x6E;                // §5.31 roster sync
inline constexpr uint8_t ZONE_TIMER_VALUE = 0x6F;           // §5.49 zone timer value (NOT cinematic camera)
inline constexpr uint8_t SPECTATOR_FLAGS = 0x75;            // 2 B death-screen/spectator state (distinct from SPECTATOR_FLAG 0x79)
inline constexpr uint8_t SERVER_TICK16 = 0x76;              // u16 server tick [orig: NetPacket_WriteServerTick16 @0x510350]
inline constexpr uint8_t SPECTATOR_FLAG = 0x79;             // §5.35 spectator flag (distinct from SPECTATOR_FLAGS 0x75)
inline constexpr uint8_t PLAYER_NAME = 0x7A;                // player name -> server-info struct
inline constexpr uint8_t FULL_PLAYER_INFO = 0x7B;           // §5.32 full player/session info
inline constexpr uint8_t SERVER_CONFIG_STRINGS = 0x7E;      // two cstrings
inline constexpr uint8_t SCORE_DELTA_SOUND = 0x81;          // i32 score; positive delta plays hit-confirm sound

} // namespace opennova::s2c

namespace opennova::c2s { // client -> server [orig: g_np_msginfo_server @0x82B5D8]

inline constexpr uint8_t JOIN = 0x00;                       // join request
inline constexpr uint8_t JOIN_FORM_POST = 0x01;             // §6.4 early-join side-password compare
inline constexpr uint8_t JOIN_PADDING_ECHO = 0x02;          // §5.55 reply to s2c::JOIN_PADDING_PROBE
inline constexpr uint8_t SET_PLAYER_VALUE = 0x03;           // i32 -> player entity field
inline constexpr uint8_t FIRED_ROUND = 0x06;                // §5.16 client fired round
inline constexpr uint8_t TIME_SYNC_REPLY = 0x08;            // §5.34 reply to s2c::TIME_SYNC_PING
inline constexpr uint8_t CHECKSUM_RESPONSE = 0x09;          // client checksum response
inline constexpr uint8_t SPAWN_MENU_REQUEST = 0x0A;         // len 0; game state 9 + s2c::SPAWN_ACK_TIMESTAMP
inline constexpr uint8_t MISSION_FILE_STATUS = 0x0B;        // §5.4 mission-file status report
inline constexpr uint8_t ENTITY_UPLINK = 0x0C;              // §5.10 player extended uplink
inline constexpr uint8_t CHAT_MESSAGE = 0x0D;               // §5.52 chat uplink -> s2c::CHAT_BROADCAST (retired misnomer: "replication-ack")
inline constexpr uint8_t RESPAWN_REQUEST = 0x0E;            // i16 spawnHandle deploy request (0xFFFE = auto team spawn)
inline constexpr uint8_t ENTITY_INFO_QUERY = 0x0F;          // §5.46 u16 handle self-heal -> s2c::FULL_ENTITY_SPAWN
inline constexpr uint8_t CHAT = 0x16;                       // text chat
inline constexpr uint8_t CHARATTR_CRC_REPLY = 0x1C;         // §5.34 reply to s2c::CHARATTR_CRC_CHALLENGE
inline constexpr uint8_t STANCE_CHANGE = 0x1D;              // i16 actionId from the stance key select
inline constexpr uint8_t ENTITY_CHECKSUM_REPLY = 0x20;      // §5.35 reply to s2c::ENTITY_CHECKSUM_REQ
inline constexpr uint8_t CHECKSUM_REPLY = 0x21;             // §5.17 client checksum reply
inline constexpr uint8_t PLAYER_SYNC_REQUEST = 0x22;        // §5.33 burst player-sync request
inline constexpr uint8_t VISIBLE_PLAYERS_REQUEST = 0x23;    // §5.33 burst visible-players request
inline constexpr uint8_t WEAPON_RELOAD_REQUEST = 0x25;      // §5.58 mid-game reload request; host broadcasts s2c::WEAPON_RELOAD
                                                            // (same 4-B body). Direction asymmetry vs S2C 0x25 — §5.3.
inline constexpr uint8_t VEHICLE_ATTACH_REQUEST = 0x26;     // word0 overwritten with requester's own handle
inline constexpr uint8_t VEHICLE_DETACH_REQUEST = 0x27;     // u16 handle detach
inline constexpr uint8_t LOADOUT_REQUEST = 0x28;            // §5.33 burst loadout request
inline constexpr uint8_t TEAM_SPAWN_ACK = 0x29;             // §5.59 deploy/team ack (D-NET-148)
inline constexpr uint8_t RTT_CONSUMED = 0x2C;               // §5.34 rtt sample
inline constexpr uint8_t BURST_MEMBER_2D = 0x2D;            // §5.33 burst receiver
inline constexpr uint8_t LOADOUT_SUBMIT = 0x2F;             // §5.56 team + class + weapon slot + ADM rows -> s2c::WEAPON_LOADOUT (D-NET-168)
inline constexpr uint8_t EMPTY_SLOT_SWEEP_REQUEST = 0x32;   // no fields; answered s2c::EMPTY_SLOT_SWEEP to the requester only
inline constexpr uint8_t FILE_CHUNK_REQUEST = 0x33;         // §5.28 [u32 id][u32 nextOffset] re-request for s2c::FILE_TRANSFER_CHUNK
inline constexpr uint8_t KEEPALIVE = 0x34;                  // §5.44 per-frame housekeeping keepalive
inline constexpr uint8_t MISSION_CHUNK_REQUEST = 0x37;      // §5.28 [u32 id][u32 nextOffset] re-request for s2c::MISSION_DATA_CHUNK
inline constexpr uint8_t LOADED_MODEL_PAGE_REPLY = 0x3D;    // §5.34 frozen loaded-model page reply to s2c::LOADED_MODEL_PAGE_REQUEST
inline constexpr uint8_t PING = 0x47;                       // re-broadcast request -> s2c::SPECTATOR_FLAGS
inline constexpr uint8_t CLIENT_ACK = 0x48;                 // 4 B read + discarded; server handler is an empty stub
inline constexpr uint8_t CLIENT_QUALITY = 0x4C;             // §5.33 burst client quality
inline constexpr uint8_t GAME_START_ACK = 0x4E;             // 4 B reply to s2c::GAME_START_SIGNAL

} // namespace opennova::c2s

#endif // OPENNOVA_NPWIRE_INGAME_MESSAGE_ID_H
