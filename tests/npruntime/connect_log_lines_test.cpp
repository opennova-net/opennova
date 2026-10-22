// The _connectlog.txt packet and session lines (D-NET-NEW-L8-8's remaining writers): the
// line texts, and a host's manager-receive lines while the log is on.
//
// Retail's NAPI manager logs every datagram its socket hands over (RAW), the unpacked
// packet (COOKED), the magic packet (BAFFLE), the dispatch (PROTOCOL with the protocol's
// name when an opcode handler took the packet, else TOSSED [4]) and an unpack failure
// (TOSSED [3]); PunkBuster's datagrams go to their own handler unpacked. A server's first
// new connection turns the log off for the process, a listen host's own one included, so a
// host's lines cover the probes and the first join before it. The protocol's NET lines are
// the client side's (the server side's slots write no line: +0x2DC / +0x2E0 / +0x2E4 /
// +0x2EC are empty stubs, +0x2F0 sets only the net-quality error bit).
// [orig: NapiNPManager_PumpReceive @0x623010 (cb_on_raw @0x623121, the PunkBuster arm
//  @0x6231A7..0x6231D9, NapiNP_UnpackPacket @0x623206, cb_on_error(3) @0x623229, cb_on_packet
//  @0x623254); NapiNPManager_HandlePacket @0x622F10 (the magic test @0x622F1B..0x622F36,
//  cb_on_dispatch @0x622FCD, cb_on_error(4) @0x622FF9); NapiNPProtocol_HandleClientHello
//  @0x6213B0 (`return protocol_version == 0` @0x62190B); NapiNPProtocol_HandleClientJoin
//  @0x62B750; NapiNPServer_HandleNewConnection @0x4C8040 (@0x4C8054..0x4C80AF);
//  CNapiNetwork_Init @0x4CA908..0x4CA9CD; Napi_FormatAddress @0x62DED0]
#include <net/napi/envelope.h>
#include <net/npwire/lan_discovery.h> // net_session_removed_text / net_session_discovered_text
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/punt_log.h>
#include <runtime/inmatch/server_files.h>
#include <runtime/inmatch/server_session.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "host_test_setup.h"

using namespace opennova;
namespace ns = opennova::replication;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

// The log's lines after its header, each without the "T : " stamp and the line ending.
std::vector<std::string> log_lines(MemoryFiles &files) {
	std::vector<std::string> out;
	const std::string &text = files.files[inmatch::kConnectLogFile];
	size_t at = 0;
	while (at < text.size()) {
		const size_t end = text.find("\r\n", at);
		const std::string line = text.substr(at, end - at);
		out.push_back(line.size() >= 4 ? line.substr(4) : line);
		if (end == std::string::npos) break;
		at = end + 2;
	}
	// Drop the header the start writes (STARTED, EXE, VERSION, TITLE, VER1, VER2).
	if (out.size() >= 6) out.erase(out.begin(), out.begin() + 6);
	return out;
}

void test_line_texts() {
	const PeerAddr peer = peer_addr_from_octets({192, 168, 1, 20}, 3000);
	CHECK(inmatch::napi_got_raw_text(512, peer) == "NAPI GOT RAW 512 BYTES FROM 192.168.1.20:3000");
	CHECK(inmatch::napi_got_cooked_text(508, peer) ==
			"NAPI GOT COOKED 508 BYTES FROM 192.168.1.20:3000");
	CHECK(inmatch::napi_got_baffle_text(48, peer) == "NAPI GOT BAFFLE 48 BYTES FROM 192.168.1.20:3000");
	CHECK(inmatch::napi_got_protocol_text(508, peer, "jointops") ==
			"NAPI GOT PROTOCOL 508 BYTES FROM 192.168.1.20:3000 [jointops]");
	CHECK(inmatch::napi_tossed_text(9, peer, inmatch::NapiTossReason::Unpack) ==
			"NAPI TOSSED 9 BYTES FROM 192.168.1.20:3000 [3]");
	CHECK(inmatch::net_got_text(40, peer, 0x83) == "NET GOT 40 BYTES FROM 192.168.1.20:3000 [0x83]");
	CHECK(inmatch::net_got_text(40, peer, 0x8) == "NET GOT 40 BYTES FROM 192.168.1.20:3000 [0x08]");
	CHECK(inmatch::net_sending_text(60, peer, 0x43, 7) ==
			"NET SENDING 60 BYTES TO 192.168.1.20:3000 [0x43, p#7]");
	CHECK(inmatch::net_processing_text(32, peer) == "NET PROCESSING 32 BYTES FROM 192.168.1.20:3000");
	CHECK(inmatch::net_unknown_message_text(200, 12) == "NET UNKNOWN MESSAGE!! [200, 12 bytes]");
	CHECK(inmatch::net_incoming_packet_error_text() == "NET INCOMING PACKET ERROR!!");
	CHECK(inmatch::net_outgoing_packet_error_text() == "NET OUTGOING PACKET ERROR!!");
	CHECK(inmatch::net_tearing_down_socket_text() == "NET TEARING DOWN SOCKET");
	CHECK(inmatch::net_connected_to_server_text(peer, peer_addr_from_octets({10, 0, 0, 2}, 2302)) ==
			"NET CONNECTED TO SERVER 192.168.1.20:3000 [yourip=10.0.0.2:2302]");
	CHECK(inmatch::net_disconnecting_from_server_text(peer, 1, 9, 0, 0, "", 0, "NP.C:SH:STOP") ==
			"NET DISCONNECTING FROM SERVER 192.168.1.20:3000 [1,9,0,0,\"\",0,\"NP.C:SH:STOP\"]");
	CHECK(net_session_removed_text(peer, "Host") ==
			"NET SESSION AT 192.168.1.20:3000 [Host] HAS BEEN REMOVED.");
	CHECK(net_session_discovered_text(peer, "Host") ==
			"NET SESSION DISCOVERED AT 192.168.1.20:3000 [Host]");
}

