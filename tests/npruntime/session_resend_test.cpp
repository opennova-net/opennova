// Retail 0x44/0x84 missing-sequence recovery across the two public in-match runtime seams.
//
// Each direction drops packet 1, delivers packet 2, observes the exact NACK, and feeds the
// reconstructed packet 1 back through the receiver so its queued packet 2 drains. The tests also
// pin key validation, malformed-body rejection, current-ACK retransmit headers, and ACK retirement.

#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;

constexpr PeerAddr kPeer{0x0100007Fu, 30124};
constexpr uint32_t kClientKey = 1;
constexpr uint32_t kServerKey = 0x55667788u;
const std::string kClientScrk = "CLIENT-LOSS-RECOVERY-SCRK";
const std::string kServerScrk = "SERVER-LOSS-RECOVERY-SCRK";

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void seed_host(inmatch::NapiNPServerCtx &ctx) {
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	inmatch::NapiNPConnection conn;
	conn.connection_id = 3;
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.peer = kPeer;
	conn.client_scrk = kClientScrk;
	conn.server_scrk = kServerScrk;
	conn.client_ck = kClientKey;
	conn.server_sk = kServerKey;
	conn.spawned_announced = true;
	conn.burst.spawned = true;
	ctx.np_protocol.connection_list.push_back(std::move(conn));
}

bool decode_session_datagram(const std::vector<uint8_t> &datagram, uint8_t expected_opcode,
		std::string_view scrk, ProtocolPacketHeader &header,
		std::vector<ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected_opcode &&
			decode_protocol_packet_plaintext(
					body.data(), body.size(), scrk, header, messages);
}

bool decode_resend_datagram(const std::vector<uint8_t> &datagram, uint8_t expected_opcode,
		uint32_t local_key, std::vector<uint32_t> &requested) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected_opcode &&
			decode_session_resend_list(
					body.data(), body.size(), local_key, requested);
}

std::vector<uint8_t> make_resend_datagram(
		uint8_t opcode, uint32_t key, std::vector<uint32_t> requested) {
	std::vector<uint8_t> body;
	if (!encode_session_resend_list(key, requested, body)) return {};
	return nw_encode_outbound(opcode, std::move(body));
}

bool frame_test_session_datagram(SessionSequencing &sequencing,
		const SessionCrypto &crypto, uint8_t opcode,
		std::vector<ProtocolMessage> messages, std::vector<uint8_t> &datagram) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(sequencing, crypto, messages, body)) return false;
	datagram = nw_encode_outbound(opcode, std::move(body));
	return true;
}

bool check_session_header_key_validation() {
	// Host: a correctly encrypted packet addressed to a different server-local SK must be dropped
	// before its sequence or ACK can enter this connection.
	inmatch::NapiNPServerCtx ctx;
	seed_host(ctx);
	SessionSequencing client_tx{1, 0};
	std::vector<uint8_t> wrong_c2s;
	if (!expect(frame_test_session_datagram(
			client_tx, SessionCrypto{kClientScrk, {}, kServerKey + 1},
			SESSION_OPCODE_PROTOCOL_MESSAGE,
			{make_protocol_message(0x34, {0xA1})}, wrong_c2s),
	            "frame C2S packet for a different server session"))
		return false;
	const inmatch::HandleResult host_drop = inmatch::handle_server_datagram(
			ctx, kPeer, wrong_c2s.data(), wrong_c2s.size(), 1);
	if (!expect(host_drop.outbound.empty() && host_drop.events.empty() &&
	                    ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 0 &&
	                    ctx.np_protocol.connection_list[0].seq.queued_inbound.empty(),
	            "host drops wrong-SK C2S before sequencing or dispatch"))
		return false;

	// Joiner mirror: S2C headers are addressed to its client-local CK.
	inmatch::JoinerConnection joiner("KeyValidation");
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0001, 0x14B9);
	SessionSequencing server_tx{1, 0};
	std::vector<uint8_t> wrong_s2c;
	if (!expect(frame_test_session_datagram(
			server_tx, SessionCrypto{kServerScrk, {}, kClientKey + 1},
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			{make_protocol_message(0x49, {0xC5})}, wrong_s2c),
	            "frame S2C packet for a different client session"))
		return false;
	const inmatch::JoinerConnection::PollResult joiner_drop =
			joiner.handle_datagram(wrong_s2c.data(), wrong_s2c.size());
	return expect(joiner_drop.inbound_gameplay.empty() &&
	                      joiner_drop.inbound_0a.empty() &&
	                      joiner_drop.outbound.empty() &&
	                      joiner.connection().seq.last_inbound_seq == 0 &&
	                      joiner.connection().seq.queued_inbound.empty(),
	              "joiner drops wrong-CK S2C before sequencing or dispatch");
}

