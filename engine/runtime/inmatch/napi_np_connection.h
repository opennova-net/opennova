#pragma once

#include <array>
#include <base/io/tick_rate.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <runtime/replication/connection.h>             // replication::Connection, replication::TransportMode
#include <runtime/inmatch/session_timeout_config.h>     // SessionTimeoutConfig (the cs_dir template copy)
#include <net/npwire/peer_addr.h> // opennova::PeerAddr (the transport-addr key)
#include <net/npwire/protocol_message.h> // opennova::SessionSequencing (the per-connection seq/ack, ADR 0013)
#include <net/npwire/session_hello.h>    // opennova::DisconnectEvent (the latched disconnect record)

namespace opennova::inmatch {

// dcb (ConnectionId) reservation on a LAN listen server. The host's loopback client consumes
// ConnectionId 0/1; the host's own PLAYER is dcb 2; remote joiners are assigned from 3 up. A
// 0x14B9 organic with entity+0x78 == 0 is the dedicated-server reservation, which makes a joining
// client keep no player entity for that slot and flood C2S 0x0F → 0xC9 disconnect — so the host
// player MUST carry a non-zero dcb. Wire-proven by the golden host_and_join_lan.pcapng (host player
// eFlags=2, first joiner 3, AI 0). [orig: NapiNP_GetLocalConnectionId @0x4c6d40; Server_PlayerAdd
// @0x51cbc0 writes entity+0x78 = conn->connection_id; net-re §5.2a/§5.2b]
inline constexpr uint32_t kHostPlayerDcb = 2;
inline constexpr uint32_t kFirstJoinerDcb = kHostPlayerDcb + 1;

// The protocol callbacks that raise the connection indicators' link-error
// flags (hud/net_quality_indicators.h), as a mask a connection latches for
// its owner: a resend list that named a sequence (the peer missed ours: flag
// 1, outgoing) and a missing-sequence request that named one (we missed the
// peer's: flag 2, incoming). Zero-only and key-only lists raise nothing.
// [orig: NapiNP_HandleResendList @0x6239ef..0x623a37 (cb_server_6 /
//  cb_client_3); CNapiNPConnection_SendMissingSeqList @0x623780..0x6237bd
//  (cb_server_5 / cb_client_2); the callbacks CNapiNetwork_Init installs
//  @0x4ca948..0x4ca9cd: sub_4C62A0 and Network_LogOutgoingPacketError raise
//  flag 1, @0x4c4681 and Network_LogIncomingPacketError flag 2]
inline constexpr uint32_t kNetQualityLinkErrorOutgoing = 1u;
inline constexpr uint32_t kNetQualityLinkErrorIncoming = 2u;

// Deterministic GetTickCount seam for the authoritative 62 Hz owner. Retail's
// time-sync validator compares only unsigned deltas, so a nonzero logical base
// preserves its clock contract without introducing wall-time into native tests.
inline uint32_t host_milliseconds_for_logic_tick(uint32_t logic_tick) {
	return static_cast<uint32_t>(
			1ull + (static_cast<uint64_t>(logic_tick) * 1000ull) /
					static_cast<uint64_t>(io::kTicksPerSecondInt));
}

// Retail's Joint Operations connection template bounds the reliable outbound-message pool at
// 0x4B0 records.
// NapiNPMessage_Create rejects a new record when pending + retained + 1 exceeds this field; admitted
// peer ACKs retire retained records. Keep this opt-in at the JO in-match connection seam. The
// NOVAWORLDUDP lobby ClientSession has no witnessed 0x44/0x84 owner in this slice and remains on the
// generic, recovery-disabled SessionSequencing behavior.
// [orig: CNapiNetwork_Init @0x4CAB20/@0x4CABF0 writes cs_dir0.msg_out_max = 0x4B0;
// NapiNPMessage_Create @0x627FC0 checks the combined count]
inline constexpr std::size_t JO_GAME_SESSION_OUTBOUND_MESSAGE_MAX =
		JO_SESSION_OUTBOUND_MESSAGE_MAX;

// Both JO game-session direction profiles reap a connection after 120000 ms without receive
// activity. The client keepalive interval is deliberately shorter (30000 ms). This is the
// DEFAULT of SessionTimeoutConfig::timeout_ms; a loose `_NSTMOUT.TXT` overrides it at init.
// [orig: CNapiNetwork_Init @0x4caa81/@0x4cab54]
inline constexpr uint32_t JO_GAME_SESSION_TIMEOUT_MS = 120000;

// cs_dir0.recv_max_per_tick (CS field 1) = 4 for the JOINTOPERATIONS template; the teardown of an
// active connection sends its disconnect packet up to this many times, clamped to [0, 32].
// [orig: CNapiNetwork_Init @0x4cab60; CNapiNPConnection_TeardownActiveConnection @0x6253C0 —
//  the clamp @0x6253ef..0x625403 and the send loop @0x625406..0x625424 (state 1),
//  @0x62549e..0x6254d3 (state 5)]
inline constexpr uint32_t JO_GAME_SESSION_RECV_MAX_PER_TICK = 4;
inline constexpr std::size_t disconnect_burst_count() {
	return JO_GAME_SESSION_RECV_MAX_PER_TICK > 32u ? 32u : JO_GAME_SESSION_RECV_MAX_PER_TICK;
}

// A negative cs_dir msg_out_max (the `_NSTMOUT.TXT` NEVER form) is unbounded: NapiNPMessage_Create
// only checks the pool when `msg_out_max >= 0` [orig: @0x628048], and zero is
// session_outbound_message_prefix_count's unbounded sentinel.
inline std::size_t outbound_message_limit_for(int32_t msg_out_max) {
	return msg_out_max < 0 ? std::size_t{0} : static_cast<std::size_t>(msg_out_max);
}

inline SessionSequencing make_jo_game_session_sequencing(
		uint32_t next_outbound_seq = 1, uint32_t last_inbound_seq = 0,
		int32_t msg_out_max = static_cast<int32_t>(JO_GAME_SESSION_OUTBOUND_MESSAGE_MAX)) {
	SessionSequencing sequencing;
	sequencing.next_outbound_seq = next_outbound_seq;
	sequencing.last_inbound_seq = last_inbound_seq;
	sequencing.ordered_recovery_enabled = true;
	sequencing.outbound_message_limit = outbound_message_limit_for(msg_out_max);
	return sequencing;
}

// The per-connection lifecycle phase a server-side node walks from a fresh datagram to an
// in-match player (§5.0 / §5.2a). Names mirror the witnessed original flow:
//
//   0x41 [NapiNPProtocol_HandleClientHello @0x6213b0] -> emit 0x81 statelessly
//   New -> ValidateJoin [CNapiNetwork_ValidateJoinRequest @0x4c61b0]
//       -> Joined        0x42 [NapiNPProtocol_HandleClientJoin  @0x62b750] -> emit 0x82 (SCRK)
//       -> NewConnection      [NapiNPServer_HandleNewConnection  @0x4c8040]
//       -> InitRound          [Server_InitNewRoundState          @0x51c8e0]
//       -> PendingSpawn       [CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]
//       -> PlayerAdded        [Server_BuildPlayerInfoAndAdd @0x51d560 -> Server_PlayerAdd @0x51cbc0]
//       -> SendingInitial     [Server_SendInitialGameStateToPlayer @0x51bba0]
//       -> Spawned            (spawn_remote_player binds owned_entity)
//       -> InMatch            (per-frame 0x0A out / 0x0C in)
//       -> Goodbye       0x46 [Nwu_HandleClientGoodbye @0x624250]
//
// The SCRK/seq/ack state currently in HostSessionAccept::PeerState folds onto this node in P2.
enum class ConnectionPhase : uint8_t {
	New = 0,
	ValidateJoin,
	Joined,
	NewConnection,
	InitRound,
	PendingSpawn,
	PlayerAdded,
	SendingInitial,
	Spawned,
	InMatch,
	Goodbye,
};

// The three-turn game admission exchange after 0x42/0x82 key setup. A fresh remote connection may
// decrypt SESSION traffic at AwaitJoinRequest, but it is not a player yet: only the witnessed
// 0x00 JOIN -> 0x01 form post -> 0x02 challenge echo sequence makes it spawn-eligible. `Complete`
// is the default for type-2 loopback/synthetic connections, which do not traverse the UDP join FSM.
// Rejected is a transient signal to the protocol owner to tear the pending connection down.
// [wire: retail LAN frames 8-13; orig: NapiNPServerMsg_0x000/001/002
// @0x512AA0/@0x512ED0/@0x512FD0]
enum class GameAdmissionStage : uint8_t {
	Complete = 0,
	AwaitJoinRequest,
	AwaitFormPost,
	AwaitPaddingEcho,
	Rejected,
};

// The §5.2a initial-state burst cursor for one connection — Server_SendInitialGameStateToPlayer's
// per-player phase counters, folded onto the connection node like P2 folded PeerState. After P3 this
// is the SINGLE authority for the connection's spawn-gate progress (the F3 / PeerSpawned latches read
// entity_batch_count / spawned here, no longer the game_runtime GameSessionState).
// [orig: the playerSlot+0x20 sync-state + playerSlot+89878/89882/89884 phase counters; net-re §5.2a]
struct InitialStateBurst {
	uint8_t  sync_state = 0;            // [playerSlot+0x20] 0 idle / 2 player-sync / 3 WAIT for the
	                                    // client's C2S 0x0A spawn-menu request / 4 world-stream / 5 done.
	                                    // State 3 is the load gate: the player-sync tail parks here
	                                    // [orig: @0x51c134] and ONLY NapiNPServerMsg_HandlePlayerSpawnRequest
	                                    // @0x513260 (C2S 0x0A) advances 3 -> 4 — the world stream never
	                                    // races a client that hasn't asked for it (D-NET-150).
	// The player-sync tags, one per subphase, and the world-stream phases
	// (server_initial_state.cpp walks both) [orig: playerSlot+89878/+89882].
	enum PlayerSyncSubphase : uint16_t {
		kSyncDone = 0,
		kSyncMissionMapNames = 8,
		kSyncSessionConfig = 9,
		kSyncChatHistoryFirst = 10,
		kSyncChatHistoryLast = 15,
		kSyncNoop = 16,
		kSyncBmsHeader = 17,
		kSyncWeaponRestrictions = 18,
		kSyncClassAllowMask = 19,
		kSyncDisconnectUnlock = 20,
	};
	enum WorldStreamPhase : uint8_t {
		kStreamInit = 0,
		kStreamPool2Static = 1,
		kStreamPool1Items = 2,
		kStreamPool0Organics = 3,
		kStreamPool3Markers = 4,
		kStreamTerrainTiles = 5,
		kStreamMissionText = 6,
		kStreamWaitForGameStartAck = 7,
		kStreamGameStartBundle = 8,
	};
	uint16_t player_sync_subphase = kSyncMissionMapNames;  // [playerSlot+89878] 8..20 (one tag each; see server_initial_state)
	uint8_t  world_stream_phase = kStreamInit;    // [playerSlot+89882] 0 phase-0 init [orig: @0x51bc1a case 0], 1=0x10 .. 7=0x1A
	uint16_t phase_loop_counter = 0;    // [playerSlot+89884] per-phase record cursor (reserved; paging)
	uint8_t  game_state = 0;            // [CNetPlayer_SetGameState] 8 player-added / 9 in-game
	uint32_t entity_batch_count = 0;    // world-stream batches emitted — the F3 readiness signal
	bool     loadout_received = false;  // C2S 0x2F (loadout request) received — gates the game-start
	                                    // bundle (phase 8). Golden: 0x1A (end of world-stream, f316) ->
	                                    // client sends 0x2F (f317) -> server replies 0x5A + game-start
	                                    // (f318). Without this gate, 0x1A and game-start land in the
	                                    // same datagram batch and the client never gets to send 0x2F.
	// (the prior loadout_wait_ticks fallback counter was removed — the phase-8 wait has NO
	//  timeout: the golden host emits nothing in-match until the joiner's 0x2F, D-NET-145)
	bool     spawned = false;           // set with game_state==9 (the PeerSpawned source)
};

// The joiner's pose cached from its pre-spawn C2S 0x0C uplink — the host's fallback spawn/pose
// while no World entity is bound yet. Valid ONLY until the spawn pipeline binds the connection's
// owned_entity; pose_for_conn then prefers the live registry Entity. Consolidated from the loose
// SessionReplyState.client_* fields so the pre-spawn fallback is one named unit distinct from the
// live World binding (owned_entity). [orig: NapiNPServerMsg_0x00C @0x501c30 caches the uplink into
// the player slot; the pose fallback is HostSessionAccept::pose_from_session]
struct PreSpawnJoinerPose {
	bool valid = false;                // a pre-spawn C2S 0x0C has been cached
	uint16_t entity_handle = 0;
	uint16_t item_type_id = 0;
	uint16_t carrier_handle = 0xFFFF;
	uint32_t pos_x = 0;
	uint32_t pos_y = 0;
	uint32_t pos_z = 0;
	int16_t heading = 0;
	int16_t pitch = 0;
};

// The joiner's character/profile join vars, uploaded as CU chunks in its game-session 0x42
// ClientAuth: the per-SIDE character selection (side A = teams 1/3, side B = teams 2/4).
//   CI0/CI1 = per-side minimap/character-slot ids (u16 truncation of retail's atol),
//   TR      = requested side (0 = A, 1 = B; anything else clamps to 0xFF = auto),
//   CTA/CTB = per-side soldier class, VCA/VCB = per-side avatar byte (-> entity+0x374 animSlot).
// 0 everywhere = "tag absent" (retail's zero-initialized NapiNetConfig). The player add picks the
// side by the ASSIGNED team and stamps the entity. [orig: client emit
// CNapiServerInfo_SerializeToSession @0x4c3650; host parse NapiNPProtocol_HandleClientJoin
// @0x62b750 CU loop -> NapiNetConfig_LoadFromConnTags @0x4c7260 (jsp[56..63] + ci0.lo); consume
// Server_PlayerAdd @0x51cbc0. Wire example: golden retail-ashi5a 0x42 f=199140 carries CI0=512
// CI1=33287 TR=-1 CTA=CTB=8 VCA=1 VCB=4 — profile-derived values, not protocol defaults; the
// joiner's 0x0C record echoes 0x8207/4. net-re D-NET-146]
struct CharacterJoinVars {
	uint16_t char_id[2] = {0, 0};   // CI0 / CI1 -> jsp[56] / jsp[58]
	uint8_t team_request = 0;       // TR -> jsp[60] (retail clamp: != 0xFF && >= 2 -> 0xFF)
	uint8_t char_class[2] = {0, 0}; // CTA / CTB -> jsp[61] / jsp[62]
	uint8_t avatar[2] = {0, 0};     // VCA / VCB -> jsp[63] / ci0 low byte
};

// The stock-Avatars.def FRESH-PROFILE character vars — what a retail client with
// no saved selection uploads, and what the listen host's own local player carries
// before the shell installs the mounted profile: each side's first AvatarDefs entry
// of the matching alignment, packed {nat=0,div=0,combo=1,good}=0x0200 and
// {nat=7,div=0,combo=1,evil}=0x8207, class 8 (rifleman) on both sides, the two
// selected heads' voice/avatar bytes 1/10, and no side request (TR 0xFF).
// [orig: PlayerProfile_InitDefaults @0x54bbe0..0x54bc24 -> EntitySlot_LookupAndPackEntry
//  @0x57ad40; the avatar byte via Avatars_ResolveSelectionIndex (ex sub_57AE60) @0x57ae60; wire: retail join f=199140 (the
//  captured VCB=4 is a saved profile override, not the fresh default)]
inline CharacterJoinVars retail_fresh_profile_character_vars() {
	CharacterJoinVars vars;
	vars.char_id[0] = 0x0200;
	vars.char_id[1] = 0x8207;
	vars.team_request = 0xFF;
	vars.char_class[0] = 8;
	vars.char_class[1] = 8;
	vars.avatar[0] = 1;
	vars.avatar[1] = 10;
	return vars;
}

// Host-side fire state for ONE of this player's weapon slots — the clip the C2S 0x06 fire
// pipeline checks + decrements and the C2S 0x25 reload relay refills. Keyed (in the map
// below) by the weapon-slot combo = category*65 + rank [orig: the per-player 100-B
// weapon-slot array playerSlot+464, index roundSlotIndex = adm[4] + 65*adm[0]
// (Server_ClientFiredRound @0x50c0d7); the u16 clip = slot+16 (WeaponSlot_CanFire
// @0x541ba0 reads it, Weapon_ConsumeAmmo @0x540850 decrements it, WeaponSlot_ReloadAmmo
// @0x541720 refills it)]. The shared ammo pools (adm+220 belt / adm+216 ammo-point
// classes) and the +96472 fire-rate stamp are deferred — D-NET-152 tails.
struct WeaponSlotState {
	uint8_t adm_index = 0; // the weapon bound to this combo [orig: slot+32 adm ptr]
	int16_t clip = 0;      // rounds left in the magazine [orig: slot+16]
};

// Per-connection state for the reactive gameplay-message reply handlers (the §5.1 handshake /
// server-info / mission-metadata / loadout replies a retail joiner expects). Folded
// onto the connection node like InitialStateBurst — the slice of the retired GameSessionState the
// reactive NapiNPServerMsg_0x0NN handlers actually read (P8). The spawn-gate / world-stream burst
// state lives in `burst` above (driven by Server_SendInitialGameStateToPlayer); this carries only the
// reactive request→reply bookkeeping. [orig: per-player fields the NapiNPServerMsg_* handlers touch]
struct SessionReplyState {
	uint32_t rtt_request_countdown = 0; // player slot +89816, Server_TickUpdate @0x51D7E0
	// The host-measured round trip of this player's last completed C2S 0x2C
	// return leg (`GetTickCount - sent`, zeroed for the listen host's own local
	// slot) and its ring. Retail keeps ten dwords at +100368 with the ring
	// cursor at +100408: the cursor is pre-incremented, wraps past 10 to 0, and
	// the sample is stored at cursor*4 — so cursor 10 aliases the cursor dword
	// itself, which the next pre-increment then reads as a huge index and wraps.
	// The eleven-entry array below IS that layout (index 10 = the cursor).
	// [orig: NapiNPServerMsg_HandlePingResponse @0x515116..0x515155]
	uint32_t rtt_ms = 0;                         // +94400
	std::array<uint32_t, 11> rtt_ring{};         // +100368..+100408 (slot 10 = cursor)
	// Consecutive out-of-range samples under the host's min/max ping checks;
	// strictly over 20 punts the player. [orig: +94404 / +94406, @0x51519A..0x51521A]
	uint16_t min_ping_strikes = 0;
	uint16_t max_ping_strikes = 0;
	// The client's reported connection-quality level (C2S 0x4C, clamped 0..4)
	// and the dirty flag the 1 Hz S2C 0x46 quality resend consumes. A fresh
	// slot advertises 1 (the witnessed join-broadcast value).
	// [orig: slot+418 / slot+89860, sub_5006E0 @0x5006E0; the resend walk
	//  Server_TickUpdate @0x51DE79..0x51DF4A]
	uint8_t client_quality = 1;
	bool client_quality_dirty = false;
	// The host frame clock stamped by the C2S 0x3D loaded-model page reply; the
	// reply carries no payload the host reads. [orig: NapiNPServerMsg_0x03D
	//  @0x500EC0 -> player+97560 = dword_A87060]
	uint32_t loaded_model_reply_frame = 0;
	// The revive pose GameEvent_RevivePlayer saves on the victim (its live
	// position raised 0x4000, and yaw/pitch/roll) for the deploy that follows a
	// medic revive. [orig: @0x517DCD..0x517E09 (weaponSlots[20..40])]
	bool revive_pose_valid = false;
	int32_t revive_pos[3] = {0, 0, 0};
	int16_t revive_yaw = 0, revive_pitch = 0, revive_roll = 0;
	// A C2S 0x51 spectator-respawn request the dispatcher admitted; the host
	// tick performs the conversion (it needs the owning context).
	// [orig: Server_ProcessClientRequestSpectatorRespawn @0x51C840]
	bool spectator_convert_pending = false;
	int16_t spectator_convert_killer = -1;
	// Host ms of this player's last accepted chat (the 1000 ms per-sender
	// throttle; 0 = never). The same slot word is the refused-capture-touch
	// nag's stamp: a numbered flip stamps the capturer's and the join zeroes it.
	// [orig: slot+100360, NapiNPServer_HandleChatMessage @0x5137FF;
	//  GameEvent_FlagCapture @0x50F912; Server_OnPlayerJoin @0x51A6CD]
	uint32_t chat_last_ms = 0;
	// Two bits of the player-slot state byte (+89912). 0x04 asks the per-tick
	// slot pass for the S2C 0x1E event 58 frontier hint once the slot has
	// played 1240 ticks; 0x08 holds off a second refused-touch nag until a
	// numbered flip clears it on every slot. The join sets 0x04 and clears 0x08,
	// a refused touch sets both, the deploy leg's whole-byte write clears both.
	// [orig: CNetPlayer_SetGameState @0x4C4213..0x4C421F; Server_OnPlayerJoin
	//  @0x51A730..0x51A73A; Server_OnPlayerTouchCaptureZone @0x500C35;
	//  GameEvent_FlagCapture @0x50F7A0; Server_ProcessPlayerDeath @0x517803;
	//  Server_UpdateAllActivePlayerSlots @0x5189A6]
	bool frontier_hint_pending = false; // bit 0x04
	bool capture_nag_held = false;      // bit 0x08
	bool loadout_synced = false;        // 0x2F WEAPON-LOADOUT request seen (set on the 0x5A reply)
	// Authority-side per-ammo-class pool table (serverPlayer+88664), written in
	// full by S2C 0x0F after loadout acceptance. Indices are the weapon table's
	// retail ammo-class ids; unused classes remain zero.
	std::array<int32_t, 128> ammo_pools{};
    // Loaded-round buckets [orig: player+89176; sub_540670 @0x540670].
    std::array<int32_t, 128> shared_clips{};
	// The last GRANTED 0x5A loadout body, retained for the deploy-release re-send: the retail
	// deploy leg re-sends the player's loadout, and the client's 0x5A handler is the deploy
	// UN-LATCHER — it resets dword_81474C (set by the 0x0E pick) on completion, which is what
	// resumes the client's per-frame C2S 0x0C uplink [orig: Server_ProcessPlayerDeath's tail
	// calls Server_SendWeaponSlotListToPlayer @0x502550; client NapiNPClientMsg_
	// HandleWeaponLoadoutSync @0x4290E0 resets 81474C, §5.30; golden deploy frame 240018 =
	// 0x5A + 0x61 + 0x1E in one datagram; v32: without it both joiners' uplinks stopped
	// forever at the pick — the rubber-band]. (D-NET-156 tail)
	std::vector<uint8_t> last_loadout_reply;
	bool mission_status_received = false; // 0x0B mission-file status report seen
	// Retail's join completion is three packet boundaries, not one semantic
	// bag. The C2S 0x02 handler emits admission metadata; a later pending-spawn
	// pump replays settings + 0x04; the first 0x37 response then carries 0x64 +
	// the initial 0x16 beside any queued 0x75. Keeping independent latches
	// prevents a serializer refactor from moving 0x04 or 0x16 earlier (the retail
	// client resets its team between the first two packets).
	bool admission_metadata_pushed = false;
	bool spawn_metadata_pushed = false;
	// A pending player the spawn pump HELD (capacity or team balance) is re-sent
	// the S2C 0x03 restriction flag at most once per 1000 ms (netPlayer+36 is the
	// last send stamp); the pump stages the nag, tick_connections frames it.
	// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8F7A..0x4C8FD1]
	bool admission_hold_nag_pending = false;
	uint32_t admission_hold_nag_host_ms = 0;
	bool admission_hold_nag_host_ms_valid = false;
	uint32_t admission_completed_tick = 0;
	bool roster_pushed = false;
	uint32_t roster_completed_tick = 0;
	// Roster versioning (D-NET-155): roster_counted marks this connection's spawn as
	// tallied into NapiNPProtocol.roster_generation; roster_seen_gen is the last roster
	// version 0x16-pushed to this client — stale => re-push (covers the own-spawn grow,
	// golden 0x16 31→39 just before deploy, AND every later join/leave; the old
	// one-shot-per-connection re-push left existing clients' player lists stale when a
	// later joiner arrived — the v29 stuck HUD count).
	bool roster_counted = false;
	uint32_t roster_seen_gen = 0;
	// Retail's 0x30/0x31 integrity family rides the global scoreboard countdown,
	// so it has no per-connection epoch/serial. Network quality retains its own
	// independently witnessed global clock.
	// Per-player maintenance state [orig: Server_UpdateAllActivePlayerSlots
	// @0x518820]. Eligible active player slots (including the listen host) accrue
	// the 1,860-tick age after the quartet check; hidden/deploy-pending pauses
	// (never resets) that age. Thus
	// the first quartet is observed on call 1,861. Once mature, its 744-tick
	// countdown continues through death/deploy presentation state.
	uint32_t control_live_ticks = 0;
	uint32_t control_request_countdown = 0;
	uint32_t charattr_unanswered_count = 0;
	uint32_t time_sync_unanswered_count = 0;
	// Consecutive state-6 ticks whose owned entity carries Flags bit 0x02.
	// Retail increments before comparing, resets immediately on a live frame,
	// and emits the t7 punt on tick 361 when permanent death is disabled.
	// playerSlot+89864: consecutive periodic SECONDS the state-6 entity has
	// carried Flags bit 0x02; punt type 7 fires strictly over 360 of them.
	// [orig: Server_TickUpdate @0x51E066..0x51E07D inside the 62-tick block]
	uint32_t dead_live_seconds = 0;
	uint32_t control_challenge_seed = 0;
	// Per-player XOR salts used by the two definition-integrity reply handlers.
	// They are zero in the currently witnessed session setup, but belong to the
	// player slot rather than the selected resource profile. [orig: player-slot
	// dwords +89920 (C2S 0x20) and +89924 (C2S 0x21)]
	uint32_t integrity_weapon_crc_salt = 0;
	uint32_t integrity_ammo_crc_salt = 0;
	// Host GetTickCount value when retail installs this player in slot state 6.
	// Validity is explicit because zero is a legitimate mission-start timestamp.
	// The join-time respawn-pending AFK punt compares unsigned elapsed ms against
	// 360000 and requires strictly greater-than, not greater-or-equal.
	uint32_t state6_entry_host_ms = 0;
	bool state6_entry_host_ms_valid = false;
	// S2C 0x43 time-sync state [orig: producer @0x507CA0; validator
	// @0x502210; player-slot dwords 22486..22492]. The producer fixes one host
	// baseline, opens a round with its own host timestamp and increments the
	// sequence. A first matching reply fixes client baseline/previous time; a
	// later matching reply closes the round only when both client deltas fit
	// within 103% of the corresponding host deltas. This yields 1,1,2,2...
	uint32_t time_sync_host_baseline_ms = 0;
	uint32_t time_sync_round_host_ms = 0;
	uint32_t time_sync_current_host_ms = 0;
	uint32_t time_sync_client_baseline_ms = 0;
	uint32_t time_sync_previous_client_ms = 0;
	uint32_t time_sync_latest_client_ms = 0;
	uint32_t time_sync_sequence = 0;
	// Retail advances by 50 and wraps against the live viewport height before
	// emitting S2C 0x68, so zero-initial state first sends 50 for a tall viewport.
	uint32_t loaded_model_page_cursor = 0;
	// General minimap-overlay producer state [orig:
	// Server_UpdateAllActivePlayerSlots @0x5188CB..0x5188FB]. A newly spawned
	// client gets one complete persistent pool-2 scan. Thereafter retail calls
	// the producer every 14 ticks and advances one of 128 pool-1 residue classes.
	bool minimap_initial_scan_pending = true;
	uint8_t minimap_overlay_cooldown = 0;
	uint8_t minimap_pool1_phase = 0;
	// Last CRenderState Points value sent to this requester in S2C 0x81.
	// Retail stores the same signed dword at player-slot word 83 and starts at
	// zero, so a zero score has no initial packet.
	// [orig: Server_UpdateCaptureZoneProximity @0x50874E..0x508790]
	int32_t score_delta_sound_value = 0;

