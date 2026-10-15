#include "agent.h"

#include "reconcile.h"

#include "net_sockets.h"

#include <base/io/log.h>
#include <base/io/strutil.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace opennova::hostd {

namespace {

using io::LogLevel;

uint64_t steady_now_ms() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
					.count());
}

bool read_file(const std::string &path, std::string &out) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	std::ostringstream text;
	text << in.rdbuf();
	out = text.str();
	return static_cast<bool>(in) || in.eof();
}

// `bytes` at `path`, owner-only, whole or not at all: a file of its own
// written, flushed and renamed over `path`.
bool write_owner_only(const std::string &path, const std::string &bytes, std::string &error) {
	const std::string temp = path + ".tmp";
	::unlink(temp.c_str());
	const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		error = "cannot create " + temp + ": " + std::strerror(errno);
		return false;
	}
	size_t done = 0;
	bool ok = true;
	while (ok && done < bytes.size()) {
		const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
		if (n < 0 && errno == EINTR)
			continue;
		ok = n > 0;
		if (ok)
			done += static_cast<size_t>(n);
	}
	ok = ok && ::fsync(fd) == 0;
	const int saved = errno;
	::close(fd);
	if (!ok || ::rename(temp.c_str(), path.c_str()) != 0) {
		error = "cannot write " + path + ": " + std::strerror(ok ? errno : saved);
		::unlink(temp.c_str());
		return false;
	}
	return true;
}

// A token as the service mints them: letters and digits, 32..128 of them.
bool is_token_text(const std::string &token) {
	if (token.size() < 32 || token.size() > 128)
		return false;
	for (const char c : token)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	return true;
}

std::string http_failure(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "the service is unreachable (" + reply.error + ")";
	return "the service answered " + std::to_string(reply.code);
}

} // namespace

// --- the link ------------------------------------------------------------------

HttpServiceLink::HttpServiceLink(std::string base_url, int timeout_ms) :
		base_url_(std::move(base_url)), timeout_ms_(timeout_ms) {
	while (!base_url_.empty() && base_url_.back() == '/')
		base_url_.pop_back();
}

net::HttpReply HttpServiceLink::post(
		const std::string &path, const std::string &bearer, const std::string &body) {
	return net::http_exchange("POST", base_url_ + path,
			{ "Authorization: Bearer " + bearer, "Content-Type: application/json" }, body,
			timeout_ms_, [](const std::string &address, net::Endpoint &out) {
				return net::resolve_ipv4(address, out);
			});
}

// --- the agent -----------------------------------------------------------------

Agent::Agent(AgentConfig config, ServiceLink &link, ContainerRuntime &runtime, SlotFiles &slots,
		EgressFactory make_egress, Clock now_ms) :
		config_(std::move(config)),
		link_(link),
		runtime_(runtime),
		slots_(slots),
		make_egress_(std::move(make_egress)),
		now_(now_ms ? std::move(now_ms) : Clock(steady_now_ms)) {}

Agent::~Agent() = default;

std::string Agent::saved_path() const {
	return config_.state_dir + "/machine-" + std::to_string(config_.machine_id) + ".json";
}

void Agent::warn(const std::string &key, const std::string &text) {
	const uint64_t now = now_();
	auto it = warnings_.find(key);
	if (it != warnings_.end() && it->second.first == text && now - it->second.second < kRepeatLogMs)
		return;
	io::logf(LogLevel::kWarn, "%s", text.c_str());
	warnings_[key] = { text, now };
}

void Agent::recovered(const std::string &key, const std::string &text) {
	if (warnings_.erase(key))
		io::logf(LogLevel::kInfo, "%s", text.c_str());
}

bool Agent::load_saved() {
	std::string text;
	if (!read_file(saved_path(), text))
		return false;
	std::string error;
	hosted::MachineBootstrap saved;
	if (!hosted::decode_machine_bootstrap(text, saved, error)) {
		warn("saved", "the saved bootstrap " + saved_path() + " does not read (" + error + ")");
		return false;
	}
	machine_ = std::move(saved);
	return true;
}

