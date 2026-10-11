// The listing's owner end to end (host_keys, migration 0009), through the
// service's real listeners in-process: the Crow HttpListener (the retail
// NW*.dll pages from apps/novaworld_server/templates), the NwUdpListener and
// the GateListener over one pooled SQLite file, on loopback ports the OS
// picks. Two such services run side by side, one with ONNET_REQUIRE_HOST_KEY.
// Built only with BUILD_NOVAWORLD_HTTP (the HTTP layer is Linux-only).
//
// A host presents its HOSTKEY where a stock host does: not in its fresh
// ClientHostRequest (BuildHostVarLists rebuilds the Host list without one) but
// in the ClientHostUpdate that first carries the full Host list, and again in a
// re-host's kept list after an NWU reconnect.
//
//   * opennova-nw-lister's Lister with an account (its --credentials): the gate,
//     the NWU session, the EPASK login, /NWHost.dll's HOSTKEY, the host leg and
//     its first update; the row's host_user_id is that account, and host_keys
//     holds the key's hash claimed for the row's RID;
//   * the Lister with no account lists with no owner when the service is not
//     strict; the strict service never lists it and ends its hosting with
//     ServerStopHosting (NWUSERVERMSGCODE_NEEDTOLOGIN);
//   * on the strict service a logged-in host is held out of the browser until
//     its key arrives, then listed as its account's; a second connection
//     presenting that key is ended; the host's NWU reconnect (the session's own
//     reap, the re-probe, the re-join and the CurrentlyHosting=1 re-host) keeps
//     its RID and owner; with its keys revoked, the next re-host is refused with
//     MsgCode 200, a stock host's NWEC58;
//   * on the lenient service a second connection presenting a listed key is
//     listed with no owner.

#include "auth.h"
#include "gate_listener.h"
#include "http_listener.h"
#include "lister.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#include "session_store.h"

#include "net_http.h"
#include "net_sockets.h"

#include <base/io/log.h>
#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>

#include "common/file_io.h"
#include "common/temp_dir.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace hostdb = opennova::hostdb;
using opennova::ClientSession;
using opennova::HostRegistration;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

constexpr const char *kOwner = "owner";
constexpr const char *kOwnerPassword = "pw-owner";
constexpr const char *kNeedToLogin = "NWUSERVERMSGCODE_NEEDTOLOGIN";

// The engine's diagnostics (the lobby, the lister) beside the listeners' own
// printf lines, so a failure reads in order.
void log_sink(opennova::io::LogLevel level, const char *message) {
	if (level == opennova::io::LogLevel::kDebug) return;
	std::printf("%s\n", message);
}

bool wait_for(const std::function<bool()> &done, int tries = 100) {
	for (int i = 0; i < tries && !done(); ++i) std::this_thread::sleep_for(20ms);
	return done();
}

// One NovaWorld service, wired as main() wires it: the HTTP listener first,
// then the NW UDP listener naming its port as the web domain, then the gate
// naming both.
struct Service {
	opennova::db::ConnectionPool pool;
	opennova::ConnectionManager manager;
	nws::SessionStore sessions;
	nws::HttpListener http;
	nws::NwUdpListener nwudp;
	nws::GateListener gate;
	int64_t owner_id = 0;

	explicit Service(const std::filesystem::path &db_path)
		: pool(db_path), http(manager, pool, sessions), nwudp(manager) {}

	bool start(const std::filesystem::path &source_dir, const std::filesystem::path &scratch,
	           bool require_host_key) {
		{
			auto boot = pool.acquire();
			opennova::db::run_migrations(*boot, source_dir / "backend" / "migrations");
			boot->exec_script(test_io::read_file_text(
					(source_dir / "backend" / "seed" / "0001_games.sql").string()));
			nws::CreateUserParams p;
			p.username = kOwner;
			p.password = kOwnerPassword;
			p.pcid = "0e2e0001";
			p.nwh = "1";
			p.nwhandle = "OwnerHandle";
			const auto created = nws::create_user(*boot, p);
			if (!created.ok) return false;
			owner_id = created.id;
		}
		manager.on_lost([this](const opennova::Connection &c, opennova::DropReason r) {
			nwudp.erase_lobby_state(c.addr, opennova::drop_reason_name(r));
		});
		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.http_port = 0;
		config.nw_udp_port = 0;
		config.gate_udp_port = 0;
		config.templates_dir = source_dir / "apps" / "novaworld_server" / "templates";
		config.web_dist_dir = scratch / "web_dist";
		config.static_dir = scratch / "static";
		config.require_host_key = require_host_key;
		nwudp.set_db_pool(&pool);
		gate.set_db_pool(&pool);
		if (!http.start(config)) return false;
		config.http_port = http.bound_port();
		if (!nwudp.start(config)) return false;
		config.nw_udp_port = nwudp.bound_port();
		return gate.start(config);
	}

