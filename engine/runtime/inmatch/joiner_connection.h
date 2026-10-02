#pragma once

#include <runtime/inmatch/charattr_challenge.h>
#include <runtime/inmatch/integrity_challenge_profile.h>
#include <runtime/inmatch/join_role.h>
#include <runtime/inmatch/napi_np_connection.h>

#include <net/npwire/ingame_decode.h>     // OrganicSpawnBatch / PlayerExtendedUplink / EntityPacketSubHeader
#include <net/npwire/protocol_message.h>  // ProtocolMessage
#include <net/npwire/session_hello.h>     // DisconnectEvent
#include <net/npwire/session_vars.h>      // SessionVars (the 0x60 server-info list)

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <array>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// P2 — the CLIENT mirror of the np server legs. JoinerConnection is the bytes-in / bytes-out state
// machine that drives a JointOperations in-match JOIN from the joiner side, promoted verbatim from
// novaworld::JoinerSession onto a client-side NapiNPConnection (type 2) that holds its SCRK/seq/ack.
// It is the symmetric inverse of the np server legs (this class crafts what handle_server_datagram
// parses, and parses what frame_session_replies frames), so the two are unit-testable against each
// other in-process with no sockets.
//
// It does NOT compose ClientSession: ClientSession's post-0x82 path is the matchmaking lobby-verify
// flow, the WRONG channel for the in-match game connection. The game connection follows retail's
// post-auth 0x00 -> 0x01 -> 0x02 exchange, learns its mission from 0x7B, then drives the witnessed
// in-match C2S burst that trips the host's spawn gate. Its identity comes from npwire's neutral
// retail JO helper; this LAN/game-session state machine has no NovaWorld service dependency.
//
//   start()                         -> ClientHello (0x41)   [Idle -> Hello]
//   <- ServerHello (0x81)             : learn host key hk; send ClientAuth (0x42)
//   <- ServerAuth  (0x82)             : learn server keys                       [-> Driving]
//   <- initial S2C settings            : header-only ACK; send C2S 0x00 JOIN
//   <- S2C 0x00                       : header-only ACK; send C2S 0x01 {0}
//   <- S2C 0x02                       : send witnessed 256-byte C2S 0x02 response
//   <- S2C 0x05/0x60/0x64/0x16       : reactively complete admission and file-transfer requests
//   <- S2C 0x11                       : ACK terminal pre-world sync; hold 0x0A until world ready
//   <- S2C 0x0C organic-spawn (0x83)  : owner-ID match self and retain wire handle H
//   <- S2C 0x1A                       : send the 0x2F/0x2F/0x0B loadout/status bundle (the
//                                       binding's kit via set_loadout_kit, else the capture default)
//   <- initial S2C 0x5A grant pair    : open gameplay once H is known; if zones, keep deploy UI pending
//                                      (headless callers immediately send C2S 0x0E {FFFF} and re-arm hold)
//   <- ACK-qualified post-pick 0x5A   : reopen the 0x0E-armed gameplay hold; complete deploy UI
//   frame_c2s_uplink(H, ...)          : per-frame C2S 0x0C player uplink
//
// Self-identification uses ServerAuth.MI, the authenticated connection ID (dcb).
// S2C 0x0C carries that ID in entity+120. A pool-0 player (Flags & 0x100) owned
// by this connection supplies the wire handle H used by the C2S player uplink.
// Callsigns and roster indices are independent of ownership. No uplink is sent
// until both H and the deployment release are known.
// [orig: Player_FindLocalPlayerEntity @0x4E0090; NapiNPClientMsg_0x00C @0x42E730;
// NapiNP_GetLocalConnectionId @0x4C6D40]. The owner pumps bytes; no socket I/O here.
namespace opennova::inmatch {

// The OS time-zone bias in minutes, UTC = local + bias, of standard (not
// daylight) time: the standard-library equivalent of TIME_ZONE_INFORMATION.Bias.
// UTC's broken-down fields read back as local standard time (tm_isdst = 0) lie
// exactly `bias` after the instant they came from.
// [orig: UI_JoinSelectedSession @0x5699d0 — GetTimeZoneInformation @0x569da1,
//  net_config.tzb = Bias @0x569dbd]
inline int32_t local_time_zone_bias_minutes() {
	const std::time_t now = std::time(nullptr);
	std::tm utc{};
#ifdef _WIN32
	if (gmtime_s(&utc, &now) != 0) return 0;
#else
	if (gmtime_r(&now, &utc) == nullptr) return 0;
#endif
	utc.tm_isdst = 0;
	const std::time_t as_local = std::mktime(&utc);
	if (as_local == static_cast<std::time_t>(-1)) return 0;
	return static_cast<int32_t>(std::difftime(as_local, now) / 60.0);
}

enum class TerrainTilState : uint8_t {
	Absent = 0,
	Receiving = 1,
	Complete = 2,
	Invalid = 3,
};

class JoinerConnection {
public:
	using MonotonicMilliseconds = std::function<uint64_t()>;

	enum class Phase {
		Idle,     // nothing sent yet
		Hello,    // ClientHello sent, awaiting ServerHello
		Auth,     // ClientAuth sent, awaiting ServerAuth
		Driving,  // ServerAuth accepted; reactive admission/world/deployment FSM is active
		InMatch,  // self handle is known and gameplay may run; deploy UI can independently remain pending
		Error,    // protocol/envelope error or server rejection
	};

	// The joiner's own spawn, learned from the named S2C 0x0C organic-spawn record.
	struct SelfSpawn {
		int32_t  pos_x = 0, pos_y = 0, pos_z = 0; // i32 16.16 world
		int32_t  orientation = 0;                 // 32-bit BAM
		uint8_t  team = 0;
		uint16_t item_type_id = 0x14B9;
		uint8_t  anim_slot = 0;                   // character selector (entity+0x374)
		uint16_t net_id = 0;                      // packed character/minimap id (entity+0x15C)
	};

	// S2C 0x53 and 0x6F mutate one shared retail timer-list entry; 0x6C updates
	// the selected active-window presence byte. Keeping them in
	// one typed FIFO preserves message order when both tags share a protocol packet;
	// separate per-tag vectors would necessarily reorder that packet at the runtime fold.
	using ZoneTimerUpdate =
			std::variant<ZoneTimerValue, ZoneTimerWindow, ZonePresenceCount>;

