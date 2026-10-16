// Standalone novaworld server entrypoint.
//
// Wires the gate UDP, NW UDP, HTTP listeners into one process over the
// service's PostgreSQL database. Architecture and design rationale: the
// README.md beside this file.
// This is our NovaWorld server, not an emulator (CONTEXT.md "NovaWorld").

#include "gate_listener.h"
#include "health.h"
#include "loggable.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#ifdef OPENNOVA_HTTP_ENABLED
#include "auth.h"
#include "billing/billing.h"
#include "billing/billing_config.h"
#include "billing/billing_repository.h"
#include "billing/stripe_api.h"
#include "command_runner.h"
#include "hosted/ec2_machine_driver.h"
#include "hosted/fleet_config.h"
#include "hosted/fleet_repository.h"
#include "hosted/hosted_settings.h"
#include "hosted/internal_listener.h"
#include "hosted/local_machine_driver.h"
#include "hosted/owner_file.h"
#include "hosted/supervisor.h"
#include "http_listener.h"
#include "mailer.h"
#include "session_store.h"
#include "web_session.h"

#include <net/napi/session.h>
#endif

#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/unknown_tracker.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <cstdlib>
#include <base/io/log.h>

namespace {

std::atomic<bool> g_shutdown{false};

// The first signal starts the orderly shutdown; the handler then puts the
// default action back, so the same signal again ends the process even when
// that shutdown is stuck. Both calls are signal-safe: a lock-free atomic store,
// and std::signal for the signal being handled. (Windows' CRT resets a handler
// to SIG_DFL before calling it anyway, so a second Ctrl+C there already ended
// the process; the reset brings POSIX to the same behavior.) Not as a PID
// namespace's init, though: the kernel drops a default-action SIGINT or SIGTERM
// sent to a container's PID 1 (pid_namespaces(7)), so the second one would do
// nothing there. deploy/compose runs the server under Docker's init
// (`init: true`), which is PID 1 and forwards the signals.
void on_signal(int sig) {
	g_shutdown.store(true);
	std::signal(sig, SIG_DFL);
}

// main() alone owns SIGINT and SIGTERM (Ctrl+C; `docker stop` sends SIGTERM):
// the handler raises g_shutdown (and restores the default action, above), and
// the tick loop then runs the orderly shutdown. The HTTP listener keeps Crow's
// own signal handling off both.
void install_signal_handlers() {
	std::signal(SIGINT,  on_signal);
	std::signal(SIGTERM, on_signal);
}

// Every seed file, each a script the server runs as one transaction. The
// caller holds the migration lock, so two services booting on one database
// seed it one after the other.
void apply_seed(opennova::db::Database &db,
                const std::filesystem::path &seed_dir,
                bool include_dev_users) {
	if (!std::filesystem::exists(seed_dir)) {
		std::printf("[boot] seed dir %s missing; skipping seed\n",
		            seed_dir.string().c_str());
		return;
	}
	std::vector<std::filesystem::path> files;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() != ".sql") continue;
		// Dev-only seed (the `test`/`foo` accounts with public passwords) is
		// skipped unless SEED_DEV_USERS is set — production must not create them.
		if (!include_dev_users &&
		    e.path().filename().string().find("dev_users") != std::string::npos) {
			std::printf("[boot] seed skipped (dev-only): %s\n",
			            e.path().filename().string().c_str());
			continue;
		}
		files.push_back(e.path());
	}
	std::sort(files.begin(), files.end());
	for (const auto &p : files) {
		std::ifstream in(p, std::ios::binary);
		std::ostringstream os;
		os << in.rdbuf();
		try {
			db.exec_script(os.str());
			std::printf("[boot] seed applied: %s\n",
			            p.filename().string().c_str());
		} catch (const opennova::db::Error &e) {
			std::fprintf(stderr,
			             "[boot] seed FAILED: %s — %s\n",
			             p.filename().string().c_str(), e.what());
		}
	}
}

#ifdef OPENNOVA_HTTP_ENABLED
// The hosted namespace under a name of its own here (main()'s using-directive
// brings in `hosted` itself).
namespace hosted_ns = opennova::novaworld_server::hosted;

// The hosted servers' fleet: the MachineDriver ONNET_HOSTED_PROVIDER names, the
// fleet's settings (hosted/fleet_config.h) and, for "ec2", what the driver
// stands on (the AWS CLI runner and its owner-only scratch directory). main()
// declares it before the HTTP listener and the internal listener; the driver
// alone is reset at the shutdown.
struct HostedFleet {
	// The data sets the driver offers (hosted/data_sets.h); the supervisor's.
	std::vector<hosted_ns::DataSet> data_sets;
	hosted_ns::FleetConfig fleet;
	std::unique_ptr<opennova::proc::CommandRunner> runner;
	std::filesystem::path scratch_dir; // the ec2 driver's, removed at the shutdown
	hosted_ns::LocalMachineDriver *local = nullptr; // the driver, when it is the local one
	std::unique_ptr<hosted_ns::MachineDriver> driver;
};