	void stop() {
		http.stop();
		gate.stop();
		nwudp.stop();
	}

	// The hosting connection named `name`, from the listener's memory.
	std::optional<opennova::LobbyState> lobby(const std::string &name) const {
		for (const auto &h : nwudp.snapshot_hosted()) {
			if (h.lobby.hosting && h.lobby.server_name == name) return h.lobby;
		}
		return std::nullopt;
	}

	// The browser's row (active_hosts) of the connection named `name`.
	std::optional<hostdb::HostRow> row(const std::string &name) {
		for (const auto &h : hostdb::list_hosts(*pool.acquire())) {
			if (h.server_name == name) return h;
		}
		return std::nullopt;
	}

	// The row named `name` once it names `owner` (nullopt: no owner), within a
	// couple of seconds.
	bool row_owned_by(const std::string &name, std::optional<int64_t> owner) {
		return wait_for([&] {
			const auto r = row(name);
			return r && r->host_user_id == owner;
		});
	}

	// The key's host_keys row: {user_id, claimed_rid}; nullopt when none.
	std::optional<std::pair<int64_t, std::optional<int64_t>>> key_row(const std::string &key) {
		const auto rows = pool.acquire()->query(
				"SELECT user_id, claimed_rid FROM host_keys WHERE key_hash = ?;",
				{opennova::db::BindValue(hostdb::host_key_hash(key))});
		if (rows.empty()) return std::nullopt;
		return std::make_pair(rows[0].as_int(0).value_or(0), rows[0].as_int(1));
	}
};

// A ListingSource standing in for a game server: fixed columns, no players,
// and the host leg's last outcome kept for the test.
class FixedSource final : public opennova::nw_lister::ListingSource {
public:
	explicit FixedSource(std::string name) { registration_.server_name = std::move(name); }
	bool start(int &) override { return true; }
	bool refresh(uint32_t, bool) override { return false; }
	HostRegistration registration() const override { return registration_; }
	std::vector<opennova::HostPlayerSlot> wanted_roster(
			const std::map<int, opennova::HostPlayerSlot> &) const override {
		return {};
	}
	void on_host_result(bool hosting, const std::string &failure) override {
		this->hosting = hosting;
		this->failure = failure;
	}
	void on_command(const opennova::ServerCommand &) override {}

	bool hosting = false;
	std::string failure;

private:
	HostRegistration registration_;
};

// A Lister on a synthetic clock, 10 ms a pass.
struct ListerRun {
	opennova::nw_lister::Lister &lister;
	uint32_t now = 0;

	// Pass until `done` holds or the lister finishes; `each` runs every pass.
	bool until(const std::function<bool()> &done, const std::function<void()> &each = {},
	           int rounds = 3000) {
		for (int i = 0; i < rounds && !done(); ++i) {
			now += 10;
			const bool running = lister.tick(now);
			if (each) each();
			if (!running) break;
			std::this_thread::sleep_for(2ms);
		}
		return done();
	}
};

opennova::nw_lister::ListerOptions lister_options(const Service &service, bool with_account) {
	opennova::nw_lister::ListerOptions options;
	options.master_host = "127.0.0.1";
	options.master_gate_port = service.gate.bound_port();
	if (with_account) {
		options.credentials.user = kOwner;
		options.credentials.pass = kOwnerPassword;
	}
	return options;
}