	struct PollResult {
		// Datagrams to send back to the host at the next send boundary: the admission packets
		// whose sequences this receive allocated (ClientRuntime holds them behind the
		// send-holdoff gate) and the handshake's ClientAuth (no holdoff exists before the
		// host's CS update), or a terminal teardown's goodbye burst (shipped at once).
		std::vector<std::vector<uint8_t>> outbound;
		// Datagrams retail transmits from INSIDE the receive pump: the opcode handler
		// calls CNapiNPManager_SendTo itself, so they never wait for the send-holdoff gate.
		// They are the 0x84 resend answers (each requested sequence rebuilt from its
		// retained records with the current ACK) and the 0x45 pong of a WR-flagged 0x85.
		// The owner ships them at their datagram's receive position.
		// [orig: NapiNPProtocol_PumpRecvQueues @0x6266a0 dispatches 0x84/0x85 off its
		//  receive FIFO (the queue_mask & 2 leg @0x6267f2..0x6268d0, the client receive
		//  pump's flags 26 from CNapiNetwork_PumpClientProtocolRecv @0x4c4fe0);
		//  NapiNP_HandleResendList @0x623800 -> CNapiNPConnection_SendSessionPacket
		//  @0x6239b6 -> CNapiNPManager_SendTo @0x61f039; Nwu_HandlePing @0x623a70 ->
		//  CNapiNPConnection_SendPing @0x623c6f -> CNapiNPManager_SendTo @0x61f261]
		std::vector<std::vector<uint8_t>> immediate_outbound;
		// Receive handlers queue reliable semantic replies here, in the order they ran.
		// ClientRuntime appends them to its one C2S queue as it folds the reducer stream.
		std::vector<ProtocolMessage> queued_send_messages;
		// For each queued reply, how many inbound_reducer entries its handler's message
		// follows: the reply joins the queue right after the fold of that many entries, so
		// the replies of a datagram keep its messages' wire order beside the fold's own
		// requests (the 0x16 handler's 0x22, the 0x0A handler's 0x0F).
		std::vector<std::size_t> queued_send_after;
		// DIAGNOSTIC VIEW ONLY: raw S2C 0x0A bodies. ClientRuntime applies
		// nothing from this vector — the reducer stream below is the sole
		// applied stream; per-family tests assert against these views.
		std::vector<std::vector<uint8_t>> inbound_0a;
		// DIAGNOSTIC VIEW ONLY: raw S2C world-stream spawn/static bodies the
		// host sends during load, as (tag, body): 0x0C organics, 0x0D pool-1,
		// 0x10 statics, 0x20 markers.
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> inbound_world;
		// DIAGNOSTIC VIEW ONLY: validated live S2C gameplay bodies (reload
		// echoes and entity lifecycle updates). 0x0A tag-2 rounds remain
		// embedded in inbound_0a.
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> inbound_gameplay;
		// The canonical client-reducer stream in original packet order. The
		// legacy per-family vectors above remain diagnostic views only; applying
		// them separately reorders 0x0A/world/map/gameplay records that shared a
		// protocol packet.
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> inbound_reducer;
		bool reached_in_match = false;                // applicable deployment edge once self H is known
		// Every successfully decoded S2C 0x5A runs retail's unconditional
		// dword_81474C clear, independently from whether it is the causal
		// ACK-qualified deployment/UI release.
		bool gameplay_release_applied = false;
		// The headless auto-pick can be queued inside this receive fold. It re-arms
		// only dword_81474C after an initial 0x5A may already have opened gameplay.
		bool gameplay_hold_rearmed = false;
		// S2C 0x61 — the per-player TICK SEED (not an SCRK exchange). The client's whole
		// network-role clock is anchored to it: retail stores it into BOTH currentTick and
		// g_LastKeepaliveTick, and the host stamps the same value into the player slot's
		// fire-freshness expiration. `set` distinguishes "no 0x61 this poll" from a
		// witnessed seed of ZERO (the round-end/disarm form ships four zero bytes).
		// [orig: dispatch row 0x61 (table @0x82ae28) -> NapiNPClientMsg_HandleSessionKey
		//  @0x4297c0: g_ClientCurrentTick @0xA8229C <- @0x4297f8, g_LastKeepaliveTick @0xA822A0
		//  <- @0x4297fd; a short body seeds 0 @0x4297eb]
		bool tick_seed_set = false;
		uint32_t tick_seed = 0;
		bool send_holdoff_set = false;
		uint32_t send_holdoff = 0;
		std::vector<WeaponLoadout> loadout_grants;
		// S2C 0x0F — the authority's per-ammo-class pool image (serverPlayer+88664),
		// the fixed 128-dword span at body offset 23 that retail copies straight into
		// g_LocalAmmoPools before re-drawing every clip. `set` marks a 0x0F seen this
		// poll (a later one in the same poll wins, as the last handler run would).
		// [orig: NapiNPClientMsg_0x00F @0x42e324..0x42e34a -> @0x42e424]
		bool ammo_pools_set = false;
		std::array<int32_t, kWorldStateAmmoPoolCount> ammo_pools{};
		std::vector<ZoneTimerUpdate> zone_timer_updates;
		// S2C 0x50 leg 1: this poll re-latched OUR OWN team (the same byte_A85B48
		// latch the S2C 0x04 tail writes). The binding re-styles friend/foe from it.
		// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — byte_A85B48 store @0x4319db]
		bool self_team_changed = false;
		uint8_t self_team = 0;
		// Diagnostic summary of S2C 0x50; entity state is folded only from the
		// corresponding raw inbound_gameplay record.
		std::vector<std::pair<uint16_t, uint8_t>> entity_team_assigns;
		// Diagnostic summary of S2C 0x5D; entity state is folded only from the
		// corresponding raw inbound_gameplay record.
		std::vector<uint16_t> destroyed_pool0_slots;
		// S2C 0x46 with bit15 (0x8000): the roster slot's BOOKKEEPING is cleared
		// (active flag, names, team, entity ref). The ENTITY IS NOT DESTROYED on this
		// leg — the entity-field wipes @0x431437 are dead code there because
		// entitySlotPtr is null. [orig: NapiNPClientMsg_PlayerSync @0x431370
		//  @0x431411..0x43144c -> PlayerSlot_ClearAndUnlink @0x434730]
		std::vector<uint8_t> cleared_player_slots;
	};

	// `player_name` is the displayed game ClientAuth.NA callsign; numeric connection
	// ownership identifies the local player. ClientHello.CO is the company identity.
	explicit JoinerConnection(std::string player_name);
	// Injectable monotonic wall clock for deterministic hosts/tests. Production uses steady_clock.
	JoinerConnection(std::string player_name, MonotonicMilliseconds monotonic_milliseconds);

	// Begin the handshake. Returns the ClientHello datagram to send (Idle -> Hello).
	std::vector<uint8_t> start();
	// The browse row's record of the host's 0x81 (a LAN row the player picked):
	// a join from a discovered session goes straight to the 0x42 with the row's
	// HK and SF -- no new 0x41 -- as retail's connect takes HK, address and port
	// from the session record into the connect state. Configure before start();
	// `present == false` (a bare dial) sends the 0x41 first.
	// [orig: UI_JoinSelectedSession @0x5699d0 -> CNapiNetwork_StartClientConnection
	//  @0x4ca160 -> CNapiNPConnection_InitFromSession @0x626320 (the record's HK
	//  @0x62639c, address/port @0x6263ae..0x6263b7, state 3 @0x626496)]
	struct DiscoveredSession {
		bool present = false;
		uint32_t host_key = 0;
		bool password_required = false;
		std::string expansion; // the 0x81 SUS2, echoed in the C2S JOIN EXP TLV
	};
	void set_discovered_session(DiscoveredSession session) { discovered_ = std::move(session); }

	// Feed one inbound datagram (off the socket, CRC envelope intact). Returns the reply datagrams +
	// any surfaced 0x0A bodies + whether this datagram learned H.
	PollResult handle_datagram(const uint8_t *raw, std::size_t len);

	// While awaiting 0x81/0x82, re-emit the exact pending 0x41/0x42 wire datagram, measured by the
	// monotonic wall clock (independent of render/simulation cadence): the 0x41 every 1000 ms, the
	// 0x42 once more than 2000 ms passed, failing the join with connect error 2 (NCC002) once the
	// 0x42 leg has waited more than 30000 ms.
	// Once Driving, flush deferred ACKs, the active-interval header-only probes, and the held C2S
	// 0x0A after local world readiness. Semantic admission transitions are emitted reactively from
	// handle_datagram(); the 0x44 missing-sequence request belongs to finish_receive_pump().
	std::vector<std::vector<uint8_t>> pump(uint32_t now_tick);

	// The receive pump's per-connection tail, run once per client frame after every datagram
	// of the batch went through handle_datagram. A future S2C packet in the batch latched the
	// missing-sequence check; when the ordered queue still holds a packet (the gap outlived the
	// batch) ONE C2S 0x44 naming the missing sequences goes out NOW, outside the send-holdoff
	// gate, and the latch clears either way. Empty when nothing is owed or the session is lost.
	// [orig: NapiNPProtocol_PumpRecvQueues @0x6266a0, the per-connection tail @0x6269bb..0x6269d6
	//  -> CNapiNPConnection_SendMissingSeqList(conn, 0) @0x6269ce -> CNapiNPManager_SendTo
	//  @0x62376d; the latch is set by NapiNPProtocol_HandleSessionPacket @0x626c3a]
	std::vector<uint8_t> finish_receive_pump();