struct LoggedHost {
	MemoryFiles files;
	inmatch::ConnectLog log{files};
	inmatch::NapiNPServerCtx ctx;

	explicit LoggedHost(inmatch::ConnectionMode mode, ns::ISessionTransport *own = nullptr) {
		log.set_clock([] { return std::string("T"); });
		log.start(inmatch::ConnectLogHeader{});
		ctx.logs.connect = &log;
		inmatch::test::bring_up_host(ctx, mode, inmatch::SocketMode::Lan, /*host_key=*/0x1234, own);
	}
};

int32_t unpacked_len(const std::vector<uint8_t> &dg) {
	std::vector<uint8_t> unpacked;
	return nw_unpack_inbound(dg.data(), dg.size(), unpacked) ? static_cast<int32_t>(unpacked.size())
	                                                          : -1;
}

// A dedicated host before its first connection.
void test_dedicated_host_receive_lines() {
	LoggedHost host(inmatch::ConnectionMode::HostOnly);
	const PeerAddr peer = peer_addr_from_octets({10, 0, 0, 7}, 4100);
	const std::string from = "10.0.0.7:4100";
	const std::string pn = kJo.pn;

	// A probe: answered and taken.
	ClientHello hello = make_title_client_hello(kJo, 1);
	const std::vector<uint8_t> probe =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	inmatch::HandleResult r = inmatch::handle_server_datagram(host.ctx, peer, probe.data(), probe.size());
	CHECK(r.outbound.size() == 1 && r.taken);
	// A peer host's announce (a nonzero PM): answered, not taken.
	hello.pm = 3;
	const std::vector<uint8_t> announce =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	r = inmatch::handle_server_datagram(host.ctx, peer, announce.data(), announce.size());
	CHECK(r.outbound.size() == 1 && !r.taken);
	// A session packet with no connection: nobody takes it.
	const std::vector<uint8_t> stray = nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, {1, 2, 3, 4});
	(void)inmatch::handle_server_datagram(host.ctx, peer, stray.data(), stray.size());
	// A datagram whose CRC fails: tossed at the unpack, its socket length.
	std::vector<uint8_t> broken = probe;
	broken[broken.size() - 1] ^= 0x55;
	(void)inmatch::handle_server_datagram(host.ctx, peer, broken.data(), broken.size());
	// PunkBuster's: the raw line alone.
	const std::vector<uint8_t> pb = {0xFF, 0xFF, 0xFF, 0xFF, 'P', 'B', '_', 'X', 'Y'};
	(void)inmatch::handle_server_datagram(host.ctx, peer, pb.data(), pb.size());
	// The magic packet: 0x1071, '0', 48 bytes.
	std::vector<uint8_t> magic(48, 0);
	magic[0] = 0x71;
	magic[1] = 0x10;
	magic[2] = 0x30;
	std::vector<uint8_t> magic_dg(magic.size() + 4);
	size_t magic_size = 0;
	CHECK(napi_envelope_encode(magic.data(), magic.size(), magic_dg.data(), magic_dg.size(),
			&magic_size) == 0);
	magic_dg.resize(magic_size);
	(void)inmatch::handle_server_datagram(host.ctx, peer, magic_dg.data(), magic_dg.size());

	const std::vector<std::string> lines = log_lines(host.files);
	const auto n = [](size_t v) { return std::to_string(v); };
	const auto m = [](int32_t v) { return std::to_string(v); };
	const std::vector<std::string> expected = {
			"NAPI GOT RAW " + n(probe.size()) + " BYTES FROM " + from,
			"NAPI GOT COOKED " + m(unpacked_len(probe)) + " BYTES FROM " + from,
			"NAPI GOT PROTOCOL " + m(unpacked_len(probe)) + " BYTES FROM " + from + " [" + pn + "]",
			"NAPI GOT RAW " + n(announce.size()) + " BYTES FROM " + from,
			"NAPI GOT COOKED " + m(unpacked_len(announce)) + " BYTES FROM " + from,
			"NAPI TOSSED " + m(unpacked_len(announce)) + " BYTES FROM " + from + " [4]",
			"NAPI GOT RAW " + n(stray.size()) + " BYTES FROM " + from,
			"NAPI GOT COOKED " + m(unpacked_len(stray)) + " BYTES FROM " + from,
			"NAPI TOSSED " + m(unpacked_len(stray)) + " BYTES FROM " + from + " [4]",
			"NAPI GOT RAW " + n(broken.size()) + " BYTES FROM " + from,
			"NAPI TOSSED " + n(broken.size()) + " BYTES FROM " + from + " [3]",
			"NAPI GOT RAW " + n(pb.size()) + " BYTES FROM " + from,
			"NAPI GOT RAW " + n(magic_dg.size()) + " BYTES FROM " + from,
			"NAPI GOT COOKED 48 BYTES FROM " + from,
			"NAPI GOT BAFFLE 48 BYTES FROM " + from,
	};
	CHECK(lines == expected);
	if (lines != expected)
		for (const std::string &l : lines) std::printf("  got: %s\n", l.c_str());

	// The first join: taken on the socket pass (its PROTOCOL line); a probe behind it in the
	// same drain still writes its lines; the queued pass's new connection then turns the log
	// off, after the drain; nothing after it is written.
	// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 (the socket pass @0x62C323 / @0x62C328);
	//  the queued pass NapiNPProtocol_PumpRecvQueues @0x6266A0 (@0x6266CA..0x6267B2) after the
	//  manager's drain]
	ClientAuth auth = make_title_client_auth(kJo, 1, 0x55AA, 0x1234, "Joiner", "SCRK1");
	const std::vector<uint8_t> join =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	(void)inmatch::handle_server_datagram(host.ctx, peer, join.data(), join.size());
	(void)inmatch::handle_server_datagram(host.ctx, peer, probe.data(), probe.size());
	CHECK(host.log.enabled()); // the drain is not over
	inmatch::finish_manager_receive(host.ctx);
	(void)inmatch::handle_server_datagram(host.ctx, peer, probe.data(), probe.size());
	const std::vector<std::string> after = log_lines(host.files);
	CHECK(after.size() == expected.size() + 7);
	if (after.size() == expected.size() + 7) {
		CHECK(after[expected.size()] == "NAPI GOT RAW " + n(join.size()) + " BYTES FROM " + from);
		CHECK(after[expected.size() + 1] ==
				"NAPI GOT COOKED " + m(unpacked_len(join)) + " BYTES FROM " + from);
		CHECK(after[expected.size() + 2] == "NAPI GOT PROTOCOL " + m(unpacked_len(join)) +
				" BYTES FROM " + from + " [" + pn + "]");
		CHECK(after[expected.size() + 3] ==
				"NAPI GOT RAW " + n(probe.size()) + " BYTES FROM " + from);
		CHECK(after[expected.size() + 4] ==
				"NAPI GOT COOKED " + m(unpacked_len(probe)) + " BYTES FROM " + from);
		CHECK(after[expected.size() + 5] == "NAPI GOT PROTOCOL " + m(unpacked_len(probe)) +
				" BYTES FROM " + from + " [" + pn + "]");
		CHECK(after[expected.size() + 6] ==
				"NET SERVER DISABLING CONNECTLOG BECAUSE WE DON'T NEED IT ANYMORE.");
	}
	CHECK(!host.log.enabled());
}