// The slugs of `data_sets`, in their order.
std::vector<std::string> data_set_slugs(const std::vector<hosted_ns::DataSet> &data_sets) {
	std::vector<std::string> out;
	for (const hosted_ns::DataSet &d : data_sets) out.push_back(d.game);
	return out;
}

// The ec2 driver over `f.data_sets` and `f.fleet`, or nothing with a boot
// warning naming what it lacks.
void make_ec2_driver(const opennova::novaworld_server::ServerConfig &config, HostedFleet &f) {
	using opennova::novaworld_server::loggable;
	const auto off = [](const std::string &why) {
		std::fprintf(stderr, "[boot] WARN hosted servers: the ec2 driver %s; hosting off\n", why.c_str());
	};
	// The servers list through the service's public address, so the listing
	// records each machine's own public address (ADR 0053).
	if (config.public_host.empty() || config.public_host == "127.0.0.1" || config.public_host == "localhost") {
		off("needs ONNET_PUBLIC_HOST, the public address its servers list on");
		return;
	}
	hosted_ns::Ec2MachineConfig &ec2 = f.fleet.ec2;
	ec2.data_sets = data_set_slugs(f.data_sets);
	std::string error;
	if (!hosted_ns::load_machine_script(f.fleet.machine_script_path, ec2.machine_script, error)) {
		off(loggable(error, 300) + " (ONNET_HOSTED_MACHINE_SCRIPT)");
		return;
	}
	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
	const std::filesystem::path scratch = temp / ("opennova-hosted-" + hosted_ns::random_secret(12));
	if (ec || !hosted_ns::make_owner_directory(scratch, error)) {
		off("cannot make its scratch directory" + (error.empty() ? std::string() : ": " + error));
		return;
	}
	ec2.scratch_dir = scratch.u8string();
	if (const std::string why = hosted_ns::Ec2MachineDriver::check_config(ec2); !why.empty()) {
		std::filesystem::remove_all(scratch, ec);
		off("is not configured: " + why);
		return;
	}
	f.runner = std::make_unique<opennova::proc::ProcessCommandRunner>(
			opennova::proc::ProcessCommandRunner::service_environment(), ec2.scratch_dir);
	f.scratch_dir = scratch;
	std::printf("[boot] hosted servers: ec2 in %s, subnet %s, deployment %s, shared machines from %s, own from %s\n",
	            ec2.region.c_str(), ec2.subnet.c_str(), ec2.deployment.c_str(), ec2.shared_template.c_str(),
	            ec2.own_template.c_str());
	std::printf("[boot] hosted servers: serve image %s, hostd image %s\n", loggable(ec2.serve_image, 255).c_str(),
	            loggable(ec2.hostd_image.empty() ? ec2.serve_image : ec2.hostd_image, 255).c_str());
	f.driver = std::make_unique<hosted_ns::Ec2MachineDriver>(ec2, *f.runner);
}

// The hosted servers' fleet ONNET_HOSTED_PROVIDER names, over the data sets
// its driver offers; no driver when hosting is off ("none", an unknown name,
// fleet settings or data sets that did not read, none the driver has the data
// of, or a driver missing what it needs; each but "none" logged as a warning).
HostedFleet make_hosted_fleet(const opennova::novaworld_server::ServerConfig &config) {
	using opennova::novaworld_server::loggable;
	HostedFleet f;
	if (config.hosted_provider == "none") return f;
	if (config.hosted_provider != "local" && config.hosted_provider != "ec2") {
		std::fprintf(stderr, "[boot] WARN hosted servers: unknown ONNET_HOSTED_PROVIDER '%s'; hosting off\n",
		             loggable(config.hosted_provider).c_str());
		return f;
	}
	std::vector<std::string> errors;
	f.fleet = hosted_ns::fleet_config_from_env(errors);
	if (!errors.empty()) {
		for (const std::string &e : errors)
			std::fprintf(stderr, "[boot] WARN hosted servers: %s\n", loggable(e, 300).c_str());
		std::fprintf(stderr, "[boot] WARN hosted servers: the fleet's settings do not read; hosting off\n");
		return f;
	}
	std::vector<hosted_ns::DataSetConfig> configs;
	std::string error;
	if (!hosted_ns::parse_data_set_configs(config.hosted_data_sets, configs, error)) {
		std::fprintf(stderr, "[boot] WARN hosted servers: %s; hosting off\n", loggable(error, 300).c_str());
		return f;
	}
	f.data_sets = hosted_ns::offered_data_sets(config.hosted_provider, configs);
	if (f.data_sets.empty()) {
		std::fprintf(stderr, "[boot] WARN hosted servers: the %s driver has no data set (ONNET_HOSTED_DATA_SETS %s); "
		                     "hosting off\n",
		             config.hosted_provider.c_str(),
		             config.hosted_provider == "ec2" ? "names none" : "gives none a resource_dir");
		return f;
	}
	if (config.hosted_provider == "ec2") {
		make_ec2_driver(config, f);
		return f;
	}
	if (config.hosted_serve_bin.empty()) {
		std::fprintf(stderr, "[boot] WARN hosted servers: the local driver needs "
		                     "ONNET_HOSTED_SERVE_BIN; hosting off\n");
		return f;
	}
	for (const hosted_ns::DataSet &d : f.data_sets)
		std::printf("[boot] hosted servers: data set %s from %s%s\n", d.game.c_str(),
		            loggable(d.resource_dir, 260).c_str(), d.loose_root ? " (loose)" : "");
	hosted_ns::LocalMachineConfig local;
	local.serve_binary = config.hosted_serve_bin;
	local.data_sets = f.data_sets;
	local.work_dir = config.hosted_work_dir;
	auto driver = std::make_unique<hosted_ns::LocalMachineDriver>(std::move(local));
	f.local = driver.get();
	f.driver = std::move(driver);
	// The local driver's containers run its own binary; the rows name an image all the same.
	if (f.fleet.ec2.serve_image.empty()) f.fleet.ec2.serve_image = "opennova-serve:local";
	return f;
}

