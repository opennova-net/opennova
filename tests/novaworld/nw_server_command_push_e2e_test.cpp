// The service pushes ServerCommand and ServerStopHosting to a listed host over the host's own
// NovaWorld session, through the real listener over loopback UDP.
//
// A stock host reads both statements off its NOVAWORLDUDP connection's statement dispatch
// [orig: CNapiGameSession_DispatchServerStatement @0x4d18a0 -> CNapiGameSession_HandleServerCommand
// @0x4d22f0 / CNapiGameSession_HandleServerMessage @0x4d1c50]. The service frames each push as a
// reliable record exactly as it frames a reply, so the connection's ordered receive and its 0x44
// recover a lost one; the service's ACTIVE send-interval leg (CS field 5, 1000 ms) sends a
// header-only packet while the record is unACKed, whose fresh sequence shows the client the gap
// [orig: CNapiNPConnection_PumpSendIntervals @0x628fd0, the active leg @0x628ff1..0x629017].
// This drives a ClientSession (the host's NWU session) against the listener: the RID resolves only
// once hosting, a pushed SetServerName lands as the session's Command notice, a pushed
// TextChatServer whose first packet is lost lands after the 0x44 under the same sequence, and a
// pushed ServerStopHosting lands as the StopHosting notice (the sysop-punt key) and takes the row
// out of the browser.

#include "nw_udp_listener.h"
#include "server_config.h"

#include <net/napi/session.h>
#include <net/napi/tlv.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include "net_sockets.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using opennova::novaworld_server::HostPushResult;

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

opennova::net::Endpoint loopback_ep(uint16_t port) {
	opennova::net::Endpoint ep;
	ep.ip = {127, 0, 0, 1};
	ep.port = port;
	return ep;
}

// A service datagram as the test sees it: a 0x83's header and records, decoded with the session's
// receive key; `records` false for any other opcode or a header-only packet.
struct Seen {
	bool session_packet = false;
	opennova::ProtocolPacketHeader header{};
	std::vector<opennova::ProtocolMessage> messages;
	std::vector<std::string> statements; // the container names the records carry
};

Seen inspect(const uint8_t *data, size_t size, const std::string &scrk) {
	Seen seen;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!opennova::nw_decode_inbound(data, size, opcode, body) ||
	    opcode != opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE) {
		return seen;
	}
	if (!opennova::decode_protocol_packet_plaintext(body.data(), body.size(), scrk, seen.header,
	                                               seen.messages)) {
		return seen;
	}
	seen.session_packet = true;
	for (const opennova::ProtocolMessage &pm : seen.messages) {
		std::vector<opennova::NapiMessage> stream;
		size_t consumed = 0;
		if (opennova::napi_stream_decode(pm.payload.data(), pm.payload.size(), stream, &consumed) != 0)
			continue;
		for (const auto &m : stream) seen.statements.push_back(m.name);
	}
	return seen;
}

bool carries(const Seen &seen, const char *name) {
	for (const std::string &s : seen.statements) {
		if (s == name) return true;
	}
	return false;
}

} // namespace

