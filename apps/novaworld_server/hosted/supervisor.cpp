#include "supervisor.h"

#include "auth.h"
#include "hosted_settings.h"
#include "loggable.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

uint64_t steady_now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string seconds_text(uint64_t ms) { return std::to_string(ms / 1000) + " s"; }

} // namespace

Supervisor::Supervisor(db::ConnectionPool &pool, MachineProvider &provider, SupervisorConfig config) :
		pool_(pool), provider_(provider), config_(std::move(config)) {}

Supervisor::~Supervisor() { stop(); }

void Supervisor::start() {
	if (running_.exchange(true)) return;
	std::printf("[hosted] supervisor: provider %s, cap %d, servers list on %s:%u\n",
			provider_.name(), config_.cap, config_.master_host.c_str(),
			static_cast<unsigned>(config_.master_gate_port));
	try {
		reconcile(steady_now_ms());
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[hosted] WARN reconciliation failed: %s\n", e.what());
	}
	{
		std::lock_guard<std::mutex> lock(mu_);
		stop_requested_ = false;
	}
	thread_ = std::thread([this] { run(); });
}

void Supervisor::stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		stop_requested_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable()) thread_.join();
	running_.store(false);
}

void Supervisor::run() {
	for (;;) {
		try {
			tick(steady_now_ms());
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[hosted] WARN pass failed: %s\n", e.what());
		}
		std::unique_lock<std::mutex> lock(mu_);
		if (cv_.wait_for(lock, std::chrono::milliseconds(config_.tick_interval_ms),
					[this] { return stop_requested_; })) {
			return;
		}
	}
}

std::set<std::string> Supervisor::end_untracked(const std::vector<HostedServer> &live, const char *when) {
	std::map<int64_t, const HostedServer *> by_id;
	for (const HostedServer &s : live) by_id[s.id] = &s;
	std::set<std::string> running;
	for (const TaggedMachine &m : provider_.list_tagged()) {
		const auto it = by_id.find(m.server_id);
		if (it == by_id.end() || it->second->machine_id != m.machine_id) {
			std::printf("[hosted] %s: machine %s (server %lld) is no live server's; ending it\n", when,
					loggable(m.machine_id).c_str(), static_cast<long long>(m.server_id));
			provider_.terminate(m.machine_id);
			continue;
		}
		running.insert(m.machine_id);
	}
	return running;
}

void Supervisor::reconcile(uint64_t now_ms) {
	auto lease = pool_.acquire();
	db::Database &db = *lease;
	const std::vector<HostedServer> live = list_live(db);
	const std::set<std::string> running = end_untracked(live, "reconcile");
	last_orphan_sweep_ms_ = now_ms;

	for (const HostedServer &s : live) {
		if (s.machine_id.empty()) continue; // requested, or provisioning before a launch
		Runtime &rt = runtimes_[s.id];
		rt = Runtime{};
		rt.state = s.state;
		rt.since_ms = now_ms;
		rt.last_check_ms = now_ms;
		if (running.count(s.machine_id) != 0) {
			// Its lister re-hosts over the NWU reconnect; the health checks wait for it.
			rt.grace_until_ms = now_ms + config_.listing_timeout_ms;
			std::printf("[hosted] reconcile: server %lld resumes %s on machine %s\n",
					static_cast<long long>(s.id), state_name(s.state), loggable(s.machine_id).c_str());
			continue;
		}
		if (s.state == State::Stopping) {
			finish(db, s, State::Stopped, "its machine was gone at the service's start");
			continue;
		}
		if (s.state == State::Provisioning) {
			// Launched just before the service went: launched again, the same
			// state, no event.
			set_machine(db, s.id, s.provider, "", "");
			continue;
		}
		// A service restart is no crash: launched again, no restart counted.
		move(db, s, rt, now_ms, State::Provisioning, "its machine was gone at the service's start",
				TransitionOptions{ false, true });
	}
}

