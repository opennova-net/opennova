// The service pushes ServerCommand and ServerStopHosting to a listed host over the host's own
// NovaWorld session, through the real listener over loopback UDP.
//
// A stock host reads both statements off its NOVAWORLDUDP connection's statement dispatch
// [orig: CNapiGameSession_DispatchServerStatement @0x4d18a0 -> CNapiGameSession_HandleServerCommand
// @0x4d22f0 / CNapiGameSession_HandleServerMessage @0x4d1c50]. The service frames each push as a
// reliable record exactly as it frames a reply, so the connection's ordered receive and its 0x44
// recover a lost one; the service's ACTIVE send-interval leg (CS field 5, 1000 ms) sends a
// header-only packet each interval while the record is unACKed, whose fresh sequence shows the
// client the gap [orig: CNapiNPConnection_PumpSendIntervals @0x628fd0, the active leg
// @0x628ff1..0x629017]. This drives a ClientSession (the host's NWU session) against the listener
// over a database of the test's own: the pushes are commands this thread inserts on its own
// connection (lobby_repository.h) and the listener's receive thread takes and sends. The session
// has its lobby_sessions row from its Hello on, naming its hosting once it hosts; the RID resolves
// only once hosting, a pushed SetServerName lands as the session's Command notice and once ACKed
// draws nothing more, a command left waiting past its expiry is dropped unsent, a pushed
// TextChatServer whose first packet is lost lands after the 0x44 under the same sequence, and a
// pushed ServerStopHosting lands as the StopHosting notice (the sysop-punt key) and takes the row
// out of the browser. Every command run is gone from lobby_commands, and the session's row goes
// with its goodbye.

#include "nw_udp_listener.h"
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>
#include "program/server_config.h"

#include <net/napi/session.h>
#include <net/napi/tlv.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/lobby_repository.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include "net_sockets.h"

#include "common/test_database.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

namespace {

using opennova::lobbydb::PushResult;

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

int main(int argc, char **argv) {
	using namespace std::chrono_literals;
	using Notice = opennova::ClientSession::Notice;
	test_db::init(argc, argv);
	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	test_db::TestDatabase database;
	opennova::db::ConnectionPool pool(database.options());
	opennova::ConnectionManager manager;
	opennova::novaworld_server::NwUdpListener listener(manager);
	listener.set_db_pool(&pool);
	manager.on_lost([&listener](const opennova::Connection &connection, opennova::DropReason reason) {
		listener.erase_lobby_state(connection.addr, opennova::drop_reason_name(reason));
	});
	// The pushes, as a route inserts them: on a connection of this thread's own.
	auto push_command = [&pool](uint32_t rid, const std::string &cmd) {
		return opennova::lobbydb::queue_server_command(*pool.acquire(), rid, cmd);
	};
	auto push_stop = [&pool](uint32_t rid) {
		return opennova::lobbydb::queue_stop_hosting(
				*pool.acquire(), rid, opennova::SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT);
	};
	auto commands_left = [&pool] {
		return pool.acquire()->query("SELECT 1 FROM lobby_commands;").size();
	};
	opennova::novaworld_server::ServerConfig config;
	config.nw_udp_port = 0;
	// The endpoint its instance announces (the in-test gate hands out the port
	// it bound instead).
	config.lobby_endpoint = "198.51.100.7:64206";
	if (!listener.start(config) || listener.bound_port() == 0) {
		std::fprintf(stderr, "FAIL: listener.start\n");
		return 1;
	}

	opennova::ClientSession::Config cfg = opennova::ClientSession::Config::lobby(kJo, kJo.gate_tag);
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
	const std::string client_addr = "127.0.0.1:" + std::to_string(client_port);
	// The session's lobby_sessions row, by its address.
	auto session_row = [&pool, &client_addr]() -> std::optional<opennova::lobbydb::SessionRow> {
		for (auto &row : opennova::lobbydb::list_sessions(*pool.acquire())) {
			if (opennova::lobbydb::session_addr(row) == client_addr) return row;
		}
		return std::nullopt;
	};
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
	// Its row, written before its 0x82 went out: active, its NA, hosting nothing.
	{
		const auto row = session_row();
		expect(row.has_value(), "the verified session has its lobby_sessions row");
		if (row) {
			expect(row->state == "active" && row->identity == "push:host" &&
			               row->pn == "NOVAWORLDUDP",
			       "the row is the active session's, with its NA and PN");
			expect(!row->hosting && !row->listed && row->rid == 0, "the row hosts nothing yet");
			expect(row->connected_ms > 0 && row->last_seen_ms >= row->connected_ms,
			       "the row's times are wall-clock ones");
		}
	}

	// No hosting yet: no RID resolves, and 0 never does.
	expect(push_command(0, "Cycle") == PushResult::UnknownRid, "RID 0 is unknown");
	expect(push_command(0x0A000001u, "Cycle") == PushResult::UnknownRid,
	       "a RID before any hosting is unknown");
	expect(commands_left() == 0, "a refused push inserts nothing");

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
		const auto row = session_row();
		expect(row && row->hosting && row->listed && row->rid == rid &&
		               row->server_name == "Push Host" && row->players == 1 && row->max_players == 8,
		       "the session's row names its listing");
	}