namespace billing_ns = opennova::novaworld_server::billing;

// Billing (billing/billing.h) as main() runs it: the curl transport (its
// runner and its owner-only scratch directory, removed at the shutdown) and the
// billing over it. Declared before the HTTP listener, whose routes use it.
struct BillingStack {
	std::filesystem::path scratch_dir;
	std::unique_ptr<opennova::proc::CommandRunner> runner;
	std::unique_ptr<billing_ns::CurlStripeApi> api;
	std::unique_ptr<billing_ns::Billing> billing;
};

// Billing over `config`, or none with a boot warning naming why: settings not
// complete and well-formed (check_billing_config), no scratch directory, a
// transport that would not run. Both secrets are log secrets either way.
BillingStack make_billing(const billing_ns::BillingConfig &config, opennova::db::ConnectionPool &pool,
                          opennova::db::Database &db) {
	using opennova::novaworld_server::add_log_secret;
	BillingStack s;
	add_log_secret(config.secret_key);
	add_log_secret(config.webhook_secret);
	const auto off = [](const std::string &why) {
		std::fprintf(stderr, "[boot] WARN billing: %s; billing off\n", why.c_str());
	};
	if (const std::string why = billing_ns::check_billing_config(config); !why.empty()) {
		off(why);
		return s;
	}
	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
	std::string error;
	const std::filesystem::path scratch = temp / ("opennova-billing-" + hosted_ns::random_secret(12));
	if (ec || !hosted_ns::make_owner_directory(scratch, error)) {
		off("no scratch directory for the Stripe calls" + (error.empty() ? std::string() : ": " + error));
		return s;
	}
	billing_ns::CurlStripeApi::Config api;
	api.curl = config.curl_path;
	api.secret_key = config.secret_key;
	api.scratch_dir = scratch.u8string();
	if (const std::string why = billing_ns::CurlStripeApi::check_config(api); !why.empty()) {
		std::filesystem::remove_all(scratch, ec);
		off("the Stripe transport: " + why);
		return s;
	}
	std::string instance;
	try {
		instance = billing_ns::instance_id(db);
	} catch (const opennova::db::Error &e) {
		std::filesystem::remove_all(scratch, ec);
		off(std::string("no instance id: ") + e.what());
		return s;
	}
	s.scratch_dir = scratch;
	s.runner = std::make_unique<opennova::proc::ProcessCommandRunner>(billing_ns::curl_environment(), api.scratch_dir);
	s.api = std::make_unique<billing_ns::CurlStripeApi>(*s.runner, std::move(api));
	s.billing = std::make_unique<billing_ns::Billing>(config, *s.api, pool, instance);
	std::printf("[boot] billing: on, %s mode, instance %s, checkout returns to %s%s\n", config.live() ? "live" : "test",
	            instance.c_str(), config.site_url.c_str(), config.automatic_tax ? ", Stripe Tax on" : "");
	return s;
}

namespace ns = opennova::novaworld_server;

// Outbound mail (mailer.h) as main() runs it: the mailer (SES through the AWS
// CLI, with its runner and its owner-only scratch directory, removed at the
// shutdown; a NullMailer while mail is off) and the outbox the routes post
// to. Declared before the HTTP listener, whose routes post to it.
struct MailStack {
	std::filesystem::path scratch_dir;
	std::unique_ptr<opennova::proc::CommandRunner> runner;
	std::unique_ptr<ns::Mailer> mailer;
	std::unique_ptr<ns::MailOutbox> outbox;
};

