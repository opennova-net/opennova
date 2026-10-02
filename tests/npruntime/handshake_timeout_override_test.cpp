// The `_NSTMOUT.TXT` connection-template override and the joiner's 0x82 CS overlay.
//
// CNapiNetwork_Init seeds timeout_ms = 120000 / msg_out_max = 1200, then reads a loose
// `_NSTMOUT.TXT` from the game directory: a case-insensitive NEVER prefix sets both to -1,
// atol(text) >= 0 sets timeout_ms = 1000 * seconds (msg_out_max untouched), a negative number
// sets both to -1. The host stores the pair into both cs_dir blocks, every node is created from
// them, and the 0x82 advertises them; a joiner overlays the host's CLIENT-direction CS entries
// onto its own template at acceptance, so the host's override governs the joiner's reap and
// pool too. Every consumer guards on `< 0`, so -1 disables rather than fires.
// [orig: CNapiNetwork_Init @0x4CA4A0 @0x4ca9d7..0x4caa4b; NapiNP_HandleServerJoinResponse
//  @0x629b4c..0x629b75 -> @0x629d72; PumpStateMachine @0x62934c/@0x6295a2;
//  NapiNPMessage_Create @0x628048]

#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/session_timeout_config.h>

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;

constexpr uint32_t kServerKey = 0x55667788u;
const std::string kServerScrk = "SERVER-NSTMOUT-SCRK";

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool check_parse(const char *text, int32_t timeout_ms, int32_t msg_out_max, const char *message) {
	inmatch::SessionTimeoutConfig cfg;
	inmatch::parse_nstmout(text, cfg);
	return expect(cfg.timeout_ms == timeout_ms && cfg.msg_out_max == msg_out_max, message);
}

bool check_parser_matches_network_init() {
	bool ok = true;
	const inmatch::SessionTimeoutConfig defaults;
	ok = expect(defaults.timeout_ms == 120000 && defaults.msg_out_max == 1200,
			"the template defaults are 120000 ms / 1200 records") && ok;
	const inmatch::SessionTimeoutConfig absent = inmatch::load_session_timeout_config("");
	ok = expect(absent.timeout_ms == 120000 && absent.msg_out_max == 1200,
			"no game directory keeps the defaults") && ok;
	ok = check_parse("NEVER", -1, -1, "NEVER disables the reap and the pool bound") && ok;
	ok = check_parse("never please", -1, -1, "the NEVER prefix is case-insensitive") && ok;
	ok = check_parse("30", 30000, 1200, "a positive number is seconds; the pool stays 1200") && ok;
	ok = check_parse("0", 0, 1200, "zero is a zero-millisecond reap") && ok;
	ok = check_parse("-5", -1, -1, "a negative number disables both") && ok;
	ok = check_parse("abc", 0, 1200, "non-numeric text atol's to 0") && ok;
	ok = check_parse("  45 seconds", 45000, 1200, "atol skips leading whitespace") && ok;
	ok = expect(inmatch::outbound_message_limit_for(-1) == 0 &&
					inmatch::outbound_message_limit_for(1200) == 1200,
			"a negative pool bound is the unbounded sentinel") && ok;
	return ok;
}

struct CsOverride {
	uint32_t field_index;
	uint32_t value;
};

// Drive a joiner through 0x41/0x81/0x42 and answer with a 0x82 whose CLIENT-direction CS block
// is the JOINTOPERATIONS template with `overrides` applied.
bool accept_with_cs_overrides(inmatch::JoinerConnection &joiner,
		const std::vector<CsOverride> &overrides) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello client_hello;
	const std::vector<uint8_t> hello = joiner.start();
	if (!expect(nw_decode_inbound(hello.data(), hello.size(), opcode, body) &&
					opcode == SESSION_OPCODE_CLIENT_HELLO &&
					parse_client_hello(body.data(), body.size(), client_hello),
			"decode the ClientHello"))
		return false;
	ServerHello server_hello = build_server_hello(client_hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const inmatch::JoinerConnection::PollResult hello_result =
			joiner.handle_datagram(server_hello_datagram.data(), server_hello_datagram.size());
	ClientAuth client_auth;
	body.clear();
	if (!expect(hello_result.outbound.size() == 1 &&
					nw_decode_inbound(hello_result.outbound[0].data(),
							hello_result.outbound[0].size(), opcode, body) &&
					opcode == SESSION_OPCODE_CLIENT_AUTH &&
					parse_client_auth(body.data(), body.size(), client_auth),
			"decode the ClientAuth"))
		return false;
	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, kServerScrk, "", "", "", false);
	server_auth.mi = 3;
	server_auth.client_cs = jointoperations_cs_fields();
	server_auth.server_cs = jointoperations_cs_fields();
	for (CsField &field : server_auth.client_cs) {
		for (const CsOverride &override_field : overrides)
			if (field.field_index == override_field.field_index) field.value = override_field.value;
	}
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	const inmatch::JoinerConnection::PollResult auth_result =
			joiner.handle_datagram(server_auth_datagram.data(), server_auth_datagram.size());
	return expect(auth_result.outbound.empty() &&
					joiner.phase() == inmatch::JoinerConnection::Phase::Driving,
			"the accepted 0x82 enters Driving");
}