void Supervisor::tick(uint64_t now_ms) {
	auto lease = pool_.acquire();
	db::Database &db = *lease;
	std::vector<HostedServer> live = list_live(db);
	if (now_ms - last_orphan_sweep_ms_ >= config_.orphan_sweep_interval_ms) {
		// A machine no live row names (a launch whose row write failed, a row
		// ended while its machine lived on) is ended between the boots too.
		last_orphan_sweep_ms_ = now_ms;
		end_untracked(live, "sweep");
	}
	std::set<int64_t> seen;
	for (const HostedServer &s : live) {
		seen.insert(s.id);
		Runtime &rt = runtimes_[s.id];
		if (rt.since_ms == 0 || rt.state != s.state) {
			// A state this pass did not set (a route's stop, a new request, or a
			// row the supervisor first sees): its clocks start now.
			rt = Runtime{};
			rt.state = s.state;
			rt.since_ms = now_ms;
			rt.last_check_ms = now_ms;
		}
		try {
			step(db, s, rt, now_ms);
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[hosted] WARN server %lld: %s\n", static_cast<long long>(s.id), e.what());
		}
	}
	for (auto it = runtimes_.begin(); it != runtimes_.end();) {
		it = seen.count(it->first) == 0 ? runtimes_.erase(it) : std::next(it);
	}
	if (admin_links_ != nullptr) {
		try {
			sync_admin_links(db);
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[hosted] WARN admin links: %s\n", e.what());
		}
	}
}

void Supervisor::sync_admin_links(db::Database &db) {
	std::map<int64_t, AdminLinkTarget> wanted;
	for (const HostedServer &s : list_live(db)) {
		if (s.state != State::Running || s.machine_address.empty()) continue;
		const auto login = admin_login_of(db, s.id);
		if (!login || login->port <= 0 || login->port > 65535) continue;
		AdminLinkTarget target;
		target.host = s.machine_address;
		target.port = static_cast<uint16_t>(login->port);
		target.user = login->user;
		target.password = login->password;
		wanted.emplace(s.id, std::move(target));
	}
	admin_links_->sync(wanted);
}

bool Supervisor::owner_active(db::Database &db, const HostedServer &s, std::string &why) {
	if (!s.owner_user_id) {
		why = "its owner's account was deleted";
		return false;
	}
	const auto owner = get_user_by_id(db, *s.owner_user_id);
	if (!owner) {
		why = "its owner's account was deleted";
		return false;
	}
	if (owner->account_status != "active") {
		why = "its owner's account is " + loggable(owner->account_status, 32);
		return false;
	}
	// The approval is the brake (ADR 0053 d6): withdrawn, the server stops.
	if (!owner->can_host) {
		why = "its owner's approval to host was withdrawn";
		return false;
	}
	return true;
}

void Supervisor::step(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	if (s.state != State::Stopping) {
		std::string why;
		if (!owner_active(db, s, why)) {
			if (request_stop(db, s.id, why)) {
				std::printf("[hosted] server %lld: %s -> stopping (%s)\n", static_cast<long long>(s.id),
						state_name(s.state), why.c_str());
			}
			return;
		}
	}
	switch (s.state) {
		case State::Requested: on_requested(db, s, rt, now_ms); break;
		case State::Provisioning: on_provisioning(db, s, rt, now_ms); break;
		case State::Booting: on_booting(db, s, rt, now_ms); break;
		case State::Listing: on_listing(db, s, rt, now_ms); break;
		case State::Running: on_running(db, s, rt, now_ms); break;
		case State::Stopping: on_stopping(db, s, rt, now_ms); break;
		case State::Stopped:
		case State::Failed: break;
	}
}

bool Supervisor::move(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms, State to,
		const std::string &detail, TransitionOptions options) {
	if (!transition(db, s.id, s.state, to, detail, options)) return false;
	std::printf("[hosted] server %lld: %s -> %s (%s)\n", static_cast<long long>(s.id), state_name(s.state),
			state_name(to), detail.c_str());
	rt = Runtime{};
	rt.state = to;
	rt.since_ms = now_ms;
	rt.last_check_ms = now_ms;
	return true;
}

void Supervisor::on_requested(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	// step() checked the owner (active, approved) in this pass.
	move(db, s, rt, now_ms, State::Provisioning, "picked up");
}

