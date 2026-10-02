// JoinerConnection — the C2S send side: framing semantic messages into sequenced
// 0x43 packets, the receive pump's missing-sequence tail, the send-boundary pump
// (the handshake retries and the established session's send-interval legs), and
// the loadout / deployment-pick / uplink producers that frame through it. Split
// out of joiner_connection.cpp; the class header is the shared declaration.
#include <runtime/inmatch/joiner_connection.h>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace opennova::inmatch {

namespace {

// The 0x41 ClientHello leg's retransmit cadence. A retail game join sends no 0x41 at all: it
// joins from the browse row, whose session record already holds the 0x81, and goes straight to
// the connect state below. Our joiner has no browse row, so its Hello leg stands in for the
// browse probe of that one host; this stays a capture-pinned policy value (about one second).
constexpr uint64_t kClientHelloRetryMilliseconds = 1000;

// The connect state (retail state 3) of a JO game join: the 0x42 ClientJoin is re-sent once
// MORE than 2000 ms passed since the last send, and the join fails once MORE than 30000 ms passed
// since the state was entered, latching connect error 2, whose text is gameerr
// "MPNetConnectCodes" NCC002. A ServerAuth (state 5) ends both.
// [orig: CNapiNetwork_StartClientConnection @0x4ca160 stores conn+0x5B0 = 2000 @0x4ca2ab and
//  +0x5B4 = 30000 @0x4ca2bb (CNapiGameSession_CreateSession @0x4c9c27 stores the same pair);
//  CNapiNPConnection_InitFromSession @0x626320 enters state 3, stamping the start +0x5E0
//  @0x62648a and the last send +0x5E4 = now - 2000 @0x626499; CNapiNPConnection_PumpStateMachine
//  case 3: `now - start > +0x5B4` @0x62951c -> +0x5F0 = 2 @0x629525, state 4 @0x629564; else
//  `now - last > +0x5B0` @0x629588 -> last = now @0x62958f, SendClientJoin @0x629595;
//  {2, "NCC002"} in g_MPNetConnectCodes @0x82ba60, read by
//  CNapiNetwork_GetDisconnectReasonString @0x4c703d]
constexpr uint64_t kClientJoinRetryMilliseconds = 2000;
constexpr uint64_t kClientJoinTimeoutMilliseconds = 30000;
constexpr uint32_t kConnectErrorJoinTimeout = 2; // NCC002

// The established session's two send-interval legs read this connection's cs_dir0 (CS fields 5
// and 4; 10000 / 30000 on the JOINTOPERATIONS template, overlaid by the host's 0x82 and H:0x00
// CS updates), each disabled by a negative value:
//   ACTIVE (field 5): while reliable records remain retained, a sender with nothing else to send
//   waits strictly more than the interval, then mints a header-only sequence so the peer's
//   ordinary 0x44/0x84 machinery requests the lost semantic packet.
//   EMPTY (field 4): with NOTHING queued and NOTHING retained the pump still mints a packet once
//   the interval elapses, so the peer's connection timeout never fires on a quiet client — the
//   keepalive that carries a client parked at the deploy pick, killed, or simply idle.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0 — the active leg @0x628ff1..0x629017,
//  the empty leg @0x629041..0x629067 (-> force_send -> BuildOutgoingPackets); CNapiNetwork_Init
//  @0x4ca4a0 stores 10000 @0x4caac5/@0x4cab98 and 30000 @0x4caab5/@0x4cab88; the reaping
//  timeout_ms = 120000 is stored @0x4caa81/@0x4cab54]

// The golden joiner's profile kit (retail-lan-host-join-session): the capture default every
// headless caller keeps submitting. Rows carry the profile's "-1" ammo/flags strings (atol
// low byte -> 0xFF) [orig: the kit tuple buffer NetPacket_SendLoadoutSubmit @0x42cdc0 walks].
LoadoutSubmit build_retail_default_loadout_submit(uint8_t team, bool alternate) {
	LoadoutSubmit submit;
	submit.team = team;
	submit.player_class = 0x08;
	// First submission: the fixed Primary-key slot 195 (pre-Player_InitPlayer the slot table is
	// empty, so the builder's side-mask resolution returns the argument raw). Second: the live
	// g_CurrentWeaponSlot after the spawn fill — the golden kit's primary landed at category 3
	// rank 17 = 212. [orig: Game_StartMission @0x525836 (195) / @0x525c2e (g_CurrentWeaponSlot)]
	submit.weapon_slot_index = alternate ? 212u : 195u;
	for (uint8_t adm : {
			uint8_t{0x18}, uint8_t{0x03}, uint8_t{0x2C}, uint8_t{0x28},
			uint8_t{0x29}, uint8_t{0x2A}, uint8_t{0x02},
	}) {
		submit.entries.push_back(LoadoutSubmitEntry{adm, 0xFF, 0xFF, 0xFF});
	}
	return submit;
}

} // namespace

