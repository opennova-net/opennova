#include "supervisor.h"

#include "auth.h"
#include "fleet_jobs.h"
#include "hosted_settings.h"
#include "loggable.h"

#include <net/novaworld/lobby_repository.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <iterator>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

std::string seconds_text(uint64_t ms) { return std::to_string(ms / 1000) + " s"; }

// "1 hosted server", "3 hosted servers".
std::string servers_text(int n) { return std::to_string(n) + " hosted server" + (n == 1 ? "" : "s"); }

// Why a server past its owner's entitlement stops: the event its owner reads.
std::string limit_detail(const billing::Entitlement &e) {
	return std::string(e.source == "tier" ? "its owner's support tier allows " : "its owner's hosting entitlement allows ") +
			servers_text(e.max_servers);
}

// "machine 3, slot 1": where a server runs, as its events say it.
std::string place_text(int64_t machine_id, int slot) {
	return "machine " + std::to_string(machine_id) + ", slot " + std::to_string(slot);
}

// Every state a live machine can leave for its end.
const std::vector<FleetMachineState> &live_machine_states() {
	static const std::vector<FleetMachineState> states = { FleetMachineState::Launching,
		FleetMachineState::Booting, FleetMachineState::Ready, FleetMachineState::Draining,
		FleetMachineState::Ending, FleetMachineState::Lost };
	return states;
}

// `now_ms` - `since_ms` on the passes' clock, 0 for a time ahead of it (a
// row another supervisor wrote on a clock a little ahead).
uint64_t elapsed(uint64_t now_ms, int64_t since_ms) {
	return since_ms > 0 && now_ms > static_cast<uint64_t>(since_ms)
			? now_ms - static_cast<uint64_t>(since_ms)
			: 0;
}

} // namespace

uint64_t SupervisorConfig::wall_clock_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

Supervisor::Supervisor(db::ConnectionPool &pool, MachineProfile profile, SupervisorConfig config) :
		pool_(pool),
		profile_(std::move(profile)),
		config_(std::move(config)),
		sampler_(config_.slot_cpus) {}

Supervisor::~Supervisor() { stop(); }

void Supervisor::start() {
	if (running_.exchange(true)) return;
	std::printf("[hosted] supervisor: machines %s (shared machines of %d slots, at most %d shared "
				"and %d own), "
				"cap %d, servers list on %s:%u\n",
			profile_.provider.c_str(), config_.placement.shared_slots, config_.placement.max_shared,
			config_.placement.max_own, config_.cap, config_.master_host.c_str(),
			static_cast<unsigned>(config_.master_gate_port));
	try {
		reconcile(config_.clock_ms());
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
			tick(config_.clock_ms());
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

// --- the view of the fleet ------------------------------------------------------

Supervisor::Fleet Supervisor::read_fleet(db::Database &db) {
	Fleet fleet;
	for (FleetMachineRow &m : list_live_machines(db)) {
		for (SlotReport &r : slot_reports(db, m.id)) {
			const int slot = r.slot;
			fleet.reports.emplace(std::make_pair(m.id, slot), std::move(r));
		}
		const int64_t id = m.id;
		fleet.machines.emplace(id, std::move(m));
	}
	return fleet;
}

const FleetMachineRow *Supervisor::machine_of(const Fleet &fleet, const HostedServer &s) const {
	if (!s.machine_ref) return nullptr;
	const auto it = fleet.machines.find(*s.machine_ref);
	return it == fleet.machines.end() ? nullptr : &it->second;
}

const SlotReport *Supervisor::report_of(const Fleet &fleet, const HostedServer &s) const {
	if (!s.machine_ref || s.container.empty()) return nullptr;
	const auto it = fleet.reports.find(std::make_pair(*s.machine_ref, s.slot));
	if (it == fleet.reports.end() || it->second.container != s.container) return nullptr;
	return &it->second;
}

void Supervisor::forget_start(int64_t server_id) {
	std::lock_guard<std::mutex> lock(starts_mu_);
	starts_.erase(server_id);
}

// --- the boot and the passes ----------------------------------------------------------

void Supervisor::reconcile(uint64_t now_ms) {
	auto lease = pool_.acquire();
	db::Database &db = *lease;
	// A launch whose job is gone (it ended without a word on the row, or the
	// row predates the jobs): its cloud machine, if any, is no live row's now,
	// and the sweep ends it. One whose job waits or runs resumes.
	for (const FleetMachineRow &m : list_live_machines(db)) {
		if (m.state == FleetMachineState::Launching && !machine_launch_pending(db, m.id) &&
				transition_machine(db, m.id, FleetMachineState::Launching,
						FleetMachineState::Failed, "the service restarted during its launch"))
			std::printf("[fleet] reconcile: machine %lld failed: the service restarted during its launch\n",
					static_cast<long long>(m.id));
	}
	// The driver's machines against the rows, on a worker: a cloud machine no
	// live row names ends, and a row whose machine is gone ends at a pass.
	enqueue_fleet_sweep(db);

	const Fleet fleet = read_fleet(db);
	for (const HostedServer &s : list_live(db)) {
		Runtime &rt = runtimes_[s.id];
		rt = Runtime{};
		rt.state = s.state;
		rt.since_ms = now_ms;
		rt.last_check_ms = now_ms;
		if (!s.machine_ref) continue; // requested, or waiting for its place
		const FleetMachineRow *m = machine_of(fleet, s);
		if (!m) {
			// A service restart is no crash: placed again, no restart counted.
			replace(db, s, now_ms, "its machine was gone at the service's start");
			continue;
		}
		if (s.state == State::Provisioning) {
			// Placed just before the service went, its files never rendered: the
			// next pass places it again.
			release_slot(db, s.id);
			continue;
		}
		if (s.state == State::Booting && !report_of(fleet, s)) {
			// Its files went with the service before its agent took them.
			replace(db, s, now_ms, "its start was cut short by the service's restart");
			continue;
		}
		// Its lister re-hosts over the NWU reconnect; the health checks wait for it.
		rt.grace_until_ms = now_ms + config_.relist_grace_ms;
		std::printf("[hosted] reconcile: server %lld resumes %s on %s\n", static_cast<long long>(s.id),
				state_name(s.state), place_text(*s.machine_ref, s.slot).c_str());
	}
}

void Supervisor::tick(uint64_t now_ms) {
	auto lease = pool_.acquire();
	db::Database &db = *lease;
	// A controller that no longer leads runs no pass (it is on its way out).
	if (pass_guard_ && !pass_guard_(db))
		return;
	pass_reserved_.clear();
	// A lobby that takes clients, read as the gates read it: with none, no
	// server starts and an exit costs no restart (the class comment).
	const bool lobby_live = lobbydb::live_lobbies(db).instances > 0;
	if (lobby_live != lobby_live_) {
		if (lobby_live)
			std::printf("[hosted] a lobby instance is live: the servers waiting for one start\n");
		else
			std::fprintf(stderr,
					"[hosted] WARN no lobby instance is live: no hosted server starts, and "
					"an exit counts no restart, until one is\n");
		lobby_live_ = lobby_live;
	}
	// The servers this pass stopped for their owners' limits: stepped from the
	// next pass on, as stopping (their rows here hold the state before it).
	std::set<int64_t> limited;
	try {
		limited = enforce_limits(db, list_live(db));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[hosted] WARN the owners' limits: %s\n", e.what());
	}
	try {
		step_machines(db, now_ms);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[fleet] WARN the machines: %s\n", e.what());
	}

	// The servers, as the machines' moves left them.
	const std::vector<HostedServer> live = list_live(db);
	const Fleet fleet = read_fleet(db);
	std::set<int64_t> seen;
	for (const HostedServer &s : live) {
		seen.insert(s.id);
		if (limited.count(s.id) != 0) continue;
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
			step(db, fleet, s, rt, now_ms);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[hosted] WARN server %lld: %s\n", static_cast<long long>(s.id), e.what());
		}
		pass_turns_.fetch_add(1, std::memory_order_relaxed);
	}
	for (auto it = runtimes_.begin(); it != runtimes_.end();) {
		it = seen.count(it->first) == 0 ? runtimes_.erase(it) : std::next(it);
	}
	if (admin_links_ != nullptr) {
		try {
			sync_admin_links(db);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[hosted] WARN admin links: %s\n", e.what());
		}
	}
	try {
		sampler_.sample(db, config_.wall_clock_s(), [this](int64_t id) -> std::optional<StatsSampler::LinkView> {
			const auto link = admin_links_ != nullptr ? admin_links_->find(id) : nullptr;
			if (!link) return std::nullopt;
			const AdminLinkStatus status = link->status();
			return StatsSampler::LinkView{ status.game_state, status.state == AdminLinkState::Up };
		});
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[hosted] WARN the stats' sample: %s\n", e.what());
	}
	pass_turns_.fetch_add(1, std::memory_order_relaxed);
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
	// Their statuses for the web routes (hosted_admin_links).
	admin_links_->publish(db);
}

// --- the owners --------------------------------------------------------------------

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
	return true;
}