bool check_reordered_same_batch_closes_gap_without_nack() {
	// Joiner receive batch: frontier=1, then S2C seq3 arrives before seq2. Both datagrams are
	// consumed before the receive pump's tail, so seq2 admits and drains seq3 before the missing
	// latch is resolved. [orig: NapiNPProtocol_PumpRecvQueues @0x6269bb..0x6269d6]
	inmatch::JoinerConnection joiner("SameBatch");
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 1, 0x0001, 0x14B9);
	SessionSequencing server_tx{2, 0};
	std::vector<uint8_t> s2c2;
	std::vector<uint8_t> s2c3;
	if (!expect(frame_test_session_datagram(
			server_tx, SessionCrypto{kServerScrk, {}, kClientKey},
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			{make_protocol_message(0x49, {0x02})}, s2c2) &&
	                    frame_test_session_datagram(
			server_tx, SessionCrypto{kServerScrk, {}, kClientKey},
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			{make_protocol_message(0x49, {0x03})}, s2c3),
	            "frame reordered same-batch S2C packets"))
		return false;
	if (!expect(joiner.handle_datagram(
			s2c3.data(), s2c3.size()).inbound_gameplay.empty(),
	            "joiner queues the future S2C packet"))
		return false;
	const inmatch::JoinerConnection::PollResult joiner_close =
			joiner.handle_datagram(s2c2.data(), s2c2.size());
	if (!expect(joiner_close.inbound_gameplay.size() == 2 &&
	                    joiner.connection().seq.queued_inbound.empty(),
	            "later S2C packet in the batch closes and drains the gap"))
		return false;
	if (!expect(joiner.finish_receive_pump().empty() &&
	                    !joiner.connection().seq.missing_request_pending,
	            "the receive pump's tail clears the latch without a NACK once the gap closed"))
		return false;
	const std::vector<std::vector<uint8_t>> joiner_boundary = joiner.pump(1);
	ProtocolPacketHeader joiner_ack_header;
	std::vector<ProtocolMessage> joiner_ack_messages;
	if (!expect(joiner_boundary.size() == 1 &&
	                    decode_session_datagram(
							joiner_boundary[0], SESSION_OPCODE_PROTOCOL_MESSAGE,
							kClientScrk, joiner_ack_header, joiner_ack_messages) &&
	                    joiner_ack_header.seq_num == 1 &&
	                    joiner_ack_header.ack_count == 3 &&
	                    joiner_ack_messages.empty() &&
	                    !joiner.connection().seq.missing_request_pending,
	            "joiner batch boundary suppresses a NACK and carries the recovered ACK"))
		return false;

	// Host mirror: frontier=1, then C2S seq3 before seq2 in one socket drain.
	inmatch::NapiNPServerCtx ctx;
	seed_host(ctx);
	ctx.np_protocol.connection_list[0].seq.last_inbound_seq = 1;
	SessionSequencing client_tx{2, 0};
	std::vector<uint8_t> c2s2;
	std::vector<uint8_t> c2s3;
	if (!expect(frame_test_session_datagram(
			client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
			SESSION_OPCODE_PROTOCOL_MESSAGE,
			{make_protocol_message(0x34, {0x02})}, c2s2) &&
	                    frame_test_session_datagram(
			client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
			SESSION_OPCODE_PROTOCOL_MESSAGE,
			{make_protocol_message(0x34, {0x03})}, c2s3),
	            "frame reordered same-batch C2S packets"))
		return false;
	if (!expect(inmatch::handle_server_datagram(
			ctx, kPeer, c2s3.data(), c2s3.size(), 1).outbound.empty(),
	            "host queues the future C2S packet"))
		return false;
	if (!expect(inmatch::handle_server_datagram(
			ctx, kPeer, c2s2.data(), c2s2.size(), 1).outbound.empty() &&
	                    ctx.np_protocol.connection_list[0].seq.queued_inbound.empty(),
	            "later C2S packet in the batch closes and drains the gap"))
		return false;
	if (!expect(inmatch::flush_server_missing_requests(ctx).empty() &&
	                    !ctx.np_protocol.connection_list[0].seq.missing_request_pending,
	            "host batch boundary suppresses a NACK after same-batch recovery"))
		return false;
	return true;
}