	// The retail loop whose passes call this connection's send pump at the current admission
	// stage. A stock client reaches its holdoff-gated Client_ProcessNetworkFrame only when
	// mission loading ends; until then each stage waits inside its own loop, and each loop
	// calls PumpClientProtocolSend (flags 738, whose build still needs the holdoff countdown at
	// zero) its own way. NetworkFrame is one receive + one send per frame (the join state
	// machine's UI frames, then the in-match frame); the busy spins pass receive + send every
	// few microseconds, so a running countdown is out before the next datagram; the timed
	// loops call the send pump only once MORE than their pace passed since the last call.
	// [orig: MultiPlayer_JoinSessionStateMachine @0x56a320 states 5/6 (@0x56a5dd, sub_424740
	//  @0x424767); SaveFile_SendAndWaitForServerAck @0x5204b0; InitRandomSeedOrRequest
	//  @0x51e8f0; NapiClient_WaitForDisconnect @0x42cb20; NapiClient_WaitForGameStart @0x42cc10;
	//  Game_StartMission's 0x0F wait @0x52628d..0x5262df; Client_ProcessNetworkFrame @0x42c3dd]
	enum class SendPumpLoop : uint8_t {
		NetworkFrame,    // a send per frame behind the countdown (UI frames, the in-match frame)
		ServerInfoWait,  // SaveFile_SendAndWaitForServerAck: the send after > 50 ms
		MissionDataWait, // CNapiGameSession_InitRandomSeedOrRequest: a busy spin
		SyncTailWait,    // NapiClient_WaitForDisconnect: the send after > 100 ms
		WorldStreamWait, // NapiClient_WaitForGameStart: a busy spin
		WorldStateWait,  // Game_StartMission's final wait for S2C 0x0F: a busy spin
	};
	SendPumpLoop send_pump_loop() const;
	// A timed loop's pace in GetTickCount milliseconds (strictly more must pass); 0 otherwise.
	static int32_t send_pump_loop_pace_ms(SendPumpLoop loop);

	// Frame the retail leave: a burst of identical 0x46 ClientGoodBye datagrams for the owner to
	// ship before dropping the socket (the host's only non-timeout teardown trigger). Empty until
	// ServerAuth assigns the session key, and idempotent — a second call returns nothing. The
	// body carries the connection's LATCHED disconnect record: a host punt/goodbye already latched
	// its record (echoed back), the silence reap its CLNTTMOUT record, a pool overflow MSGCRE;
	// a user leave with nothing latched latches {2, 2, 0, 0, "I.C:CIDEMIS", 0, ""} first.
	// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 -> SendDisconnectPacket
	// @0x61f2a0 (the record TLVs @0x61f3d0..0x61f4aa); send count clamped by
	// cs_dir0.recv_max_per_tick (JO template: 4 — CNapiNetwork_Init @0x4ca4a0 stores @0x4cab60);
	// the leave record Input_HandleActionBinding case 3 @0x49af38..0x49af42 ->
	// CNapiNetwork_DisconnectActiveConnection @0x4C9140 @0x4c91ef..0x4c923e]
	std::vector<std::vector<uint8_t>> disconnect();

	// Build a C2S 0x0C player uplink datagram: 5-byte sub-header (handle = H, item_type_id = type,
	// sub_op = 0x0A extended) + the 43-byte extended body, wrapped as one 0x43 SESSION packet.
	std::vector<uint8_t> frame_c2s_uplink(uint16_t handle_H, uint16_t type,
	                                      const PlayerExtendedUplink &body);

	// Frame one inner {tag,body} as a complete send boundary of its own: one 0x43 SESSION datagram
	// over this connection's live SCRK + seq (advances the outbound seq, stamps the ack), then the
	// boundary's flush-counter advance. A test/replay seam only: every live C2S producer queues a
	// ProtocolMessage for ClientRuntime's holdoff-gated boundary instead, as retail queues every
	// message and frames only in PumpClientProtocolSend. [orig: CNapiNetwork_QueueReliableMessage
	// @0x4c4fa0 -> CNapiNPConnection_QueueMessage @0x628640]
	std::vector<uint8_t> frame_inner(uint8_t tag, std::vector<uint8_t> body) {
		std::vector<uint8_t> datagram =
				frame_session({make_protocol_message(tag, std::move(body))});
		if (!datagram.empty()) complete_session_send_flush(conn_.seq);
		return datagram;
	}
	// Flush several queued inner messages through as few sequenced packets as fit under the
	// negotiated JO packet ceiling. Retail queues semantic records first and frames at the send
	// boundary; callers use this instead of minting one outer datagram per record.
	struct FrameMessagesResult {
		std::vector<std::vector<uint8_t>> datagrams;
		// The msg_out_max-admitted prefix. Nodes after this prefix were rejected
		// by capacity and are intentionally not retried.
		std::size_t admitted_count = 0;
		// The prefix actually committed to session packets. If framing fails,
		// [framed_count, admitted_count) remains owned by the caller.
		std::size_t framed_count = 0;
		bool frame_failed = false;
		// Admitted nodes the build's packet budget left queued, in queue order (a split
		// message's remaining pieces first): the caller keeps them at the head of its queue
		// for the next build.
		std::vector<ProtocolMessage> unbuilt;
	};
	// The one-argument forms frame under this connection's packet ceiling: cs_dir0 field 13
	// (the template's 1300, overlaid by the host's 0x82 and its 0x2000 CS update, the
	// negotiated mpmaxpacketsize) [orig: BuildOutgoingPackets @0x628436, min 26 @0x628446].
	// Every form but the three-argument one builds at most cs_dir0 field 14's packets
	// (max_packets_per_build; the template's -1 is unbounded) [orig: @0x62844e].
	FrameMessagesResult frame_messages_detailed(
			const std::vector<ProtocolMessage> &messages) {
		return frame_messages_detailed(messages, packet_ceiling_bytes());
	}
	FrameMessagesResult frame_messages_detailed(
			const std::vector<ProtocolMessage> &messages, std::size_t max_packet_bytes) {
		return frame_messages_detailed(messages, max_packet_bytes,
				max_packets_per_build(conn_.timeouts.max_packets_per_tick));
	}
	// `max_packets` is what remains of this build's packet budget (at least 1).
	FrameMessagesResult frame_messages_detailed(const std::vector<ProtocolMessage> &messages,
			std::size_t max_packet_bytes, std::size_t max_packets);
	std::vector<std::vector<uint8_t>> frame_messages(
			const std::vector<ProtocolMessage> &messages) {
		return frame_messages(messages, packet_ceiling_bytes());
	}
	std::vector<std::vector<uint8_t>> frame_messages(
			const std::vector<ProtocolMessage> &messages, std::size_t max_packet_bytes);
	// cs_dir0 field 13 as a byte count; a negative stored value reads as BuildOutgoingPackets'
	// signed compare does, below its 26-byte floor.
	std::size_t packet_ceiling_bytes() const {
		return conn_.timeouts.max_packet_bytes < 26
				? std::size_t{26}
				: static_cast<std::size_t>(conn_.timeouts.max_packet_bytes);
	}

	// Deterministic golden replay: force the in-match connection state so frame_c2s_uplink
	// reproduces a CAPTURED C2S 0x0C datagram byte-for-byte (the ROADMAP "Determinism" seed-inject).
	// session_id = the captured 0x43 header session_id (= ServerAuth.sk); client_key = the captured
	// ClientAuth.ck — the S2C admission gate validates every inbound 0x83 header against it, so a
	// replay that also decodes the capture's S2C stream MUST supply it (0 keeps the local default);
	// client_scrk = the captured client SCRK (its ClientAuth.scrk); next_seq/last_ack = the captured
	// datagram's seq/ack; self_handle/self_type = its 0x0C sub-header. server_scrk lets the same
	// runtime also decode the capture's S2C 0x83 stream. Skips the live handshake — replay/parity only.
	void seed_in_match(uint32_t session_id, uint32_t client_key, std::string client_scrk,
	                   std::string server_scrk, uint32_t next_seq, uint32_t last_ack,
	                   uint16_t self_handle, uint16_t self_type, uint32_t game_type = 0);