std::set<int64_t> Supervisor::enforce_limits(db::Database &db, const std::vector<HostedServer> &live) {
	// The clean reconciles with Stripe so far, read before the cache is: each
	// one counted landed in what this pass reads.
	const uint64_t reconciles = limits_hold::reconciles(db);
	// The owners' live servers not already stopping, oldest first (list_live
	// orders by id).
	std::map<int64_t, std::vector<const HostedServer *>> by_owner;
	for (const HostedServer &s : live) {
		if (s.owner_user_id && s.state != State::Stopping) by_owner[*s.owner_user_id].push_back(&s);
	}
	struct Stop {
		int64_t id;
		std::string detail;
	};
	std::vector<Stop> stops;
	size_t owners = 0;
	std::set<int64_t> owners_stopped;
	const int64_t wall = config_.wall_clock_s();
	for (const auto &[owner_id, servers] : by_owner) {
		// An account gone or no longer active is owner_active's, with its own words.
		const auto owner = get_user_by_id(db, owner_id);
		if (!owner || owner->account_status != "active") continue;
		++owners;
		const billing::Entitlement e = billing::entitlement_of(db, owner_id, wall);
		std::set<int64_t> chosen;
		// The newest past the entitlement's servers.
		const size_t max = static_cast<size_t>(std::max(e.max_servers, 0));
		for (size_t i = servers.size(); i > max; --i) {
			chosen.insert(servers[i - 1]->id);
			stops.push_back({ servers[i - 1]->id, limit_detail(e) });
		}
		// Of the rest, the newest on own machines past the add-on's.
		std::vector<const HostedServer *> own;
		for (const HostedServer *s : servers) {
			if (s->own_machine && chosen.count(s->id) == 0) own.push_back(s);
		}
		const size_t own_max = static_cast<size_t>(std::max(e.own_machines, 0));
		for (size_t i = own.size(); i > own_max; --i) {
			chosen.insert(own[i - 1]->id);
			stops.push_back({ own[i - 1]->id, "its owner's own-machine add-on covers " +
								std::to_string(e.own_machines) + " machine" + (e.own_machines == 1 ? "" : "s") });
		}
		if (!chosen.empty()) owners_stopped.insert(owner_id);
	}
	// An admin's override (limits_hold.h) is this pass's, whatever it finds.
	const bool forced = limits_hold::take_force(db);
	if (stops.empty()) {
		if (forced) std::printf("[hosted] the owners' limits: an admin's override found no server to stop\n");
		if ((forced && limits_hold::clear(db)) ||
				limits_hold::release_if_confirmed(db, {}, reconciles))
			std::printf("[hosted] the owners' limits hold released: no server is past its owner's entitlement\n");
		return {};
	}
	const std::string reason = "a pass would stop " + std::to_string(stops.size()) + " server(s) of " +
			std::to_string(owners_stopped.size()) + " of " + std::to_string(owners) + " owners";
	// The brake on the brake: a pass that would stop more owners' servers than
	// max(2, half the owners) reads as the cache gone wrong (a lost sync, a
	// mass expiry), not as that many owners lapsing at once. It holds every
	// stop until a clean reconcile with Stripe confirms the cache (then the
	// same owners' stops apply) or an admin's override applies them.
	const size_t threshold = std::max<size_t>(2, owners / 2);
	if (forced) {
		limits_hold::clear(db);
		std::printf("[hosted] the owners' limits applied past the brake by an admin's override: %s\n", reason.c_str());
	} else if (limits_hold::held(db) || owners_stopped.size() > threshold) {
		if (!limits_hold::release_if_confirmed(db, owners_stopped, reconciles)) {
			switch (limits_hold::hold(db, reason, owners_stopped, stops.size(), reconciles)) {
				case LimitsHeld::Now:
					std::fprintf(stderr, "[hosted] ERROR the owners' limits held: %s; nothing stopped until a "
										 "reconcile with Stripe confirms it or an admin applies the stops\n",
							reason.c_str());
					break;
				case LimitsHeld::Widened:
					std::fprintf(stderr, "[hosted] ERROR the owners' limits hold widened: %s; it waits for the next "
										 "reconcile with Stripe\n",
							reason.c_str());
					break;
				case LimitsHeld::Still:
					break;
			}
			return {};
		}
		std::printf("[hosted] the owners' limits hold released: a clean reconcile with Stripe confirmed the cache, "
					"and the same owners are past their entitlements: %s\n",
				reason.c_str());
	}
	std::set<int64_t> stopped;
	for (const Stop &stop : stops) {
		if (!request_stop(db, stop.id, stop.detail)) continue;
		stopped.insert(stop.id);
		std::printf("[hosted] server %lld: -> stopping (%s)\n", static_cast<long long>(stop.id), stop.detail.c_str());
	}
	return stopped;
}

