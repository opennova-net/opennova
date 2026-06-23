#include "nw_udp_listener.h"

#include "server_config.h"

#include <net_sockets.h>
#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <novaworld/connection/manager.h>
#include <novaworld/db/sqlite.h>
#include <novaworld/host_repository.h>
#include <novaworld/lobby_session.h>
#include <novaworld/nw_session_framing.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>
#include <novaworld/session_protocol.h>
#include <novaworld/unknown_tracker.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::server {

namespace {

uint64_t now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
		duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// "0x41"-style signature for the unknown tracker. Width follows the value so
// a 1-byte opcode reads "0x41" and a wider tag reads "0x0123".
std::string hex_sig(uint32_t value, int min_width = 2) {
	char buf[16];
	std::snprintf(buf, sizeof(buf), "0x%0*x", min_width, value);
	return buf;
}

// Pack the IP so the LE-serialized uint32 (TLV-encoded by
// server_hello_to_bytes / server_auth_to_bytes) spells out the IP
// octets a.b.c.d in memory order — LSB = first octet. Retail's TLV
// reader treats those four bytes positionally as octets, so packing
// the OTHER way (big-endian) makes retail show "1.0.0.127" instead of
// "127.0.0.1" in `_connectlog.txt` (verified 2026-04-27).
//
// The "network byte order" comment in session_hello.h was misleading;
// the value goes onto the wire as plain LE — what matters is how the
// receiver interprets the four payload bytes as octets.
uint32_t ip_to_le(const std::array<uint8_t, 4> &ip) {
	return uint32_t(ip[0])
	     | (uint32_t(ip[1]) <<  8)
	     | (uint32_t(ip[2]) << 16)
	     | (uint32_t(ip[3]) << 24);
}

// make_dev_scrk / make_dev_nwuid / make_random_session_u32 and the NW-UDP
// envelope transform (nw_decode_inbound / nw_encode_outbound) moved to
// libs/novaworld nw_session_framing.h so the standalone server's lobby path and
// the consolidated HostSessionAccept share exactly one definition.

struct MaintenanceStatus {
	bool enabled = false;
	std::string message = "NovaWorld is temporarily unavailable.";
};

MaintenanceStatus load_maintenance_status(opennova::db::Database *db) {
	MaintenanceStatus status;
	if (!db) return status;
	try {
		auto rows = db->query(
			"SELECT maintenance_enabled, message "
			"FROM server_status WHERE id = 1 LIMIT 1;");
		if (!rows.empty()) {
			status.enabled = rows.front().as_int(0).value_or(0) != 0;
			status.message = rows.front().as_text(1).value_or(status.message);
			if (status.message.empty()) {
				status.message = "NovaWorld is temporarily unavailable.";
			}
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[nwudp] WARN server_status lookup failed: %s\n", e.what());
	}
	return status;
}

void set_field(NapiMessage &msg, const std::string &name, const std::string &value) {
	msg.fields.push_back({name, std::vector<uint8_t>(value.begin(), value.end())});
}

NapiMessage make_server_verify_failure(const std::string &message) {
	NapiMessage reply;
	reply.name = "ServerVerifyResult";
	set_field(reply, "Success", "0");
	set_field(reply, "MsgCode", "3000");
	set_field(reply, "MsgParam1", "0");
	set_field(reply, "MsgParam2", "0");
	set_field(reply, "Message", message);
	NapiMessage var_list;
	var_list.name = "ServerVarList";
	set_field(var_list, "VarList", "ConnectCommands");
	reply.children.push_back(std::move(var_list));
	return reply;
}

} // namespace

NwUdpListener::NwUdpListener(ConnectionManager &manager) : manager_(manager) {}

NwUdpListener::~NwUdpListener() { stop(); }

bool NwUdpListener::start(const ServerConfig &config) {
	if (running_.load()) return true;

	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "[nwudp] net::startup failed\n");
		return false;
	}

	uint16_t bound = 0;
	auto sock = opennova::net::udp_bind(config.nw_udp_port, &bound);
	if (!sock.is_valid()) {
		std::fprintf(stderr, "[nwudp] failed to bind UDP %u\n",
		             static_cast<unsigned>(config.nw_udp_port));
		return false;
	}
	opennova::net::close_socket(sock);
	bound_port_ = config.nw_udp_port;

	stop_requested_.store(false);
	accept_.start();
	running_.store(true);
	worker_ = std::thread([this] { run_loop(); });
	std::printf("[nwudp] listening on UDP :%u\n",
	            static_cast<unsigned>(bound_port_));
	return true;
}