// A HOSTKEY the way a logged-in client fetches one: the EPASK login on the
// gate's startup URL, then the main page's @HOST_URL@ (/NWHost.dll and its
// relay). Empty when either leg fails.
std::string fetch_host_key(const Service &service) {
	opennova::LobbyHttpFlow flow;
	opennova::LobbyHttpContext ctx;
	ctx.startup_url = "http://127.0.0.1:" + std::to_string(service.http.bound_port()) +
	                  "/nwprepare.dll?ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm";
	flow.set_context(ctx);
	auto exchange = [](const opennova::HttpRequestSpec &spec) {
		return net::http_exchange(spec.method == opennova::HttpMethod::Post ? "POST" : "GET", spec.url,
		                          spec.headers, spec.body, 3000,
		                          [](const std::string &host, net::Endpoint &out) {
			                          return net::resolve_ipv4(host, out, false);
		                          });
	};
	auto login = flow.login(kOwner, kOwnerPassword);
	for (int i = 0; i < 16 && login.kind == opennova::LoginResult::Kind::NeedRequest; ++i) {
		const auto reply = exchange(login.request);
		login = flow.on_login_response(reply.transport_ok, reply.code, reply.headers, reply.body);
	}
	if (login.kind != opennova::LoginResult::Kind::Succeeded) {
		std::fprintf(stderr, "  login: %s\n", login.reason.c_str());
		return {};
	}
	auto host = flow.host();
	for (int i = 0; i < 16 && host.kind == opennova::HostKeyResult::Kind::NeedRequest; ++i) {
		const auto reply = exchange(host.request);
		host = flow.on_host_response(reply.transport_ok, reply.code, reply.headers, reply.body);
	}
	if (host.kind != opennova::HostKeyResult::Kind::Resolved) {
		std::fprintf(stderr, "  host key: %s\n", host.reason.c_str());
		return {};
	}
	return host.host_key;
}

// A host's NovaWorld session driven by hand over loopback (the recipe of
// nw_host_register_e2e): the handshake, the host leg, the update carrying the
// key, and the reconnect.
class RawHost {
public:
	RawHost(uint16_t service_port, uint32_t ci, uint32_t ck)
		: session_([ci, ck] {
			  ClientSession::Config cfg;
			  cfg.client_index = ci;
			  cfg.client_key = ck;
			  cfg.na = "ownership:host";
			  cfg.cookie_vars = [] {
				  return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
			  };
			  cfg.next_client_key = [next = ck]() mutable { return ++next; };
			  return cfg;
		  }()) {
		socket_ = net::ScopedSocket(net::udp_bind(0));
		service_.ip = {127, 0, 0, 1};
		service_.port = service_port;
	}

	bool verify() {
		send(session_.start());
		return drive([this] { return session_.is_verified(); }, 100);
	}

	// The fresh host leg (no HostKey in it, as a stock host's): Established,
	// or Failed with host_result().
	ClientSession::HostState host(const std::string &name) {
		HostRegistration reg;
		reg.server_name = name;
		reg.app_id = 2468;
		if (!session_.request_hosting(reg, 0)) return ClientSession::HostState::Idle;
		drive([this] { return settled(); }, 100);
		return session_.host_state();
	}

	// The update that first carries the key (a stock host's at its mission's
	// start), which also lands it in the session's kept Host list.
	void present_key(const std::string &key) {
		session_.send_host_update({{0, "HostKey", key}}, {});
		drive([] { return false; }, 4);
	}

	// Whether a ServerStopHosting carrying `msg_key` arrived, within `rounds`.
	bool stopped_with(const std::string &msg_key, int rounds = 40) {
		return drive([&] {
			for (const auto &notice : notices_) {
				if (notice.kind == ClientSession::Notice::Kind::StopHosting && notice.msg_key == msg_key)
					return true;
			}
			return false;
		}, rounds);
	}