	// Joiner pose cached from the pre-spawn C2S 0x0C — the host's pose fallback when no World entity
	// is bound yet (pose_for_conn prefers the live registry Entity once owned_entity binds).
	PreSpawnJoinerPose pre_spawn_pose{};

	// The reactive-reply ROSTER IDENTITY. D-NET-132 / ADR 0013 §6.9 COLLAPSED the cached team + entity
	// handle onto link.owned_entity: `CAdminServer_HandleStatus @0x402e30` reads name/team/class OFF the
	// pool-0 entity (team @entity+344), so the reply builders now read the handle from
	// link.owned_entity.packed and the team from the live registry Entity through it — not a parallel
	// cache. player_slot (roster ORDER, not stored on the entity) stays a field; player_name is the
	// echoed game ClientAuth.NA the entity does not carry in the reimpl. The pre-World reactive path
	// (bind_session_reply_player) now stamps link.owned_entity with the bare wire handle instead of a
	// separate handle cache, so a World-less session-responder host resolves the same way.
	std::string player_name;
	uint8_t player_slot = 0;
	// S2C 0x04 advertises player_slot before Server_BuildPlayerInfoAndAdd runs.
	// Keep that pre-spawn table claim explicit because slot 0 is a valid identity,
	// not an available/uninitialized sentinel. Player-add consumes the claim; a
	// connection erase releases it.
	bool player_slot_reserved = false;
};

// One node on the host's connection_list — the reimpl of a NapiNPConnection [orig: the list
// NapiNPServer_SendFiltered @0x4C87E0 walks, one SendToConn @0x4c4f20 per node; struct reached via
// NapiNPProtocol.connection_list @+0xEBC, §6.5]. Named fields + cited offsets, idiomatic types
// (the in-memory layout is NOT byte-exact — wire bytes are; see the plan's fidelity decision).
//
// This unifies the two per-peer representations the old code split apart: HostSessionAccept's
// PeerState (handshake/SCRK/seq) and replication::Connection (transport + owned_entity + send_mask).
// The ClientAuth CU values loaded by NapiNetConfig_LoadFromConnTags; retained
// until the game-layer JOIN invokes Server_ValidatePlayerJoinRequest.
// [orig: @0x4c7260 -> @0x512100]
struct ClientGameEnvironment {
	int32_t bt = 0;
	int32_t vn = 0;
	int32_t bn = 0;
	// The "DB" (debug build) tag, NapiNetConfig+0x0C = NetPlayer+216: a nonzero
	// value exempts the slot from the min/max-ping punts. A stock client uploads
	// 0. [orig: NapiNetConfig_LoadFromConnTags @0x4C7260, the DB leg @0x4C733E]
	int32_t db = 0;
	int32_t mbn = 0;
	int32_t sopd = 0;
};

struct NapiNPConnection {
	// The slot's SCORE and the cached copy the S2C 0x81 change gate compares against.
	// Retail keeps both on the per-slot stats object: the score is field id 28
	// (== 0x1C, what CPlayerStats_GetFieldPlusOne (ex CRenderState_GetFieldByIndex)(playerSlotPtr + 18, 0x1C) reads);
	// the cached mirror is slot[83] [orig: Server_UpdateCaptureZoneProximity @0x5086A0].
	// Both start at 0, matching a fresh slot: retail sends nothing until the score
	// first CHANGES, so an untouched slot is silent rather than sending a zero.
	int32_t score = 0;
	int32_t score_wire_mirror = 0;
	// [orig +0x18] ConnectionId / dcb — the join-order id a player record is matched by [orig:
	// NapiNP_GetLocalConnectionId @0x4c6d40; Player_FindLocalPlayerEntity @0x4e0090 numeric match].
	uint32_t connection_id = 0;