int main() {
	using namespace std::chrono_literals;
	using Notice = opennova::ClientSession::Notice;
	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	opennova::ConnectionManager manager;
	opennova::novaworld_server::NwUdpListener listener(manager);
	manager.on_lost([&listener](const opennova::Connection &connection, opennova::DropReason reason) {
		listener.erase_lobby_state(connection.addr, opennova::drop_reason_name(reason));
	});
	opennova::novaworld_server::ServerConfig config;
	config.nw_udp_port = 0;
	if (!listener.start(config) || listener.bound_port() == 0) {
		std::fprintf(stderr, "FAIL: listener.start\n");
		return 1;
	}

	opennova::ClientSession::Config cfg;
	cfg.client_index = 0x50555348u;
	cfg.client_key = 0x434D4453u;
	cfg.na = "push:host";
	cfg.cookie_vars = []() {
		return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
	};
	opennova::ClientSession session(cfg);
	uint16_t client_port = 0;
	auto client = opennova::net::udp_bind(0, &client_port);
	expect(client.is_valid(), "client UDP socket bound");
	const auto server_ep = loopback_ep(listener.bound_port());
	auto send = [&](const std::vector<uint8_t> &dg) {
		if (!dg.empty()) opennova::net::udp_send_to(client, server_ep, dg.data(), dg.size());
	};
	std::vector<Notice> notices;
	// The host's receive pump for up to `ms`: every datagram `drop` passes over is fed to the
	// session, each one its own receive batch (the 0x44 tail, then the send pump), until `done`.
	auto service = [&](int ms, const std::function<bool(const Seen &)> &drop,
	                   const std::function<bool()> &done) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		uint8_t rx[4096];
		while (std::chrono::steady_clock::now() < deadline && !done()) {
			opennova::net::Endpoint from;
			const int n = opennova::net::udp_recv_from(client, rx, sizeof rx, from, 20);
			std::vector<std::vector<uint8_t>> out;
			if (n > 0) {
				const Seen seen = inspect(rx, static_cast<size_t>(n), session.server_scrk());
				if (drop && drop(seen)) continue;
				session.handle_datagram(rx, static_cast<size_t>(n), out);
			}
			session.finish_receive_batch(out);
			session.pump(out);
			session.process_periodic_update();
			for (const auto &dg : out) send(dg);
			for (Notice &notice : session.take_notices()) notices.push_back(std::move(notice));
		}
		return done();
	};
	auto find_notice = [&](Notice::Kind kind) -> const Notice * {
		for (const Notice &notice : notices) {
			if (notice.kind == kind) return &notice;
		}
		return nullptr;
	};

	send(session.start());
	service(8000, nullptr, [&] { return session.is_verified(); });
	expect(session.is_verified(), "the session verifies against the real listener");

	// No hosting yet: no RID resolves, and 0 never does.
	expect(listener.push_server_command(0, "Cycle") == HostPushResult::UnknownRid,
	       "RID 0 is unknown");
	expect(listener.push_server_command(0x0A000001u, "Cycle") == HostPushResult::UnknownRid,
	       "a RID before any hosting is unknown");

	uint32_t rid = 0;
	if (session.is_verified()) {
		session.queue_statement(opennova::make_client_host_request(
				0, {{0, "NWUID", session.server_nwuid()}},
				{{0, "LobbyName", "jop_2_consumer"}, {0, "MaxPlayers", "8"}},
				{{0, "ServerName", "Push Host"}, {0, "Players", "1"}},
				{{0, "PlayerName", "Host"}}));
		service(3000, nullptr, [&] { return find_notice(Notice::Kind::HostResult) != nullptr; });
		const Notice *host = find_notice(Notice::Kind::HostResult);
		expect(host != nullptr && host->fields.success != 0, "the host request is answered with success");
		for (const auto &h : listener.snapshot_hosted()) {
			if (h.lobby.server_name == "Push Host") rid = h.lobby.rid;
		}
		expect(rid != 0, "the hosted row carries a RID");
	}

	if (rid != 0) {
		expect(listener.push_server_command(rid + 1, "Cycle") == HostPushResult::UnknownRid,
		       "a RID no connection holds is unknown");

		// --- a pushed SetServerName lands as the Command notice.
		notices.clear();
		const std::string rename = opennova::server_command_text(
				opennova::ServerCommandVerb::SetServerName, opennova::ServerCommandTarget::None,
				{"Pushed Name"});
		expect(rename == "SetServerName \"Pushed Name\"", "the composed rename");
		expect(listener.push_server_command(rid, rename) == HostPushResult::Queued,
		       "the rename is queued for the hosting connection");
		service(3000, nullptr, [&] { return find_notice(Notice::Kind::Command) != nullptr; });
		const Notice *renamed = find_notice(Notice::Kind::Command);
		expect(renamed != nullptr, "the pushed ServerCommand reaches the host's session");
		if (renamed != nullptr) {
			expect(renamed->command.verb == opennova::ServerCommandVerb::SetServerName &&
			               renamed->command.args == std::vector<std::string>{"Pushed Name"},
			       "the host's reader sees SetServerName with its one arg");
		}

		// --- a push whose first packet is lost: the send-interval leg's header-only packet shows
		// the gap, the client's 0x44 asks for it, and the records come back under the same
		// sequence with the same bytes.
		notices.clear();
		std::optional<Seen> lost;
		std::optional<Seen> recovered;
		bool saw_header_only = false;
		const auto pushed_at = std::chrono::steady_clock::now();
		expect(listener.push_server_command(rid, "TextChatServer \"hello host\"") ==
		               HostPushResult::Queued,
		       "the chat is queued");
		service(5000,
		        [&](const Seen &seen) {
			        if (!seen.session_packet) return false;
			        if (seen.messages.empty()) {
				        if (lost) saw_header_only = true;
				        return false;
			        }
			        if (!carries(seen, "ServerCommand")) return false;
			        if (!lost) {
				        lost = seen;
				        return true; // the loss
			        }
			        if (!recovered) recovered = seen;
			        return false;
		        },
		        [&] { return find_notice(Notice::Kind::Command) != nullptr; });
		const auto recovered_after = std::chrono::steady_clock::now() - pushed_at;
		const Notice *chat = find_notice(Notice::Kind::Command);
		expect(lost.has_value(), "the push's first packet was seen (and dropped)");
		expect(saw_header_only, "a header-only packet follows the unACKed push");
		expect(recovered.has_value(), "the push's records come back after the 0x44");
		if (lost && recovered) {
			expect(recovered->header.seq_num == lost->header.seq_num,
			       "the resend reuses the lost packet's sequence");
			expect(recovered->messages.size() == lost->messages.size() &&
			               recovered->messages.front().payload == lost->messages.front().payload,
			       "the resend carries the same record");
		}
		expect(chat != nullptr && chat->command.verb == opennova::ServerCommandVerb::TextChatServer &&
		               chat->command.args == std::vector<std::string>{"hello host"},
		       "the lost chat reaches the host's reader after all");
		expect(recovered_after >= 900ms, "the recovery waited for the send interval");
		std::printf("recovered the lost push after %lld ms\n",
		            static_cast<long long>(
		                    std::chrono::duration_cast<std::chrono::milliseconds>(recovered_after).count()));

		// --- ServerStopHosting: the StopHosting notice with the sysop-punt key, and the row
		// leaves the browser though the host sends no ClientStopHosting.
		notices.clear();
		expect(listener.push_stop_hosting(rid, opennova::SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT) ==
		               HostPushResult::Queued,
		       "the stop is queued");
		service(3000, nullptr, [&] { return find_notice(Notice::Kind::StopHosting) != nullptr; });
		const Notice *stop = find_notice(Notice::Kind::StopHosting);
		expect(stop != nullptr, "the pushed ServerStopHosting reaches the host's session");
		if (stop != nullptr) {
			expect(stop->fields.msg_code == 7 && stop->fields.msg_param1 == 0 &&
			               stop->fields.msg_param2 == 0,
			       "MsgCode 7, params 0");
			expect(stop->msg_key == "NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT", "the sysop-punt key");
		}
		expect(session.session_role() == opennova::ClientSession::kSessionRoleVerified,
		       "the host's session word drops to verified");
		bool listed = false;
		for (const auto &h : listener.snapshot_hosted()) listed = listed || h.lobby.rid == rid;
		expect(!listed, "the stopped host leaves the browser");
		expect(listener.push_server_command(rid, "Cycle") == HostPushResult::NotHosted,
		       "a RID whose connection stopped hosting is refused as not hosting");

		// --- the goodbye drops the connection: its RID is unknown again.
		send(session.build_goodbye());
		bool gone = false;
		for (int i = 0; i < 50 && !gone; ++i) {
			std::this_thread::sleep_for(20ms);
			gone = listener.push_server_command(rid, "Cycle") == HostPushResult::UnknownRid;
		}
		expect(gone, "a dropped connection's RID is unknown");
	}

	opennova::net::close_socket(client);
	listener.stop();
	opennova::net::shutdown();
	if (g_failures == 0) {
		std::printf("OK: the service pushes ServerCommand / ServerStopHosting to a listed host\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