	// The session's own reap (its 0x46 burst drops the service's connection,
	// which parks the lobby identity), the re-probe, and the re-join, whose
	// re-verify re-hosts with CurrentlyHosting=1 and the kept Host list.
	// `dropped` holds once the service took the row out of the browser.
	ClientSession::HostState reconnect(const std::function<bool()> &dropped) {
		const uint32_t dcnt = session_.disconnect_count();
		clock_ms_ += 240001;
		session_.set_clock_ms(clock_ms_);
		std::vector<std::vector<uint8_t>> burst;
		session_.pump(burst);
		for (const auto &dg : burst) send(dg);
		wait_for(dropped);
		bool probed = false;
		for (uint32_t step = 1; step < 10000 && !probed; ++step) {
			session_.set_clock_ms(++clock_ms_);
			std::vector<std::vector<uint8_t>> pumped;
			session_.pump(pumped);
			for (const auto &dg : pumped) send(dg);
			probed = !pumped.empty();
		}
		if (!probed) return ClientSession::HostState::Idle;
		drive([this, dcnt] {
			return session_.disconnect_count() == dcnt + 1 && session_.is_verified() && settled();
		}, 150);
		return session_.host_state();
	}

	void goodbye() { send(session_.build_goodbye()); }
	const ClientSession &session() const { return session_; }

private:
	bool settled() const {
		const auto state = session_.host_state();
		return state == ClientSession::HostState::Established ||
		       state == ClientSession::HostState::Failed;
	}

	void send(const std::vector<uint8_t> &dg) {
		if (!dg.empty() && socket_.is_valid())
			net::udp_send_to(socket_.get(), service_, dg.data(), dg.size());
	}

	// Receive, dispatch and answer until `done` holds (the session clock stays put).
	bool drive(const std::function<bool()> &done, int rounds) {
		for (int i = 0; i < rounds && !done(); ++i) {
			uint8_t buf[4096];
			net::Endpoint from;
			const int n = socket_.is_valid()
			                      ? net::udp_recv_from(socket_.get(), buf, sizeof buf, from, 50)
			                      : -1;
			std::vector<std::vector<uint8_t>> out;
			if (n > 0) session_.handle_datagram(buf, static_cast<size_t>(n), out);
			session_.finish_receive_batch(out);
			session_.pump(out);
			session_.process_periodic_update();
			for (const auto &dg : out) send(dg);
			for (auto &notice : session_.take_notices()) notices_.push_back(std::move(notice));
		}
		return done();
	}

	ClientSession session_;
	net::ScopedSocket socket_;
	net::Endpoint service_{};
	uint32_t clock_ms_ = 0;
	std::vector<ClientSession::Notice> notices_;
};

// The Lister with an account: its listing is the account's once its first
// update carries the key it fetched, which host_keys holds as a hash claimed for
// the row's RID. Its stop deregisters.
void lister_with_account(Service &service) {
	FixedSource source("Owned Lister");
	opennova::nw_lister::Lister lister(lister_options(service, true), source, nullptr);
	expect(lister.start(), "lister: starts");
	ListerRun run{lister};
	const bool hosting = run.until([&] { return lister.hosting(); });
	expect(hosting && source.hosting, "lister: logs in, fetches its HOSTKEY and hosts");
	const bool owned = run.until([&] {
		const auto r = service.row("Owned Lister");
		return r && r->host_user_id == service.owner_id;
	}, {}, 500);
	expect(owned, "lister: active_hosts.host_user_id is the account");
	const auto lobby = service.lobby("Owned Lister");
	expect(lobby && lobby->host_user_id == service.owner_id, "lister: the lobby state names the account");
	if (lobby) {
		expect(hostdb::is_host_key(lobby->host_key), "lister: its update carried the minted HOSTKEY");
		const auto key = service.key_row(lobby->host_key);
		expect(key && key->first == service.owner_id && key->second == static_cast<int64_t>(lobby->rid),
		       "lister: host_keys holds the key's hash for the account, claimed for the RID");
		const auto mine = hostdb::list_hosts_by_owner(*service.pool.acquire(), service.owner_id);
		expect(mine.size() == 1 && mine[0].rid == lobby->rid, "lister: the account lists one server");
	}
	lister.stop();
	expect(lister.exit_code() == opennova::nw_lister::kExitStopped, "lister: a stop exits 0");
	expect(wait_for([&] { return !service.row("Owned Lister"); }), "lister: the stop deregisters");
}