bool Agent::save(std::string &error) const {
	return write_owner_only(saved_path(), hosted::encode_machine_bootstrap(machine_), error);
}

bool Agent::bootstrap() {
	if (bootstrapped_)
		return true;
	if (load_saved()) {
		io::logf(LogLevel::kInfo, "machine %lld: the saved bootstrap reads (%d slots)",
				static_cast<long long>(config_.machine_id), machine_.slots);
		bootstrapped_now();
		return true;
	}
	std::string token;
	if (!read_file(config_.token_file, token)) {
		warn("bootstrap", "no saved bootstrap and no token at " + config_.token_file);
		return false;
	}
	token = strutil::trim(token);
	if (!is_token_text(token)) {
		warn("bootstrap", "the token file " + config_.token_file + " holds no token");
		return false;
	}
	hosted::AgentHello hello;
	hello.agent_version = config_.agent_version;
	const net::HttpReply reply = link_.post(hosted::machine_bootstrap_path(config_.machine_id),
			token, hosted::encode_agent_hello(hello));
	if (!reply.transport_ok || reply.code != 200) {
		warn("bootstrap",
				"the bootstrap failed: " + http_failure(reply) +
						(reply.transport_ok ? " (a token spent, lapsed or unknown)" : ""));
		return false;
	}
	std::string error;
	hosted::MachineBootstrap got;
	if (!hosted::decode_machine_bootstrap(
				std::string(reply.body.begin(), reply.body.end()), got, error)) {
		warn("bootstrap", "the bootstrap's reply does not read (" + error + ")");
		return false;
	}
	machine_ = std::move(got);
	// Spent now: the saved reply stands in for it from here on. A save that
	// fails leaves the agent running on what it holds; its next restart cannot
	// bootstrap again, which the service sees as a lost machine.
	if (!save(error))
		io::logf(LogLevel::kError, "the bootstrap could not be saved: %s", error.c_str());
	if (::unlink(config_.token_file.c_str()) != 0 && errno != ENOENT)
		io::logf(LogLevel::kWarn, "the spent token file %s could not be removed: %s",
				config_.token_file.c_str(), std::strerror(errno));
	recovered("bootstrap", "bootstrapped");
	io::logf(LogLevel::kInfo, "machine %lld bootstrapped: %d slots, %zu data sets, image %s",
			static_cast<long long>(config_.machine_id), machine_.slots, machine_.data_sets.size(),
			machine_.serve_image.c_str());
	bootstrapped_now();
	return true;
}

void Agent::bootstrapped_now() {
	bootstrapped_ = true;
	egress_ = make_egress_ ? make_egress_(machine_.slots) : nullptr;
	egress_ready_ = false;
	egress_attempted_ = false;
}

void Agent::refresh_stats(const std::vector<ContainerInfo> &observed, uint64_t now) {
	if (stats_read_ && now - stats_at_ < kStatsIntervalMs)
		return;
	std::vector<std::string> running;
	for (const ContainerInfo &c : observed)
		if (c.state == hosted::ContainerState::Running)
			running.push_back(c.name);
	std::map<std::string, ContainerStats> read;
	std::string error;
	stats_read_ = true;
	stats_at_ = now;
	if (!runtime_.stats(running, read, error)) {
		warn("stats", "the containers' stats do not read: " + error);
		return;
	}
	recovered("stats", "the containers' stats read again");
	stats_ = std::move(read);
}

void Agent::refresh_egress(uint64_t now) {
	if (!egress_)
		return;
	std::string error;
	std::map<int, uint64_t> read;
	bool ok = egress_ready_ && egress_->read(read, error);
	// Put in place at the first pass, and again (at most every
	// kEgressEnsureMs) whenever a read fails.
	if (!ok && (!egress_attempted_ || now - egress_attempt_at_ >= kEgressEnsureMs)) {
		egress_attempted_ = true;
		egress_attempt_at_ = now;
		if (egress_->ensure(error)) {
			egress_ready_ = true;
			read.clear();
			ok = egress_->read(read, error);
		}
	}
	// What did read counts; a slot that did not keeps its last count.
	for (const auto &slot : read)
		egress_bytes_[slot.first] = slot.second;
	if (!ok) {
		warn("egress", "the egress counters do not read: " + error);
		return;
	}
	recovered("egress", "the egress counters read again");
}

