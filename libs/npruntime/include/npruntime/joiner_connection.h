#pragma once

#include "npruntime/charattr_challenge.h"
#include "npruntime/napi_np_connection.h"

#include <npwire/ingame_decode.h>     // OrganicSpawnBatch / PlayerExtendedUplink / EntityPacketSubHeader
#include <npwire/protocol_message.h>  // ProtocolMessage
#include <npwire/session_hello.h>     // DisconnectEvent

#include <cstddef>
#include <cstdint>
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
//   <- S2C 0x0C organic-spawn (0x83)  : name-match self and retain wire handle H
//   <- S2C 0x1A                       : send the 0x2F/0x2F/0x0B loadout/status bundle (the
//                                       binding's kit via set_loadout_kit, else the capture default)
//   <- initial S2C 0x5A grant pair    : if 0x0F advertises spawn zones, send C2S 0x0E {FFFF}
//   <- applicable final S2C 0x5A      : initial grant (no zones) or post-pick release [-> InMatch]
//   frame_c2s_uplink(H, ...)          : per-frame C2S 0x0C player uplink
//
// SELF-IDENTIFICATION = NAME-MATCH (D.0, docs/net §5.23): the host streams the joiner's admitted
// pool-0 player entity (type 0x14B9) as a named S2C 0x0C organic-spawn record whose entity_name is
// the joiner's player name. The joiner matches it against its own name and adopts record.slot_id as
// its wire handle H (the value it stamps in its C2S 0x0C sub-header so the host's
// apply_player_intent resolves the right peer). The joiner sends NO C2S 0x0C before it knows H.
//
// [orig: NapiNPClientMsg_0x00C @0x42E730 (self name-match), NapiNPClientMsg_0x00F @0x42E200
//  (world-state-load), the joiner C2S in-match burst from the host_and_join_lan capture]. No socket
// I/O lives here — the owner pumps bytes.
namespace opennova::np {

class JoinerConnection {
public:
	using MonotonicMilliseconds = std::function<uint64_t()>;

	enum class Phase {
		Idle,     // nothing sent yet
		Hello,    // ClientHello sent, awaiting ServerHello
		Auth,     // ClientAuth sent, awaiting ServerAuth
		Driving,  // ServerAuth accepted; reactive admission/world/deployment FSM is active
		InMatch,  // self handle is known and the applicable final deployment release was received
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

	// S2C 0x53 and 0x6F mutate one shared retail timer-list entry. Keeping them in
	// one typed FIFO preserves message order when both tags share a protocol packet;
	// separate per-tag vectors would necessarily reorder that packet at the runtime fold.
	using ZoneTimerUpdate = std::variant<ZoneTimerValue, ZoneTimerWindow>;

