// A lossy UDP transport must not turn either pre-session or sequenced join legs into one-shots.
// The pre-session 0x41/0x42 legs retry byte-identically. Once a sequenced session exists, retail's
// active-send interval instead emits a NEW header-only sequence; the resulting gap makes the peer
// request reconstruction of the retained semantic packet through 0x44/0x84.

#include <runtime/inmatch/client_runtime.h>
#include <base/gameprofile/gameprofile.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_protocol.h>

#include "host_test_setup.h"

#include <base/io/le.h>
#include <net/npwire/flat_tlv.h>
#include <net/npwire/lan_discovery.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool is_opcode(const std::vector<uint8_t> &datagram, uint8_t expected) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected;
}

bool decode_session(const std::vector<uint8_t> &datagram, uint8_t expected_opcode,
		const std::string &scrk, ProtocolPacketHeader &header,
		std::vector<ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected_opcode &&
			decode_protocol_packet_plaintext(
					body.data(), body.size(), scrk, header, messages);
}

bool is_resend_request(const std::vector<uint8_t> &datagram, uint8_t expected_opcode,
		uint32_t local_key, uint32_t expected_sequence) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	std::vector<uint32_t> requested;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == expected_opcode &&
			decode_session_resend_list(
					body.data(), body.size(), local_key, requested) &&
			requested == std::vector<uint32_t>{expected_sequence};
}

// D-NET-236: the EMPTY leg also waits while an out-of-order packet is held: retail tests the
// connection's packet-queue count beside the retained count. A joiner holding S2C sequence 3
// behind a missing 2 mints no keepalive; once the gap closes the leg runs again.
// [orig: CNapiNPConnection_PumpSendIntervals @0x629041..0x629065 — `cmp [esi+768h], 0`
//  @0x62904b (retained), `cmp [esi+7A8h], 0` @0x629053 (the out-of-order queue)]
bool run_idle_keepalive_waits_while_a_packet_is_held() {
	constexpr uint64_t kIdleIntervalMs = 30000;
	constexpr uint32_t kServerKey = 0x5A5A4321u;
	constexpr uint32_t kClientKey = 1u;
	const std::string client_scrk = "CLIENT-HELD-KEEPALIVE-SCRK";
	const std::string server_scrk = "SERVER-HELD-KEEPALIVE-SCRK";
	uint64_t now_ms = 1000;
	inmatch::JoinerConnection joiner("HeldKeepalive", [&now_ms] { return now_ms; });
	joiner.set_title(&kJo);
	joiner.seed_in_match(kServerKey, kClientKey, client_scrk, server_scrk,
	                     1, 0, 0x0001, 0x14B9);
	(void)joiner.pump(0);
	// S2C sequence 1 lands, sequence 2 is lost, sequence 3 is held.
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> packets[3];
	for (std::vector<uint8_t> &datagram : packets) {
		std::vector<uint8_t> body;
		if (!expect(frame_session_packet(server_tx, SessionCrypto{server_scrk, {}, kClientKey},
					{}, body),
				"frame an S2C header-only packet"))
			return false;
		datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
	}
	joiner.handle_datagram(packets[0].data(), packets[0].size());
	joiner.handle_datagram(packets[2].data(), packets[2].size());
	if (!expect(joiner.inbound_gap_depth() == 1 && joiner.retained_outbound_depth() == 0,
			"held: sequence 3 waits behind the lost 2, nothing retained"))
		return false;
	now_ms = 1000 + kIdleIntervalMs + 1;
	if (!expect(joiner.pump(0).empty(),
			"held: no keepalive while an out-of-order packet is queued"))
		return false;
	joiner.handle_datagram(packets[1].data(), packets[1].size());
	return expect(joiner.inbound_gap_depth() == 0 && joiner.pump(0).size() == 1,
			"held: the keepalive runs once the gap closes");
}

bool run_dropped_hello_and_auth_recover() {
	// The enumerator's 0x41 re-announce, once MORE than 3000 ms passed with no time limit
	// (D-NET-703) — deliberately NOT the 10000-ms session active-send interval — and retail's
	// connect-state 0x42 cadence: re-sent once MORE than 2000 ms passed
	// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 - @0x6290ed, @0x629119;
	//  StartClientConnection @0x4ca2ab; PumpStateMachine @0x629588].
	constexpr uint64_t kHandshakeRetryMs = 3000;
	constexpr uint64_t kClientJoinRetryMs = 2000;
	const PeerAddr peer{0x0100007Fu, 32769};

	inmatch::NapiNPServerCtx host;
	uint64_t now_ms = 0;
	inmatch::ClientRuntime client("RetryJoiner", [&now_ms] { return now_ms; });
	client.set_title(&kJo);
	const std::vector<uint8_t> first_hello = client.start();
	if (!expect(is_opcode(first_hello, SESSION_OPCODE_CLIENT_HELLO),
			"start emits retail ClientHello 0x41")) {
		return false;
	}
	const inmatch::HandleResult cold_result = inmatch::handle_server_datagram(
			host, peer, first_hello.data(), first_hello.size(), 0);
	if (!expect(cold_result.outbound.empty(),
			"cold host drops the initial ClientHello")) {
		return false;
	}
	if (!expect(client.Client_ProcessNetworkFrame(7).empty(),
			"handshake retry timer arms without an immediate duplicate")) {
		return false;
	}
	now_ms = kHandshakeRetryMs;
	if (!expect(client.Client_ProcessNetworkFrame(7).empty(),
			"ClientHello is not re-announced until MORE than 3000 ms passed")) {
		return false;
	}

	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, 0x0FE0E112u);
	now_ms = kHandshakeRetryMs + 1;
	const std::vector<std::vector<uint8_t>> hello_retry =
			client.Client_ProcessNetworkFrame(7);
	if (!expect(hello_retry.size() == 1 && hello_retry[0] == first_hello,
			"dropped ClientHello retries byte-identically at the interval")) {
		return false;
	}

	const inmatch::HandleResult hello_result = inmatch::handle_server_datagram(
			host, peer, hello_retry[0].data(), hello_retry[0].size(), 1);
	if (!expect(hello_result.outbound.size() == 1 &&
				is_opcode(hello_result.outbound[0], SESSION_OPCODE_SERVER_HELLO),
			"real host accepts the retried ClientHello")) {
		return false;
	}
	client.receive(
			hello_result.outbound[0].data(), hello_result.outbound[0].size());
	const std::vector<std::vector<uint8_t>> auth_out =
			client.Client_ProcessNetworkFrame(9999);
	if (!expect(auth_out.size() == 1 &&
				is_opcode(auth_out[0], SESSION_OPCODE_CLIENT_AUTH),
			"ServerHello advances the joiner to ClientAuth 0x42")) {
		return false;
	}
	const std::vector<uint8_t> first_auth = auth_out[0];

	now_ms = kHandshakeRetryMs + 1 + kClientJoinRetryMs;
	if (!expect(client.Client_ProcessNetworkFrame(1).empty(),
			"ClientAuth is not retried until MORE than 2000 ms passed")) {
		return false;
	}
	now_ms = kHandshakeRetryMs + 1 + kClientJoinRetryMs + 1;
	const std::vector<std::vector<uint8_t>> auth_retry =
			client.Client_ProcessNetworkFrame(1);
	if (!expect(auth_retry.size() == 1 && auth_retry[0] == first_auth,
			"dropped ClientAuth retries byte-identically past 2000 ms")) {
		return false;
	}

	const inmatch::HandleResult auth_result = inmatch::handle_server_datagram(
			host, peer, auth_retry[0].data(), auth_retry[0].size(), 2);
	if (!expect(!auth_result.outbound.empty(),
			"real host accepts the retried ClientAuth")) {
		return false;
	}
	for (const std::vector<uint8_t> &reply : auth_result.outbound)
		client.receive(reply.data(), reply.size());
	const std::vector<std::vector<uint8_t>> post_auth =
			client.Client_ProcessNetworkFrame(300);
	if (!expect(client.phase() == inmatch::JoinerConnection::Phase::Driving,
			"retried pre-session handshake reaches the in-match drive")) {
		return false;
	}
	for (const std::vector<uint8_t> &datagram : post_auth) {
		if (!expect(!is_opcode(datagram, SESSION_OPCODE_CLIENT_AUTH),
				"ServerAuth cancels the ClientAuth retry")) {
			return false;
		}
	}
	return true;
}