// The 0x82 carrying `timeout_ms` (field 0) and `msg_out_max` (field 11).
bool accept_with_cs(inmatch::JoinerConnection &joiner, uint32_t timeout_ms, uint32_t msg_out_max) {
	return accept_with_cs_overrides(joiner, {{0, timeout_ms}, {11, msg_out_max}});
}

std::vector<uint8_t> frame_server_packet(SessionSequencing &server_tx, uint32_t client_key,
		const std::vector<ProtocolMessage> &messages) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(server_tx, SessionCrypto{kServerScrk, {}, client_key}, messages, body))
		return {};
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
}

// CS field 10 is the connection's out-of-order queue bound: a future packet is held only while
// fewer than that many are queued, a negative value holding every one. The joiner reads it off
// the host's 0x82 like every other cs_dir0 slot.
// [orig: NapiNPProtocol_HandleSessionPacket @0x626c18..0x626c32 - `test eax,eax; jl` to the
//  insert, `cmp [esi+7A8h], eax; jge` past it; the template's 100 @0x4cabe0;
//  NapiNP_HandleServerJoinResponse @0x629b4c..0x629b75]
bool check_joiner_holds_out_of_order_packets_up_to_field_10() {
	for (const uint32_t bound : {2u, 0xFFFFFFFFu}) {
		uint64_t now_ms = 1000;
		inmatch::JoinerConnection joiner("QueueBoundJoiner", [&now_ms] { return now_ms; });
		if (!accept_with_cs_overrides(joiner, {{10, bound}})) return false;
		if (!expect(joiner.session_timeouts().packet_queue_max == static_cast<int32_t>(bound),
				"the 0x82 overlays CS field 10"))
			return false;
		// S2C sequence 1 is lost; 101 later packets arrive behind it.
		SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing(2, 0);
		for (int i = 0; i < 101; ++i) {
			const std::vector<uint8_t> dg = frame_server_packet(
					server_tx, joiner.client_key(), {make_protocol_message(0x03, {0x00})});
			(void)joiner.handle_datagram(dg.data(), dg.size());
		}
		const std::size_t expected = bound == 2u ? 2u : 101u;
		if (!expect(joiner.inbound_gap_depth() == expected,
				bound == 2u ? "a field-10 bound of 2 holds two future packets"
				            : "a negative field 10 holds every future packet, past the template's 100"))
			return false;
	}
	return true;
}

// CS field 14 caps the packets one build sends: the template's -1 is unbounded, 1 sends one
// packet and leaves the rest of the queue, in order, for the next build.
// [orig: BuildOutgoingPackets @0x62844e (the load), @0x62860b..0x628619 (`max < 0` or
//  `built < max` loops again); the template's -1 @0x4cac18]
bool check_joiner_builds_at_most_field_14_packets() {
	uint64_t now_ms = 1000;
	inmatch::JoinerConnection joiner("PacketBudgetJoiner", [&now_ms] { return now_ms; });
	if (!accept_with_cs_overrides(joiner, {{14, 1}})) return false;
	if (!expect(joiner.session_timeouts().max_packets_per_tick == 1,
			"the 0x82 overlays CS field 14"))
		return false;
	// Three 600-byte records: two fit one 1300-byte packet, the third needs another.
	std::vector<ProtocolMessage> queue;
	for (uint8_t tag : {uint8_t{0x31}, uint8_t{0x32}, uint8_t{0x33}})
		queue.push_back(make_protocol_message(tag, std::vector<uint8_t>(600, tag)));
	const inmatch::JoinerConnection::FrameMessagesResult first =
			joiner.frame_messages_detailed(queue);
	if (!expect(first.datagrams.size() == 1 && first.framed_count == 2 &&
					first.unbuilt.size() == 1 && first.unbuilt[0].tag == 0x33,
			"a field-14 budget of one builds one packet and leaves the third record queued"))
		return false;
	const inmatch::JoinerConnection::FrameMessagesResult second =
			joiner.frame_messages_detailed(first.unbuilt);
	return expect(second.datagrams.size() == 1 && second.framed_count == 1 &&
					second.unbuilt.empty(),
			"the next build sends the record the budget left");
}