hosted::SyncRequest Agent::report(const std::vector<ContainerInfo> &observed) const {
	hosted::SyncRequest request;
	request.agent_version = config_.agent_version;
	const auto egress_of = [this](int slot) -> uint64_t {
		const auto it = egress_bytes_.find(slot);
		return it == egress_bytes_.end() ? 0 : it->second;
	};
	std::set<std::string> reported;
	for (const ContainerInfo &c : observed) {
		// One without its slot label is none of the agent's to report.
		if (!hosted::slot_valid(c.slot))
			continue;
		hosted::ContainerReport r;
		r.name = c.name;
		r.slot = c.slot;
		r.state = c.state;
		r.exit_code = c.exit_code;
		r.oom = c.oom;
		r.started_at = c.started_at;
		if (c.state == hosted::ContainerState::Running) {
			const auto it = stats_.find(c.name);
			if (it != stats_.end()) {
				r.cpu_percent = it->second.cpu_percent;
				r.memory_bytes = it->second.memory_bytes;
			}
		}
		r.egress_bytes = egress_of(c.slot);
		reported.insert(c.name);
		request.containers.push_back(std::move(r));
	}
	for (const auto &failed : failed_starts_) {
		if (reported.count(failed.first))
			continue;
		hosted::ContainerReport r;
		r.name = failed.first;
		r.slot = failed.second.slot;
		r.state = hosted::ContainerState::Exited;
		r.exit_code = failed.second.exit_code;
		r.egress_bytes = egress_of(failed.second.slot);
		request.containers.push_back(std::move(r));
	}
	return request;
}

int Agent::step() {
	if (!bootstrapped_ && !bootstrap())
		return kBootstrapRetryMs;
	const uint64_t now = now_();
	std::vector<ContainerInfo> observed;
	std::string error;
	if (!runtime_.list(observed, error)) {
		// Nothing is reported or done blind: the service reads the silence.
		warn("runtime", "the containers cannot be listed: " + error);
		last_sync_ok_ = false;
		return hosted::kSyncIntervalMs;
	}
	recovered("runtime", "the containers list again");
	refresh_stats(observed, now);
	refresh_egress(now);

	const net::HttpReply reply = link_.post(hosted::machine_sync_path(config_.machine_id),
			machine_.agent_secret, hosted::encode_sync_request(report(observed)));
	last_sync_ok_ = false;
	if (!reply.transport_ok || reply.code != 200) {
		warn("sync",
				"the sync failed: " + http_failure(reply) +
						"; the containers keep running as they are");
		return hosted::kSyncIntervalMs;
	}
	hosted::SyncReply desired;
	if (!hosted::decode_sync_reply(
				std::string(reply.body.begin(), reply.body.end()), desired, error)) {
		warn("sync",
				"the service's reply does not read (" + error +
						"); the containers keep running as they are");
		return hosted::kSyncIntervalMs;
	}
	last_sync_ok_ = true;
	recovered("sync", "the service answers again");
	if (desired.drain != draining_) {
		io::logf(LogLevel::kInfo, "%s",
				desired.drain ? "the service drains this machine" : "the machine no longer drains");
		draining_ = desired.drain;
	}
	int delay = hosted::kSyncIntervalMs;
	act(desired, observed, delay);
	return delay;
}