// A host that answers the ServerHello but never the 0x42 fails the join after retail's 30000 ms
// connect window with connect error 2 (NCC002), the 0x42 having been re-sent every 2000+ ms.
// [orig: StartClientConnection @0x4ca2ab/@0x4ca2bb; InitFromSession @0x62648a; PumpStateMachine
//  case 3 @0x62951c (timeout, +0x5F0 = 2 @0x629525) / @0x629588 (resend); NCC002 @0x82ba60]
bool run_unanswered_client_join_fails_after_30000_ms() {
	const PeerAddr peer{0x0100007Fu, 32770};
	inmatch::NapiNPServerCtx host;
	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, 0x0FE0E113u);
	uint64_t now_ms = 5000;
	inmatch::ClientRuntime client("TimeoutJoiner", [&now_ms] { return now_ms; });
	client.set_title(&kJo);
	const std::vector<uint8_t> hello = client.start();
	const inmatch::HandleResult hello_result = inmatch::handle_server_datagram(
			host, peer, hello.data(), hello.size(), 0);
	if (!expect(hello_result.outbound.size() == 1, "host answers the ClientHello")) return false;
	client.receive(hello_result.outbound[0].data(), hello_result.outbound[0].size());
	const std::vector<std::vector<uint8_t>> auth = client.Client_ProcessNetworkFrame(1);
	if (!expect(auth.size() == 1 && is_opcode(auth[0], SESSION_OPCODE_CLIENT_AUTH),
			"ServerHello sends the first ClientAuth")) {
		return false;
	}
	// Every 0x42 is lost: the connect state re-sends past each 2000 ms gap until the window.
	const uint64_t start = now_ms;
	std::size_t resends = 0;
	for (now_ms = start + 1; now_ms <= start + 30000; now_ms += 1) {
		for (const std::vector<uint8_t> &datagram : client.Client_ProcessNetworkFrame(1)) {
			if (is_opcode(datagram, SESSION_OPCODE_CLIENT_AUTH)) ++resends;
		}
	}
	if (!expect(resends == 14, "the 0x42 is re-sent fourteen times in the 30000 ms window"))
		return false;
	if (!expect(client.phase() == inmatch::JoinerConnection::Phase::Auth &&
				!client.last_join_reject().set,
			"the join is still connecting at exactly 30000 ms")) {
		return false;
	}
	now_ms = start + 30001;
	const std::vector<std::vector<uint8_t>> expired = client.Client_ProcessNetworkFrame(1);
	return expect(expired.empty() &&
				client.phase() == inmatch::JoinerConnection::Phase::Error &&
				client.last_join_reject().set && client.last_join_reject().jfc == 2,
			"past 30000 ms the join fails with connect error 2 (NCC002) and stops sending");
}

// A join from a discovered row goes straight to the 0x42: the browse record already holds the
// host's 0x81, so the connection starts in the connect state with the record's HK (no new 0x41),
// and the host admits it. A bare dial still sends the 0x41 first.
// [orig: UI_JoinSelectedSession @0x5699d0 -> CNapiNetwork_StartClientConnection @0x4ca160 ->
//  CNapiNPConnection_InitFromSession @0x626320 (HK @0x62639c, state 3 @0x626496)]
bool run_discovered_row_join_skips_the_hello() {
	const PeerAddr scanner{0x0100007Fu, 32771};
	const PeerAddr joiner_peer{0x0100007Fu, 32772};
	inmatch::NapiNPServerCtx host;
	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, 0x0FE0E114u);
	// The browse: one probe, one 0x81, projected into the row record.
	const std::vector<uint8_t> probe = build_lan_discovery_probe(kJo, 0x00C0FFEEu, 0xFFFFFFFFu, 32768u);
	const inmatch::HandleResult browse = inmatch::handle_server_datagram(
			host, scanner, probe.data(), probe.size(), 0);
	LanDiscoveryServer row;
	if (!expect(browse.outbound.size() == 1 &&
	                    parse_lan_discovery_reply(kJo, browse.outbound[0].data(), browse.outbound[0].size(),
	                                              0x00C0FFEEu, row) &&
	                    row.host_key == 0x0FE0E114u && !row.password_required,
	            "the browse row records the host's HK and SF")) {
		return false;
	}
	uint64_t now_ms = 7000;
	inmatch::ClientRuntime client("RowJoiner", [&now_ms] { return now_ms; });
	client.set_title(&kJo);
	inmatch::JoinerConnection::DiscoveredSession session;
	session.present = true;
	session.host_key = row.host_key;
	session.password_required = row.password_required;
	session.expansion = row.expansion;
	client.set_discovered_session(session);
	const std::vector<uint8_t> first = client.start();
	if (!expect(is_opcode(first, SESSION_OPCODE_CLIENT_AUTH) &&
	                    client.phase() == inmatch::JoinerConnection::Phase::Auth,
	            "a discovered-row join's first datagram is the 0x42, not a 0x41")) {
		return false;
	}
	const inmatch::HandleResult admitted = inmatch::handle_server_datagram(
			host, joiner_peer, first.data(), first.size(), 0);
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ServerAuth sa;
	if (!expect(!admitted.outbound.empty() &&
	                    nw_decode_inbound(admitted.outbound[0].data(), admitted.outbound[0].size(),
	                                      opcode, body) &&
	                    opcode == SESSION_OPCODE_SERVER_AUTH &&
	                    parse_server_auth(body.data(), body.size(), sa) && sa.cr == 1u,
	            "the host admits the row's HK with an accepting 0x82")) {
		return false;
	}
	inmatch::ClientRuntime bare("BareJoiner", [&now_ms] { return now_ms; });
	bare.set_title(&kJo);
	return expect(is_opcode(bare.start(), SESSION_OPCODE_CLIENT_HELLO),
	              "a bare dial (no row record) still sends the 0x41 first");
}

// The flat-TLV field names of a body, in wire order.
std::vector<std::string> field_names(const std::vector<uint8_t> &body) {
	std::vector<std::string> names;
	std::size_t pos = 0;
	while (pos < body.size()) {
		FlatTlvField field;
		const std::size_t next = read_flat_tlv(body.data(), body.size(), pos, field);
		if (next == kFlatTlvEnd) break;
		names.emplace_back(field.name);
		pos = next;
	}
	return names;
}

// `name` is directly followed by `after` among `names`.
bool follows(const std::vector<std::string> &names, std::string_view name, std::string_view after) {
	for (std::size_t i = 0; i + 1 < names.size(); ++i)
		if (names[i] == name) return names[i + 1] == after;
	return false;
}

