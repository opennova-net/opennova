#include "lister.h"

#include "console_log.h"

#include <base/io/log.h>
#include <base/io/strutil.h>
#include <net/napi/session.h>
#include <net/novaworld/connect_or_host.h>
#include <net/novaworld/lobby_identity.h>

#include <chrono>
#include <set>
#include <system_error>

namespace opennova::lister {

using io::LogLevel;

namespace {

constexpr int kHttpTimeoutMs = 15000;
constexpr uint32_t kListingCheckMs = 2000;

// The destination policy: until --allow-public every destination must be on 127.0.0.0/8, and a
// name does not resolve at all (the lookup would itself be traffic); "localhost" is loopback.
bool resolve_destination(const std::string &host, net::Endpoint &out, bool allow_public, const char *what) {
	bool resolved = false;
	if (host == "localhost") {
		out.ip = {127, 0, 0, 1};
		resolved = true;
	} else {
		resolved = net::resolve_ipv4(host, out, allow_public);
	}
	if (!resolved) {
		io::logf(LogLevel::kWarn, "[net] cannot resolve %s '%s'%s", what, host.c_str(),
		         allow_public ? "" : " (names resolve only with --allow-public)");
		return false;
	}
	if (!allow_public && out.ip[0] != 127) {
		io::logf(LogLevel::kWarn, "[net] refused %s %s: not on 127.0.0.0/8 and --allow-public not given", what,
		         net::endpoint_to_string(out).c_str());
		return false;
	}
	return true;
}

// The policy at the datagram level: the gate response and the session name further hosts, so
// every send is checked, not just the first resolve.
class PolicySocket final : public IDatagramSocket {
public:
	PolicySocket(net::Socket &socket, bool allow_public) : inner_(socket), allow_public_(allow_public) {}

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		return inner_.recv_from(buf, cap, from);
	}

	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		if (!allow_public_ && (to.ip & 0xFFu) != 127u) {
			if (refused_.insert(to.ip).second) {
				io::logf(LogLevel::kWarn, "[net] refused datagrams to %s: not on 127.0.0.0/8 and --allow-public not given",
				         net::endpoint_to_string(net::NetDatagramSocket::to_endpoint(to)).c_str());
			}
			return;
		}
		inner_.send_to(to, data, len);
	}

private:
	net::NetDatagramSocket inner_;
	bool allow_public_;
	std::set<uint32_t> refused_;
};

std::string ip4(const std::array<uint8_t, 4> &ip) {
	return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
	       std::to_string(ip[3]);
}

// A stable machine id for the stand-in NWPSSK / NWUSID where retail's inputs do not read.
std::string stable_identity() {
	const std::string host = net::local_host_name();
	return host.empty() ? std::string("nw-lister") : "nw-lister|" + host;
}

// The strings the Host list's Region, TimeLeft, Y/N and TimeOfDay tokens resolve to: retail's
// English Gametext.bin (NovaWorld STRNOVA07..12, the TimeOfDay section), which the game reads
// through its gametext and a lister, running without retail data, carries.
// [orig: Lobby_UpdateServerInfo @0x4fe8c0 — STRNOVA07/08/09 @0x4fea56.., STRNOVA10 @0x4fed8d,
//  STRNOVA11/12 @0x4fec2c.., the TimeOfDay quintet @0x4ff09b..0x4ff138]
HostLobbyText english_lobby_text() {
	HostLobbyText text;
	text.yes = "Y";
	text.no = "N";
	text.no_time_limit = "NA";
	text.region = {"Jungle", "Desert", "Snow"};
	text.time_of_day = {"Unknown", "Dawn", "Day", "Dusk", "Night"};
	return text;
}

bool same_player(const HostPlayerSlot &a, const HostPlayerSlot &b) {
	return a.player_name == b.player_name && a.team == b.team && a.ip_and_port == b.ip_and_port &&
	       a.pcid == b.pcid && a.type == b.type;
}

} // namespace