// The Lister with no account: listed with no owner by a lenient service (its
// update's empty HostKey names nobody).
void anonymous_lister_listed(Service &service) {
	FixedSource source("Anonymous Lister");
	opennova::nw_lister::Lister lister(lister_options(service, false), source, nullptr);
	expect(lister.start(), "anonymous: starts");
	ListerRun run{lister};
	expect(run.until([&] { return lister.hosting(); }), "anonymous: hosts");
	const bool decided = run.until([&] {
		const auto lobby = service.lobby("Anonymous Lister");
		return lobby && lobby->owner_key.has_value();
	}, {}, 500);
	expect(decided, "anonymous: its update presents its (empty) HostKey");
	expect(service.row_owned_by("Anonymous Lister", std::nullopt),
	       "anonymous: listed, active_hosts.host_user_id NULL");
	lister.stop();
	expect(wait_for([&] { return !service.row("Anonymous Lister"); }), "anonymous: deregisters");
}

// The Lister with no account against ONNET_REQUIRE_HOST_KEY: answered, never
// listed, and ended with ServerStopHosting once its update's HostKey names
// nobody.
void anonymous_lister_stopped(Service &strict) {
	FixedSource source("Refused Lister");
	opennova::nw_lister::Lister lister(lister_options(strict, false), source, nullptr);
	expect(lister.start(), "strict lister: starts");
	bool ever_listed = false;
	ListerRun run{lister};
	run.until([&] { return lister.finished(); },
	          [&] { ever_listed = ever_listed || !hostdb::list_hosts(*strict.pool.acquire()).empty(); });
	expect(lister.finished(), "strict lister: finishes");
	expect(lister.exit_code() == opennova::nw_lister::kExitStoppedByService,
	       "strict lister: the service stopped its hosting");
	expect(!source.hosting && source.failure == kNeedToLogin,
	       "strict lister: with NWUSERVERMSGCODE_NEEDTOLOGIN");
	expect(!ever_listed, "strict lister: never in the browser");
}

// On the strict service: a logged-in host held until its key, then listed as
// its account's; a second connection presenting the key ended; the reconnect's
// re-host keeping the RID and the owner; a re-host with the keys revoked
// refused with MsgCode 200 (NWEC58).
void strict_flow(Service &strict) {
	const std::string key = fetch_host_key(strict);
	expect(hostdb::is_host_key(key), "strict: a logged-in client fetches a HOSTKEY");
	const auto record = strict.key_row(key);
	expect(record && record->first == strict.owner_id && !record->second,
	       "strict: /NWHost.dll recorded the key for the account, unclaimed");

	RawHost host(strict.nwudp.bound_port(), 0x4f574e31u, 0x4f574b10u);
	expect(host.verify(), "strict: the host verifies");
	expect(host.host("Strict Owned") == ClientSession::HostState::Established,
	       "strict: the keyless fresh request is answered");
	const auto held = strict.lobby("Strict Owned");
	expect(held && held->awaiting_host_key, "strict: the host is held for its HostKey");
	expect(!strict.row("Strict Owned"), "strict: held out of the browser");
	host.present_key(key);
	expect(strict.row_owned_by("Strict Owned", strict.owner_id), "strict: the key lists it as the account's");
	const auto owned = strict.lobby("Strict Owned");
	const uint32_t rid = owned ? owned->rid : 0;
	expect(rid != 0 && !owned->awaiting_host_key, "strict: no longer held");

	// Another connection presenting the same key while it is listed.
	RawHost thief(strict.nwudp.bound_port(), 0x54484631u, 0x54484b10u);
	expect(thief.verify(), "strict thief: verifies");
	expect(thief.host("Strict Thief") == ClientSession::HostState::Established,
	       "strict thief: the keyless request is answered");
	thief.present_key(key);
	expect(thief.stopped_with(kNeedToLogin), "strict thief: ended with NWUSERVERMSGCODE_NEEDTOLOGIN");
	expect(thief.session().host_state() == ClientSession::HostState::Idle &&
	               thief.session().host_result().msg_code == opennova::SERVER_MSG_CODE_NEED_TO_LOGIN,
	       "strict thief: back to state 4 with MsgCode 200");
	expect(!strict.row("Strict Thief") && !strict.lobby("Strict Thief"), "strict thief: never listed");
	thief.goodbye();

	// The reconnect keeps the RID and the owner: the re-host carries the key.
	auto rehost = host.reconnect([&] { return !strict.row("Strict Owned"); });
	expect(rehost == ClientSession::HostState::Established, "strict reconnect: the re-host is accepted");
	expect(host.session().disconnect_count() == 1, "strict reconnect: the session counted the disconnect");
	const auto after = strict.lobby("Strict Owned");
	expect(after && after->rid == rid && after->host_user_id == strict.owner_id,
	       "strict reconnect: the re-host keeps its RID and its owner");
	expect(strict.row_owned_by("Strict Owned", strict.owner_id),
	       "strict reconnect: active_hosts names the account again");
	const auto claim = strict.key_row(key);
	expect(claim && claim->second == static_cast<int64_t>(rid), "strict reconnect: the claim is the RID's");

	// The account's keys revoked (an admin password reset deletes them): the
	// next re-host's key names nobody, and the request is refused.
	strict.pool.acquire()->exec("DELETE FROM host_keys WHERE user_id = ?;",
	                            {opennova::db::BindValue(strict.owner_id)});
	rehost = host.reconnect([&] { return !strict.row("Strict Owned"); });
	expect(rehost == ClientSession::HostState::Failed, "strict revoked: the re-host is refused");
	expect(host.session().host_result().msg_code == opennova::SERVER_MSG_CODE_NEED_TO_LOGIN &&
	               opennova::novaworld_host_error_tag(host.session().host_result().msg_code) == "NWEC58",
	       "strict revoked: MsgCode 200, NWEC58");
	expect(!strict.row("Strict Owned") && !strict.lobby("Strict Owned"), "strict revoked: not listed");
	host.goodbye();
}