	struct PollResult {
		std::vector<std::vector<uint8_t>> outbound;   // datagrams to send back to the host
		// Receive handlers queue reliable semantic replies here. ClientRuntime folds all state
		// from this receive boundary first, then places these behind the shared send-holdoff gate
		// so they batch with same-frame housekeeping/gameplay at PumpClientProtocolSend.
		std::vector<ProtocolMessage> queued_send_messages;
		std::vector<std::vector<uint8_t>> inbound_0a; // raw S2C 0x0A bodies (for NetClientView)
		// Raw S2C world-stream spawn/static bodies the host sends during load, as (tag, body):
		// 0x0C organics, 0x0D pool-1, 0x10 statics, 0x20 markers.
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> inbound_world;
		// Live S2C gameplay bodies consumed by NetClientView (currently the 0x49
		// reload echo; 0x0A tag-2 rounds remain embedded in inbound_0a).
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> inbound_gameplay;
		bool reached_in_match = false;                // true when handle discovery + deployment meet
		// S2C 0x61 — the per-player TICK SEED (not an SCRK exchange). The client's whole
		// network-role clock is anchored to it: retail stores it into BOTH currentTick and
		// g_lastKeepaliveTick, and the host stamps the same value into the player slot's
		// fire-freshness expiration. `set` distinguishes "no 0x61 this poll" from a
		// witnessed seed of ZERO (the round-end/disarm form ships four zero bytes).
		// [orig: dispatch row 0x61 (table @0x82ae28) -> NapiNPClientMsg_HandleSessionKey
		//  @0x4297c0: currentTick @0xA8229C <- @0x4297f8, g_lastKeepaliveTick @0xA822A0
		//  <- @0x4297fd; a short body seeds 0 @0x4297eb]
		bool tick_seed_set = false;
		uint32_t tick_seed = 0;
		bool send_holdoff_set = false;
		uint32_t send_holdoff = 0;
		std::vector<WeaponLoadout> loadout_grants;
		std::vector<ZoneTimerUpdate> zone_timer_updates;
		// S2C 0x50 leg 1: this poll re-latched OUR OWN team (the same byte_A85B48
		// latch the S2C 0x04 tail writes). The binding re-styles friend/foe from it.
		// [orig: NapiNPClientMsg_0x050 @0x431910 — byte_A85B48 store @0x4319db]
		bool self_team_changed = false;
		uint8_t self_team = 0;
		// S2C 0x50 leg 2: (handle, team) for every valid assignment in this poll,
		// including our own — the decoded-view fold [orig: @0x4319ee].
		std::vector<std::pair<uint16_t, uint8_t>> entity_team_assigns;
		// S2C 0x5D empty-slot sweep: RAW pool-0 slot indices the host says are empty.
		// [orig: NapiNPClientMsg_DestroyEntityList @0x429730]
		std::vector<uint16_t> destroyed_pool0_slots;
		// S2C 0x46 with bit15 (0x8000): the roster slot's BOOKKEEPING is cleared
		// (active flag, names, team, entity ref). The ENTITY IS NOT DESTROYED on this
		// leg — the entity-field wipes @0x431437 are dead code there because
		// entitySlotPtr is null. [orig: NapiNPClientMsg_PlayerSync @0x431370
		//  @0x431411..0x43144c -> PlayerSlot_ClearAndUnlink @0x434730]
		std::vector<uint8_t> cleared_player_slots;
	};

	// `player_name` is the on-wire game ClientAuth.NA callsign and the local key the joiner
	// name-matches against. ClientHello.CO remains retail's company identity.
	explicit JoinerConnection(std::string player_name);
	// Injectable monotonic wall clock for deterministic hosts/tests. Production uses steady_clock.
	JoinerConnection(std::string player_name, MonotonicMilliseconds monotonic_milliseconds);

	// Begin the handshake. Returns the ClientHello datagram to send (Idle -> Hello).
	std::vector<uint8_t> start();

	// Feed one inbound datagram (off the socket, CRC envelope intact). Returns the reply datagrams +
	// any surfaced 0x0A bodies + whether this datagram learned H.
	PollResult handle_datagram(const uint8_t *raw, std::size_t len);

	// While awaiting 0x81/0x82, re-emit the exact pending 0x41/0x42 wire datagram on retail's active
	// send interval measured by the monotonic wall clock (independent of render/simulation cadence).
	// Once Driving, flush deferred ACKs, loss-recovery probes, and the held C2S 0x0A after local
	// world readiness. Semantic admission transitions are emitted reactively from handle_datagram().
	std::vector<std::vector<uint8_t>> pump(uint32_t now_tick);

	// Frame the retail leave: a burst of identical 0x46 ClientGoodBye datagrams for the owner to
	// ship before dropping the socket (the host's only non-timeout teardown trigger). Empty until
	// ServerAuth assigns the session key, and idempotent — a second call returns nothing.
	// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 -> SendDisconnectPacket
	// @0x61f2a0; send count clamped by cs_dir0.recv_max_per_tick (JO template: 4 —
	// CNapiNetwork_Init @0x4ca4a0 stores @0x4cab60)]
	std::vector<std::vector<uint8_t>> disconnect();

	// Build a C2S 0x0C player uplink datagram: 5-byte sub-header (handle = H, item_type_id = type,
	// sub_op = 0x0A extended) + the 43-byte extended body, wrapped as one 0x43 SESSION packet.
	std::vector<uint8_t> frame_c2s_uplink(uint16_t handle_H, uint16_t type,
	                                      const PlayerExtendedUplink &body);

