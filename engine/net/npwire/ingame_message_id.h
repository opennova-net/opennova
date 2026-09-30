#pragma once

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
// [orig: S2C table g_NPMsgInfoClient @0x82AE28; C2S table g_NPMsgInfoServer @0x82B5D8]
//
// The high-table (settings) control tags H:0x00..0x03 / full tag 0x100..0x103 are NOT
// in-game msg ids and do not live here — they sit beside the full_tag machinery in
// npwire/protocol_message.h.

namespace opennova::s2c { // server -> client [orig: g_NPMsgInfoClient @0x82AE28]

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
inline constexpr uint8_t ENTITY_REMOVE = 0x12;              // entity expiry/removal
inline constexpr uint8_t ENTITY_DEATH = 0x13;               // §5.35 entity death
inline constexpr uint8_t CHAT_BROADCAST = 0x14;             // §5.52 fan-out of c2s::CHAT_MESSAGE
inline constexpr uint8_t PLAYER_LIST = 0x16;                // §5.20 player list
inline constexpr uint8_t FULL_ENTITY_SPAWN = 0x18;          // §5.46 reply to c2s::ENTITY_INFO_QUERY + vehicle-spawn broadcast
inline constexpr uint8_t SPAWN_ACK_TIMESTAMP = 0x19;        // ts ack of c2s::SPAWN_MENU_REQUEST
inline constexpr uint8_t WAIT_FOR_GAME_START_ACK = 0x1A;    // game-start gate
inline constexpr uint8_t NOOP = 0x1C;                       // empty stub
inline constexpr uint8_t END_ROUND_HEADER = 0x1D;           // §5.68 fixed 7-B scoreboard header; triggers c2s::END_ROUND_STATS_REQUEST
inline constexpr uint8_t GAME_EVENT = 0x1E;                 // §5.26 game event
inline constexpr uint8_t POOL3_SYNC = 0x20;                 // §5.12 pool-3 marker/waypoint sync batch
inline constexpr uint8_t SCRIPT_REMOTE_COMMAND = 0x23;      // WAC command replicated by the host VM (registry flags 0x18): GameMode_DispatchRemoteCommand
inline constexpr uint8_t TEXT_COMMAND = 0x24;               // NapiNPClientMsg_HandleTextCommand
inline constexpr uint8_t KILL_SYNC = 0x26;                  // §5.26 kill record
inline constexpr uint8_t CHAR_MINIMAP_UPDATE = 0x29;        // §5.59 NetId + CharacterEntity rebind
inline constexpr uint8_t CHAT_HISTORY = 0x2A;               // §5.35 chat history entry
inline constexpr uint8_t MISSION_MAP_NAMES = 0x2C;          // §5.51 server name + map file (join burst)
inline constexpr uint8_t OBJECTIVE_ENTITY_STATE = 0x2F;     // flag/carryable pose + occupant + ground links
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
inline constexpr uint8_t VISIBLE_PLAYERS = 0x4C;            // the player-slot pointer table snapshot (retired misnomer: "target assignment")
inline constexpr uint8_t SPAWN_SLOT_NOTICE = 0x4D;          // u8 slot: a player joined; others re-request 0x22 + 0x23
inline constexpr uint8_t KILL_BY_SLOT = 0x4E;               // §5.26 paginated join-window kill list [u16 resume][u16 slot...]; any slot -> c2s::LOADOUT_REQUEST continuation (retired misnomer: "BatchSpawn")
inline constexpr uint8_t TEAM_ASSIGN = 0x50;                // identity pair latch + team write
inline constexpr uint8_t TEAM_CHANGE_CONFIRM = 0x51;        // §5.59 team-change only, never on plain join (D-NET-148)
inline constexpr uint8_t DEATH_CAMERA_TARGET = 0x52;        // victim-only [3xi32] killer/death position
inline constexpr uint8_t ZONE_TIMER_WINDOW = 0x53;          // §5.49 capture/takeover HUD window
inline constexpr uint8_t PLAYER_DOWNED_STATE = 0x54;        // [u16 entity][u8 revive seconds | request bit]
inline constexpr uint8_t END_ROUND_STATS = 0x56;            // §5.68 end-of-round stat board, pulled in 200-B chunks over C2S 0x2B (the stat.mnu table source)
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
inline constexpr uint8_t ZONE_PRESENCE_COUNT = 0x6C;        // §5.61 [u16 active-zone handle][u8 unique presence]
inline constexpr uint8_t SPAWN_WAVE_STATUS = 0x6E;          // §5.31 deploy-screen wave groups
inline constexpr uint8_t ZONE_TIMER_VALUE = 0x6F;           // §5.49 zone timer value (NOT cinematic camera)
inline constexpr uint8_t SPECTATOR_FLAGS = 0x75;            // 2 B [death-screen/spectator bit, player team]
inline constexpr uint8_t CLASS_ALLOW_MASK = 0x76;           // u16 g_HostClassAllowMask [orig: NetPacket_WriteClassAllowMask @0x510350]
inline constexpr uint8_t NETWORK_QUALITY = 0x79;             // host CNetQuality scalar, broadcast every 0x136 ticks
inline constexpr uint8_t PLAYER_NAME = 0x7A;                // player name -> server-info struct
inline constexpr uint8_t FULL_PLAYER_INFO = 0x7B;           // §5.32 full player/session info
inline constexpr uint8_t SERVER_CONFIG_STRINGS = 0x7E;      // two cstrings
inline constexpr uint8_t SCORE_DELTA_SOUND = 0x81;          // i32 score; positive delta plays hit-confirm sound


// Remaining retail dispatch rows; meanings and witnesses live in the catalog.
inline constexpr uint8_t CLIENT_CONSOLE_COMMAND = 0x06;
inline constexpr uint8_t RESERVED_NOOP_07 = 0x07;
inline constexpr uint8_t ENTITY_SLOT_MESSAGE = 0x17;
inline constexpr uint8_t NETWORK_DELAY = 0x1B;
inline constexpr uint8_t COUNTDOWN_SYNC = 0x1F;
inline constexpr uint8_t EXPLOSION_EFFECT = 0x21;
inline constexpr uint8_t ENTITY_CREATE = 0x22;
inline constexpr uint8_t GAME_RESET = 0x25;
inline constexpr uint8_t SPAWN_EFFECT = 0x27;
inline constexpr uint8_t DIALOG_PLAYER_NAME = 0x28;
inline constexpr uint8_t EXIT_SESSION = 0x2B;
inline constexpr uint8_t EMOTE_BROADCAST = 0x2D;            // emote + sender pool-0 index, to the players near the sender
inline constexpr uint8_t FORM_FIELD = 0x2E;
inline constexpr uint8_t FORMATTED_GAME_TEXT = 0x32;
inline constexpr uint8_t WAYPOINT_CREATE = 0x33;
inline constexpr uint8_t PLAYER_ACTION = 0x35;
inline constexpr uint8_t VEHICLE_SPAWN_NOTIFY = 0x36;
// Door-row sync: [u16 handle][i16 state][u8 number] (5 B). The IDB names it a
// "weapon slot action"; the rows it writes are the 0xA8A418 door records
// (docs/world/world-wac-ai-re.md §33.14). Emitted by the authority on every
// row completion and per selected section of a door command, and as the
// requester-only reply to c2s::DOOR_SLOT_REQUEST.
// [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x431250; senders
//  Server_SendWeaponSlotActionPacket @0x50F9A0, the 0x1A reply @0x514c74]
inline constexpr uint8_t DOOR_SLOT_ACTION = 0x37;
inline constexpr uint8_t WEAPON_SWITCH = 0x38;
// A medic has started reviving the recipient: the handler latches the local
// entity's +0x1E0 "being revived" word, draws the default progress bar and
// starts the player's ambient sound; empty body. The latch hides the DEATH
// screen's MEDIC/CALLMEDIC statics and blocks a second medic until the local
// respawn (Game_InitNewRound @0x422740) or mission start clears it.
// [orig: NapiNPClientMsg_0x03A @0x422680; host sender @0x517cd0 stamps the
//  victim's +0x1E0 = 1; reader GameEvent_HandleMedicInteraction @0x4e6790]
inline constexpr uint8_t MEDIC_REVIVING = 0x3A;
inline constexpr uint8_t FORM_POST_REQUEST = 0x3B;
inline constexpr uint8_t EXIT_SESSION_ALT = 0x3D;
inline constexpr uint8_t OBJECTIVE_NOTIFICATION = 0x3F;
inline constexpr uint8_t TARGET_LIST = 0x48;
inline constexpr uint8_t TARGET_LIST_PAGE = 0x4F;
inline constexpr uint8_t WORLD_SYNC_REQUEST = 0x5B;
inline constexpr uint8_t LOAD_SAVED_GAME = 0x5C;
inline constexpr uint8_t RESERVED_NOOP_5E = 0x5E;
inline constexpr uint8_t RESERVED_NOOP_5F = 0x5F;
inline constexpr uint8_t MEMORY_CRC_CHALLENGE = 0x62;
inline constexpr uint8_t POOF_TOGGLE = 0x63;
inline constexpr uint8_t RESERVED_NOOP_65 = 0x65;
inline constexpr uint8_t TELEPORT = 0x67;
// NovaWorld clan roster: [u8 action][u32 accountNetId] + (actions 1/3)
// [cstr name <=64][cstr tag <=8]; action 2 removes. Action 3 is the reply to
// the c2s::GAME_START_ACK roster walk and re-queues it with the node's id.
// [orig: NapiNPClientMsg_HandlePlayerJoinLeave @0x432510; serializer
//  NetPacket_SerializeMinimapSlot @0x5073B0]
inline constexpr uint8_t CLAN_ROSTER = 0x6A;
inline constexpr uint8_t TRACKED_PLAYER_VOICE = 0x6D;
// Per-vehicle-type spawn availability for the requester's team, from the
// host's EntityLimit table: [u8 3] + rows [u16 typeId][u8 avail][u8 max] +
// u16 0. Requester-only reply to c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST.
// [orig: NapiNPClientMsg_HandleWeaponLoadoutList @0x429A30;
//  NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0]
inline constexpr uint8_t VEHICLE_SPAWN_AVAILABILITY = 0x70;
// The command map's squad legs (net/npwire/squad_messages.h): a member's
// leader, an order line, a member's fireteam, a recruit notice, a go code.
// [orig: NapiNPClientMsg_HandleSquadJoin @0x425600, NapiNPClientMsg_0x072
//  @0x425710, NapiNPClientMsg_0x073 @0x425770, NapiNPClientMsg_PlayerRecruited
//  @0x4258b0, NapiNPClientMsg_0x078 @0x425970]
inline constexpr uint8_t SQUAD_JOIN = 0x71;
inline constexpr uint8_t SQUAD_ORDER = 0x72;
inline constexpr uint8_t FIRETEAM_SET = 0x73;
inline constexpr uint8_t SQUAD_RECRUITED = 0x74;
inline constexpr uint8_t GO_CODE = 0x78;
inline constexpr uint8_t DESTROY_ENTITY = 0x7C;
inline constexpr uint8_t CLIENT_METRICS_REQUEST = 0x7D;
inline constexpr uint8_t RESERVED_NOOP_7F = 0x7F;
inline constexpr uint8_t SCORE_TRACKER_RESET = 0x80;
inline constexpr uint8_t SCORE_TRACKER_TIME = 0x82;
inline constexpr uint8_t PLAYER_PROFILE_REFRESH = 0x83;

} // namespace opennova::s2c

