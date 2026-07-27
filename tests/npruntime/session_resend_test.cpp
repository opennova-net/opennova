// Retail 0x44/0x84 missing-sequence recovery across the two public in-match runtime seams.
//
// Each direction drops packet 1, delivers packet 2, observes the exact NACK, and feeds the
// reconstructed packet 1 back through the receiver so its queued packet 2 drains. The tests also
// pin key validation, malformed-body rejection, current-ACK retransmit headers, and ACK retirement.

#include <npruntime/joiner_connection.h>
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_protocol.h>
#include <npruntime/napi_np_server_ctx.h>

#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h>
#include <npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;

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

void seed_host(np::NapiNPServerCtx &ctx) {
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	np::NapiNPConnection conn;
	conn.connection_id = 3;
	conn.type = 1;
	conn.phase = np::ConnectionPhase::InMatch;
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
	np::NapiNPServerCtx ctx;
	seed_host(ctx);
	SessionSequencing client_tx{1, 0};
	std::vector<uint8_t> wrong_c2s;
	if (!expect(frame_test_session_datagram(
			client_tx, SessionCrypto{kClientScrk, {}, kServerKey + 1},
			SESSION_OPCODE_PROTOCOL_MESSAGE,
			{make_protocol_message(0x34, {0xA1})}, wrong_c2s),
	            "frame C2S packet for a different server session"))
		return false;
	const np::HandleResult host_drop = np::handle_server_datagram(
			ctx, kPeer, wrong_c2s.data(), wrong_c2s.size(), 1);
	if (!expect(host_drop.outbound.empty() && host_drop.events.empty() &&
	                    ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 0 &&
	                    ctx.np_protocol.connection_list[0].seq.queued_inbound.empty(),
	            "host drops wrong-SK C2S before sequencing or dispatch"))
		return false;

	// Joiner mirror: S2C headers are addressed to its client-local CK.
	np::JoinerConnection joiner("KeyValidation");
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
	const np::JoinerConnection::PollResult joiner_drop =
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
	// consumed before pump(), so seq2 admits and drains seq3 before the missing latch is resolved.
	np::JoinerConnection joiner("SameBatch");
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
	const np::JoinerConnection::PollResult joiner_close =
			joiner.handle_datagram(s2c2.data(), s2c2.size());
	if (!expect(joiner_close.inbound_gameplay.size() == 2 &&
	                    joiner.connection().seq.queued_inbound.empty(),
	            "later S2C packet in the batch closes and drains the gap"))
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
	np::NapiNPServerCtx ctx;
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
	if (!expect(np::handle_server_datagram(
			ctx, kPeer, c2s3.data(), c2s3.size(), 1).outbound.empty(),
	            "host queues the future C2S packet"))
		return false;
	if (!expect(np::handle_server_datagram(
			ctx, kPeer, c2s2.data(), c2s2.size(), 1).outbound.empty() &&
	                    ctx.np_protocol.connection_list[0].seq.queued_inbound.empty(),
	            "later C2S packet in the batch closes and drains the gap"))
		return false;
	if (!expect(np::flush_server_missing_requests(ctx).empty() &&
	                    !ctx.np_protocol.connection_list[0].seq.missing_request_pending,
	            "host batch boundary suppresses a NACK after same-batch recovery"))
		return false;
	return true;
}

