#include "gate_listener.h"

#include "core/loggable.h"
#include "program/server_config.h"

#include <net_sockets.h>
#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_metrics.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>
#include <net/novaworld/lobby_update.h>
#include <net/novaworld/unknown_tracker.h>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

namespace {

// Build a VAR-encoded gate response.
//
// Wire shape (all CRLF-terminated):
//   GATEPROTOCOL "1.0"
//   VAR "key" "value"   <- keys + values double-quoted
//   ...
//
// Only keys the retail parser recognises are emitted — it knows exactly
// nineteen (case-insensitive) and skips every other VAR line without
// counting it [orig: CNapiGateManager_ProcessResponse @0x4ced20]:
// POSTIPADDRESS, POSTIPPORT, LOBBYNAME, METIPADDRESS, METIPPORT, METLABEL,
// METPING, METEXT, STARTUPURL, UDPNOVAWORLD, UDPCODE1, UDPCODE2,
// REFLECTEDIPADDRESS, REFLECTEDPORTNUMBER, USEJUNCTION, CLEARJUNCTION,
// GLSVSSREQUEST, GLSVSSRIMS, GLSVSSAGRMS. (NovaworldName and
// NovaworldWebDomainNameAndPortNumber are 0x82 SessionInit CU chunks, not
// gate VARs [orig: CNapiGameSession_OnNovaWorldConnected @0x4d1570].)
//
// POSTIPADDRESS + POSTIPPORT: the parser fails the gate to state -9 ("NO NW
// POST IP" / "NO NW POST PORT") without them unless the NWU-in-use word is
// set, which a UDPNOVAWORLD line (this reply always has one) or the junction
// bypass sets [orig: CNapiGateManager_ProcessResponse @0x4ced20]. A host posts
// its status blob there when it is off NWU [orig: Lobby_UpdateServerInfo
// @0x4ff441..0x4ff62c], so the pair names this listener.
//
// LobbyName, the startupurl's query and the GLSVSS leg are the probe's games
// row's (lobby_name, startup_query, glsvss_*): JO's row answers
// "jop_2_consumer" and "...&gt=[GT]&url=jop_2_start.htm" with no GLSVSS, the
// bytes this reply always carried; DFX2's row answers as NovaLogic's live gate
// answered the DFX2 tag (dfx2-vs-jo-re.md section 6.5, DFX2-6): LobbyName
// "dfx2_0_consumer", pfid=38 in the startupurl, GLSVSS "[DFX2]KILLS" 3600000
// 45000, the GLSVSS lines before the reflected pair as that reply has them.
// A title whose live reply is witnessed (TitleIdentity::gate_reply_witnessed,
// DFX2's) gets that reply's shape too (D-NET-721): no POST pair (its
// udpnovaworld line skips the client's POST tests, and a host on NWU posts no
// status blob), the startupurl on http://[domainname]/ (the client fills the
// domain from the lobby's 0x82 NovaworldWebDomainNameAndPortNumber, which
// names the same web endpoint), and a TimeOnGateServer line, which no parse
// reads, after the reflected pair: the gate's clock in asctime's shape (the
// zone is this service's choice, UTC; the witness's is unrecorded). JO's live
// reply is unrecorded, so its rows keep the POST pair, this service's address
// in the startupurl and no TimeOnGateServer.
//
// Every address it names comes from the config or the lobby instances' rows,
// never from a port another listener bound (the lobbies and web run in other
// processes): the POST pair is this gate (ONNET_PUBLIC_HOST and its own port),
// udpnovaworld the lobby address every live instance announces (with none,
// the gate's fallback ONNET_LOBBY_ENDPOINT, and with no fallback no reply) and
// the startupurl the one web endpoint (ONNET_WEB_HOST, ONNET_HTTP_PORT).
struct GateResponseFields {
	std::string lobby_name;
	std::string startup_query;
	std::string public_host;
	uint16_t post_port = 0;      // this listener's port
	std::string lobby_endpoint; // <host>:<port>
	std::string web_host;
	uint16_t http_port = 0;
	std::string reflected_ip;
	uint16_t reflected_port = 0;
	std::string met_ip;
	uint16_t met_port = 0;
	std::string met_label;
	int met_ping = 0;
	int met_ext = 0;
	std::string glsvss_request;
	int64_t glsvss_rims = 0;
	int64_t glsvss_agrms = 0;
	// The witnessed live shape (TitleIdentity::gate_reply_witnessed) and its
	// TimeOnGateServer value.
	bool witnessed_shape = false;
	std::string time_on_gate_server;
};

// The gate's clock as C's asctime spells it ("Wed Oct 14 07:50:11 2026", the
// day space-padded), with no newline, in UTC.
std::string gate_clock_text(std::time_t now) {
	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char text[64] = {};
	std::strftime(text, sizeof text, "%a %b %e %H:%M:%S %Y", &utc);
	return text;
}

std::string build_gate_response(const GateResponseFields &f) {
	auto append_var = [](std::ostringstream &os, const char *key, const std::string &value) {
		os << "VAR \"" << key << "\" \"" << value << "\"\r\n";
	};

	std::ostringstream os;
	os << "GATEPROTOCOL \"1.0\"\r\n";
	append_var(os, "LobbyName", f.lobby_name);
	if (!f.witnessed_shape) {
		append_var(os, "POSTIPADDRESS", f.public_host);
		append_var(os, "POSTIPPORT", std::to_string(f.post_port));
	}
	if (!f.met_ip.empty()) {
		append_var(os, "METIPADDRESS", f.met_ip);
		append_var(os, "METIPPORT", std::to_string(f.met_port));
		append_var(os, "METLABEL", f.met_label);
		append_var(os, "METPING", std::to_string(f.met_ping));
		append_var(os, "METEXT", std::to_string(f.met_ext));
	}
	append_var(os, "udpnovaworld", f.lobby_endpoint);
	append_var(os, "udpcode1", "abc");
	append_var(os, "udpcode2", "xyz");
	const std::string web = f.witnessed_shape
			? std::string("[domainname]")
			: f.web_host + ":" + std::to_string(f.http_port);
	append_var(os, "startupurl", "http://" + web + "/nwprepare.dll?" + f.startup_query);
	append_var(os, "usejunction", "0");
	append_var(os, "clearjunction", "0");
	if (!f.glsvss_request.empty()) {
		append_var(os, "GLSVSSREQUEST", f.glsvss_request);
		append_var(os, "GLSVSSRIMS", std::to_string(f.glsvss_rims));
		append_var(os, "GLSVSSAGRMS", std::to_string(f.glsvss_agrms));
	}
	append_var(os, "ReflectedIpAddress", f.reflected_ip);
	append_var(os, "ReflectedPortNumber", std::to_string(f.reflected_port));
	if (f.witnessed_shape)
		append_var(os, "TimeOnGateServer", f.time_on_gate_server);
	return os.str();
}

} // namespace