	// Wrap one inner {tag,body} as a single 0x43 SESSION datagram over this connection's live SCRK +
	// seq (advances the outbound seq, stamps the ack). The framing path the per-frame housekeeping
	// rides (0x34 keepalive / 0x4C net-quality / 0x2C RTT, P6 §5.44) so those messages share the SAME
	// 0x43/SCRK envelope and seq stream as the 0x0C uplink — the witnessed PumpClientProtocolSend flush
	// bundles them into 0x43s the same way. [orig: CNapiNetwork_QueueReliableMessage @0x4c4fa0 ->
	// CNapiNPConnection_QueueMessage @0x628640]
	std::vector<uint8_t> frame_inner(uint8_t tag, std::vector<uint8_t> body) {
		return frame_session({make_protocol_message(tag, std::move(body))});
	}
	// Flush several queued inner messages through as few sequenced packets as fit under the
	// negotiated JO packet ceiling. Retail queues semantic records first and frames at the send
	// boundary; callers use this instead of minting one outer datagram per record.
	std::vector<std::vector<uint8_t>> frame_messages(
			const std::vector<ProtocolMessage> &messages,
			std::size_t max_packet_body_bytes = 1300);

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
	// with the live g_currentWeaponSlot (the equipped combo after the spawn fill). The wire team
	// byte is NOT part of this seam — it is latched from the host's S2C 0x04 tail byte, exactly
	// like retail's byte_A85B48. Unset keeps the capture-default kit, so headless callers (ctests,
	// nw_replay, seed_in_match) preserve today's auto behavior byte-for-byte.
	// [orig: Game_StartMission @0x525836/@0x525c2e -> NetPacket_SendLoadoutSubmit @0x42cdc0]
	// The optional blue/red side blocks below carry the profile content the two PROFILE-sourced
	// submissions read (this pair and the S2C 0x50 reselect). player_class/rows remain the
	// CURRENTLY APPLIED kit and stay the source for the armory ACCEPT re-send, which retail
	// builds from the armory's own edited buffer, not from the profile
	// [orig: WeaponLoadout_ApplyFromBuffer @0x565d94].
	struct LoadoutKit {
		uint8_t player_class = 8;    // wire byte 1 — the profile's class for the assigned side (5..9)
		int32_t equipped_combo = -1; // g_currentWeaponSlot after the spawn fill; < 0 = none (195)
		std::vector<LoadoutSubmitEntry> rows; // ADM-resolved kit tuples (unknown names pre-skipped)

		// ONE side block of the player profile: that side's CLASS byte and the kit page
		// the class selects. Retail keeps BOTH sides resident and re-reads the side the
		// wire team byte names every time it submits FROM THE PROFILE — at join
		// [orig: Game_StartMission @0x525767: esi = (team == 1 || team == 3)
		//  ? &g_charSelClass[slot*0x1080C] : &byte_2559136[slot*0x1080C]; the class is
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
	const CharacterJoinVars &character_join_vars() const {
		return character_join_vars_;
	}

	// Anti-cheat character-attribute challenge source. This is the exact
	// sixteen-row table loaded from charattr.def at boot, not AnimMap/.adm data.
	// S2C 0x41 mutates the retained table in receive order.
	void set_charattr_challenge_table(CharAttrChallengeTable table) {
		charattr_challenge_table_ = std::move(table);
	}
	void clear_charattr_challenge_table() { charattr_challenge_table_ = {}; }

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
	// g_currentWeaponSlot); a non-authority client resets its slot pool first and the
	// S2C 0x5A grant refills it]
	std::vector<uint8_t> frame_loadout_resubmit();
	// Semantic-message form for ClientRuntime's shared send boundary. This performs the same stage
	// validation as frame_loadout_resubmit without allocating a separate sequenced datagram.
	bool prepare_loadout_resubmit(ProtocolMessage &message_out) const;

	// One C2S 0x1D stance-change ([i16 action id] 0xA9 crouch / 0xAA prone / 0xAC
	// stand), sent immediately from the stance key SELECT. Empty before in-match.
	// [orig: @0x4e0d77/@0x4e0df3/@0x4e0e3e -> NapiNPServerMsg_HandleStanceChange @0x501c60]
	std::vector<uint8_t> frame_stance_change(uint16_t action_id);