bool check_s2c_loss_requests_0x44_and_host_reconstructs() {
	np::NapiNPServerCtx ctx;
	seed_host(ctx);
	np::JoinerConnection joiner("LossRecovery");
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0001, 0x14B9);

	if (!expect(ctx.np_protocol.connection_list[0].seq.outbound_message_limit ==
	                    np::JO_GAME_SESSION_OUTBOUND_MESSAGE_MAX,
	            "JO game host connection opts into retail's bounded reliable-message retention"))
		return false;
	if (!expect(joiner.connection().seq.outbound_message_limit ==
	                    np::JO_GAME_SESSION_OUTBOUND_MESSAGE_MAX,
	            "JO game joiner connection opts into retail's bounded reliable-message retention"))
		return false;

	// Give the host an admitted C2S sequence before its first S2C packet, then advance that ACK once
	// more after framing. The retransmit must carry ACK=2 even though the original carried ACK=1.
	const std::vector<uint8_t> c2s1 = joiner.frame_inner(0x34, {});
	np::handle_server_datagram(ctx, kPeer, c2s1.data(), c2s1.size(), 1);

	std::vector<uint8_t> first;
	std::vector<uint8_t> second;
	if (!expect(np::frame_in_match_s2c(ctx, kPeer, 0x49, {0xC5}, first) &&
	                    np::frame_in_match_s2c(ctx, kPeer, 0x49, {0xC6}, second),
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
	np::handle_server_datagram(ctx, kPeer, c2s2.data(), c2s2.size(), 2);
	if (!expect(ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 2,
	            "host ACK advances before handling the resend request"))
		return false;

	const np::JoinerConnection::PollResult gap =
			joiner.handle_datagram(second.data(), second.size());
	if (!expect(gap.inbound_gameplay.empty() && gap.outbound.empty(),
	            "joiner queues S2C sequence two without NACKing before the batch boundary"))
		return false;
	const std::vector<std::vector<uint8_t>> gap_nacks = joiner.pump(3);
	if (!expect(gap_nacks.size() == 1 && joiner.pump(3).empty(),
	            "joiner emits exactly one NACK after the persistent-gap receive batch"))
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
	if (!expect(np::handle_server_datagram(
			ctx, kPeer, wrong_key.data(), wrong_key.size(), 3).outbound.empty() &&
		            ctx.np_protocol.connection_list[0].seq.next_outbound_seq ==
		                    next_before_bad,
	            "host ignores 0x44 with a mismatched local key"))
		return false;
	const std::vector<uint8_t> malformed =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_RESEND_LIST, {0x88, 0x77, 0x66});
	if (!expect(np::handle_server_datagram(
			ctx, kPeer, malformed.data(), malformed.size(), 4).outbound.empty(),
	            "host ignores a resend-list body shorter than its key"))
		return false;

	const np::HandleResult resend = np::handle_server_datagram(
			ctx, kPeer, gap_nacks[0].data(), gap_nacks[0].size(), 5);
	if (!expect(resend.outbound.size() == 1,
	            "valid client 0x44 makes the host emit one reconstructed packet"))
		return false;
	ProtocolPacketHeader resent_header;
	std::vector<ProtocolMessage> resent_messages;
	if (!expect(decode_session_datagram(
			resend.outbound[0], SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, kServerScrk,
			resent_header, resent_messages) &&
		            resent_header.seq_num == 1 && resent_header.ack_count == 2 &&
		            resent_messages.size() == 1 && resent_messages[0].tag == 0x49 &&
		            resent_messages[0].payload == std::vector<uint8_t>({0xC5}),
	            "host reconstructs old S2C records with its current ACK"))
		return false;

	const np::JoinerConnection::PollResult recovered =
			joiner.handle_datagram(resend.outbound[0].data(), resend.outbound[0].size());
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
	np::handle_server_datagram(ctx, kPeer, c2s_ack.data(), c2s_ack.size(), 6);
	if (!expect(ctx.np_protocol.connection_list[0].seq.retained_outbound.empty(),
	            "joiner's admitted ACK retires the host's recovered S2C records"))
		return false;
	std::vector<uint8_t> final_server_ack;
	if (!expect(np::frame_in_match_s2c(ctx, kPeer, 0x34, {}, final_server_ack),
	            "host frames final ACK-bearing S2C packet"))
		return false;
	joiner.handle_datagram(final_server_ack.data(), final_server_ack.size());
	return expect(joiner.connection().seq.retained_outbound.empty(),
	              "host's admitted ACK retires the joiner's remaining C2S records");
}