// D-NET-520: the connect writes the JO build string as PV3 and names its peer. A dial without a
// row announces to the dialed host, so its 0x41 writes that address and port as EIP / EPN; the
// 0x42 writes the session record's address and port (the reply's source, the same host) as SIP /
// SPN, and PV3 right after PV2, from a dial and from a discovered row alike. A joiner with no
// host (socketless) writes none of the four.
// [orig: CNapiNetwork_Init @0x4ca4a0 - PV3 into proto+0x1AC @0x4ca8bb..0x4ca8c2;
//  NapiNPSession_SendAnnouncePacket @0x61fa00 - PV3 @0x61fc68..0x61fc9a, EIP / EPN
//  @0x61fcf5..0x61fd47; CNapiNPConnection_InitFromSession @0x6263ae..0x6263b7;
//  CNapiNPConnection_SendClientJoin @0x61fe20 - PV3 @0x620088..0x6200ba, SIP @0x6201a9]
bool run_connect_writes_pv3_and_its_peer() {
	const std::string pv3 = "CONSUMER RELEASE 1.7.5.7 [8165168.8165140] Jul 21 2009 18:54:42";
	const PeerAddr host_addr{ 0x0A01A8C0u, 32786 }; // 192.168.1.10:32786 in PeerAddr's packing
	uint64_t now_ms = 0;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;

	inmatch::JoinerConnection dial("DialPeer", [&now_ms] { return now_ms; });
	dial.set_title(&kJo);
	dial.set_host_address(host_addr);
	const std::vector<uint8_t> hello_datagram = dial.start();
	ClientHello hello;
	if (!expect(nw_decode_inbound(hello_datagram.data(), hello_datagram.size(), opcode, body) &&
						opcode == SESSION_OPCODE_CLIENT_HELLO &&
						parse_client_hello(body.data(), body.size(), hello),
				"pv3/peer: a dial's first datagram is the 0x41"))
		return false;
	if (!expect(hello.pv3 == pv3 && follows(field_names(body), "PV2", "PV3"),
				"pv3/peer: the 0x41 carries the build string as PV3 after PV2"))
		return false;
	if (!expect(hello.eip == host_addr.ip && hello.epn == 32786u,
				"pv3/peer: the dial's 0x41 names the dialed host as EIP / EPN"))
		return false;
	ServerHello reply = build_server_hello(hello, 0x0A01A8C0u, 32786);
	reply.hk = 0x0FE0E117u;
	const std::vector<uint8_t> reply_datagram =
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply));
	const inmatch::JoinerConnection::PollResult joined =
			dial.handle_datagram(reply_datagram.data(), reply_datagram.size());
	ClientAuth auth;
	if (!expect(joined.outbound.size() == 1 &&
						nw_decode_inbound(joined.outbound[0].data(), joined.outbound[0].size(),
								opcode, body) &&
						opcode == SESSION_OPCODE_CLIENT_AUTH &&
						parse_client_auth(body.data(), body.size(), auth),
				"pv3/peer: the 0x81 draws the dial's 0x42"))
		return false;
	if (!expect(auth.pv3 == pv3 && follows(field_names(body), "PV2", "PV3") &&
						follows(field_names(body), "PV3", "CI"),
				"pv3/peer: the 0x42 carries PV3 between PV2 and CI"))
		return false;
	if (!expect(auth.sip == host_addr.ip && auth.spn == 32786u,
				"pv3/peer: the dial's 0x42 names the session's host as SIP / SPN"))
		return false;

	inmatch::JoinerConnection row("RowPeer", [&now_ms] { return now_ms; });
	row.set_title(&kJo);
	inmatch::JoinerConnection::DiscoveredSession session;
	session.present = true;
	session.host_key = 0x0FE0E117u;
	row.set_discovered_session(session);
	row.set_host_address(host_addr);
	const std::vector<uint8_t> row_first = row.start();
	ClientAuth row_auth;
	if (!expect(nw_decode_inbound(row_first.data(), row_first.size(), opcode, body) &&
						opcode == SESSION_OPCODE_CLIENT_AUTH &&
						parse_client_auth(body.data(), body.size(), row_auth) &&
						row_auth.pv3 == pv3 && row_auth.sip == host_addr.ip &&
						row_auth.spn == 32786u,
				"pv3/peer: a row's 0x42 carries PV3 and the row's host as SIP / SPN"))
		return false;

	inmatch::JoinerConnection socketless("NoPeer", [&now_ms] { return now_ms; });
	socketless.set_title(&kJo);
	const std::vector<uint8_t> bare = socketless.start();
	ClientHello bare_hello;
	const bool decoded = nw_decode_inbound(bare.data(), bare.size(), opcode, body) &&
			parse_client_hello(body.data(), body.size(), bare_hello);
	const std::vector<std::string> names = decoded ? field_names(body) : std::vector<std::string>{};
	return expect(decoded && bare_hello.pv3 == pv3 &&
					std::find(names.begin(), names.end(), "EIP") == names.end() &&
					std::find(names.begin(), names.end(), "EPN") == names.end(),
			"pv3/peer: a joiner with no host writes PV3 but no EIP / EPN");
}

// D-NET-521: one connection enumerates and joins. The connection counter pre-increments and
// steps over 0; a LAN browse takes the process's next index for its 0x41s, and the join from a
// row it discovered goes on with that connection: its first datagram is a 0x42 with the browse's
// CI, which the host's 0x82 echoes and the joiner accepts. A join without a row enumerates on
// its own connection, created as every enumerate's is, so its CI is the counter's second step
// (D-NET-703; next_browse_connection_index).
// [orig: CNapiNPConnection_Create @0x62acb0 - @0x62b282..0x62b296; CNapiNetwork_StartClientConnection
//  @0x4ca160 (the connection ctx+0xE60 whose enumerator found the session @0x4ca1dd..0x4ca1ee) ->
//  CNapiNPConnection_InitFromSession @0x626320]
bool run_one_connection_index_per_join() {
	uint32_t counter = 0xFFFFFFFEu;
	const uint32_t last = advance_connection_index(counter);
	const uint32_t wrapped = advance_connection_index(counter);
	if (!expect(last == 0xFFFFFFFFu && wrapped == 1u && counter == 1u,
	            "one index: the counter steps over 0 to 1"))
		return false;

	const PeerAddr joiner_peer{0x0100007Fu, 32790};
	inmatch::NapiNPServerCtx host;
	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostClient,
	                             inmatch::SocketMode::Socketless, 0x0FE0E119u);
	LanDiscoveryBrowser browser;
	if (!expect(browser.begin(kJo, next_connection_index(), 0xFFFFFFFFu, 32768, 32768,
	                          kLanConnectTypeLan),
	            "one index: the browse begins"))
		return false;
	const uint32_t browse_ci = browser.client_index();
	const std::vector<uint8_t> probe = browser.probe(32768);
	const inmatch::HandleResult browse =
			inmatch::handle_server_datagram(host, joiner_peer, probe.data(), probe.size(), 0);
	LanDiscoveryServer row;
	if (!expect(browse.outbound.size() == 1 &&
	                    parse_lan_discovery_reply(kJo, browse.outbound[0].data(),
	                                              browse.outbound[0].size(), browse_ci, row),
	            "one index: the browse's 0x81 answers its CI"))
		return false;

	uint64_t now_ms = 0;
	inmatch::JoinerConnection join("OneConnection", [&now_ms] { return now_ms; });
	join.set_title(&kJo);
	inmatch::JoinerConnection::DiscoveredSession session;
	session.present = true;
	session.host_key = row.host_key;
	session.password_required = row.password_required;
	session.client_index = browse_ci;
	join.set_discovered_session(session);
	const std::vector<uint8_t> first = join.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	if (!expect(nw_decode_inbound(first.data(), first.size(), opcode, body) &&
	                    opcode == SESSION_OPCODE_CLIENT_AUTH &&
	                    parse_client_auth(body.data(), body.size(), auth) &&
	                    auth.ci == browse_ci && join.client_index() == browse_ci,
	            "one index: the row's join sends a 0x42 with the browse's CI"))
		return false;
	if (!expect(join.join_screen_stage() == inmatch::JoinScreenStage::Joining,
	            "one index: the row's join reads PRE_JOININGSESSION"))
		return false;
	const inmatch::HandleResult admitted =
			inmatch::handle_server_datagram(host, joiner_peer, first.data(), first.size(), 1);
	ServerAuth sa;
	if (!expect(!admitted.outbound.empty() &&
	                    nw_decode_inbound(admitted.outbound[0].data(),
	                                      admitted.outbound[0].size(), opcode, body) &&
	                    opcode == SESSION_OPCODE_SERVER_AUTH &&
	                    parse_server_auth(body.data(), body.size(), sa) && sa.cr == 1u &&
	                    sa.ci == browse_ci,
	            "one index: the host's 0x82 echoes the browse's CI"))
		return false;
	join.handle_datagram(admitted.outbound[0].data(), admitted.outbound[0].size());
	if (!expect(join.phase() == inmatch::JoinerConnection::Phase::Driving,
	            "one index: the joiner accepts the 0x82 for its CI"))
		return false;

	// The process's counter as it stands, and the index a join without a row carries: the
	// enumerate creates its connection twice and keeps the second.
	// [orig: UI_EnumerateAndJoinSession @0x569f50 - CreateServerConnection @0x56a011, then
	//  InitTransportConnection @0x56a097, whose own create @0x4c9e16 replaces the first]
	uint32_t expected = next_connection_index();
	advance_connection_index(expected);
	advance_connection_index(expected);
	inmatch::JoinerConnection dial("FreshConnection", [&now_ms] { return now_ms; });
	dial.set_title(&kJo);
	const std::vector<uint8_t> hello_datagram = dial.start();
	ClientHello hello;
	return expect(nw_decode_inbound(hello_datagram.data(), hello_datagram.size(), opcode, body) &&
	                      opcode == SESSION_OPCODE_CLIENT_HELLO &&
	                      parse_client_hello(body.data(), body.size(), hello) &&
	                      hello.ci == expected && dial.client_index() == expected &&
	                      dial.join_screen_stage() == inmatch::JoinScreenStage::Enumerating,
	              "one index: a join without a row enumerates (PRE_ENUMSESSION) with the "
	              "counter's second step");
}