	// Inbound-gap diagnostics: how many future S2C packets are queued behind an
	// unresolved sequence gap, and how many reliable outbound records remain
	// retained (unACKed). A queue that never drains or retention that only grows
	// is the frozen-session signature.
	std::size_t inbound_gap_depth() const { return conn_.seq.queued_inbound.size(); }
	std::size_t retained_outbound_depth() const {
		return static_cast<std::size_t>(conn_.seq.retained_outbound_message_count);
	}

	// Player-paced deployment (the deploy-map screen). When enabled BEFORE the grants
	// complete, a pick-required join parks in AwaitDeployPick instead of auto-answering
	// C2S 0x0E {FF FF}; the binding then sends the player's pick — repeatedly, if the host
	// silently drops an invalid/contested one (retail re-picks: Input case 12 has no
	// re-entry gate, the screen stays up on the still-set 0x0A flags1 bit1, and each list
	// click queues a fresh 0x0E). Headless callers keep the auto parameter-0 default.
	// [orig: the DEATH screen SPAWNPOINTS_LIST select -> Input_QueueEvent(12, node)
	//  @0x55364d -> Input_HandleActionBinding case 12 @0x49b0c5-0x49b17b]
	void set_player_paced_deployment(bool paced) { player_paced_deployment_ = paced; }
	// A deployment pick is owed by the player (AwaitDeployPick), or sent and awaiting the
	// host's release (AwaitDeployRelease). The shell's deploy screen shows while true.
	bool deployment_pick_pending() const {
		return post_auth_stage_ == PostAuthStage::AwaitDeployPick ||
		       post_auth_stage_ == PostAuthStage::AwaitDeployRelease;
	}
	// Frame one C2S 0x0E [i16 wire_value] deployment pick (0xFFFF parameter-0 default,
	// 0xFFFE auto team spawn, else a pool<<12|slot spawn-target handle). Allowed in
	// AwaitDeployPick and again in AwaitDeployRelease (the re-pick); empty otherwise.
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
	// (2) Nothing arrives for the JO connection template's cs_dir0/cs_dir1 `timeout_ms`
	// (120000 ms) — the fallback for a host that vanishes without a goodbye. Reported
	// only once the connection is in-match; the pre-match admission stall is the shell
	// watchdog's.
	//
	// Neither raises an in-world dialog in retail: the disconnect handler clears the
	// session strings and maps the reason code onto g_mission_exit_reason / an error
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

	Phase phase() const { return phase_; }
	// Admission-stage name for diagnostics (the shell's post-load join watchdog names the
	// stage a stalled join is parked in). Not a wire surface.
	const char *post_auth_stage_name() const;
	bool in_match() const { return phase_ == Phase::InMatch; }
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
	// This joiner's own roster slot id, latched from S2C 0x04 body byte 17 (retail's
	// g_local_player_slot_id; the host's mirror is its per-player record slot+20
	// [orig: NetPacket_WriteSlotAssignment @0x502b30]). The fired-round hit_part word
	// carries it in bits 9..15, so 0 attributes our shots to the HOST's own slot.
	uint8_t local_player_slot() const { return local_player_slot_; }
	const SelfSpawn &spawn_pose() const { return spawn_; }
	uint32_t game_type() const { return game_type_; }
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
	std::vector<uint8_t> frame_retained_session(uint32_t sequence);

	void on_server_hello(const std::vector<uint8_t> &body, PollResult &out);
	void on_server_auth(const std::vector<uint8_t> &body, PollResult &out);
	void on_server_session(const std::vector<uint8_t> &body, PollResult &out);
	void on_server_resend_list(const std::vector<uint8_t> &body, PollResult &out);
	void on_host_disconnect(const DisconnectEvent &event);
	void fail(std::string reason);

	uint32_t client_index_ = 1;
	uint32_t client_key_ = 1;
	std::string player_name_;
	Phase phase_ = Phase::Idle;

	// SCRK / seq / ack live on the client-side connection node (folded, as on the server side).
	NapiNPConnection conn_;
	std::string advertised_expansion_; // ServerHello.SUS2, echoed as C2S JOIN EXP