// One flat TLV's size: its name, the NUL, the u16 length, the value [orig: NapiNP_WriteTLV
// @0x61DD60]; a string value carries its NUL.
size_t tlv(const char *name, size_t value_len) { return std::strlen(name) + 1 + 2 + value_len; }
size_t tlv_str(const char *name, const std::string &value) { return tlv(name, value.size() + 1); }

// A listen host's own client joins its own server at the session create, through the manager's
// internal FIFO (address 0, port 0, no RAW or COOKED line): its 0x41 (sent, then taken), the
// server's 0x81, its 0x42 (sent, then taken), and the server-side node its 0x42 creates turns the
// log off. Each length is the packet's TLV sum plus its opcode byte, from the builders' gates.
// [orig: CNapiGameSession_CreateSession @0x4C97C0 (the wait's call @0x4C9C6F);
//  NapiNPSession_SendAnnouncePacket @0x61FA00 (the identity block, CI, no PM under the announce
//  flag @0x61FCCA, no EIP / EPN to address 0; the send callback @0x61FDCA);
//  NapiNPProtocol_SendServerInfoPacket @0x6204B0 (CI, the identity, UT only when nonzero
//  @0x620670, HK and SN always, SF always @0x620898, P1 / P2 / MP when nonzero, NP / NC 0 with no
//  server-side node, RIP / RPN not for address 0 @0x620A81 / @0x620AA9, SUS2 when non-empty);
//  CNapiNPConnection_SendClientJoin @0x61FE20 (the identity, CI, HK, CK, NA, SCRK; no PW, SIP,
//  SPN, CU, NF, DCNT or RCNT on the self-join; the send callback @0x6203F0); NapiLog_OnSend
//  @0x4C4760; NapiNPManager_HandlePacket @0x622F10 (cb_on_dispatch @0x622FCD);
//  NapiNPServer_HandleNewConnection @0x4C8040 (@0x4C8054..0x4C80AF) ahead of the local arm
//  @0x4C8213]
void test_listen_host_self_join_lines() {
	ns::LoopbackChannel loop;
	LoggedHost host(inmatch::ConnectionMode::HostClient, &loop);
	CHECK(!host.log.enabled());
	const inmatch::NapiNPConnection *own = nullptr;
	for (const inmatch::NapiNPConnection &c : host.ctx.np_protocol.connection_list)
		if (c.type == inmatch::NapiNPConnection::kTypeClientSide) own = &c;
	CHECK(own != nullptr);
	if (own == nullptr) return;
	CHECK(own->client_ci != 0 && own->client_ck != 0 && !own->client_scrk.empty());
	const ClientHello id = make_title_client_hello(kJo, own->client_ci);
	const size_t identity = tlv_str("CO", id.co) + tlv_str("AP", id.ap) + tlv_str("BDAT", id.bdat) +
			tlv_str("PN", id.pn) + tlv("PG", 16) + tlv_str("PV1", id.pv1) + tlv_str("PV2", id.pv2) +
			(id.pv3.empty() ? 0 : tlv_str("PV3", id.pv3));
	const size_t hello_len = 1 + tlv_str("NVS", id.nvs) + identity + tlv("CI", 4);
	const inmatch::GameConfig &cfg = host.ctx.config;
	const size_t reply_len = 1 + tlv("CI", 4) + identity + tlv("HK", 4) +
			tlv_str("SN", cfg.server_name) + tlv("SF", 4) +
			(cfg.game_type != 0 ? tlv("P1", 4) : 0) +
			(host.ctx.np_protocol.build_flags != 0 ? tlv("P2", 4) : 0) +
			(host.ctx.np_protocol.max_players != 0 ? tlv("MP", 4) : 0) +
			(cfg.expansion.empty() ? 0 : tlv_str("SUS2", cfg.expansion));
	const std::string na = cfg.player_name.empty() ? "???" : cfg.player_name;
	const size_t join_len = 1 + tlv_str("NVS", id.nvs) + identity + tlv("CI", 4) + tlv("HK", 4) +
			tlv("CK", 4) + tlv_str("NA", na) + tlv_str("SCRK", own->client_scrk);
	const std::string pn = kJo.pn;
	const std::string at = "0.0.0.0:0";
	const std::vector<std::string> lines = log_lines(host.files);
	CHECK(lines.size() == 6);
	if (lines.size() != 6) {
		for (const std::string &l : lines) std::printf("  got: %s\n", l.c_str());
		return;
	}
	const auto sending = [&](size_t len, const char *opcode) {
		return "NET SENDING " + std::to_string(len) + " BYTES TO " + at + " [" + opcode + ", p#0]";
	};
	const auto taken = [&](size_t len) {
		return "NAPI GOT PROTOCOL " + std::to_string(len) + " BYTES FROM " + at + " [" + pn + "]";
	};
	CHECK(lines[0] == sending(hello_len, "0x41"));
	CHECK(lines[1] == taken(hello_len));
	CHECK(lines[2] == taken(reply_len));
	CHECK(lines[3] == sending(join_len, "0x42"));
	CHECK(lines[4] == taken(join_len));
	CHECK(lines[5] == "NET SERVER DISABLING CONNECTLOG BECAUSE WE DON'T NEED IT ANYMORE.");
}