	// The PER-PLAYER tick seed shipped as S2C 0x61 and stamped into this player's slot as the
	// fire-freshness floor. Retail re-rolls it as `((rand() & 0xFE) + 1) << 16` on join,
	// revive and deploy release, so it is per-connection state, never a session constant — a
	// shared constant would re-seed every client's clock to the same value on every deploy.
	// The zero form is the disarm: death and round-end pass enable=0, clearing this stamp and
	// freezing that client's network-role tick until the next re-arm.
	// [orig: Server_SendRandomSeedToPlayer @0x5101a0 — value @0x5101d4, slot stamp
	//  playerCtx+0x178D8 / arm +0x178E0; fresh-roll senders @0x51a982 / @0x51aa02 join,
	//  @0x51796d deploy, @0x517e47 revive; disarm @0x516ef4 death, arm @0x510237]
	uint32_t tick_seed = 0;
	// PlayerSlot_IsActive's two clock modes. The floor advances only after an
	// accepted shot; disarming retains it and opens the short receive grace.
	// [orig: PlayerSlot_IsActive @0x4FC760; Server_SendRandomSeedToPlayer @0x5101A0]
	uint32_t fire_tick_floor = 0;     // playerSlot+96472
	uint32_t fire_disarmed_at = 0;   // playerSlot+96460 (host logic tick)
	bool fire_tick_mode = false;    // playerSlot+96480


