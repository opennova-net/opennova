#include "nw_udp_listener.h"

#include "core/loggable.h"
#include "program/server_config.h"

#include <net_datagram_socket.h>
#include <base/io/tick_rate.h>
#include <net_sockets.h>
#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/napi/tlv.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_session.h>
#include <net/npwire/cs_config.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/outgoing_packets.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/novaworld/session_protocol.h>
#include <net/novaworld/unknown_tracker.h>
#include <net/title/title_identity.h>

#include <base/gameprofile/gameprofile.h>

#include <formats/mission/bms.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::novaworld_server {

namespace {

uint64_t now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
		duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int64_t unix_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// A registry state as a session row names it.
const char *connection_state_name(ConnectionState s) {
	switch (s) {
		case ConnectionState::Handshaking:
			return "handshaking";
		case ConnectionState::Active:
			return "active";
		case ConnectionState::Closing:
			return "closing";
	}
	return "handshaking";
}

// How long a dropped connection's lobby identity waits for its reconnect: the NOVAWORLDUDP
// connection's own reap window (CS field 0). Service policy.
// [orig: CNapiGameSession_InitNPConnection @0x4d3e1f (240000)]
constexpr uint64_t kParkedLobbyTtlMs = 240000;

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

// make_dev_scrk / make_dev_nwuid / make_session_key and the NW-UDP
// envelope transform (nw_decode_inbound / nw_encode_outbound) moved to
// engine/net/novaworld nw_session_framing.h so the standalone server's lobby path and
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

// The shared HostOwner pump expects sole ownership of an IDatagramSocket's
// receive stream. This listener also serves lobby traffic on the same OS
// socket, so demultiplexed JO datagrams are queued here while sends go
// straight back through that socket.
class QueuedJoSocket final : public opennova::IDatagramSocket {
public:
	explicit QueuedJoSocket(opennova::net::Socket &socket) : sender_(socket) {}

	void push(const PeerAddr &peer, const uint8_t *data, std::size_t size) {
		Pending item;
		item.peer = peer;
		item.bytes.assign(data, data + size);
		pending_.push_back(std::move(item));
	}

	int recv_from(uint8_t *data, std::size_t capacity, PeerAddr &peer) override {
		if (pending_.empty()) return 0;
		Pending item = std::move(pending_.front());
		pending_.pop_front();
		if (item.bytes.size() > capacity) return -1;
		peer = item.peer;
		std::memcpy(data, item.bytes.data(), item.bytes.size());
		return static_cast<int>(item.bytes.size());
	}

	void send_to(const PeerAddr &peer, const uint8_t *data, std::size_t size) override {
		sender_.send_to(peer, data, size);
	}

private:
	struct Pending {
		PeerAddr peer{};
		std::vector<uint8_t> bytes;
	};

	opennova::net::NetDatagramSocket sender_;
	std::deque<Pending> pending_;
};

// One lobby statement as the layer-4 record it rides in: the container alone in a root stream,
// LEN8 or LEN16 by its size, at full message type 0, reliable (retained under its packet's
// sequence until the client's ACK covers it, so a 0x44 resends it). Empty when it fails to encode.
std::optional<ProtocolMessage> lobby_record(NapiMessage statement) {
	std::vector<NapiMessage> root_stream{std::move(statement)};
	std::vector<uint8_t> stream_bytes(napi_stream_size(root_stream));
	size_t stream_size = 0;
	if (napi_stream_encode(root_stream, stream_bytes.data(), stream_bytes.size(), &stream_size) !=
	    0) {
		return std::nullopt;
	}
	stream_bytes.resize(stream_size);

	ProtocolMessage rpm;
	rpm.flags.raw = (stream_size > 0xFF) ? opennova::PROTOCOL_MSG_FLAG_LEN16
	                                     : opennova::PROTOCOL_MSG_FLAG_LEN8;
	rpm.flags.len16 = stream_size > 0xFF;
	rpm.flags.len8 = stream_size <= 0xFF;
	rpm.tag = 0;
	rpm.full_tag = 0;
	rpm.length = static_cast<uint32_t>(stream_size);
	rpm.payload = std::move(stream_bytes);
	rpm.reliable = true; // retained until ACKed: a 0x44 resends it
	return rpm;
}

// One send build on a lobby connection, framed: its datagrams, and whether every record went out
// framed (false when the pool, CS field 11, refused one, one failed to encode, or a packet failed
// to frame; the datagrams framed before that still go). The shared BuildOutgoingPackets planner
// splits the records at the connection's CS field 13 (a long one leaves as FIRST/MID/FINAL
// records), and the ONE seq/ack framer (ADR 0013) stamps each packet: session_id, seq_num =
// next_outbound_seq++, ack_count = last_inbound_seq, connection_flags = 0. session_id = retail's
// local_key = retail's ClientAuth.ck (per protocol_message.h's NapiNPProtocol_HandleSessionPacket
// witness); using ClientHello.ci made retail TOSS our reply with code [4] — confirmed via
// _connectlog.txt 2026-04-27. No records builds one header-only packet. `what` names the
// per-packet log line (null: none).
// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430 — the ACK-only packet when the queue is
//  empty @0x62847e..0x62848c, the per-packet send stamp @0x628605; NapiNPMessage_SplitAtLength
//  @0x628350]
struct FramedRecords {
	std::vector<std::vector<uint8_t>> datagrams;
	bool all_framed = false;
};
FramedRecords frame_lobby_records(const opennova::net::Endpoint &to, LobbyConnState &state,
                                  std::string_view server_scrk,
                                  const std::vector<ProtocolMessage> &records, const char *what) {
	FramedRecords out;
	const std::string label = opennova::net::endpoint_to_string(to);
	OutgoingPacketPlan plan = plan_outgoing_packets(state.sequencing, records,
			packet_ceiling_bytes(state.cs.max_packet_bytes),
			max_packets_per_build(state.cs.max_packets_per_tick),
			OutgoingOverflow::DropRefused);
	out.all_framed = !plan.overflow && !plan.encode_failed && plan.unbuilt.empty();
	if (plan.overflow) {
		std::fprintf(stderr, "[nwudp] %s — reply pool full (NP.C:MSGCRE, %u records)\n",
		             label.c_str(), plan.overflow_count);
	}
	if (plan.encode_failed) {
		std::fprintf(stderr, "[nwudp] %s — a record failed to encode\n", label.c_str());
	}
	if (plan.packets.empty()) plan.packets.emplace_back();
	for (const std::vector<ProtocolMessage> &packet_records : plan.packets) {
		std::vector<uint8_t> body_out;
		if (!frame_session_packet(state.sequencing,
		                          SessionCrypto{server_scrk, {}, state.client_ck},
		                          packet_records, body_out)) {
			std::fprintf(stderr, "[nwudp] %s — frame_session_packet failed\n", label.c_str());
			out.all_framed = false;
			break;
		}
		state.last_framed_send_ms = now_ms();
		if (what != nullptr) {
			std::printf("[nwudp]   sending %zu %s byte(s) (server_scrk=%zuB)\n", body_out.size(),
			            what, server_scrk.size());
		}
		out.datagrams.push_back(nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
		                                           std::move(body_out)));
	}
	return out;
}

void send_datagrams(opennova::net::Socket &socket, const opennova::net::Endpoint &to,
                    const std::vector<std::vector<uint8_t>> &datagrams) {
	for (const std::vector<uint8_t> &packet : datagrams)
		opennova::net::udp_send_to(socket, to, packet.data(), packet.size());
}

// One send build on a lobby connection (frame_lobby_records), sent. True when every record went
// out framed.
bool send_lobby_records(opennova::net::Socket &socket, const opennova::net::Endpoint &to,
                        LobbyConnState &state, std::string_view server_scrk,
                        const std::vector<ProtocolMessage> &records, const char *what) {
	const FramedRecords framed = frame_lobby_records(to, state, server_scrk, records, what);
	send_datagrams(socket, to, framed.datagrams);
	return framed.all_framed;
}

} // namespace

int64_t unix_ms_of_steady(uint64_t steady_ms, uint64_t steady_now_ms, int64_t unix_now_ms) {
	const uint64_t age = steady_now_ms > steady_ms ? steady_now_ms - steady_ms : 0;
	return unix_now_ms - static_cast<int64_t>(age);
}

NwUdpListener::NwUdpListener(ConnectionManager &manager) : manager_(manager) {}