void Supervisor::on_provisioning(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	if (!s.machine_id.empty()) {
		// Launched by an earlier pass whose move to booting lost to nothing (a
		// database failure between the two writes): carry on with that machine.
		move(db, s, rt, now_ms, State::Booting, "machine " + loggable(s.machine_id) + " launched");
		return;
	}
	if (now_ms < rt.not_before_ms) return; // a restart's backoff

	// The owner read again right before credentials are minted (a crash
	// restart's backoff gives an admin time to withdraw the approval).
	std::string why;
	if (!owner_active(db, s, why)) {
		if (request_stop(db, s.id, why)) {
			std::printf("[hosted] server %lld: provisioning -> stopping (%s)\n", static_cast<long long>(s.id),
					why.c_str());
		}
		return;
	}

	// The service account, with a new password at every start.
	const std::string username = service_username(s.id);
	const std::string nw_password = random_secret(kServicePasswordChars);
	const MutationResult account = upsert_service_account(db, username, nw_password);
	if (!account.ok) {
		if (account.error_code == "db_error") return; // logged by auth; the next pass tries again
		finish(db, s, State::Failed, "its service account could not be made (" + account.error_code + ")");
		return;
	}
	set_service_account(db, s.id, account.id);

	// The plan, a new admin login and the files.
	const MachinePlan plan = provider_.plan(s.id);
	const std::string admin_password = random_secret(kAdminPasswordChars);
	set_admin_login(db, s.id, plan.admin_port, kAdminUser, admin_password);
	RenderInputs inputs;
	inputs.server_id = s.id;
	inputs.plan = plan;
	inputs.service_username = username;
	inputs.service_password = nw_password;
	inputs.admin_password = admin_password;
	inputs.master_host = config_.master_host;
	inputs.master_gate_port = config_.master_gate_port;
	const LaunchSpec spec = render_launch(s.settings, inputs);

	std::string error;
	const std::optional<std::string> machine = provider_.launch(spec, error);
	if (!machine) {
		// The provider's own text (a path, a system error) goes to the log alone;
		// the event, which the owner reads, says what happened.
		std::fprintf(stderr, "[hosted] server %lld: the launch failed: %s\n", static_cast<long long>(s.id),
				error.c_str());
		attempt_failed(db, s, rt, now_ms, "the launch failed");
		return;
	}
	const MachineStatus status = provider_.describe(*machine);
	set_machine(db, s.id, provider_.name(), *machine, status.address);
	if (!move(db, s, rt, now_ms, State::Booting, "machine " + loggable(*machine) + " launched")) {
		// A stop landed meanwhile: the next pass ends the machine set above.
		std::printf("[hosted] server %lld: launched machine %s while it left provisioning\n",
				static_cast<long long>(s.id), loggable(*machine).c_str());
	}
}

void Supervisor::on_machine_down(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms,
		const MachineStatus &status) {
	// Whatever the code: a stop is the supervisor's own act (the row in
	// stopping), never read from an exit. opennova-serve exits 0 for its
	// rotation's end or the admin's GOTO MENUSTATE, 3 when the service ends its
	// hosting and 1 for a SIGTERM while it waits to be listed
	// (apps/serve/README.md "Exit codes"), and none of those is the owner's stop.
	attempt_failed(db, s, rt, now_ms,
			status.state == MachineState::Exited
					? "opennova-serve exited with code " + std::to_string(status.exit_code.value_or(-1))
					: std::string("its machine is gone"));
}

MachineStatus Supervisor::describe(db::Database &db, const HostedServer &s) {
	MachineStatus status = provider_.describe(s.machine_id);
	// A machine may have no address yet right after its launch (a cloud machine
	// is given one as it boots): the first describe that names one writes it, so
	// the admin link and the views have it from then on.
	if (s.machine_address.empty() && !status.address.empty() && status.state != MachineState::Gone) {
		set_machine(db, s.id, s.provider, s.machine_id, status.address);
		std::printf("[hosted] server %lld: machine %s at %s\n", static_cast<long long>(s.id),
				loggable(s.machine_id).c_str(), loggable(status.address).c_str());
	}
	return status;
}

void Supervisor::on_booting(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	const MachineStatus status = describe(db, s);
	switch (status.state) {
		case MachineState::Running:
			move(db, s, rt, now_ms, State::Listing, "opennova-serve runs");
			return;
		case MachineState::Pending:
			if (now_ms - rt.since_ms >= config_.boot_timeout_ms) {
				attempt_failed(db, s, rt, now_ms,
						"its machine did not start in " + seconds_text(config_.boot_timeout_ms));
			}
			return;
		case MachineState::Exited:
		case MachineState::Gone:
			on_machine_down(db, s, rt, now_ms, status);
			return;
	}
}

void Supervisor::on_listing(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	const MachineStatus status = describe(db, s);
	if (status.state == MachineState::Exited || status.state == MachineState::Gone) {
		on_machine_down(db, s, rt, now_ms, status);
		return;
	}
	const auto rid = s.service_user_id ? listing_rid_of(db, *s.service_user_id) : std::nullopt;
	if (rid) {
		move(db, s, rt, now_ms, State::Running, "listed as RID " + std::to_string(*rid));
		return;
	}
	if (now_ms - rt.since_ms >= config_.listing_timeout_ms) {
		attempt_failed(db, s, rt, now_ms, "no listing in " + seconds_text(config_.listing_timeout_ms));
	}
}