// A flat-TLV body with every field named `unwanted` left out, the rest as written.
std::vector<uint8_t> without_field(const std::vector<uint8_t> &body, std::string_view unwanted) {
	std::vector<uint8_t> out;
	std::size_t pos = 0;
	while (pos < body.size()) {
		FlatTlvField field;
		const std::size_t next = read_flat_tlv(body.data(), body.size(), pos, field);
		if (next == kFlatTlvEnd) break;
		if (field.name != unwanted)
			out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(pos),
			           body.begin() + static_cast<std::ptrdiff_t>(next));
		pos = next;
	}
	return out;
}

// D-NET-452: the joiner reads any 0x81 with its CI; one whose PN, PG or PV1 is not the JO
// protocol's is dropped, and one without HK joins with a 0x42 that carries none. The LAN
// browser lists such a reply with host key 0.
// [orig: Nwu_HandleServerHello @0x626D20 - HK zeroed @0x626E97, the CI lookup @0x627533, PN
//  @0x62758B, PG @0x627591..0x627624, PV1 @0x627643; CNapiNPConnection_SendClientJoin
//  @0x61FE20 - the HK gate @0x6200F2]
bool run_server_hello_needs_no_hk() {
	uint64_t now_ms = 0;
	inmatch::JoinerConnection joiner("NoKeyJoiner", [&now_ms] { return now_ms; });
	joiner.set_title(&kJo);
	const std::vector<uint8_t> first = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!expect(nw_decode_inbound(first.data(), first.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_HELLO &&
				parse_client_hello(body.data(), body.size(), hello),
			"the joiner's 0x41 parses"))
		return false;
	ServerHello reply = build_server_hello(hello, 0x7F000001u, 32768);
	reply.sn = "NoKeyHost";
	ServerHello foreign = reply;
	foreign.pn = "NOVAWORLDUDP";
	const std::vector<uint8_t> foreign_datagram =
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(foreign));
	const inmatch::JoinerConnection::PollResult dropped =
			joiner.handle_datagram(foreign_datagram.data(), foreign_datagram.size());
	if (!expect(dropped.outbound.empty() && joiner.phase() == inmatch::JoinerConnection::Phase::Hello &&
				joiner.last_error().empty(),
			"a 0x81 with the joiner's CI but a foreign PN is dropped"))
		return false;
	const std::vector<uint8_t> hk_less = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, without_field(server_hello_to_bytes(reply), "HK"));
	const inmatch::JoinerConnection::PollResult joined =
			joiner.handle_datagram(hk_less.data(), hk_less.size());
	ClientAuth auth;
	FlatTlvField field;
	bool carries_hk = false;
	if (!expect(joined.outbound.size() == 1 &&
				joiner.phase() == inmatch::JoinerConnection::Phase::Auth &&
				nw_decode_inbound(joined.outbound[0].data(), joined.outbound[0].size(), opcode,
						body) &&
				opcode == SESSION_OPCODE_CLIENT_AUTH &&
				parse_client_auth(body.data(), body.size(), auth),
			"an HK-less 0x81 with the joiner's CI and the JO identity joins"))
		return false;
	for (std::size_t pos = 0; pos < body.size();) {
		const std::size_t next = read_flat_tlv(body.data(), body.size(), pos, field);
		if (next == kFlatTlvEnd) break;
		if (field.name == "HK") carries_hk = true;
		pos = next;
	}
	if (!expect(!carries_hk && auth.hk == 0, "the 0x42 from an HK-less 0x81 carries no HK"))
		return false;
	LanDiscoveryServer row;
	return expect(parse_lan_discovery_reply(kJo, hk_less.data(), hk_less.size(), hello.ci, row) &&
				row.host_key == 0 && row.server_name == "NoKeyHost",
			"the LAN browser lists an HK-less 0x81 with host key 0");
}