	// [orig: CNapiNPConnection_Create @0x62ACB0 direction mirroring, §6.5] 1 = server-side
	// connection (the host's view of a client), 2 = client-side connection (a client's view of
	// the host, incl. the host's own local loopback client).
	static constexpr uint8_t kTypeServerSide = 1;
	static constexpr uint8_t kTypeClientSide = 2;
	uint8_t type = kTypeServerSide;

	// Where this node is in its lifecycle (above).
	ConnectionPhase phase = ConnectionPhase::New;

	// The byte transport + the pool-0 entity this connection drives + the per-connection send
	// filter, modeled by replication::Connection (TransportMode, owned_entity, send_mask). The
	// transport is NON-OWNING: the binding/test owns the LoopbackChannel / UdpSessionTransport.
	replication::Connection link{};
	// A successful C2S 0x0E is dispatched during the socket receive pump, while
	// older C2S 0x0C events from that pump are staged for Server_TickUpdate.
	// Consume those pre-release uplinks before the next authority drain so they
	// cannot overwrite the host-selected deploy pose. The client cannot produce
	// a post-release uplink until it receives the release at the following send
	// boundary, so everything already staged at this edge is older than the pick.
	bool discard_pre_deploy_uplinks = false;
	// First host-side connection-description event wins. Staging a punt closes
	// the gameplay predicate immediately while leaving the owner node resident
	// long enough to frame and flush its reliable H:0x03 record.
	bool host_disconnect_sent = false;
	uint32_t host_disconnect_mismatch_type = 0;
	// Host-to-this-peer send block. Retail owns the countdown on each
	// NapiNPConnection (+0x648), so peers admitted on different ticks keep
	// independent boundaries even though the dictated period is session-wide.
	uint32_t s2c_send_holdoff_ticks = 0;
	uint32_t s2c_send_holdoff_countdown = 0;
	bool s2c_send_boundary_open = true;
	// This tick's 0x0A, built after the script pass and the maintenance legs
	// and queued behind their sends, ahead of the entity motor: retail's frame
	// is the last leg of Server_TickUpdate.
	// [orig: Game_ProcessMainFrame @0x5263F0 — the gated Server_TickUpdate call
	//  @0x5266B4..0x5266B6 (Server_TickUpdate @0x51D7E0 ends with the per-slot
	//  0x0A @0x51E3D6..0x51E450), then the Entity_UpdateAllEntities call @0x52674B]
	std::vector<uint8_t> staged_frame_update;
	bool frame_update_staged = false;
	// Hold routed guidance until the frame that can carry its fire descriptor.
	std::vector<std::vector<uint8_t>> pending_guidance;
	// The configured period is known when the node is allocated, but +0x648 is
	// not armed until NapiNPServer_UpdateHoldoffTicks dictates CS field 3 and
	// resets the counter. Pre-dictation hello/auth/admission turns stay open.
	bool s2c_send_holdoff_dictated = false;
	// Host tick of this connection's most recent framed session send — the
	// per-connection "last send" clock retail's send-interval pump reads.
	// Zero = not yet armed; the first open boundary arms it without sending.
	// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0]
	uint32_t last_session_send_tick = 0;