// SES over the config's ONNET_MAIL_FROM, ONNET_MAIL_REGION and ONNET_SITE_URL,
// or mail off, logged: unset (no sender), else a warning naming what is wrong.
MailStack make_mail(const ns::ServerConfig &config) {
	MailStack s;
	const auto off = [&s](bool warn, const std::string &why) {
		std::fprintf(warn ? stderr : stdout, "[boot] %smail: %s; mail off\n", warn ? "WARN " : "", why.c_str());
		s.mailer = std::make_unique<ns::NullMailer>();
		s.outbox = std::make_unique<ns::MailOutbox>(*s.mailer);
	};
	if (config.mail_from.empty()) {
		off(false, "no ONNET_MAIL_FROM");
		return s;
	}
	if (config.site_url.empty()) {
		off(true, "ONNET_SITE_URL is not set, so no link could be sent");
		return s;
	}
	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
	std::string error;
	const std::filesystem::path scratch = temp / ("opennova-mail-" + hosted_ns::random_secret(12));
	if (ec || !hosted_ns::make_owner_directory(scratch, error)) {
		off(true, "no scratch directory for the SES calls" + (error.empty() ? std::string() : ": " + error));
		return s;
	}
	ns::SesMailerConfig ses;
	ses.region = config.mail_region;
	ses.from = config.mail_from;
	ses.scratch_dir = scratch.u8string();
	if (const std::string why = ns::SesMailer::check_config(ses); !why.empty()) {
		std::filesystem::remove_all(scratch, ec);
		off(true, why);
		return s;
	}
	s.scratch_dir = scratch;
	s.runner = std::make_unique<opennova::proc::ProcessCommandRunner>(
			opennova::proc::ProcessCommandRunner::service_environment(), ses.scratch_dir);
	s.mailer = std::make_unique<ns::SesMailer>(ses, *s.runner);
	s.outbox = std::make_unique<ns::MailOutbox>(*s.mailer);
	std::printf("[boot] mail: on, SES in %s, from %s, links to %s\n", ses.region.c_str(), ses.from.c_str(),
	            config.site_url.c_str());
	return s;
}
#endif

} // namespace


namespace {
// The engine/ diagnostic channel (io/log.h): libraries are silent until the host
// installs a sink. Reproduce the historical stream split — lifecycle to stdout,
// warnings and errors to stderr; per-tick kDebug tracing opts in via NW_LOG_DEBUG.
// A message is one line: its control bytes go out escaped (loggable), so a
// name a NovaWorld session carried into an engine log line cannot forge another,
// and every registered log secret masked (a hosted server's admin password).
bool g_log_debug_enabled = false;
void app_log_sink(opennova::io::LogLevel level, const char *msg) {
	if (level == opennova::io::LogLevel::kDebug && !g_log_debug_enabled) return;
	const std::string text = opennova::novaworld_server::mask_log_secrets(msg);
	std::fprintf(level >= opennova::io::LogLevel::kWarn ? stderr : stdout, "%s\n",
			opennova::novaworld_server::loggable(text, text.size()).c_str());
}
} // namespace

int main() {
	g_log_debug_enabled = std::getenv("NW_LOG_DEBUG") != nullptr;
	opennova::io::set_log_sink(&app_log_sink);
	using namespace opennova;
	using namespace opennova::novaworld_server;

	// Force unbuffered stdout/stderr so log lines actually flush when
	// running under a non-tty parent (background pipe, file redirect,
	// etc). _IOLBF crashed MSVC's CRT with STATUS_STACK_BUFFER_OVERRUN —
	// _IONBF is the safe form on Windows.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);

	const ServerConfig config = ServerConfig::from_env();

	std::printf("==== opennova-novaworld-server booting ====\n");
	std::printf("  public_host=%s\n", config.public_host.c_str());
	std::printf("  gate_udp=:%u  nw_udp=:%u  http=:%u\n",
	            config.gate_udp_port, config.nw_udp_port, config.http_port);
	std::printf("  database=%s (pool of %zu)\n", db::masked_url(config.database_url).c_str(),
	            config.database_pool_max);

	// --- DB: connect, migrate, seed --------------------------------------
	// The password never reaches a log line.
	add_log_secret(config.database_password);
	db::ConnectOptions db_options;
	db_options.url = config.database_url;
	db_options.password = config.database_password;
	// Every thread that touches the database owns its connection (Database is
	// single-threaded), and a thread holds one lease at a time (database.h):
	// this thread holds one for the boot's work, none while it starts and
	// stops the listeners and workers (each leases its own, some of them on
	// this thread), and one for the main loop; the gate's and the NW UDP
	// listener's receive threads each lease one for their lifetime (start()
	// waits for it, failing the boot when the connect does); each HTTP
	// request leases one for the call, and erase_lobby_state writes on the
	// calling thread's. The pool outlives every listener (declared first,
	// destroyed last).
	std::unique_ptr<db::ConnectionPool> db_pool;
	std::optional<db::ConnectionPool::Lease> boot_lease;
	try {
		db_pool = std::make_unique<db::ConnectionPool>(db_options, config.database_pool_max);
		boot_lease.emplace(db_pool->acquire());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] FATAL: open db failed: %s\n", e.what());
		return 1;
	}
	db::Database &boot_db = **boot_lease;

	// The migrations and the seeds under one hold of the migration lock.
	try {
		const db::SessionLock migrating(boot_db, db::kMigrationLock);
		auto r = db::run_migrations(boot_db, config.migrations_dir);
		std::printf("[boot] migrations: %zu applied, %zu skipped\n",
		            r.applied.size(), r.skipped.size());
		apply_seed(boot_db, config.seed_dir, config.seed_dev_users);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[boot] FATAL: migrations failed: %s\n", e.what());
		return 1;
	}