	// The shell's kit for the retail loadout-submission pair the world-stream terminator (S2C 0x1A)
	// releases. Retail sends the SAME per-side profile kit twice: first with the fixed Primary-key
	// slot 195 (pre-Player_InitPlayer, the empty slot table leaves the argument unresolved), then
	// with the live g_CurrentWeaponSlot (the equipped combo after the spawn fill). The wire team
	// byte is NOT part of this seam — it is latched from the host's S2C 0x04 tail byte, exactly
	// like retail's byte_A85B48. Unset keeps the capture-default kit, so headless callers (ctests,
	// seed_in_match) preserve today's auto behavior byte-for-byte.
	// [orig: Game_StartMission @0x525836/@0x525c2e -> NetPacket_SendLoadoutSubmit @0x42cdc0]
	// The optional blue/red side blocks below carry the profile content the two PROFILE-sourced
	// submissions read (this pair and the S2C 0x50 reselect). player_class/rows remain the
	// CURRENTLY APPLIED kit and stay the source for the armory ACCEPT re-send, which retail
	// builds from the armory's own edited buffer, not from the profile
	// [orig: WeaponLoadout_ApplyFromBuffer @0x565d94].
	struct LoadoutKit {
		uint8_t player_class = 8;    // wire byte 1 — the profile's class for the assigned side (5..9)
		int32_t equipped_combo = -1; // g_CurrentWeaponSlot after the spawn fill; < 0 = none (195)
		std::vector<LoadoutSubmitEntry> rows; // ADM-resolved kit tuples (unknown names pre-skipped)

		// ONE side block of the player profile: that side's CLASS byte and the kit page
		// the class selects. Retail keeps BOTH sides resident and re-reads the side the
		// wire team byte names every time it submits FROM THE PROFILE — at join
		// [orig: Game_StartMission @0x525767: esi = (team == 1 || team == 3)
		//  ? &g_CharSelClass[slot*0x1080C] : &byte_2559136[slot*0x1080C]; the class is
		//  *(u8*)esi and the page is esi + {6,0x806,0x1006,0x1806,0x2006}[class-5]] and
		// again on the S2C 0x50 team assign [orig: NapiNPClientMsg_TeamAssign
		//  @0x431a35..0x431a9a]. ONE integer selects BOTH the wire class byte and the kit
		// page, which is why a retail kit is always legal for the class it is paired with
		// (the host's accept gate tests the .adm charfilter against that class byte
		//  [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515A36]).
		//
		// A side left `set == false` falls back to the single applied kit above, so every
		// caller that only ever fills player_class/rows keeps its behavior byte-for-byte.
		struct SideKit {
			bool set = false;
			uint8_t player_class = 8;
			std::vector<LoadoutSubmitEntry> rows;
		};
		SideKit blue; // the block wire team bytes 1 and 3 select
		SideKit red;  // the block every other wire team byte selects
		// The side block a wire team byte names [orig: @0x525767 / @0x431a35].
		const SideKit &side_for_team(uint8_t team) const {
			return (team == 1 || team == 3) ? blue : red;
		}
	};
	void set_loadout_kit(LoadoutKit kit) {
		loadout_kit_ = std::move(kit);
		loadout_kit_set_ = true;
	}

	// The current profile's per-side character selections, serialized into the
	// game-session ClientAuth as CI0/CI1/TR/CTA/CTB/VCA/VCB. Configure this before
	// start(); the binding resolves the packed ids and avatar bytes from its
	// mounted Avatars.def. Headless callers retain the stock fresh-profile values.
	// [orig: PlayerProfile_InitDefaults @0x54bbe0..0x54bc24 ->
	//  CNapiServerInfo_SerializeToSession @0x4c3a1b..0x4c3c4c]
	void set_character_join_vars(CharacterJoinVars vars) {
		character_join_vars_ = vars;
	}
	// Configure the retail game-session request before start(). Spectator emits
	// JSR=1 and optional JSPP; join_password emits JSP for a side/squad credential.
	// The transport PW, JSP and spectator JSPP remain separate.
	void set_join_request(JoinRole role, std::string spectator_password,
			std::string server_password, std::string join_password = {}) {
		join_role_ = role;
		spectator_password_ = std::move(spectator_password);
		server_password_ = std::move(server_password);
		join_password_ = join_password.substr(0, 63);
	}

	// The install root the JOIN VERSIONCRCSTRING checksum is computed from
	// (D-NET-166): at JOIN-build time the joiner CRCs the loose
	// expansion/<SUS2>/version.txt under this root, exactly the file retail's
	// pre-connect expansion switch had already checksummed into
	// g_ExpansionChecksum [orig: Expansion_LoadAssets @0x4a4885;
	// NapiNP_WriteClientAuthPayload @0x42a287]. Unset (empty) keeps the golden
	// "0" — the no-version.txt install every capture used.
	// The same root's CC.BIN is the ClientAuth COUNTRYCODE (vfs_country_code).
	void set_expansion_version_root(std::string game_root) {
		expansion_version_root_ = std::move(game_root);
	}
	// The ClientAuth TZB: the OS time-zone bias in minutes (UTC = local + bias),
	// sent only when nonzero; defaults to local_time_zone_bias_minutes().
	void set_time_zone_bias(int32_t bias_minutes) { time_zone_bias_ = bias_minutes; }

	// The game-session APPID — the decimal the retail client recovers from the
	// NWJoin .joi CK and uploads as the ClientAuth APPID conn-tag; a NovaWorld
	// host validates it (reject code 9). Configure before start(); unset keeps
	// "0" (the LAN default; LAN sends no APPID and no host checks one).
	// [orig: net_config.bt = atol(decoded CK) @0x569b8e; the host reads the
	//  APPID tag into that field, NapiNetConfig_LoadFromConnTags @0x4c7260 ->
	//  Server_ValidatePlayerJoinRequest @0x512100 @0x5122c5]
	void set_app_id(std::string token) {
		app_id_ = token.empty() ? std::string("0") : std::move(token);
	}
	const std::string &app_id() const { return app_id_; }

	// The CD identity cookie (packed PUB* blob) the C2S 0x00 JOIN relays: the
	// NovaWorld-issued NAMEINFO/PCID/SQUADINFO/JOINTICKET the host validates
	// (codes 23/24/25/28). Empty for LAN / an unauthenticated join.
	// [orig: the "CD" TLV in NapiNP_WriteClientAuthPayload @0x42a180]
	void set_cd_cookie(std::vector<uint8_t> cookie) {
		cd_cookie_ = std::move(cookie);
	}
	const std::vector<uint8_t> &cd_cookie() const { return cd_cookie_; }

	// Anti-cheat character-attribute challenge source. This is the exact
	// sixteen-row table loaded from charattr.def at boot, not AnimMap/.adm data.
	// S2C 0x41 mutates the retained table in receive order.
	void set_charattr_challenge_table(CharAttrChallengeTable table) {
		charattr_challenge_table_ = std::move(table);
	}
	void clear_charattr_challenge_table() { charattr_challenge_table_ = {}; }
	// The LIVE table (post every S2C 0x41 applied so far) -- the HUD's
	// per-class ATTRIBUTES words read from here, the same g_CharAttr the
	// checksum hashes [orig: AnimMap_IsSlotActive @0x4125e0].
	const CharAttrChallengeTable &charattr_challenge_table() const {
		return charattr_challenge_table_;
	}

