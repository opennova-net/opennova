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
#include <runtime/inmatch/session_timeout_config.h>

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

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

// Drive a joiner through 0x41/0x81/0x42 and answer with a 0x82 whose CLIENT-direction CS block
// carries `timeout_ms` (field 0) and `msg_out_max` (field 11).
bool accept_with_cs(inmatch::JoinerConnection &joiner, uint32_t timeout_ms, uint32_t msg_out_max) {
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
		if (field.field_index == 0) field.value = timeout_ms;
		if (field.field_index == 11) field.value = msg_out_max;
	}
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	const inmatch::JoinerConnection::PollResult auth_result =
			joiner.handle_datagram(server_auth_datagram.data(), server_auth_datagram.size());
	return expect(auth_result.outbound.empty() &&
					joiner.phase() == inmatch::JoinerConnection::Phase::Driving,
			"the accepted 0x82 enters Driving");
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
	return ok ? 0 : 1;
}