// The SCRK the manager LCG's mod-32 walk gives after a key whose last byte is `key`'s: x' = 17x
// + 1 (mod 32) for 63 draws, the NUL slot 31 appending nothing.
std::string scrk_after_key(uint32_t key) {
	static constexpr char kCharset[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";
	std::string out;
	uint32_t x = key & 31u;
	for (int i = 0; i < 63; ++i) {
		x = (17u * x + 1u) & 31u;
		if (x != 31u) out.push_back(kCharset[x]);
	}
	return out;
}

// A key whose bytes follow b' = 49b + 1 (mod 256), the LCG's byte draws.
bool key_bytes_chain(uint32_t key) {
	for (int shift = 24; shift > 0; shift -= 8) {
		const uint32_t b = (key >> shift) & 0xFFu;
		if (((49u * b + 1u) & 0xFFu) != ((key >> (shift - 8)) & 0xFFu)) return false;
	}
	return true;
}

// D-NET-524: the joiner draws its CK and then its SCRK from the manager's LCG. Seeded so the CK
// is RR's captured 0x4667b839, the 0x42 carries that CK and the 61-character SCRK the LCG walks
// next; the default (the process's manager) gives a CK whose bytes chain and an SCRK that
// follows it. (D-NET-438's redraw of a zero key stays ported; the LCG never yields one.)
// [orig: CNapiNPConnection_InitFromSession @0x6263f4 -> NapiNP_GenerateSessionKey @0x61ea70,
//  then CNapiNPConnection_GenerateTxKey @0x61dfe0 @0x626401]
bool run_client_keys_draw_from_the_manager_lcg() {
	uint64_t now_ms = 0;
	NapiPrng prng(0x55u);
	inmatch::JoinerConnection joiner("LcgJoiner", [&now_ms] { return now_ms; });
	joiner.set_title(&kJo);
	joiner.set_key_prng(&prng);
	inmatch::JoinerConnection::DiscoveredSession session;
	session.present = true;
	session.host_key = 0x0FE0E115u;
	joiner.set_discovered_session(session);
	const std::vector<uint8_t> first = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	if (!expect(nw_decode_inbound(first.data(), first.size(), opcode, body) &&
	                    opcode == SESSION_OPCODE_CLIENT_AUTH &&
	                    parse_client_auth(body.data(), body.size(), auth),
	            "lcg: the joiner's first datagram is its 0x42"))
		return false;
	if (!expect(auth.ck == 0x4667B839u && joiner.client_key() == 0x4667B839u &&
	                    auth.scrk == "BCXYGH01LM45QR89VWDFZJK23NP67STBCXYGH01LM45QR89VWDFZJK23NP67S" &&
	                    joiner.client_scrk() == auth.scrk && prng.draws() == 67u,
	            "lcg: the seeded CK then SCRK ride the 0x42"))
		return false;

	inmatch::JoinerConnection fresh("ManagerJoiner", [&now_ms] { return now_ms; });
	fresh.set_title(&kJo);
	fresh.set_discovered_session(session);
	const std::vector<uint8_t> second = fresh.start();
	ClientAuth fresh_auth;
	return expect(nw_decode_inbound(second.data(), second.size(), opcode, body) &&
	                      parse_client_auth(body.data(), body.size(), fresh_auth) &&
	                      fresh_auth.ck != 0 && key_bytes_chain(fresh_auth.ck) &&
	                      fresh_auth.scrk == scrk_after_key(fresh_auth.ck),
	              "lcg: the manager's CK chains its bytes and its SCRK follows it");
}

// D-NET-437: the joiner reads any 0x82 from its host, no field required. One whose CI or CK is
// not its connection's is dropped and the connection stays in the connect state; then CR's truth
// splits it: any nonzero CR accepts (an SCRK-less one too), and zero rejects with failure code 1
// when the 0x82 carries no JFC.
// [orig: NapiNP_HandleServerJoinResponse @0x629840 - JFC = 1 @0x629977, the CI / CK match
//  @0x629CAC..0x629CBF, the CR split @0x629CD2..0x629CD6, the reject's JFC store @0x629CF0]
bool run_server_auth_acceptance_follows_retail() {
	uint64_t now_ms = 9000;
	const auto connect = [](inmatch::JoinerConnection &joiner, ClientAuth &auth) {
		inmatch::JoinerConnection::DiscoveredSession session;
		session.present = true;
		session.host_key = 0x0FE0E115u;
		joiner.set_discovered_session(session);
		const std::vector<uint8_t> first = joiner.start();
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		return nw_decode_inbound(first.data(), first.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_AUTH &&
				parse_client_auth(body.data(), body.size(), auth) &&
				joiner.phase() == inmatch::JoinerConnection::Phase::Auth;
	};
	// A 0x82 of dword fields only: no SCRK, and no JFC, JFP or JFS.
	const auto server_auth = [](std::vector<std::pair<const char *, uint32_t>> dwords) {
		std::vector<uint8_t> body;
		for (const auto &field : dwords) {
			uint8_t le[4];
			io::write_u32_le(le, field.second);
			append_flat_tlv(body, field.first, le, 4);
		}
		return nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, std::move(body));
	};

	inmatch::JoinerConnection rejected("AuthRejected", [&now_ms] { return now_ms; });
	rejected.set_title(&kJo);
	ClientAuth auth;
	if (!expect(connect(rejected, auth), "the joiner is connecting with its 0x42")) return false;
	const std::vector<uint8_t> foreign_ci =
			server_auth({{"CI", auth.ci ^ 0x10u}, {"CK", auth.ck}, {"CR", 1u}});
	const std::vector<uint8_t> foreign_ck =
			server_auth({{"CI", auth.ci}, {"CK", auth.ck ^ 0x01u}, {"CR", 0u}});
	rejected.handle_datagram(foreign_ci.data(), foreign_ci.size());
	rejected.handle_datagram(foreign_ck.data(), foreign_ck.size());
	if (!expect(rejected.phase() == inmatch::JoinerConnection::Phase::Auth &&
				rejected.last_error().empty() && !rejected.last_join_reject().set,
			"an SCRK-less 0x82 with a foreign CI or CK is dropped; the join stays connecting"))
		return false;
	const std::vector<uint8_t> bare_reject =
			server_auth({{"CI", auth.ci}, {"CK", auth.ck}, {"CR", 0u}});
	rejected.handle_datagram(bare_reject.data(), bare_reject.size());
	if (!expect(rejected.phase() == inmatch::JoinerConnection::Phase::Error &&
				rejected.last_join_reject().set && rejected.last_join_reject().jfc == 1u &&
				rejected.last_join_reject().jfp == 0u,
			"a CR 0 0x82 without JFC rejects the join with failure code 1"))
		return false;

	inmatch::JoinerConnection accepted("AuthAccepted", [&now_ms] { return now_ms; });
	accepted.set_title(&kJo);
	if (!expect(connect(accepted, auth), "the second joiner is connecting with its 0x42"))
		return false;
	// An accept whose CU walk overruns its bounds (a 65th CU, or one of 2048 bytes) is dropped
	// before the CI / CK match: the socket pass returns without queuing it.
	// [orig: NapiNP_HandleServerJoinResponse @0x629840 - @0x629B97..0x629BA6, @0x629F0B..0x629F12]
	const auto overrun_accept = [&](size_t cus, size_t cu_bytes) {
		std::vector<uint8_t> body;
		for (const auto &field : std::vector<std::pair<const char *, uint32_t>>{
					 {"CI", auth.ci}, {"CK", auth.ck}, {"CR", 1u}, {"SK", 0x12345678u}}) {
			uint8_t le[4];
			io::write_u32_le(le, field.second);
			append_flat_tlv(body, field.first, le, 4);
		}
		const std::vector<uint8_t> cu(cu_bytes, 0x01);
		for (size_t i = 0; i < cus; ++i)
			append_flat_tlv(body, "CU", cu.data(), static_cast<uint16_t>(cu.size()));
		return nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, std::move(body));
	};
	const std::vector<uint8_t> sixty_five = overrun_accept(65, 1);
	const std::vector<uint8_t> long_cu = overrun_accept(1, 2048);
	accepted.handle_datagram(sixty_five.data(), sixty_five.size());
	accepted.handle_datagram(long_cu.data(), long_cu.size());
	if (!expect(accepted.phase() == inmatch::JoinerConnection::Phase::Auth &&
				accepted.last_error().empty() && !accepted.last_join_reject().set,
			"an accept with a 65th CU or a 2048-byte CU is dropped; the join stays connecting"))
		return false;
	const std::vector<uint8_t> accept = server_auth(
			{{"CI", auth.ci}, {"CK", auth.ck}, {"CR", 2u}, {"SK", 0x12345678u}});
	accepted.handle_datagram(accept.data(), accept.size());
	if (!expect(accepted.phase() == inmatch::JoinerConnection::Phase::Driving &&
				!accepted.last_join_reject().set,
			"a CR 2 0x82 without SCRK accepts the join"))
		return false;
	// The accepted session reloads into the host's next mission whatever its key: retail's
	// reload legs test the session alone [orig: Game_StartMission @0x524360 -
	// @0x5243E2..0x524400, @0x524532..0x524538, @0x5247F9..0x5247FF].
	return expect(!accepted.begin_mission_reload().empty() && accepted.reloading(),
			"the SCRK-less session's mission reload sends its first legs");
}