	// Install one exact retail-corpus anti-cheat source profile. Unknown ids
	// clear any previous profile and return false. With no profile—or when a
	// profile does not cover the requested in-range record—S2C 0x30/0x31 remain
	// deliberately unanswered because a guessed reply is grounds for a punt.
	bool set_integrity_challenge_profile(std::string_view id);
	void clear_integrity_challenge_profile() {
		integrity_challenge_profile_ = nullptr;
	}

	// The C2S 0x3D source is NOT the decoded network-entity registry. Retail freezes a
	// renderer-side snapshot of loaded, non-foliage .3DI definitions after mission model
	// loading and pages its stored dwords verbatim in groups of at most fifty. Preserve
	// order and duplicates: the retail producer owns both. OpenNova's production renderer
	// currently supplies one zero dword per unique loaded definition, matching the settled
	// values observed in this binary while retaining the exact paging seam for richer data.
	void set_loaded_model_challenge_snapshot(std::vector<uint32_t> values) {
		loaded_model_challenge_snapshot_ = std::move(values);
	}
	ProtocolMessage make_loaded_model_page_reply(uint32_t start_index) const;

	// The mid-session loadout RE-submission — the armory ACCEPT leg: one C2S 0x2F composed
	// from the current kit seam under the latched team, slot = the live equipped combo.
	// Valid only after the initial 0x1A pair went out (retail's armory exists only
	// in-world); returns an empty vector otherwise. [orig: WeaponLoadout_ApplyFromBuffer
	// @0x565d94 -> NetPacket_SendLoadoutSubmit(entity Team, armory class, buffer,
	// g_CurrentWeaponSlot); a non-authority client resets its slot pool first and the
	// S2C 0x5A grant refills it]
	std::vector<uint8_t> frame_loadout_resubmit();
	// Semantic-message form for ClientRuntime's shared send boundary. This performs the same stage
	// validation as frame_loadout_resubmit without allocating a separate sequenced datagram.
	bool prepare_loadout_resubmit(ProtocolMessage &message_out) const;

	// Anti-cheat challenge traffic counters. Every field is bookkeeping around
	// the existing handlers (no wire effect): a live join that punts can be
	// read back — did challenges arrive, and did the deliberate-silence policy
	// (no integrity profile) leave 0x30/0x31 unanswered.
	struct ChallengeDiagnostics {
		uint32_t entity_checksum_seen = 0;     // S2C 0x30 received
		uint32_t entity_checksum_answered = 0; // C2S 0x20 replies queued
		uint32_t loadout_crc_seen = 0;         // S2C 0x31 received
		uint32_t loadout_crc_answered = 0;     // C2S 0x21 replies queued
		uint32_t charattr_seen = 0;            // S2C 0x39 received (always answered)
		uint32_t charattr_row_missing = 0;     // 0x39 answered via the zero no-row arm
		uint32_t property_clears = 0;          // S2C 0x41 table mutations applied
	};
	const ChallengeDiagnostics &challenge_diagnostics() const {
		return challenge_diagnostics_;
	}
	// The last CR=0 ServerAuth rejection's raw fields, retained past fail() so
	// diagnostics can name the reject family after the mapped string is gone.
	struct JoinRejectRecord {
		bool set = false;
		uint32_t jfc = 0;
		uint32_t jfp = 0;
		std::string jfs;
	};
	const JoinRejectRecord &last_join_reject() const { return last_join_reject_; }
	// The connection's LATCHED disconnect record (retail's conn+0x654 NapiNPDisconnectEvent):
	// the first host description punt / 0x86 goodbye received, the silence-reap CLNTTMOUT, a
	// pool-overflow MSGCRE, or the user-leave CIDEMIS — whichever came first. disconnect()
	// serializes it into the 0x46 burst.
	bool has_disconnect_event() const { return disconnect_event_set_; }
	const DisconnectEvent &last_disconnect_event() const {
		return last_disconnect_event_;
	}
	// The cs_dir0 values this connection runs under: the template, overlaid by the host's 0x82 CS
	// block at acceptance and by later cs_dir0 H:0x00 updates (fields 0, 1, 4, 5, 11, 13).
	const SessionTimeoutConfig &session_timeouts() const { return conn_.timeouts; }

	// Inbound-gap diagnostics: how many future S2C packets are queued behind an
	// unresolved sequence gap, and how many reliable outbound records remain
	// retained (unACKed). A queue that never drains or retention that only grows
	// is the frozen-session signature.
	std::size_t inbound_gap_depth() const { return conn_.seq.queued_inbound.size(); }
	std::size_t retained_outbound_depth() const {
		return static_cast<std::size_t>(conn_.seq.retained_outbound_message_count);
	}
	// Finish the current open client send boundary after every packet (including
	// MTU splits and an optional active-send probe) has been framed.
	void complete_send_flush() { complete_session_send_flush(conn_.seq); }
	uint32_t send_flush_counter() const { return conn_.seq.send_flush_counter; }

	// A deployment pick is owed by the player (AwaitDeployPick), or sent and awaiting the
	// host's release (AwaitDeployRelease). The shell's deploy screen shows while true;
	// this UI state is independent from in_match() and the runtime's gameplay/uplink gate.
	bool deployment_pick_pending() const {
		return post_auth_stage_ == PostAuthStage::AwaitDeployPick ||
		       post_auth_stage_ == PostAuthStage::AwaitDeployRelease;
	}
	// The player-paced half alone: the pick is owed and not yet sent.
	bool awaiting_deploy_pick() const { return post_auth_stage_ == PostAuthStage::AwaitDeployPick; }
	// Monotonic initial-admission boundary: the host's deployment policy and
	// BOTH profile-side 0x5A grants have been consumed. This is deliberately
	// independent from Phase::InMatch (the first grant opens gameplay early)
	// and from deployment_pick_pending() (the shell routes that UI separately).
	// Redeploy and terminal session loss do not erase the historical boundary.
	bool initial_admission_complete() const {
		return initial_admission_complete_;
	}
	// Frame one C2S 0x0E [i16 wire_value] deployment pick (0xFFFF parameter-0 default,
	// 0xFFFE auto team spawn, else a pool<<12|slot spawn-target handle). Accepts an
	// initial overlay selection after admission, AwaitDeployPick, and a re-pick in
	// AwaitDeployRelease. ClientRuntime gates initial selection on the active overlay.
	std::vector<uint8_t> frame_deployment_pick(uint16_t wire_value);
	// Semantic-message form for ClientRuntime's shared send boundary. A successful preparation
	// records the sequence about to be framed so the later S2C 0x5A must cumulatively ACK this pick.
	bool prepare_deployment_pick(
			uint16_t wire_value, ProtocolMessage &message_out);
	// Death returns an established connection to the pick/release portion of the deployment FSM.
	// The self handle and authenticated transport remain valid; a covering 0x5A release returns
	// Phase::InMatch and raises PollResult::reached_in_match again.
	bool begin_redeployment();

	// Retail completes the 0x00 -> 0x01 -> 0x02 exchange and learns the mission from S2C 0x7B before it
	// begins the load/spawn drive. A binding that must load that advertised mission sets this false
	// before start(), keeps pumping the handshake, then flips it true after the world is installed.
	// Direct-loaded callers retain their historical behavior through the true default.
	void set_world_ready(bool ready) { world_ready_ = ready; }
	bool world_ready() const { return world_ready_; }
	bool mission_known() const { return mission_known_; }
	// The exact S2C 0x0B body copied by retail into g_BmsHeaderBlock. This is
	// the non-authority joiner's mission identity source; no canonical encode or
	// local .bms read may replace its bytes. A body is retained only at the
	// witnessed exact 616-byte width.
	// [orig: NapiNPClientMsg_HandleBMSHeader (0x0B) @0x422660]
	bool has_mission_header() const { return !mission_header_bytes_.empty(); }
	const std::vector<uint8_t> &mission_header_bytes() const {
		return mission_header_bytes_;
	}
	// Complete `.til` image reconstructed from the load-only paged S2C 0x45
	// stream. Partial pages remain private so an embedder cannot install an
	// incomplete terrain override. Header and 12-byte tile records are copied
	// from the received bodies verbatim.
	TerrainTilState terrain_til_state() const { return terrain_til_state_; }
	bool has_terrain_til() const {
		return terrain_til_state_ == TerrainTilState::Complete;
	}
	const std::vector<uint8_t> &terrain_til_bytes() const {
		return terrain_til_bytes_;
	}
	// Retail's safe synchronous-load boundary: the pre-world admission exchange has completed and
	// S2C 0x11 was ACKed, but C2S 0x0A still waits for the local mission to be installed.
	bool preload_ready() const { return preload_ready_; }

