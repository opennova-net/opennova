#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include <netsim/connection.h>             // netsim::Connection, netsim::TransportMode
#include <npwire/peer_addr.h> // opennova::PeerAddr (the transport-addr key)
#include <npwire/protocol_message.h> // opennova::SessionSequencing (the per-connection seq/ack, ADR 0013)

namespace opennova::np {

// dcb (ConnectionId) reservation on a LAN listen server. The host's loopback client consumes
// ConnectionId 0/1; the host's own PLAYER is dcb 2; remote joiners are assigned from 3 up. A
// 0x14B9 organic with entity+0x78 == 0 is the dedicated-server reservation, which makes a joining
// client keep no player entity for that slot and flood C2S 0x0F → 0xC9 disconnect — so the host
// player MUST carry a non-zero dcb. Wire-proven by the golden host_and_join_lan.pcapng (host player
// eFlags=2, first joiner 3, AI 0). [orig: NapiNP_GetLocalConnectionId @0x4c6d40; Server_PlayerAdd
// @0x51cbc0 writes entity+0x78 = conn->connection_id; net-re §5.2a/§5.2b]
inline constexpr uint32_t kHostPlayerDcb = 2;
inline constexpr uint32_t kFirstJoinerDcb = kHostPlayerDcb + 1;

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
// activity. The client keepalive interval is deliberately shorter (30000 ms).
// [orig: CNapiNetwork_Init @0x4caa81/@0x4cab54]
inline constexpr uint32_t JO_GAME_SESSION_TIMEOUT_MS = 120000;

inline SessionSequencing make_jo_game_session_sequencing(
		uint32_t next_outbound_seq = 1, uint32_t last_inbound_seq = 0) {
	SessionSequencing sequencing;
	sequencing.next_outbound_seq = next_outbound_seq;
	sequencing.last_inbound_seq = last_inbound_seq;
	sequencing.ordered_recovery_enabled = true;
	sequencing.outbound_message_limit = JO_GAME_SESSION_OUTBOUND_MESSAGE_MAX;
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
	uint16_t player_sync_subphase = 8;  // [playerSlot+89878] 8..20 (one tag each; see server_initial_state)
	uint8_t  world_stream_phase = 0;    // [playerSlot+89882] 0 phase-0 init [orig: @0x51bc1a case 0], 1=0x10 .. 7=0x1A
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

// Host-side fire state for ONE of this player's weapon slots — the clip the C2S 0x06 fire
// pipeline checks + decrements and the C2S 0x25 reload relay refills. Keyed (in the map
// below) by the weapon-slot combo = category*65 + rank [orig: the per-player 100-B
// weapon-slot array playerSlot+464, index roundSlotIndex = adm[4] + 65*adm[0]
// (Server_ClientFiredRound @0x50c0d7); the u16 clip = slot+16 (WeaponSlot_CanFire
// @0x541ba0 reads it, consume_weapon_ammo @0x540850 decrements it, WeaponSlot_ReloadAmmo
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
	bool loadout_synced = false;        // 0x2F WEAPON-LOADOUT request seen (set on the 0x5A reply)
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
	bool roster_pushed = false;           // 0x16/0x46 roster PUSHED proactively post-handshake (once) —
	                                      // the working host pushes it before the joiner ever sends 0x0A
	// Roster versioning (D-NET-155): roster_counted marks this connection's spawn as
	// tallied into NapiNPProtocol.roster_generation; roster_seen_gen is the last roster
	// version 0x16-pushed to this client — stale => re-push (covers the own-spawn grow,
	// golden 0x16 31→39 just before deploy, AND every later join/leave; the old
	// one-shot-per-connection re-push left existing clients' player lists stale when a
	// later joiner arrived — the v29 stuck HUD count).
	bool roster_counted = false;
	uint32_t roster_seen_gen = 0;

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
// PeerState (handshake/SCRK/seq) and netsim::Connection (transport + owned_entity + send_mask).
struct NapiNPConnection {
	// [orig +0x18] ConnectionId / dcb — the join-order id a player record is matched by [orig:
	// NapiNP_GetLocalConnectionId @0x4c6d40; Player_FindLocalPlayerEntity @0x4e0090 numeric match].
	uint32_t connection_id = 0;

	// The PER-PLAYER tick seed shipped as S2C 0x61 and stamped into this player's slot as the
	// fire-freshness floor. Retail re-rolls it as `((rand() & 0xFE) + 1) << 16` on join, death,
	// revive and deploy release, so it is per-connection state, never a session constant — a
	// shared constant would re-seed every client's clock to the same value on every deploy.
	// The zero form is the witnessed round-end disarm.
	// [orig: Server_SendRandomSeedToPlayer @0x5101a0 — value @0x5101d4, slot stamp
	//  playerCtx+0x178D8 / arm +0x178E0; senders @0x51a982 join, @0x516ef4 / @0x51796d death,
	//  @0x517e47 revive; disarm @0x510237]
	uint32_t tick_seed = 0;

	// [orig: NapiNPConnection_Create @0x62ACB0 direction mirroring, §6.5] 1 = server-side
	// connection (the host's view of a client), 2 = client-side connection (a client's view of
	// the host, incl. the host's own local loopback client).
	uint8_t type = 1;

	// Where this node is in its lifecycle (above).
	ConnectionPhase phase = ConnectionPhase::New;

	// The byte transport + the pool-0 entity this connection drives + the per-connection send
	// filter, modeled by netsim::Connection (TransportMode, owned_entity, send_mask). The
	// transport is NON-OWNING: the binding/test owns the LoopbackChannel / UdpSessionTransport.
	netsim::Connection link{};

	// --- per-connection handshake state (P2: the old HostSessionAccept::PeerState, folded on) ---
	// SCRK / seq / ack + the session-flow latches, witnessed as fields the original keeps on the
	// NapiNPConnection node. Wire bytes stay byte-exact; this in-memory layout is not padded.

	PeerAddr peer{};               // the transport-addr key (distinct from connection_id; the
	                               // host scans connection_list by this to route a datagram).

	std::string pn;                // ClientHello.pn — game-session protocol identity
	std::string player_name;       // game ClientAuth.na — echoed into the organic-spawn 0x0C (D.0)
	std::string client_scrk;       // ClientAuth.scrk — decrypts inbound 0x43
	std::string server_scrk;       // our SCRK — encrypts outbound 0x83, echoed in ServerAuth
	uint32_t client_ck = 0;        // ClientAuth.ck -> session_id on our S2C
	uint32_t client_ci = 0;        // ClientAuth.ci — with client_ck, the retransmit-match key the join
	                               // leg compares a repeat 0x42 against [orig: session_keys.client_id
	                               // == CI && session_keys.remote_key == CK @ HandleClientJoin 0x62b750]
	uint32_t server_sk = 0;        // our ServerAuth.SK
	SessionSequencing seq = make_jo_game_session_sequencing();
	                               // outbound seq (from 1) + last inbound ack [ADR 0013 shared framing]
	uint32_t active_send_elapsed_ms = 0; // retained-message active-send interval; reset by every
	                                     // framed S2C packet, ticked by tick_connections
	uint32_t receive_inactive_ms = 0;     // elapsed since the last cryptographically receiver-valid
	                                     // packet; the 120 s timeout sweep resets it on activity
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

	// Host-side per-weapon-slot fire/ammo state, by slot combo (WeaponSlotState above). Seeded
	// lazily on the first 0x06 for a combo; refilled by the 0x25 relay. (D-NET-152)
	std::map<uint16_t, WeaponSlotState> weapon_slots;
};

// [D-NET-122] The single in-match predicate shared by Server_TickUpdate's C2S drain AND its S2C 0x0A
// emit fan (it was an inline `conn.burst.spawned` in each). In-match = the §5.2a initial-state burst
// has completed (game_state 9 -> burst.spawned). BOTH a remote joiner AND the host's OWN type-2
// loopback latch this the SAME way: tick_connections drives every connection's §5.2a burst
// (including the host loopback, D-NET-114) until completion, THEN this flips true. So the host's own
// loopback is no longer starved of its per-frame 0x0A once Server_TickUpdate is the SP/host driver.
// Its 0x0A anchors correctly (not the D-NET-121 dvxi5 fallback) because Server_BuildPlayerInfoAndAdd
// binds conn.link.owned_entity to the host player when it spawns. [orig: the per-frame replicate fan
// is is_in_session-gated inside Server_TickUpdate; the per-connection in-match selector is the burst
// completion]
inline bool is_in_match(const NapiNPConnection &conn) { return conn.burst.spawned; }

} // namespace opennova::np
