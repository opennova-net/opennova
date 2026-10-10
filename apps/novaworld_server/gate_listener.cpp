#include "gate_listener.h"

#include "server_config.h"

#include <net_sockets.h>
#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_metrics.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_update.h>
#include <net/novaworld/unknown_tracker.h>

#include <chrono>
#include <cstdio>
#include <exception>
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
// POSTIPADDRESS + POSTIPPORT are REQUIRED: the parser fails the gate to
// state -9 ("NO NW POST IP" / "NO NW POST PORT") without them, unless the
// junction bypass is set. The host posts its status blob there
// [orig: Lobby_UpdateServerInfo @0x4ff62c], so it names this listener.
struct GateResponseFields {
	std::string public_host;
	uint16_t post_port = 0;      // this listener's port
	uint16_t nw_udp_port = 0;
	uint16_t http_port = 0;
	std::string reflected_ip;
	uint16_t reflected_port = 0;
	std::string met_ip;
	uint16_t met_port = 0;
	std::string met_label;
	int met_ping = 0;
	int met_ext = 0;
	std::string glsvss_request;
	int glsvss_rims = 0;
	int glsvss_agrms = 0;
};

std::string build_gate_response(const GateResponseFields &f) {
	auto append_var = [](std::ostringstream &os, const char *key, const std::string &value) {
		os << "VAR \"" << key << "\" \"" << value << "\"\r\n";
	};

	std::ostringstream os;
	os << "GATEPROTOCOL \"1.0\"\r\n";
	append_var(os, "LobbyName", "jop_2_consumer");
	append_var(os, "POSTIPADDRESS", f.public_host);
	append_var(os, "POSTIPPORT", std::to_string(f.post_port));
	if (!f.met_ip.empty()) {
		append_var(os, "METIPADDRESS", f.met_ip);
		append_var(os, "METIPPORT", std::to_string(f.met_port));
		append_var(os, "METLABEL", f.met_label);
		append_var(os, "METPING", std::to_string(f.met_ping));
		append_var(os, "METEXT", std::to_string(f.met_ext));
	}
	append_var(os, "udpnovaworld",
	           f.public_host + ":" + std::to_string(f.nw_udp_port));
	append_var(os, "udpcode1", "abc");
	append_var(os, "udpcode2", "xyz");
	append_var(os, "startupurl",
	           "http://" + f.public_host + ":" + std::to_string(f.http_port) +
	           "/nwprepare.dll?ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm");
	append_var(os, "usejunction", "0");
	append_var(os, "clearjunction", "0");
	append_var(os, "ReflectedIpAddress", f.reflected_ip);
	append_var(os, "ReflectedPortNumber", std::to_string(f.reflected_port));
	if (!f.glsvss_request.empty()) {
		append_var(os, "GLSVSSREQUEST", f.glsvss_request);
		append_var(os, "GLSVSSRIMS", std::to_string(f.glsvss_rims));
		append_var(os, "GLSVSSAGRMS", std::to_string(f.glsvss_agrms));
	}
	return os.str();
}

// Per memory `reference_gate_tags.md`:
//   `jopd:cus4` = jodemo (Joint Operations demo)
//   `jop:cus2`  = retail Joint Operations
// Both share LobbyName `jop_2_consumer`. Reject anything else for now.
bool is_known_tag(const std::string &tag) {
	return tag == "jopd:cus4"
	    || tag == "jop:cus2"
	    || tag == "dfx2:0:cus:buffy";
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
	nw_udp_port_    = config.nw_udp_port;
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
	// leased here so a database that cannot be opened stops the boot.
	std::optional<db::ConnectionPool::Lease> db_conn;
	if (db_pool_) {
		try {
			db_conn.emplace(db_pool_->acquire());
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[gate] db open failed: %s\n", e.what());
			return false;
		}
	}

	stop_requested_.store(false);
	running_.store(true);
	worker_ = std::thread([this, socket = std::move(socket),
	                       db_conn = std::move(db_conn)]() mutable {
		run_loop(std::move(socket), std::move(db_conn));
	});
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

void GateListener::run_loop(opennova::net::ScopedSocket socket,
                            std::optional<db::ConnectionPool::Lease> db_conn) {
	uint8_t rx[65535];
	while (!stop_requested_.load()) {
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
				metrics.block.c_str(), metrics.fields.size(), metrics.entries.size());
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
				std::printf("[gate] %s:%u status blob lobby=%s host_key=%zuB vars=%zu players=%zu -> %s\n",
				            opennova::net::endpoint_to_string(from).c_str(), from.port,
				            blob.lobby_name.c_str(), blob.host_key.size(),
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

		if (!is_known_tag(tag)) {
			std::fprintf(stderr, "[gate] %s:%u — unknown tag '%s'; ignoring\n",
			             opennova::net::endpoint_to_string(from).c_str(),
			             from.port, tag.c_str());
			if (tracker_) {
				using namespace std::chrono;
				const auto gate_now = static_cast<uint64_t>(
					duration_cast<milliseconds>(
						steady_clock::now().time_since_epoch()).count());
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
		GateResponseFields fields;
		fields.public_host    = public_host_;
		fields.post_port      = bound_port_;
		fields.nw_udp_port    = nw_udp_port_;
		fields.http_port      = http_port_;
		fields.reflected_ip   = reflect_ip_.empty() ? client_ip : reflect_ip_;
		fields.reflected_port = reflect_port_ != 0 ? reflect_port_ : from.port;
		fields.met_ip         = met_ip_;
		fields.met_port       = met_port_;
		fields.met_label      = met_label_;
		fields.met_ping       = met_ping_;
		fields.met_ext        = met_ext_;
		fields.glsvss_request = glsvss_request_;
		fields.glsvss_rims    = glsvss_rims_;
		fields.glsvss_agrms   = glsvss_agrms_;
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
		const int sent = opennova::net::udp_send_to(socket.get(), from,
		                                            reply.data(), reply.size());
		if (sent > 0) {
			std::printf("[gate] %s:%u tag=%s -> %d bytes\n",
			            client_ip.c_str(), from.port, tag.c_str(), sent);
		} else {
			std::fprintf(stderr, "[gate] %s:%u send failed\n",
			             client_ip.c_str(), from.port);
		}
	}

	std::printf("[gate] loop exiting\n");
}

} // namespace opennova::novaworld_server