Lister::Lister(ListerOptions options)
    : options_(std::move(options)),
      rng_(std::random_device{}()),
      lobby_(
          [this]() {
	          NwuLobbySession::Hooks hooks;
	          hooks.on_gate_response = [](const GateResponse &gate) {
		          io::logf(LogLevel::kInfo, "[gate] lobby %s, NovaWorld %s", gate.lobby_name.c_str(),
		                   gate.udp_novaworld.c_str());
	          };
	          hooks.cookie_vars = [this]() { return cookie_vars(); };
	          hooks.on_fatal = [this](const std::string &message) {
		          io::logf(LogLevel::kError, "[session] failed: %s", message.c_str());
		          end(kExitFailed);
	          };
	          hooks.on_soft_error = [](const std::string &message) {
		          io::logf(LogLevel::kDebug, "[gate] %s", message.c_str());
	          };
	          return hooks;
          }(),
          [this]() {
	          NwuLobbySession::Environment env;
	          env.random_u32 = [this]() { return static_cast<uint32_t>(rng_()); };
	          env.resolve_ipv4 = [this](const std::string &host, PeerAddr &out) {
		          net::Endpoint endpoint;
		          if (!resolve_destination(host, endpoint, options_.allow_public, "NovaWorld host")) return false;
		          out.ip = net::NetDatagramSocket::to_peer(endpoint).ip;
		          return true;
	          };
	          return env;
          }()),
      role_(lobby_, [this]() {
	      NwuHostRole::Hooks hooks;
	      hooks.on_hosting = [this]() {
		      phase_ = Phase::Hosting;
		      io::logf(LogLevel::kInfo, "[host] listed: '%s', %zu player(s)", registration().server_name.c_str(),
		               role_.roster().size());
	      };
	      hooks.on_failed = [this](const std::string &tag) {
		      io::logf(LogLevel::kError, "[host] the host request failed: %s", tag.c_str());
		      end(kExitFailed);
	      };
	      hooks.on_stopped = [this](const std::string &key) {
		      io::logf(LogLevel::kError, "[host] the service stopped the hosting (%s)", key.c_str());
		      end(kExitStoppedByService);
	      };
	      hooks.on_command = [](const ServerCommand &command) {
		      io::logf(LogLevel::kInfo, "[host] ServerCommand %s ignored: no game behind this listing",
		               server_command_verb_name(command.verb));
	      };
	      return hooks;
      }()) {
	role_.set_lobby_text(english_lobby_text());
}

Lister::~Lister() {
	admin_.stop();
	if (http_.valid()) http_.wait();
}

bool Lister::start() {
	std::string error;
	if (!load_listing(options_.listing_path, listing_, error)) {
		io::logf(LogLevel::kError, "[listing] %s", error.c_str());
		exit_code_ = kExitBadInput;
		return false;
	}
	std::error_code ec;
	listing_mtime_ = std::filesystem::last_write_time(options_.listing_path, ec);
	if (!options_.admin_host.empty()) {
		net::Endpoint server;
		server.port = options_.admin_port;
		if (!resolve_destination(options_.admin_host, server, options_.allow_public, "admin server")) {
			exit_code_ = kExitNetwork;
			return false;
		}
		admin_.start(server, options_.credentials.admin_user, options_.credentials.admin_pass);
		io::logf(LogLevel::kInfo, "[admin] reading %s every %d s for the players, map and time left",
		         net::endpoint_to_string(server).c_str(), kAdminPollSeconds);
	}
	gate_udp_ = net::ScopedSocket(net::udp_bind(0));
	session_udp_ = net::ScopedSocket(net::udp_bind(0));
	if (!gate_udp_.is_valid() || !session_udp_.is_valid()) {
		io::logf(LogLevel::kError, "[net] cannot bind the UDP sockets");
		exit_code_ = kExitNetwork;
		return false;
	}
	gate_socket_ = std::make_unique<PolicySocket>(gate_udp_.get(), options_.allow_public);
	session_socket_ = std::make_unique<PolicySocket>(session_udp_.get(), options_.allow_public);
	lobby_.open(*gate_socket_, *session_socket_);
	lobby_.probe(options_.master_host, options_.master_gate_port);
	io::logf(LogLevel::kInfo, "[gate] probing %s:%u (%s)", options_.master_host.c_str(), options_.master_gate_port,
	         options_.allow_public ? "public destinations allowed" : "loopback only");
	return true;
}