bool check_s2c_loss_requests_0x44_and_host_reconstructs() {
	inmatch::NapiNPServerCtx ctx;
	seed_host(ctx);
	inmatch::JoinerConnection joiner("LossRecovery");
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0001, 0x14B9);

	if (!expect(ctx.np_protocol.connection_list[0].seq.outbound_message_limit ==
	                    inmatch::JO_GAME_SESSION_OUTBOUND_MESSAGE_MAX,
	            "JO game host connection opts into retail's bounded reliable-message retention"))
		return false;
	if (!expect(joiner.connection().seq.outbound_message_limit ==
	                    inmatch::JO_GAME_SESSION_OUTBOUND_MESSAGE_MAX,
	            "JO game joiner connection opts into retail's bounded reliable-message retention"))
		return false;

	// Give the host an admitted C2S sequence before its first S2C packet, then advance that ACK once
	// more after framing. The retransmit must carry ACK=2 even though the original carried ACK=1.
	const std::vector<uint8_t> c2s1 = joiner.frame_inner(0x34, {});
	inmatch::handle_server_datagram(ctx, kPeer, c2s1.data(), c2s1.size(), 1);

	std::vector<uint8_t> first;
	std::vector<uint8_t> second;
	if (!expect(inmatch::frame_in_match_s2c(ctx, kPeer, 0x49, {0xC5}, first) &&
	                    inmatch::frame_in_match_s2c(ctx, kPeer, 0x49, {0xC6}, second),
	            "host frames two retained S2C packets"))
		return false;
	ProtocolPacketHeader first_header;
	std::vector<ProtocolMessage> first_messages;
	if (!expect(decode_session_datagram(
			first, SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, kServerScrk,
			first_header, first_messages) &&
		            first_header.seq_num == 1 && first_header.ack_count == 1,
	            "original lost S2C packet carries its first-send ACK"))
		return false;

	const std::vector<uint8_t> c2s2 = joiner.frame_inner(0x34, {});
	inmatch::handle_server_datagram(ctx, kPeer, c2s2.data(), c2s2.size(), 2);
	if (!expect(ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 2,
	            "host ACK advances before handling the resend request"))
		return false;

	const inmatch::JoinerConnection::PollResult gap =
			joiner.handle_datagram(second.data(), second.size());
	if (!expect(gap.inbound_gameplay.empty() && gap.outbound.empty() &&
	                    gap.immediate_outbound.empty(),
	            "joiner queues S2C sequence two without NACKing before the batch boundary"))
		return false;
	if (!expect(joiner.take_net_quality_link_errors() == 0,
	            "a queued gap raises no link error before the request goes out"))
		return false;
	if (!expect(joiner.pump(3).empty(),
	            "the send boundary never carries the missing-sequence request"))
		return false;
	const std::vector<uint8_t> gap_nack = joiner.finish_receive_pump();
	if (!expect(!gap_nack.empty() && joiner.finish_receive_pump().empty(),
	            "joiner emits exactly one NACK after the persistent-gap receive batch"))
		return false;
	const std::vector<std::vector<uint8_t>> gap_nacks{gap_nack};
	// The sent 0x44 named a sequence: the incoming link error (flag 2)
	// [orig: SendMissingSeqList cb_client_2 @0x6237aa].
	if (!expect(joiner.take_net_quality_link_errors() == inmatch::kNetQualityLinkErrorIncoming &&
	                    joiner.take_net_quality_link_errors() == 0,
	            "the joiner's 0x44 raises the incoming link error once"))
		return false;
	std::vector<uint32_t> requested;
	if (!expect(decode_resend_datagram(
			gap_nacks[0], SESSION_OPCODE_CLIENT_RESEND_LIST, kServerKey, requested) &&
		            requested == std::vector<uint32_t>({1}),
	            "client 0x44 requests the missing first S2C sequence"))
		return false;

	const uint32_t next_before_bad = ctx.np_protocol.connection_list[0].seq.next_outbound_seq;
	const std::vector<uint8_t> wrong_key = make_resend_datagram(
			SESSION_OPCODE_CLIENT_RESEND_LIST, kServerKey + 1, {1});
	if (!expect(inmatch::handle_server_datagram(
			ctx, kPeer, wrong_key.data(), wrong_key.size(), 3).immediate_outbound.empty() &&
		            ctx.np_protocol.connection_list[0].seq.next_outbound_seq ==
		                    next_before_bad,
	            "host ignores 0x44 with a mismatched local key"))
		return false;
	const std::vector<uint8_t> malformed =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_RESEND_LIST, {0x88, 0x77, 0x66});
	if (!expect(inmatch::handle_server_datagram(
			ctx, kPeer, malformed.data(), malformed.size(), 4).immediate_outbound.empty(),
	            "host ignores a resend-list body shorter than its key"))
		return false;

    if (!expect(!ctx.np_protocol.connection_list[0].link.nak_backoff_pending,
        "invalid resend lists do not back off replication")) return false;
	if (!expect(ctx.net_quality_link_errors == 0,
	            "invalid resend lists raise no link error"))
		return false;
	const inmatch::HandleResult resend = inmatch::handle_server_datagram(
			ctx, kPeer, gap_nacks[0].data(), gap_nacks[0].size(), 5);
    if (!expect(ctx.np_protocol.connection_list[0].link.nak_backoff_pending,
        "valid NAK arms the recipient backoff")) return false;
	// The same callback raises the host's outgoing link error (flag 1)
	// [orig: sub_4C62A0 @0x4c62ce].
	if (!expect(ctx.net_quality_link_errors == inmatch::kNetQualityLinkErrorOutgoing,
	            "a joiner's valid 0x44 raises the host's outgoing link error"))
		return false;
	if (!expect(resend.immediate_outbound.size() == 1,
	            "valid client 0x44 makes the host emit one reconstructed packet"))
		return false;
	ProtocolPacketHeader resent_header;
	std::vector<ProtocolMessage> resent_messages;
	if (!expect(decode_session_datagram(
			resend.immediate_outbound[0], SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, kServerScrk,
			resent_header, resent_messages) &&
		            resent_header.seq_num == 1 && resent_header.ack_count == 2 &&
		            resent_messages.size() == 1 && resent_messages[0].tag == 0x49 &&
		            resent_messages[0].payload == std::vector<uint8_t>({0xC5}),
	            "host reconstructs old S2C records with its current ACK"))
		return false;

	const inmatch::JoinerConnection::PollResult recovered =
			joiner.handle_datagram(resend.immediate_outbound[0].data(), resend.immediate_outbound[0].size());
	if (!expect(recovered.inbound_gameplay.size() == 2 &&
	                    recovered.inbound_gameplay[0].second ==
	                            std::vector<uint8_t>({0xC5}) &&
	                    recovered.inbound_gameplay[1].second ==
	                            std::vector<uint8_t>({0xC6}) &&
	                    joiner.connection().seq.last_inbound_seq == 2,
	            "recovered S2C sequence one dispatches before queued sequence two"))
		return false;
	if (!expect(joiner.connection().seq.retained_outbound.empty(),
	            "retransmit's current server ACK retires every covered joiner record"))
		return false;

	const std::vector<uint8_t> c2s_ack = joiner.frame_inner(0x34, {});
	inmatch::handle_server_datagram(ctx, kPeer, c2s_ack.data(), c2s_ack.size(), 6);
	if (!expect(ctx.np_protocol.connection_list[0].seq.retained_outbound.empty(),
	            "joiner's admitted ACK retires the host's recovered S2C records"))
		return false;
	std::vector<uint8_t> final_server_ack;
	if (!expect(inmatch::frame_in_match_s2c(ctx, kPeer, 0x34, {}, final_server_ack),
	            "host frames final ACK-bearing S2C packet"))
		return false;
	joiner.handle_datagram(final_server_ack.data(), final_server_ack.size());
	return expect(joiner.connection().seq.retained_outbound.empty(),
	              "host's admitted ACK retires the joiner's remaining C2S records");
}