GateListener::GateListener() = default;

GateListener::~GateListener() {
	stop();
}

bool GateListener::start(const ServerConfig &config) {
	if (running_.load()) {
		return true;
	}

	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "[gate] net::startup failed\n");
		return false;
	}

	// The socket stays bound from here into the receive thread, so the port
	// reported (and advertised as POSTIPPORT) is the port served: a close and
	// a re-bind would free it in between, and with port 0 take another one.
	uint16_t bound = 0;
	opennova::net::ScopedSocket socket(opennova::net::udp_bind(config.gate_udp_port, &bound));
	if (!socket.is_valid()) {
		std::fprintf(stderr, "[gate] failed to bind UDP %u\n",
		             static_cast<unsigned>(config.gate_udp_port));
		return false;
	}

	bound_port_     = bound;
	public_host_    = config.public_host;
	lobby_endpoint_ = config.lobby_endpoint;
	web_host_ = config.web_host;
	http_port_      = config.http_port;

	reflect_ip_   = config.client_reflect_ip;
	reflect_port_ = config.client_reflect_gate_port;

	met_ip_    = config.met_ip;
	met_port_  = config.met_port;
	met_label_ = config.met_label;
	met_ping_  = config.met_ping;
	met_ext_   = config.met_ext;
	glsvss_request_ = config.glsvss_request;
	glsvss_rims_    = config.glsvss_rims;
	glsvss_agrms_   = config.glsvss_agrms;

	// The receive thread's own connection (Database is single-threaded),
	// leased by the thread itself (a lease stays on the thread that acquired
	// it), which reads the games catalogue on it once before it serves.
	// start() waits for both, so a database that cannot be opened stops the
	// boot.
	stop_requested_.store(false);
	std::promise<bool> leased;
	std::future<bool> lease_taken = leased.get_future();
	worker_ = std::thread([this, socket = std::move(socket), leased = std::move(leased)]() mutable {
		std::optional<db::ConnectionPool::Lease> db_conn;
		if (db_pool_) {
			try {
				db_conn.emplace(db_pool_->acquire());
				games_ = catalog::GameCatalog::load(**db_conn);
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[gate] db open failed: %s\n", e.what());
				leased.set_value(false);
				return;
			}
		}
		leased.set_value(true);
		run_loop(std::move(socket), std::move(db_conn));
	});
	if (!lease_taken.get()) {
		worker_.join();
		return false;
	}
	running_.store(true);
	if (games_.empty()) {
		std::fprintf(stderr, "[gate] WARN no games rows: every probe goes unanswered\n");
	}
	for (const catalog::GameRow &g : games_.games()) {
		std::printf("[gate] lobby %s answers tag %s (nwprepare.dll?%s)\n",
		            loggable(g.lobby_name).c_str(), loggable(g.gate_tag).c_str(),
		            loggable(g.startup_query, 255).c_str());
	}
	for (const catalog::GateTagAlias &alias : catalog::kGateTagAliases) {
		if (const catalog::GameRow *g = games_.by_gate_tag(alias.tag)) {
			std::printf("[gate] lobby %s answers tag %s (alias of %s)\n",
			            loggable(g->lobby_name).c_str(), std::string(alias.tag).c_str(),
			            std::string(alias.answers_as).c_str());
		}
	}

	std::printf("[gate] listening on UDP :%u (also the POSTIPPORT status sink)\n",
	            static_cast<unsigned>(bound_port_));
	return true;
}