	// SESSION LOSS, from either of the two witnessed causes.
	//
	// (1) The host EXPLICITLY closes the session by sending the connection-description
	// record (settings flag + tag 3 = full tag 0x103). That is retail's own punt
	// channel — a stock host fires it for a CRC mismatch, a violation sweep, and (the
	// case that exposed this gap) the six-minute deploy-screen idle timeout. Receipt is
	// terminal at ANY stage: the joiner moves to Phase::Error so nothing keeps pumping a
	// closed session, and the FIRST such record wins, exactly like retail's
	// store-if-not-valid event slot.
	// (2) Nothing arrives for the JO connection template's cs_dir0 `timeout_ms` (120000 ms,
	// or the host's advertised override; < 0 disables the reap) — the fallback for a host
	// that vanishes without a goodbye. Armed from the accepted 0x82 (retail state 5) onward
	// with no gameplay gate: the join stages, the world stream, the deploy screen and the
	// in-match phases all reap the same way; the shell's 60 s admission watchdogs merely
	// run alongside it. The reap is evaluated by the send pump (pump()), so in match it
	// fires at the first open send boundary past the window (reap_silent_session). (3) The host's 0x86 SERVER_GOODBYE burst (its reap, StopServer, a
	// replacement, its answer to our own leave) is the third cause, handled like (1).
	//
	// Neither raises an in-world dialog in retail: the disconnect handler clears the
	// session strings and maps the reason code onto g_MissionExitReason / an error
	// screen, i.e. it EXITS THE MISSION with a reason. Our shell's analog is
	// return-to-menu with the reason surfaced the way a join failure is.
	// [orig: the punt path Server_LogCRCMismatchPunt @0x517ed0 ->
	//  CNapiNPConnection_SendChatMessage @0x4c7ef0 -> NapiNPDataTransfer_SendDescription
	//  @0x628c80, received by CNapiNPConnection_HandleDescriptionPacket @0x621ae0;
	//  the cs_dir0.timeout_ms reap @0x4ca4a0 (stores @0x4caa81/@0x4cab54); both land in
	//  CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
	bool session_lost() const;
	// Empty while healthy; a player-facing reason once lost — the decoded reason code, class
	// and strings for an explicit close, the silence window for the reap.
	std::string session_loss_reason() const;
	// Milliseconds since the last VALID inbound datagram on this connection (0
	// before the first one). Diagnostics + the loss predicate share this clock.
	uint64_t milliseconds_since_last_receive() const;
	// The link-error callbacks this connection fired since the last take
	// (kNetQualityLinkError* bits): a received 0x84 resend list that named a
	// sequence, a sent 0x44 missing-sequence request that named one. The
	// owner raises the connection indicators' flags from them.
	uint32_t take_net_quality_link_errors() {
		const uint32_t mask = net_quality_link_errors_;
		net_quality_link_errors_ = 0;
		return mask;
	}

	Phase phase() const { return phase_; }
	// Admission-stage name for diagnostics (the shell's post-load join watchdog names the
	// stage a stalled join is parked in). Not a wire surface.
	const char *post_auth_stage_name() const;
	bool in_match() const { return phase_ == Phase::InMatch; }
	// The authenticated session (retail g_NapiNPCtx.is_in_session): Driving
	// or InMatch — a dead player re-entering the deploy flow is still in it.
	bool in_session() const { return phase_ == Phase::Driving || phase_ == Phase::InMatch; }
	// The host's session-variable list parsed from the reassembled S2C 0x60
	// server-info transfer (npwire/session_vars.h): the server name and
	// mission title the Tab board's header reads, and the EXP_FANFARE u16 (lo
	// byte = the KILLTONE threshold, hi byte = the HEADSHOTTONE threshold)
	// [orig: Client_ParseServerSessionVariables @0x5202f0, the store @0x520478
	// -> g_SessionVarExpFanfare @0x24d5a10]. Empty / 0 until the transfer lands.
	const SessionVars &session_vars() const { return session_vars_; }
	uint16_t exp_fanfare() const { return session_vars_.exp_fanfare; }
	bool has_self_handle() const { return has_self_handle_; }
	uint16_t self_handle() const { return self_handle_; } // the wire handle H
	// The server-assigned team latched from the S2C 0x04 tail byte, and re-latched by the
	// S2C 0x50 team assign [orig: byte_A85B48 — @0x425499 then @0x4319db]. This is the
	// side selector a kit build must use: teams 1 and 3 are the BLUE block, everything
	// else the RED block. Before the 0x04 latch it reads 0 — exactly like retail's
	// zero-initialized global — which selects the RED block; a real admission always
	// delivers 0x04 ahead of the 0x1A submission trigger, and a harness that cares must
	// feed the 0x04 it expects.
	uint8_t assigned_team() const { return assigned_team_; }
	bool spectator() const { return spectator_mode_; }
	// Retail's host-global class availability word, replaced by every S2C 0x76.
	// The malformed/short handler value is zero; before the initial-state message,
	// retain the stock all-ten-classes default used by the local UI.
	// [orig: NapiNPClientMsg_HandleClassAllowMask @0x42d540]
	uint16_t class_allow_mask() const { return class_allow_mask_; }
	// This joiner's own roster slot id, latched from S2C 0x04 body byte 17 (retail's
	// g_LocalPlayerSlotId; the host's mirror is its per-player record slot+20
	// [orig: NetPacket_WriteSlotAssignment @0x502b30]). The fired-round hit_part word
	// carries it in bits 9..15, so 0 attributes our shots to the HOST's own slot.
	uint8_t local_player_slot() const { return local_player_slot_; }
	const SelfSpawn &spawn_pose() const { return spawn_; }
	uint32_t game_type() const { return game_type_; }
	// Authoritative mission-session attributes retained from the fixed 180-byte
	// S2C 0x64 block (little-endian dword at offset 44).
	uint32_t mp_attributes() const { return mp_attributes_; }
	// The joined session's published player cap, the same block's dword at
	// offset 36: the host writes it from its own BMS admission limit [orig:
	// Client_BuildMissionDataRequestBlock @0x51e89f <- dword_24D211C, the limit
	// Entity_SpawnFromBMSRecord @0x40ea86 admits against; a dedicated host
	// publishes one less @0x51e8ab]. 0 until the transfer completes. Retail
	// joiners never admit, so only an embedder's own full-BMS load reads it
	// (Simulation's stamp_admission_limits).
	uint32_t session_max_players() const { return session_max_players_; }
	const std::string &server_name() const { return server_name_; }
	const std::string &mission_name() const { return mission_name_; }
	const std::string &map_file() const { return map_file_; }
	const std::string &expansion() const { return expansion_; }
	const std::string &player_name() const { return player_name_; }
	uint32_t server_key() const { return conn_.server_sk; }
	// This connection's own numeric key (ClientAuth.CK; fresh per start()) — the
	// receiver-local key a host's 0x84 resend request addresses us by.
	uint32_t client_key() const { return client_key_; }
	uint32_t monotonic_milliseconds32() const {
		return static_cast<uint32_t>(monotonic_milliseconds_() & 0xFFFFFFFFu);
	}
	// The client's own ping: the last completed S2C 0x57 round trip and the
	// ten-entry ring's mean (see rtt_ring_ below) [orig: dword_A860D4;
	//  CNetStats_GetAveragePing @0x4C2750].
	uint32_t client_ping_ms() const { return rtt_current_ms_; }
	uint32_t client_average_ping_ms() const {
		uint32_t sum = 0;
		for (const uint32_t sample : rtt_ring_) sum += sample;
		return sum / static_cast<uint32_t>(rtt_ring_.size());
	}
	// The outer 0x45/0x85 connection ping's rtt (conn->session_keys.rtt_ms);
	// 0 until the host answers one [orig: Nwu_HandlePing @0x623C9B].
	uint32_t session_ping_ms() const { return session_rtt_ms_; }
	const std::string &client_scrk() const { return conn_.client_scrk; }
	const std::string &server_scrk() const { return conn_.server_scrk; }
	const std::string &last_error() const { return last_error_; }