void Agent::act(
		const hosted::SyncReply &reply, const std::vector<ContainerInfo> &observed, int &delay) {
	std::set<std::string> failed;
	for (const auto &f : failed_starts_)
		failed.insert(f.first);
	PlanInput in;
	in.desired = &reply.desired;
	in.observed = &observed;
	in.stop_requested_ms = &stop_requested_;
	in.failed_starts = &failed;
	in.machine_slots = machine_.slots;
	in.data_sets = &machine_.data_sets;
	in.now_ms = now_();
	const Plan plan = plan_sync(in);

	std::string error;
	for (const std::string &name : plan.kill) {
		io::logf(LogLevel::kWarn, "%s did not stop within %llu s: killing it", name.c_str(),
				static_cast<unsigned long long>(kStopTimeoutMs / 1000));
		if (!runtime_.kill(name, error))
			io::logf(LogLevel::kError, "%s", error.c_str());
	}
	for (const std::string &name : plan.stop) {
		io::logf(LogLevel::kInfo, "stopping %s", name.c_str());
		if (runtime_.stop(name, error))
			stop_requested_[name] = in.now_ms;
		else
			io::logf(LogLevel::kError, "%s", error.c_str());
	}
	for (const ContainerInfo &c : plan.remove) {
		if (!runtime_.remove(c.name, error)) {
			io::logf(LogLevel::kError, "%s", error.c_str());
			continue;
		}
		io::logf(LogLevel::kInfo, "removed %s (slot %d)", c.name.c_str(), c.slot);
		stop_requested_.erase(c.name);
		if (!slots_.wipe(c.slot, error))
			io::logf(LogLevel::kError, "%s", error.c_str());
	}
	for (const auto &refused : plan.refused) {
		const std::string &name = reply.desired[refused.first].name;
		if (refused_logged_.insert(name).second)
			io::logf(LogLevel::kWarn, "%s cannot start here: %s", name.c_str(),
					refused.second.c_str());
	}
	for (const size_t index : plan.start)
		start(reply.desired[index]);

	// Forget what the reply no longer names, and the stops of containers gone.
	std::set<std::string> desired_names;
	for (const hosted::DesiredContainer &d : reply.desired)
		desired_names.insert(d.name);
	std::set<std::string> observed_names;
	for (const ContainerInfo &c : observed)
		observed_names.insert(c.name);
	for (auto it = failed_starts_.begin(); it != failed_starts_.end();)
		it = desired_names.count(it->first) ? std::next(it) : failed_starts_.erase(it);
	for (auto it = refused_logged_.begin(); it != refused_logged_.end();)
		it = desired_names.count(*it) ? std::next(it) : refused_logged_.erase(it);
	for (auto it = stop_requested_.begin(); it != stop_requested_.end();)
		it = observed_names.count(it->first) ? std::next(it) : stop_requested_.erase(it);
	if (plan.busy)
		delay = hosted::kBusySyncIntervalMs;
}

void Agent::start(const hosted::DesiredContainer &desired) {
	std::string error;
	ContainerSpec spec;
	spec.name = desired.name;
	spec.slot = desired.slot;
	spec.image = desired.image;
	spec.uid = desired.uid;
	spec.data_dir = machine_.data_sets.at(desired.data_set);
	spec.work_dir = slots_.dir(desired.slot);
	spec.args = desired.args;
	spec.cpus = desired.cpus;
	spec.memory_mb = desired.memory_mb;
	spec.pids = desired.pids;

	const auto fail = [&](int exit_code, const std::string &why) {
		io::logf(LogLevel::kError, "%s did not start on slot %d: %s", desired.name.c_str(),
				desired.slot, why.c_str());
		failed_starts_[desired.name] = { desired.slot, exit_code };
		std::string ignored;
		// A container the runtime made but could not start goes, and its files.
		runtime_.remove(desired.name, ignored);
		slots_.wipe(desired.slot, ignored);
	};
	if (!slots_.prepare(desired.slot, desired.uid, *desired.files, error))
		return fail(125, error);
	if (!desired.env.empty()) {
		std::string lines;
		for (const auto &var : desired.env)
			lines += var.first + "=" + var.second + "\n";
		spec.env_file = config_.scratch_dir + "/env-" + desired.name;
		if (!write_owner_only(spec.env_file, lines, error))
			return fail(125, error);
	}
	int exit_code = 125;
	const bool ran = runtime_.run(spec, exit_code, error);
	if (!spec.env_file.empty())
		::unlink(spec.env_file.c_str());
	if (!ran)
		return fail(exit_code, error);
	io::logf(LogLevel::kInfo, "started %s on slot %d (%s)", desired.name.c_str(), desired.slot,
			desired.data_set.c_str());
}

} // namespace opennova::hostd