	// --- per-connection handshake state (P2: the old HostSessionAccept::PeerState, folded on) ---
	// SCRK / seq / ack + the session-flow latches, witnessed as fields the original keeps on the
	// NapiNPConnection node. Wire bytes stay byte-exact; this in-memory layout is not padded.

	PeerAddr peer{};               // the transport-addr key (distinct from connection_id; the
	                               // host scans connection_list by this to route a datagram).

	std::string pn;                // ClientHello.pn — game-session protocol identity
	std::string player_name;       // game ClientAuth.na — echoed into the organic-spawn 0x0C (D.0)
	// The ClientAuth CU JSR/JSPP values, stored at the 0x42 exactly like
	// retail's connection tag list; the game-layer 0x00 join message latches
	// link.spectator from the request and validates the password there.
	// [orig: NapiNetConfig_LoadFromConnTags @0x4c7260 (JSR -> ci1, JSPP);
	// the entry+55 latch in NapiNPServer_HandlePlayerJoinMessage @0x512aa0]
	ClientGameEnvironment join_environment;
	uint8_t join_spectator_request = 0;
	std::string join_spectator_password;
	std::string join_password; // ClientAuth JSP, shared side/squad credential (not numeric FID)
	// The team restored when the portable player-slot spectator bit is cleared.
	uint8_t spectator_restore_team = 1;
	std::string client_scrk;       // ClientAuth.scrk — decrypts inbound 0x43
	std::string server_scrk;       // our SCRK — encrypts outbound 0x83, echoed in ServerAuth
	uint32_t client_ck = 0;        // ClientAuth.ck -> session_id on our S2C
	uint32_t client_ci = 0;        // ClientAuth.ci — with client_ck, the retransmit-match key the join
	                               // leg compares a repeat 0x42 against [orig: session_keys.client_id
	                               // == CI && session_keys.remote_key == CK @ HandleClientJoin 0x62b750]
	uint32_t server_sk = 0;        // our ServerAuth.SK
	SessionSequencing seq = make_jo_game_session_sequencing();
	                               // outbound seq (from 1) + last inbound ack [ADR 0013 shared framing]
	// FIRST/MID/FINAL C2S records share one receive buffer on this peer's
	// connection. Only a completed semantic message may reach host dispatch.
	ProtocolReassemblyState c2s_reassembly{};
	uint32_t active_send_elapsed_ms = 0; // retained-message active-send interval; reset by every
	                                     // framed S2C packet, ticked by tick_connections
	// The host-clock millisecond at which the 0x42 join was accepted: the
	// validation-phase deadline base (netPlayer+0xA4) that reaps a peer which
	// keeps talking but never completes its admission.
	// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 reads [esi+0xA4] @0x4C8B8A]
	uint32_t join_validated_host_ms = 0;
	// The C2S 0x00 JOIN's CD identity blob as [name][value] pairs: the joiner's
	// NovaWorld NAMEINFO / PCID / SQUADINFO / JOINTICKET cookies. Empty on LAN.
	// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0 "CD" -> the cookie
	//  buffer; KeyValueBuffer_FindValue @0x4C2A30 walks it by key]
	std::vector<std::pair<std::string, std::string>> join_identity_pairs;
	// NetPlayer game state 4 (the NovaWorld ticket wait): the
	// ClientPlayerEnterRequest went to the service; admitted = its result was
	// success (state 6), and the spawn pump holds the player until then. The
	// deadline base stays join_validated_host_ms.
	// [orig: CNetPlayer_SetGameState(player, 4) @0x4C8BCA;
	//  CNapiGameSession_HandlePlayEnterResponse @0x4D1940 — state-4 gate
	//  @0x4D1B3E, SetGameState(6) @0x4D1C11]
	bool player_enter_requested = false;
	bool player_enter_admitted = false;
	bool player_enter_pending() const { return player_enter_requested && !player_enter_admitted; }
	// The last outer-namespace 0x45 ping round trip this host measured
	// (session_keys.rtt_ms; stored when the peer answers our MS without WR).
	// [orig: Nwu_HandlePing @0x623C9B]
	uint32_t session_ping_rtt_ms = 0;
	uint32_t receive_inactive_ms = 0;     // elapsed since the last IN-ORDER admitted session packet
	                                     // (a zero-message keepalive counts; duplicates, futures,
	                                     // resend lists and every other opcode do not) — retail's
	                                     // conn+0x5E8 reap clock, stamped only by ParseMessages
	                                     // and HandlePing [orig: CNapiNPConnection_ParseMessages
	                                     // @0x625d54; Nwu_HandlePing @0x623c56; read by
	                                     // PumpStateMachine @0x62935c]
	// The cs_dir0 template values copied onto this node at create (the reap window and the
	// outbound-message pool bound); the host's template is NapiNPProtocol::connection_template.
	// [orig: CNapiNPConnection_Create @0x62acb0 copies proto+0xE44/+0xE80 (`rep movsd ecx=0Fh`
	//  @0x62ae7b/@0x62aeb8); a client overlays the host's 0x82 CS block on top,
	//  NapiNP_HandleServerJoinResponse @0x629b4c..0x629b75 -> @0x629d72/@0x629d89]
	SessionTimeoutConfig timeouts{};
	// The connection's disconnect record — the NapiNPDisconnectEvent at +0x654 whose `valid`
	// gate makes the FIRST latch win. SendDisconnectPacket serializes it into the 0x46/0x86
	// teardown burst; producers are the receive reap (SERTMOUT/CLNTTMOUT), StopServer (STOP),
	// the pool overflow (MSGCRE), the pending-disconnect pump (PDESTME), a received goodbye or
	// description record (echoed back), and a user leave (CIDEMIS). latch_disconnect_event below.
	// [orig: the store-if-!valid gates @0x6293f4..0x629406 / @0x62a904..0x62a919 /
	//  @0x6280f9..0x62810a / @0x62a770..0x62a788 / @0x6240a8..0x6240ba / @0x621d3c..0x621d4d]
	DisconnectEvent disconnect_event{};
	bool disconnect_event_valid = false;
	// RequestDisconnect on a server-side (state 1) connection: the next protocol pump destroys
	// the node — the 0x86 burst then the player teardown — instead of tearing down inline.
	// [orig: CNapiNPConnection_RequestDisconnect @0x61e107 -> NapiNPProtocol_Pump @0x62a6fd]
	bool pending_disconnect = false;
	uint32_t peer_acked_seq = 0;   // highest hdr.ack_count the peer has echoed = the last of OUR 0x83
	                               // seqs it confirmed. Drives the initial-state backlog throttle: the
	                               // original stalls both burst tracks while the connection's
	                               // outstanding-message count (conn+0x768) >= 20 [orig: the
	                               // NapiNPMessageQueueState field read at 0x51bf1b / 0x51bc04] — the
	                               // client-paced backpressure that stretches the world stream across a
	                               // cold client's mission build (D-NET-150). Our structural stand-in
	                               // counts unconfirmed outbound datagrams (next_outbound_seq-1 minus
	                               // this); exact retail inc/dec sites pending an IDA pass.
	std::string session_id;        // key into the GameServerRuntime sessions_ (the peer label)
	GameAdmissionStage admission_stage = GameAdmissionStage::Complete;
	uint32_t admission_padding_x = 0; // S2C 0x02 challenge prefix the C2S echo must preserve
	uint32_t admission_padding_y = 0;