#ifdef OPENNOVA_HTTP_ENABLED
	// The first website admin: ONNET_BOOTSTRAP_ADMIN names an existing account,
	// promoted only while no account is an admin (bootstrap_admin).
	if (!config.bootstrap_admin.empty()) {
		const std::string name = loggable(config.bootstrap_admin);
		try {
			const auto result = bootstrap_admin(boot_db, config.bootstrap_admin);
			switch (result.outcome) {
			case BootstrapAdminResult::Outcome::Promoted:
				std::printf("[boot] bootstrap admin: '%s' is now the first admin; remove "
				            "ONNET_BOOTSTRAP_ADMIN\n",
				            name.c_str());
				break;
			case BootstrapAdminResult::Outcome::AdminExists:
				std::fprintf(stderr,
				             "[boot] WARN bootstrap admin: skipped '%s', an admin exists ('%s'); "
				             "remove ONNET_BOOTSTRAP_ADMIN\n",
				             name.c_str(), loggable(result.existing_admin).c_str());
				break;
			case BootstrapAdminResult::Outcome::NoSuchAccount:
				std::fprintf(stderr,
				             "[boot] WARN bootstrap admin: skipped, no account named '%s'\n",
				             name.c_str());
				break;
			}
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[boot] WARN bootstrap admin: %s\n", e.what());
		}
	}
#endif

	// --- Connection manager (shared across listeners) ---------------------
	ConnectionManager manager(config.heartbeat_timeout_ms);
	manager.on_added([](const Connection &c) {
		std::printf("[conn] added id=0x%08x addr=%s pn=%s\n", c.id,
				peer_addr_to_string(c.addr).c_str(), loggable(c.pn).c_str());
	});

	// --- Unknown-message tracker (shared across listeners) ----------------
	// Listener threads record() sightings of anything we don't handle; the
	// main tick loop flushes them into unknown_messages (never per-packet).
	UnknownTracker unknown_tracker;

	// --- Previous-run cleanup, before any listener serves -----------------
	// Phase I.2: drop stale active_hosts rows from the previous server
	// run before accepting new connections (the UDP HELLO from any
	// surviving retail process will fail anyway, so its prior row is
	// dead state).
	try {
		hostdb::clear_all(boot_db);
		std::printf("[boot] active_hosts + host_players + host key claims cleared (previous-run cleanup)\n");
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] WARN active_hosts clear: %s\n", e.what());
	}
#ifdef OPENNOVA_HTTP_ENABLED
	try {
		clear_all_active_user_sessions(boot_db);
		std::printf("[boot] active_user_sessions cleared (previous-run cleanup)\n");
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] WARN active_user_sessions clear: %s\n", e.what());
	}
#endif

	// --- Listeners --------------------------------------------------------
	GateListener gate;
	gate.set_unknown_tracker(&unknown_tracker);
	gate.set_db_pool(db_pool.get());

	NwUdpListener nwudp(manager);
	nwudp.set_db_pool(db_pool.get());
	// The endpoint advertised to joiners uses the client's NOVAWORLD session port
	// (client_reflect_novaworld_port, 32768), NOT the gate port
	// (client_reflect_gate_port, 49152 — that's only the gate's
	// ReflectedPortNumber). See server_config.h.
	nwudp.set_reflect_endpoint(config.client_reflect_ip, config.client_reflect_novaworld_port);
	nwudp.set_unknown_tracker(&unknown_tracker);

	// Wired once NwUdpListener exists, before any listener starts, so the
	// lost-handler can clean up the per-connection lobby_state entry too.
	// Without this, a hosting retail process that exits without GOODBYE
	// (heartbeat timeout) would stay in /api/hosts forever (G.6). Pass the
	// dropped Connection's PeerAddr — lobby_states_ is keyed by addr, not by
	// ci (two retail processes both send ci=0x00000001) (G.7).
	manager.on_lost([&nwudp](const Connection &c, DropReason r) {
		std::printf("[conn] lost  id=0x%08x identity='%s' reason=%s\n", c.id,
				loggable(c.identity).c_str(), drop_reason_name(r));
		nwudp.erase_lobby_state(c.addr, drop_reason_name(r));
	});

	// Each listener advertises its siblings' ports as the config it starts
	// with names them: the NW UDP listener the HTTP port (the SessionInit's
	// web domain), the gate both (UDPNOVAWORLD, STARTUPURL). So they start in
	// the order those ports become known, HTTP, then NW UDP, then the gate,
	// and once one binds, `listening` carries the port it serves: the
	// configured port, or the OS's pick for port 0. With nonzero ports every
	// advertised byte stays the configured port's; and the gate, the client's
	// first contact, answers only once what it names serves.
	ServerConfig listening = config;
	// The stall watchdog (health.h): main()'s loop samples the receive loops'
	// (and the supervisor's) turn counters; GET /healthz reads it, so it is
	// declared before the HTTP listener and outlives it.
	Watchdog watchdog;