// A scripted datagram socket for the host owner loop: datagrams queued in `inbound` drain through
// recv_from in order, and every send_to lands in `sent`.
class ScriptedDatagramSocket final : public IDatagramSocket {
public:
	std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> inbound;
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty()) return 0;
		const std::vector<uint8_t> datagram = std::move(inbound.front().second);
		from = inbound.front().first;
		inbound.pop_front();
		if (datagram.size() > cap) return -1;
		std::memcpy(buf, datagram.data(), datagram.size());
		return static_cast<int>(datagram.size());
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

// D-NET-229: the host's receive pump runs every Server_TickUpdate, ungated, so the 0x83 packets a
// joiner's 0x44 asks for and the host's own 0x84 for a C2S gap leave in the SAME owner pump, even
// while the connection's S2C send boundary is closed (a NovaWorld host's 12-tick holdoff).
// [orig: Server_TickUpdate -> PumpServerProtocolRecv @0x51d895; NapiNP_HandleResendList ->
//  SendSessionPacket @0x6239b6; PumpRecvQueues tail -> SendMissingSeqList @0x6269ce]
bool check_host_receive_pump_sends_ignore_the_s2c_boundary() {
	inmatch::HostOwner owner;
	seed_host(owner.ctx);
	inmatch::NapiNPConnection &conn = owner.ctx.np_protocol.connection_list[0];
	inmatch::arm_s2c_send_holdoff(conn, 12);
	ScriptedDatagramSocket sock;

	std::vector<uint8_t> lost;
	std::vector<uint8_t> delivered;
	if (!expect(inmatch::frame_in_match_s2c(owner.ctx, kPeer, 0x49, {0xB1}, lost) &&
	                    inmatch::frame_in_match_s2c(owner.ctx, kPeer, 0x49, {0xB2}, delivered),
	            "host frames and retains two S2C packets"))
		return false;

	sock.inbound.emplace_back(kPeer, make_resend_datagram(
			SESSION_OPCODE_CLIENT_RESEND_LIST, kServerKey, {1}));
	inmatch::host_session_pump(owner, sock);
	if (!expect(!conn.s2c_send_boundary_open && conn.s2c_send_holdoff_countdown == 11,
	            "the S2C send boundary stays closed through this pump"))
		return false;
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(sock.sent.size() == 1 && sock.sent[0].first == kPeer &&
	                    decode_session_datagram(sock.sent[0].second,
	                            SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, kServerScrk,
	                            header, messages) &&
	                    header.seq_num == 1 && messages.size() == 1 &&
	                    messages[0].payload == std::vector<uint8_t>({0xB1}),
	            "the rebuilt S2C sequence one leaves in the pump that received the 0x44"))
		return false;
	if (!expect(owner.pending_session_datagrams.empty(),
	            "no rebuilt packet waits for the send boundary"))
		return false;

	// C2S sequence two arrives ahead of the lost sequence one: the 0x84 for it goes out in the
	// same pump, behind the closed S2C boundary.
	sock.sent.clear();
	SessionSequencing client_tx{2, 0};
	std::vector<uint8_t> c2s2;
	if (!expect(frame_test_session_datagram(
			client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
			SESSION_OPCODE_PROTOCOL_MESSAGE,
			{make_protocol_message(0x34, {0x02})}, c2s2),
	            "frame C2S sequence two"))
		return false;
	sock.inbound.emplace_back(kPeer, std::move(c2s2));
	inmatch::host_session_pump(owner, sock);
	std::vector<uint32_t> requested;
	if (!expect(!conn.s2c_send_boundary_open && sock.sent.size() == 1 &&
	                    decode_resend_datagram(sock.sent[0].second,
	                            SESSION_OPCODE_SERVER_RESEND_LIST, kClientKey, requested) &&
	                    requested == std::vector<uint32_t>({1}) &&
	                    !conn.seq.missing_request_pending,
	            "the host's 0x84 for the C2S gap leaves in the receiving pump"))
		return false;
	return expect(owner.pending_session_datagrams.empty(),
	              "the missing-sequence request never waits for the send boundary");
}