	// self_id (the joiner's own ConnectionId / dcb / unk_18) UNIFIES onto connection_id above
	// (+0x18), the value Player_FindLocalPlayerEntity @0x4e0090 numeric-matches. LAN latches the
	// host-assigned id after admission 0x02; a later 0x48 echo may restamp it (NovaWorld supplies it).
	bool self_id_seen = false;             // true only once player admission has completed
	bool world_stream_announced = false;   // F3 edge-latch (PeerEnteredWorldStreaming fires once)
	bool spawned_announced = false;        // edge-latch (PeerSpawned fires once)

	// --- P3: the World-driven initial-state burst cursor (Server_SendInitialGameStateToPlayer) ---
	InitialStateBurst burst{};

	// --- P8: the reactive gameplay-message reply state (the retired GameSessionState slice) ---
	SessionReplyState reply{};

	// The joiner's 0x42 CU character vars (above) — parsed at the join, consumed by the player add.
	CharacterJoinVars char_vars{};

	// Reserved before S2C 0x04 advertises the team, then consumed by the later
	// player-add pass. The validity bit is separate because team 0 is the future
	// spectator value, not an uninitialized sentinel.
	// [orig: playerSlot+416 is populated before NetPacket_WriteSlotAssignment @0x502b30]
	bool assigned_team_valid = false;
	uint8_t assigned_team = 0;