// --- the servers -------------------------------------------------------------------

void Supervisor::step(db::Database &db, const Fleet &fleet, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
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
		case State::Booting: on_booting(db, fleet, s, rt, now_ms); break;
		case State::Listing: on_listing(db, fleet, s, rt, now_ms); break;
		case State::Running: on_running(db, fleet, s, rt, now_ms); break;
		case State::Stopping: on_stopping(db, fleet, s, rt, now_ms); break;
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
	// step() checked the owner (active) in this pass.
	move(db, s, rt, now_ms, State::Provisioning, "picked up");
}

void Supervisor::on_provisioning(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	if (s.machine_ref) {
		// Placed by an earlier pass whose start never rendered (the service went
		// in between): the slot goes, and the next pass places it again.
		release_slot(db, s.id);
		return;
	}
	if (now_ms < rt.not_before_ms) return; // a restart's backoff
	// No lobby live: the gate would hand its opennova-serve none. It waits here
	// unplaced, no machine launched for it, and starts once one is (its owner
	// and its place read then, below).
	if (!lobby_live_) {
		if (!rt.lobby_wait_logged)
			std::printf(
					"[hosted] server %lld waits for a live lobby\n", static_cast<long long>(s.id));
		rt.lobby_wait_logged = true;
		return;
	}

	// The owner read again right before credentials are minted (a crash
	// restart's backoff gives an admin time to withdraw a grant).
	std::string why;
	if (!owner_active(db, s, why)) {
		if (request_stop(db, s.id, why)) {
			std::printf("[hosted] server %lld: provisioning -> stopping (%s)\n", static_cast<long long>(s.id),
					why.c_str());
		}
		return;
	}
	// Its place under the owner's entitlement, read again (a tier lowered, a
	// grant cut since the request): the owner's live servers not stopping keep
	// their places oldest first, and one past the entitlement's servers is
	// stopped rather than started (not while the limits are held: the cache is
	// then in doubt).
	const billing::Entitlement entitlement =
			billing::entitlement_of(db, *s.owner_user_id, config_.wall_clock_s());
	int rank = 0;
	for (const HostedServer &other : live_servers_of(db, *s.owner_user_id)) {
		if (other.id == s.id) break;
		if (other.state != State::Stopping) ++rank;
	}
	if (rank >= entitlement.max_servers && !limits_hold::held(db)) {
		const std::string limit = limit_detail(entitlement);
		if (request_stop(db, s.id, limit)) {
			std::printf("[hosted] server %lld: provisioning -> stopping (%s)\n", static_cast<long long>(s.id),
					limit.c_str());
		}
		return;
	}

	// Its data set rows and their data (no restart mends what is missing): the
	// trees it is pinned to, when it was started before and they are not pruned,
	// else the data sets' current trees (its first start, or a pin pruned since).
	if (!data_of(db, s, why)) {
		finish(db, s, State::Failed, why);
		return;
	}
	if (!pins_hold(db, s) && !current_trees_of(db, s.settings.data_set_id, s.settings.expansion_id)) {
		const bool base_has_data = current_trees_of(db, s.settings.data_set_id, std::nullopt).has_value();
		finish(db, s, State::Failed, base_has_data ? "its expansion has no data" : "its data set has no data");
		return;
	}
	place(db, s, rt, now_ms);
}

std::optional<Supervisor::ServerData> Supervisor::data_of(db::Database &db, const HostedServer &s, std::string &why) {
	ServerData out;
	const auto base = s.settings.data_set_id != 0 ? find_data_set(db, s.settings.data_set_id) : std::nullopt;
	if (!base || base->kind != DataSetKind::Base) {
		why = "its data set is gone";
		return std::nullopt;
	}
	out.title = base->title;
	if (s.settings.expansion_id) {
		const auto expansion = find_data_set(db, *s.settings.expansion_id);
		if (!expansion || expansion->kind != DataSetKind::Expansion || expansion->parent_id != base->id) {
			why = "its expansion is gone";
			return std::nullopt;
		}
		out.expansion_code = expansion->expansion_code;
	}
	return out;
}

bool Supervisor::pins_hold(db::Database &db, const HostedServer &s) {
	if (!s.base_tree_id) return false;
	const auto base = find_tree(db, *s.base_tree_id);
	if (!base || base->state == TreeState::Pruned) return false;
	if (s.settings.expansion_id) {
		if (!s.expansion_tree_id) return false;
		const auto expansion = find_tree(db, *s.expansion_tree_id);
		if (!expansion || expansion->state == TreeState::Pruned) return false;
	}
	return true;
}

std::optional<TreeSpec> Supervisor::tree_spec(db::Database &db, int64_t id) {
	{
		std::lock_guard<std::mutex> lock(trees_mu_);
		const auto it = tree_specs_.find(id);
		if (it != tree_specs_.end()) return it->second;
	}
	const auto tree = find_tree(db, id);
	if (!tree || tree->files.empty()) return std::nullopt;
	TreeSpec spec;
	spec.url = config_.tree_url ? config_.tree_url(id) : std::string();
	// The version as the reply carries it: one short printable line.
	for (const char c : tree->version_text) {
		if (spec.version.size() >= 128) break;
		spec.version.push_back(c >= ' ' && c <= '~' ? c : ' ');
	}
	for (const TreeFile &f : tree->files) spec.files.push_back({ f.name, f.size });
	// A ready tree is immutable: read once, kept.
	if (tree->state == TreeState::Ready) {
		std::lock_guard<std::mutex> lock(trees_mu_);
		tree_specs_[id] = spec;
	}
	return spec;
}

