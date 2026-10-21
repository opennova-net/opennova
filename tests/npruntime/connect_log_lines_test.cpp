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
//  @0x62B750; NapiNPServer_HandleNewConnection @0x4C8054..0x4C80AF; CNapiNetwork_Init
//  @0x4CA908..0x4CA9CD; Napi_FormatAddress @0x62DED0]
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

	// The first join: taken on the socket pass (its PROTOCOL line), then the new connection
	// turns the log off; nothing after it is written.
	ClientAuth auth = make_title_client_auth(kJo, 1, 0x55AA, 0x1234, "Joiner", "SCRK1");
	const std::vector<uint8_t> join =
			nw_encode_outbound(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	(void)inmatch::handle_server_datagram(host.ctx, peer, join.data(), join.size());
	(void)inmatch::handle_server_datagram(host.ctx, peer, probe.data(), probe.size());
	const std::vector<std::string> after = log_lines(host.files);
	CHECK(after.size() == expected.size() + 4);
	if (after.size() == expected.size() + 4) {
		CHECK(after[expected.size()] == "NAPI GOT RAW " + n(join.size()) + " BYTES FROM " + from);
		CHECK(after[expected.size() + 1] ==
				"NAPI GOT COOKED " + m(unpacked_len(join)) + " BYTES FROM " + from);
		CHECK(after[expected.size() + 2] == "NAPI GOT PROTOCOL " + m(unpacked_len(join)) +
				" BYTES FROM " + from + " [" + pn + "]");
		CHECK(after[expected.size() + 3] ==
				"NET SERVER DISABLING CONNECTLOG BECAUSE WE DON'T NEED IT ANYMORE.");
	}
	CHECK(!host.log.enabled());
}

// A listen host's own connection turns the log off at its session create.
// [orig: NapiNPServer_HandleNewConnection @0x4C8054..0x4C80AF ahead of the local arm @0x4C8213]
void test_listen_host_create_turns_the_log_off() {
	ns::LoopbackChannel loop;
	LoggedHost host(inmatch::ConnectionMode::HostClient, &loop);
	CHECK(!host.log.enabled());
	const std::vector<std::string> lines = log_lines(host.files);
	CHECK(lines.size() == 1 &&
			lines[0] == "NET SERVER DISABLING CONNECTLOG BECAUSE WE DON'T NEED IT ANYMORE.");
}

} // namespace

int main() {
	test_line_texts();
	test_dedicated_host_receive_lines();
	test_listen_host_create_turns_the_log_off();
	if (failures != 0) {
		std::printf("connect_log_lines: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("connect_log_lines: ok\n");
	return 0;
}