void NwUdpListener::stop() {
	stop_requested_.store(true);
	if (worker_.joinable()) worker_.join();
	accept_.stop();
	running_.store(false);
}

void NwUdpListener::erase_lobby_state(const PeerAddr &peer, const char *reason) {
	bool was_hosting = false;
	uint32_t rid = 0;
	{
		std::lock_guard<std::mutex> lk(lobby_states_mu_);
		auto it = lobby_states_.find(peer);
		if (it == lobby_states_.end()) return;
		was_hosting = it->second.lobby.hosting;
		rid = it->second.lobby.rid;
		if (was_hosting) {
			std::printf("[lobby] stopped addr=%u.%u.%u.%u:%u rid=%u server_name='%s' reason=%s\n",
			             peer.ip        & 0xff, (peer.ip >>  8) & 0xff,
			            (peer.ip >> 16) & 0xff, (peer.ip >> 24) & 0xff,
			            peer.port, it->second.lobby.rid,
			            it->second.lobby.server_name.c_str(), reason);
		}
		lobby_states_.erase(it);
	}

	// Phase I.2: drop the matching row(s) from active_hosts +
	// host_players. Hosting peer is keyed by RID (drops the host row);
	// joining peer's own host_players entry is keyed by peer addr.
	if (db_) {
		try {
			// PeerAddr.ip is LE (ip_to_le) — read low->high so the dotted
			// string matches the peer_ip stored at add_player time (built
			// from the network-order octets), or remove_player_by_peer won't
			// match and host_players rows leak until the host row cascades.
			char ip_buf[32];
			std::snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
			               peer.ip        & 0xff, (peer.ip >>  8) & 0xff,
			              (peer.ip >> 16) & 0xff, (peer.ip >> 24) & 0xff);
			if (was_hosting && rid != 0) {
				hostdb::remove_host_by_rid(*db_, rid);
			}
			hostdb::remove_player_by_peer(*db_, ip_buf, peer.port);
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[lobby] WARN db cleanup on erase_lobby_state: %s\n", e.what());
		}
	}
}

std::vector<NwUdpListener::HostedSnapshot> NwUdpListener::snapshot_hosted() const {
	std::vector<HostedSnapshot> out;
	std::lock_guard<std::mutex> lk(lobby_states_mu_);
	out.reserve(lobby_states_.size());
	for (const auto &[peer, state] : lobby_states_) {
		if (state.lobby.hosting) {
			// connection_id field still surfaces the client's CI for
			// diagnostics — it's unique within a process but NOT across
			// processes (two retail instances both report CI=1).
			(void)peer;
			out.push_back({state.client_ck, state.lobby});
		}
	}
	return out;
}

