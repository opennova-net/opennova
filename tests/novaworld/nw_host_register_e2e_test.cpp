// End-to-end host registration through the REAL gate listener.
//
// The in-process tests cover the two halves separately: client_session_loopback
// drives ClientSession against a mock server, and host_register_list drives a
// LobbySession directly. Neither exercises the actual apps/novaworld_server
// NwUdpListener — the UDP socket, the NWU/CRC envelope decode, the HELLO->AUTH->
// SESSION opcode dispatch, and the PN routing into the lobby path. This closes
// that gap: it stands up a real NwUdpListener on a loopback UDP port and drives
// a real ClientSession (the same one NovaWorldClient runs when it hosts) through the full
// handshake to Verified, then sends a ClientHostRequest and asserts the host
// shows up in the listener's hosted snapshot — the source /api/hosts + the GSB
// browser read from. This is the F1 "browsable host" claim, verified against the
// process that serves it. [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]

#include "nw_udp_listener.h"
#include "server_config.h"

#include <net/napi/session.h>            // make_client_host_request, ClientVar
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_connection.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include "net_sockets.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

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

} // namespace

int main() {
	using namespace std::chrono_literals;

	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}

	// Stand up the real listener (no DB: snapshot_hosted reads the in-memory
	// lobby state, so host registration is observable without SQLite).
	opennova::ConnectionManager manager;
	opennova::novaworld_server::NwUdpListener listener(manager);
	manager.on_lost([&listener](
				const opennova::Connection &connection,
				opennova::DropReason reason) {
		listener.erase_lobby_state(
				connection.addr, opennova::drop_reason_name(reason));
	});
	opennova::novaworld_server::ServerConfig config;
	config.nw_udp_port = 0;
	if (!listener.start(config)) {
		std::fprintf(stderr, "FAIL: listener.start\n");
		return 1;
	}
	const uint16_t test_nw_port = listener.bound_port();
	if (test_nw_port == 0) {
		std::fprintf(stderr, "FAIL: listener reports OS-assigned UDP port\n");
		listener.stop();
		opennova::net::shutdown();
		return 1;
	}
	// Keep the selected port for the stop/start lifecycle check later in this
	// test; only the initial allocation needs to be collision-free.
	config.nw_udp_port = test_nw_port;
	// No wait before the first datagram: start() hands its bound socket to the
	// worker thread, so a datagram sent now queues on the port it reported.

	// The client: a real ClientSession over a real UDP socket — exactly what
	// NwuLobbySession::begin_session sets up (minus the gate-probe leg, which only
	// resolves this endpoint; we know it directly).
	opennova::ClientSession::Config cfg;
	cfg.cookie_vars = []() {
		return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
	};  // echoed from the SessionInit
	opennova::ClientSession session(cfg);

	uint16_t client_port = 0;
	auto client = opennova::net::udp_bind(0, &client_port);
	expect(client.is_valid(), "client UDP socket bound");
	const auto server_ep = loopback_ep(test_nw_port);

	// ClientAuth is not a stateless entry point for the lobby route. A peer
	// must first own the exact Handshaking row created by its ClientHello;
	// otherwise an unsolicited 0x42 would mint ServerAuth material and leave
	// LobbyConnState with no ConnectionManager owner/timeout.
	{
		uint16_t direct_auth_port = 0;
		auto direct_auth_client =
				opennova::net::udp_bind(0, &direct_auth_port);
		expect(direct_auth_client.is_valid(),
		       "direct-Auth lobby UDP socket bound");

		opennova::ClientSession::Config direct_cfg;
		direct_cfg.client_index = 0x44495245u;
		direct_cfg.client_key = 0x43544155u;
		direct_cfg.na = "direct:auth";
		const opennova::ClientAuth direct_auth =
				opennova::make_client_auth(
						direct_cfg, direct_cfg.co, 0x12345678u,
						opennova::make_dev_scrk());
		const std::vector<uint8_t> direct_datagram =
				opennova::nw_encode_outbound(
						opennova::SESSION_OPCODE_CLIENT_AUTH,
						opennova::client_auth_to_bytes(direct_auth));
		if (direct_auth_client.is_valid()) {
			opennova::net::udp_send_to(
					direct_auth_client, server_ep,
					direct_datagram.data(), direct_datagram.size());
		}

		uint8_t direct_rx[4096];
		opennova::net::Endpoint direct_from;
		const int direct_reply_size =
				direct_auth_client.is_valid()
						? opennova::net::udp_recv_from(
								direct_auth_client, direct_rx,
								sizeof direct_rx, direct_from, 250)
						: -1;
		expect(direct_reply_size <= 0,
		       "direct lobby ClientAuth receives no ServerAuth");
		const opennova::PeerAddr direct_peer{
				0x0100007Fu, direct_auth_port};
		expect(!manager.registry().find_by_addr(direct_peer),
		       "direct lobby ClientAuth creates no manager identity");
		opennova::net::close_socket(direct_auth_client);
	}

	// A delayed Auth from an older Hello must not claim a newer Hello's
	// address row. Preserve the replacement Handshaking identity so its own
	// matching Auth can still complete.
	{
		uint16_t reordered_port = 0;
		auto reordered_client =
				opennova::net::udp_bind(0, &reordered_port);
		expect(reordered_client.is_valid(),
		       "reordered-Auth lobby UDP socket bound");

		auto send_reordered = [&](const std::vector<uint8_t> &dg) {
			if (!dg.empty()) {
				opennova::net::udp_send_to(
						reordered_client, server_ep, dg.data(), dg.size());
			}
		};
		auto receive_reordered = [&](std::vector<uint8_t> &dg, int timeout_ms) {
			uint8_t reordered_rx[4096];
			opennova::net::Endpoint from;
			const int n = opennova::net::udp_recv_from(
					reordered_client, reordered_rx, sizeof reordered_rx,
					from, timeout_ms);
			if (n <= 0) return false;
			dg.assign(reordered_rx, reordered_rx + n);
			return true;
		};

		opennova::ClientSession::Config old_cfg;
		old_cfg.client_index = 0x4F4C4441u;
		old_cfg.client_key = 0x4F4C444Bu;
		old_cfg.na = "reordered:old";
		opennova::ClientSession old_session(old_cfg);
		send_reordered(old_session.start());
		std::vector<uint8_t> inbound;
		std::vector<std::vector<uint8_t>> old_out;
		const bool got_old_hello =
				receive_reordered(inbound, 1000);
		expect(got_old_hello, "old lobby Hello receives ServerHello");
		if (got_old_hello) {
			expect(old_session.handle_datagram(
					       inbound.data(), inbound.size(), old_out) &&
			               old_out.size() == 1,
			       "old ServerHello produces delayed ClientAuth");
		}
		const std::vector<uint8_t> delayed_old_auth =
				old_out.empty() ? std::vector<uint8_t>{} : old_out.front();

		opennova::ClientSession::Config fresh_cfg;
		fresh_cfg.client_index = 0x4E455741u;
		fresh_cfg.client_key = 0x4E45574Bu;
		fresh_cfg.na = "reordered:fresh";
		opennova::ClientSession fresh_session(fresh_cfg);
		send_reordered(fresh_session.start());
		inbound.clear();
		std::vector<std::vector<uint8_t>> fresh_out;
		const bool got_fresh_hello =
				receive_reordered(inbound, 1000);
		expect(got_fresh_hello,
		       "replacement lobby Hello receives ServerHello");
		if (got_fresh_hello) {
			expect(fresh_session.handle_datagram(
					       inbound.data(), inbound.size(), fresh_out) &&
			               fresh_out.size() == 1,
			       "replacement ServerHello produces matching ClientAuth");
		}
		const std::vector<uint8_t> matching_fresh_auth =
				fresh_out.empty() ? std::vector<uint8_t>{} : fresh_out.front();

		send_reordered(delayed_old_auth);
		inbound.clear();
		expect(!receive_reordered(inbound, 250),
		       "stale ClientAuth receives no ServerAuth after replacement Hello");
		const opennova::PeerAddr reordered_peer{
				0x0100007Fu, reordered_port};
		const auto after_stale_auth =
				manager.registry().find_by_addr(reordered_peer);
		expect(after_stale_auth &&
		               after_stale_auth->reported_id ==
				               fresh_cfg.client_index &&
		               after_stale_auth->state ==
				               opennova::ConnectionState::Handshaking,
		       "stale Auth preserves the replacement Hello identity");

		send_reordered(matching_fresh_auth);
		inbound.clear();
		const bool got_matching_auth =
				receive_reordered(inbound, 1000);
		uint8_t matching_opcode = 0;
		std::vector<uint8_t> matching_body;
		expect(got_matching_auth &&
		               opennova::nw_decode_inbound(
				               inbound.data(), inbound.size(),
				               matching_opcode, matching_body) &&
		               matching_opcode ==
				               opennova::SESSION_OPCODE_SERVER_AUTH,
		       "matching replacement ClientAuth receives ServerAuth");
		opennova::net::close_socket(reordered_client);
	}

	auto send = [&](const std::vector<uint8_t> &dg) {
		if (!dg.empty())
			opennova::net::udp_send_to(client, server_ep, dg.data(), dg.size());
	};
	// The owner's tick after its receive batch: the missing-sequence tail and the send pump,
	// everything they build on the wire.
	auto flush_session = [&]() {
		std::vector<std::vector<uint8_t>> out;
		session.finish_receive_batch(out);
		session.pump(out);
		for (const auto &dg : out) send(dg);
	};
	// Receive whatever the listener sent (a 0x84 among it) and answer, for `ms` milliseconds.
	auto service_session = [&](int ms) {
		uint8_t buf[4096];
		for (int waited = 0; waited < ms; waited += 20) {
			opennova::net::Endpoint from;
			const int n = opennova::net::udp_recv_from(client, buf, sizeof buf, from, 20);
			if (n <= 0) continue;
			std::vector<std::vector<uint8_t>> out;
			session.handle_datagram(buf, static_cast<size_t>(n), out);
			for (const auto &dg : out) send(dg);
			flush_session();
		}
	};

	// Drive the handshake to Verified: send ClientHello, then pump replies back
	// through the session, then run the same periodic-update boundary as the
	// Godot pump, until the ServerVerifyResult lands or we time out.
	send(session.start());
	uint8_t rx[4096];
	bool verified = false;
	for (int i = 0; i < 40 && !verified; ++i) {
		opennova::net::Endpoint from;
		const int n = opennova::net::udp_recv_from(client, rx, sizeof rx, from, 200);
		std::vector<std::vector<uint8_t>> out;
		if (n > 0) {
			if (!session.handle_datagram(rx, static_cast<size_t>(n), out)) {
				std::fprintf(stderr, "FAIL: session error: %s\n",
				             session.last_error().c_str());
				++g_failures;
				break;
			}
		}
		session.finish_receive_batch(out);
		session.pump(out);
		session.process_periodic_update();
		for (const auto &dg : out) send(dg);
		verified = session.is_verified();
	}
	expect(verified, "ClientSession reached Verified against the real listener");

	if (verified) {
		using opennova::ClientVar;
		// Register a host the same way NovaWorldClient's hosting half does on the wire.
		auto host_req = opennova::make_client_host_request(
		    /*CurrentlyHosting*/ 1,
		    /*Cookie*/    {{0, "NWUID", session.server_nwuid()}},
		    /*HostSetup*/ {{0, "AppId", "28"}, {0, "LobbyName", "jop_2_consumer"},
		                   {0, "MaxPlayers", "24"}},
		    /*Host*/      {{0, "ServerName", "E2E Listen Host"}, {0, "Port", "32768"},
		                   {0, "Players", "1"}, {0, "Region", "us"}},
		    /*PlayerList*/{{0, "PlayerName", "Host"}});
		expect(session.queue_statement(host_req), "host request queued (session Verified)");
		flush_session();

		// The listener processes the request on its worker thread; poll the
		// hosted snapshot until the row appears (or time out).
		std::vector<opennova::novaworld_server::NwUdpListener::HostedSnapshot> hosted;
		for (int i = 0; i < 100 && hosted.empty(); ++i) {
			std::this_thread::sleep_for(20ms);
			hosted = listener.snapshot_hosted();
		}
		expect(hosted.size() == 1, "exactly one host registered in the snapshot");
		if (hosted.size() == 1) {
			const auto &lobby = hosted[0].lobby;
			expect(lobby.hosting, "snapshot host is hosting");
			expect(lobby.server_name == "E2E Listen Host", "snapshot ServerName matches");
			expect(lobby.host_port == 32768, "snapshot host_port matches the positive Host.Port override");
			expect(lobby.max_players == 24, "snapshot MaxPlayers matches");
			expect(lobby.game == "jop_2_consumer", "snapshot LobbyName matches");
		}

		// A ClientHostUpdate refreshes the live occupancy on the same session.
		auto upd = opennova::make_client_host_update(
		    /*Host*/      {{0, "Players", "2"}},
		    /*PlayerList*/{{0, "PlayerName", "Host"}, {1, "PlayerName", "Joiner"}});
		session.queue_statement(upd);
		flush_session();
		int players = 0;
		for (int i = 0; i < 100; ++i) {
			std::this_thread::sleep_for(20ms);
			auto snap = listener.snapshot_hosted();
			if (!snap.empty()) players = snap[0].lobby.player_count;
			if (players == 2) break;
		}
		expect(players == 2, "ClientHostUpdate refreshed the player count to 2");

		// The lobby connection is a type-2 NAPI connection on both ends: the service admits
		// only the next sequence, holds a later packet until its gap closes, and asks for a
		// permanently lost one with a 0x84 the client answers from its retained records.
		// [orig: NapiNPProtocol_HandleSessionPacket @0x626be0..0x626c3a;
		//  NapiNPProtocol_PumpRecvQueues @0x6269bb..0x6269d6; NapiNP_HandleResendList @0x623800]
		auto built = [&](const opennova::NapiMessage &statement) {
			session.queue_statement(statement);
			std::vector<std::vector<uint8_t>> out;
			session.pump(out);
			return out.empty() ? std::vector<uint8_t>{} : out.front();
		};
		auto wait_players = [&](int want) {
			int seen = 0;
			for (int i = 0; i < 100; ++i) {
				std::this_thread::sleep_for(20ms);
				const auto snap = listener.snapshot_hosted();
				if (!snap.empty()) seen = snap[0].lobby.player_count;
				if (seen == want) break;
			}
			return seen;
		};
		service_session(200); // the update's ACK
		const auto gap_datagram = built(opennova::make_client_host_update(
		    /*Host*/      {{0, "Players", "4"}},
		    /*PlayerList*/{}));
		const auto future_datagram = built(opennova::make_client_host_update(
		    /*Host*/      {{0, "Players", "5"}},
		    /*PlayerList*/{}));
		send(future_datagram);
		std::this_thread::sleep_for(100ms);
		{
			const auto snap = listener.snapshot_hosted();
			expect(!snap.empty() && snap[0].lobby.player_count == 2,
			       "a lobby packet past a gap is held, not dispatched");
		}
		send(gap_datagram);
		expect(wait_players(5) == 5, "the gap's arrival releases both packets in order");
		send(future_datagram);
		std::this_thread::sleep_for(100ms);
		{
			const auto snap = listener.snapshot_hosted();
			expect(!snap.empty() && snap[0].lobby.player_count == 5,
			       "a duplicate lobby packet is not redispatched");
		}

		// Permanently omit the next sequence, then deliver its successor: the service asks for
		// the lost one with a 0x84 and the client answers from its retained records.
		const auto permanently_lost = built(opennova::make_client_host_update(
				/*Host*/{{0, "Players", "6"}},
				/*PlayerList*/{}));
		(void)permanently_lost;
		const auto after_loss = built(opennova::make_client_host_update(
				/*Host*/{{0, "Players", "7"}},
				/*PlayerList*/{}));
		send(after_loss);
		service_session(500);
		expect(wait_players(7) == 7,
		       "a lost lobby packet is recovered through the service's 0x84");
	}

	// The NWU reconnect against the real listener: a registered host's connection drops (its own
	// reap: the 0x46 burst reaches the listener, which drops the connection and parks its lobby
	// identity), the session re-probes with the same CI, re-joins with a fresh CK counting the
	// disconnect (the 0x82 echoes RCNT 1), re-verifies and re-hosts with CurrentlyHosting=1,
	// keeping its RID and GSID. [orig: PumpStateMachine @0x629487..0x6296cb;
	//  HandleClientJoin @0x62c28d..0x62c2a3; HandleConnectVerifyResponse @0x4d5961..0x4d5978]
	{
		uint16_t recon_port = 0;
		auto recon_client = opennova::net::udp_bind(0, &recon_port);
		expect(recon_client.is_valid(), "reconnect UDP socket bound");
		auto send_recon = [&](const std::vector<uint8_t> &dg) {
			if (!dg.empty() && recon_client.is_valid())
				opennova::net::udp_send_to(recon_client, server_ep, dg.data(), dg.size());
		};
		opennova::ClientSession::Config rcfg;
		rcfg.client_index = 0x5245434Eu;
		rcfg.client_key = 0x52454331u;
		rcfg.na = "reconnect:host";
		rcfg.cookie_vars = []() {
			return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
		};
		rcfg.next_client_key = []() { return 0x52454332u; };
		opennova::ClientSession recon(rcfg);
		// Receive, dispatch and answer until `done` holds (the session clock stays put).
		auto drive = [&](auto done, int rounds) {
			for (int i = 0; i < rounds && !done(); ++i) {
				uint8_t buf[4096];
				opennova::net::Endpoint from;
				const int n = recon_client.is_valid()
						? opennova::net::udp_recv_from(recon_client, buf, sizeof buf, from, 50)
						: -1;
				std::vector<std::vector<uint8_t>> out;
				if (n > 0) recon.handle_datagram(buf, static_cast<size_t>(n), out);
				recon.finish_receive_batch(out);
				recon.pump(out);
				recon.process_periodic_update();
				for (const auto &dg : out) send_recon(dg);
			}
			return done();
		};
		auto hosted_row = [&](uint32_t &rid) {
			for (const auto &row : listener.snapshot_hosted()) {
				if (row.lobby.server_name == "Reconnect Host" && row.lobby.hosting) {
					rid = row.lobby.rid;
					return true;
				}
			}
			return false;
		};
		send_recon(recon.start());
		const bool up = drive([&] { return recon.is_verified(); }, 80);
		expect(up, "reconnect: the first connect verifies");
		opennova::HostRegistration reg;
		reg.server_name = "Reconnect Host";
		reg.app_id = 1357;
		if (up) recon.request_hosting(reg, 0);
		const bool hosting = up && drive([&] {
			return recon.host_state() == opennova::ClientSession::HostState::Established;
		}, 80);
		expect(hosting, "reconnect: the host registers");
		const std::string gsid = recon.host_gsid();
		uint32_t rid_before = 0;
		for (int i = 0; i < 50 && !hosted_row(rid_before); ++i) std::this_thread::sleep_for(20ms);
		expect(hosting && rid_before != 0 && !gsid.empty(), "reconnect: the host row is listed");

		// The session's own reap: its 0x46 burst drops the listener's connection.
		recon.set_clock_ms(240001);
		std::vector<std::vector<uint8_t>> burst;
		recon.pump(burst);
		expect(recon.reconnecting() && burst.size() == 4, "reconnect: the reap sends the 0x46 burst");
		for (const auto &dg : burst) send_recon(dg);
		uint32_t rid_dropped = 0;
		bool listed = true;
		for (int i = 0; i < 100 && listed; ++i) {
			std::this_thread::sleep_for(20ms);
			listed = hosted_row(rid_dropped);
		}
		expect(!listed, "reconnect: the listener drops the host row on the burst");

		// The re-probe: past the 1000 ms first delay the window opens and the 0x41 goes out.
		bool probed = false;
		for (uint32_t t = 240002; t < 250000 && !probed; ++t) {
			recon.set_clock_ms(t);
			std::vector<std::vector<uint8_t>> pumped;
			recon.pump(pumped);
			for (const auto &dg : pumped) send_recon(dg);
			probed = !pumped.empty();
		}
		expect(probed, "reconnect: the session re-probes");
		const bool rehosted = probed && drive([&] {
			return recon.host_state() == opennova::ClientSession::HostState::Established;
		}, 120);
		expect(rehosted, "reconnect: the session re-joins, re-verifies and re-hosts");
		expect(recon.client_key() == 0x52454332u && recon.disconnect_count() == 1 &&
		               recon.reconnect_count() == 1,
		       "reconnect: a fresh CK, DCNT 1, and the listener's 0x82 echoed RCNT 1");
		expect(recon.host_reconnect_counter() == 1, "reconnect: the re-host counted ReconnectCounter 1");
		expect(recon.host_gsid() == gsid, "reconnect: the re-host keeps its GSID");
		uint32_t rid_after = 0;
		for (int i = 0; i < 50 && !hosted_row(rid_after); ++i) std::this_thread::sleep_for(20ms);
		expect(rid_after == rid_before, "reconnect: the re-host keeps its RID");
		// Leave cleanly so the later checks see only the first host.
		send_recon(recon.build_goodbye());
		uint32_t rid_left = 0;
		bool still = true;
		for (int i = 0; i < 100 && still; ++i) {
			std::this_thread::sleep_for(20ms);
			still = hosted_row(rid_left);
		}
		expect(!still, "reconnect: the goodbye drops the re-hosted row");
		if (recon_client.is_valid()) opennova::net::close_socket(recon_client);
	}

	// Both retail processes report CI=1. The second endpoint therefore gets a
	// synthetic registry id. A later byte-identical Hello from that endpoint is
	// a pre-session retransmit, not a replacement merely because raw CI=1 is
	// different from the synthetic id assigned by the registry.
	{
		uint16_t collision_port = 0;
		auto collision_client = opennova::net::udp_bind(0, &collision_port);
		expect(collision_client.is_valid(), "CI-collision UDP socket bound");
		auto send_collision = [&](const std::vector<uint8_t> &dg) {
			if (!dg.empty()) {
				opennova::net::udp_send_to(
						collision_client, server_ep, dg.data(), dg.size());
			}
		};
		auto receive_collision = [&](std::vector<uint8_t> &dg) {
			opennova::net::Endpoint from;
			const int n = opennova::net::udp_recv_from(
					collision_client, rx, sizeof rx, from, 1000);
			if (n <= 0) return false;
			dg.assign(rx, rx + n);
			return true;
		};

		opennova::ClientSession::Config collision_cfg;
		collision_cfg.client_index = 1;
		collision_cfg.client_key = 0x76543210u;
		collision_cfg.na = "collision:second-peer";
		opennova::ClientSession collision(collision_cfg);
		const std::vector<uint8_t> repeated_hello = collision.start();
		send_collision(repeated_hello);

		std::vector<uint8_t> inbound;
		std::vector<std::vector<uint8_t>> out;
		const bool got_hello = receive_collision(inbound);
		expect(got_hello, "CI-collision peer receives ServerHello");
		if (got_hello) {
			expect(collision.handle_datagram(
					       inbound.data(), inbound.size(), out) &&
			               out.size() == 1,
			       "CI-collision ServerHello produces ClientAuth");
		}
		const std::vector<uint8_t> repeated_auth =
				out.empty() ? std::vector<uint8_t>{} : out.front();
		send_collision(repeated_auth);

		std::vector<uint8_t> first_server_auth;
		const bool got_auth = receive_collision(first_server_auth);
		expect(got_auth, "CI-collision peer receives ServerAuth");
		out.clear();
		if (got_auth) {
			expect(collision.handle_datagram(
					       first_server_auth.data(), first_server_auth.size(), out),
			       "CI-collision peer accepts ServerAuth");
		}

		const opennova::PeerAddr collision_peer{
				0x0100007Fu, collision_port};
		const auto first_registry =
				manager.registry().find_by_addr(collision_peer);
		expect(first_registry &&
		               first_registry->id != collision_cfg.client_index &&
		               first_registry->state == opennova::ConnectionState::Active,
		       "second raw-CI=1 peer owns a synthetic active registry identity");

		send_collision(repeated_hello);
		inbound.clear();
		expect(receive_collision(inbound),
		       "synthetic-id peer's repeated Hello receives ServerHello");
		send_collision(repeated_auth);
		std::vector<uint8_t> replayed_server_auth;
		const bool got_replayed_auth =
				receive_collision(replayed_server_auth);
		expect(got_replayed_auth &&
		               replayed_server_auth == first_server_auth,
		       "repeated Hello preserves cached byte-identical ServerAuth");
		const auto after_repeated_hello =
				manager.registry().find_by_addr(collision_peer);
		expect(first_registry && after_repeated_hello &&
		               after_repeated_hello->id == first_registry->id &&
		               after_repeated_hello->state ==
				               opennova::ConnectionState::Active,
		       "repeated Hello preserves the registry-assigned synthetic identity");

		send_collision(collision.build_goodbye());
		opennova::net::close_socket(collision_client);
	}

	// ClientAuth is retransmitted when ServerAuth is delayed. The listener must
	// replay the exact same ServerAuth (SK/SCRK included) and preserve the
	// already-admitted lobby sequence frontier. Otherwise a delayed first
	// ServerAuth can install stale keys, or the retry can rewind the server's
	// outbound sequence so the client drops ServerVerifyResult as a duplicate.
	// A different auth on the same endpoint is a new logical session and must
	// reset that frontier so its sequence-one ClientConnected is admitted.
	{
		uint16_t retry_client_port = 0;
		auto retry_client = opennova::net::udp_bind(0, &retry_client_port);
		expect(retry_client.is_valid(), "ClientAuth retry UDP socket bound");

		auto send_retry = [&](const std::vector<uint8_t> &dg) {
			if (!dg.empty()) {
				opennova::net::udp_send_to(
						retry_client, server_ep, dg.data(), dg.size());
			}
		};
		auto receive_retry = [&](std::vector<uint8_t> &dg) {
			opennova::net::Endpoint from;
			const int n = opennova::net::udp_recv_from(
					retry_client, rx, sizeof rx, from, 1000);
			if (n <= 0) return false;
			dg.assign(rx, rx + n);
			return true;
		};

		opennova::ClientSession::Config retry_cfg;
		retry_cfg.client_index = 0x13572468u;
		retry_cfg.client_key = 0x24681357u;
		retry_cfg.na = "retry:stable-auth";
		opennova::ClientSession retry_session(retry_cfg);

		send_retry(retry_session.start());
		std::vector<uint8_t> inbound;
		std::vector<std::vector<uint8_t>> out;
		const bool got_retry_hello = receive_retry(inbound);
		expect(got_retry_hello, "ClientAuth retry receives ServerHello");
		if (got_retry_hello) {
			expect(retry_session.handle_datagram(
					       inbound.data(), inbound.size(), out) &&
			               out.size() == 1,
			       "ClientAuth retry ServerHello produces one ClientAuth");
		}
		const std::vector<uint8_t> client_auth =
				out.empty() ? std::vector<uint8_t>{} : out.front();
		send_retry(client_auth);

		std::vector<uint8_t> first_server_auth;
		const bool got_first_server_auth = receive_retry(first_server_auth);
		expect(got_first_server_auth, "ClientAuth retry receives first ServerAuth");
		out.clear();
		if (got_first_server_auth) {
			expect(retry_session.handle_datagram(
					       first_server_auth.data(), first_server_auth.size(), out) &&
			               retry_session.state() ==
					               opennova::ClientSession::State::Verifying,
			       "first ServerAuth installs retry session material");
		}
		retry_session.process_periodic_update();
		retry_session.pump(out);
		expect(out.size() == 1,
		       "retry session emits sequence-one ClientConnected");
		if (out.size() == 1) send_retry(out.front());

		std::vector<uint8_t> first_start_verify;
		const bool got_first_start_verify = receive_retry(first_start_verify);
		expect(got_first_start_verify,
		       "retry session receives sequence-one ServerStartVerify");

		// Re-send the byte-identical ClientAuth after sequence one was admitted,
		// while retaining the first verify reply for delayed delivery.
		send_retry(client_auth);
		std::vector<uint8_t> replayed_server_auth;
		const bool got_replayed_server_auth = receive_retry(replayed_server_auth);
		expect(got_replayed_server_auth,
		       "identical ClientAuth retry receives cached ServerAuth");
		expect(got_replayed_server_auth &&
		               replayed_server_auth == first_server_auth,
		       "identical ClientAuth retry replays byte-identical session material");

		out.clear();
		if (got_first_start_verify) {
			expect(retry_session.handle_datagram(
					       first_start_verify.data(), first_start_verify.size(), out) &&
			               out.empty(),
			       "delayed ServerStartVerify queues the verify request");
			retry_session.pump(out);
			expect(out.size() == 1, "delayed ServerStartVerify produces sequence-two verify request");
		}
		if (out.size() == 1) send_retry(out.front());
		inbound.clear();
		const bool got_verify_result = receive_retry(inbound);
		expect(got_verify_result,
		       "retry session receives ServerVerifyResult after repeated auth");
		out.clear();
		if (got_verify_result) {
			expect(retry_session.handle_datagram(
					       inbound.data(), inbound.size(), out),
			       "retry session accepts ServerVerifyResult");
		}
		expect(retry_session.is_verified(),
		       "identical ClientAuth retry preserves admitted sequencing");

		// A distinct CI/CK/auth body on the same UDP endpoint is re-auth, not a
		// retransmission. Its fresh sequence-one exchange must succeed.
		opennova::ClientSession::Config replacement_cfg;
		replacement_cfg.client_index = 0x89ABCDEFu;
		replacement_cfg.client_key = 0x10293847u;
		replacement_cfg.na = "retry:new-auth";
		opennova::ClientSession replacement(replacement_cfg);
		send_retry(replacement.start());
		inbound.clear();
		out.clear();
		const bool got_replacement_hello = receive_retry(inbound);
		expect(got_replacement_hello, "replacement auth receives ServerHello");
		if (got_replacement_hello) {
			expect(replacement.handle_datagram(
					       inbound.data(), inbound.size(), out) &&
			               out.size() == 1,
			       "replacement ServerHello produces distinct ClientAuth");
		}
		if (out.size() == 1) send_retry(out.front());
		std::vector<uint8_t> replacement_server_auth;
		const bool got_replacement_auth =
				receive_retry(replacement_server_auth);
		expect(got_replacement_auth,
		       "replacement auth receives fresh ServerAuth");
		expect(got_replacement_auth &&
		               replacement_server_auth != first_server_auth,
		       "genuinely new auth receives new session material");

		out.clear();
		if (got_replacement_auth) {
			expect(replacement.handle_datagram(
					       replacement_server_auth.data(),
					       replacement_server_auth.size(), out),
			       "replacement accepts fresh ServerAuth");
		}
		replacement.process_periodic_update();
		replacement.pump(out);
		expect(out.size() == 1,
		       "replacement emits fresh sequence-one ClientConnected");
		if (out.size() == 1) send_retry(out.front());
		inbound.clear();
		out.clear();
		const bool got_replacement_start_verify = receive_retry(inbound);
		expect(got_replacement_start_verify,
		       "new auth reset admits replacement ClientConnected");
		if (got_replacement_start_verify) {
			expect(replacement.handle_datagram(
					       inbound.data(), inbound.size(), out) &&
			               out.empty(),
			       "replacement ServerStartVerify queues the verify request");
			replacement.pump(out);
			expect(out.size() == 1, "replacement ServerStartVerify produces verify request");
		}
		if (out.size() == 1) send_retry(out.front());
		inbound.clear();
		out.clear();
		const bool got_replacement_verify = receive_retry(inbound);
		expect(got_replacement_verify,
		       "replacement receives ServerVerifyResult");
		if (got_replacement_verify) {
			expect(replacement.handle_datagram(
					       inbound.data(), inbound.size(), out),
			       "replacement accepts ServerVerifyResult");
		}
		expect(replacement.is_verified(),
		       "genuinely new endpoint auth resets admitted sequencing");

		// A stock client that dies without its goodbye and restarts within the reap
		// window comes back on the SAME endpoint (its client port range starts at the
		// same port) with the SAME CI and a fresh CK. Its Hello finds the still-Active
		// row; its 0x42 is a new connection, not a retransmit, and must be answered:
		// retail's join handler looks the connection up by address and replaces any
		// node that is not the same CI+CK retransmit, with no Hello state required.
		// [orig: NapiNPProtocol_HandleClientJoin @0x62beba..0x62bf06 (FindConnection,
		//  the same-CI/CK SendSessionInit retransmit, else Destroy + Create)]
		opennova::ClientSession::Config restarted_cfg = replacement_cfg;
		restarted_cfg.client_key = 0x5EB007EDu;
		opennova::ClientSession restarted(restarted_cfg);
		send_retry(restarted.start());
		inbound.clear();
		out.clear();
		const bool got_restarted_hello = receive_retry(inbound);
		expect(got_restarted_hello, "restarted same-CI client receives ServerHello");
		if (got_restarted_hello) {
			expect(restarted.handle_datagram(inbound.data(), inbound.size(), out) &&
			               out.size() == 1,
			       "restarted ServerHello produces a fresh-CK ClientAuth");
		}
		if (out.size() == 1) send_retry(out.front());
		std::vector<uint8_t> restarted_server_auth;
		const bool got_restarted_auth = receive_retry(restarted_server_auth);
		expect(got_restarted_auth,
		       "a restarted same-CI client's ClientAuth replaces the Active row and is answered");
		out.clear();
		if (got_restarted_auth) {
			expect(restarted.handle_datagram(restarted_server_auth.data(),
			                                 restarted_server_auth.size(), out) &&
			               restarted.state() == opennova::ClientSession::State::Verifying,
			       "the restarted client installs fresh session material");
		}

		send_retry(restarted.build_goodbye());
		send_retry(replacement.build_goodbye());
		opennova::net::close_socket(retry_client);
	}

	// The same real listener also owns the JO game-session responder. Drop the joiner's first
	// post-auth 0x43, deliver sequence two, and require the listener's actual socket receive-batch
	// boundary to emit the retail server 0x84 requesting sequence one.
	uint32_t first_jo_host_key = 0;
	uint16_t first_jo_client_port = 0;
	{
		uint16_t jo_client_port = 0;
		auto jo_client = opennova::net::udp_bind(0, &jo_client_port);
		first_jo_client_port = jo_client_port;
		expect(jo_client.is_valid(), "JO loss-probe UDP socket bound");
		opennova::inmatch::JoinerConnection joiner("E2ELossProbe");

		auto send_jo = [&](const std::vector<uint8_t> &dg) {
			if (!dg.empty()) {
				opennova::net::udp_send_to(
						jo_client, server_ep, dg.data(), dg.size());
			}
		};
		auto receive_jo = [&](std::vector<uint8_t> &dg) {
			opennova::net::Endpoint from;
			const int n = opennova::net::udp_recv_from(
					jo_client, rx, sizeof rx, from, 1000);
			if (n <= 0) return false;
			dg.assign(rx, rx + n);
			return true;
		};

		send_jo(joiner.start());
		std::vector<uint8_t> inbound;
		const bool got_server_hello = receive_jo(inbound);
		expect(got_server_hello, "JO loss probe receives ServerHello");
		if (got_server_hello) {
			uint8_t hello_opcode = 0;
			std::vector<uint8_t> hello_body;
			opennova::ServerHello server_hello;
			expect(opennova::nw_decode_inbound(
			               inbound.data(), inbound.size(), hello_opcode, hello_body) &&
			               hello_opcode == opennova::SESSION_OPCODE_SERVER_HELLO &&
			               opennova::parse_server_hello(
			                       hello_body.data(), hello_body.size(), server_hello) &&
			               server_hello.hk != 0,
			       "standalone JO ServerHello advertises a nonzero production host key");
			first_jo_host_key = server_hello.hk;
			auto hello = joiner.handle_datagram(inbound.data(), inbound.size());
			const bool has_client_auth = hello.outbound.size() == 1;
			expect(has_client_auth, "JO ServerHello produces ClientAuth");
			if (has_client_auth) {
				send_jo(hello.outbound[0]);
			}
		}
		inbound.clear();
		const bool got_server_auth = receive_jo(inbound);
		expect(got_server_auth, "JO loss probe receives ServerAuth");
		if (got_server_auth) {
			auto auth = joiner.handle_datagram(inbound.data(), inbound.size());
			// The settings-gated joiner speaks only after the host's initial settings
			// packet, which rides its own datagram behind the 0x82 (net-re §5.0d f6-8).
			expect(auth.outbound.empty(),
			       "JO ServerAuth alone produces no sequenced request");
		}

		inbound.clear();
		std::vector<uint8_t> dropped_first;
		std::vector<uint8_t> second;
		const bool got_settings = receive_jo(inbound);
		expect(got_settings, "JO loss probe receives the initial settings packet");
		if (got_settings) {
			auto settings = joiner.handle_datagram(inbound.data(), inbound.size());
			const bool has_ack_join_pair = settings.outbound.size() == 2;
			expect(has_ack_join_pair,
			       "JO settings packet produces the header-ACK + JOIN pair");
			if (has_ack_join_pair) {
				dropped_first = settings.outbound[0]; // seq 1 header-ACK: deliberately dropped
				second = settings.outbound[1];        // seq 2 C2S 0x00 JOIN: delivered
			}
		}
		expect(!dropped_first.empty() && !second.empty(),
		       "JO loss probe frames dropped sequence one and delivered sequence two");

		// A delayed goodbye from a prior occupant of this endpoint is rejected
		// by the JO session verifier. The socket owner must keep its JO route
		// and spawn bookkeeping unless that verifier surfaces PeerGoodbye.
		const std::vector<uint8_t> stale_jo_goodbye =
				opennova::nw_encode_outbound(
						opennova::SESSION_OPCODE_CLIENT_GOODBYE,
						opennova::client_goodbye_to_bytes(
								joiner.server_key() ^ 0x01010101u));
		send_jo(stale_jo_goodbye);
		std::this_thread::sleep_for(20ms);
		send_jo(second);

		inbound.clear();
		const bool got_missing_response = receive_jo(inbound);
		expect(got_missing_response,
		       "real listener emits a missing-sequence response after FIFO drain");
		if (got_missing_response) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			std::vector<uint32_t> requested;
			expect(opennova::nw_decode_inbound(
			               inbound.data(), inbound.size(), opcode, body) &&
			               opcode == opennova::SESSION_OPCODE_SERVER_RESEND_LIST &&
			               opennova::decode_session_resend_list(
			                       body.data(), body.size(), joiner.client_key(),
			                       requested) &&
			               requested == std::vector<uint32_t>({1}),
			       "real listener 0x84 requests the missing first C2S sequence");

			// The resend handler transmits the rebuilt packet itself, from the
			// receive pump [orig: NapiNP_HandleResendList ->
			// CNapiNPConnection_SendSessionPacket @0x6239b6].
			auto resend = joiner.handle_datagram(inbound.data(), inbound.size());
			const bool has_reconstructed =
					resend.immediate_outbound.size() == 1 && resend.outbound.empty();
			expect(has_reconstructed,
			       "joiner reconstructs one packet for the listener's 0x84");
			if (has_reconstructed) {
				uint8_t resend_opcode = 0;
				std::vector<uint8_t> resend_body;
				opennova::ProtocolPacketHeader resend_header;
				std::vector<opennova::ProtocolMessage> resend_messages;
				expect(opennova::nw_decode_inbound(
				               resend.immediate_outbound[0].data(),
				               resend.immediate_outbound[0].size(),
				               resend_opcode, resend_body) &&
				               resend_opcode == opennova::SESSION_OPCODE_PROTOCOL_MESSAGE &&
				               opennova::decode_protocol_packet_plaintext(
				                       resend_body.data(), resend_body.size(),
				                       joiner.connection().client_scrk,
				                       resend_header, resend_messages) &&
				               resend_header.seq_num == 1,
				       "listener NACK reconstructs C2S sequence one under its old number");
				send_jo(resend.immediate_outbound[0]);
			}
		}
		opennova::net::close_socket(jo_client);
	}

	// A standalone JO connection must run the same complete host lifecycle as
	// the dedicated game host. The named pool-0 0x0C in that initial-state
	// stream is the joiner's only way to discover its own entity handle.
	{
		uint16_t live_client_port = 0;
		auto live_client = opennova::net::udp_bind(0, &live_client_port);
		expect(live_client.is_valid(), "standalone-spawn UDP socket bound");
		opennova::inmatch::ClientRuntime live_joiner("E2EStandaloneSpawn");

		auto send_live = [&](const std::vector<uint8_t> &dg) {
			if (!dg.empty()) {
				opennova::net::udp_send_to(
						live_client, server_ep, dg.data(), dg.size());
			}
		};
		send_live(live_joiner.start());

		uint32_t live_tick = 1;
		for (int frame = 0;
		     frame < 800 && !live_joiner.has_self_handle();
		     ++frame, ++live_tick) {
			opennova::net::Endpoint from;
			const int n = opennova::net::udp_recv_from(
					live_client, rx, sizeof rx, from, 5);
			if (n > 0) {
				live_joiner.receive(rx, static_cast<size_t>(n));
			}
			for (const auto &dg :
			     live_joiner.Client_ProcessNetworkFrame(live_tick)) {
				send_live(dg);
			}
		}
		expect(live_joiner.has_self_handle(),
		       "standalone JO host streams a named pool-0 spawn for self discovery");
		opennova::net::close_socket(live_client);
	}

	// A correctly encrypted lobby SESSION addressed to some other receiver
	// key is not activity on this connection. It must be rejected before the
	// matchmaking liveness timestamp is refreshed.
	const opennova::PeerAddr lobby_peer{0x0100007Fu, client_port};
	const auto before_wrong_session =
			manager.registry().find_by_addr(lobby_peer);
	expect(before_wrong_session.has_value(),
	       "live lobby connection is present before wrong-key SESSION");
	// A header-only packet under the session's own SCRK, addressed to a foreign receiver key.
	opennova::ProtocolPacketHeader heartbeat_header;
	heartbeat_header.session_id = session.server_key() ^ 0x01010101u;
	heartbeat_header.seq_num = 1;
	std::vector<uint8_t> wrong_session_body;
	const bool encoded_wrong_session =
			opennova::encode_protocol_packet_plaintext(
					heartbeat_header, {}, session.client_scrk(), wrong_session_body);
	expect(encoded_wrong_session,
	       "wrong-key liveness probe re-encodes its foreign receiver key");
	std::this_thread::sleep_for(20ms);
	if (encoded_wrong_session) {
		send(opennova::nw_encode_outbound(
				opennova::SESSION_OPCODE_PROTOCOL_MESSAGE,
				std::move(wrong_session_body)));
	}
	std::this_thread::sleep_for(100ms);
	const auto after_wrong_session =
			manager.registry().find_by_addr(lobby_peer);
	expect(before_wrong_session && after_wrong_session &&
	               after_wrong_session->last_seen_ms ==
			               before_wrong_session->last_seen_ms,
	       "wrong-key lobby SESSION does not refresh liveness");

	// A stale/foreign 0x46 must not evict this addr-keyed live session. The
	// receiver validates the leading remote-session key before teardown.
	const auto stale_goodbye = opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_CLIENT_GOODBYE,
			opennova::client_goodbye_to_bytes(session.server_key() ^ 0x01010101u));
	send(stale_goodbye);
	std::this_thread::sleep_for(100ms);
	expect(listener.snapshot_hosted().size() == 1,
	       "wrong-key ClientGoodbye cannot remove the live lobby session");

	// A valid lobby Hello owns a ConnectionManager entry before Auth creates
	// LobbyConnState. Keep that endpoint at Hello-only across stop/start so the
	// regression exercises the exact state the old lobby_states_-only sweep
	// leaked. Also seed an unrelated entry in the shared manager: listener
	// teardown must remove its own exact addresses without calling shutdown().
	uint16_t hello_only_port = 0;
	auto hello_only_client =
			opennova::net::udp_bind(0, &hello_only_port);
	expect(hello_only_client.is_valid(), "Hello-only lobby UDP socket bound");
	opennova::ClientSession::Config hello_only_cfg;
	hello_only_cfg.client_index = 0x48454C4Fu;
	hello_only_cfg.client_key = 0x4F4E4C59u;
	hello_only_cfg.na = "restart:hello-only";
	opennova::ClientSession hello_only_session(hello_only_cfg);
	const std::vector<uint8_t> hello_only_datagram =
			hello_only_session.start();
	if (hello_only_client.is_valid()) {
		opennova::net::udp_send_to(
				hello_only_client, server_ep,
				hello_only_datagram.data(), hello_only_datagram.size());
	}
	opennova::net::Endpoint hello_only_from;
	const int hello_only_reply_size =
			hello_only_client.is_valid()
					? opennova::net::udp_recv_from(
							hello_only_client, rx, sizeof rx,
							hello_only_from, 1000)
					: -1;
	expect(hello_only_reply_size > 0,
	       "valid Hello-only lobby peer receives ServerHello");
	const opennova::PeerAddr hello_only_peer{
			0x0100007Fu, hello_only_port};
	std::optional<opennova::Connection> first_hello_only;
	for (int i = 0; i < 100 && !first_hello_only; ++i) {
		first_hello_only =
				manager.registry().find_by_addr(hello_only_peer);
		if (!first_hello_only) std::this_thread::sleep_for(10ms);
	}
	expect(first_hello_only &&
	               first_hello_only->state ==
			               opennova::ConnectionState::Handshaking,
	       "Hello-only lobby peer is tracked before Auth");
	const uint64_t first_hello_created_ms =
			first_hello_only ? first_hello_only->created_ms : 0;

	const opennova::PeerAddr shared_manager_peer{
			0x0100000Au, 45678};
	opennova::Connection shared_manager_connection;
	shared_manager_connection.id = 0x53484152u;
	shared_manager_connection.reported_id =
			shared_manager_connection.id;
	shared_manager_connection.addr = shared_manager_peer;
	shared_manager_connection.state =
			opennova::ConnectionState::Handshaking;
	shared_manager_connection.created_ms = 1;
	shared_manager_connection.last_seen_ms = 1;
	shared_manager_connection.pn = "SHARED-OWNER";
	manager.notify_handshake(shared_manager_connection);
	expect(manager.registry().find_by_addr(shared_manager_peer).has_value(),
	       "shared manager contains an unrelated peer owned elsewhere");

	send(session.build_goodbye());
	bool goodbye_removed_host = false;
	for (int i = 0; i < 100; ++i) {
		std::this_thread::sleep_for(20ms);
		if (listener.snapshot_hosted().empty()) {
			goodbye_removed_host = true;
			break;
		}
	}
	expect(goodbye_removed_host,
	       "ServerAuth.SK-keyed ClientGoodbye removes the lobby session");
	opennova::net::close_socket(client);
	listener.stop();
	expect(!manager.registry().find_by_addr(hello_only_peer) &&
	               listener.snapshot_hosted().empty(),
	       "listener stop clears its valid Hello-only peer and lobby auth state");
	expect(manager.registry().size() == 1 &&
	               manager.registry().find_by_addr(shared_manager_peer),
	       "listener stop preserves unrelated entries in a shared manager");

	// Restart the same listener and reuse the prior JO endpoint. A new
	// production host must rotate HK and accept a fresh handshake; retaining
	// jo_ctx/route/auth state would either replay the fixed key or reject this
	// new ClientAuth as belonging to the old run.
	uint16_t rebound_port = 0;
	auto restart_client =
			opennova::net::udp_bind(first_jo_client_port, &rebound_port);
	expect(restart_client.is_valid() &&
	               rebound_port == first_jo_client_port,
	       "listener restart reuses the prior JO client endpoint");
	const bool restarted = listener.start(config);
	expect(restarted, "listener starts again after a complete stop");
	if (restarted && hello_only_client.is_valid()) {
		opennova::ClientSession fresh_hello_only(hello_only_cfg);
		const std::vector<uint8_t> fresh_hello =
				fresh_hello_only.start();
		opennova::net::udp_send_to(
				hello_only_client, server_ep,
				fresh_hello.data(), fresh_hello.size());
		opennova::net::Endpoint from;
		const int fresh_hello_size = opennova::net::udp_recv_from(
				hello_only_client, rx, sizeof rx, from, 1000);
		expect(fresh_hello_size > 0,
		       "same-endpoint lobby Hello succeeds after listener restart");
		const auto fresh_registry =
				manager.registry().find_by_addr(hello_only_peer);
		expect(fresh_registry &&
		               fresh_registry->state ==
				               opennova::ConnectionState::Handshaking &&
		               fresh_registry->created_ms > first_hello_created_ms,
		       "restart creates fresh manager state for the Hello-only endpoint");
		expect(manager.registry().find_by_addr(shared_manager_peer).has_value(),
		       "listener restart preserves the shared manager's unrelated peer");
	}
	if (restarted && restart_client.is_valid()) {
		opennova::inmatch::JoinerConnection restart_joiner("E2ERestartProbe");
		const auto restart_hello = restart_joiner.start();
		opennova::net::udp_send_to(
				restart_client, server_ep,
				restart_hello.data(), restart_hello.size());

		opennova::net::Endpoint from;
		const int hello_size = opennova::net::udp_recv_from(
				restart_client, rx, sizeof rx, from, 1000);
		expect(hello_size > 0,
		       "fresh same-endpoint JO Hello succeeds after listener restart");
		if (hello_size > 0) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			opennova::ServerHello hello;
			const bool decoded =
					opennova::nw_decode_inbound(
							rx, static_cast<size_t>(hello_size), opcode, body) &&
					opcode == opennova::SESSION_OPCODE_SERVER_HELLO &&
					opennova::parse_server_hello(
							body.data(), body.size(), hello);
			expect(decoded && hello.hk != 0 &&
			               hello.hk != first_jo_host_key,
			       "listener restart rotates the production JO host key");

			auto handled = restart_joiner.handle_datagram(
					rx, static_cast<size_t>(hello_size));
			expect(handled.outbound.size() == 1,
			       "restarted JO ServerHello produces fresh ClientAuth");
			if (handled.outbound.size() == 1) {
				opennova::net::udp_send_to(
						restart_client, server_ep,
						handled.outbound[0].data(),
						handled.outbound[0].size());
				const int auth_size = opennova::net::udp_recv_from(
						restart_client, rx, sizeof rx, from, 1000);
				expect(auth_size > 0,
				       "restarted JO host accepts fresh same-endpoint ClientAuth");
			}
		}
	}
	opennova::net::close_socket(restart_client);
	listener.stop();
	expect(!manager.registry().find_by_addr(hello_only_peer),
	       "second listener stop clears the restarted Hello-only peer");
	expect(manager.registry().find_by_addr(shared_manager_peer).has_value(),
	       "second listener stop still preserves the unrelated shared peer");
	manager.notify_logout_addr(shared_manager_peer);
	expect(manager.registry().size() == 0,
	       "test cleanup removes the unrelated shared-manager peer explicitly");
	opennova::net::close_socket(hello_only_client);
	opennova::net::shutdown();

	if (g_failures == 0) {
		std::printf("OK: host registered + updated through the real NwUdpListener\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