std::vector<uint8_t> JoinerConnection::frame_session(const std::vector<ProtocolMessage> &messages) {
	if (session_lost() || phase_ == Phase::Error) return {};
	// Joiner C2S direction: encrypt with our client_scrk, stamp session_id = the server's SK
	// (ServerAuth.sk, the peer's local_key). Shared seq/ack framing (ADR 0013).
	std::vector<uint8_t> body;
	if (!frame_session_packet(conn_.seq, SessionCrypto{conn_.client_scrk, {}, conn_.server_sk}, messages,
	                          body)) {
		return {};
	}
	session_last_send_ms_ = monotonic_milliseconds_();
	session_send_clock_armed_ = true;
	session_ack_pending_ = false; // every sequenced C2S packet carries the latest S2C ACK
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

JoinerConnection::FrameMessagesResult JoinerConnection::frame_messages_detailed(
		const std::vector<ProtocolMessage> &messages, std::size_t max_packet_bytes) {
	FrameMessagesResult result;
	if (session_lost() || phase_ == Phase::Error) return result;
	if (max_packet_bytes <= PROTOCOL_DATAGRAM_OVERHEAD) { result.frame_failed = true; return result; }
	max_packet_bytes = std::max<std::size_t>(26, max_packet_bytes);
	std::size_t available = session_outbound_message_prefix_count(conn_.seq,
			std::numeric_limits<std::size_t>::max());
	std::vector<ProtocolMessage> packet;
	std::size_t packet_bytes = PROTOCOL_DATAGRAM_OVERHEAD;
	std::size_t completed_in_packet = 0;
	auto flush = [&] {
		if (packet.empty()) return true;
		std::vector<uint8_t> datagram = frame_session(packet);
		if (datagram.empty()) return false;
		result.framed_count += completed_in_packet;
		result.datagrams.push_back(std::move(datagram));
		packet.clear(); completed_in_packet = 0;
		packet_bytes = PROTOCOL_DATAGRAM_OVERHEAD;
		return true;
	};
	// Plan/admit the nodes before framing, per physical node in queue order like
	// NapiNPMessage_Create (a SplitAtLength piece goes through Create too). An
	// encoding failure must leave the entire admitted suffix with its caller,
	// including later nodes. The FIRST non-exempt node that does not fit is the
	// pool overflow: the node is dropped, {2, 4, count, max, "", 0, "NP.C:MSGCRE"}
	// latches (first cause wins) and RequestDisconnect on a client-side (state 5)
	// connection tears it down INLINE — SetState(6) sends the 0x46 burst carrying
	// that record and the session is terminal, so nothing queued this boundary
	// (admitted or not) is built.
	// [orig: NapiNPMessage_Create @0x627FC0 — exemption @0x628031, `msg_out_max >= 0`
	//  @0x628048, count @0x628062..0x62806b, the record @0x628099..0x6280eb,
	//  latch @0x6280f9..0x62810a, RequestDisconnect @0x628112 -> @0x61e0fa
	//  SetState(6) -> TeardownActiveConnection @0x62549e..0x6254d3;
	//  SplitAtLength @0x62838f]
	std::vector<std::vector<ProtocolMessage>> planned;
	std::size_t planned_packet_bytes = PROTOCOL_DATAGRAM_OVERHEAD;
	const std::size_t occupied = conn_.seq.retained_outbound_message_count +
			conn_.seq.transient_outbound_message_count;
	std::size_t admitted_nodes = 0;
	for (const ProtocolMessage &message : messages) {
		auto pieces = split_protocol_message_to_fill(message, max_packet_bytes, planned_packet_bytes);
		for (const ProtocolMessage &piece : pieces) {
			if (piece.capacity_exempt) continue;
			if (available == 0) {
				latch_disconnect_event(make_disconnect_event(2, 4,
						static_cast<uint32_t>(occupied + admitted_nodes + 1),
						static_cast<uint32_t>(conn_.seq.outbound_message_limit),
						"", 0, "NP.C:MSGCRE"), 2);
				result.datagrams = disconnect();
				result.framed_count = 0;
				fail("NP.C:MSGCRE");
				return result;
			}
			--available;
			++admitted_nodes;
		}
		++result.admitted_count;
		planned.push_back(std::move(pieces));
	}
	for (auto &pieces : planned) {
		for (std::size_t i = 0; i < pieces.size(); ++i) {
			std::vector<uint8_t> encoded;
			if (!append_protocol_message(encoded, pieces[i])) {
				(void)flush(); result.frame_failed = true; return result;
			}
			if (packet_bytes + encoded.size() > max_packet_bytes && !flush()) {
				result.frame_failed = true; return result;
			}
			packet.push_back(std::move(pieces[i]));
			packet_bytes += encoded.size();
			if (i + 1 == pieces.size()) ++completed_in_packet;
		}
	}
	if (!flush()) result.frame_failed = true;
	return result;
}

std::vector<std::vector<uint8_t>> JoinerConnection::frame_messages(
		const std::vector<ProtocolMessage> &messages, std::size_t max_packet_bytes) {
	return frame_messages_detailed(messages, max_packet_bytes).datagrams;
}

std::vector<uint8_t> JoinerConnection::frame_retained_session(uint32_t sequence) {
	std::vector<uint8_t> body;
	if (!frame_session_packet_for_sequence(
			conn_.seq,
			SessionCrypto{conn_.client_scrk, {}, conn_.server_sk},
			sequence, body)) {
		return {};
	}
	session_ack_pending_ = false;
	return nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

// Retail's client receive pump (flags 26: the 0x8 PumpRecvQueues leg, never the send pump's 738)
// drains its datagram FIFO through the opcode handlers, then walks each client connection: it
// parses every queued packet the closed frontier now admits, and when HandleSessionPacket latched
// a future packet (conn+0x174) while the queue still holds one, it sends the missing-sequence
// list at once and clears the latch. Every client frame runs this, so the request goes out on the
// frame the gap is seen, whatever the send-holdoff countdown says. The send pump's own timed
// missing-sequence leg (PumpSendIntervals' packet_queue_interval_ms) is off for a JO connection:
// the JOINTOPERATIONS template stores -1 and a retail host's CS updates only carry fields 3 and 13.
// [orig: CNapiNetwork_PumpClientProtocolRecv @0x4c4fe0 -> NapiNPProtocol_Pump @0x62a6ac ->
//  NapiNPProtocol_PumpRecvQueues @0x6266a0 — the latch test @0x6269bb, the queue test @0x6269c8,
//  CNapiNPConnection_SendMissingSeqList(conn, 0) @0x6269ce, the clear @0x6269d6; the template
//  -1 CNapiNetwork_Init @0x4caad8/@0x4caba8; the timed leg CNapiNPConnection_PumpSendIntervals
//  @0x629032; the CS senders NapiNPServer_UpdateHoldoffTicks @0x4c5f5e (mask 8) and
//  NapiNPServer_HandleNewConnection @0x4c81bd (mask 0x2000)]
std::vector<uint8_t> JoinerConnection::finish_receive_pump() {
	if (session_lost() || phase_ == Phase::Error) return {};
	if (!conn_.seq.missing_request_pending) return {};
	conn_.seq.missing_request_pending = false;
	if (conn_.seq.queued_inbound.empty()) return {};
	const std::vector<uint32_t> missing = build_session_missing_sequence_list(conn_.seq, false);
	std::vector<uint8_t> missing_body;
	if (!encode_session_resend_list(conn_.server_sk, missing, missing_body)) return {};
	// A sent request that named a sequence fires the incoming link-error callback
	// [orig: CNapiNPConnection_SendMissingSeqList — has_missing_seqs @0x623690, after the send
	//  @0x623775..0x6237bd cb_client_2 = Network_LogIncomingPacketError @0x4c4890].
	if (std::any_of(missing.begin(), missing.end(),
			[](uint32_t sequence) { return sequence != 0; }))
		net_quality_link_errors_ |= kNetQualityLinkErrorIncoming;
	return nw_encode_outbound(SESSION_OPCODE_CLIENT_RESEND_LIST, std::move(missing_body));
}

std::vector<std::vector<uint8_t>> JoinerConnection::pump(uint32_t /*now_tick*/) {
	std::vector<std::vector<uint8_t>> out;
	if (session_lost() || phase_ == Phase::Error) return out;
	// [orig: NetClient_HandleGameEnd @0x424752 increments @0xA822A4 once per client net pump]
	++net_frame_counter_;
	if (phase_ == Phase::Hello || phase_ == Phase::Auth) {
		if (handshake_retry_datagram_.empty()) return out;
		const uint64_t now_ms = monotonic_milliseconds_();
		// Each initial send records its wall-clock timestamp. Keep a defensive lazy-anchor for a future
		// caller that installs a pending leg without going through those builders.
		if (!handshake_retry_clock_armed_) {
			handshake_last_send_ms_ = now_ms;
			client_join_start_ms_ = now_ms;
			handshake_retry_clock_armed_ = true;
			return out;
		}
		if (phase_ == Phase::Auth) {
			// The connect state: the window test runs ahead of the resend test
			// [orig: PumpStateMachine case 3 @0x62951c, then @0x629588].
			if (now_ms >= client_join_start_ms_ &&
			    now_ms - client_join_start_ms_ > kClientJoinTimeoutMilliseconds) {
				last_join_reject_.set = true;
				last_join_reject_.jfc = kConnectErrorJoinTimeout;
				last_join_reject_.jfp = 0;
				last_join_reject_.jfs.clear();
				fail("The host did not answer the join request (NCC002)");
				return out;
			}
			if (now_ms >= handshake_last_send_ms_ &&
			    now_ms - handshake_last_send_ms_ > kClientJoinRetryMilliseconds) {
				handshake_last_send_ms_ = now_ms;
				out.push_back(handshake_retry_datagram_);
			}
			return out;
		}
		if (now_ms >= handshake_last_send_ms_ &&
		    now_ms - handshake_last_send_ms_ >= kClientHelloRetryMilliseconds) {
			handshake_last_send_ms_ = now_ms;
			out.push_back(handshake_retry_datagram_);
		}
		return out;
	}
	// The send boundary never emits the 0x44 missing-sequence request: retail resolves that latch
	// in the receive pump, every client frame, outside the holdoff gate (finish_receive_pump).
	// Retail's reliable sender does not blindly resend an unanswered semantic packet. While retained
	// records remain, the active-send interval mints a NEW header-only sequence. A peer missing the
	// preceding semantic sequence observes the gap and requests it through 0x44/0x84; the existing
	// retained-record path then reconstructs that old sequence with the current ACK.
	if (phase_ == Phase::Driving || phase_ == Phase::InMatch) {
		// The connected state's reap runs first and ends the pump when it fires
		// (D-NET-255) [orig: PumpStateMachine case 5 @0x6295a2..0x62940f].
		if (reap_silent_session()) return out;
		if (conn_.seq.retained_outbound_message_count == 0) {
			session_send_clock_armed_ = false;
			// Nothing queued, retained or held out of order (+0x7A8 @0x629053, D-NET-236): the
			// EMPTY interval keeps the connection alive. Without it a joiner that owes no reply
			// (deploy pick, dead, idle) transmits nothing and a stock host drops it at 120 s.
			const uint64_t now_ms = monotonic_milliseconds_();
			const int32_t idle_interval = conn_.timeouts.idle_send_interval_ms;
			if (idle_interval >= 0 && conn_.server_sk != 0 && session_last_send_ms_ != 0 &&
			    conn_.seq.queued_inbound.empty() && now_ms >= session_last_send_ms_ &&
			    now_ms - session_last_send_ms_ > static_cast<uint64_t>(idle_interval)) {
				out.push_back(frame_session({}));
			}
		} else {
			const uint64_t now_ms = monotonic_milliseconds_();
			if (!session_send_clock_armed_) {
				session_last_send_ms_ = now_ms;
				session_send_clock_armed_ = true;
			} else if (conn_.timeouts.active_send_interval_ms >= 0 &&
			           now_ms >= session_last_send_ms_ &&
			           now_ms - session_last_send_ms_ >
			                   static_cast<uint64_t>(conn_.timeouts.active_send_interval_ms)) {
				out.push_back(frame_session({}));
			}
		}
	}
	if (phase_ != Phase::Driving) {
		if (phase_ == Phase::InMatch && session_ack_pending_)
			out.push_back(frame_session({}));
		return out;
	}

	// The admission FSM is entirely server-driven. When S2C 0x11 arrived during a synchronous
	// load hold, release its empty C2S 0x0A world-stream request as soon as the binding reports that
	// the advertised mission has been installed.
	if (pending_spawn_menu_request_ && world_ready_) {
		// Remains inside ClientRuntime's surrounding logical send boundary.
		out.push_back(frame_session({make_protocol_message(0x0A, {})}));
		pending_spawn_menu_request_ = false;
		post_auth_stage_ = PostAuthStage::AwaitWorldStreamEnd;
	}

	// Carry a pending cumulative ACK only when no substantive C2S producer above already did so.
	// All semantic join messages are emitted at their captured S2C trigger in on_server_session.
	if (session_ack_pending_) out.push_back(frame_session({}));
	return out;
}

LoadoutSubmit JoinerConnection::build_profile_loadout_submit(
		uint8_t team, bool alternate) const {
	// Retail re-reads the PROFILE for every submission that is not the armory's: the
	// wire team byte picks the side block, the block's first byte is the wire class, and
	// that class indexes the 2048-byte kit page inside the same block. One integer, both
	// fields — the reason a retail kit can never be class-illegal.
	// [orig: Game_StartMission @0x525767-0x525836 (join) and NapiNPClientMsg_TeamAssign
	//  @0x431a35..0x431a9a (the S2C 0x50 reselect)]
	if (!loadout_kit_set_) return build_retail_default_loadout_submit(team, alternate);
	LoadoutSubmit submit;
	submit.team = team;
	const LoadoutKit::SideKit &side = loadout_kit_.side_for_team(team);
	if (side.set) {
		submit.player_class = side.player_class;
		submit.entries = side.rows;
	} else {
		// The binding supplied only the applied kit (the historical single-kit seam).
		submit.player_class = loadout_kit_.player_class;
		submit.entries = loadout_kit_.rows;
	}
	// [orig: @0x525836 passes 195, @0x525c2e passes g_CurrentWeaponSlot]
	submit.weapon_slot_index = alternate && loadout_kit_.equipped_combo >= 0
			? static_cast<uint32_t>(loadout_kit_.equipped_combo)
			: 195u;
	return submit;
}

std::vector<uint8_t> JoinerConnection::frame_loadout_resubmit() {
	ProtocolMessage message;
	if (!prepare_loadout_resubmit(message)) return {};
	std::vector<uint8_t> datagram = frame_session({std::move(message)});
	if (!datagram.empty()) complete_session_send_flush(conn_.seq);
	return datagram;
}

bool JoinerConnection::prepare_loadout_resubmit(
		ProtocolMessage &message_out) const {
	// The armory-ACCEPT re-send [orig: WeaponLoadout_ApplyFromBuffer @0x565d94 ->
	// NetPacket_SendLoadoutSubmit]: one 0x2F, current team + class + kit, slot = the
	// live equipped combo (retail passes g_CurrentWeaponSlot). Only meaningful once
	// the initial submission has gone out — the armory opens in-world.
	// This leg does NOT re-read the profile side blocks: retail hands the ARMORY's
	// own edited buffer and the armory's class to the builder, so the applied kit
	// (LoadoutKit::player_class/rows) stays the source here.
	if (post_auth_stage_ != PostAuthStage::AwaitDeployment &&
	    post_auth_stage_ != PostAuthStage::AwaitDeployPick &&
	    post_auth_stage_ != PostAuthStage::AwaitDeployRelease &&
	    post_auth_stage_ != PostAuthStage::Complete) {
		return {};
	}
	LoadoutSubmit submit;
	if (loadout_kit_set_) {
		submit.team = assigned_team_;
		submit.player_class = loadout_kit_.player_class;
		submit.weapon_slot_index = loadout_kit_.equipped_combo >= 0
				? static_cast<uint32_t>(loadout_kit_.equipped_combo)
				: 195u;
		submit.entries = loadout_kit_.rows;
	} else {
		submit = build_retail_default_loadout_submit(assigned_team_, true);
	}
	message_out = make_protocol_message(
			0x2F, encode_loadout_submit(submit));
	return true;
}

std::vector<uint8_t> JoinerConnection::frame_deployment_pick(uint16_t wire_value) {
	ProtocolMessage message;
	if (!prepare_deployment_pick(wire_value, message)) return {};
	std::vector<uint8_t> datagram = frame_session({std::move(message)});
	if (!datagram.empty()) complete_session_send_flush(conn_.seq);
	return datagram;
}

bool JoinerConnection::prepare_deployment_pick(
		uint16_t wire_value, ProtocolMessage &message_out) {
	// The player's deploy-map pick [orig: Input_HandleActionBinding case 12
	// @0x49b0c5-0x49b17b — dialogs reset, dword_81474C set, one C2S 0x0E [i16]].
	// A re-pick while awaiting the release is retail behavior: the host silently
	// drops an invalid/contested pick and the screen stays up; every list click
	// queues a fresh 0x0E. The release rule keys on the FIRST in-flight pick's
	// sequence: the host releases on the first VALID pick it processes, so a
	// re-pick racing an already-sent release must not raise the ack bar past it
	// (the grant-vs-release discrimination only needs the ack to clear the first
	// pick — a cumulative ack covering any later pick clears it too).
	const bool initial_overlay = post_auth_stage_ == PostAuthStage::Complete &&
			phase_ == Phase::InMatch && has_self_handle_;
	if (!initial_overlay && post_auth_stage_ != PostAuthStage::AwaitDeployPick &&
	    post_auth_stage_ != PostAuthStage::AwaitDeployRelease) {
		return false;
	}
	if (initial_overlay || !deployment_pick_sent_)
		deployment_pick_sequence_ = conn_.seq.next_outbound_seq;
	message_out = make_protocol_message(
			0x0E, {static_cast<uint8_t>(wire_value & 0xFFu),
			       static_cast<uint8_t>((wire_value >> 8) & 0xFFu)});
	deployment_pick_sent_ = true;
	// Input case 12 re-arms dword_81474C without leaving the established
	// in-session phase. ClientRuntime closes its separate gameplay gate when the
	// input action is accepted; the ACK-qualified S2C 0x5A above opens it again.
	deployment_reply_seen_ = false;
	post_auth_stage_ = PostAuthStage::AwaitDeployRelease;
	return true;
}

bool JoinerConnection::begin_redeployment() {
	if (phase_ != Phase::InMatch || !has_self_handle_) return false;
	// The authenticated connection and self entity survive death. Only the spawn-success gate and
	// the pick/release sub-state reset; the host's subsequent 0x61 re-seeds the client clock.
	phase_ = Phase::Driving;
	post_auth_stage_ = PostAuthStage::AwaitDeployPick;
	deployment_pick_sent_ = false;
	deployment_pick_sequence_ = 0;
	deployment_reply_seen_ = false;
	return true;
}

std::vector<uint8_t> JoinerConnection::frame_c2s_uplink(uint16_t handle_H, uint16_t type,
                                                        const PlayerExtendedUplink &body) {
	EntityPacketSubHeader sub;
	sub.handle = handle_H;
	sub.item_type_id = type;
	sub.sub_op = ENTITY_SUB_OP_EXTENDED; // extended (type-10) uplink
	std::vector<uint8_t> payload = encode_entity_packet_sub_header(sub);
	std::vector<uint8_t> ext = encode_player_extended_uplink(body);
	payload.insert(payload.end(), ext.begin(), ext.end());
	ProtocolMessage uplink =
			make_protocol_message(c2s::ENTITY_UPLINK, std::move(payload));
	// Keep the public replay/golden seam aligned with ClientRuntime's live
	// producer: both retail call sites pass userParam=1.
	uplink.reliable = false;
	std::vector<uint8_t> datagram = frame_session({std::move(uplink)});
	if (!datagram.empty()) complete_session_send_flush(conn_.seq);
	return datagram;
}

} // namespace opennova::inmatch