	// The client-side connection node carrying this joiner's SCRK/seq/ack (type 2).
	const NapiNPConnection &connection() const { return conn_; }

private:
	enum class PostAuthStage {
		Inactive,
		AwaitServerSettings,
		AwaitJoinAck,
		AwaitPaddingProbe,
		AwaitGameStart,
		AwaitServerInfo,
		AwaitMissionData,
		AwaitPlayerList,
		AwaitInitialSyncTail,
		AwaitWorldStreamEnd,
		AwaitDeployment,
		AwaitDeployPick,    // player-paced: grants+policy complete, the pick is the player's
		AwaitDeployRelease,
		Complete,
	};

	// The PROFILE-sourced C2S 0x2F body for a wire team byte: the side block that team
	// names when the binding supplied it, else the single applied kit, else the
	// capture-default kit. `alternate` selects the second submission's live equipped slot
	// (the first submission is the fixed pre-Player_InitPlayer 195).
	// [orig: Game_StartMission @0x525767 / NapiNPClientMsg_TeamAssign @0x431a35 ->
	//  NetPacket_SendLoadoutSubmit @0x42cdc0]
	LoadoutSubmit build_profile_loadout_submit(uint8_t team, bool alternate) const;

	std::vector<uint8_t> build_client_hello();
	std::vector<uint8_t> build_client_auth();
	std::vector<uint8_t> frame_session(const std::vector<ProtocolMessage> &messages);
	std::vector<uint8_t> frame_inner_pending(
			uint8_t tag, std::vector<uint8_t> body) {
		return frame_session({make_protocol_message(tag, std::move(body))});
	}
	std::vector<uint8_t> frame_retained_session(uint32_t sequence);

	void on_server_hello(const std::vector<uint8_t> &body, PollResult &out);
	void on_server_auth(const std::vector<uint8_t> &body, PollResult &out);
	void on_server_session(const std::vector<uint8_t> &body, PollResult &out);
	void retire_self_handle(uint16_t handle);
	void apply_self_spawn(uint16_t handle, bool has_body, uint32_t owner,
			uint16_t flags, const SelfSpawn &spawn, PollResult &out);
	void on_server_resend_list(const std::vector<uint8_t> &body, PollResult &out);
	// The paged host lists a client walks with its own C2S reply
	// (joiner_connection_walks.cpp): the team-change list's S2C 0x51 -> C2S 0x29
	// {index + 1} and the join-window kill list's S2C 0x4E -> C2S 0x28
	// {0x19 value, 0x1A value, resume}. The pages' entity folds are the
	// replica pipeline's.
	void on_list_walk_page(const ProtocolMessage &m, std::vector<ProtocolMessage> &replies);
	// S2C 0x86 SERVER_GOODBYE: the host's teardown burst — keyed by OUR CK, its record latched
	// with the peer role 1, answered with the 0x46 burst, then terminal like a description punt.
	void on_server_goodbye(const std::vector<uint8_t> &body, PollResult &out);
	// S2C 0x85 the outer connection ping: keyed by OUR CK, `WR` set = the host wants our 0x45
	// pong of its `MS` stamp, clear = the pong of our own ping (rtt = now - MS). Either form
	// refreshes the reap clock exactly like an admitted 0x83.
	void on_server_ping(const std::vector<uint8_t> &body, PollResult &out);
	// Store `event` (with `role` as DS) as the connection's disconnect record only while none is
	// latched — retail's store-if-!valid slot. The 128/32-byte record caps apply.
	void latch_disconnect_event(const DisconnectEvent &event, uint32_t role);
	// The send pump's silence reap (PumpStateMachine case 5): past the window it latches the
	// CLNTTMOUT record, enters Phase::Error and returns true (joiner_connection_disconnect.cpp).
	bool reap_silent_session();
	void retain_mission_metadata_chunk(const FileTransferChunk &chunk);
	// Accumulate the S2C 0x60 server-info transfer and, at its final chunk,
	// walk the `[key\0][u32 len][bytes]` VarList for EXP_FANFARE.
	void retain_server_info_chunk(const FileTransferChunk &chunk);
	void retain_terrain_load_page(const std::vector<uint8_t> &body);
	void reset_terrain_load();
	void invalidate_terrain_load();
	void on_host_disconnect(const DisconnectEvent &event);
	void fail(std::string reason);

	uint32_t client_index_ = 1;
	uint32_t client_key_ = 1;
	std::string player_name_;
	JoinRole join_role_ = JoinRole::Player;
	std::string spectator_password_;
	std::string server_password_;
	std::string join_password_; // JSP: optional side/squad credential, up to 63 bytes
	bool server_password_required_ = false;
	bool spectator_mode_ = false;
	Phase phase_ = Phase::Idle;

	// SCRK / seq / ack live on the client-side connection node (folded, as on the server side).
	NapiNPConnection conn_;
	// Protocol FIRST/MID/FINAL records can span sequenced 0x83 packets. Keep the
	// retail receive buffer on the connection so only the completed semantic
	// payload reaches gameplay dispatch.
	ProtocolReassemblyState s2c_reassembly_;
	std::string advertised_expansion_; // ServerHello.SUS2, echoed as C2S JOIN EXP
	std::string expansion_version_root_; // install root: the JOIN checksum (D-NET-166), CC.BIN (D-NET-296)
	int32_t time_zone_bias_ = local_time_zone_bias_minutes(); // ClientAuth TZB (D-NET-296)
	std::string app_id_ = "0"; // ClientAuth APPID (decoded .joi CK); "0" = LAN default
	std::vector<uint8_t> cd_cookie_; // 0x00 JOIN CD identity cookie (packed PUB* blob)