NwUdpListener::~NwUdpListener() { stop(); }

void NwUdpListener::initialize_jo_host() {
	jo_world_ = std::make_unique<world::World>();
	jo_world_->registry.configure_pool(0, 64);
	jo_world_->registry.configure_pool(3, 4);
	// No items.def means no trait sweep, so nothing stamps the Player row's
	// ordinal (tables.player.item_type_index, 0 unswept) on the bodies this
	// world spawns. Retail's player spawn stores the class item's items.def
	// ordinal (Entity_SpawnFromAnimSlotProperty: the lookup @0x43C3C3, the
	// +0x1C store @0x43C426), which a host with its items.def never leaves 0.
	// Stand in a nonzero one, as the template's has_item_def default already
	// claims the row: the C2S 0x32 sweep lists every pool-0 row at index 0
	// (D-NET-486), and a joiner destroys what it lists, its own body included.
	// Every reader of a player body's index tests it against 0 alone.
	jo_world_->tables.player.item_type_index = 1;

	// A loaded mission normally supplies this marker. The standalone gate has
	// no mission path, so its minimal authoritative world uses the spawn
	// pipeline's ordinary start-marker input at the origin. It loads no
	// items.def either, so the start has no ItemTypeIndex and the 0x20 page
	// streams its slot as an empty one, as a host whose items.def lacked the
	// row would (D-NET-416). Nothing reads that copy: the start's reader is the
	// spawn resolve (world::resolve_player_spawn_pose), which runs on the host's
	// own registry, never on a streamed row.
	world::Entity start;
	start.kind = world::EntityKind::Marker;
	start.item_id = 6002;
	jo_world_->registry.spawn(3, start);

	jo_mission_ = std::make_unique<bms::File>();
	jo_mission_->header.magic[0] = 'B';
	jo_mission_->header.magic[1] = 'M';
	jo_mission_->header.magic[2] = 'S';
	jo_mission_->header.magic[3] = static_cast<char>(bms::kMinVersion);

	jo_owner_ = std::make_unique<inmatch::HostOwner>();
	jo_owner_->ctx.world = jo_world_.get();
	jo_owner_->ctx.mission = jo_mission_.get();

	inmatch::HostConfig config;
	// The embedded host speaks JO's in-match identity.
	config.title = title_identity(gameprofile::GAME_JO);
	config.config.server_name = "OpenNova";
	config.config.max_players = 64;
	config.config.dedicated_server = true;
	config.socket_mode = inmatch::SocketMode::Lan;
	config.serve_and_play = false;
	inmatch::start_host_session(*jo_owner_, config);
}

void NwUdpListener::reset_per_run_state(const char *reason) {
	// Retire every exact ConnectionManager address this listener admitted,
	// including peers that stopped after a valid Hello and never reached Auth.
	// The manager can be shared, so never sweep its full registry here.
	// erase_lobby_state remains the fallback when no on_lost callback is
	// installed.
	std::vector<PeerAddr> lobby_peers;
	{
		std::lock_guard<std::mutex> lock(lobby_states_mu_);
		lobby_peers.reserve(lobby_peers_.size());
		for (const auto &entry : lobby_peers_) lobby_peers.push_back(entry.first);
	}
	for (const PeerAddr &peer : lobby_peers) {
		manager_.notify_logout_addr(peer);
		erase_lobby_state(peer, reason);
	}
	{
		std::lock_guard<std::mutex> lock(lobby_states_mu_);
		lobby_peers_.clear();
		lobby_states_.clear();
		parked_lobby_states_.clear();
	}
	last_command_drain_ms_ = 0;
	last_seen_write_ms_ = 0;

	jo_peers_.clear();
	jo_owner_.reset();
	jo_mission_.reset();
	jo_world_.reset();
}

void NwUdpListener::observe_jo_event(
		void *context, const inmatch::HostAcceptEvent &event) {
	if (event.kind != inmatch::HostAcceptEvent::Kind::PeerGoodbye) return;
	auto &listener = *static_cast<NwUdpListener *>(context);
	if (!listener.jo_owner_) {
		listener.jo_peers_.erase(event.peer);
		return;
	}
	// A changed ClientAuth can retire an old node and create its replacement
	// in one HandleResult. Keep routing when that fresh connection exists.
	const bool replacement_exists = std::any_of(
			listener.jo_owner_->ctx.np_protocol.connection_list.begin(),
			listener.jo_owner_->ctx.np_protocol.connection_list.end(),
			[&event](const inmatch::NapiNPConnection &connection) {
				return connection.type == 1 && connection.peer == event.peer;
			});
	if (!replacement_exists) listener.jo_peers_.erase(event.peer);
}

bool NwUdpListener::start(const ServerConfig &config) {
	if (running_.load()) return true;
	if (worker_.joinable()) worker_.join();
	reset_per_run_state("restart");

	// The endpoint its instance announces, the one the gates hand out: the
	// config's (ONNET_LOBBY_ENDPOINT: the deployment's one lobby address), never
	// derived from the port this listener binds. With a database it registers
	// an instance, which needs one.
	if (db_pool_ != nullptr && config.lobby_endpoint.empty()) {
		std::fprintf(stderr,
				"[nwudp] no lobby endpoint (ONNET_LOBBY_ENDPOINT) for its instance "
				"to announce\n");
		return false;
	}

	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "[nwudp] net::startup failed\n");
		return false;
	}

	// The socket stays bound from here into the receive thread, so the port
	// reported is the port served: a close and a re-bind would free it in
	// between (with port 0, for any other socket to take).
	uint16_t bound = 0;
	opennova::net::ScopedSocket socket(opennova::net::udp_bind(config.nw_udp_port, &bound));
	if (!socket.is_valid()) {
		std::fprintf(stderr, "[nwudp] failed to bind UDP %u\n",
		             static_cast<unsigned>(config.nw_udp_port));
		return false;
	}
	bound_port_ = bound;
	instance_endpoint_ = config.lobby_endpoint;
	lobby_id_.store(0);
	// The SessionInit CU carries the web domain as bare host:port — the
	// retail client prepends the scheme itself
	// [orig: CNapiGameSession_OnNovaWorldConnected @0x4d1627 sprintf("http://%s")].
	web_domain_ = config.web_host + ":" + std::to_string(config.http_port);
	lobby_session_.set_glsvss_results(config.glsvss_results);
	lobby_session_.set_require_host_key(config.require_host_key);
	if (config.require_host_key) {
		std::printf("[nwudp] ONNET_REQUIRE_HOST_KEY: no host is listed before its HostKey names an account\n");
	}

	// The receive thread's own connection (Database is single-threaded): the
	// lobby session's host persistence and the maintenance lookup run on it.
	// The thread leases it itself (a lease stays on the thread that acquired
	// it), and start() waits for the lease, so a database that cannot be
	// opened stops the boot.
	stop_requested_.store(false);
	std::promise<bool> leased;
	std::future<bool> lease_taken = leased.get_future();
	const bool embedded_jo_host = config.embedded_jo_host;
	worker_ = std::thread([this, socket = std::move(socket), leased = std::move(leased),
			embedded_jo_host]() mutable {
		std::optional<db::ConnectionPool::Lease> db_conn;
		if (db_pool_) {
			try {
				db_conn.emplace(db_pool_->acquire());
				register_instance(**db_conn);
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[nwudp] db open or lobby instance failed: %s\n", e.what());
				leased.set_value(false);
				return;
			}
		}
		// The in-match JO host only when the config asks for it (ONNET_EMBEDDED_JO_HOST).
		if (embedded_jo_host) initialize_jo_host();
		leased.set_value(true);
		run_loop(std::move(socket), std::move(db_conn));
	});
	if (!lease_taken.get()) {
		worker_.join();
		return false;
	}
	running_.store(true);
	std::printf("[nwudp] listening on UDP :%u\n",
	            static_cast<unsigned>(bound_port_));
	return true;
}

void NwUdpListener::stop() {
	stop_requested_.store(true);
	if (worker_.joinable()) worker_.join();
	running_.store(false);
	reset_per_run_state("listener-stop");
	// Its instance last: nothing names it now but rows a drop left behind, which go with it.
	const int64_t id = lobby_id_.exchange(0);
	if (id != 0 && db_pool_) {
		try {
			std::optional<db::ConnectionPool::Lease> lease;
			db::Database *db = db_pool_->leased_by_this_thread();
			if (db == nullptr) {
				lease.emplace(db_pool_->acquire());
				db = lease->get();
			}
			lobbydb::remove_instance(*db, id);
			std::printf("[lobby] instance %lld at %s removed\n", static_cast<long long>(id),
					loggable(instance_endpoint_).c_str());
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[lobby] WARN removing instance %lld: %s\n",
					static_cast<long long>(id), e.what());
		}
	}
}