void NwUdpListener::run_loop() {
	opennova::net::ScopedSocket socket(opennova::net::udp_bind(bound_port_));
	if (!socket.is_valid()) {
		std::fprintf(stderr, "[nwudp] re-bind failed; aborting loop\n");
		accept_.stop();
		running_.store(false);
		return;
	}

	uint8_t rx[4096];
	while (!stop_requested_.load()) {
		opennova::net::Endpoint from{};
		const int n = opennova::net::udp_recv_from(socket.get(), rx, sizeof(rx),
		                                           from, /*timeout_ms=*/250);
		if (n <= 0) continue;

		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(rx, static_cast<size_t>(n), opcode, body)) {
			std::fprintf(stderr, "[nwudp] %s:%u — bad envelope (%d bytes)\n",
			             opennova::net::endpoint_to_string(from).c_str(),
			             from.port, n);
			continue;
		}

		const PeerAddr peer{ip_to_le(from.ip), from.port};
		const uint32_t client_ip_net = peer.ip;
		const uint16_t client_port = from.port;
		const auto client_label = opennova::net::endpoint_to_string(from);
		char ip_only_buf[32];
		std::snprintf(ip_only_buf, sizeof(ip_only_buf), "%u.%u.%u.%u",
		              from.ip[0], from.ip[1], from.ip[2], from.ip[3]);
		const std::string client_ip_str = ip_only_buf;

		// JointOperations in-match join → the consolidated HostSessionAccept
		// (the same accept the in-engine listen server drives). PN is learned at
		// HELLO; thereafter route 0x42/0x43/0x46 by membership so the lobby
		// (NOVAWORLDUDP) container path below only ever sees lobby peers. The
		// component owns these peers' handshake/SCRK state + GameServerRuntime;
		// it has no World here, so its spawn/in-match events are ignored — the
		// standalone server is a session responder, not a live-sim host.
		bool route_jo = jo_peers_.count(peer) != 0;
		if (opcode == SESSION_OPCODE_CLIENT_HELLO) {
			ClientHello probe;
			if (parse_client_hello(body.data(), body.size(), probe) &&
			    classify_session_protocol(probe.pn) ==
			            SessionProtocolKind::JointOperations) {
				route_jo = true;
				jo_peers_.insert(peer);
			}
		}
		if (route_jo) {
			manager_.notify_seen_addr(peer, now_ms());
			auto result = accept_.handle_datagram(
			        peer, rx, static_cast<size_t>(n),
			        static_cast<uint32_t>(now_ms() & 0xFFFFFFFFu));
			for (const auto &dg : result.outbound) {
				opennova::net::udp_send_to(socket.get(), from, dg.data(), dg.size());
			}
			if (opcode == SESSION_OPCODE_CLIENT_GOODBYE) jo_peers_.erase(peer);
			continue;
		}

		switch (opcode) {
		case SESSION_OPCODE_CLIENT_HELLO: {
			ClientHello hello;
			if (!parse_client_hello(body.data(), body.size(), hello)) {
				std::fprintf(stderr, "[nwudp] %s — bad ClientHello\n",
				             client_label.c_str());
				break;
			}
			const SessionProtocolKind protocol = classify_session_protocol(hello.pn);
			if (protocol == SessionProtocolKind::Unsupported) {
				std::fprintf(stderr, "[nwudp] %s — refusing PN='%s'\n",
				             client_label.c_str(), hello.pn.c_str());
				if (tracker_) {
					tracker_->record("pn", hello.pn, body.data(), body.size(),
					                 client_label, now_ms());
				}
				break;
			}
			Connection conn;
			conn.id = hello.ci;
			conn.addr = peer;
			conn.state = ConnectionState::Handshaking;
			conn.created_ms = now_ms();
			conn.last_seen_ms = conn.created_ms;
			conn.pn = hello.pn;
			manager_.notify_handshake(conn);

			ServerHello reply = build_server_hello(hello, client_ip_net,
			                                       client_port);
			auto packet = nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO,
			                                 server_hello_to_bytes(reply));
			opennova::net::udp_send_to(socket.get(), from, packet.data(),
			                           packet.size());
			std::printf("[nwudp] HELLO from %s ci=0x%08x pn=%s -> ServerHello (%zu B)\n",
			            client_label.c_str(), hello.ci, hello.pn.c_str(),
			            packet.size());
			break;
		}

		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth auth;
			if (!parse_client_auth(body.data(), body.size(), auth)) {
				std::fprintf(stderr, "[nwudp] %s — bad ClientAuth\n",
				             client_label.c_str());
				break;
			}
			const std::string server_scrk = make_dev_scrk();
			const std::string nwuid = make_dev_nwuid();
			// auth.scrk is the CLIENT-generated session key — store it for
			// decrypting inbound SESSION packets. Our locally-generated
			// server_scrk is what we encrypt outbound SESSION with AND
			// what we echo back in ServerAuth.scrk so the client can
			// decrypt our replies.
			//
			// Addr-keyed (G.7): auth.ci collides between two retail
			// processes — using id-keyed mark_active aliased instance 1's
			// connection record onto instance 2's, so subsequent
			// find_by_addr(instance_1) returned instance 2's scrk and
			// retail #1 got "INCOMING PACKET ERROR" on every reply.
			manager_.notify_active_addr(peer, /*identity*/ auth.na,
			                            /*client_scrk=*/auth.scrk,
			                            /*server_scrk=*/server_scrk);
			manager_.notify_seen_addr(peer, now_ms());

			std::printf("[nwudp] AUTH scrks: client_scrk=%zuB server_scrk=%zuB ck=0x%08x\n",
			            auth.scrk.size(), server_scrk.size(), auth.ck);

			// Stash the client's CK + our generated server SK in the
			// per-connection lobby state. CK feeds outbound SESSION headers
			// (retail's validator wants session_id == its own local_key ==
			// its CK). Server SK is what we advertise in ServerAuth.SK and
			// what retail echoes back as the inbound session_id; we keep it
			// per-connection (was hardcoded 0xC0FFEE00 before — see G.2).
			//
			// Keyed by PeerAddr (G.7): two retail processes both send
			// ci=0x00000001, so keying by ci would have the second AUTH
			// overwrite the first's per-connection state.
			const uint32_t server_sk = make_random_session_u32();
			{
				std::lock_guard<std::mutex> lk(lobby_states_mu_);
				auto &state = lobby_states_[peer];
				state.client_ck = auth.ck;
				state.server_sk = server_sk;
			}

			ServerAuth reply = build_server_auth(auth, client_ip_net,
			                                     client_port, server_sk,
			                                     server_scrk,
			                                     /*novaworld_name=*/"NWServer",
			                                     /*novaworld_web_url=*/"http://127.0.0.1:8080",
			                                     /*nwuid=*/nwuid);
			auto packet = nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH,
			                                 server_auth_to_bytes(reply));
			opennova::net::udp_send_to(socket.get(), from, packet.data(),
			                           packet.size());
			std::printf("[nwudp] AUTH from %s ci=0x%08x na='%s' -> ServerAuth (%zu B)\n",
			            client_label.c_str(), auth.ci, auth.na.c_str(),
			            packet.size());
			break;
		}

		case SESSION_OPCODE_PROTOCOL_MESSAGE: {
			manager_.notify_seen_addr(peer, now_ms());

			auto conn_opt = manager_.registry().find_by_addr(peer);
			if (!conn_opt || conn_opt->client_scrk.empty()) {
				// SESSION before AUTH completed — drop quietly.
				std::printf("[nwudp] SESSION before AUTH from %s; dropped\n",
				            client_label.c_str());
				break;
			}

			std::printf("[nwudp] SESSION from %s body=%zuB scrk=client(%zuB)\n",
			            client_label.c_str(), body.size(),
			            conn_opt->client_scrk.size());

			ProtocolPacketHeader hdr;
			std::vector<ProtocolMessage> messages;
			if (!decode_protocol_packet_plaintext(body.data(), body.size(),
			                                      conn_opt->client_scrk, hdr, messages)) {
				std::fprintf(stderr, "[nwudp] %s — bad SESSION envelope\n",
				             client_label.c_str());
				break;
			}
			std::printf("[nwudp]   hdr.session_id=0x%08x seq=%u ack=%u messages=%zu\n",
			            hdr.session_id, hdr.seq_num, hdr.ack_count, messages.size());

			// Look up (or create) per-connection lobby state, keyed by the
			// peer address (NOT ClientHello.ci — both retail processes send
			// ci=0x00000001 so keying by ci aliased their state, see G.7).
			// Hold the lock for the WHOLE dispatch — the HTTP thread's
			// snapshot_hosted() reads the same LobbyState (std::string /
			// std::map fields), and a race against the dispatch's writes
			// manifests as a SIGSEGV on /api/hosts under load (2026-04-28).
			std::lock_guard<std::mutex> dispatch_lk(lobby_states_mu_);
			auto &lobby_state = lobby_states_[peer];
			lobby_state.last_inbound_seq = hdr.seq_num;

			std::vector<ProtocolMessage> replies;
			const SessionProtocolKind protocol = classify_session_protocol(conn_opt->pn);
			// JointOperations peers are routed to HostSessionAccept above and
			// never reach this switch — only the lobby (NOVAWORLDUDP) container
			// path and unsupported PNs land here.
			if (protocol == SessionProtocolKind::Lobby) {
				for (const auto &pm : messages) {
					std::printf("[nwudp]     pm flags=0x%02x tag=0x%03x len=%u settings=%d frag_cont=%d frag_end=%d\n",
					            pm.flags.raw, pm.full_tag, pm.length,
					            pm.flags.settings_update ? 1 : 0,
					            pm.flags.frag_cont ? 1 : 0,
					            pm.flags.frag_end ? 1 : 0);
					if (pm.flags.settings_update) continue; // socket tuning, ignore
					if (pm.full_tag != 0) {
						// Non-zero full message type = a container/protocol
						// selector we have no Layer-4 handler for. Record the
						// hex tag so /api/unknowns surfaces what retail sent.
						if (tracker_) {
							tracker_->record("ptype", hex_sig(pm.full_tag, 3),
							                 pm.payload.data(), pm.payload.size(),
							                 client_label, now_ms());
						}
						continue;                            // only Layer-4 lobby
					}

					// Fragment reassembly: multi-packet payloads (e.g.
					// ClientRequestVerifyResult @ ~3.4 KB) span 2-3 SESSION
					// packets with FRAG_CONT set on every chunk except the
					// final. reassemble_protocol_payload accumulates and
					// returns true only when the assembled buffer is ready.
					std::vector<uint8_t> assembled;
					bool was_fragmented = false;
					if (!reassemble_protocol_payload(lobby_state.reassembly, pm,
					                                 assembled, &was_fragmented)) {
						std::printf("[nwudp]     fragment buffered (assembly=%zuB so far)\n",
						            lobby_state.reassembly.buffer.size());
						continue;
					}
					if (assembled.empty()) continue; // ack-only

					if (was_fragmented) {
						std::printf("[nwudp]     reassembled %zu-byte payload from fragments\n",
						            assembled.size());
					}

					// Parse the inner stream as a Container (NapiMessage tree).
					std::vector<NapiMessage> outer_messages;
					size_t consumed = 0;
					if (napi_stream_decode(assembled.data(), assembled.size(),
					                       outer_messages, &consumed) != 0) {
						std::fprintf(stderr, "[nwudp]     napi_stream_decode FAILED on %zu-byte payload\n",
						             assembled.size());
						continue;
					}
					std::printf("[nwudp]     parsed %zu container(s)\n", outer_messages.size());
					for (const auto &outer : outer_messages) {
						// onnet wraps the actual message inside a root container,
						// so each top-level message we get IS the lobby message
						// (its name is the message kind).
						LobbyDispatchResult result;
						if (outer.name == "ClientRequestVerifyResult") {
							const auto maintenance = load_maintenance_status(db_);
							if (maintenance.enabled) {
								result.label = "ClientRequestVerifyResult:maintenance";
								result.reply_containers.push_back(
									make_server_verify_failure(maintenance.message));
							}
						}
						if (result.label.empty()) {
							result = lobby_session_.dispatch(outer, lobby_state.lobby,
							                                client_ip_str, from.port);
						}
						if (!result.label.empty()) {
							std::printf("[nwudp] %s SESSION recv name=%s -> %zu replies\n",
							            client_label.c_str(), result.label.c_str(),
							            result.reply_containers.size());
							// lobby_session.cpp returns "unknown:<name>" for any
							// container it has no handler for (lobby_session.cpp:258).
							static constexpr char kUnknownPrefix[] = "unknown:";
							if (tracker_ &&
							    result.label.rfind(kUnknownPrefix, 0) == 0) {
								const std::string name =
									result.label.substr(sizeof(kUnknownPrefix) - 1);
								tracker_->record("container", name,
								                 assembled.data(), assembled.size(),
								                 client_label, now_ms());
							}
						}
						for (auto &reply_container : result.reply_containers) {
							std::vector<NapiMessage> root_stream{std::move(reply_container)};
							std::vector<uint8_t> stream_bytes(napi_stream_size(root_stream));
							size_t stream_size = 0;
							if (napi_stream_encode(root_stream, stream_bytes.data(),
							                       stream_bytes.size(), &stream_size) != 0) {
								continue;
							}
							stream_bytes.resize(stream_size);

							ProtocolMessage rpm;
							rpm.flags.raw = (stream_size > 0xFF) ? 0x40u : 0x20u;
							rpm.flags.len16 = stream_size > 0xFF;
							rpm.flags.len8  = stream_size <= 0xFF;
							rpm.tag = 0;
							rpm.full_tag = 0;
							rpm.length = static_cast<uint32_t>(stream_size);
							rpm.payload = std::move(stream_bytes);
							replies.push_back(std::move(rpm));
						}
					}
				}
			} else {
				std::fprintf(stderr, "[nwudp] %s — unsupported SESSION PN='%s'\n",
				             client_label.c_str(), conn_opt->pn.c_str());
				if (tracker_) {
					tracker_->record("pn", conn_opt->pn, body.data(), body.size(),
					                 client_label, now_ms());
				}
			}

			ProtocolPacketHeader rhdr;
			// session_id = retail's local_key = retail's ClientAuth.ck
			// (per protocol_message.h NapiNPProtocol_HandleSessionPacket
			// witness). Using conn_opt->id (= ClientHello.ci) made retail
			// TOSS our reply with code [4] — confirmed via _connectlog.txt
			// 2026-04-27.
			rhdr.session_id = lobby_state.client_ck;
			rhdr.seq_num = lobby_state.next_outbound_seq++;
			rhdr.ack_count = lobby_state.last_inbound_seq;
			rhdr.connection_flags = 0;

			std::vector<uint8_t> body_out;
			if (!encode_protocol_packet_plaintext(rhdr, replies, conn_opt->server_scrk, body_out)) {
				std::fprintf(stderr, "[nwudp] %s — encode_protocol_packet_plaintext failed\n",
				             client_label.c_str());
				break;
			}
			std::printf("[nwudp]   sending %zu reply byte(s) (server_scrk=%zuB)\n",
			            body_out.size(), conn_opt->server_scrk.size());
			auto packet = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			                                 std::move(body_out));
			opennova::net::udp_send_to(socket.get(), from, packet.data(), packet.size());
			break;
		}

		case SESSION_OPCODE_CLIENT_GOODBYE: {
			// Drop the entry on graceful exit. The 4-byte conn id sits at
			// the head of the GoodBye payload (per onnet's nw_udp_server.py).
			uint32_t ci = 0;
			if (body.size() >= 4) {
				std::memcpy(&ci, body.data(), 4); // little-endian
			}
			std::printf("[nwudp] GOODBYE from %s ci=0x%08x\n",
			            client_label.c_str(), ci);
			// Addr-keyed (G.7): two retail processes both ship ci=1, so
			// notify_logout(ci) would drop the wrong connection.
			manager_.notify_logout_addr(peer);
			// notify_logout_addr fires on_lost which calls
			// erase_lobby_state via main.cpp's wiring (G.6).
			// Belt-and-braces erase here too in case the handler isn't
			// installed.
			erase_lobby_state(peer, "goodbye");
			break;
		}

		default:
			std::fprintf(stderr, "[nwudp] %s — unhandled opcode 0x%02x\n",
			             client_label.c_str(), opcode);
			if (tracker_) {
				tracker_->record("nwu", hex_sig(opcode), body.data(), body.size(),
				                 client_label, now_ms());
			}
			break;
		}
	}

	std::printf("[nwudp] loop exiting\n");
}

} // namespace opennova::server