	uint32_t server_hk_ = 0;    // ServerHello.hk — echoed in ClientAuth.hk (transient)
	DiscoveredSession discovered_; // the browse row's 0x81 record (survives start())
	// Pre-session UDP legs are reliable-by-retransmit in retail. Cache the already-framed bytes so
	// retrying never regenerates identity/session material (especially ClientAuth CK/SCRK).
	MonotonicMilliseconds monotonic_milliseconds_;
	std::vector<uint8_t> handshake_retry_datagram_;
	uint64_t handshake_last_send_ms_ = 0;
	// When the 0x42 connect state began (the ServerHello): its 30000 ms window's start.
	uint64_t client_join_start_ms_ = 0;
	bool handshake_retry_clock_armed_ = false;
	// Wall-clock stamp of the last IN-ORDER admitted 0x83 (a zero-message keepalive counts;
	// duplicates, futures, 0x84 resend lists and every other opcode do not) or of a
	// key-matched 0x85 ping — retail's per-connection reap clock conn+0x5E8, initialized at
	// state-5 entry (the accepted 0x82) and measured against cs_dir0.timeout_ms.
	// [orig: CNapiNPConnection_OnStateChange @0x62612f (init); CNapiNPConnection_ParseMessages
	//  @0x625d54 (stamp); Nwu_HandlePing @0x623c56 (the 0x85 ping stamps it too);
	//  read by PumpStateMachine @0x6295b2]
	uint64_t last_receive_ms_ = 0;
	bool receive_clock_armed_ = false;
	uint32_t net_quality_link_errors_ = 0; // take_net_quality_link_errors()
	bool silence_timeout_latched_ = false;
	PostAuthStage post_auth_stage_ = PostAuthStage::Inactive;
	uint64_t session_last_send_ms_ = 0;
	bool session_send_clock_armed_ = false;
	bool goodbye_sent_ = false; // disconnect() burst emitted; retail clears the keys right after
	bool session_ack_pending_ = false; // admitted S2C messages with no substantive C2S reply yet
	bool world_ready_ = true;   // binding-controlled: the wire-header world/assets are staged
	bool mission_known_ = false; // structurally valid authoritative S2C 0x7B received
	// The joiner's server-assigned team, latched from the S2C 0x04 slot-assignment tail byte —
	// the 0x2F loadout-submit header byte 0 source [orig: NapiNPClientMsg_SessionSlotConfig (0x04) @0x425410 ->
	// byte_A85B48 @0x425499]. Zero-initialized like retail's global: 0x04 always precedes the
	// 0x1A submission trigger in a real admission, so harnesses must feed the 0x04 they expect.
	uint8_t assigned_team_ = 0;
	uint16_t class_allow_mask_ = 0x03FFu;
	uint8_t local_player_slot_ = 0; // S2C 0x04 body byte 17 (see local_player_slot())
	// Inclusive final roster slot for the S2C 0x46 queue-ack walk, supplied by
	// the same S2C 0x04 body at byte 18. A value of 4 walks slots 0..4.
	// [orig: g_MaxPlayerSlots read at NapiNPClientMsg_PlayerSync @0x431370]
	uint8_t max_player_slot_ = 0;
	// Client net-frame counter echoed as the padding echo's third dword. Retail increments its
	// global once per client net pump and never resets it for a rejoin.
	// [orig: @0xA822A4, ++ in NetClient_HandleGameEnd @0x424752]
	uint32_t net_frame_counter_ = 0;
	LoadoutKit loadout_kit_;      // binding-injected submission content (see set_loadout_kit)
	bool loadout_kit_set_ = false;
	CharacterJoinVars character_join_vars_{};
	uint8_t current_player_class_ = 0; // authoritative S2C 0x5A avatarClass
	// Boot-loaded g_CharAttr[16] bytes. Ordered S2C 0x41 property clears
	// mutate this retained table before every later 0x39 challenge.
	CharAttrChallengeTable charattr_challenge_table_{};
	const IntegrityChallengeProfile *integrity_challenge_profile_ = nullptr;
	// Frozen renderer-definition snapshot for S2C 0x68 -> C2S 0x3D. Configuration
	// survives start(): bindings may finish loading models before the first net pump.
	std::vector<uint32_t> loaded_model_challenge_snapshot_;
	bool preload_ready_ = false; // terminal pre-world S2C 0x11 received and ACK boundary reached
	bool sync_tail_seen_ = false; // the host emits 0x11 exactly once; latch an early arrival
	bool player_list_seen_ = false; // valid S2C 0x16 may arrive before the 0x64 transfer completes
	bool deployment_policy_seen_ = false; // a valid S2C 0x0F supplied gameFlags (completes admission)
	uint8_t initial_loadout_grant_count_ = 0; // both profile-side 0x5A grants precede the pick
	bool initial_admission_complete_ = false;
	bool deployment_pick_sent_ = false;
	uint32_t deployment_pick_sequence_ = 0; // release 0x5A must cumulatively ACK this C2S 0x0E
	bool deployment_pick_sequence_unbound_ = false; // queued, its packet not yet built
	bool deployment_reply_seen_ = false; // applicable spawn release seen; pairs with self H
	bool pending_spawn_menu_request_ = false; // S2C 0x11 arrived while the local world was held
	// S2C 0x19 supplies dword 0 of the post-world C2S 0x28 request. Retail clears
	// this scalar on a malformed body and retains the latest valid value.
	// [orig: NapiNPClientMsg_0x019 @0x425e80 -> dword_A82360]
	uint32_t spawn_ack_timestamp_ = 0;
	// S2C 0x1A's dword, the window max of the 0x4E continuation's C2S 0x28;
	// a body under four bytes stores 0. [orig: NapiNPClientMsg_0x01A @0x425eb0
	// -> dword_A82364 @0x425ec3/@0x425ecb; read @0x4318e8]
	uint32_t game_start_ack_timestamp_ = 0;
	// The client's own round-trip measurement: every S2C 0x57 with the echo
	// flag CLEAR is the pong of our C2S 0x2C ping, and its `now - timestamp`
	// lands in a ten-entry ring (the index wraps at 10) plus the current-ping
	// word; the ring's mean (all ten slots, zero-initialized slots included)
	// is the quality metric's ping term. [orig: NapiNPClientMsg_0x057_RTT
	//  @0x432210 — the flag=0 arm @0x432280 (rtt into
	//  stru_A86920.ring[index], dword_A860D4 = rtt, ++index >= 10 -> 0);
	//  CNetStats_GetAveragePing @0x4C2750 = sum(ring[0..9]) / 10]
	std::array<uint32_t, 10> rtt_ring_{};
	uint32_t rtt_ring_index_ = 0;
	uint32_t rtt_current_ms_ = 0;
	// The OUTER connection ping's own measurement (conn->session_keys.rtt_ms):
	// a 0x85 with WR clear is the host's pong of our 0x45 [orig: Nwu_HandlePing
	//  @0x623A70, the rtt store @0x623C9B].
	uint32_t session_rtt_ms_ = 0;

	bool has_self_handle_ = false;
	uint16_t self_handle_ = 0;  // wire handle H, learned via the owning connection ID (pool<<12|slot)
	SelfSpawn spawn_;
	uint32_t game_type_ = 0;    // authoritative g_GameType learned from S2C 0x08 field 3 / 0x7B extra
	uint32_t mp_attributes_ = 0; // S2C 0x64 fixed session block, offset 44
	uint32_t session_max_players_ = 0; // S2C 0x64 fixed session block, offset 36
	uint32_t mission_metadata_transfer_id_ = 0;
	uint32_t mission_metadata_total_size_ = 0;
	uint32_t server_info_transfer_id_ = 0;
	std::vector<uint8_t> server_info_bytes_;
	SessionVars session_vars_;
	// The 0x64 block's [36, 48) window (the player-cap, game-type and mpattrib
	// dwords) assembled across chunk boundaries; mask bit i = byte i received.
	std::array<uint8_t, 12> mission_metadata_fixed_bytes_{};
	uint16_t mission_metadata_fixed_byte_mask_ = 0;
	std::string server_name_;   // authoritative S2C 0x7B field 3
	std::string mission_name_;  // authoritative S2C 0x7B field 4 (title or waypoint filename)
	std::string map_file_;      // authoritative S2C 0x7B field 5
	std::string expansion_;     // authoritative S2C 0x7B field 7
	std::vector<uint8_t> mission_header_bytes_; // exact valid 616-byte S2C 0x0B body
	TerrainTilState terrain_til_state_ = TerrainTilState::Absent;
	std::vector<uint8_t> terrain_til_staging_; // header + received tile slots
	uint32_t terrain_tile_count_ = 0;
	uint32_t terrain_next_index_ = 0; // canonical retail-writer cursor
	std::vector<uint8_t> terrain_til_bytes_; // published only when coverage is complete

	std::string last_error_;
	// The host's explicit close, latched from the first connection-description record and
	// never overwritten [orig: the store-if-!valid event slot @0x621d3c]. Non-empty is the
	// terminal session-loss state, independent of the receive-silence clock.
	std::string host_disconnect_reason_;
	ChallengeDiagnostics challenge_diagnostics_{};
	JoinRejectRecord last_join_reject_{};
	DisconnectEvent last_disconnect_event_{};
	bool disconnect_event_set_ = false;
};

} // namespace opennova::inmatch