// On the lenient service a second connection presenting a listed key is
// listed with no owner; the key stays the first listing's.
void lenient_thief_unlinked(Service &service) {
	const std::string key = fetch_host_key(service);
	expect(hostdb::is_host_key(key), "lenient: a logged-in client fetches a HOSTKEY");
	RawHost host(service.nwudp.bound_port(), 0x4c4e4831u, 0x4c4e4b10u);
	expect(host.verify() && host.host("Lenient Owned") == ClientSession::HostState::Established,
	       "lenient: the owner hosts");
	expect(service.row_owned_by("Lenient Owned", std::nullopt),
	       "lenient: listed at once, no owner before the key");
	host.present_key(key);
	expect(service.row_owned_by("Lenient Owned", service.owner_id), "lenient: the key links it");

	RawHost thief(service.nwudp.bound_port(), 0x4c544831u, 0x4c544b10u);
	expect(thief.verify() && thief.host("Lenient Thief") == ClientSession::HostState::Established,
	       "lenient thief: hosts");
	thief.present_key(key);
	const bool decided = wait_for([&] {
		const auto lobby = service.lobby("Lenient Thief");
		return lobby && lobby->owner_key.has_value();
	});
	expect(decided, "lenient thief: its key is presented");
	expect(service.row_owned_by("Lenient Thief", std::nullopt), "lenient thief: listed with no owner");
	const auto owned = service.lobby("Lenient Owned");
	const auto claim = service.key_row(key);
	expect(owned && claim && claim->second == static_cast<int64_t>(owned->rid),
	       "lenient: the key stays claimed for the owner's RID");
	thief.goodbye();
	host.goodbye();
	expect(wait_for([&] { return !service.row("Lenient Owned") && !service.row("Lenient Thief"); }),
	       "lenient: both deregister");
}

} // namespace

int main() {
	opennova::io::set_log_sink(&log_sink);
	if (net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	{
		// Declared first, so the databases' files go only once every
		// connection is closed.
		test_temp::TempDir temp("nw_host_ownership");
		Service lenient(temp.path / "lenient.db");
		Service strict(temp.path / "strict.db");
		const bool up = lenient.start(source_dir, temp.path, /*require_host_key=*/false) &&
		                strict.start(source_dir, temp.path, /*require_host_key=*/true);
		expect(up, "both services start");
		if (up) {
			lister_with_account(lenient);
			anonymous_lister_listed(lenient);
			lenient_thief_unlinked(lenient);
			anonymous_lister_stopped(strict);
			strict_flow(strict);
		}
		strict.stop();
		lenient.stop();
	}
	net::shutdown();
	opennova::io::set_log_sink(nullptr);
	if (g_failures == 0) {
		std::printf("OK: listed servers link to the account whose HOSTKEY listed them\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