// D-NET-230: a host's send pump runs every Server_TickUpdate, so +0x64C advances once per server
// tick whether or not the connection's S2C boundary opened, and a finite-lifetime S2C record ages
// per tick. Retail's 0x57 RTT echo (userParam 62) built at counter C is pruned at the first build
// whose counter reaches C+61 — about a second — not 62 send boundaries later (12 s at holdoff 12).
// [orig: Server_TickUpdate -> PumpServerProtocolSend @0x51e487 (flags 0x2E1) -> PumpFlags
//  increment @0x6297d5; BuildOutgoingPackets deadline @0x6285a0; PrunePacketQueue @0x6292bb;
//  the 0x57 send SendFiltered(0x57, 1, 62) @0x51e450]
bool check_host_flush_counter_ages_finite_records_per_tick() {
	inmatch::HostOwner owner;
	seed_host(owner.ctx);
	inmatch::admit_peer(owner, kPeer);
	inmatch::NapiNPConnection &conn = owner.ctx.np_protocol.connection_list[0];
	if (!expect(conn.link.transport != nullptr, "the owner attached the peer's transport"))
		return false;
	inmatch::arm_s2c_send_holdoff(conn, 12);
	ScriptedDatagramSocket sock;

	// The finite record waits for the first open boundary (the 12th pump); every pump after that
	// also queues a one-send record so each boundary builds, as the per-boundary 0x0A does.
	conn.link.transport->host_send(0x57, {0x10, 0x20, 0x30, 0x40, 0x00},
			/*reliable=*/true, 0, false, /*retention_flushes=*/62);
	uint32_t pumps = 0;
	auto pump_to = [&](uint32_t target) {
		while (pumps < target) {
			inmatch::host_session_pump(owner, sock);
			++pumps;
			conn.link.transport->host_send(0x49, {0x01}, /*reliable=*/false);
		}
	};
	pump_to(11);
	if (!expect(sock.sent.empty(), "nothing leaves while the S2C boundary is held"))
		return false;
	pump_to(12);
	if (!expect(!sock.sent.empty() && conn.seq.retained_outbound_message_count == 1,
	            "the first open boundary sends and retains the finite record"))
		return false;
	if (!expect(conn.seq.send_flush_counter == 12,
	            "+0x64C advances once per host pump, held or open"))
		return false;
	// Built at counter 11 (pumps 1..11 advanced it), the record's deadline is 11 + 62 - 1 = 72.
	// The boundary at pump 72 builds at 71: the record survives and a 0x44 could still rebuild it.
	pump_to(72);
	if (!expect(conn.seq.retained_outbound_message_count == 1,
	            "the record survives every build before its deadline"))
		return false;
	// The boundary at pump 84 builds at 83 and prunes it.
	pump_to(84);
	return expect(conn.seq.send_flush_counter == 84 &&
	                      conn.seq.retained_outbound_message_count == 0,
	              "the first build past the deadline prunes the finite record");
}

// D-NET-233: a C2S packet that carried message records owes the joiner an ACK. With nothing else
// queued, the next open S2C boundary builds a header-only packet carrying it; a header-only C2S
// packet owes nothing. [orig: ParseMessages has_pending_out @0x625dff; PumpEnumeratorAndSend
// @0x6292a9 -> BuildOutgoingPackets @0x6292b4; the clear @0x628629]
bool check_host_owes_an_ack_for_c2s_records() {
	inmatch::HostOwner owner;
	seed_host(owner.ctx);
	inmatch::NapiNPConnection &conn = owner.ctx.np_protocol.connection_list[0];
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	inmatch::arm_s2c_send_holdoff(conn, 12);
	ScriptedDatagramSocket sock;
	SessionSequencing client_tx{1, 0};

	// A header-only C2S packet: admitted, owes nothing; the first boundary (pump 12) stays quiet.
	std::vector<uint8_t> empty_c2s;
	if (!expect(frame_test_session_datagram(client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
				SESSION_OPCODE_PROTOCOL_MESSAGE, {}, empty_c2s),
			"frame a header-only C2S packet"))
		return false;
	sock.inbound.emplace_back(kPeer, std::move(empty_c2s));
	for (int i = 0; i < 12; ++i) inmatch::host_session_pump(owner, sock);
	if (!expect(sock.sent.empty() && conn.seq.last_inbound_seq == 1,
			"a header-only C2S packet is admitted and owes no ACK"))
		return false;

	// A C2S packet with a record: the next open boundary (pump 24) sends one header-only ACK.
	std::vector<uint8_t> keepalive_c2s;
	if (!expect(frame_test_session_datagram(client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
				SESSION_OPCODE_PROTOCOL_MESSAGE,
				{make_protocol_message(0x34, {0x01, 0x00, 0x00, 0x00})}, keepalive_c2s),
			"frame a C2S packet with a record"))
		return false;
	sock.inbound.emplace_back(kPeer, std::move(keepalive_c2s));
	for (int i = 0; i < 11; ++i) inmatch::host_session_pump(owner, sock);
	if (!expect(sock.sent.empty(), "the owed ACK waits for the open boundary")) return false;
	inmatch::host_session_pump(owner, sock);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(sock.sent.size() == 1 &&
				decode_session_datagram(sock.sent[0].second,
						SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, kServerScrk, header, messages) &&
				messages.empty() && header.ack_count == 2,
			"the open boundary sends one header-only packet ACKing the record"))
		return false;
	// Owed once: the next boundary has nothing to say.
	sock.sent.clear();
	for (int i = 0; i < 12; ++i) inmatch::host_session_pump(owner, sock);
	return expect(sock.sent.empty(), "the ACK is owed once, not every boundary");
}

// D-NET-234: the host frames a connection's S2C under that connection's negotiated packet ceiling
// (cs_dir0 field 13), not a fixed 1300. [orig: BuildOutgoingPackets @0x628436; NapiNPServer_
// HandleNewConnection @0x4c81ca]
bool check_host_frames_under_the_negotiated_ceiling() {
	inmatch::HostOwner owner;
	seed_host(owner.ctx);
	inmatch::admit_peer(owner, kPeer);
	inmatch::NapiNPConnection &conn = owner.ctx.np_protocol.connection_list[0];
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.timeouts.max_packet_bytes = 576;
	ScriptedDatagramSocket sock;
	for (uint8_t i = 0; i < 3; ++i)
		conn.link.transport->host_send(0x49, std::vector<uint8_t>(300, i));
	inmatch::host_session_pump(owner, sock);
	bool within = !sock.sent.empty();
	for (const auto &datagram : sock.sent) within = within && datagram.second.size() <= 576;
	return expect(sock.sent.size() == 2 && within,
	              "three 300-byte records leave in two packets of at most 576 bytes");
}