// D-NET-232: the joiner runs on its cs_dir0 block, overlaid by the host's 0x82 CS TLVs and by
// later H:0x00 CS updates: field 13 is its packet ceiling, field 5 the active-send interval,
// field 4 the empty one. A host whose mpmaxpacketsize is 576 advertises 576 in the 0x82; a later
// update (a retail host's 0x2000 negotiation, plus fields 4/5 here) moves them again.
// [orig: NapiNP_HandleServerJoinResponse @0x629b4c..0x629b75 / @0x629d72; CNapiNPConnection_
//  HandleCSConfigUpdate @0x6219c8; BuildOutgoingPackets @0x628436; PumpSendIntervals
//  @0x628ff1 (active) / @0x629041 (empty); NapiNPServer_HandleNewConnection @0x4c81bd]
bool run_cs_block_drives_the_joiner_ceiling_and_intervals() {
	const PeerAddr peer{0x0100007Fu, 32771};
	inmatch::NapiNPServerCtx host;
	inmatch::GameConfig config;
	config.max_packet_size = 576;
	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, 0x0FE0E114u, nullptr, config);
	uint64_t now_ms = 0;
	inmatch::JoinerConnection joiner("CsJoiner", [&now_ms] { return now_ms; });
	joiner.set_title(&kJo);
	const std::vector<uint8_t> hello = joiner.start();
	const inmatch::HandleResult hello_reply = inmatch::handle_server_datagram(
			host, peer, hello.data(), hello.size(), 0);
	if (!expect(hello_reply.outbound.size() == 1, "host answers the ClientHello")) return false;
	const inmatch::JoinerConnection::PollResult auth = joiner.handle_datagram(
			hello_reply.outbound[0].data(), hello_reply.outbound[0].size());
	if (!expect(auth.outbound.size() == 1 && is_opcode(auth.outbound[0], SESSION_OPCODE_CLIENT_AUTH),
			"ServerHello sends ClientAuth")) {
		return false;
	}
	const inmatch::HandleResult accept = inmatch::handle_server_datagram(
			host, peer, auth.outbound[0].data(), auth.outbound[0].size(), 1);
	if (!expect(!accept.outbound.empty() &&
				is_opcode(accept.outbound[0], SESSION_OPCODE_SERVER_AUTH),
			"host accepts the join")) {
		return false;
	}
	joiner.handle_datagram(accept.outbound[0].data(), accept.outbound[0].size());
	if (!expect(joiner.session_timeouts().max_packet_bytes == 576 &&
				joiner.packet_ceiling_bytes() == 576,
			"the 0x82's CS field 13 becomes the joiner's packet ceiling")) {
		return false;
	}

	// A cs_dir0 update (direction byte nonzero): field 4 = 5000, field 5 = 3000, field 13 = 400.
	std::vector<uint8_t> update{0x01, 0x30, 0x20, 0x00, 0x00};
	for (uint32_t value : {5000u, 3000u, 400u}) {
		for (int shift = 0; shift < 32; shift += 8)
			update.push_back(static_cast<uint8_t>(value >> shift));
	}
	inmatch::NapiNPConnection *node = nullptr;
	for (inmatch::NapiNPConnection &c : host.np_protocol.connection_list)
		if (c.peer == peer) node = &c;
	if (!expect(node != nullptr, "host holds the joiner's node")) return false;
	// Skip the host's retained settings packet: the update rides the next sequence, so the
	// joiner queues it; deliver the settings first, then the update, in order.
	if (!expect(accept.outbound.size() == 2, "the 0x82 is followed by the settings packet"))
		return false;
	joiner.handle_datagram(accept.outbound[1].data(), accept.outbound[1].size());
	std::vector<uint8_t> update_packet;
	if (!expect(inmatch::frame_in_match_s2c_batch(host, peer,
				{make_protocol_message(0x00, update,
						PROTOCOL_MSG_FLAG_SETTINGS_UPDATE | PROTOCOL_MSG_FLAG_LEN8)},
				update_packet),
			"host frames the CS update")) {
		return false;
	}
	joiner.handle_datagram(update_packet.data(), update_packet.size());
	const CsConfig &cs = joiner.session_timeouts();
	if (!expect(cs.idle_send_interval_ms == 5000 && cs.active_send_interval_ms == 3000 &&
				cs.max_packet_bytes == 400,
			"a cs_dir0 update stores fields 4, 5 and 13")) {
		return false;
	}

	// Field 13: three 300-byte records no longer share one packet.
	std::vector<ProtocolMessage> records;
	for (uint8_t i = 0; i < 3; ++i)
		records.push_back(make_protocol_message(0x34, std::vector<uint8_t>(300, i)));
	const std::vector<std::vector<uint8_t>> framed = joiner.frame_messages(records);
	bool within = !framed.empty();
	for (const std::vector<uint8_t> &datagram : framed) within = within && datagram.size() <= 400;
	if (!expect(framed.size() == 3 && within,
			"C2S packets obey the dictated 400-byte ceiling")) {
		return false;
	}

	// Field 5: with those records retained, the active probe waits MORE than 3000 ms.
	joiner.pump(0); // flushes any owed ACK
	now_ms = 3000;
	if (!expect(joiner.pump(0).empty(), "no active probe at exactly 3000 ms")) return false;
	now_ms = 3001;
	const std::vector<std::vector<uint8_t>> probe = joiner.pump(0);
	return expect(probe.size() == 1 && is_opcode(probe[0], SESSION_OPCODE_PROTOCOL_MESSAGE),
			"the active probe follows the dictated 3000 ms interval");
}