// The remote joiner's client half (L2-s10): while the log is on, every datagram the joiner
// sends writes NET SENDING (its packet before the manager's pack, the host, the opcode, the
// 0x43's packet sequence else 0); each packet the client queue takes writes NET GOT (the
// 0x81 after its CI and identity, the 0x82 after its CI / CK, the 0x83 whose key is the
// connection's, the 0x86); the enumerate's new session writes NET SESSION DISCOVERED with its
// SN, the accepted 0x82 NET CONNECTED TO SERVER with its RIP / RPN as yourip, and every packet
// the in-order parse takes NET PROCESSING with the parse's length plus its opcode byte.
// [orig: NapiLog_OnSend @0x4C4760 - SendAnnouncePacket @0x61FDCA, SendClientJoin @0x6203F0,
//  SendSessionPacket @0x61F00D; NapiNPProtocol_PumpRecvQueues @0x6266A0 - NapiLog_OnRecv
//  @0x6268AA; Nwu_HandleServerHello @0x626D20 - @0x6278D7; CNapiNetwork_OnSessionDiscovered
//  @0x4C8470; CNapiNetwork_OnConnectedToServer @0x4C62E0; CNapiNPConnection_ParseMessages
//  @0x625D3E]
void test_remote_joiner_client_lines() {
	MemoryFiles files;
	inmatch::ConnectLog log{files};
	log.set_clock([] { return std::string("T"); });
	log.start(inmatch::ConnectLogHeader{});
	inmatch::NapiNPServerCtx host;
	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0x0FE0E119u);
	const PeerAddr host_addr = peer_addr_from_octets({10, 0, 0, 1}, 32786);
	const PeerAddr joiner_peer = peer_addr_from_octets({10, 0, 0, 9}, 4200);
	const std::string host_at = "10.0.0.1:32786";
	uint64_t now_ms = 1000;
	inmatch::ClientRuntime client("LogJoiner", [&now_ms] { return now_ms; });
	client.set_title(&kJo);
	client.set_host_address(host_addr);
	client.set_connect_log(&log);
	const auto sending = [&](const std::vector<uint8_t> &dg, const char *opcode, int packet) {
		return "NET SENDING " + std::to_string(unpacked_len(dg)) + " BYTES TO " + host_at + " [" +
				opcode + ", p#" + std::to_string(packet) + "]";
	};
	const auto got = [&](const std::vector<uint8_t> &dg, const char *opcode) {
		return "NET GOT " + std::to_string(unpacked_len(dg)) + " BYTES FROM " + host_at + " [" +
				opcode + "]";
	};
	const auto opcode_of = [](const std::vector<uint8_t> &dg) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		return nw_decode_inbound(dg.data(), dg.size(), opcode, body) ? opcode : uint8_t{0};
	};
	// The manager receive's three lines for a datagram a handler took.
	const std::string pn = kJo.pn != nullptr ? kJo.pn : "";
	const auto manager_taken = [&](std::vector<std::string> &want, const std::vector<uint8_t> &dg) {
		want.push_back("NAPI GOT RAW " + std::to_string(dg.size()) + " BYTES FROM " + host_at);
		want.push_back("NAPI GOT COOKED " + std::to_string(unpacked_len(dg)) + " BYTES FROM " +
				host_at);
		want.push_back("NAPI GOT PROTOCOL " + std::to_string(unpacked_len(dg)) + " BYTES FROM " +
				host_at + " [" + pn + "]");
	};
	// The enumerate's announce.
	const std::vector<uint8_t> hello = client.start();
	std::vector<std::string> lines = log_lines(files);
	CHECK(lines.size() == 1 && lines[0] == sending(hello, "0x41", 0));
	// The host's 0x81: the manager receive's lines (RAW, COOKED, PROTOCOL: the handler queued
	// it), then the client pump's NET GOT and its new session (DISCOVERED) in its frame; the
	// next frame starts the connect state and the first frame past that start sends the 0x42
	// (D-NET-NEW-L2-25).
	const inmatch::HandleResult reply =
			inmatch::handle_server_datagram(host, joiner_peer, hello.data(), hello.size(), 0);
	CHECK(reply.outbound.size() == 1);
	if (reply.outbound.size() != 1) return;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ServerHello sh;
	CHECK(nw_decode_inbound(reply.outbound[0].data(), reply.outbound[0].size(), opcode, body) &&
			parse_server_hello(body.data(), body.size(), sh));
	client.receive(reply.outbound[0].data(), reply.outbound[0].size());
	now_ms += 16;
	std::vector<std::vector<uint8_t>> sent = client.Client_ProcessNetworkFrame(1);
	CHECK(sent.empty() && log_lines(files).size() == 6);
	CHECK(client.Client_ProcessNetworkFrame(1).empty());
	now_ms += 16;
	sent = client.Client_ProcessNetworkFrame(1);
	CHECK(sent.size() == 1 && opcode_of(sent[0]) == SESSION_OPCODE_CLIENT_AUTH);
	if (sent.size() != 1) return;
	lines = log_lines(files);
	std::vector<std::string> hello_frame;
	manager_taken(hello_frame, reply.outbound[0]);
	hello_frame.push_back(got(reply.outbound[0], "0x81"));
	hello_frame.push_back("NET SESSION DISCOVERED AT " + host_at + " [" + sh.sn + "]");
	hello_frame.push_back(sending(sent[0], "0x42", 0));
	CHECK(lines.size() == 1 + hello_frame.size());
	if (lines.size() != 1 + hello_frame.size()) {
		for (const std::string &l : lines) std::printf("  got: %s\n", l.c_str());
		return;
	}
	for (std::size_t i = 0; i < hello_frame.size(); ++i) CHECK(lines[1 + i] == hello_frame[i]);
	const std::size_t before_auth = lines.size();
	// The 0x82 and what the host bundles behind it, one frame: the manager receive's lines
	// for every datagram first (each queued: the 0x83s by their key, ahead of the accept),
	// then the client pump's: the 0x82's NET GOT and NET CONNECTED with the RIP / RPN the host
	// saw, then each 0x83's NET GOT and its parse's NET PROCESSING.
	const std::vector<uint8_t> join = sent[0];
	const inmatch::HandleResult admitted =
			inmatch::handle_server_datagram(host, joiner_peer, join.data(), join.size(), 1);
	CHECK(!admitted.outbound.empty() &&
			opcode_of(admitted.outbound[0]) == SESSION_OPCODE_SERVER_AUTH);
	if (admitted.outbound.empty()) return;
	for (const std::vector<uint8_t> &dg : admitted.outbound) client.receive(dg.data(), dg.size());
	now_ms += 16;
	sent = client.Client_ProcessNetworkFrame(2);
	lines = log_lines(files);
	std::vector<std::string> want;
	for (std::size_t i = 0; i < admitted.outbound.size(); ++i) {
		const uint8_t s2c = opcode_of(admitted.outbound[i]);
		CHECK(s2c == SESSION_OPCODE_SERVER_AUTH || s2c == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE);
		manager_taken(want, admitted.outbound[i]);
	}
	want.push_back(got(admitted.outbound[0], "0x82"));
	want.push_back("NET CONNECTED TO SERVER " + host_at + " [yourip=10.0.0.9:4200]");
	for (std::size_t i = 1; i < admitted.outbound.size(); ++i) {
		if (opcode_of(admitted.outbound[i]) != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE) continue;
		want.push_back(got(admitted.outbound[i], "0x83"));
		want.push_back("NET PROCESSING " + std::to_string(unpacked_len(admitted.outbound[i])) +
				" BYTES FROM " + host_at);
	}
	for (const std::vector<uint8_t> &dg : sent) {
		std::vector<uint8_t> unpacked;
		uint8_t sent_opcode = 0;
		std::vector<uint8_t> sent_body;
		ProtocolPacketHeader header;
		const bool session = nw_unpack_inbound(dg.data(), dg.size(), unpacked) &&
				nw_split_unpacked(unpacked, sent_opcode, sent_body) &&
				sent_opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
				parse_protocol_packet_header(sent_body.data(), sent_body.size(), header);
		CHECK(session);
		want.push_back(sending(dg, "0x43", session ? static_cast<int>(header.seq_num) : -1));
	}
	CHECK(lines.size() == before_auth + want.size());
	if (lines.size() != before_auth + want.size()) {
		for (const std::string &l : lines) std::printf("  got: %s\n", l.c_str());
		return;
	}
	for (std::size_t i = 0; i < want.size(); ++i) CHECK(lines[before_auth + i] == want[i]);
}

