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
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/peer_addr.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
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
	CHECK(inmatch::net_session_removed_text(peer, "Host") ==
			"NET SESSION AT 192.168.1.20:3000 [Host] HAS BEEN REMOVED.");
	CHECK(inmatch::net_session_discovered_text(peer, "Host") ==
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
	for (const std::string &l : lines) std::printf("  self-join: %s\n", l.c_str());
}

} // namespace

int main() {
	test_line_texts();
	test_dedicated_host_receive_lines();
	test_listen_host_self_join_lines();
	if (failures != 0) {
		std::printf("connect_log_lines: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("connect_log_lines: ok\n");
	return 0;
}