void Supervisor::place(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms) {
	PlacementRequest request;
	request.server_id = s.id;
	request.own_machine = s.own_machine;
	// The machines, with the slots this pass's earlier servers will take on the
	// ones still launching held.
	const auto machines = [&] {
		std::vector<FleetMachine> out = placement_machines(db);
		for (const auto &[machine, slot] : pass_reserved_) {
			for (FleetMachine &m : out) {
				if (m.id == machine)
					m.used_slots.push_back(slot);
			}
		}
		return out;
	};
	Placement p = place_server(request, machines(), config_.placement);
	if (p.outcome == Placement::Outcome::Launch) {
		// The machine launched for it takes it first: placed again at once, it
		// lands there (a launching machine takes servers).
		if (!launch_machine(db, s, p.launch_kind, rt, now_ms)) return;
		p = place_server(request, machines(), config_.placement);
	}
	switch (p.outcome) {
		case Placement::Outcome::Full:
			if (!rt.waiting_logged) {
				std::printf("[hosted] server %lld waits for a slot: the fleet's %s machines are full\n",
						static_cast<long long>(s.id), s.own_machine ? "own" : "shared");
				rt.waiting_logged = true;
			}
			return;
		case Placement::Outcome::Launch:
			return; // the launch's backoff, or a launch the next pass sees
		case Placement::Outcome::Placed:
			break;
	}
	const auto machine = find_machine(db, p.machine_id);
	// A machine the cloud has not answered for yet (no address its agent comes
	// from, none its admin link reaches): the next pass, its slot held for it
	// through this one.
	if (!machine || machine->private_address.empty()) {
		pass_reserved_.emplace_back(p.machine_id, p.slot);
		return;
	}
	const SlotPlan plan = profile_.slot_plan(p.slot);
	if (!assign_slot(db, s.id, machine->id, p.slot, plan.game_port)) return; // raced: the next pass
	const auto placed = find_server(db, s.id);
	if (!placed || !placed->machine_ref || placed->state != State::Provisioning) {
		// A stop landed meanwhile: it holds no slot it never ran on.
		release_slot(db, s.id);
		return;
	}
	start_on_slot(db, *placed, *machine, plan, rt, now_ms);
}

bool Supervisor::launch_machine(db::Database &db, const HostedServer &s, MachineKind kind, Runtime &rt,
		uint64_t now_ms) {
	// A refusal its job's worker wrote while this pass ran (after the pass
	// counted the refusals, before this placement) counted first, so its
	// backoff holds this launch too, never one more launch inside it.
	note_refused_launches(db, now_ms);
	// A refused launch's backoff.
	if (static_cast<int64_t>(now_ms) < launch_not_before_ms(db, kind))
		return false;
	NewMachine wanted;
	wanted.provider = profile_.provider;
	wanted.kind = kind;
	wanted.slots = kind == MachineKind::Own ? 1 : config_.placement.shared_slots;
	wanted.serve_image = config_.serve_image;
	wanted.reserved_for = kind == MachineKind::Own ? s.id : 0;
	wanted.launched_for = s.id;
	// The row and its launch job, together: no row without its launch, no
	// launch without its row. A worker runs the launch (fleet_jobs.h); the
	// machine takes servers as it launches, and the next pass places them once
	// the cloud has answered with its address.
	CreatedMachine created;
	{
		db::Transaction tx(db);
		// The leadership fenced inside the write: a controller another has
		// replaced since its pass began launches nothing (a new leader's election
		// waits for this transaction, then reads the row it wrote).
		if (pass_guard_ && !pass_guard_(db)) {
			std::fprintf(stderr,
					"[fleet] no machine launched for server %lld: this controller no longer "
					"leads\n",
					static_cast<long long>(s.id));
			return false;
		}
		created = create_machine(db, wanted);
		FleetMachineRow seen;
		seen.id = created.id;
		seen.seen_state = fleet_machine_state_text(FleetMachineState::Launching);
		seen.seen_since_ms = static_cast<int64_t>(now_ms);
		save_machine_runtime(db, seen);
		enqueue_machine_launch(db, created.id, kind, wanted.slots, created.bootstrap_token);
		tx.commit();
	}
	std::printf("[fleet] machine %lld (%s, %d slot%s) launching for server %lld\n",
			static_cast<long long>(created.id), machine_kind_text(kind), wanted.slots,
			wanted.slots == 1 ? "" : "s", static_cast<long long>(s.id));
	rt.waiting_logged = true;
	return true;
}

void Supervisor::start_on_slot(db::Database &db, const HostedServer &s, const FleetMachineRow &machine,
		const SlotPlan &plan, Runtime &rt, uint64_t now_ms) {
	std::string why;
	const auto data = data_of(db, s, why);
	if (!data) {
		release_slot(db, s.id); // checked in this pass; the next one fails it
		return;
	}
	// The trees it runs on: the pins its last start took (a crash restart or a
	// move keeps them), else the data sets' current trees (its first start, the
	// owner's update, a pinned tree pruned since). Written with the machine,
	// in one transaction, below.
	int64_t base_tree = s.base_tree_id.value_or(0);
	std::optional<int64_t> expansion_tree = s.expansion_tree_id;
	const bool kept = pins_hold(db, s) && !s.update_requested;
	if (!kept) {
		const auto current = current_trees_of(db, s.settings.data_set_id, s.settings.expansion_id);
		if (current) {
			base_tree = current->base_tree_id;
			expansion_tree = current->expansion_tree_id;
		} else if (base_tree == 0 || !pins_hold(db, s)) {
			release_slot(db, s.id); // no data: the next pass fails it
			return;
		}
	}
	// The service account, with a new password at every start.
	const std::string username = service_username(s.id);
	const std::string nw_password = random_secret(kServicePasswordChars);
	const MutationResult account = upsert_service_account(db, username, nw_password);
	if (!account.ok) {
		release_slot(db, s.id);
		if (account.error_code == "db_error") return; // logged by auth; the next pass tries again
		finish(db, s, State::Failed, "its service account could not be made (" + account.error_code + ")");
		return;
	}
	set_service_account(db, s.id, account.id);

	// A new admin login and the files, for the slot's ports.
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
	const LaunchSpec spec = render_launch(s.settings, data->title, data->expansion_code, inputs);

	Start start;
	start.machine_id = machine.id;
	DesiredContainer &c = start.container;
	c.name = hosted_container_name(s.id, s.generation);
	c.slot = s.slot;
	c.image = config_.serve_image;
	c.uid = slot_uid(s.slot);
	c.base_tree = base_tree;
	c.expansion_tree = expansion_tree;
	c.expansion_code = data->expansion_code;
	c.args = spec.serve_args;
	std::map<std::string, std::string> files;
	for (const LaunchFile &f : spec.files) files[f.name] = f.content;
	c.files = std::move(files);
	c.cpus = config_.slot_cpus;
	c.memory_mb = config_.slot_memory_mb;
	c.pids = config_.slot_pids;
	set_serve_image(db, s.id, c.image);
	const std::string container = c.name;
	{
		// Before the move: a sync that sees the server booting finds its files.
		std::lock_guard<std::mutex> lock(starts_mu_);
		starts_[s.id] = std::move(start);
	}
	{
		// The pins and the machine together: a server on a machine runs the
		// trees its row names (update_requested cleared with them).
		db::Transaction tx(db);
		set_tree_pins(db, s.id, base_tree, expansion_tree);
		set_machine(db, s.id, profile_.provider, container, machine.private_address);
		tx.commit();
	}
	std::string placed_text = "placed on " + place_text(machine.id, s.slot) + " with tree " + std::to_string(base_tree);
	if (expansion_tree) placed_text += " and expansion tree " + std::to_string(*expansion_tree);
	if (!move(db, s, rt, now_ms, State::Booting, placed_text)) {
		// A stop landed meanwhile: its container never reported, so the stop ends
		// at the next pass with nothing to stop.
		std::printf("[hosted] server %lld: placed on %s while it left provisioning\n", static_cast<long long>(s.id),
				place_text(machine.id, s.slot).c_str());
	}
}