// D-NET-236 (host): the EMPTY leg waits while an out-of-order C2S packet is held.
// [orig: CNapiNPConnection_PumpSendIntervals `cmp [esi+7A8h], 0` @0x629053]
bool check_host_keepalive_waits_while_a_packet_is_held() {
	inmatch::HostOwner owner;
	seed_host(owner.ctx);
	inmatch::NapiNPConnection &conn = owner.ctx.np_protocol.connection_list[0];
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	ScriptedDatagramSocket sock;
	// C2S sequence 2 arrives ahead of a lost 1 (header-only: it owes no ACK).
	SessionSequencing client_tx{2, 0};
	std::vector<uint8_t> future;
	if (!expect(frame_test_session_datagram(client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
				SESSION_OPCODE_PROTOCOL_MESSAGE, {}, future),
			"frame a future C2S packet"))
		return false;
	sock.inbound.emplace_back(kPeer, std::move(future));
	// Well past the 30000 ms empty interval (62 host ticks a second).
	for (int i = 0; i < 62 * 32; ++i) inmatch::host_session_pump(owner, sock);
	std::size_t keepalives = 0;
	for (const auto &datagram : sock.sent) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (nw_decode_inbound(datagram.second.data(), datagram.second.size(), opcode, body) &&
				opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE)
			++keepalives;
	}
	return expect(conn.seq.queued_inbound.size() == 1 && keepalives == 0,
			"the host mints no keepalive while a C2S packet is held out of order");
}

bool check_c2s_loss_requests_0x84_and_joiner_reconstructs() {
	inmatch::NapiNPServerCtx ctx;
	seed_host(ctx);
	inmatch::JoinerConnection joiner("LossRecovery");
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0001, 0x14B9);

	const std::vector<uint8_t> first = joiner.frame_inner(0x34, {0xA1});
	const std::vector<uint8_t> second = joiner.frame_inner(0x34, {0xA2});
	ProtocolPacketHeader original_header;
	std::vector<ProtocolMessage> original_messages;
	if (!expect(decode_session_datagram(
			first, SESSION_OPCODE_PROTOCOL_MESSAGE, kClientScrk,
			original_header, original_messages) &&
		            original_header.seq_num == 1 && original_header.ack_count == 0,
	            "original lost C2S packet starts with ACK zero"))
		return false;

	std::vector<uint8_t> server_packet;
	if (!expect(inmatch::frame_in_match_s2c(ctx, kPeer, 0x34, {}, server_packet),
	            "host frames one S2C packet to advance the joiner's current ACK"))
		return false;
	joiner.handle_datagram(server_packet.data(), server_packet.size());
	if (!expect(joiner.connection().seq.last_inbound_seq == 1,
	            "joiner ACK advances before it handles the resend request"))
		return false;

	const inmatch::HandleResult gap =
			inmatch::handle_server_datagram(ctx, kPeer, second.data(), second.size(), 10);
	if (!expect(gap.events.empty() && gap.outbound.empty(),
	            "host queues C2S sequence two without NACKing before the batch boundary"))
		return false;
	const std::vector<inmatch::TickOut> gap_nacks =
			inmatch::flush_server_missing_requests(ctx);
	if (!expect(gap_nacks.size() == 1 && gap_nacks[0].outbound.size() == 1 &&
	                    inmatch::flush_server_missing_requests(ctx).empty(),
	            "host emits exactly one NACK after the persistent-gap receive batch"))
		return false;
	// The sent 0x84 named a sequence: the host's incoming link error (flag 2)
	// [orig: SendMissingSeqList cb_server_5 @0x623788].
	if (!expect(ctx.net_quality_link_errors == inmatch::kNetQualityLinkErrorIncoming,
	            "the host's 0x84 raises its incoming link error"))
		return false;
	std::vector<uint32_t> requested;
	if (!expect(decode_resend_datagram(
			gap_nacks[0].outbound[0], SESSION_OPCODE_SERVER_RESEND_LIST, kClientKey, requested) &&
		            requested == std::vector<uint32_t>({1}),
	            "server 0x84 requests the missing first C2S sequence"))
		return false;

	const uint32_t next_before_bad = joiner.connection().seq.next_outbound_seq;
	const std::vector<uint8_t> wrong_key = make_resend_datagram(
			SESSION_OPCODE_SERVER_RESEND_LIST, kClientKey + 1, {1});
	const inmatch::JoinerConnection::PollResult wrong_key_result =
			joiner.handle_datagram(wrong_key.data(), wrong_key.size());
	if (!expect(wrong_key_result.outbound.empty() &&
		            wrong_key_result.immediate_outbound.empty() &&
		            joiner.connection().seq.next_outbound_seq == next_before_bad,
	            "joiner ignores 0x84 with a mismatched local key"))
		return false;
	const std::vector<uint8_t> malformed =
			nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, {0x01, 0x00, 0x00});
	const inmatch::JoinerConnection::PollResult malformed_result =
			joiner.handle_datagram(malformed.data(), malformed.size());
	if (!expect(malformed_result.outbound.empty() &&
		            malformed_result.immediate_outbound.empty(),
	            "joiner ignores a resend-list body shorter than its key"))
		return false;

	if (!expect(joiner.take_net_quality_link_errors() == 0,
	            "invalid resend lists raise no joiner link error"))
		return false;
	const inmatch::JoinerConnection::PollResult resend =
			joiner.handle_datagram(
					gap_nacks[0].outbound[0].data(),
					gap_nacks[0].outbound[0].size());
	// The rebuilt packet is transmitted by the resend handler itself, not held for the
	// send boundary [orig: NapiNP_HandleResendList -> SendSessionPacket @0x6239b6].
	if (!expect(resend.immediate_outbound.size() == 1 && resend.outbound.empty(),
	            "valid server 0x84 makes the joiner transmit one reconstructed packet at once"))
		return false;
	// The honoured 0x84 named a sequence: the joiner's outgoing link error
	// (flag 1) [orig: NapiNP_HandleResendList cb_client_3 @0x623a24].
	if (!expect(joiner.take_net_quality_link_errors() == inmatch::kNetQualityLinkErrorOutgoing,
	            "a valid 0x84 raises the joiner's outgoing link error"))
		return false;
	ProtocolPacketHeader resent_header;
	std::vector<ProtocolMessage> resent_messages;
	if (!expect(decode_session_datagram(
			resend.immediate_outbound[0], SESSION_OPCODE_PROTOCOL_MESSAGE, kClientScrk,
			resent_header, resent_messages) &&
		            resent_header.seq_num == 1 && resent_header.ack_count == 1 &&
		            resent_messages.size() == 1 && resent_messages[0].tag == 0x34 &&
		            resent_messages[0].payload == std::vector<uint8_t>({0xA1}),
	            "joiner reconstructs old C2S records with its current ACK"))
		return false;

	inmatch::handle_server_datagram(
			ctx, kPeer, resend.immediate_outbound[0].data(),
			resend.immediate_outbound[0].size(), 11);
	if (!expect(ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 2 &&
	                    ctx.np_protocol.connection_list[0].seq.queued_inbound.empty(),
	            "recovered C2S sequence one admits and drains queued sequence two"))
		return false;

	std::vector<uint8_t> server_ack;
	if (!expect(inmatch::frame_in_match_s2c(ctx, kPeer, 0x34, {}, server_ack),
	            "host frames ACK for the recovered C2S frontier"))
		return false;
	joiner.handle_datagram(server_ack.data(), server_ack.size());
	if (!expect(joiner.connection().seq.retained_outbound.empty(),
	            "host ACK retires both recovered joiner records"))
		return false;
	const std::vector<uint8_t> client_ack = joiner.frame_inner(0x34, {});
	inmatch::handle_server_datagram(ctx, kPeer, client_ack.data(), client_ack.size(), 12);
	return expect(ctx.np_protocol.connection_list[0].seq.retained_outbound.empty(),
	              "joiner ACK retires the host's ACK-bearing session records");
}