	// The command map's squad bytes on the player slot: its leader's slot
	// (+100576; 0xFF none) and its fireteam (+100577), seeded 0xFF / 0 by the
	// player add, and the slot it votes to punt (+100578). Retail's add
	// leaves the punt byte at the row memset's 0, which names row 0, always
	// the host's own local row (+5), so the tally skips it and a non-voter
	// counts nothing; the port's slot 0 can be a joiner's (a dedicated host
	// has no local slot), so the seed is 0xFF, the no-vote value the tally's
	// reset writes. (server_squad.h)
	// [orig: Server_PlayerAdd @0x51cd06 (the row memset), @0x51cf0a
	//  (+0x188E0 = 0xFF, +0x188E1 = 0; the loop @0x51d4e0 an older cite named
	//  fills slot+0x1708A's four 0xFFFF words, not the leader);
	//  Server_InitNewRoundState @0x51ca31..0x51ca35 (g_LocalNetPlayer, the
	//  table's row 0, active and local); Server_ProcessVoteKickResults
	//  @0x5114dc (the +5 skip), @0x511551 (the 0xFF reset);
	//  NetPacket_WritePlayerChainLink @0x5106d0;
	//  NapiNPServerMsg_0x045_HandleTeamAssignment @0x510c00;
	//  NapiNPServerMsg_VoteKick @0x518f10]
	uint8_t squad_leader = 0xFF;
	uint8_t fireteam = 0;
	uint8_t punt_vote = 0xFF;