bool check_joiner_overlays_the_host_cs_block() {
	// The stock advertisement keeps the template.
	{
		uint64_t now_ms = 1000;
		inmatch::JoinerConnection joiner("StockJoiner", [&now_ms] { return now_ms; });
		if (!accept_with_cs(joiner, 120000u, 1200u)) return false;
		if (!expect(joiner.session_timeouts().timeout_ms == 120000 &&
						joiner.session_timeouts().msg_out_max == 1200 &&
						joiner.connection().seq.outbound_message_limit == 1200,
				"the stock 0x82 keeps 120000 / 1200"))
			return false;
		// The reap runs from acceptance (retail state 5), strictly greater-than.
		now_ms += 120000;
		if (!expect(!joiner.session_lost(), "120000 ms after acceptance is still inside the window"))
			return false;
		now_ms += 1;
		if (!expect(!joiner.session_lost(), "the reap waits for the send pump that runs it"))
			return false;
		(void)joiner.pump(0);
		if (!expect(joiner.session_lost() &&
						joiner.session_loss_reason().find("120 seconds") != std::string::npos,
				"120001 ms of silence after acceptance reaps, before any gameplay"))
			return false;
	}
	// A host NEVER (-1 / -1) reaches the joiner as 0xFFFFFFFF and disables both.
	{
		uint64_t now_ms = 1000;
		inmatch::JoinerConnection joiner("NeverJoiner", [&now_ms] { return now_ms; });
		if (!accept_with_cs(joiner, 0xFFFFFFFFu, 0xFFFFFFFFu)) return false;
		if (!expect(joiner.session_timeouts().timeout_ms == -1 &&
						joiner.session_timeouts().msg_out_max == -1 &&
						joiner.connection().seq.outbound_message_limit == 0,
				"a NEVER host advertises -1 / -1 and the joiner runs unbounded"))
			return false;
		now_ms += 10u * 60u * 1000u;
		if (!expect(!joiner.session_lost() && joiner.pump(0).size() <= 1,
				"a -1 timeout never reaps the joiner"))
			return false;
	}
	// A 30-second host override: 30000 ms stays healthy, 30001 ms reaps, the reason says 30 s.
	{
		uint64_t now_ms = 1000;
		inmatch::JoinerConnection joiner("ThirtyJoiner", [&now_ms] { return now_ms; });
		if (!accept_with_cs(joiner, 30000u, 1200u)) return false;
		now_ms += 30000;
		if (!expect(!joiner.session_lost(), "30000 ms is inside a 30 s window")) return false;
		now_ms += 1;
		(void)joiner.pump(0); // the send pump runs the reap (PumpStateMachine case 5)
		return expect(joiner.session_lost() &&
						joiner.session_loss_reason().find("30 seconds") != std::string::npos &&
						joiner.has_disconnect_event() &&
						joiner.last_disconnect_event().dc == 3 &&
						joiner.last_disconnect_event().dp1 == 30001 &&
						joiner.last_disconnect_event().dp2 == 30000 &&
						joiner.last_disconnect_event().ddstr == "NP.C:PT:CLNTTMOUT",
				"30001 ms reaps with the CLNTTMOUT record latched");
	}
}

} // namespace

int main() {
	bool ok = true;
	ok = check_parser_matches_network_init() && ok;
	ok = check_joiner_overlays_the_host_cs_block() && ok;
	ok = check_joiner_holds_out_of_order_packets_up_to_field_10() && ok;
	ok = check_joiner_builds_at_most_field_14_packets() && ok;
	return ok ? 0 : 1;
}