void GateListener::stop() {
	stop_requested_.store(true);
	if (worker_.joinable()) {
		worker_.join();
	}
	running_.store(false);
}

void GateListener::heartbeat(db::Database &db, Heartbeat &beat) {
	beat.probes_unanswered =
			static_cast<int64_t>(probes_unanswered_.exchange(0, std::memory_order_relaxed));
	beat.lobbies_live = static_cast<int64_t>(lobbydb::live_lobbies(db).instances);
}

void GateListener::run_loop(opennova::net::ScopedSocket socket,
                            std::optional<db::ConnectionPool::Lease> db_conn) {
	uint8_t rx[65535];
	while (!stop_requested_.load()) {
		loop_turns_.fetch_add(1, std::memory_order_relaxed);
		opennova::net::Endpoint from{};
		const int n = opennova::net::udp_recv_from(socket.get(), rx, sizeof(rx),
		                                           from, /*timeout_ms=*/250);
		if (n <= 0) continue;

		// Strip the LSB-scatter CRC envelope first: retail wraps the gate
		// probe, the METPROTOCOL block and the status blob in it
		// [orig: NapiSocket_SendTo @0x62cfa0 encodes when its encrypt flag is set;
		//  CNapiNPManager_SendTo @0x61ec20 passes 1 for the status blob].
		std::vector<uint8_t> plain(static_cast<size_t>(n));
		size_t inner_len = 0;
		if (opennova::napi_envelope_decode(rx, static_cast<size_t>(n),
		                                   plain.data(), plain.size(),
		                                   &inner_len) != 0) {
			std::fprintf(stderr, "[gate] %s:%u — bad envelope (%d bytes)\n",
			             opennova::net::endpoint_to_string(from).c_str(),
			             from.port, n);
			continue;
		}
		plain.resize(inner_len);
		GateMetricsReport metrics;
		if (gate_metrics_decode(plain.data(), plain.size(), metrics)) {
			std::printf("[gate] metrics block=%s fields=%zu entries=%zu\n",
					loggable(metrics.block).c_str(), metrics.fields.size(), metrics.entries.size());
			continue;
		}

		// The host-status heartbeat is plaintext (no NWU) and carries the
		// "HostKey =" preamble; it lands on POSTIPPORT, which is this port.
		{
			LobbyStatusBlob blob;
			const std::string_view text(reinterpret_cast<const char *>(plain.data()),
			                            plain.size());
			if (lobby_update_parse(text, blob)) {
				bool applied = false;
				if (db_conn) {
					try {
						applied = hostdb::apply_status_blob(**db_conn, blob);
					} catch (const std::exception &e) {
						std::fprintf(stderr, "[gate] WARN status blob apply: %s\n", e.what());
					}
				}
				std::printf("[gate] %s:%u status blob lobby=%s host_key=%zuB vars=%zu players=%zu "
							"-> %s\n",
						opennova::net::endpoint_to_string(from).c_str(), from.port,
						loggable(blob.lobby_name).c_str(), blob.host_key.size(),
						blob.host_vars.size(), blob.player_names.size(),
						applied ? "applied" : "no matching host");
				continue;
			}
		}

		// Decrypt with NWU + GATE_NWU_KEY ("GATEAPI") to recover the tag.
		// (Names are swapped vs onnet — see memory
		// `reference_nwu_names_swapped.md`.)
		opennova::nwu_encrypt(plain.data(), plain.size(),
		                      opennova::GATE_NWU_KEY);

		// Trim trailing NUL the client appended (gate_probe_build adds one).
		const size_t tag_len = (!plain.empty() && plain.back() == 0)
		                       ? plain.size() - 1 : plain.size();
		std::string tag(reinterpret_cast<const char *>(plain.data()), tag_len);

		const catalog::GameRow *game = games_.by_gate_tag(tag);
		if (game == nullptr) {
			std::fprintf(stderr, "[gate] %s:%u — unknown tag '%s'; ignoring\n",
					opennova::net::endpoint_to_string(from).c_str(), from.port,
					loggable(tag).c_str());
			if (tracker_) {
				using namespace std::chrono;
				const auto gate_now = static_cast<uint64_t>(
					duration_cast<milliseconds>(
						system_clock::now().time_since_epoch()).count());
				tracker_->record("gate", tag, plain.data(), plain.size(),
				                 opennova::net::endpoint_to_string(from),
				                 gate_now);
			}
			continue;
		}

		const std::string client_ip =
			std::to_string(from.ip[0]) + "." + std::to_string(from.ip[1]) + "." +
			std::to_string(from.ip[2]) + "." + std::to_string(from.ip[3]);
		// Reflection override (dev/NAT): tell the client its reachable endpoint
		// is the configured reflect addr, not the observed source (the docker
		// gateway behind a bridge). Empty/0 => advertise the observed source
		// (the prod path).
		// The lobby address the live instances announce, read again every lobby_refresh_ms_. A
		// read that fails drops what the last one read, so the gate hands its fallback lobby
		// (lobby_endpoint_; none: no reply) until a read answers: the instances read before may
		// all have stopped since. The next read is kLobbyRetryFactor refreshes later: a read
		// during a database outage waits out the connect timeout on this thread, which answers
		// every probe.
		if (db_conn) {
			const uint64_t now =
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now().time_since_epoch())
									.count());
			const uint64_t wait_ms = lobbies_read_failed_ ? lobby_refresh_ms_ * kLobbyRetryFactor
														  : lobby_refresh_ms_;
			if (lobbies_read_ms_ == 0 || now - lobbies_read_ms_ >= wait_ms) {
				lobbies_read_ms_ = now;
				try {
					live_lobbies_ = lobbydb::live_lobbies(**db_conn);
					if (lobbies_read_failed_)
						std::printf("[gate] the lobby instances read again\n");
					lobbies_read_failed_ = false;
				} catch (const db::Error &e) {
					if (!lobbies_read_failed_)
						std::fprintf(stderr,
								"[gate] WARN reading the lobby instances: %s; handing out the "
								"fallback lobby, if any, until a read answers\n",
								e.what());
					live_lobbies_ = {};
					lobbies_read_failed_ = true;
				}
			}
		}
		// No lobby to hand out (none takes clients, or none could be read, and no
		// fallback): no reply, which a client meets as a lost one and asks again. A
		// line every kNoLobbyLogMs at most, with the probes dropped since.
		const std::string &lobby_endpoint =
				live_lobbies_.endpoint.empty() ? lobby_endpoint_ : live_lobbies_.endpoint;
		if (lobby_endpoint.empty()) {
			++no_lobby_dropped_;
			probes_unanswered_.fetch_add(1, std::memory_order_relaxed);
			const uint64_t now_ms =
					static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now().time_since_epoch())
									.count());
			if (no_lobby_logged_ms_ == 0 || now_ms - no_lobby_logged_ms_ >= kNoLobbyLogMs) {
				no_lobby_logged_ms_ = now_ms;
				std::fprintf(stderr,
						"[gate] WARN no lobby instance takes clients and no fallback "
						"(ONNET_LOBBY_ENDPOINT): %llu probe(s) unanswered\n",
						static_cast<unsigned long long>(no_lobby_dropped_));
				no_lobby_dropped_ = 0;
			}
			continue;
		}
		GateResponseFields fields;
		fields.lobby_name     = game->lobby_name;
		fields.startup_query  = game->startup_query;
		fields.public_host    = public_host_;
		fields.post_port      = bound_port_;
		fields.lobby_endpoint = lobby_endpoint;
		fields.web_host = web_host_;
		fields.http_port      = http_port_;
		fields.reflected_ip   = reflect_ip_.empty() ? client_ip : reflect_ip_;
		fields.reflected_port = reflect_port_ != 0 ? reflect_port_ : from.port;
		fields.met_ip         = met_ip_;
		fields.met_port       = met_port_;
		fields.met_label      = met_label_;
		fields.met_ping       = met_ping_;
		fields.met_ext        = met_ext_;
		// The row's GLSVSS leg; a row with none takes the configured one
		// (ONNET_GLSVSS_*, none by default).
		const bool row_glsvss = !game->glsvss_request.empty();
		fields.glsvss_request = row_glsvss ? game->glsvss_request : glsvss_request_;
		fields.glsvss_rims    = row_glsvss ? game->glsvss_rims : glsvss_rims_;
		fields.glsvss_agrms   = row_glsvss ? game->glsvss_agrms : glsvss_agrms_;
		// The row's title answers in its witnessed live shape, if it has one.
		const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(game->title.c_str());
		const TitleIdentity *title = profile != nullptr ? title_identity(profile->id) : nullptr;
		fields.witnessed_shape = title != nullptr && title->gate_reply_witnessed;
		if (fields.witnessed_shape)
			fields.time_on_gate_server = gate_clock_text(std::time(nullptr));
		const std::string body = build_gate_response(fields);

		std::vector<uint8_t> reply_inner(body.begin(), body.end());
		opennova::nwu_decrypt(reply_inner.data(), reply_inner.size(),
		                      opennova::GATE_NWU_KEY);
		// Wrap in the LSB-scatter CRC envelope retail expects.
		std::vector<uint8_t> reply(reply_inner.size() + 16);
		size_t reply_size = 0;
		if (opennova::napi_envelope_encode(reply_inner.data(), reply_inner.size(),
		                                   reply.data(), reply.size(),
		                                   &reply_size) != 0) {
			std::fprintf(stderr, "[gate] %s:%u — envelope encode failed\n",
			             client_ip.c_str(), from.port);
			continue;
		}
		reply.resize(reply_size);
		// The gate's one send: this reply, to the probe's source, at once. It
		// never sends a datagram of its own (the METPROTOCOL block and the
		// status blob above are taken without an answer), so behind a UDP load
		// balancer (ECS: the NLB on 7597) every datagram it sends rides the
		// probe's own flow back through the balancer, never the task's own
		// address, which a client's NAT would drop.
		const int sent = opennova::net::udp_send_to(socket.get(), from,
		                                            reply.data(), reply.size());
		if (sent > 0) {
			std::printf("[gate] %s:%u tag=%s lobby=%s at %s -> %d bytes\n", client_ip.c_str(),
					from.port, loggable(tag).c_str(), loggable(game->lobby_name).c_str(),
					loggable(fields.lobby_endpoint).c_str(), sent);
		} else {
			std::fprintf(stderr, "[gate] %s:%u send failed\n",
			             client_ip.c_str(), from.port);
		}
	}

	std::printf("[gate] loop exiting\n");
}

} // namespace opennova::novaworld_server