	uint32_t server_hk_ = 0;    // ServerHello.hk — echoed in ClientAuth.hk (transient)
	// Pre-session UDP legs are reliable-by-retransmit in retail. Cache the already-framed bytes so
	// retrying never regenerates identity/session material (especially ClientAuth CK/SCRK).
	MonotonicMilliseconds monotonic_milliseconds_;
	std::vector<uint8_t> handshake_retry_datagram_;
	uint64_t handshake_last_send_ms_ = 0;
	bool handshake_retry_clock_armed_ = false;
	// Wall-clock stamp of the last VALID inbound datagram — retail's per-connection
	// receive-activity clock, the one cs_dir0.timeout_ms is measured against.
	// [orig: CNapiNetwork_Init @0x4ca4a0 timeout_ms = 120000 @0x4caa81/@0x4cab54]
	uint64_t last_receive_ms_ = 0;
	bool receive_clock_armed_ = false;
	PostAuthStage post_auth_stage_ = PostAuthStage::Inactive;
	uint64_t session_last_send_ms_ = 0;
	bool session_send_clock_armed_ = false;
	bool goodbye_sent_ = false; // disconnect() burst emitted; retail clears the keys right after
	bool session_ack_pending_ = false; // admitted S2C messages with no substantive C2S reply yet
	bool world_ready_ = true;   // binding-controlled: local advertised mission is installed
	bool mission_known_ = false; // structurally valid authoritative S2C 0x7B received
	// The joiner's server-assigned team, latched from the S2C 0x04 slot-assignment tail byte —
	// the 0x2F loadout-submit header byte 0 source [orig: NapiNPClientMsg_0x004 @0x425410 ->
	// byte_A85B48 @0x425499]. Zero-initialized like retail's global: 0x04 always precedes the
	// 0x1A submission trigger in a real admission, so harnesses must feed the 0x04 they expect.
	uint8_t assigned_team_ = 0;
	uint8_t local_player_slot_ = 0; // S2C 0x04 body byte 17 (see local_player_slot())
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
	// Frozen renderer-definition snapshot for S2C 0x68 -> C2S 0x3D. Configuration
	// survives start(): bindings may finish loading models before the first net pump.
	std::vector<uint32_t> loaded_model_challenge_snapshot_;
	bool player_paced_deployment_ = false; // binding mode: the deploy pick waits for the player
	bool preload_ready_ = false; // terminal pre-world S2C 0x11 received and ACK boundary reached
	bool sync_tail_seen_ = false; // the host emits 0x11 exactly once; latch an early arrival
	bool player_list_seen_ = false; // valid S2C 0x16 may arrive before the 0x64 transfer completes
	bool deployment_policy_seen_ = false; // a valid S2C 0x0F supplied gameFlags
	bool deployment_pick_required_ = false; // S2C 0x0F gameFlags bit0: host has spawn zones
	uint8_t initial_loadout_grant_count_ = 0; // both profile-side 0x5A grants precede the pick
	bool deployment_pick_sent_ = false;
	uint32_t deployment_pick_sequence_ = 0; // release 0x5A must cumulatively ACK this C2S 0x0E
	bool deployment_reply_seen_ = false; // applicable final S2C 0x5A; pairs with self-handle discovery
	bool pending_spawn_menu_request_ = false; // S2C 0x11 arrived while the local world was held
	// One C2S 0x32 empty-slot sweep request per session, queued from the S2C 0x0F
	// world-state-load reply burst. [orig: NapiNPClientMsg_0x00F @0x42e647]
	bool empty_slot_sweep_requested_ = false;

	bool has_self_handle_ = false;
	uint16_t self_handle_ = 0;  // wire handle H, learned via the name-match (pool<<12|slot)
	SelfSpawn spawn_;
	uint32_t game_type_ = 0;    // authoritative g_GameType learned from S2C 0x08 field 3 / 0x7B extra
	std::string server_name_;   // authoritative S2C 0x7B field 3
	std::string mission_name_;  // authoritative S2C 0x7B field 4
	std::string map_file_;      // authoritative S2C 0x7B field 5
	std::string expansion_;     // authoritative S2C 0x7B field 7

	std::string last_error_;
	// The host's explicit close, latched from the first connection-description record and
	// never overwritten [orig: the store-if-!valid event slot @0x621d3c]. Non-empty is the
	// terminal session-loss state, independent of the receive-silence clock.
	std::string host_disconnect_reason_;
};

} // namespace opennova::np