// The joiner's receive in two passes (L2-s12, D-NET-NEW-L8-8's receive split): the NAPI
// manager's receive reads every datagram of the frame first, its lines with it, and the 0x84
// resend list acts there (its rebuilt packet carries the ACK the connection held before the
// frame's client pump, its NET SENDING ahead of the 0x84's PROTOCOL line); only then the
// client pump writes the queued packets' NET GOT and parses them. A 0x83 that arrives ahead
// of the accepted 0x82 is queued by its key (PROTOCOL, NET GOT) and its queued leg returns at
// the active test, unparsed.
// [orig: Game_ProcessMainFrame @0x5263F0 - CNapiNetwork_PumpManagerReceive (the call
//  @0x526528); NapiNPManager_PumpReceive @0x623010; NapiNPManager_HandlePacket @0x622F10 -
//  cb_on_dispatch @0x622FCD; NapiNP_HandleResendList @0x623800 - a zero dword resends the next
//  packet @0x6239A4, CNapiNPConnection_SendSessionPacket (the call @0x6239B6);
//  NapiNPProtocol_HandleSessionPacket @0x626A00 - the key @0x626B72, the queue @0x626CB0,
//  the queued leg's active test @0x626B01; NapiNPProtocol_PumpRecvQueues @0x6266A0 -
//  NapiLog_OnRecv @0x6268AA; CNapiNPConnection_ParseMessages @0x625BC0 - @0x625D3E]
void test_remote_joiner_manager_pass_ahead_of_the_client_pump() {
	MemoryFiles files;
	inmatch::ConnectLog log{files};
	log.set_clock([] { return std::string("T"); });
	log.start(inmatch::ConnectLogHeader{});
	inmatch::NapiNPServerCtx host;
	inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0x0FE0E119u);
	const PeerAddr host_addr = peer_addr_from_octets({10, 0, 0, 1}, 32786);
	const PeerAddr joiner_peer = peer_addr_from_octets({10, 0, 0, 9}, 4200);
	const std::string host_at = "10.0.0.1:32786";
	const std::string pn = kJo.pn != nullptr ? kJo.pn : "";
	uint64_t now_ms = 1000;
	inmatch::ClientRuntime client("SplitJoiner", [&now_ms] { return now_ms; });
	client.set_title(&kJo);
	client.set_host_address(host_addr);
	client.set_connect_log(&log);
	const auto decode = [](const std::vector<uint8_t> &dg, uint8_t &opcode,
			std::vector<uint8_t> &body) {
		return nw_decode_inbound(dg.data(), dg.size(), opcode, body);
	};
	// The enumerate, the host's 0x81, and the 0x42 two passes later (D-NET-NEW-L2-25).
	const std::vector<uint8_t> hello = client.start();
	const inmatch::HandleResult reply =
			inmatch::handle_server_datagram(host, joiner_peer, hello.data(), hello.size(), 0);
	CHECK(reply.outbound.size() == 1);
	if (reply.outbound.size() != 1) return;
	client.receive(reply.outbound[0].data(), reply.outbound[0].size());
	std::vector<std::vector<uint8_t>> sent;
	for (int pass = 0; pass < 3 && sent.empty(); ++pass) {
		now_ms += 16;
		sent = client.Client_ProcessNetworkFrame(1);
	}
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	CHECK(sent.size() == 1 && decode(sent[0], opcode, body) &&
			opcode == SESSION_OPCODE_CLIENT_AUTH && parse_client_auth(body.data(), body.size(), auth));
	if (sent.size() != 1) return;
	const inmatch::HandleResult admitted =
			inmatch::handle_server_datagram(host, joiner_peer, sent[0].data(), sent[0].size(), 1);
	std::vector<uint8_t> first_session; // the host's first 0x83 (packet 1)
	std::vector<uint8_t> accept;
	for (const std::vector<uint8_t> &dg : admitted.outbound) {
		if (!decode(dg, opcode, body)) continue;
		if (opcode == SESSION_OPCODE_SERVER_AUTH && accept.empty()) accept = dg;
		if (opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE && first_session.empty())
			first_session = dg;
	}
	CHECK(!accept.empty() && !first_session.empty());
	if (accept.empty() || first_session.empty()) return;
	const auto raw = [&](const std::vector<uint8_t> &dg) {
		return "NAPI GOT RAW " + std::to_string(dg.size()) + " BYTES FROM " + host_at;
	};
	const auto cooked = [&](const std::vector<uint8_t> &dg) {
		return "NAPI GOT COOKED " + std::to_string(unpacked_len(dg)) + " BYTES FROM " + host_at;
	};
	const auto taken = [&](const std::vector<uint8_t> &dg) {
		return "NAPI GOT PROTOCOL " + std::to_string(unpacked_len(dg)) + " BYTES FROM " + host_at +
				" [" + pn + "]";
	};
	const auto got = [&](const std::vector<uint8_t> &dg, const char *op) {
		return "NET GOT " + std::to_string(unpacked_len(dg)) + " BYTES FROM " + host_at + " [" +
				op + "]";
	};
	// A 0x83 ahead of the 0x82: queued by its key, read by the client pump, never parsed.
	std::size_t at = log_lines(files).size();
	client.receive(first_session.data(), first_session.size());
	now_ms += 16;
	(void)client.Client_ProcessNetworkFrame(2);
	std::vector<std::string> lines = log_lines(files);
	const std::vector<std::string> early = {raw(first_session), cooked(first_session),
			taken(first_session), got(first_session, "0x83")};
	CHECK(lines.size() >= at + early.size());
	if (lines.size() < at + early.size()) return;
	for (std::size_t i = 0; i < early.size(); ++i) CHECK(lines[at + i] == early[i]);
	CHECK(lines.size() == at + early.size()); // no NET PROCESSING: the connection is not active
	// The 0x82 accepts.
	client.receive(accept.data(), accept.size());
	now_ms += 16;
	(void)client.Client_ProcessNetworkFrame(3);
	// One frame: the 0x83 again (packet 1, now in order), then a 0x84 whose zero dword asks for
	// the next packet. The manager receive answers the 0x84 before the client pump parses the
	// 0x83: the rebuilt packet's ACK is 0, the connection's before this frame (no packet parsed
	// yet), and it is the frame's first datagram out.
	std::vector<uint8_t> resend_body;
	CHECK(encode_session_resend_list(auth.ck, {0u}, resend_body));
	const std::vector<uint8_t> resend =
			nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, std::move(resend_body));
	at = log_lines(files).size();
	client.receive(first_session.data(), first_session.size());
	client.receive(resend.data(), resend.size());
	now_ms += 16;
	const std::vector<std::vector<uint8_t>> out = client.Client_ProcessNetworkFrame(4);
	std::vector<uint8_t> unpacked;
	ProtocolPacketHeader rebuilt;
	CHECK(!out.empty() && nw_unpack_inbound(out[0].data(), out[0].size(), unpacked) &&
			nw_split_unpacked(unpacked, opcode, body) &&
			opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
			parse_protocol_packet_header(body.data(), body.size(), rebuilt));
	if (out.empty()) return;
	CHECK(rebuilt.ack_count == 0);
	lines = log_lines(files);
	const std::vector<std::string> frame = {raw(first_session), cooked(first_session),
			taken(first_session), raw(resend), cooked(resend),
			"NET SENDING " + std::to_string(unpacked.size()) + " BYTES TO " + host_at + " [0x43, p#" +
					std::to_string(rebuilt.seq_num) + "]",
			taken(resend), got(first_session, "0x83"),
			"NET PROCESSING " + std::to_string(unpacked_len(first_session)) + " BYTES FROM " +
					host_at};
	CHECK(lines.size() >= at + frame.size());
	if (lines.size() < at + frame.size()) {
		for (const std::string &l : lines) std::printf("  got: %s\n", l.c_str());
		return;
	}
	for (std::size_t i = 0; i < frame.size(); ++i) CHECK(lines[at + i] == frame[i]);
}