#ifdef OPENNOVA_HTTP_ENABLED
	// The hosted servers' machine driver, decided before the routes start (they
	// answer that hosting is off without one); its supervisor starts once the
	// gate has its port. Declared before the supervisor and the internal
	// listener, which use it.
	HostedFleet hosted_fleet = make_hosted_fleet(config);
	std::unique_ptr<hosted::MachineDriver> &hosted_driver = hosted_fleet.driver;
	// The admin links to the running hosted servers (hosted/admin_link.h): the
	// supervisor keeps them in step with its rows; declared before the HTTP
	// listener, whose routes run commands on them, so they outlive it.
	std::unique_ptr<hosted::AdminLinks> admin_links;
	if (hosted_driver) admin_links = std::make_unique<hosted::AdminLinks>(db_pool.get());
	// The hosted servers' limits hold (hosted/limits_hold.h): the supervisor
	// sets it, a clean billing reconcile or an admin's override releases it;
	// it outlives the supervisor, the billing worker and the HTTP listener.
	hosted::LimitsHold limits_hold;
	// The supervisor, then the internal listener whose agent routes ask it
	// what each machine runs (declared after it, so destroyed first).
	std::unique_ptr<hosted::Supervisor> supervisor;
	std::unique_ptr<hosted::InternalListener> internal_listener;
	listening.hosted_provider = hosted_driver ? hosted_driver->name() : "none";
	// Billing (the monthly support tiers; billing/): on only when its settings
	// are complete. Declared before the HTTP listener, whose routes use it; its
	// sync worker starts once the listeners serve.
	BillingStack billing_stack =
			make_billing(billing::billing_config_from_env(), *db_pool, boot_db);
	// A clean reconcile confirms the cache the limits hold doubts.
	if (billing_stack.billing)
		billing_stack.billing->sync().set_on_reconciled([&limits_hold] { limits_hold.reconciled(); });
	// Outbound mail (the welcome mail, the password reset links); its outbox
	// outlives the HTTP listener, whose routes post to it.
	MailStack mail_stack = make_mail(config);
	SessionStore sessions;
	HttpListener http(manager, *db_pool, sessions);
	http.set_unknown_tracker(&unknown_tracker);
	http.set_mail_outbox(mail_stack.outbox.get());
	http.set_watchdog(&watchdog);
	http.set_billing(billing_stack.billing.get());
	http.set_limits_hold(&limits_hold);
	// The admin host routes push ServerCommand / ServerStopHosting through the
	// NW UDP listener, which outlives this one (stopped after it below).
	http.set_nw_udp_listener(&nwudp);
	// The console routes run their commands on the admin links (declared above,
	// so they outlive the listener); none without a driver.
	http.set_admin_links(admin_links.get());
	// A start's at_capacity and the list's `full` ask the fleet's planner.
	http.set_fleet_placement(hosted_fleet.fleet.placement);
	// The admin usage view's limits: each container's memory and CPUs.
	http.set_fleet_slots(hosted_fleet.fleet.slot_memory_mb, hosted_fleet.fleet.slot_cpus);
#endif
	// The boot's work is done: the listeners and workers below lease their own
	// connections, some of them on this thread, which then holds none.
	boot_lease.reset();
#ifdef OPENNOVA_HTTP_ENABLED
	if (!http.start(listening)) {
		std::fprintf(stderr, "[boot] FATAL: HTTP listener start failed\n");
		return 1;
	}
	listening.http_port = http.bound_port();
#endif
	if (!nwudp.start(listening)) {
		std::fprintf(stderr, "[boot] FATAL: NW UDP listener start failed\n");
#ifdef OPENNOVA_HTTP_ENABLED
		http.stop();
#endif
		return 1;
	}
	listening.nw_udp_port = nwudp.bound_port();
	if (!gate.start(listening)) {
		std::fprintf(stderr, "[boot] FATAL: gate listener start failed\n");
#ifdef OPENNOVA_HTTP_ENABLED
		http.stop();
#endif
		nwudp.stop();
		return 1;
	}

#ifdef OPENNOVA_HTTP_ENABLED
	if (hosted_driver) {
		const hosted::FleetConfig &fleet = hosted_fleet.fleet;
		hosted::SupervisorConfig hosted_config;
		hosted_config.cap = config.hosted_cap;
		// The local driver's servers run on this machine, so they reach the gate
		// on loopback; a cloud machine lists through the service's public
		// address, so the listing records the machine's own public one.
		hosted_config.master_host = hosted_fleet.local ? "127.0.0.1" : config.public_host;
		hosted_config.master_gate_port = gate.bound_port();
		hosted_config.data_sets = hosted_fleet.data_sets;
		hosted_config.placement = fleet.placement;
		hosted_config.idle_ms = static_cast<uint64_t>(fleet.idle_minutes) * 60 * 1000;
		hosted_config.slot_cpus = fleet.slot_cpus;
		hosted_config.slot_memory_mb = fleet.slot_memory_mb;
		hosted_config.slot_pids = fleet.slot_pids;
		hosted_config.serve_image = fleet.ec2.serve_image;
		// A cloud container may pull its image before it starts, and lists over
		// the internet: longer clocks than a local one's.
		if (!hosted_fleet.local) {
			hosted_config.boot_timeout_ms = 10 * 60 * 1000;
			hosted_config.listing_timeout_ms = 5 * 60 * 1000;
		}
		supervisor = std::make_unique<hosted::Supervisor>(*db_pool, *hosted_driver, hosted_config);
		// A stop's ServerStopHosting, scoped to the service account that holds
		// the listing (the sysop punt's code, as the admin stop route's).
		supervisor->set_stop_listing([&nwudp](uint32_t rid, int64_t account) {
			return nwudp.push_stop_hosting(rid, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT, 0, 0, account) ==
			       HostPushResult::Queued;
		});
		supervisor->set_admin_links(admin_links.get());
		supervisor->set_limits_hold(&limits_hold);
		// The machine agents' routes, on the internal listener before the
		// supervisor's first pass launches a machine whose agent calls them.
		internal_listener = std::make_unique<hosted::InternalListener>(
				*db_pool, *supervisor, hosted::agent_route_config(fleet, data_set_slugs(hosted_fleet.data_sets)));
		if (!internal_listener->start(fleet.internal_address, fleet.internal_port)) {
			std::fprintf(stderr, "[boot] FATAL: the internal listener could not bind %s:%u (ONNET_INTERNAL_HTTP_BIND)\n",
			             fleet.internal_address.c_str(), static_cast<unsigned>(fleet.internal_port));
			gate.stop();
			http.stop();
			nwudp.stop();
			return 1;
		}
		if (hosted_fleet.local)
			hosted_fleet.local->set_service_url("http://127.0.0.1:" + std::to_string(internal_listener->bound_port()));
		supervisor->start();
	}
	// The billing worker: its first pass reconciles with Stripe and checks the prices.
	if (billing_stack.billing) billing_stack.billing->sync().start();