void Supervisor::on_running(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	const MachineStatus status = describe(db, s);
	if (status.state == MachineState::Exited || status.state == MachineState::Gone) {
		on_machine_down(db, s, rt, now_ms, status);
		return;
	}
	// The restart window: restart_reset_ms of running in one go and the
	// crashes before it are forgiven, so an always-on server that crashes now
	// and then is restarted every time.
	if (s.restarts > 0 && now_ms - rt.since_ms >= config_.restart_reset_ms && reset_restarts(db, s.id)) {
		std::printf("[hosted] server %lld: ran %s, its %d restart(s) forgiven\n", static_cast<long long>(s.id),
				seconds_text(config_.restart_reset_ms).c_str(), s.restarts);
	}
	if (now_ms < rt.grace_until_ms || now_ms - rt.last_check_ms < config_.health_interval_ms) return;
	rt.last_check_ms = now_ms;
	// Healthy: listed, and (with admin links) its admin link's session answering
	// its status polls (ADR 0053 d6).
	const bool listed = s.service_user_id && listing_rid_of(db, *s.service_user_id);
	const bool answers = admin_links_ == nullptr || admin_links_->answering(s.id);
	if (listed && answers) {
		rt.misses = 0;
		return;
	}
	++rt.misses;
	const std::string found = listed ? "no answer on its admin link" : "no listing";
	std::printf("[hosted] server %lld: health check %d of %d found %s\n", static_cast<long long>(s.id),
			rt.misses, config_.health_misses, found.c_str());
	if (rt.misses >= config_.health_misses) {
		finish(db, s, State::Failed,
				found + " at " + std::to_string(config_.health_misses) + " health checks in a row");
	}
}

void Supervisor::on_stopping(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	if (s.machine_id.empty()) {
		finish(db, s, State::Stopped, "stopped before its machine ran");
		return;
	}
	const MachineStatus status = provider_.describe(s.machine_id);
	if (status.state == MachineState::Exited || status.state == MachineState::Gone) {
		finish(db, s, State::Stopped,
				status.state == MachineState::Exited
						? "opennova-serve exited (code " + std::to_string(status.exit_code.value_or(-1)) + ")"
						: std::string("its machine is gone"));
		return;
	}
	const auto rid = s.service_user_id ? listing_rid_of(db, *s.service_user_id) : std::nullopt;
	if (!rt.stop_pushed) {
		// The service's word first: a stock host leaves NovaWorld on it, and
		// opennova-serve ends its session; the machine goes once the listing has.
		rt.stop_pushed = true;
		rt.stop_pushed_ms = now_ms;
		if (rid && stop_listing_) {
			const bool queued = stop_listing_(*rid, *s.service_user_id);
			std::printf("[hosted] server %lld: ServerStopHosting to RID %u %s\n",
					static_cast<long long>(s.id), *rid, queued ? "queued" : "not queued");
		}
		return;
	}
	if (!rid) {
		finish(db, s, State::Stopped, "its listing has ended");
	} else if (now_ms - rt.stop_pushed_ms >= config_.stop_timeout_ms) {
		finish(db, s, State::Stopped,
				"ended " + seconds_text(config_.stop_timeout_ms) + " after the stop, still listed");
	}
}

void Supervisor::attempt_failed(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms,
		const std::string &reason) {
	if (!s.machine_id.empty()) provider_.terminate(s.machine_id);
	if (s.restarts >= config_.restart_limit) {
		finish(db, s, State::Failed,
				reason + "; no restart left (" + std::to_string(config_.restart_limit) + " used)");
		return;
	}
	const std::string detail = reason + "; restart " + std::to_string(s.restarts + 1) + " of " +
			std::to_string(config_.restart_limit);
	if (move(db, s, rt, now_ms, State::Provisioning, detail, TransitionOptions{ true, true }))
		rt.not_before_ms = now_ms + config_.restart_backoff_ms;
}

void Supervisor::finish(db::Database &db, const HostedServer &s, State to, const std::string &detail) {
	if (!s.machine_id.empty()) provider_.terminate(s.machine_id);
	// The end and the service account's delete commit together: no ended row
	// keeps an account, so nothing still resolves its listing to the owner.
	TransitionOptions options;
	options.clear_machine = true;
	options.end_service_account = true;
	if (!transition(db, s.id, s.state, to, detail, options)) return;
	// Its runtime goes at the next pass, which no longer sees it live (the caller
	// may still hold it).
	std::printf("[hosted] server %lld: %s -> %s (%s)\n", static_cast<long long>(s.id), state_name(s.state),
			state_name(to), detail.c_str());
}

} // namespace opennova::novaworld_server::hosted