// A later 0x81 in the pass that records the enumerate's session finds the record by the host's
// address: the same HK and SN update it in place (no second NET SESSION DISCOVERED); another
// SN replaces it: the old record's REMOVED line, the new record's DISCOVERED line, and the join
// takes the new record's HK. A 0x81 in a later frame meets the stopped enumerator: no line, no
// change.
// [orig: Nwu_HandleServerHello @0x626D20 - the record lookup
//  NapiNPEnumerator_FindSessionByAddress (the call @0x627667), the HK test @0x627917,
//  String_ExactMatch @0x627928, the update @0x627BFA..0x627E34; the replace NapiNPTimer_Destroy
//  (the call @0x627941) -> NapiNPSession_Create (the call @0x627947), its callback
//  @0x627BB6..0x627BCD; NapiNPTimer_Destroy @0x621E50 - the +0x20 callback @0x621E63..0x621E6F
//  -> CNapiNetwork_OnSessionRemoved @0x4C68C0; the enumerator gate @0x627547..0x62755C;
//  CNapiNetwork_StartClientConnection @0x4CA160 - NapiTimer_Stop @0x4CA1D8]
void test_remote_joiner_duplicate_hellos() {
	const PeerAddr host_addr = peer_addr_from_octets({10, 0, 0, 1}, 32786);
	const PeerAddr joiner_peer = peer_addr_from_octets({10, 0, 0, 9}, 4200);
	const std::string host_at = "10.0.0.1:32786";
	for (const bool same_record : {true, false}) {
		MemoryFiles files;
		inmatch::ConnectLog log{files};
		log.set_clock([] { return std::string("T"); });
		log.start(inmatch::ConnectLogHeader{});
		inmatch::NapiNPServerCtx host;
		inmatch::test::bring_up_host(host, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Lan, /*host_key=*/0x0FE0E11Au);
		uint64_t now_ms = 1000;
		inmatch::ClientRuntime client("DupJoiner", [&now_ms] { return now_ms; });
		client.set_title(&kJo);
		client.set_host_address(host_addr);
		client.set_connect_log(&log);
		const std::vector<uint8_t> hello = client.start();
		const inmatch::HandleResult reply =
				inmatch::handle_server_datagram(host, joiner_peer, hello.data(), hello.size(), 0);
		CHECK(reply.outbound.size() == 1);
		if (reply.outbound.size() != 1) return;
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ServerHello first;
		CHECK(nw_decode_inbound(reply.outbound[0].data(), reply.outbound[0].size(), opcode, body) &&
				parse_server_hello(body.data(), body.size(), first));
		ServerHello second = first; // the same record: an in-place update, no callback
		if (!same_record) {
			second.sn = first.sn + "-B";
			second.hk = 0x0FE0E11Bu;
		}
		const std::vector<uint8_t> second_dg =
				nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(second));
		const std::size_t at = log_lines(files).size();
		client.receive(reply.outbound[0].data(), reply.outbound[0].size());
		client.receive(second_dg.data(), second_dg.size());
		now_ms += 16;
		CHECK(client.Client_ProcessNetworkFrame(1).empty());
		std::vector<std::string> lines = log_lines(files);
		std::size_t discovered = 0;
		std::string last_sn;
		for (std::size_t i = at; i < lines.size(); ++i) {
			const std::string prefix = "NET SESSION DISCOVERED AT " + host_at + " [";
			if (lines[i].compare(0, prefix.size(), prefix) != 0) continue;
			++discovered;
			last_sn = lines[i].substr(prefix.size(), lines[i].size() - prefix.size() - 1);
		}
		CHECK(discovered == (same_record ? 1u : 2u));
		CHECK(last_sn == (same_record ? first.sn : second.sn));
		// The replace destroys the old record first: its removed line, then the new record's.
		const std::string removed = "NET SESSION AT " + host_at + " [" + first.sn +
				"] HAS BEEN REMOVED.";
		std::size_t removed_at = 0;
		std::size_t second_discovered_at = 0;
		for (std::size_t i = at; i < lines.size(); ++i) {
			if (lines[i] == removed) removed_at = i;
			if (lines[i] == "NET SESSION DISCOVERED AT " + host_at + " [" + second.sn + "]")
				second_discovered_at = i;
		}
		if (same_record) {
			CHECK(removed_at == 0);
		} else {
			CHECK(removed_at != 0 && second_discovered_at == removed_at + 1);
		}
		// A 0x81 in the next frame meets the stopped enumerator.
		ServerHello late = first;
		late.sn = first.sn + "-LATE";
		late.hk = 0x0FE0E11Cu;
		const std::vector<uint8_t> late_dg =
				nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(late));
		client.receive(late_dg.data(), late_dg.size());
		now_ms += 16;
		CHECK(client.Client_ProcessNetworkFrame(1).empty());
		lines = log_lines(files);
		for (std::size_t i = at; i < lines.size(); ++i)
			CHECK(lines[i].find("-LATE") == std::string::npos);
		now_ms += 16;
		const std::vector<std::vector<uint8_t>> sent = client.Client_ProcessNetworkFrame(1);
		ClientAuth auth;
		CHECK(sent.size() == 1 && nw_decode_inbound(sent[0].data(), sent[0].size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_AUTH && parse_client_auth(body.data(), body.size(), auth));
		CHECK(auth.hk == (same_record ? first.hk : second.hk));
	}
}