namespace opennova::c2s { // client -> server [orig: g_NPMsgInfoServer @0x82B5D8]

inline constexpr uint8_t JOIN = 0x00;                       // join request
inline constexpr uint8_t JOIN_FORM_POST = 0x01;             // §6.4 early-join side-password compare
inline constexpr uint8_t JOIN_PADDING_ECHO = 0x02;          // §5.55 reply to s2c::JOIN_PADDING_PROBE
inline constexpr uint8_t AUTO_MEDIC_PREFERENCE = 0x03;      // i32: zero enables automatic medic requests
inline constexpr uint8_t FIRED_ROUND = 0x06;                // §5.16 client fired round
inline constexpr uint8_t TIME_SYNC_REPLY = 0x08;            // §5.34 reply to s2c::TIME_SYNC_PING
inline constexpr uint8_t CHECKSUM_RESPONSE = 0x09;          // client checksum response
inline constexpr uint8_t SPAWN_MENU_REQUEST = 0x0A;         // len 0; game state 9 + s2c::SPAWN_ACK_TIMESTAMP
inline constexpr uint8_t MISSION_FILE_STATUS = 0x0B;        // §5.4 mission-file status report
inline constexpr uint8_t ENTITY_UPLINK = 0x0C;              // §5.10 player extended uplink
inline constexpr uint8_t CHAT_MESSAGE = 0x0D;               // §5.52 chat uplink -> s2c::CHAT_BROADCAST (retired misnomer: "replication-ack")
inline constexpr uint8_t RESPAWN_REQUEST = 0x0E;            // i16 spawnHandle deploy request (0xFFFE = auto team spawn)
inline constexpr uint8_t ENTITY_INFO_QUERY = 0x0F;          // §5.46 u16 handle self-heal -> s2c::FULL_ENTITY_SPAWN
inline constexpr uint8_t MOUNTED_WEAPON_SLOT_SELECT = 0x16; // action-6 bool-as-i16: 0 child MountSlot, nonzero groundEntity vehicle slot
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
inline constexpr uint8_t LOADOUT_REQUEST = 0x28;            // §5.33 join-window kill-list request [u32 windowMin][u32 windowMax][u16 start] -> s2c::KILL_BY_SLOT page (0x0F burst + 0x4E continuation; IDB misnomer "weapon loadout")
inline constexpr uint8_t TEAM_SPAWN_ACK = 0x29;             // §5.59 deploy/team ack (D-NET-148)
inline constexpr uint8_t END_ROUND_STATS_REQUEST = 0x2B;    // §5.68 [u16 nextOffset] -> s2c::END_ROUND_STATS
inline constexpr uint8_t RTT_CONSUMED = 0x2C;               // §5.34 rtt sample
inline constexpr uint8_t BURST_MEMBER_2D = 0x2D;            // §5.33 burst receiver
inline constexpr uint8_t MEDIC_REQUEST = 0x2E;              // [u32 entityIndex] a downed player's manual medic call -> host 0x54/0x14/0x34 fan (§5.60, D-NET-108)
inline constexpr uint8_t LOADOUT_SUBMIT = 0x2F;             // §5.56 team + class + weapon slot + ADM rows -> s2c::WEAPON_LOADOUT (D-NET-168)
inline constexpr uint8_t EMPTY_SLOT_SWEEP_REQUEST = 0x32;   // no fields; answered s2c::EMPTY_SLOT_SWEEP to the requester only
inline constexpr uint8_t FILE_CHUNK_REQUEST = 0x33;         // §5.28 [u32 id][u32 nextOffset] re-request for s2c::FILE_TRANSFER_CHUNK
inline constexpr uint8_t KEEPALIVE = 0x34;                  // §5.44 per-frame housekeeping keepalive
inline constexpr uint8_t MISSION_CHUNK_REQUEST = 0x37;      // §5.28 [u32 id][u32 nextOffset] re-request for s2c::MISSION_DATA_CHUNK
inline constexpr uint8_t LOADED_MODEL_PAGE_REPLY = 0x3D;    // §5.34 frozen loaded-model page reply to s2c::LOADED_MODEL_PAGE_REQUEST
inline constexpr uint8_t PING = 0x47;                       // re-broadcast request -> s2c::SPECTATOR_FLAGS
inline constexpr uint8_t CLIENT_ACK = 0x48;                 // 4 B read + discarded; server handler is an empty stub
inline constexpr uint8_t CLIENT_QUALITY = 0x4C;             // §5.33 burst client quality
inline constexpr uint8_t GAME_START_ACK = 0x4E;             // clan-roster walk [u32 afterNetId]: {0} kick on s2c::GAME_START_SIGNAL @0x42e1d9, {netId} continuation on s2c::CLAN_ROSTER action 3 @0x43266c (misnomer; the joiner caller still spells it)


// Remaining retail dispatch rows; meanings and witnesses live in the catalog.
inline constexpr uint8_t ADMIN_NETLOG_COMMAND = 0x04;
inline constexpr uint8_t RESERVED_NOOP_07 = 0x07;
inline constexpr uint8_t SECTOR_ACTION = 0x13;
inline constexpr uint8_t EMOTE_REQUEST = 0x14;              // i16 Emotes-menu digit -> s2c::EMOTE_BROADCAST (retired misnomer: "object sound")
// A user waypoint shared to the sender's subordinates (target 0xFF) or one
// squad joiner (squad_messages.h WaypointShare).
// [orig: NetPacket_SendChatMessage @0x42DDC0 (a misnomer) ->
//  NapiNPServerMsg_HandleChatOrWhisper @0x514850 (a misnomer)]
inline constexpr uint8_t WAYPOINT_SHARE = 0x17;
inline constexpr uint8_t WEAPON_SPAWN = 0x18;
inline constexpr uint8_t ENTITY_REMOVE_REQUEST = 0x19;
// A non-authority's door-row request: the same [u16 handle][i16 state][u8 number]
// body as s2c::DOOR_SLOT_ACTION, queued reliable per selected section of a
// door command; the host answers s2c::DOOR_SLOT_ACTION to the requester only.
// [orig: NetPacket_SendWeaponSwitch @0x42D0C0 -> NapiNPServerMsg_HandleVoteUpdate
//  @0x514B20 (both IDB misnomers)]
inline constexpr uint8_t DOOR_SLOT_REQUEST = 0x1A;
inline constexpr uint8_t PLAYER_CLASS_SELECT = 0x1B;
inline constexpr uint8_t BAN_PUNT_COMMAND = 0x24;
inline constexpr uint8_t LOAD_SAVED_GAME_REQUEST = 0x30;
inline constexpr uint8_t RESERVED_NOOP_31 = 0x31;
inline constexpr uint8_t MEMORY_CRC_REPLY = 0x35;
inline constexpr uint8_t RESERVED_NOOP_36 = 0x36;
inline constexpr uint8_t RESERVED_NOOP_38 = 0x38;
inline constexpr uint8_t RESERVED_NOOP_39 = 0x39;
inline constexpr uint8_t CLIENT_CRC_VALIDATION = 0x3C;
inline constexpr uint8_t RESERVED_NOOP_3E = 0x3E;
// The PLAYERS tab's punt vote [u8 target]. [orig: NapiNPServerMsg_VoteKick @0x518F10]
inline constexpr uint8_t PUNT_VOTE = 0x3F;
// Vehicle-spawn pick: [u16 sourceHandle][u8 typeIndex] (3 B; the type index is
// the source def's pcvehicle_spawnlist bit). [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0]
inline constexpr uint8_t VEHICLE_SPAWN_REQUEST = 0x40;
inline constexpr uint8_t DEATH_TIMEOUT_RESET = 0x41;
// No fields read; the host answers s2c::VEHICLE_SPAWN_AVAILABILITY to the requester.
// [orig: NapiNPServerMsg_SendWeaponSlotStates @0x510930]
inline constexpr uint8_t VEHICLE_SPAWN_AVAILABILITY_REQUEST = 0x42;
// The command map's squad requests (net/npwire/squad_messages.h): join a
// leader, order subordinates, assign a fireteam, recruit a player.
// [orig: Server_HandleEntitySync @0x510990, NapiNPServerMsg_HandleChatBroadcast
//  @0x510AE0, NapiNPServerMsg_0x045_HandleTeamAssignment @0x510C00,
//  NapiNPServerMsg_HandleVoteKick @0x510D20 — all IDB misnomers]
inline constexpr uint8_t SQUAD_JOIN_REQUEST = 0x43;
inline constexpr uint8_t SQUAD_ORDER_REQUEST = 0x44;
inline constexpr uint8_t FIRETEAM_ASSIGN = 0x45;
inline constexpr uint8_t SQUAD_RECRUIT = 0x46;
inline constexpr uint8_t PLAYER_PROFILE = 0x49;
// A leader's go code [u8 leader][u8 code].
// [orig: NapiNPServer_BroadcastPlayerProfileUpdate @0x510DC0 (a misnomer)]
inline constexpr uint8_t GO_CODE = 0x4B;
inline constexpr uint8_t TEAM_CHANGE_REQUEST = 0x4D;
// A user waypoint deleted [u16 handle]. [orig: NapiNPServerMsg_0x04F @0x514A40]
inline constexpr uint8_t WAYPOINT_DELETE = 0x4F;
inline constexpr uint8_t CLIENT_METRICS = 0x50;
inline constexpr uint8_t SPECTATOR_RESPAWN = 0x51;

} // namespace opennova::c2s