	// Host-side per-weapon-slot fire/ammo state, by slot combo (WeaponSlotState above). Seeded
	// lazily on the first 0x06 for a combo; refilled by the 0x25 relay. (D-NET-152)
	std::map<uint16_t, WeaponSlotState> weapon_slots;

	// This player's entity EquippedSlot (entity+0x118) as the host points it: the personal
	// slot of the last accepted network fire's combo or the borrowed mount slot it fired
	// through, else the 0x2F loadout's submitted slot when that slot's def is NoSelect, else
	// none. The C2S 0x0C weapon echo moves only entity+0x2B0, so the slot can name a weapon
	// the player no longer holds — exactly what the remote held-weapon draw gate's ammo leg
	// then reads. [orig: Server_ClientFiredRound @0x50c1d4..0x50c28d;
	//  NapiNPServerMsg_HandlePlayerLoadout @0x515f52..0x515f8a; Entity_CanFireWeapon
	//  @0x4dcb30]
	struct EquippedSlotRef {
		enum Kind : uint8_t { kNone, kPersonal, kMount } kind = kNone;
		uint16_t combo = 0;          // kPersonal: the weapon_slots row
		world::EntityHandle mount{}; // kMount: the mount whose inline slot was borrowed
	};
	EquippedSlotRef equipped_slot;
};

// Store `event` as the connection's disconnect record only while none is latched — retail's
// `if (!conn->disconnect_event.valid) { copy; valid = 1; }` at every producer, so the FIRST
// cause of a teardown is the one its burst carries. The strings are capped like the 128/32-byte
// record fields (make_disconnect_event already applies the cap; a caller-built event gets it
// here). Returns true when this call latched.
// [orig: PumpStateMachine @0x6293f4..0x629406; NapiNPProtocol_StopServer @0x62a904..0x62a919;
//  NapiNPMessage_Create @0x6280f9..0x62810a; NapiNPProtocol_Pump @0x62a770..0x62a788;
//  Nwu_HandleDisconnect @0x6240a8..0x6240ba; HandleDescriptionPacket @0x621d3c..0x621d4d]
inline bool latch_disconnect_event(NapiNPConnection &conn, const DisconnectEvent &event) {
	if (conn.disconnect_event_valid) return false;
	conn.disconnect_event = make_disconnect_event(event.ds, event.dc, event.dp1, event.dp2,
			event.dstr, event.dpc, event.ddstr);
	conn.disconnect_event_valid = true;
	return true;
}

inline uint32_t clamp_send_holdoff_ticks(uint32_t ticks) {
	return ticks < 255u ? ticks : 255u;
}

inline void arm_s2c_send_holdoff(NapiNPConnection &conn, uint32_t ticks) {
	conn.s2c_send_holdoff_ticks = clamp_send_holdoff_ticks(ticks);
	conn.s2c_send_holdoff_countdown = conn.s2c_send_holdoff_ticks;
	conn.s2c_send_boundary_open = conn.s2c_send_holdoff_ticks == 0;
	conn.s2c_send_holdoff_dictated = true;
}

// Retail opens the first established boundary immediately after dictating CS
// field 3; only the completed send boundary reloads the stored period.
// [orig: CNapiNPConnection_ResetSendHoldoffCounter @0x61e140]
inline void reset_s2c_send_holdoff_counter(NapiNPConnection &conn) {
	conn.s2c_send_holdoff_countdown = 0;
	conn.s2c_send_boundary_open = true;
	conn.s2c_send_holdoff_dictated = true;
}

// [D-NET-122] The single in-match predicate shared by Server_TickUpdate's C2S drain AND its S2C 0x0A
// emit fan (it was an inline `conn.burst.spawned` in each). In-match = the §5.2a initial-state burst
// has completed (game_state 9 -> burst.spawned). BOTH a remote joiner AND the host's OWN type-2
// loopback latch this the SAME way: tick_connections drives every connection's §5.2a burst
// (including the host loopback, D-NET-114) until completion, THEN this flips true. So the host's own
// loopback is no longer starved of its per-frame 0x0A once Server_TickUpdate is the SP/host driver.
// Server_BuildPlayerInfoAndAdd binds the loopback's live owner; D-NET-121 separately rejects an
// unbound/freed/lifetime-stale owner before the writer can mutate or emit.
// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate; recipient entity eye reads
// @0x517BF5..0x517C13; the outer filtered fan is NapiNPServer_SendFiltered @0x4C87E0]
inline bool is_in_match(const NapiNPConnection &conn) {
	return conn.burst.spawned && !conn.host_disconnect_sent;
}

// NapiNPServer_SendFiltered's 0x80 arm accepts player-slot state 6 or 7. It
// includes the listen host and does not inspect entity health; this runtime's
// completed initial-state burst is the shared representation of that active
// slot state. [orig: NapiNPServer_SendFiltered @0x4C8874..0x4C8894,
// @0x4C893E..0x4C8953]
inline bool active_player_recipient(const NapiNPConnection &conn) {
	return is_in_match(conn) && conn.link.transport != nullptr;
}

} // namespace opennova::inmatch