// The joiner's session destroy (FullDestroy): the leave's lines, then the transport's teardown,
// NET TEARING DOWN SOCKET, unless the join runs on NovaWorld (the menu's connect type 1 keeps
// the transport). A destroy ahead of the 0x82 has no session to leave, so the teardown line
// follows the announce's.
// [orig: CNapiGameSession_FullDestroy @0x4C96A0 - @0x4C96DB, `networkConnectType_480 != 1`
//  @0x4C9722, CNapiNetwork_TearDownSocket (the call @0x4C9763); CNapiNetwork_TearDownSocket
//  @0x4C4C90 - the line @0x4C4CBE..0x4C4CE7]
void test_remote_joiner_destroy_tears_down_the_socket() {
	const PeerAddr host_addr = peer_addr_from_octets({10, 0, 0, 1}, 32786);
	for (const bool novaworld : {false, true}) {
		MemoryFiles files;
		inmatch::ConnectLog log{files};
		log.set_clock([] { return std::string("T"); });
		log.start(inmatch::ConnectLogHeader{});
		uint64_t now_ms = 1000;
		inmatch::ClientRuntime client("TeardownJoiner", [&now_ms] { return now_ms; });
		client.set_title(&kJo);
		client.set_host_address(host_addr);
		client.set_connect_log(&log);
		const std::vector<uint8_t> hello = client.start();
		CHECK(!hello.empty());
		CHECK(client.destroy_session(-1, novaworld).empty());
		const std::vector<std::string> lines = log_lines(files);
		if (novaworld) {
			CHECK(lines.size() == 1);
		} else {
			CHECK(lines.size() == 2 && lines[1] == "NET TEARING DOWN SOCKET");
		}
	}
}

} // namespace

int main() {
	test_line_texts();
	test_dedicated_host_receive_lines();
	test_listen_host_self_join_lines();
	test_remote_joiner_client_lines();
	test_remote_joiner_manager_pass_ahead_of_the_client_pump();
	test_remote_joiner_duplicate_hellos();
	test_remote_joiner_destroy_tears_down_the_socket();
	if (failures != 0) {
		std::printf("connect_log_lines: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("connect_log_lines: ok\n");
	return 0;
}