bool Lister::tick(uint32_t now_ms) {
	if (phase_ == Phase::Done) return false;
	now_ms_ = now_ms;
	lobby_.tick(now_ms);
	if (ClientSession *session = lobby_.session()) {
		for (const ClientSession::Notice &notice : session->take_notices()) {
			if (role_.handle_notice(notice)) continue;
			if (notice.kind == ClientSession::Notice::Kind::LeaveNovaWorld) {
				io::logf(LogLevel::kError, "[session] the service ended the session (MsgCode %d)",
				         notice.fields.msg_code);
				end(kExitStoppedByService);
			}
		}
	}
	role_.tick(now_ms);
	if (!ending_ && phase_ == Phase::Connecting && lobby_.session_verified()) on_verified();
	if (!ending_ && http_.valid() && http_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		on_http_reply(http_.get());
	}
	if (!ending_) refresh_listing(false);
	if (ending_) teardown();
	return phase_ != Phase::Done;
}

void Lister::stop() {
	if (phase_ == Phase::Done) return;
	io::logf(LogLevel::kInfo, "[main] stopping");
	end(kExitStopped);
	teardown();
}

// Verified: the account login first when there is one (the hosting page needs it), else the host
// request straight away.
void Lister::on_verified() {
	io::logf(LogLevel::kInfo, "[session] verified at %s:%u", lobby_.nw_udp_host().c_str(), lobby_.nw_udp_port());
	if (!options_.credentials.present()) {
		io::logf(LogLevel::kInfo, "[http] no account: hosting without a HOSTKEY");
		request_host();
		return;
	}
	sync_flow_context();
	const LoginResult first = flow_.login(options_.credentials.user, options_.credentials.pass);
	if (first.kind != LoginResult::Kind::NeedRequest) {
		io::logf(LogLevel::kError, "[http] login failed: %s", first.reason.c_str());
		end(kExitFailed);
		return;
	}
	phase_ = Phase::LoggingIn;
	ship(first.request);
}

void Lister::on_http_reply(const net::HttpReply &reply) {
	if (!reply.transport_ok) io::logf(LogLevel::kWarn, "[http] %s", reply.error.c_str());
	if (phase_ == Phase::LoggingIn) {
		const LoginResult r = flow_.on_login_response(reply.transport_ok, reply.code, reply.headers, reply.body);
		if (r.kind == LoginResult::Kind::NeedRequest) {
			ship(r.request);
			return;
		}
		if (r.kind == LoginResult::Kind::Failed) {
			io::logf(LogLevel::kError, "[http] login failed: %s", r.reason.c_str());
			end(kExitFailed);
			return;
		}
		add_log_secret(r.nwhandle);
		io::logf(LogLevel::kInfo, "[http] logged in");
		sync_flow_context();
		const HostKeyResult first = flow_.host();
		if (first.kind != HostKeyResult::Kind::NeedRequest) {
			io::logf(LogLevel::kError, "[http] the hosting page failed: %s", first.reason.c_str());
			end(kExitFailed);
			return;
		}
		phase_ = Phase::FetchingHostKey;
		ship(first.request);
		return;
	}
	if (phase_ == Phase::FetchingHostKey) {
		const HostKeyResult r = flow_.on_host_response(reply.transport_ok, reply.code, reply.headers, reply.body);
		if (r.kind == HostKeyResult::Kind::NeedRequest) {
			ship(r.request);
			return;
		}
		if (r.kind == HostKeyResult::Kind::Failed) {
			io::logf(LogLevel::kError, "[http] the hosting page failed: %s", r.reason.c_str());
			end(kExitFailed);
			return;
		}
		host_key_ = r.host_key;
		io::logf(LogLevel::kInfo, "[http] HOSTKEY %s", mask_value(host_key_, 6).c_str());
		request_host();
	}
}

// One HTTP exchange on a worker thread, so the session keeps pumping while it runs (the game's
// HTTPRequest nodes run beside its frame the same way).
void Lister::ship(const HttpRequestSpec &spec) {
	// The session tags the last reply set ride the next URL: mask them before it is logged.
	for (const char *tag : {"LOGINSESSIONTAG", "NWJOINSESSIONTAG", "NWHANDLE"}) {
		if (const std::string *value = flow_.cookies().find(tag)) add_log_secret(*value);
	}
	const bool allow_public = options_.allow_public;
	io::logf(LogLevel::kDebug, "[http] %s %s", spec.method == HttpMethod::Post ? "POST" : "GET", spec.url.c_str());
	http_ = std::async(std::launch::async, [spec, allow_public]() {
		return net::http_exchange(spec.method == HttpMethod::Post, spec.url, spec.headers, spec.body, kHttpTimeoutMs,
		                          [allow_public](const std::string &host, net::Endpoint &out) {
			                          return resolve_destination(host, out, allow_public, "web host");
		                          });
	});
}