// Retail's resend receiver walks EVERY requested dword in the one datagram —
// the 16-entry bound is the BUILDER's stack buffer, not a receiver cap
// [orig: NapiNP_HandleResendList @ 0x623917..0x6239da do-while over
// buf..buf+size-4; the 16 lives in SendMissingSeqList's missing_seq_buf[16]
// @ 0x62367a]. Three retained records requested in one 0x44 come back as
// three reconstructed packets, each under its old sequence, in request order.
bool check_multi_sequence_resend_request_reconstructs_each() {
	inmatch::NapiNPServerCtx ctx;
	seed_host(ctx);
	inmatch::JoinerConnection joiner("MultiNack");
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0001, 0x14B9);

	std::vector<uint8_t> framed;
	const uint8_t payloads[3] = {0xD1, 0xD2, 0xD3};
	for (uint8_t byte : payloads) {
		if (!expect(inmatch::frame_in_match_s2c(ctx, kPeer, 0x49, {byte}, framed),
		            "host frames and retains three S2C packets"))
			return false;
	}

	const std::vector<uint8_t> nack = make_resend_datagram(
			SESSION_OPCODE_CLIENT_RESEND_LIST, kServerKey, {1, 2, 3});
	const inmatch::HandleResult resent = inmatch::handle_server_datagram(
			ctx, kPeer, nack.data(), nack.size(), 3);
	if (!expect(resent.immediate_outbound.size() == 3,
	            "one 0x44 carrying three requested sequences reconstructs three packets"))
		return false;
	for (int i = 0; i < 3; ++i) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!expect(decode_session_datagram(
				resent.immediate_outbound[static_cast<size_t>(i)],
				SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, kServerScrk,
				header, messages) &&
		                    header.seq_num == static_cast<uint32_t>(i + 1) &&
		                    messages.size() == 1 &&
		                    messages[0].payload ==
		                            std::vector<uint8_t>({payloads[i]}),
		            "each reconstructed packet carries its old sequence and record"))
			return false;
	}
	return true;
}