void Supervisor::on_exit(db::Database &db, const HostedServer &s, Runtime &rt, uint64_t now_ms,
		const SlotReport &report) {
	// Whatever the code: a stop is the supervisor's own act (the row in
	// stopping), never read from an exit. opennova-serve exits 0 for its
	// rotation's end or the admin's GOTO MENUSTATE, 3 when the service ends its
	// hosting and 1 for a SIGTERM while it waits to be listed
	// (apps/serve/README.md "Exit codes"), and none of those is the owner's stop.
	// A container that never started may say why (its data could not be
	// fetched): the agent's reason rides its report.
	std::string reason;
	if (report.oom)
		reason = "opennova-serve ran out of memory";
	else if (report.exit_code == 125 && !report.started_at)
		reason = report.reason.empty() ? "its container did not start" : report.reason;
	else
		reason = "opennova-serve exited with code " +
				(report.exit_code ? std::to_string(*report.exit_code) : std::string("unknown"));
	if (!lobby_live_) {
		// NovaWorld's cause, not the server's: with no lobby live opennova-serve
		// gives up on the gate (retail's 30 s probe). Placed again, no restart
		// counted; it waits for a lobby in provisioning.
		replace(db, s, now_ms, reason + " while no lobby instance was live");
		return;
	}
	attempt_failed(db, s, rt, now_ms, reason);
}

void Supervisor::on_booting(db::Database &db, const Fleet &fleet, const HostedServer &s, Runtime &rt,
		uint64_t now_ms) {
	const FleetMachineRow *machine = machine_of(fleet, s);
	if (!machine) {
		replace(db, s, now_ms, "its machine is gone");
		return;
	}
	if (s.update_requested) {
		replace(db, s, now_ms, "updated to its data set's current data");
		return;
	}
	if (machine->state != FleetMachineState::Ready) {
		// Its machine boots (or is lost, or drains): the container's clock waits.
		rt.since_ms = now_ms;
		return;
	}
	if (const SlotReport *report = report_of(fleet, s)) {
		if (report->state == ContainerState::Running) {
			move(db, s, rt, now_ms, State::Listing, "opennova-serve runs");
			return;
		}
		if (report->state == ContainerState::Exited) {
			on_exit(db, s, rt, now_ms, *report);
			return;
		}
	}
	if (now_ms - rt.since_ms >= config_.boot_timeout_ms)
		attempt_failed(db, s, rt, now_ms, "its container did not start in " + seconds_text(config_.boot_timeout_ms));
}

void Supervisor::on_listing(db::Database &db, const Fleet &fleet, const HostedServer &s, Runtime &rt,
		uint64_t now_ms) {
	const FleetMachineRow *machine = machine_of(fleet, s);
	if (!machine) {
		replace(db, s, now_ms, "its machine is gone");
		return;
	}
	if (s.update_requested) {
		replace(db, s, now_ms, "updated to its data set's current data");
		return;
	}
	const SlotReport *report = report_of(fleet, s);
	if (report && report->state == ContainerState::Exited) {
		on_exit(db, s, rt, now_ms, *report);
		return;
	}
	const auto rid = s.service_user_id ? listing_rid_of(db, *s.service_user_id) : std::nullopt;
	if (rid) {
		move(db, s, rt, now_ms, State::Running, "listed as RID " + std::to_string(*rid));
		return;
	}
	// While its machine is lost the listing's clock waits: the machine's end,
	// not a missed listing, decides then (as for a running server's checks). So
	// it does while no lobby is live, which nothing lists on.
	if (machine->state == FleetMachineState::Lost || !lobby_live_) {
		rt.since_ms = now_ms;
		return;
	}
	if (now_ms - rt.since_ms >= config_.listing_timeout_ms) {
		attempt_failed(db, s, rt, now_ms, "no listing in " + seconds_text(config_.listing_timeout_ms));
	}
}