// The hosting leg of ConnectOrHost: its session and gate checks, then the request; the roster
// follows it (the request clears the role's), and its players announce once hosting.
void Lister::request_host() {
	const ClientSession *session = lobby_.session();
	const GateResponse &gate = lobby_.gate_response();
	const char *refusal = host_leg_refusal(session != nullptr, session ? session->session_flags() : 0u,
	                                       true, gate.lobby_name);
	if (refusal != nullptr) {
		io::logf(LogLevel::kError, "[host] cannot host: %s", refusal);
		end(kExitFailed);
		return;
	}
	phase_ = Phase::Requesting;
	refresh_listing(true);
	if (!role_.request(registration())) return; // on_failed finished it
	sync_roster();
	io::logf(LogLevel::kInfo, "[host] ClientHostRequest for '%s' on %s", registration().server_name.c_str(),
	         gate.lobby_name.c_str());
}

// The flow reads the gate response and the session's SessionInit; both are fixed once the
// session is verified, so re-syncing before each leg cannot move the web base mid-login.
void Lister::sync_flow_context() {
	const GateResponse &gate = lobby_.gate_response();
	LobbyHttpContext ctx;
	ctx.startup_url = gate.startup_url;
	ctx.post_ip = ip4(gate.post_ip);
	ctx.post_port = std::to_string(gate.post_port);
	if (const ClientSession *session = lobby_.session()) {
		ctx.web_domain = session->server_web_domain();
		ctx.server_nwuid = session->server_nwuid();
	}
	ctx.identity_vars = identity_vars_;
	flow_.set_context(std::move(ctx));
}

// The verify Cookie: the machine's identity set, then the browser jar once the login filled it.
std::vector<std::pair<std::string, std::string>> Lister::cookie_vars() {
	if (identity_vars_.empty()) {
		identity_.client_index = lobby_.client_index();
		identity_.client_key = lobby_.client_key();
		read_locale_identity(identity_);
		LobbyMachineTokens tokens = fallback_machine_tokens(stable_identity());
		RetailMachineInputs machine;
		if (read_retail_machine_inputs(machine)) tokens = make_retail_machine_tokens(machine);
		identity_.nwpssk = tokens.nwpssk;
		identity_.nwusid = tokens.nwusid;
		identity_vars_ = make_lobby_identity_vars(identity_);
	}
	sync_flow_context();
	return flow_.session_cookie_vars();
}

// The listing file, re-read when it changed, and the admin port's latest answer over it.
void Lister::refresh_listing(bool force) {
	if (phase_ != Phase::Requesting && phase_ != Phase::Hosting) return;
	if (!force && now_ms_ - listing_checked_ms_ < kListingCheckMs) return;
	listing_checked_ms_ = now_ms_;
	bool changed = false;
	std::error_code ec;
	const auto mtime = std::filesystem::last_write_time(options_.listing_path, ec);
	if (!ec && mtime != listing_mtime_) {
		Listing fresh;
		std::string error;
		if (load_listing(options_.listing_path, fresh, error)) {
			listing_ = std::move(fresh);
			changed = true;
			io::logf(LogLevel::kInfo, "[listing] reloaded: '%s' on %s", listing_.columns.server_name.c_str(),
			         listing_.columns.mission_name.c_str());
		} else {
			io::logf(LogLevel::kWarn, "[listing] %s (keeping the previous listing)", error.c_str());
		}
		listing_mtime_ = mtime;
	}
	if (!options_.admin_host.empty()) {
		const AdminSnapshot snapshot = admin_.snapshot();
		if (snapshot.seq != admin_snapshot_.seq) {
			changed = true;
			if (snapshot.ok) {
				io::logf(LogLevel::kInfo, "[admin] %zu player(s) on %s, %d min left", snapshot.players.size(),
				         snapshot.mission.c_str(), snapshot.time_left_minutes);
			} else {
				io::logf(LogLevel::kWarn, "[admin] the server is not answering (%s): listing no players",
				         snapshot.status.c_str());
			}
			admin_snapshot_ = snapshot;
		}
	}
	if (!changed) return;
	role_.set_columns(registration());
	sync_roster();
}