void NwUdpListener::register_instance(db::Database &db) {
	// A row of its own: every instance announces the same endpoint (the deployment's one lobby
	// address), so neither the endpoint nor a previous run's row names this one.
	const int64_t id = lobbydb::register_instance(db, instance_endpoint_, false, task_id_);
	const size_t reaped = lobbydb::reap_stale_instances(db, lobbydb::kInstanceStaleSeconds, id);
	hostdb::clear_unheld(db);
	lobby_id_.store(id);
	lobby_session_.set_lobby_id(id);
	last_instance_touch_ms_ = now_ms();
	std::printf("[lobby] instance %lld at %s registered; %zu stale instance(s) reaped with their "
				"rows\n",
			static_cast<long long>(id), loggable(instance_endpoint_).c_str(), reaped);
}

void NwUdpListener::reregister_instance(db::Database &db) {
	// Drained (its own stop, or an admin's through the row), it stays drained: coming back does
	// not make it count as taking clients again.
	const bool draining = draining_.load();
	const int64_t id = lobbydb::register_instance(db, instance_endpoint_, draining, task_id_);
	lobby_id_.store(id);
	lobby_session_.set_lobby_id(id);
	std::size_t sessions = 0;
	std::size_t listings = 0;
	{
		std::lock_guard<std::mutex> lk(lobby_states_mu_);
		for (auto &entry : lobby_peers_) {
			entry.second = LobbyPeer{};
			sync_session_locked(entry.first, &db);
			if (entry.second.session_id != 0)
				++sessions;
		}
		for (auto &[peer, state] : lobby_states_) {
			if (!state.lobby.hosting || state.lobby.awaiting_host_key)
				continue;
			lobby_session_.relist(state.lobby, peer_addr_ip_to_string(peer), peer.port);
			++listings;
		}
	}
	std::printf("[lobby] instance %lld at %s registered again%s: %zu session row(s) and %zu "
				"listing(s) written anew\n",
			static_cast<long long>(id), loggable(instance_endpoint_).c_str(),
			draining ? ", draining" : "", sessions, listings);
}

bool NwUdpListener::drain() {
	const int64_t id = lobby_id_.load();
	if (id == 0 || !db_pool_)
		return false;
	// Remembered before the row is marked, so a registration again keeps it.
	draining_.store(true);
	try {
		std::optional<db::ConnectionPool::Lease> lease;
		db::Database *db = db_pool_->leased_by_this_thread();
		if (db == nullptr) {
			lease.emplace(db_pool_->acquire());
			db = lease->get();
		}
		const bool marked = lobbydb::set_instance_state(*db, id, "draining");
		if (marked)
			std::printf("[lobby] instance %lld draining: no gate counts it as taking clients\n",
					static_cast<long long>(id));
		return marked;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[lobby] WARN draining instance %lld: %s\n",
				static_cast<long long>(id), e.what());
		return false;
	}
}