bool run_client_active_probe_recovers_join() {
	// The JOINTOPERATIONS template's active_send_interval_ms [orig: CNapiNetwork_Init @0x4ca4a0].
	constexpr uint64_t kRetailActiveSendIntervalMs = 10000;
	constexpr uint32_t kServerKey = 0x11223344u;
	const std::string server_scrk = "SERVER-ACTIVE-PROBE-SCRK";
	uint64_t now_ms = 0;
	inmatch::JoinerConnection joiner("ClientProbe", [&now_ms] { return now_ms; });
	joiner.set_title(&kJo);

	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	const std::vector<uint8_t> hello_datagram = joiner.start();
	if (!expect(nw_decode_inbound(
				hello_datagram.data(), hello_datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_HELLO &&
				parse_client_hello(body.data(), body.size(), hello),
			"decode active-probe ClientHello")) {
		return false;
	}
	ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x55667788u;
	const std::vector<uint8_t> server_hello_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	const inmatch::JoinerConnection::PollResult hello_result = joiner.handle_datagram(
			server_hello_datagram.data(), server_hello_datagram.size());
	if (!expect(hello_result.outbound.size() == 1,
			"active-probe ServerHello emits ClientAuth")) {
		return false;
	}
	const std::vector<uint8_t> client_auth_datagram = hello_result.outbound[0];
	ClientAuth client_auth;
	if (!expect(nw_decode_inbound(
				client_auth_datagram.data(), client_auth_datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_AUTH &&
				parse_client_auth(body.data(), body.size(), client_auth),
			"decode active-probe ClientAuth")) {
		return false;
	}

	ServerAuth server_auth = build_server_auth(
			client_auth, 0x7F000001u, 32769, kServerKey, server_scrk,
			"", "", "", false);
	server_auth.mi = 3;
	// A GAME host's 0x82 carries the JOINTOPERATIONS block (build_server_auth defaults to the
	// NOVAWORLDUDP service's), and the joiner now runs its send intervals on it.
	// [orig: CNapiNPConnection_SendSessionInit @0x620ef0; CNapiNetwork_Init @0x4ca4a0]
	server_auth.client_cs = jointoperations_cs_fields();
	server_auth.server_cs = jointoperations_cs_fields();
	const std::vector<uint8_t> server_auth_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	const inmatch::JoinerConnection::PollResult auth_result = joiner.handle_datagram(
			server_auth_datagram.data(), server_auth_datagram.size());
	if (!expect(auth_result.outbound.empty(),
			"ServerAuth alone has no retained session record to probe")) {
		return false;
	}
	now_ms = kRetailActiveSendIntervalMs + 1;
	if (!expect(joiner.pump(0).empty(),
			"missing settings are recovered by the server-side active probe")) {
		return false;
	}

	SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> settings_body;
	if (!expect(frame_session_packet(
				server_seq, SessionCrypto{server_scrk, {}, client_auth.ck}, {
					make_protocol_message(
							0x00,
							{0x00, 0x00, 0x20, 0x00, 0x00,
							 0x14, 0x05, 0x00, 0x00},
							0xA0),
					make_protocol_message(
							0x00,
							{0x01, 0x00, 0x20, 0x00, 0x00,
							 0x14, 0x05, 0x00, 0x00},
							0xA0),
				}, settings_body),
			"frame active-probe settings")) {
		return false;
	}
	const std::vector<uint8_t> settings_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(settings_body));
	const inmatch::JoinerConnection::PollResult settings_result =
			joiner.handle_datagram(settings_datagram.data(), settings_datagram.size());
	if (!expect(settings_result.outbound.size() == 2,
			"settings emit ACK plus retained JOIN")) {
		return false;
	}
	const std::vector<uint8_t> join_datagram = settings_result.outbound[1];
	ProtocolPacketHeader join_header;
	std::vector<ProtocolMessage> join_messages;
	const std::vector<uint8_t> base_game_join = {
			'V', 'E', 'R', 'S', 'I', 'O', 'N', 'C',
			'R', 'C', 'S', 'T', 'R', 'I', 'N', 'G',
			0x00, 0x02, 0x00, 0x30, 0x00,
	};
	if (!expect(decode_session(
				join_datagram, SESSION_OPCODE_PROTOCOL_MESSAGE,
				client_auth.scrk, join_header, join_messages) &&
				join_messages.size() == 1 &&
				join_messages[0].tag == 0x00 &&
				join_messages[0].payload == base_game_join,
			"base-game ServerHello emits the 21-byte JOIN without an EXP TLV")) {
		return false;
	}

	now_ms += kRetailActiveSendIntervalMs;
	if (!expect(joiner.pump(0).empty(),
			"retained JOIN does not probe at exactly the active interval")) {
		return false;
	}
	++now_ms;
	const std::vector<std::vector<uint8_t>> active_probe = joiner.pump(0);
	ProtocolPacketHeader probe_header;
	std::vector<ProtocolMessage> probe_messages;
	if (!expect(active_probe.size() == 1 &&
				active_probe[0].size() == 18 &&
				decode_session(
						active_probe[0], SESSION_OPCODE_PROTOCOL_MESSAGE,
						client_auth.scrk, probe_header, probe_messages) &&
				probe_messages.empty() &&
				probe_header.session_id == kServerKey &&
				probe_header.seq_num == 3 &&
				probe_header.ack_count == 1,
			"retained JOIN timeout emits a fresh header-only sequence 3")) {
		return false;
	}

	std::vector<uint8_t> resend_body;
	if (!expect(encode_session_resend_list(
				client_auth.ck, {2}, resend_body),
			"frame server JOIN resend request")) {
		return false;
	}
	const std::vector<uint8_t> resend_datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_RESEND_LIST, std::move(resend_body));
	const inmatch::JoinerConnection::PollResult recovered = joiner.handle_datagram(
			resend_datagram.data(), resend_datagram.size());
	// The handler transmits the rebuilt packet itself, inside the receive pump
	// [orig: NapiNP_HandleResendList -> CNapiNPConnection_SendSessionPacket @0x6239b6].
	return expect(recovered.immediate_outbound.size() == 1 &&
				recovered.outbound.empty() &&
				recovered.immediate_outbound[0] == join_datagram,
			"server 0x84 reconstructs the retained JOIN at sequence 2");
}

// A scripted datagram socket for the host owner loop: datagrams queued in `inbound` drain through
// recv_from in order, and every send_to lands in `sent`.
class ScriptedDatagramSocket final : public IDatagramSocket {
public:
	std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> inbound;
	std::vector<std::vector<uint8_t>> sent;

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty()) return 0;
		const std::vector<uint8_t> datagram = std::move(inbound.front().second);
		from = inbound.front().first;
		inbound.pop_front();
		if (datagram.size() > cap) return -1;
		std::memcpy(buf, datagram.data(), datagram.size());
		return static_cast<int>(datagram.size());
	}
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(data, data + len);
	}
};

// The host's retained settings packet is lost: its send pump's ACTIVE leg (every server tick,
// D-NET-256) mints a header-only sequence once MORE than 10000 ms passed since that build, the
// joiner sees the gap and asks with a 0x44, and the host's receive pump rebuilds sequence 1.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628ff1..0x629017 -> BuildOutgoingPackets
//  @0x62907b; NapiNP_HandleResendList -> SendSessionPacket @0x6239b6]
bool run_server_active_probe_recovers_settings() {
	const PeerAddr peer{0x0100007Fu, 32769};
	inmatch::HostOwner owner;
	inmatch::test::bring_up_host(owner.ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, 0x0FE0E112u);
	ScriptedDatagramSocket sock;
	inmatch::ClientRuntime client("SettingsProbe");
	client.set_title(&kJo);

	sock.inbound.emplace_back(peer, client.start());
	inmatch::host_session_pump(owner, sock); // tick 0
	if (!expect(sock.sent.size() == 1, "settings recovery host emits ServerHello"))
		return false;
	client.receive(sock.sent[0].data(), sock.sent[0].size());
	sock.sent.clear();
	const std::vector<std::vector<uint8_t>> auth = client.Client_ProcessNetworkFrame(0);
	if (!expect(auth.size() == 1, "settings recovery client emits ClientAuth"))
		return false;
	sock.inbound.emplace_back(peer, auth[0]);
	inmatch::host_session_pump(owner, sock); // tick 1: the 0x82, then the settings build
	if (!expect(sock.sent.size() == 2 &&
				is_opcode(sock.sent[0], SESSION_OPCODE_SERVER_AUTH) &&
				is_opcode(sock.sent[1], SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE),
			"host emits ServerAuth followed by retained settings")) {
		return false;
	}
	const std::vector<uint8_t> settings_datagram = sock.sent[1];
	client.receive(sock.sent[0].data(), sock.sent[0].size());
	sock.sent.clear();
	if (!expect(client.Client_ProcessNetworkFrame(0).empty(),
			"dropped settings leave the client waiting without ClientAuth retry")) {
		return false;
	}

	// 625 ticks of 16 ms after the build is exactly 10000 ms: not yet MORE.
	for (int i = 0; i < 625; ++i) inmatch::host_session_pump(owner, sock);
	if (!expect(sock.sent.empty(), "server does not active-probe at exactly 10000 ms")) {
		return false;
	}
	inmatch::host_session_pump(owner, sock);
	if (!expect(sock.sent.size() == 1,
			"retained settings make the server active-probe after 10000 ms")) {
		return false;
	}
	const inmatch::NapiNPConnection &connection =
			owner.ctx.np_protocol.connection_list.front();
	ProtocolPacketHeader probe_header;
	std::vector<ProtocolMessage> probe_messages;
	if (!expect(decode_session(
				sock.sent[0],
				SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
				connection.server_scrk, probe_header, probe_messages) &&
				probe_messages.empty() &&
				probe_header.seq_num == 2 &&
				probe_header.ack_count == 0,
			"server settings timeout emits fresh header-only sequence 2")) {
		return false;
	}

	client.receive(sock.sent[0].data(), sock.sent[0].size());
	sock.sent.clear();
	const std::vector<std::vector<uint8_t>> missing =
			client.Client_ProcessNetworkFrame(0);
	if (!expect(missing.size() == 1 &&
				is_resend_request(
						missing[0], SESSION_OPCODE_CLIENT_RESEND_LIST,
						connection.server_sk, 1),
			"client requests missing server settings sequence 1")) {
		return false;
	}
	sock.inbound.emplace_back(peer, missing[0]);
	inmatch::host_session_pump(owner, sock);
	if (!expect(sock.sent.size() == 1 && sock.sent[0] == settings_datagram,
			"client 0x44 reconstructs the retained settings at sequence 1")) {
		return false;
	}

	client.receive(sock.sent[0].data(), sock.sent[0].size());
	const std::vector<std::vector<uint8_t>> post_settings =
			client.Client_ProcessNetworkFrame(0);
	return expect(post_settings.size() == 2 &&
				client.phase() == inmatch::JoinerConnection::Phase::Driving,
			"recovered settings resume the retail ACK plus JOIN sequence");
}