void Supervisor::on_running(db::Database &db, const Fleet &fleet, const HostedServer &s, Runtime &rt,
		uint64_t now_ms) {
	const FleetMachineRow *machine = machine_of(fleet, s);
	if (!machine) {
		replace(db, s, now_ms, "its machine is gone");
		return;
	}
	// The owner's update: its slot let go and placed again at once (the next
	// start takes the current trees), the machine's kind of move, no restart.
	if (s.update_requested) {
		replace(db, s, now_ms, "updated to its data set's current data");
		return;
	}
	const SlotReport *report = report_of(fleet, s);
	if (report && report->state == ContainerState::Exited) {
		on_exit(db, s, rt, now_ms, *report);
		return;
	}
	// The restart window: restart_reset_ms of running in one go and the
	// crashes before it are forgiven, so an always-on server that crashes now
	// and then is restarted every time.
	if (s.restarts > 0 && now_ms - rt.since_ms >= config_.restart_reset_ms && reset_restarts(db, s.id)) {
		std::printf("[hosted] server %lld: ran %s, its %d restart(s) forgiven\n", static_cast<long long>(s.id),
				seconds_text(config_.restart_reset_ms).c_str(), s.restarts);
	}
	// While its machine is lost the checks wait, and so does the listing's
	// clock: the machine's end, not a missed listing, decides then.
	if (machine->state == FleetMachineState::Lost) {
		rt.listing_seen_ms = now_ms;
		return;
	}
	if (now_ms < rt.grace_until_ms || now_ms - rt.last_check_ms < config_.health_interval_ms) return;
	rt.last_check_ms = now_ms;
	// Its listing: gone for listing_lost_ms, the lobby it listed on went away,
	// and its NovaWorld session reconnects only to that lobby's address (the
	// config's derivation), so a new start lists it again, no restart counted.
	// While no lobby is live the clock waits: its session reconnects to that
	// address until a lobby is back there, and a new start could not list.
	if (!lobby_live_ || (s.service_user_id && listing_rid_of(db, *s.service_user_id))) {
		rt.listing_seen_ms = now_ms;
	} else {
		const uint64_t gone_ms = now_ms - std::max(rt.listing_seen_ms, rt.since_ms);
		if (gone_ms >= config_.listing_lost_ms) {
			replace(db, s, now_ms,
					"no listing for " + seconds_text(gone_ms) +
							" (its NovaWorld session reconnects only to the lobby it listed on)");
			return;
		}
		std::printf("[hosted] server %lld: no listing for %s (placed again at %s)\n",
				static_cast<long long>(s.id), seconds_text(gone_ms).c_str(),
				seconds_text(config_.listing_lost_ms).c_str());
	}
	// Its admin link's session answering its status polls (ADR 0053 d6), with
	// admin links: health_misses checks in a row with no answer fail it.
	if (admin_links_ == nullptr || admin_links_->answering(s.id)) {
		rt.misses = 0;
		return;
	}
	++rt.misses;
	std::printf("[hosted] server %lld: health check %d of %d found no answer on its admin link\n",
			static_cast<long long>(s.id), rt.misses, config_.health_misses);
	if (rt.misses >= config_.health_misses) {
		finish(db, s, State::Failed,
				"no answer on its admin link at " + std::to_string(config_.health_misses) +
						" health checks in a row");
	}
}