bool check_c2s_loss_requests_0x84_and_joiner_reconstructs() {
	np::NapiNPServerCtx ctx;
	seed_host(ctx);
	np::JoinerConnection joiner("LossRecovery");
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
	if (!expect(np::frame_in_match_s2c(ctx, kPeer, 0x34, {}, server_packet),
	            "host frames one S2C packet to advance the joiner's current ACK"))
		return false;
	joiner.handle_datagram(server_packet.data(), server_packet.size());
	if (!expect(joiner.connection().seq.last_inbound_seq == 1,
	            "joiner ACK advances before it handles the resend request"))
		return false;

	const np::HandleResult gap =
			np::handle_server_datagram(ctx, kPeer, second.data(), second.size(), 10);
	if (!expect(gap.events.empty() && gap.outbound.empty(),
	            "host queues C2S sequence two without NACKing before the batch boundary"))
		return false;
	const std::vector<np::TickOut> gap_nacks =
			np::flush_server_missing_requests(ctx);
	if (!expect(gap_nacks.size() == 1 && gap_nacks[0].outbound.size() == 1 &&
	                    np::flush_server_missing_requests(ctx).empty(),
	            "host emits exactly one NACK after the persistent-gap receive batch"))
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
	if (!expect(joiner.handle_datagram(
			wrong_key.data(), wrong_key.size()).outbound.empty() &&
		            joiner.connection().seq.next_outbound_seq == next_before_bad,
	            "joiner ignores 0x84 with a mismatched local key"))
		return false;
	const std::vector<uint8_t> malformed =
			nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, {0x01, 0x00, 0x00});
	if (!expect(joiner.handle_datagram(
			malformed.data(), malformed.size()).outbound.empty(),
	            "joiner ignores a resend-list body shorter than its key"))
		return false;

	const np::JoinerConnection::PollResult resend =
			joiner.handle_datagram(
					gap_nacks[0].outbound[0].data(),
					gap_nacks[0].outbound[0].size());
	if (!expect(resend.outbound.size() == 1,
	            "valid server 0x84 makes the joiner emit one reconstructed packet"))
		return false;
	ProtocolPacketHeader resent_header;
	std::vector<ProtocolMessage> resent_messages;
	if (!expect(decode_session_datagram(
			resend.outbound[0], SESSION_OPCODE_PROTOCOL_MESSAGE, kClientScrk,
			resent_header, resent_messages) &&
		            resent_header.seq_num == 1 && resent_header.ack_count == 1 &&
		            resent_messages.size() == 1 && resent_messages[0].tag == 0x34 &&
		            resent_messages[0].payload == std::vector<uint8_t>({0xA1}),
	            "joiner reconstructs old C2S records with its current ACK"))
		return false;

	np::handle_server_datagram(
			ctx, kPeer, resend.outbound[0].data(), resend.outbound[0].size(), 11);
	if (!expect(ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 2 &&
	                    ctx.np_protocol.connection_list[0].seq.queued_inbound.empty(),
	            "recovered C2S sequence one admits and drains queued sequence two"))
		return false;

	std::vector<uint8_t> server_ack;
	if (!expect(np::frame_in_match_s2c(ctx, kPeer, 0x34, {}, server_ack),
	            "host frames ACK for the recovered C2S frontier"))
		return false;
	joiner.handle_datagram(server_ack.data(), server_ack.size());
	if (!expect(joiner.connection().seq.retained_outbound.empty(),
	            "host ACK retires both recovered joiner records"))
		return false;
	const std::vector<uint8_t> client_ack = joiner.frame_inner(0x34, {});
	np::handle_server_datagram(ctx, kPeer, client_ack.data(), client_ack.size(), 12);
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
	np::NapiNPServerCtx ctx;
	seed_host(ctx);
	np::JoinerConnection joiner("MultiNack");
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0001, 0x14B9);

	std::vector<uint8_t> framed;
	const uint8_t payloads[3] = {0xD1, 0xD2, 0xD3};
	for (uint8_t byte : payloads) {
		if (!expect(np::frame_in_match_s2c(ctx, kPeer, 0x49, {byte}, framed),
		            "host frames and retains three S2C packets"))
			return false;
	}

	const std::vector<uint8_t> nack = make_resend_datagram(
			SESSION_OPCODE_CLIENT_RESEND_LIST, kServerKey, {1, 2, 3});
	const np::HandleResult resent = np::handle_server_datagram(
			ctx, kPeer, nack.data(), nack.size(), 3);
	if (!expect(resent.outbound.size() == 3,
	            "one 0x44 carrying three requested sequences reconstructs three packets"))
		return false;
	for (int i = 0; i < 3; ++i) {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!expect(decode_session_datagram(
				resent.outbound[static_cast<size_t>(i)],
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

} // namespace

int main() {
	bool ok = true;
	ok = check_session_header_key_validation() && ok;
	ok = check_reordered_same_batch_closes_gap_without_nack() && ok;
	ok = check_s2c_loss_requests_0x44_and_host_reconstructs() && ok;
	ok = check_c2s_loss_requests_0x84_and_joiner_reconstructs() && ok;
	ok = check_multi_sequence_resend_request_reconstructs_each() && ok;
	return ok ? 0 : 1;
}