#endif

	// The loops the watchdog watches, each from here on.
	watchdog.watch("gate receive loop", [&gate] { return gate.loop_turns(); });
	watchdog.watch("NovaWorld UDP receive loop", [&nwudp] { return nwudp.loop_turns(); });
#ifdef OPENNOVA_HTTP_ENABLED
	if (supervisor) {
		hosted::Supervisor *watched = supervisor.get();
		watchdog.watch("hosted servers' supervisor", [watched] { return watched->pass_turns(); });
	}
#endif

	// The main loop's connection: the heartbeat's counts, the unknown-message
	// flush, the sweeps, and the lobby cleanup for a connection the manager's
	// tick drops (erase_lobby_state, on this thread's lease). Released before
	// the shutdown below, which stops the threads (leasing on this one).
	int exit_code = 0;
	std::optional<db::ConnectionPool::Lease> main_lease;
	try {
		main_lease.emplace(db_pool->acquire());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] FATAL: the main loop's connection: %s\n", e.what());
		exit_code = 1;
	}

	install_signal_handlers();
	if (main_lease)
		std::printf("[boot] ready. Ctrl+C to stop.\n");

	// --- Main loop: tick the connection manager ---------------------------
	uint64_t last_host_sweep_ms = 0;
	uint64_t last_heartbeat_ms = Watchdog::steady_now_ms();
#ifdef OPENNOVA_HTTP_ENABLED
	uint64_t launch_failures_reported = 0;
	uint64_t webhook_rejects_reported = 0;