// The EMPTY send interval keeps a QUIET session alive: with nothing queued and nothing
// retained, the pump mints a header-only packet every 30 s so the peer's 120 s connection
// timeout never fires. Without this leg a joiner parked at the deploy pick (or dead, with
// the uplink gate shut) transmits nothing and a stock host reaps it after ~2 minutes.
// [orig: CNapiNPConnection_PumpSendIntervals @0x628fd0 empty leg @0x629041..0x629067;
//  cs_dir0.idle_send_interval_ms = 30000 / timeout_ms = 120000 @0x4ca4a0]
bool run_idle_keepalive_survives_a_quiet_session() {
	constexpr uint64_t kIdleIntervalMs = 30000;
	constexpr uint32_t kServerKey = 0x5A5A1234u;
	const std::string client_scrk = "CLIENT-IDLE-KEEPALIVE-SCRK";
	const std::string server_scrk = "SERVER-IDLE-KEEPALIVE-SCRK";
	// A non-zero start so the seeded anchor is distinguishable from "never sent".
	uint64_t now_ms = 1000;
	inmatch::JoinerConnection joiner("IdleKeepalive", [&now_ms] { return now_ms; });
	joiner.set_title(&kJo);
	// Seed a live in-match session with NOTHING retained and nothing queued — the exact
	// state a joiner parked at the deploy pick (or dead, uplink gate shut) sits in, and
	// the state the ACTIVE interval deliberately ignores.
	joiner.seed_in_match(kServerKey, 1u, client_scrk, server_scrk,
	                     1, 0, 0x0001, 0x14B9);
	if (!expect(joiner.pump(0).empty(), "idle: a fresh seed does not mint immediately")) {
		return false;
	}

	now_ms = 1000 + kIdleIntervalMs;
	if (!expect(joiner.pump(0).empty(),
			"idle: no keepalive at exactly the empty interval")) {
		return false;
	}
	now_ms = 1000 + kIdleIntervalMs + 1;
	const std::vector<std::vector<uint8_t>> keepalive = joiner.pump(0);
	ProtocolPacketHeader header;
	std::vector<ProtocolMessage> messages;
	if (!expect(keepalive.size() == 1 &&
				decode_session(keepalive[0], SESSION_OPCODE_PROTOCOL_MESSAGE,
						client_scrk, header, messages) &&
				messages.empty() && header.session_id == kServerKey,
			"idle: a quiet session mints a header-only keepalive past the interval")) {
		return false;
	}
	// The mint re-anchors the clock: no second keepalive until another full interval.
	if (!expect(joiner.pump(0).empty(), "idle: the keepalive re-anchors the clock")) {
		return false;
	}
	now_ms += kIdleIntervalMs + 1;
	if (!expect(joiner.pump(0).size() == 1,
			"idle: the keepalive repeats every interval while the session stays quiet")) {
		return false;
	}
	// The whole point: a stock peer reaps at the production timeout, and the cadence pinned above
	// (the mint lands at kIdleIntervalMs + 1) keeps the gap between transmissions well inside it.
	return expect(2 * kIdleIntervalMs < inmatch::JO_GAME_SESSION_TIMEOUT_MS,
			"idle: two keepalive intervals still fit inside the retail connection timeout");
}

// A rejecting 0x82's JFS lands in the connection's 64-byte failure string, so the join-reject
// record (and the reason text built over it) keeps 63 characters of a 600-character JFS.
// [orig: NapiNP_HandleServerJoinResponse @0x629840 - JFS into 512 @0x629B29, then
//  CNapiSessionConfig_SetServerPassword @0x4CACE0 (Napi_CopyString(this + 0x5F8, jfs, 64)
//  @0x4CACEE), the call @0x629CF6]
bool run_join_reject_text_keeps_sixty_three() {
	uint64_t now_ms = 11000;
	inmatch::JoinerConnection joiner("RejectText", [&now_ms] { return now_ms; });
	joiner.set_title(&kJo);
	inmatch::JoinerConnection::DiscoveredSession session;
	session.present = true;
	session.host_key = 0x0FE0E116u;
	joiner.set_discovered_session(session);
	const std::vector<uint8_t> first = joiner.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	if (!expect(nw_decode_inbound(first.data(), first.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_AUTH &&
				parse_client_auth(body.data(), body.size(), auth),
			"reject text: the joiner is connecting with its 0x42"))
		return false;
	ServerAuth reject;
	reject.ci = auth.ci;
	reject.ck = auth.ck;
	reject.cr = 0;
	reject.jfc = 4;
	reject.jfs = std::string(600, 'j');
	const std::vector<uint8_t> datagram = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_rejection_to_bytes(reject));
	joiner.handle_datagram(datagram.data(), datagram.size());
	return expect(joiner.phase() == inmatch::JoinerConnection::Phase::Error &&
				joiner.last_join_reject().set && joiner.last_join_reject().jfc == 4u &&
				joiner.last_join_reject().jfs == std::string(63, 'j') &&
				joiner.connection_error_record().connect_text == std::string(63, 'j'),
			"reject text: a 600-character JFS keeps 63 in the join-reject record");
}

} // namespace

int main() {
	if (!run_idle_keepalive_survives_a_quiet_session()) return 1;
	if (!run_idle_keepalive_waits_while_a_packet_is_held()) return 1;
	if (!run_dropped_hello_and_auth_recover()) return 1;
	if (!run_unanswered_client_join_fails_after_30000_ms()) return 1;
	if (!run_discovered_row_join_skips_the_hello()) return 1;
	if (!run_connect_writes_pv3_and_its_peer()) return 1;
	if (!run_one_connection_index_per_join()) return 1;
	if (!run_client_keys_draw_from_the_manager_lcg()) return 1;
	if (!run_server_hello_needs_no_hk()) return 1;
	if (!run_server_auth_acceptance_follows_retail()) return 1;
	if (!run_cs_block_drives_the_joiner_ceiling_and_intervals()) return 1;
	if (!run_client_active_probe_recovers_join()) return 1;
	if (!run_server_active_probe_recovers_settings()) return 1;
	if (!run_join_reject_text_keeps_sixty_three()) return 1;
	std::printf("OK\n");
	return 0;
}