	if (rid != 0) {
		expect(push_command(rid + 1, "Cycle") == PushResult::UnknownRid,
		       "a RID no connection holds is unknown");

		// --- a pushed SetServerName lands as the Command notice.
		notices.clear();
		const std::string rename = opennova::server_command_text(
				opennova::ServerCommandVerb::SetServerName, opennova::ServerCommandTarget::None,
				{"Pushed Name"});
		expect(rename == "SetServerName \"Pushed Name\"", "the composed rename");
		expect(push_command(rid, rename) == PushResult::Queued,
		       "the rename is queued for the hosting connection");
		service(3000, nullptr, [&] { return find_notice(Notice::Kind::Command) != nullptr; });
		const Notice *renamed = find_notice(Notice::Kind::Command);
		expect(renamed != nullptr, "the pushed ServerCommand reaches the host's session");
		if (renamed != nullptr) {
			expect(renamed->command.verb == opennova::ServerCommandVerb::SetServerName &&
			               renamed->command.args == std::vector<std::string>{"Pushed Name"},
			       "the host's reader sees SetServerName with its one arg");
		}

		// --- once the push is ACKed the service has nothing retained, so the send-interval leg
		// stays quiet: the idle verified session hears nothing for well past one interval. The
		// session's ACK went out with the notice's pass; the service answers that packet once.
		service(300, nullptr, [] { return false; });
		int heard = 0;
		service(1600,
		        [&](const Seen &) {
			        ++heard;
			        return false;
		        },
		        [] { return false; });
		expect(heard == 0, "an ACKed push draws no further packet within 1.6 s");
		expect(commands_left() == 0, "the lobby took the push it ran");

		// --- a command that waited past its expiry is dropped unsent: the drain held, the
		// rename queued and aged past its expires_at, then the drain takes it and drops it.
		notices.clear();
		listener.pause_command_drain(true);
		expect(push_command(rid, "SetServerName \"Too Late\"") == PushResult::Queued,
		       "the late rename is queued");
		pool.acquire()->exec("UPDATE lobby_commands SET expires_at = now() - interval '1 second';");
		listener.pause_command_drain(false);
		service(1000, nullptr, [&] { return find_notice(Notice::Kind::Command) != nullptr; });
		expect(find_notice(Notice::Kind::Command) == nullptr, "an expired command never goes out");
		expect(commands_left() == 0, "the expired command is gone from the table");

		// --- a push whose first packet is lost: the send-interval leg's header-only packet shows
		// the gap, the client's 0x44 asks for it, and the records come back under the same
		// sequence with the same bytes.
		notices.clear();
		std::optional<Seen> lost;
		std::optional<Seen> recovered;
		bool saw_header_only = false;
		const auto pushed_at = std::chrono::steady_clock::now();
		expect(push_command(rid, "TextChatServer \"hello host\"") == PushResult::Queued,
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
		expect(push_stop(rid) == PushResult::Queued, "the stop is queued");
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
		const auto stopped_row = session_row();
		expect(stopped_row && !stopped_row->hosting && !stopped_row->listed &&
		               stopped_row->rid == rid,
		       "the session's row no longer hosts, and keeps its RID");
		expect(push_command(rid, "Cycle") == PushResult::NotHosted,
		       "a RID whose connection stopped hosting is refused as not hosting");

		// --- the goodbye drops the connection: its row goes, and its RID is unknown again.
		send(session.build_goodbye());
		bool gone = false;
		for (int i = 0; i < 50 && !gone; ++i) {
			std::this_thread::sleep_for(20ms);
			gone = push_command(rid, "Cycle") == PushResult::UnknownRid;
		}
		expect(gone, "a dropped connection's RID is unknown");
		expect(!session_row().has_value(), "the dropped session's row is gone");
		expect(commands_left() == 0, "nothing waits for the dropped session");
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