// The listing columns plus what the session owns: the gate's LobbyName and, for a dedicated
// server, the machine's locale trio. The admin port's map and time left replace the file's.
HostRegistration Lister::registration() const {
	HostRegistration r = listing_.columns;
	r.lobby_name = lobby_.gate_response().lobby_name;
	r.host_key = host_key_;
	r.max_players = host_leg_max_players(r.max_players, r.dedicated_server);
	if (r.dedicated_server) {
		r.country_name = identity_.country;
		r.language = identity_.language;
		r.tz_bias = strutil::parse_int(identity_.tz_bias).value_or(0);
	}
	if (admin_snapshot_.seq != 0 && admin_snapshot_.ok) {
		if (!admin_snapshot_.mission.empty()) r.mission_name = admin_snapshot_.mission;
		const int minutes = admin_snapshot_.time_left_minutes;
		r.round_time_remaining_ticks = minutes < 0 ? -1 : minutes * 3720 + 3719;
	}
	return r;
}

// The roster the PlayerList should carry: the admin port's players (their server slots) once it
// answered, else the file's, where a player without a slot keeps the one it has and a new name
// takes the lowest free one, so one player's change never moves another.
std::vector<HostPlayerSlot> Lister::wanted_roster() const {
	std::vector<HostPlayerSlot> out;
	if (admin_snapshot_.seq != 0) {
		if (!admin_snapshot_.ok) return out;
		for (const AdminPlayer &player : admin_snapshot_.players) {
			HostPlayerSlot slot;
			slot.slot = player.slot;
			slot.player_name = player.name;
			slot.team = player.team;
			slot.type = "0";
			out.push_back(std::move(slot));
		}
		return out;
	}
	std::set<int> taken;
	for (const HostPlayerSlot &player : listing_.players) {
		if (player.slot >= 0) {
			out.push_back(player);
			taken.insert(player.slot);
		}
	}
	const std::map<int, HostPlayerSlot> &current = role_.roster();
	for (HostPlayerSlot player : listing_.players) {
		if (player.slot >= 0) continue;
		for (const auto &entry : current) {
			if (entry.second.player_name == player.player_name && taken.count(entry.first) == 0) {
				player.slot = entry.first;
				break;
			}
		}
		if (player.slot < 0) {
			int free_slot = 0;
			while (taken.count(free_slot) != 0) ++free_slot;
			player.slot = free_slot;
		}
		taken.insert(player.slot);
		out.push_back(std::move(player));
	}
	return out;
}

// ClientHostPlayerRemoved for every slot that left or changed, then ClientHostPlayerAdded for
// every slot that arrived or changed (Server_PlayerRemove / Server_PlayerAdd per slot).
void Lister::sync_roster() {
	const std::vector<HostPlayerSlot> wanted = wanted_roster();
	std::vector<int> leaving;
	for (const auto &entry : role_.roster()) {
		bool kept = false;
		for (const HostPlayerSlot &player : wanted) {
			if (player.slot == entry.first && same_player(player, entry.second)) kept = true;
		}
		if (!kept) leaving.push_back(entry.first);
	}
	for (int slot : leaving) role_.clear_player_slot(slot);
	for (const HostPlayerSlot &player : wanted) {
		if (role_.roster().count(player.slot) == 0) role_.set_player_slot(player);
	}
}

void Lister::end(int code) {
	if (ending_ || phase_ == Phase::Done) return;
	ending_ = true;
	exit_code_ = code;
}

// Deregister and close: the stop statement leaves on one send pump, then the verified
// connection's disconnect burst (NovaWorldClient::stop's teardown).
// The NovaWorld leg goes first: a console close leaves the process a few seconds, and the admin
// thread can be mid-poll on a dead server.
void Lister::teardown() {
	if (phase_ == Phase::Done) return;
	phase_ = Phase::Done;
	if (lobby_.is_open()) {
		role_.stop();
		lobby_.flush();
		if (ClientSession *session = lobby_.session(); session != nullptr && session->is_verified()) {
			const std::vector<uint8_t> goodbye = session->build_goodbye();
			for (std::size_t i = 0; i < session->disconnect_burst_count(); ++i) lobby_.send(goodbye);
			io::logf(LogLevel::kInfo, "[session] ClientStopHosting and the goodbye sent");
		}
		lobby_.close();
	}
	admin_.stop();
}

} // namespace opennova::lister