// The recipient backoff latch (cb_server_6 -> slot+89876) is armed only by a
// NONZERO requested dword [orig: NapiNP_HandleResendList @0x6239aa; the
// callback gate @0x6239ef; the key-only early return @0x623974]. A zero-only
// "send next" list still mints the next fresh packet, and a key-only body is
// accepted but sends nothing; neither halves the next 0x0A budget.
bool check_zero_only_and_key_only_resend_lists_do_not_arm_backoff() {
	inmatch::NapiNPServerCtx ctx;
	seed_host(ctx);
	std::vector<uint8_t> framed;
	if (!expect(inmatch::frame_in_match_s2c(ctx, kPeer, 0x49, {0xE1}, framed) &&
	                    inmatch::frame_in_match_s2c(ctx, kPeer, 0x49, {0xE2}, framed),
	            "host frames and retains two S2C packets"))
		return false;
	auto conn = [&]() -> inmatch::NapiNPConnection & {
		return ctx.np_protocol.connection_list[0];
	};
	const uint32_t next_before = conn().seq.next_outbound_seq;

	const std::vector<uint8_t> zero_only = make_resend_datagram(
			SESSION_OPCODE_CLIENT_RESEND_LIST, kServerKey, {0});
	const inmatch::HandleResult minted = inmatch::handle_server_datagram(
			ctx, kPeer, zero_only.data(), zero_only.size(), 3);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(minted.immediate_outbound.size() == 1 &&
	                    decode_session_datagram(
			minted.immediate_outbound[0], SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			kServerScrk, header, messages) &&
	                    header.seq_num == next_before && messages.empty() &&
	                    conn().seq.next_outbound_seq == next_before + 1,
	            "a zero-only 0x44 mints the next fresh sequence"))
		return false;
	if (!expect(!conn().link.nak_backoff_pending && ctx.net_quality_link_errors == 0,
	            "a zero-only resend list arms neither the backoff nor the link error"))
		return false;

	std::vector<uint8_t> key_only;
	if (!expect(encode_session_resend_list(kServerKey, {}, key_only),
	            "encode a key-only resend body"))
		return false;
	const std::vector<uint8_t> key_only_datagram =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_RESEND_LIST, std::move(key_only));
	if (!expect(inmatch::handle_server_datagram(
			ctx, kPeer, key_only_datagram.data(), key_only_datagram.size(), 4)
			                    .immediate_outbound.empty() &&
	                    !conn().link.nak_backoff_pending,
	            "a key-only resend body sends nothing and does not arm the backoff"))
		return false;
	if (!expect(ctx.net_quality_link_errors == 0,
	            "a key-only resend body raises no link error"))
		return false;

	const std::vector<uint8_t> real = make_resend_datagram(
			SESSION_OPCODE_CLIENT_RESEND_LIST, kServerKey, {0, 2});
	if (!expect(inmatch::handle_server_datagram(
			ctx, kPeer, real.data(), real.size(), 5).immediate_outbound.size() == 2 &&
	                    conn().link.nak_backoff_pending,
	            "a list with one nonzero requested sequence arms the backoff"))
		return false;
	return expect(ctx.net_quality_link_errors == inmatch::kNetQualityLinkErrorOutgoing,
	              "the nonzero list raises the outgoing link error");
}

// C2S uses the same connection-local FIRST/MID/FINAL assembly as S2C. A
// physical FIRST record is not a gameplay message: only the completed payload
// at FINAL may cross the host's public in-match event seam.
bool check_c2s_fragments_dispatch_once_after_final() {
	inmatch::NapiNPServerCtx ctx;
	seed_host(ctx);
	SessionSequencing client_tx{1, 0};

	const std::vector<uint8_t> first_payload = {0x05, 0x00, 0xB9, 0x14};
	const std::vector<uint8_t> final_payload = {0x0A, 0xDE, 0xAD, 0xBE, 0xEF};
	std::vector<uint8_t> first;
	std::vector<uint8_t> final;
	if (!expect(frame_test_session_datagram(
			client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
			SESSION_OPCODE_PROTOCOL_MESSAGE,
			{make_protocol_message(c2s::ENTITY_UPLINK, first_payload,
					static_cast<uint8_t>(PROTOCOL_MSG_FLAG_LEN16 |
							PROTOCOL_MSG_FLAG_FRAG_CONT))},
			first) &&
	                    frame_test_session_datagram(
			client_tx, SessionCrypto{kClientScrk, {}, kServerKey},
			SESSION_OPCODE_PROTOCOL_MESSAGE,
			{make_protocol_message(c2s::ENTITY_UPLINK, final_payload,
					static_cast<uint8_t>(PROTOCOL_MSG_FLAG_LEN16 |
							PROTOCOL_MSG_FLAG_FRAG_END))},
			final),
			"frame C2S FIRST and FINAL records"))
		return false;

	const inmatch::HandleResult first_result = inmatch::handle_server_datagram(
			ctx, kPeer, first.data(), first.size(), 20);
	if (!expect(first_result.events.empty() && first_result.outbound.empty(),
			"host does not dispatch or reply to an incomplete C2S FIRST record"))
		return false;

	const inmatch::HandleResult final_result = inmatch::handle_server_datagram(
			ctx, kPeer, final.data(), final.size(), 21);
	if (!expect(final_result.events.size() == 1 &&
	                    final_result.events[0].kind ==
						inmatch::HostAcceptEvent::Kind::PeerC2SInMatch &&
	                    final_result.events[0].in_match_c2s.size() == 1,
			"host dispatches exactly one semantic C2S message at FINAL"))
		return false;
	const ProtocolMessage &message =
			final_result.events[0].in_match_c2s.front();
	std::vector<uint8_t> expected = first_payload;
	expected.insert(expected.end(), final_payload.begin(), final_payload.end());
	return expect(message.tag == c2s::ENTITY_UPLINK &&
	                      message.payload == expected &&
	                      !message.flags.frag_cont && !message.flags.frag_end,
			"host dispatches the complete reassembled C2S payload without physical fragment flags");
}

} // namespace

int main() {
	bool ok = true;
	ok = check_session_header_key_validation() && ok;
	ok = check_reordered_same_batch_closes_gap_without_nack() && ok;
	ok = check_s2c_loss_requests_0x44_and_host_reconstructs() && ok;
	ok = check_c2s_loss_requests_0x84_and_joiner_reconstructs() && ok;
	ok = check_host_receive_pump_sends_ignore_the_s2c_boundary() && ok;
	ok = check_host_flush_counter_ages_finite_records_per_tick() && ok;
	ok = check_host_owes_an_ack_for_c2s_records() && ok;
	ok = check_host_frames_under_the_negotiated_ceiling() && ok;
	ok = check_host_keepalive_waits_while_a_packet_is_held() && ok;
	ok = check_multi_sequence_resend_request_reconstructs_each() && ok;
	ok = check_zero_only_and_key_only_resend_lists_do_not_arm_backoff() && ok;
	ok = check_c2s_fragments_dispatch_once_after_final() && ok;
	return ok ? 0 : 1;
}