#endif
	while (main_lease && !g_shutdown.load()) {
		db::Database &dbh = **main_lease;
		std::this_thread::sleep_for(
			std::chrono::milliseconds(config.tick_interval_ms));
		using namespace std::chrono;
		const auto now = static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		// A loop stuck past the stall time ends the process (exit 70), and the
		// container runtime starts it again: nothing here can unstick a thread.
		if (const std::string stalled = watchdog.check(); !stalled.empty()) {
			std::fprintf(stderr, "[watchdog] FATAL the %s made no progress for %llu s; exiting %d\n",
			             stalled.c_str(), static_cast<unsigned long long>(watchdog.stall_ms() / 1000),
			             Watchdog::kStallExitCode);
			std::fflush(nullptr);
			std::_Exit(Watchdog::kStallExitCode);
		}
		if (auto dropped = manager.tick(now); dropped > 0) {
			std::printf("[tick] expired %zu connection(s)\n", dropped);
		}
		// The heartbeat line (health.h), every kHeartbeatMs: the counts now, and
		// what happened since the last line.
		if (now - last_heartbeat_ms >= kHeartbeatMs) {
#ifdef OPENNOVA_HTTP_ENABLED
			const auto window_s = static_cast<int64_t>((now - last_heartbeat_ms + 999) / 1000);
#endif
			last_heartbeat_ms = now;
			Heartbeat beat;
			beat.nw_sessions = static_cast<int64_t>(manager.registry().size());
			try {
				beat.listed_hosts = hostdb::aggregate(dbh).lobbies;
#ifdef OPENNOVA_HTTP_ENABLED
				beat.hosted_live = hosted::count_live(dbh);
				beat.hosted_failed = hosted::count_failed_since(dbh, window_s);
				beat.machines_lost = hosted::count_machines(dbh, hosted::FleetMachineState::Lost);
				if (billing_stack.billing)
					beat.billing_sync_age_s = billing_stack.billing->sync().sync_age_s(dbh, billing::unix_now());
#endif
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[heartbeat] WARN counts: %s\n", e.what());
			}
#ifdef OPENNOVA_HTTP_ENABLED
			if (supervisor) {
				const uint64_t failures = supervisor->launch_failures();
				beat.launch_failures = static_cast<int64_t>(failures - launch_failures_reported);
				launch_failures_reported = failures;
			}
			if (billing_stack.billing) {
				const uint64_t rejects = billing_stack.billing->webhook_rejects();
				beat.webhook_rejects = static_cast<int64_t>(rejects - webhook_rejects_reported);
				webhook_rejects_reported = rejects;
			}
			beat.limits_held = limits_hold.held() ? 1 : 0;
#endif
			std::printf("%s\n", heartbeat_line(beat).c_str());
		}
		// Flush any unknown-message sightings the listener threads recorded
		// since the last tick into unknown_messages (deduped upsert).
		try {
			unknown_tracker.flush(dbh);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[tick] WARN unknown_tracker flush: %s\n", e.what());
		}

		// policy: throttled backstop sweep of crash-orphaned host rows the
		// normal GOODBYE/StopHosting/timeout teardown missed.
		if (now - last_host_sweep_ms >= config.host_sweep_interval_ms) {
			last_host_sweep_ms = now;
			try {
				const int pruned = hostdb::prune_stale_hosts(
					dbh, static_cast<int64_t>(config.host_stale_window_ms / 1000));
				if (pruned > 0) {
					std::printf("[sweep] pruned %d stale host row(s)\n", pruned);
				}
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[sweep] WARN prune_stale_hosts: %s\n", e.what());
			}
			// Hosting keys idle for a day; a listed server's key counts as used.
			try {
				if (const auto pruned = hostdb::prune_host_keys(dbh); pruned > 0) {
					std::printf("[sweep] pruned %zu idle host key(s)\n", pruned);
				}
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[sweep] WARN prune_host_keys: %s\n", e.what());
			}
#ifdef OPENNOVA_HTTP_ENABLED
			// Website sessions past their (sliding) expiry, and login addresses
			// not used for kLoginAddressDays.
			try {
				if (const auto pruned = prune_expired_web_sessions(dbh); pruned > 0) {
					std::printf("[sweep] pruned %zu expired web session(s)\n", pruned);
				}
				if (const auto pruned = prune_login_addresses(dbh); pruned > 0) {
					std::printf("[sweep] pruned %zu stale login address(es)\n", pruned);
				}
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[sweep] WARN web session / login address prune: %s\n", e.what());
			}
			// Webhook events synced more than 30 days ago (the cache keeps no
			// history Stripe does not).
			try {
				if (const auto pruned = billing::prune_events(dbh); pruned > 0) {
					std::printf("[sweep] pruned %zu old billing event(s)\n", pruned);
				}
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[sweep] WARN billing event prune: %s\n", e.what());
			}
#endif
		}
	}

	// This thread's lease first (what stops below leases on this thread: the
	// NW UDP listener's stop() and the manager's shutdown clean up their lobby
	// rows), then the HTTP listener (Crow's threads joined: no request still
	// holds a lease), then the gate and NW UDP receive threads (each returns
	// its lease as it exits), then the connections. As main() returns, its
	// locals are destroyed in reverse declaration order, so the pool (declared
	// first) goes last, closing every connection.
	main_lease.reset();
#ifdef OPENNOVA_HTTP_ENABLED
	// The supervisor's thread first (no pass runs from here on), then the admin
	// links while their servers still run (each logged-in session ends with
	// QUIT), then the driver, which ends the local driver's stand-in machines
	// and their servers while the listeners still serve their deregistration;
	// the cloud machines and their containers run on, and the next boot's
	// reconciliation resumes them. The internal listener next (its agent routes
	// ask the supervisor, still alive), then the supervisor itself. The rows
	// keep their states.
	if (supervisor) {
		std::printf("[shutdown] stopping the hosted servers' supervisor\n");
		supervisor->stop();
	}
	if (admin_links) admin_links->stop_all();
	hosted_driver.reset();
	if (internal_listener) internal_listener->stop();
	internal_listener.reset();
	supervisor.reset();
	// The ec2 driver's scratch directory (the CLI's output and user-data
	// files, each deleted after its call) goes with it.
	if (!hosted_fleet.scratch_dir.empty()) {
		std::error_code scratch_ec;
		std::filesystem::remove_all(hosted_fleet.scratch_dir, scratch_ec);
	}
#endif
#ifdef OPENNOVA_HTTP_ENABLED
	// The billing worker before the routes: a pass in flight ends its Stripe call.
	if (billing_stack.billing) billing_stack.billing->sync().stop();
#endif
	std::printf("[shutdown] stopping listeners\n");
#ifdef OPENNOVA_HTTP_ENABLED
	http.stop();
	// No request runs now: the Stripe calls' scratch directory goes.
	if (!billing_stack.scratch_dir.empty()) {
		std::error_code billing_ec;
		std::filesystem::remove_all(billing_stack.scratch_dir, billing_ec);
	}
	// Nothing posts mail now: the mail in flight finishes, what is still
	// queued is dropped (logged), and the SES calls' scratch directory goes.
	mail_stack.outbox->stop();
	if (!mail_stack.scratch_dir.empty()) {
		std::error_code mail_ec;
		std::filesystem::remove_all(mail_stack.scratch_dir, mail_ec);
	}
#endif
	gate.stop();
	nwudp.stop();
	manager.shutdown();
	std::printf("==== opennova-novaworld-server stopped ====\n");
	return exit_code;
}