void Supervisor::on_stopping(db::Database &db, const Fleet &fleet, const HostedServer &s, Runtime &rt,
		uint64_t now_ms) {
	bool acknowledged = false;
	{
		std::lock_guard<std::mutex> lock(starts_mu_);
		const auto it = starts_.find(s.id);
		acknowledged = it != starts_.end() && it->second.acknowledged;
	}
	const SlotReport *report = report_of(fleet, s);
	if (!s.machine_ref || (!acknowledged && !report)) {
		finish(db, s, State::Stopped, "stopped before it ran");
		return;
	}
	if (!machine_of(fleet, s)) {
		finish(db, s, State::Stopped, "its machine is gone");
		return;
	}
	if (report && report->state == ContainerState::Exited) {
		finish(db, s, State::Stopped,
				"opennova-serve exited (code " +
						(report->exit_code ? std::to_string(*report->exit_code) : std::string("unknown")) + ")");
		return;
	}
	const auto rid = s.service_user_id ? listing_rid_of(db, *s.service_user_id) : std::nullopt;
	if (!rt.stop_pushed) {
		// The service's word first: a stock host leaves NovaWorld on it, and
		// opennova-serve ends its session; the container goes once the listing has.
		rt.stop_pushed = true;
		rt.stop_pushed_ms = now_ms;
		if (rid && stop_listing_) {
			const bool queued = stop_listing_(db, *rid, *s.service_user_id);
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
	forget_start(s.id);
	if (s.machine_ref) release_slot(db, s.id);
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

void Supervisor::replace(db::Database &db, const HostedServer &s, uint64_t now_ms, const std::string &reason) {
	forget_start(s.id);
	if (s.machine_ref) release_slot(db, s.id);
	switch (s.state) {
		case State::Stopping:
			finish(db, s, State::Stopped, reason);
			return;
		case State::Requested:
		case State::Provisioning:
		case State::Stopped:
		case State::Failed:
			return;
		case State::Booting:
		case State::Listing:
		case State::Running:
			move(db, s, runtimes_[s.id], now_ms, State::Provisioning, reason + "; placed again (no restart counted)",
					TransitionOptions{ false, true });
			return;
	}
}

void Supervisor::finish(db::Database &db, const HostedServer &s, State to, const std::string &detail) {
	forget_start(s.id);
	// The end and the service account's delete commit together: no ended row
	// keeps an account, so nothing still resolves its listing to the owner. The
	// row keeps the machine and slot it last held (no longer wanted there: its
	// agent stops the container).
	TransitionOptions options;
	options.clear_machine = true;
	options.end_service_account = true;
	if (!transition(db, s.id, s.state, to, detail, options)) return;
	// Its runtime goes at the next pass, which no longer sees it live (the caller
	// may still hold it).
	std::printf("[hosted] server %lld: %s -> %s (%s)\n", static_cast<long long>(s.id), state_name(s.state),
			state_name(to), detail.c_str());
}

// --- the machines -------------------------------------------------------------------

void Supervisor::launch_failed(db::Database &db, MachineKind kind, uint64_t now_ms) {
	launch_failures_.fetch_add(1, std::memory_order_relaxed);
	const auto backoff = note_launch_failure(db, kind, static_cast<int64_t>(now_ms),
			config_.launch_backoff_ms, config_.launch_backoff_max_ms);
	if (!backoff)
		return; // a launch made before the backoff that holds
	std::fprintf(stderr,
			"[fleet] WARN a launch of a %s machine failed (%lld in a row): no %s launch for %s\n",
			machine_kind_text(kind), static_cast<long long>(backoff->failures),
			machine_kind_text(kind),
			seconds_text(
					static_cast<uint64_t>(backoff->not_before_ms - static_cast<int64_t>(now_ms)))
					.c_str());
}

void Supervisor::note_refused_launches(db::Database &db, uint64_t now_ms) {
	for (const RefusedLaunch &refused : take_refused_launches(db)) {
		launch_failed(db, refused.kind, now_ms);
		if (refused.launched_for == 0)
			continue;
		const auto s = find_server(db, refused.launched_for);
		if (!s || s->state != State::Provisioning)
			continue;
		// The fleet's cause, not the server's: no restart counted. It waits for
		// the next launch the backoff lets through, its event saying so once.
		Runtime &rt = runtimes_[s->id];
		if (rt.since_ms == 0 || rt.state != s->state) {
			rt = Runtime{};
			rt.state = s->state;
			rt.since_ms = now_ms;
			rt.last_check_ms = now_ms;
		}
		if (!rt.launch_failure_noted &&
				transition(db, s->id, State::Provisioning, State::Provisioning,
						"the launch of its machine failed; it waits for the next (no restart "
						"counted)")) {
			std::printf("[hosted] server %lld: the launch of its machine failed; it waits for the "
						"next\n",
					static_cast<long long>(s->id));
		}
		rt.launch_failure_noted = true;
	}
}

void Supervisor::ask_describe(db::Database &db, const FleetMachineRow &m, uint64_t now_ms) {
	if (m.cloud_id.empty())
		return;
	if (m.describe_asked_ms > 0 &&
			elapsed(now_ms, m.describe_asked_ms) < config_.describe_interval_ms)
		return;
	if (enqueue_machine_describe(db, m.id, m.cloud_id) != 0)
		note_describe_asked(db, m.id, static_cast<int64_t>(now_ms));
}

void Supervisor::end_machine(db::Database &db, const FleetMachineRow &m, FleetMachineState to,
		const std::string &reason, uint64_t now_ms) {
	{
		// The end and its cloud machine's terminate, together, fenced as a
		// launch is.
		db::Transaction tx(db);
		if (pass_guard_ && !pass_guard_(db))
			return;
		if (!transition_machine(db, m.id, live_machine_states(), to, reason))
			return;
		if (!m.cloud_id.empty())
			enqueue_machine_terminate(db, m.cloud_id);
		tx.commit();
	}
	std::printf("[fleet] machine %lld: %s -> %s (%s)\n", static_cast<long long>(m.id),
			fleet_machine_state_text(m.state), fleet_machine_state_text(to), reason.c_str());
	// Whatever ended it, the machine's cause, never its servers': each placed
	// again with no restart counted (a stopping one's stop ends).
	for (const HostedServer &s : list_live(db)) {
		if (s.machine_ref == m.id) replace(db, s, now_ms, reason);
	}
}

void Supervisor::drain_step(db::Database &db, FleetMachineRow &m,
		const std::vector<HostedServer> &holders, uint64_t now_ms) {
	if (holders.empty()) {
		end_machine(db, m, FleetMachineState::Ended, m.end_reason.empty() ? "drained" : m.end_reason + "; drained",
				now_ms);
		return;
	}
	// One at a time: the server moved last runs again elsewhere (or ended)
	// before the next one moves.
	if (m.drain_moving != 0) {
		const auto moved = find_server(db, m.drain_moving);
		if (moved && is_live(moved->state) && moved->state != State::Running && moved->state != State::Stopping)
			return;
		m.drain_moving = 0;
	}
	for (const HostedServer &s : holders) {
		if (s.state == State::Stopping) continue; // its stop ends it here
		std::printf("[fleet] machine %lld drains: server %lld moves off it\n", static_cast<long long>(m.id),
				static_cast<long long>(s.id));
		replace(db, s, now_ms, "its machine drains");
		m.drain_moving = s.id;
		return;
	}
}

void Supervisor::step_machines(db::Database &db, uint64_t now_ms) {
	// The launches the cloud refused since the last pass, counted first: the
	// servers they were for wait out the backoff below.
	note_refused_launches(db, now_ms);
	// A machine whose agent fell silent: lost (its describes from now on decide).
	for (const FleetMachineRow &m : list_silent_machines(db, config_.machine_silence_s)) {
		const std::string why = "its agent has not reported for " + std::to_string(config_.machine_silence_s) + " s";
		if (!mark_machine_lost(db, m.id, why, static_cast<int64_t>(now_ms)))
			continue;
		std::fprintf(stderr, "[fleet] WARN machine %lld: %s -> lost (%s)\n",
				static_cast<long long>(m.id), fleet_machine_state_text(m.state), why.c_str());
	}

	std::map<int64_t, std::vector<HostedServer>> holders;
	std::set<int64_t> live_ids;
	for (HostedServer &s : list_live(db)) {
		live_ids.insert(s.id);
		if (s.machine_ref) holders[*s.machine_ref].push_back(std::move(s));
	}
	const uint64_t boot_timeout = profile_.boot_timeout_ms;
	for (FleetMachineRow m : list_live_machines(db)) {
		// What the passes remember of it, from its row: since when it has been in
		// its state (another supervisor's passes included).
		const FleetMachineRow remembered = m;
		const std::string state = fleet_machine_state_text(m.state);
		if (m.seen_since_ms == 0 || m.seen_state != state) {
			m.was_draining = m.was_draining && m.state == FleetMachineState::Lost;
			m.seen_state = state;
			m.seen_since_ms = static_cast<int64_t>(now_ms);
			m.idle_since_ms = 0;
			m.drain_moving = 0;
		}
		const uint64_t in_state = elapsed(now_ms, m.seen_since_ms);
		const std::vector<HostedServer> &on = holders[m.id];
		const std::optional<CloudState> cloud = parse_cloud_state(m.cloud_state);
		bool ended = false;
		const auto end = [&](FleetMachineState to, const std::string &reason) {
			end_machine(db, m, to, reason, now_ms);
			ended = true;
		};
		if (m.state != FleetMachineState::Launching && cloud == CloudState::Gone) {
			// A describe (or the sweep's) found its cloud machine gone.
			if (m.state == FleetMachineState::Booting) {
				launch_failed(db, m.kind, now_ms);
				end(FleetMachineState::Failed, "its machine is gone before it booted");
			} else if (m.state == FleetMachineState::Ending) {
				end(FleetMachineState::Ended, m.end_reason.empty() ? "ended" : m.end_reason);
			} else {
				end(FleetMachineState::Ended, "its machine is gone");
			}
		}
		switch (ended ? FleetMachineState::Ended : m.state) {
			case FleetMachineState::Launching:
				// Its launch job has not finished: one that never does in the boot
				// timeout is a launch that failed (the backoff, the heartbeat's
				// count), never its servers'.
				if (in_state >= boot_timeout) {
					launch_failed(db, m.kind, now_ms);
					end(FleetMachineState::Failed, "its launch never finished");
				}
				break;
			case FleetMachineState::Booting:
				// Its describes fill in its addresses and say when it is gone.
				ask_describe(db, m, now_ms);
				if (in_state >= boot_timeout) {
					launch_failed(db, m.kind, now_ms);
					end(FleetMachineState::Failed,
							"its machine did not boot in " + seconds_text(boot_timeout));
				}
				break;
			case FleetMachineState::Ready:
				if (m.public_address.empty())
					ask_describe(db, m, now_ms);
				if (m.kind == MachineKind::Own) {
					// Launched for one server: it ends with that server.
					if (m.reserved_for == 0 || live_ids.count(m.reserved_for) == 0)
						end(FleetMachineState::Ended, "its server ended");
					break;
				}
				if (!on.empty()) {
					m.idle_since_ms = 0;
				} else if (m.idle_since_ms == 0) {
					m.idle_since_ms = static_cast<int64_t>(now_ms);
				} else if (elapsed(now_ms, m.idle_since_ms) >= config_.idle_ms) {
					end(FleetMachineState::Ended,
							"no server on it for " + std::to_string(config_.idle_ms / 60000) +
									" min");
				}
				break;
			case FleetMachineState::Draining:
				// One drained while it booted, its agent never heard from (so never
				// lost): its servers move off as any, and the boot timeout still
				// ends it.
				if (m.last_report_at.empty() && in_state >= boot_timeout) {
					launch_failed(db, m.kind, now_ms);
					end(FleetMachineState::Failed,
							"its machine did not boot in " + seconds_text(boot_timeout));
					break;
				}
				drain_step(db, m, on, now_ms);
				break;
			case FleetMachineState::Ending:
				end(FleetMachineState::Ended, m.end_reason.empty() ? "ended" : m.end_reason);
				break;
			case FleetMachineState::Lost: {
				if (machine_reported_within(db, m.id, config_.machine_silence_s)) {
					const FleetMachineState back =
							m.was_draining ? FleetMachineState::Draining : FleetMachineState::Ready;
					if (transition_machine(db, m.id, FleetMachineState::Lost, back, "its agent reports again"))
						std::printf("[fleet] machine %lld: lost -> %s (its agent reports again)\n",
								static_cast<long long>(m.id), fleet_machine_state_text(back));
					break;
				}
				// The cloud decides, by a describe since it went lost: still running
				// lost_end_ms on, it ends; one that did not answer decides nothing.
				ask_describe(db, m, now_ms);
				if ((cloud == CloudState::Pending || cloud == CloudState::Running) &&
						in_state >= config_.lost_end_ms)
					end(FleetMachineState::Ended,
							"its agent was silent for " + seconds_text(config_.lost_end_ms) +
									" more");
				break;
			}
			case FleetMachineState::Ended:
			case FleetMachineState::Failed:
				break;
		}
		if (!ended &&
				(m.seen_state != remembered.seen_state ||
						m.seen_since_ms != remembered.seen_since_ms ||
						m.idle_since_ms != remembered.idle_since_ms ||
						m.drain_moving != remembered.drain_moving ||
						m.was_draining != remembered.was_draining))
			save_machine_runtime(db, m);
	}
}

// --- the desired state ---------------------------------------------------------------

FleetDecision Supervisor::sync(db::Database &db, const FleetMachineRow &machine,
		const std::vector<ContainerReport> &reported) {
	FleetDecision decision;
	std::set<std::string> names;
	for (const ContainerReport &c : reported) names.insert(c.name);
	const std::vector<SlotHolder> holders = slot_holders(db, machine.id);
	// What the starts in memory answer, in the holders' order; the containers no
	// start answers for are read from their rows after, with no database work
	// under starts_mu_ (the pass takes it too).
	std::vector<std::optional<DesiredContainer>> wanted(holders.size());
	std::vector<size_t> unread;
	{
		std::lock_guard<std::mutex> lock(starts_mu_);
		// A lost machine that was draining drains again once it is back.
		decision.drain = machine.state == FleetMachineState::Draining ||
				(machine.state == FleetMachineState::Lost && machine.was_draining);
		for (size_t i = 0; i < holders.size(); ++i) {
			const SlotHolder &h = holders[i];
			if (h.state != State::Booting && h.state != State::Listing && h.state != State::Running &&
					h.state != State::Stopping)
				continue;
			const std::string name = hosted_container_name(h.server_id, h.generation);
			const bool seen = names.count(name) != 0;
			auto it = starts_.find(h.server_id);
			if (it != starts_.end() && it->second.container.name == name && it->second.machine_id == machine.id) {
				Start &start = it->second;
				if (seen) {
					// Reported once: its files are the agent's now, and no longer kept here.
					start.acknowledged = true;
					start.container.files.reset();
				}
				// A stopping server's container that never started stays unstarted.
				if (h.state == State::Stopping && !start.acknowledged) continue;
				wanted[i] = start.container;
				continue;
			}
			// No start in memory (the service started since): only a container its
			// agent reports is still wanted, without files.
			if (seen) unread.push_back(i);
		}
	}
	for (const size_t i : unread) {
		const SlotHolder &h = holders[i];
		const auto server = find_server(db, h.server_id);
		std::string why;
		const auto data = server ? data_of(db, *server, why) : std::nullopt;
		if (!server || !data || !server->base_tree_id) continue;
		Start start;
		start.machine_id = machine.id;
		start.acknowledged = true;
		DesiredContainer &c = start.container;
		c.name = hosted_container_name(h.server_id, h.generation);
		c.slot = h.slot;
		c.image = server->serve_image.empty() ? config_.serve_image : server->serve_image;
		c.uid = slot_uid(h.slot);
		c.base_tree = *server->base_tree_id;
		c.expansion_tree = server->expansion_tree_id;
		c.expansion_code = data->expansion_code;
		c.args = render_serve_args(server->settings, data->title, data->expansion_code, config_.master_host,
				config_.master_gate_port);
		c.cpus = config_.slot_cpus;
		c.memory_mb = config_.slot_memory_mb;
		c.pids = config_.slot_pids;
		std::lock_guard<std::mutex> lock(starts_mu_);
		auto it = starts_.find(h.server_id);
		if (it == starts_.end()) {
			it = starts_.emplace(h.server_id, std::move(start)).first;
		} else if (it->second.container.name != c.name || it->second.machine_id != machine.id) {
			// The pass started the server again meanwhile: the old container is no
			// longer wanted, and the next sync answers for the new one.
			continue;
		}
		wanted[i] = it->second.container;
	}
	for (std::optional<DesiredContainer> &w : wanted) {
		if (!w) continue;
		// The trees the container uses, each as the agent fetches it (its URL in
		// the store and its manifest); one with no manifest cannot be answered
		// for (the reply would not read), so its container is left out.
		bool complete = true;
		for (const std::optional<int64_t> id : { std::optional<int64_t>(w->base_tree), w->expansion_tree }) {
			if (!id || decision.trees.count(*id) != 0) continue;
			if (const auto spec = tree_spec(db, *id))
				decision.trees[*id] = *spec;
			else
				complete = false;
		}
		if (!complete) {
			std::fprintf(stderr, "[hosted] WARN container %s of machine %lld: a tree it uses has no manifest; "
								 "not answered for\n",
					w->name.c_str(), static_cast<long long>(machine.id));
			continue;
		}
		decision.desired.push_back(std::move(*w));
	}
	return decision;
}

} // namespace opennova::novaworld_server::hosted