void NwUdpListener::erase_lobby_state(const PeerAddr &peer, const char *reason, db::Database *db) {
	bool had_state = false;
	bool was_hosting = false;
	uint32_t rid = 0;
	// The server it played on and the account it signed in as: its row in that listing's roster.
	uint32_t playing_rid = 0;
	std::optional<int64_t> user_id;
	int64_t session_id = 0;
	{
		std::lock_guard<std::mutex> lk(lobby_states_mu_);
		// This method is the ConnectionManager on_lost sink as well as the
		// lobby-state cleanup hook. Forget ownership (and the session row) even
		// for Hello-only peers, which intentionally have no LobbyConnState yet.
		if (const auto peer_it = lobby_peers_.find(peer); peer_it != lobby_peers_.end()) {
			session_id = peer_it->second.session_id;
			lobby_peers_.erase(peer_it);
		}
		auto it = lobby_states_.find(peer);
		if (it != lobby_states_.end()) {
			had_state = true;
			was_hosting = it->second.lobby.hosting;
			rid = it->second.lobby.rid;
			playing_rid = it->second.lobby.playing_rid;
			user_id = it->second.lobby.user_id;
			// Park the identity for the connection's reconnect (see parked_lobby_states_).
			const uint64_t now = now_ms();
			for (auto park = parked_lobby_states_.begin(); park != parked_lobby_states_.end();) {
				park = now - park->second.parked_ms > kParkedLobbyTtlMs ? parked_lobby_states_.erase(park)
				                                                       : std::next(park);
			}
			parked_lobby_states_[peer] = ParkedLobbyState{it->second.client_ci, it->second.lobby, now};
			if (was_hosting) {
				std::printf("[lobby] stopped addr=%s rid=%u server_name='%s' reason=%s\n",
						peer_addr_to_string(peer).c_str(), it->second.lobby.rid,
						loggable(it->second.lobby.server_name).c_str(), reason);
			}
			lobby_states_.erase(it);
		}
	}
	if (!had_state && session_id == 0) return;

	// Phase I.2: drop the matching row(s) from active_hosts +
	// host_players. Hosting peer is keyed by RID (drops the host row); a
	// joiner leaves the server it played on, its account's host_players row
	// under that RID going (LobbySession::leave_server's rule). The parked
	// state keeps its play columns, so a reconnect's replay still names the
	// server it plays on; its roster row does not come back (only
	// /NWJoin.dll writes one). The
	// session row goes by its id, and its commands with it (their foreign
	// key cascades). On the calling thread's own connection (this runs on
	// the receive thread and on main alike): the caller's, else the one its
	// lease holds, else one leased for the call.
	if (db_pool_) {
		try {
			std::optional<db::ConnectionPool::Lease> lease;
			if (db == nullptr)
				db = db_pool_->leased_by_this_thread();
			if (db == nullptr) {
				lease.emplace(db_pool_->acquire());
				db = lease->get();
			}
			if (session_id != 0) lobbydb::delete_session(*db, session_id);
			if (had_state) {
				if (was_hosting && rid != 0) {
					hostdb::remove_host_by_rid(*db, rid);
				}
				if (playing_rid != 0 && user_id) {
					hostdb::remove_player(*db, playing_rid, *user_id);
				}
			}
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

lobbydb::SessionRow NwUdpListener::session_row_locked(const PeerAddr &peer,
                                                      const Connection &conn) const {
	lobbydb::SessionRow row;
	row.connection_id = conn.id;
	row.lobby_id = lobby_id_.load();
	row.peer_ip = peer_addr_ip_to_string(peer);
	row.peer_port = peer.port;
	row.state = connection_state_name(conn.state);
	row.pn = conn.pn;
	row.identity = conn.identity;
	const uint64_t steady_now = now_ms();
	const int64_t unix_now = unix_now_ms();
	row.connected_ms = unix_ms_of_steady(conn.created_ms, steady_now, unix_now);
	row.last_seen_ms = unix_ms_of_steady(conn.last_seen_ms, steady_now, unix_now);
	const auto it = lobby_states_.find(peer);
	if (it != lobby_states_.end()) {
		const LobbyState &lobby = it->second.lobby;
		row.user_id = lobby.user_id;
		row.playing_rid = lobby.playing_rid;
		row.play_addr = lobby.play_addr;
		row.playing_since_ms = lobby.playing_since_ms;
		row.rid = lobby.rid;
		row.hosting = lobby.hosting;
		row.listed = lobby.hosting && !lobby.awaiting_host_key;
		row.host_user_id = lobby.host_user_id;
		row.server_name = lobby.server_name;
		row.mission_name = lobby.mission_name;
		row.players = lobby.player_count;
		row.max_players = lobby.max_players;
	}
	return row;
}

void NwUdpListener::sync_session_locked(const PeerAddr &peer, db::Database *db) {
	if (db == nullptr) return;
	const auto peer_it = lobby_peers_.find(peer);
	if (peer_it == lobby_peers_.end()) return;
	// A connection the registry dropped is on its way out: its on_lost erase takes the row.
	const std::optional<Connection> conn = manager_.registry().find_by_addr(peer);
	if (!conn) return;
	LobbyPeer &admitted = peer_it->second;
	lobbydb::SessionRow row = session_row_locked(peer, *conn);
	try {
		if (admitted.session_id == 0) {
			row.id = lobbydb::insert_session(*db, row);
			admitted.session_id = row.id;
			admitted.written = row;
			admitted.written_seen_ms = conn->last_seen_ms;
		} else if (!lobbydb::same_columns(row, admitted.written)) {
			row.id = admitted.session_id;
			lobbydb::update_session(*db, row);
			admitted.written = row;
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[lobby] WARN session row of %s: %s\n", peer_addr_to_string(peer).c_str(),
		             e.what());
	}
	// The lobby state carries the row's id, so persist_host writes the listing columns in the
	// listing row's own transaction (lobbydb::update_session_listing).
	if (const auto state_it = lobby_states_.find(peer); state_it != lobby_states_.end())
		state_it->second.lobby.session_row_id = admitted.session_id;
}

void NwUdpListener::write_sessions_seen(db::Database &db) {
	std::vector<lobbydb::SessionSeen> seen;
	std::vector<int64_t> held;
	{
		std::lock_guard<std::mutex> lk(lobby_states_mu_);
		const uint64_t steady_now = now_ms();
		const int64_t unix_now = unix_now_ms();
		for (auto &[peer, admitted] : lobby_peers_) {
			if (admitted.session_id == 0) continue;
			held.push_back(admitted.session_id);
			const std::optional<Connection> conn = manager_.registry().find_by_addr(peer);
			if (!conn || conn->last_seen_ms == admitted.written_seen_ms) continue;
			admitted.written_seen_ms = conn->last_seen_ms;
			seen.push_back({admitted.session_id,
			                unix_ms_of_steady(conn->last_seen_ms, steady_now, unix_now)});
		}
	}
	// Outside the lock: a row deleted meanwhile updates nothing. Then the rows of no session the
	// lobby holds go: a drop forgets its session before it deletes the row, and a delete that
	// failed would leave a row answering pushes for a RID no session holds. This thread alone
	// inserts session rows, so none is newer than `held`.
	try {
		lobbydb::touch_sessions(db, seen);
		if (const std::size_t swept = lobbydb::delete_sessions_except(db, lobby_id_.load(), held);
				swept > 0)
			std::printf("[lobby] swept %zu session row(s) no session holds\n", swept);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[lobby] WARN session times: %s\n", e.what());
	}
}

void NwUdpListener::touch_instance(db::Database &db) {
	const int64_t id = lobby_id_.load();
	if (id == 0)
		return;
	try {
		if (const std::optional<std::string> state = lobbydb::touch_instance(db, id)) {
			draining_.store(*state == "draining");
			return;
		}
		std::fprintf(stderr,
				"[lobby] instance %lld's row is gone (reaped as stale while this lobby could not "
				"reach the database, its rows with it); registering again\n",
				static_cast<long long>(id));
		reregister_instance(db);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[lobby] WARN instance %lld's heartbeat: %s\n",
				static_cast<long long>(id), e.what());
	}
}

const PeerAddr *NwUdpListener::peer_of_session_locked(int64_t session_id) const {
	if (session_id == 0) return nullptr;
	for (const auto &[peer, admitted] : lobby_peers_) {
		if (admitted.session_id == session_id) return &peer;
	}
	return nullptr;
}

// The commands in insert order, each once (take_commands deletes what it returns), on the
// receive thread, which owns the socket and the lobby's memory. A push goes out as its own send
// build on the session's connection; one whose session is gone, was replaced (a fresh 0x42 starts
// a new session row, and the old row's commands go with it), stopped hosting or changed owner
// since its insert is dropped, as are a session's end once it hosts and anything past its expiry.
// The ended sessions are dropped once the lock is released: the drop's on_lost erases the lobby
// state under the same lock.
void NwUdpListener::drain_commands(opennova::net::Socket &socket, db::Database &db) {
	if (command_drain_paused_.load()) return;
	std::vector<lobbydb::Command> commands;
	try {
		commands = lobbydb::take_commands(db, lobby_id_.load(), kCommandsPerDrain);
	} catch (const std::exception &e) {
		// A line a minute while the database fails, not one a drain.
		const uint64_t now = now_ms();
		if (last_drain_warning_ms_ == 0 || now - last_drain_warning_ms_ >= 60000) {
			last_drain_warning_ms_ = now;
			std::fprintf(stderr, "[nwudp] WARN taking the lobby's commands: %s\n", e.what());
		}
		return;
	}
	if (commands.empty()) return;
	std::vector<PeerAddr> ended;
	{
		std::lock_guard<std::mutex> lk(lobby_states_mu_);
		for (const lobbydb::Command &command : commands) {
			if (command.expired) {
				std::printf("[nwudp] COMMAND %lld (%s) dropped: it waited past %lld s\n",
				            static_cast<long long>(command.id),
				            lobbydb::command_kind_name(command.kind),
				            static_cast<long long>(lobbydb::kCommandTtlSeconds));
				continue;
			}
			switch (command.kind) {
			case lobbydb::CommandKind::ServerCommand:
			case lobbydb::CommandKind::StopHosting:
				run_push_locked(socket, db, command);
				break;
			case lobbydb::CommandKind::EndSession:
				if (std::optional<PeerAddr> peer = run_end_locked(socket, command)) ended.push_back(*peer);
				break;
			case lobbydb::CommandKind::ForgetOwner:
				if (command.account) forget_owner_locked(db, *command.account);
				break;
			}
		}
	}
	for (const PeerAddr &peer : ended) {
		manager_.notify_logout_addr(peer);
		// As the GOODBYE leg does, in case no on_lost handler is installed.
		erase_lobby_state(peer, "ended", &db);
	}
}

void NwUdpListener::run_push_locked(opennova::net::Socket &socket, db::Database &db,
                                    const lobbydb::Command &command) {
	const bool stop = command.kind == lobbydb::CommandKind::StopHosting;
	NapiMessage statement =
			stop ? make_server_stop_hosting(command.msg_code, command.msg_param1, command.msg_param2)
			     : make_server_command(command.cmd);
	const std::string name = statement.name;
	const PeerAddr *peer = peer_of_session_locked(command.session_id);
	const auto it = peer != nullptr ? lobby_states_.find(*peer) : lobby_states_.end();
	if (it == lobby_states_.end()) {
		std::printf("[nwudp] PUSH %s to rid=%u dropped: the connection is gone\n", name.c_str(),
		            command.rid);
		return;
	}
	const std::string addr = peer_addr_to_string(*peer);
	LobbyConnState &state = it->second;
	if (!state.lobby.hosting) {
		std::printf("[nwudp] PUSH %s to %s dropped: the connection no longer hosts\n",
		            name.c_str(), addr.c_str());
		return;
	}
	if (command.account) {
		// The owner in the lobby's memory (a re-host whose key names another account or none
		// changes it), and the live claim (a reset, a ban or a delete takes the account's keys
		// before its forget_owner reaches the lobby).
		bool owned = state.lobby.host_user_id == command.account;
		if (owned) {
			try {
				owned = hostdb::listing_owned_by(db, state.lobby.rid, *command.account);
			} catch (const std::exception &e) {
				std::fprintf(stderr, "[nwudp] WARN PUSH %s owner check: %s\n", name.c_str(), e.what());
				owned = false;
			}
		}
		if (!owned) {
			std::printf("[nwudp] PUSH %s to %s dropped: the listing's owner changed\n", name.c_str(),
			            addr.c_str());
			return;
		}
	}
	std::optional<ProtocolMessage> record = lobby_record(std::move(statement));
	if (!record) {
		std::fprintf(stderr, "[nwudp] PUSH %s to %s failed to encode\n", name.c_str(), addr.c_str());
		return;
	}
	std::vector<ProtocolMessage> records;
	records.push_back(std::move(*record));
	const opennova::net::Endpoint to = opennova::net::NetDatagramSocket::to_endpoint(*peer);
	const FramedRecords framed = frame_lobby_records(to, state, state.server_scrk, records, "push");
	if (!framed.all_framed) {
		send_datagrams(socket, to, framed.datagrams);
		std::fprintf(stderr, "[nwudp] PUSH %s to %s was not framed\n", name.c_str(), addr.c_str());
		return;
	}
	const uint32_t rid = state.lobby.rid;
	// The stop leaves the browser only once its record is framed (a refused one, the pool full,
	// or an unframed one leaves the hosting as it was), and before it goes out, so its session
	// row no longer hosts by the time the host reads it.
	if (stop) {
		lobby_session_.end_hosting(state.lobby, "ServerStopHosting");
		sync_session_locked(*peer, &db);
	}
	send_datagrams(socket, to, framed.datagrams);
	std::printf("[nwudp] PUSH %s -> %s rid=%u%s\n", name.c_str(), addr.c_str(), rid,
	            command.account ? " (the owner's)" : "");
}

std::optional<PeerAddr> NwUdpListener::run_end_locked(opennova::net::Socket &socket,
                                                      const lobbydb::Command &command) {
	const PeerAddr *peer = peer_of_session_locked(command.session_id);
	if (peer == nullptr) {
		std::printf("[nwudp] END of session %lld dropped: the connection changed\n",
		            static_cast<long long>(command.session_id));
		return std::nullopt;
	}
	const std::string addr = peer_addr_to_string(*peer);
	const auto it = lobby_states_.find(*peer);
	if (it != lobby_states_.end() && it->second.lobby.hosting) {
		std::printf("[nwudp] END %s dropped: the connection hosts now\n", addr.c_str());
		return std::nullopt;
	}
	std::size_t sent = 0;
	if (it != lobby_states_.end()) {
		const LobbyConnState &state = it->second;
		const std::vector<uint8_t> datagram = nw_encode_outbound(
				SESSION_OPCODE_SERVER_GOODBYE,
				server_goodbye_to_bytes(state.client_ck,
						make_disconnect_event(1, 8, 0, 0, "", 0, "NP.C:PMP:PDESTME")));
		const auto to = opennova::net::NetDatagramSocket::to_endpoint(*peer);
		const std::size_t count = disconnect_burst_count(state.cs.recv_max_per_tick);
		for (; sent < count; ++sent) {
			if (opennova::net::udp_send_to(socket, to, datagram.data(), datagram.size()) < 0) break;
		}
	}
	std::printf("[nwudp] END %s: ServerGoodBye x%zu, dropped\n", addr.c_str(), sent);
	return *peer;
}

void NwUdpListener::forget_owner_locked(db::Database &db, int64_t user_id) {
	std::vector<PeerAddr> changed;
	for (auto &[peer, state] : lobby_states_) {
		if (state.lobby.host_user_id != user_id) continue;
		state.lobby.host_user_id.reset();
		std::printf("[nwudp] rid=%u addr=%s no longer names account %lld as its owner\n",
		            state.lobby.rid, peer_addr_to_string(peer).c_str(),
		            static_cast<long long>(user_id));
		changed.push_back(peer);
	}
	for (auto &[peer, parked] : parked_lobby_states_) {
		if (parked.lobby.host_user_id == user_id) parked.lobby.host_user_id.reset();
	}
	for (const PeerAddr &peer : changed) sync_session_locked(peer, &db);
}

// PumpSendIntervals' ACTIVE leg on the service's end of each lobby connection, as the NAPI server
// role runs it: reliable records still retained (not yet ACKed) and more than CS field 5 since the
// connection's last framed packet -> a build with nothing queued, one header-only packet, and
// again every interval while the records stay unACKed. Its fresh sequence shows the client a gap
// below it when the packet before was lost, and the client's receive batch asks for the gap with
// a 0x44, which the resend leg answers from the retained records. A header-only packet draws no
// ACK from a stock client, so if the client's ACK itself is lost the leg sends one packet per
// interval (1000 ms) until the client's next packet carries the ACK (its EMPTY keepalive, field 4,
// every 60 s) or the connection is reaped (field 0, 240 s). A reply always has the client's next
// statement behind it to surface a loss; a push may have nothing, and without this leg a lost one
// would wait for the host's next refresh. The ACTIVE leg alone runs here: the server role's EMPTY
// keepalive (field 4) is skipped, a deliberate divergence (D-NET-393: the service answers every
// inbound SESSION packet, so a live client's own keepalive already draws a packet back), and the
// timed missing-sequence leg is off in the template (field 6 = -1).
// [orig: CNapiNPConnection_PumpStateMachine @0x6292e0 case 1 (the server-side connection) ->
//  CNapiNPConnection_PumpSendIntervals @0x62933f, the in-match host's port D-NET-256;
//  PumpSendIntervals @0x628fd0 — the active leg @0x628ff1..0x629017, the build
//  @0x62906f..0x629089; CNapiGameSession_InitNPConnection @0x4d3e1f (the template)]
void NwUdpListener::pump_send_intervals(opennova::net::Socket &socket) {
	const uint64_t now = now_ms();
	std::lock_guard<std::mutex> lk(lobby_states_mu_);
	for (auto &[peer, state] : lobby_states_) {
		if (state.cs.active_send_interval_ms < 0) continue;
		if (state.sequencing.retained_outbound_message_count == 0) continue;
		if (now - state.last_framed_send_ms <=
		    static_cast<uint64_t>(state.cs.active_send_interval_ms)) {
			continue;
		}
		send_lobby_records(socket, opennova::net::NetDatagramSocket::to_endpoint(peer), state,
		                   state.server_scrk, {}, nullptr);
	}
}

// The receive batch's end: a lobby connection that saw a packet past a gap and still holds one
// asks for the gap with one 0x84 keyed by the client's CK.
// [orig: NapiNPProtocol_PumpRecvQueues @0x6269bb..0x6269d6 ->
//  CNapiNPConnection_SendMissingSeqList @0x623560]
void NwUdpListener::send_missing_requests(opennova::net::Socket &socket) {
	std::lock_guard<std::mutex> lk(lobby_states_mu_);
	for (auto &entry : lobby_states_) {
		LobbyConnState &state = entry.second;
		if (!state.sequencing.missing_request_pending)
			continue;
		state.sequencing.missing_request_pending = false;
		if (state.sequencing.queued_inbound.empty())
			continue;
		std::vector<uint8_t> missing_body;
		if (!encode_session_resend_list(state.client_ck,
					build_session_missing_sequence_list(state.sequencing, false), missing_body)) {
			continue;
		}
		const auto packet =
				nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, std::move(missing_body));
		const opennova::net::Endpoint to =
				opennova::net::NetDatagramSocket::to_endpoint(entry.first);
		opennova::net::udp_send_to(socket, to, packet.data(), packet.size());
	}
}

void NwUdpListener::run_loop(opennova::net::ScopedSocket socket,
                             std::optional<db::ConnectionPool::Lease> db_conn) {
	// The receive thread's own connection: the lobby session's host rows, the session rows, the
	// commands. None in the tests that bring up no database.
	db::Database *const loop_db = db_conn ? db_conn->get() : nullptr;
	lobby_session_.set_database(loop_db);

	QueuedJoSocket jo_socket(socket.get());
	using PumpClock = std::chrono::steady_clock;
	const auto pump_period =
			std::chrono::nanoseconds(1000000000LL / opennova::io::kTicksPerSecondInt);
	auto next_pump = PumpClock::now();

	uint8_t rx[4096];
	bool receive_batch_active = false;
	std::size_t batch_datagrams = 0;
	while (!stop_requested_.load()) {
		loop_turns_.fetch_add(1, std::memory_order_relaxed);
		// A batch that took kReceiveBatchMaxDatagrams ends here as at the empty read, so a flood
		// that never empties the socket still lets the legs below run.
		if (receive_batch_active && batch_datagrams >= kReceiveBatchMaxDatagrams) {
			send_missing_requests(socket.get());
			receive_batch_active = false;
		}
		if (!receive_batch_active) {
			batch_datagrams = 0;
			const auto now = PumpClock::now();
			if (now >= next_pump) {
				if (jo_owner_) {
					inmatch::host_session_pump(
							*jo_owner_, jo_socket, nullptr, nullptr,
							&NwUdpListener::observe_jo_event, this);
					// The frame's weapon-action walk follows the server
					// tick's entity pass on every host; this one has no
					// weather or view legs to wait for.
					// [orig: Game_ProcessMainFrame -- the
					//  WeaponAction_ProcessAllEntities call @0x526786]
					jo_world_->pump_weapon_actions();
				}
				do {
					next_pump += pump_period;
				} while (next_pump <= now);
			}
			// The lobby connections' own sends between receive batches: the commands addressed
			// to the lobby (the pushes to a hosting connection, the sessions an admin ended) every
			// kCommandDrainMs, then the send-interval leg; every kSessionSeenWriteMs the
			// sessions' last-heard times; every kInstanceTouchSeconds the instance's heartbeat.
			if (loop_db != nullptr) {
				const uint64_t ms = now_ms();
				if (ms - last_command_drain_ms_ >= kCommandDrainMs) {
					last_command_drain_ms_ = ms;
					drain_commands(socket.get(), *loop_db);
				}
				if (ms - last_seen_write_ms_ >= kSessionSeenWriteMs) {
					last_seen_write_ms_ = ms;
					write_sessions_seen(*loop_db);
				}
				if (ms - last_instance_touch_ms_ >=
						static_cast<uint64_t>(lobbydb::kInstanceTouchSeconds) * 1000) {
					last_instance_touch_ms_ = ms;
					touch_instance(*loop_db);
				}
			}
			pump_send_intervals(socket.get());
		}

		opennova::net::Endpoint from{};
		// Block for the first datagram of a receive batch, then switch to non-blocking reads until
		// the OS FIFO is empty (or the batch took kReceiveBatchMaxDatagrams). That empty read is
		// the same boundary where retail drains queued contiguous session packets and decides
		// whether a surviving gap needs one 0x84.
		// [orig: NapiNPProtocol_PumpRecvQueues @0x6266A0..0x6269D6]
		const auto until_pump = std::chrono::duration_cast<std::chrono::milliseconds>(
				next_pump - PumpClock::now()).count();
		const int timeout_ms = receive_batch_active
				? 0
				: static_cast<int>(std::max<int64_t>(
						1, std::min<int64_t>(16, until_pump + 1)));
		const int n = opennova::net::udp_recv_from(socket.get(), rx, sizeof(rx),
		                                           from, timeout_ms);
		if (n <= 0) {
			if (receive_batch_active)
				send_missing_requests(socket.get());
			receive_batch_active = false;
			continue;
		}
		receive_batch_active = true;
		++batch_datagrams;

		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(rx, static_cast<size_t>(n), opcode, body)) {
			// A line per kBadEnvelopeLogMs at most: a flood of garbage must not cost an
			// unbuffered write per datagram.
			const uint64_t now = now_ms();
			if (last_bad_envelope_log_ms_ != 0 &&
					now - last_bad_envelope_log_ms_ < kBadEnvelopeLogMs) {
				++bad_envelopes_unlogged_;
				continue;
			}
			std::fprintf(stderr,
					"[nwudp] %s:%u — bad envelope (%d bytes; %llu more unlogged since)\n",
					opennova::net::endpoint_to_string(from).c_str(), from.port, n,
					static_cast<unsigned long long>(bad_envelopes_unlogged_));
			last_bad_envelope_log_ms_ = now;
			bad_envelopes_unlogged_ = 0;
			continue;
		}

		const PeerAddr peer = peer_addr_from_octets(from.ip, from.port);
		const uint32_t client_ip_net = peer.ip;
		const uint16_t client_port = from.port;
		const auto client_label = opennova::net::endpoint_to_string(from);
		// The rendering the session rows and a listing's host_ip carry
		// (peer_addr_ip_to_string): a joiner's PlaySetup names the listing by it
		// (hostdb::rid_at), so the two must agree byte for byte.
		const std::string client_ip_str = peer_addr_ip_to_string(peer);

		// Demultiplex the in-match game protocol onto the shared authoritative host
		// pump. A hello's PN names its title; a route is installed only for a hello
		// carrying that title's complete in-match identity, when the host speaks that
		// title; subsequent opcodes stay on that route so the lobby path never sees
		// game SCRKs.
		bool route_jo = jo_peers_.count(peer) != 0;
		if (opcode == SESSION_OPCODE_CLIENT_HELLO && jo_owner_) {
			ClientHello probe;
			if (parse_client_hello(body.data(), body.size(), probe)) {
				const SessionProtocol protocol = classify_session_protocol(probe.pn);
				const TitleIdentity *title = protocol.title;
				if (title != nullptr && title == jo_owner_->ctx.np_protocol.identity &&
				    title_identity_complete(*title, TitleIdentityPart::Session) &&
				    matches_title_identity(*title, probe)) {
					route_jo = true;
					jo_peers_.insert(peer);
				}
			}
		}
		if (route_jo && jo_owner_) {
			jo_socket.push(peer, rx, static_cast<std::size_t>(n));
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
			const SessionProtocolKind protocol = classify_session_protocol(hello.pn).kind;
			if (protocol == SessionProtocolKind::Unsupported) {
				std::fprintf(stderr, "[nwudp] %s — refusing PN='%s'\n", client_label.c_str(),
						loggable(hello.pn).c_str());
				if (tracker_) {
					tracker_->record("pn", hello.pn, body.data(), body.size(),
					                 client_label, static_cast<uint64_t>(unix_now_ms()));
				}
				break;
			}
			Connection conn;
			conn.id = hello.ci;
			conn.reported_id = hello.ci;
			conn.addr = peer;
			conn.state = ConnectionState::Handshaking;
			conn.created_ms = now_ms();
			conn.last_seen_ms = conn.created_ms;
			conn.pn = hello.pn;
			manager_.notify_handshake(conn);
			{
				// Insert after notify_handshake: a replacement Hello can fire
				// on_lost synchronously for a prior Handshaking occupant, whose
				// cleanup removes the old ownership record and its session row (a
				// live row is only re-named, D-NET-630, which its row follows).
				std::lock_guard<std::mutex> lk(lobby_states_mu_);
				lobby_peers_.emplace(peer, LobbyPeer{});
				sync_session_locked(peer, loop_db);
			}

			ServerHello reply = build_server_hello(hello, client_ip_net,
			                                       client_port);
			auto packet = nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO,
			                                 server_hello_to_bytes(reply));
			opennova::net::udp_send_to(socket.get(), from, packet.data(),
			                           packet.size());
			std::printf("[nwudp] HELLO from %s ci=0x%08x pn=%s -> ServerHello (%zu B)\n",
					client_label.c_str(), hello.ci, loggable(hello.pn).c_str(), packet.size());
			break;
		}

		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth auth;
			if (!parse_client_auth(body.data(), body.size(), auth)) {
				std::fprintf(stderr, "[nwudp] %s — bad ClientAuth\n",
				             client_label.c_str());
				break;
			}
			// Lobby Auth is not stateless. It belongs only to the exact
			// ClientHello-owned address/CI/protocol row created above. Without
			// this gate, a direct 0x42 minted ServerAuth and LobbyConnState even
			// though mark_active_by_addr had no manager row to promote; that
			// zombie state could neither accept SESSION nor expire.
			const std::optional<Connection> hello_owner =
					manager_.registry().find_by_addr(peer);
			const bool hello_identity_matches =
					hello_owner.has_value() &&
					hello_owner->reported_id == auth.ci &&
					hello_owner->pn == auth.pn &&
					classify_session_protocol(hello_owner->pn).kind ==
							SessionProtocolKind::Lobby;
			// A retransmit is the live connection's own CI and CK: once a ClientAuth
			// has minted this endpoint's session material, its repeat replays the same
			// ServerAuth without disturbing lobby, sequence, or reassembly state, so a
			// delayed first 0x82 remains safe for the client to accept after the retry.
			// Retail's retransmit leg reads the connection alone, no Hello state, so a
			// stray Hello in between (which re-names the row's Hello identity only)
			// does not stop it. [orig: NapiNPProtocol_HandleClientJoin - conn_state ==
			//  1 && CI && CK -> CNapiNPConnection_SendSessionInit @0x62BEE0..0x62BEE6]
			const bool live_connection = hello_owner.has_value() &&
					hello_owner->state == ConnectionState::Active;
			std::vector<uint8_t> cached_server_auth;
			{
				std::lock_guard<std::mutex> lk(lobby_states_mu_);
				const auto it = lobby_states_.find(peer);
				if (it != lobby_states_.end() &&
				    it->second.client_ci == auth.ci &&
				    it->second.client_ck == auth.ck &&
				    !it->second.server_auth_datagram.empty()) {
					cached_server_auth = it->second.server_auth_datagram;
				}
			}
			if (live_connection && !cached_server_auth.empty()) {
				manager_.notify_seen_addr(peer, now_ms());
				opennova::net::udp_send_to(
						socket.get(), from, cached_server_auth.data(),
						cached_server_auth.size());
				std::printf(
						"[nwudp] AUTH retry from %s ci=0x%08x ck=0x%08x"
						" -> cached ServerAuth (%zu B)\n",
						client_label.c_str(), auth.ci, auth.ck,
						cached_server_auth.size());
				break;
			}

			// The row this address's Hello owns may still be Active: a stock client
			// that died without its goodbye restarts on the same endpoint with the
			// same CI, and its Hello leaves the live row in place. A 0x42 that is
			// not the cached retransmit is then a new connection that replaces the
			// old one, as retail's join handler replaces any node that is not the
			// same CI+CK retransmit (no Hello state is consulted).
			// [orig: NapiNPProtocol_HandleClientJoin @0x62beba..0x62bf06]
			if (!hello_identity_matches ||
			    (hello_owner->state != ConnectionState::Handshaking &&
			     hello_owner->state != ConnectionState::Active)) {
				std::printf("[nwudp] AUTH without matching lobby Hello from %s"
							" ci=0x%08x pn=%s; dropped\n",
						client_label.c_str(), auth.ci, loggable(auth.pn).c_str());
				break;
			}

			// Any non-identical auth for this endpoint is a new logical
			// connection. Retire its prior lobby/DB state before installing a
			// fresh key set and fresh sequence frontier.
			erase_lobby_state(peer, "reauth", loop_db);

			// A re-join that counted a disconnect takes back the identity its dropped
			// connection parked; any other join from the address discards it.
			std::optional<LobbyState> resumed_lobby;
			{
				std::lock_guard<std::mutex> lk(lobby_states_mu_);
				const auto park = parked_lobby_states_.find(peer);
				if (park != parked_lobby_states_.end()) {
					if (static_cast<int32_t>(auth.dcnt) > 0 && park->second.client_ci == auth.ci &&
					    now_ms() - park->second.parked_ms <= kParkedLobbyTtlMs) {
						resumed_lobby = park->second.lobby;
					}
					parked_lobby_states_.erase(park);
				}
			}
			// [orig: HandleClientJoin @0x62c28d..0x62c2a3 — RCNT = the client's + 1 when DCNT > 0]
			const uint32_t rcnt =
					static_cast<int32_t>(auth.dcnt) > 0 ? auth.rcnt + 1u : auth.rcnt;

			const std::string server_scrk = make_dev_scrk();
			const std::string nwuid = make_dev_nwuid();
			// auth.scrk is the CLIENT-generated session key — store it for
			// decrypting inbound SESSION packets. Our locally-generated
			// server_scrk is what we encrypt outbound SESSION with AND
			// what we echo back in ServerAuth.scrk so the client can
			// decrypt our replies.
			//
			// Addr-keyed (G.7): auth.ci collides between two retail
			// processes — an id-keyed promote aliased instance 1's
			// connection record onto instance 2's, so subsequent
			// find_by_addr(instance_1) returned instance 2's scrk and
			// retail #1 got "INCOMING PACKET ERROR" on every reply.
			// Stash the client's CK + our generated server SK in the
			// per-connection lobby state. CK feeds outbound SESSION headers
			// (retail's validator wants session_id == its own local_key ==
			// its CK). Server SK is what we advertise in ServerAuth.SK and
			// what retail echoes back as the inbound session_id; we keep it
			// per-connection (was hardcoded 0xC0FFEE00 before — see G.2).
			//
			// Keyed by PeerAddr (G.7): two retail processes both send
			// ci=0x00000001, so keying by ci would have the second AUTH
			// overwrite the first's per-connection state. The SK is drawn
			// until nonzero. [orig: NapiNPProtocol_HandleClientJoin @0x62BF5E
			//  -> NapiNP_GenerateSessionKey]
			const uint32_t server_sk = make_session_key();
			ServerAuth reply = build_server_auth(auth, client_ip_net,
			                                     client_port, server_sk,
			                                     server_scrk,
			                                     /*novaworld_name=*/"NWServer",
			                                     /*novaworld_web_url=*/web_domain_,
			                                     /*nwuid=*/nwuid);
			reply.rcnt = rcnt; // [orig: SendSessionInit @0x62125d]
			auto packet = nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH,
			                                 server_auth_to_bytes(reply));
			{
				std::lock_guard<std::mutex> lk(lobby_states_mu_);
				LobbyConnState state;
				// The connection runs the NOVAWORLDUDP template its 0x82 advertised: the ordered
				// receive gate bounded by field 10, the reply records retained for the client's
				// 0x44 bounded by field 11 (500), the replies split at field 13.
				// [orig: CNapiGameSession_InitNPConnection @0x4d3be0 writes the template;
				//  NapiNPProtocol_HandleSessionPacket @0x626be0..0x626c3a (the gate);
				//  NapiNP_HandleResendList @0x623800 resends from the retained nodes]
				state.sequencing.ordered_recovery_enabled = true;
				sync_session_sequencing_limits(state.sequencing, state.cs);
				state.client_ci = auth.ci;
				state.client_ck = auth.ck;
				state.server_sk = server_sk;
				state.dcnt = auth.dcnt;
				state.rcnt = rcnt;
				if (resumed_lobby) {
					// The identity comes back unhosted: the re-host's ClientHostRequest
					// (CurrentlyHosting=1) re-registers it under the kept RID / GSID.
					state.lobby = std::move(*resumed_lobby);
					state.lobby.hosting = false;
				}
				state.server_scrk = server_scrk;
				state.server_auth_datagram = packet;
				// Auth is still lobby-routed even if a client skipped/reordered
				// Hello. Preserve the teardown invariant for every state entry.
				lobby_peers_.emplace(peer, LobbyPeer{});
				lobby_states_[peer] = std::move(state);
			}
			manager_.notify_active_addr(peer, /*identity*/ auth.na,
			                            /*client_scrk=*/auth.scrk,
			                            /*server_scrk=*/server_scrk);
			manager_.notify_seen_addr(peer, now_ms());
			{
				// The new connection's session row (the reauth erase above took the
				// previous one), before its 0x82 goes out.
				std::lock_guard<std::mutex> lk(lobby_states_mu_);
				sync_session_locked(peer, loop_db);
			}

			std::printf("[nwudp] AUTH scrks: client_scrk=%zuB server_scrk=%zuB ck=0x%08x\n",
			            auth.scrk.size(), server_scrk.size(), auth.ck);
			opennova::net::udp_send_to(socket.get(), from, packet.data(),
			                           packet.size());
			std::printf("[nwudp] AUTH from %s ci=0x%08x na='%s' -> ServerAuth (%zu B)\n",
					client_label.c_str(), auth.ci, loggable(auth.na).c_str(), packet.size());
			break;
		}

		case SESSION_OPCODE_PROTOCOL_MESSAGE: {
			// The 0x43 needs a connection whose 0x42 was accepted (the Active row the AUTH
			// arm marks), whatever key that 0x42 carried: an empty SCRK keys the crypt like
			// any other (D-NET-450).
			// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00 - conn_flag0
			//  @0x626AD7..0x626AE4, no key test before the local-key match @0x626B72]
			auto conn_opt = manager_.registry().find_by_addr(peer);
			if (!conn_opt || conn_opt->state != ConnectionState::Active) {
				// SESSION before AUTH completed — drop quietly.
				std::printf("[nwudp] SESSION before AUTH from %s; dropped\n",
				            client_label.c_str());
				break;
			}

			std::printf("[nwudp] SESSION from %s body=%zuB scrk=client(%zuB)\n",
			            client_label.c_str(), body.size(),
			            conn_opt->client_scrk.size());

			ProtocolPacketHeader hdr;
			if (!parse_protocol_packet_header(body.data(), body.size(), hdr)) {
				std::fprintf(stderr, "[nwudp] %s — bad SESSION envelope\n",
				             client_label.c_str());
				break;
			}

			// Look up the AUTH-created per-connection lobby state, keyed by the
			// peer address (NOT ClientHello.ci — both retail processes send
			// ci=0x00000001 so keying by ci aliased their state, see G.7).
			// Hold the lock for the WHOLE dispatch — the HTTP thread's
			// snapshot_hosted() reads the same LobbyState (std::string /
			// std::map fields), and a race against the dispatch's writes
			// manifests as a SIGSEGV on /api/hosts under load (2026-04-28).
			std::lock_guard<std::mutex> dispatch_lk(lobby_states_mu_);
			auto lobby_it = lobby_states_.find(peer);
			if (lobby_it == lobby_states_.end()) {
				std::printf("[nwudp] SESSION without lobby auth state from %s; dropped\n",
				            client_label.c_str());
				break;
			}
			auto &lobby_state = lobby_it->second;
			// Inbound SESSION addresses our receiver-local key, the SK we
			// advertised in ServerAuth. Reject a foreign/stale connection
			// before sequence, reassembly, or lobby state can advance.
			if (hdr.session_id != lobby_state.server_sk) {
				std::printf(
						"[nwudp] SESSION key mismatch from %s got=0x%08x expected=0x%08x; dropped\n",
						client_label.c_str(), hdr.session_id, lobby_state.server_sk);
				break;
			}
			std::vector<ProtocolMessage> messages;
			SessionDeframeAdmission admission;
			if (!deframe_session_packet(
					lobby_state.sequencing,
					SessionCrypto{{}, conn_opt->client_scrk, 0,
					              lobby_state.server_sk},
					body.data(), body.size(), hdr, messages, &admission)) {
				std::fprintf(stderr, "[nwudp] %s — bad SESSION envelope\n",
				             client_label.c_str());
				break;
			}
			// Only a receiver-key-valid, successfully authenticated SESSION is
			// activity on this connection. Correct-key stale/duplicate packets
			// still count, but a prior endpoint occupant cannot extend liveness.
			manager_.notify_seen_addr(peer, now_ms());
			// The client's ACK retires the retained reply records it covers.
			if (admission.admitted) {
				acknowledge_session_packets(lobby_state.sequencing, admission.max_ack_count);
			}
			std::printf(
					"[nwudp]   hdr.session_id=0x%08x seq=%u ack=%u admitted=%d messages=%zu\n",
					hdr.session_id, hdr.seq_num, hdr.ack_count,
					admission.admitted ? 1 : 0, messages.size());

			std::vector<ProtocolMessage> replies;
			// The session's protocol is its accepted 0x42's; a later Hello re-names only
			// the row's Hello identity (D-NET-630).
			const SessionProtocolKind protocol =
					classify_session_protocol(conn_opt->session_pn).kind;
			// In-match peers of the host's title are routed to the shared HostOwner above and
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
							                 client_label, static_cast<uint64_t>(unix_now_ms()));
						}
						continue;                            // only Layer-4 lobby
					}

					// Fragment reassembly: a statement longer than the client's CS field 13
					// (a cookie-heavy ClientRequestVerifyResult or ClientHostRequest) spans
					// several SESSION packets, FRAG_CONT set on every piece but the final.
					// reassemble_protocol_payload accumulates and returns true only when the
					// assembled buffer is ready; a statement past kLobbyStatementMaxBytes
					// is dropped.
					std::vector<uint8_t> assembled;
					bool was_fragmented = false;
					bool was_dropped = false;
					if (!reassemble_protocol_payload(lobby_state.reassembly, pm,
					                                 assembled, &was_fragmented, &was_dropped)) {
						if (was_dropped) {
							std::printf("[nwudp]     fragment dropped (the statement passes %zu bytes)\n",
							            kLobbyStatementMaxBytes);
						} else {
							std::printf("[nwudp]     fragment buffered (assembly=%zuB so far)\n",
							            lobby_state.reassembly.buffer.size());
						}
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
							const auto maintenance =
									load_maintenance_status(loop_db);
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
									client_label.c_str(), loggable(result.label).c_str(),
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
								                 client_label, static_cast<uint64_t>(unix_now_ms()));
							}
						}
						for (auto &reply_container : result.reply_containers) {
							if (auto rpm = lobby_record(std::move(reply_container))) {
								replies.push_back(std::move(*rpm));
							}
						}
					}
				}
			} else {
				std::fprintf(stderr, "[nwudp] %s — unsupported SESSION PN='%s'\n",
						client_label.c_str(), loggable(conn_opt->session_pn).c_str());
				if (tracker_) {
					tracker_->record("pn", conn_opt->session_pn, body.data(), body.size(),
					                 client_label, static_cast<uint64_t>(unix_now_ms()));
				}
			}

			// The session row follows what the statements changed (its login, its hosting, its
			// listing), before the replies go out.
			sync_session_locked(peer, loop_db);
			// Every SESSION is answered, header-only when nothing replies (send_lobby_records).
			send_lobby_records(socket.get(), from, lobby_state, conn_opt->server_scrk, replies,
			                   "reply");
			break;
		}

		case SESSION_OPCODE_CLIENT_RESEND_LIST: {
			// A stock client's NWU connection asks for the sequences missing below its
			// queued packets: resend each from its retained records under the old
			// sequence with the current ACK, 0 meaning "send the next one". The list
			// addresses this receiver by its SK, and it is not receive activity.
			// [orig: NapiNP_HandleResendList @0x623800 — the key compare, the loop
			//  @0x6239b6 -> CNapiNPConnection_SendSessionPacket, GetTickCount discarded
			//  @0x62395f; the requester PumpSendIntervals @0x629032]
			const auto conn_opt = manager_.registry().find_by_addr(peer);
			if (!conn_opt || conn_opt->server_scrk.empty()) break;
			std::lock_guard<std::mutex> resend_lk(lobby_states_mu_);
			const auto it = lobby_states_.find(peer);
			if (it == lobby_states_.end()) break;
			LobbyConnState &state = it->second;
			std::vector<uint32_t> requested;
			if (!decode_session_resend_list(body.data(), body.size(), state.server_sk, requested)) {
				break;
			}
			for (const uint32_t requested_sequence : requested) {
				const uint32_t sequence = requested_sequence == 0
						? state.sequencing.next_outbound_seq
						: requested_sequence;
				std::vector<uint8_t> session_body;
				if (!frame_session_packet_for_sequence(state.sequencing,
						SessionCrypto{conn_opt->server_scrk, {}, state.client_ck}, sequence,
						session_body)) {
					continue;
				}
				const auto packet = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
				                                       std::move(session_body));
				opennova::net::udp_send_to(socket.get(), from, packet.data(), packet.size());
			}
			std::printf("[nwudp] RESEND LIST from %s: %zu sequence(s)\n",
			            client_label.c_str(), requested.size());
			break;
		}

		case SESSION_OPCODE_CLIENT_GOODBYE: {
			// The leading dword addresses this receiver by the per-peer SK
			// advertised in ServerAuth. Trailing disconnect TLVs are lenient,
			// but malformed and stale-key packets must not evict a live peer.
			if (body.size() < 4) {
				std::printf("[nwudp] malformed GOODBYE from %s; dropped\n",
				            client_label.c_str());
				break;
			}
			const uint32_t remote_key =
					static_cast<uint32_t>(body[0]) |
					(static_cast<uint32_t>(body[1]) << 8) |
					(static_cast<uint32_t>(body[2]) << 16) |
					(static_cast<uint32_t>(body[3]) << 24);
			uint32_t expected_key = 0;
			{
				std::lock_guard<std::mutex> lk(lobby_states_mu_);
				const auto it = lobby_states_.find(peer);
				if (it == lobby_states_.end()) {
					std::printf(
							"[nwudp] GOODBYE without lobby auth state from %s; dropped\n",
							client_label.c_str());
					break;
				}
				expected_key = it->second.server_sk;
			}
			if (remote_key != expected_key) {
				std::printf(
						"[nwudp] GOODBYE key mismatch from %s got=0x%08x expected=0x%08x; dropped\n",
						client_label.c_str(), remote_key, expected_key);
				break;
			}
			std::printf("[nwudp] GOODBYE from %s sk=0x%08x\n",
			            client_label.c_str(), remote_key);
			// Addr-keyed (G.7): two retail processes both ship ci=1, so a
			// ci-keyed logout would drop the wrong connection.
			manager_.notify_logout_addr(peer);
			// notify_logout_addr fires on_lost which calls
			// erase_lobby_state via main.cpp's wiring (G.6).
			// Belt-and-braces erase here too in case the handler isn't
			// installed.
			erase_lobby_state(peer, "goodbye", loop_db);
			break;
		}

		default:
			std::fprintf(stderr, "[nwudp] %s — unhandled opcode 0x%02x\n",
			             client_label.c_str(), opcode);
			if (tracker_) {
				tracker_->record("nwu", hex_sig(opcode), body.data(), body.size(),
				                 client_label, static_cast<uint64_t>(unix_now_ms()));
			}
			break;
		}
	}

	lobby_session_.set_database(nullptr);
	std::printf("[nwudp] loop exiting\n");
}

} // namespace opennova::novaworld_server
