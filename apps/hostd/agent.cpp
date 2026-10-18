#include "agent.h"

#include "reconcile.h"
#include "saved_machine.h"
#include "steady_clock.h"

#include "net_sockets.h"

#include <base/io/file_io.h>
#include <base/io/log.h>
#include <base/io/strutil.h>

#include <base/io/json.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace opennova::hostd {

namespace {

using io::LogLevel;

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

// An image reference's repository: what comes before its digest or its tag.
std::string image_repository(const std::string &image) {
	const size_t at = image.find('@');
	if (at != std::string::npos)
		return image.substr(0, at);
	const size_t colon = image.rfind(':');
	const size_t slash = image.rfind('/');
	if (colon != std::string::npos && (slash == std::string::npos || colon > slash))
		return image.substr(0, colon);
	return image;
}

std::string http_failure(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "the service is unreachable (" + reply.error + ")";
	return "the service answered " + std::to_string(reply.code);
}

// `text` as a report's reason: printable ASCII alone, at most the protocol's
// length (a fetch's error is a CLI's line, cut here before the service refuses
// the report).
std::string reason_text(std::string text) {
	for (char &c : text)
		if (c < 0x20 || c > 0x7E)
			c = ' ';
	if (text.size() > hosted::kMaxContainerReasonBytes)
		text.resize(hosted::kMaxContainerReasonBytes);
	return text;
}

// The line an exited server wrote in `dir` at its non-zero exit (its
// --exit-file), as a report's reason; "" for none.
std::string exit_reason_in(const std::string &dir) {
	std::string text;
	std::string error;
	if (!io::read_file_text(dir + "/" + hosted::kExitFileName, text, error))
		return {};
	if (const size_t eol = text.find('\n'); eol != std::string::npos)
		text.resize(eol);
	text = strutil::trim(text);
	return text.empty() ? std::string() : reason_text(text);
}

} // namespace

std::optional<hosted::TickHealth> parse_tick_stats(std::string_view text) {
	io::JsonValue v;
	std::string error;
	if (!io::json_parse(text, v, error) || !v.is_object())
		return std::nullopt;
	hosted::TickHealth out;
	double *fields[] = { &out.p50_ms, &out.p99_ms, &out.max_ms };
	const char *names[] = { "p50_ms", "p99_ms", "max_ms" };
	for (size_t i = 0; i < 3; ++i) {
		const io::JsonValue *f = v.get(names[i]);
		if (!f || !f->is_number() || !(f->number >= 0.0 && f->number <= 1e6))
			return std::nullopt;
		*fields[i] = f->number;
	}
	const io::JsonValue *late = v.get("late");
	if (!late || !io::json_exact_whole(*late, out.late))
		return std::nullopt;
	return out;
}

// --- the link ------------------------------------------------------------------

HttpServiceLink::HttpServiceLink(std::string base_url, int timeout_ms, Resolver resolve) :
		base_url_(std::move(base_url)), timeout_ms_(timeout_ms), resolve_(std::move(resolve)) {
	while (!base_url_.empty() && base_url_.back() == '/')
		base_url_.pop_back();
}

net::HttpReply HttpServiceLink::post(
		const std::string &path, const std::string &bearer, const std::string &body) {
	// The host's addresses, read at the call's first try; each try the next.
	std::vector<net::Endpoint> addresses;
	size_t next = 0;
	net::HttpReply reply;
	do {
		reply = net::http_exchange("POST", base_url_ + path,
				{ "Authorization: Bearer " + bearer, "Content-Type: application/json" }, body,
				timeout_ms_,
				[this, &addresses, &next](const std::string &host, net::Endpoint &out) {
					if (next == 0)
						addresses = resolve_(host);
					if (next >= addresses.size())
						return false;
					out.ip = addresses[next++].ip;
					return true;
				});
	} while (!reply.connected && next > 0 && next < addresses.size());
	return reply;
}

// --- the agent -----------------------------------------------------------------

Agent::Agent(AgentConfig config, ServiceLink &link, ContainerRuntime &runtime, SlotFiles &slots,
		TreeStore &trees, SlotCounters *counters, Clock now_ms) :
		config_(std::move(config)),
		link_(link),
		runtime_(runtime),
		slots_(slots),
		trees_(trees),
		counters_(counters),
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
	std::string error;
	if (!io::read_file_text(saved_path(), text, error))
		return false;
	// The agent's own format, whatever protocol saved it (saved_machine.h).
	SavedMachine saved;
	if (!decode_saved_machine(text, saved, error)) {
		warn("saved", "the saved bootstrap " + saved_path() + " does not read (" + error + ")");
		return false;
	}
	if (saved.machine_id != config_.machine_id) {
		warn("saved",
				"the saved bootstrap " + saved_path() + " is machine " +
						std::to_string(saved.machine_id) + "'s");
		return false;
	}
	machine_ = hosted::MachineBootstrap{};
	machine_.agent_secret = std::move(saved.agent_secret);
	machine_.serve_image = std::move(saved.serve_image);
	machine_.slots = saved.slots;
	return true;
}

bool Agent::save(std::string &error) const {
	SavedMachine saved;
	saved.machine_id = config_.machine_id;
	saved.agent_secret = machine_.agent_secret;
	saved.serve_image = machine_.serve_image;
	saved.slots = machine_.slots;
	return write_owner_only(saved_path(), encode_saved_machine(saved), error);
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
	std::string token_error;
	if (!io::read_file_text(config_.token_file, token, token_error)) {
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
	if (!reply.transport_ok) {
		// The service out of reach: tried again at the usual pace.
		bootstrap_retry_ms_ = kBootstrapRetryMs;
		bootstrap_refused_ = false;
		warn("bootstrap", "the bootstrap failed: " + http_failure(reply));
		return false;
	}
	if (reply.code != 200) {
		// A refusal (a token spent, lapsed or unknown, a machine no longer
		// booting): tried again later each time, up to kBootstrapBackoffMaxMs.
		if (bootstrap_refused_)
			bootstrap_retry_ms_ = std::min(bootstrap_retry_ms_ * 2, kBootstrapBackoffMaxMs);
		bootstrap_refused_ = true;
		warn("bootstrap",
				"the bootstrap was refused: " + http_failure(reply) +
						" (a token spent, lapsed or unknown); trying again in " +
						std::to_string(bootstrap_retry_ms_ / 1000) + " s");
		return false;
	}
	std::string error;
	hosted::MachineBootstrap got;
	if (!hosted::decode_machine_bootstrap(
				std::string(reply.body.begin(), reply.body.end()), got, error)) {
		// The service keeps the token until the first sync: the next try
		// redeems it again.
		bootstrap_retry_ms_ = kBootstrapRetryMs;
		bootstrap_refused_ = false;
		warn("bootstrap", "the bootstrap's reply does not read (" + error + ")");
		return false;
	}
	machine_ = std::move(got);
	// The saved reply stands in for the token from here on (the first sync
	// spends it). A save that fails leaves the agent running on what it holds;
	// its next restart cannot bootstrap again, which the service sees as a
	// lost machine.
	if (!save(error))
		io::logf(LogLevel::kError, "the bootstrap could not be saved: %s", error.c_str());
	if (::unlink(config_.token_file.c_str()) != 0 && errno != ENOENT)
		io::logf(LogLevel::kWarn, "the spent token file %s could not be removed: %s",
				config_.token_file.c_str(), std::strerror(errno));
	recovered("bootstrap", "bootstrapped");
	io::logf(LogLevel::kInfo, "machine %lld bootstrapped: %d slots, image %s",
			static_cast<long long>(config_.machine_id), machine_.slots,
			machine_.serve_image.c_str());
	bootstrapped_now();
	return true;
}

void Agent::bootstrapped_now() {
	bootstrapped_ = true;
}

int Agent::call_ms(uint64_t deadline) const {
	const uint64_t now = now_();
	if (now >= deadline || deadline - now < kMinCallMs)
		return 0;
	return static_cast<int>(std::min<uint64_t>(deadline - now, kDockerCallMs));
}

void Agent::refresh_stats(
		const std::vector<ContainerInfo> &observed, uint64_t now, uint64_t deadline) {
	if (stats_read_ && now - stats_at_ < kStatsIntervalMs)
		return;
	// No time left in the pass: the last read stands, and the next pass reads.
	const int ms = call_ms(deadline);
	if (ms == 0)
		return;
	std::vector<std::string> running;
	for (const ContainerInfo &c : observed)
		if (c.state == hosted::ContainerState::Running)
			running.push_back(c.name);
	std::map<std::string, ContainerStats> read;
	std::string error;
	stats_read_ = true;
	stats_at_ = now;
	if (!runtime_.stats(running, read, ms, error)) {
		warn("stats", "the containers' stats do not read: " + error);
		return;
	}
	recovered("stats", "the containers' stats read again");
	stats_ = std::move(read);
}

void Agent::refresh_machine(const std::vector<ContainerInfo> &observed, uint64_t now) {
	namespace ps = novaworld_server::proc_stats;
	if (machine_read_ && now - machine_at_ < kStatsIntervalMs)
		return;
	machine_read_ = true;
	machine_at_ = now;
	// Each running server's last minute of ticks, while its file is fresh.
	ticks_.clear();
	const auto fresh_before =
			std::filesystem::file_time_type::clock::now() - std::chrono::milliseconds(kTickStaleMs);
	for (const ContainerInfo &c : observed) {
		if (c.state != hosted::ContainerState::Running || !hosted::slot_valid(c.slot))
			continue;
		const std::string path = slots_.dir(c.slot) + "/" + hosted::kTickStatsFileName;
		std::error_code ec;
		const auto written = std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
		std::string text;
		std::string error;
		if (ec || written < fresh_before || !io::read_file_text(path, text, error))
			continue;
		if (const auto tick = parse_tick_stats(text))
			ticks_[c.name] = *tick;
	}
	// The machine: the CPU busy since the last reading, the memory, the trees'
	// filesystem, the network.
	ps::CpuTimes cpu;
	ps::Memory memory;
	ps::NetBytes net;
	ps::DiskSpace disk;
	if (!ps::read_host_cpu(cpu, config_.proc_root) ||
			!ps::read_host_memory(memory, config_.proc_root) ||
			!ps::read_net_dev(net, config_.proc_root) || !ps::read_disk(trees_.root(), disk)) {
		warn("machine",
				"the machine's use does not read (/proc, statvfs of " + trees_.root() + ")");
		return;
	}
	recovered("machine", "the machine's use reads again");
	if (cpu_mark_) {
		hosted::MachineStats m;
		m.cpu_percent = std::clamp(ps::cpu_percent(*cpu_mark_, cpu), 0.0, 100.0);
		m.memory_used_bytes = memory.used_bytes;
		m.memory_total_bytes = memory.total_bytes;
		m.disk_free_bytes = disk.free_bytes;
		m.disk_total_bytes = disk.total_bytes;
		m.net_in_bytes = net.in_bytes;
		m.net_out_bytes = net.out_bytes;
		machine_stats_ = m;
	}
	cpu_mark_ = cpu;
}

void Agent::refresh_counts(uint64_t now, uint64_t deadline) {
	if (!counters_ || call_ms(deadline) == 0)
		return;
	std::string error;
	std::map<int, SlotCount> read;
	bool ok = counters_ready_ && counters_->read(read, error);
	// Put in place at the first pass, and again (at most every
	// kCountersEnsureMs) whenever a read fails.
	if (!ok && (!counters_attempted_ || now - counters_attempt_at_ >= kCountersEnsureMs)) {
		counters_attempted_ = true;
		counters_attempt_at_ = now;
		if (counters_->ensure(error)) {
			counters_ready_ = true;
			read.clear();
			ok = counters_->read(read, error);
		}
	}
	// What did read counts; a slot that did not keeps its last counts.
	for (const auto &slot : read)
		counts_[slot.first] = slot.second;
	if (!ok) {
		warn("counters", "the slots' counters do not read: " + error);
		return;
	}
	recovered("counters", "the slots' counters read again");
}

void Agent::describe_machine(hosted::SyncRequest &request) const {
	request.image_id = config_.image_id;
	request.log_stream = config_.log_stream;
	request.machine = machine_stats_;
	// Every tree the store holds: one whose failure has a try left is still
	// being fetched.
	for (const auto &[id, status] : trees_.statuses()) {
		if (request.trees.size() >= hosted::kMaxReportedTrees)
			break;
		hosted::TreeReport tree;
		if (status.state == TreeFetchState::Present)
			tree.state = hosted::TreeReportState::Ready;
		else if (status.state == TreeFetchState::Failed && status.tries >= TreeStore::kMaxTries)
			tree.state = hosted::TreeReportState::Failed;
		tree.bytes = status.bytes;
		tree.tries = status.tries;
		if (!strutil::trim(status.error).empty())
			tree.reason = reason_text(strutil::trim(status.error));
		request.trees[id] = std::move(tree);
	}
}

hosted::SyncRequest Agent::report(const std::vector<ContainerInfo> &observed) const {
	hosted::SyncRequest request;
	request.agent_version = config_.agent_version;
	request.agent_image = config_.agent_image;
	describe_machine(request);
	const auto count_of = [this](int slot) -> SlotCount {
		const auto it = counts_.find(slot);
		return it == counts_.end() ? SlotCount{} : it->second;
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
			const auto tick = ticks_.find(c.name);
			if (tick != ticks_.end())
				r.tick = tick->second;
		}
		// Why it ended: the line it wrote at a non-zero exit, in its slot's
		// directory until the container is removed.
		if (c.state == hosted::ContainerState::Exited && c.exit_code)
			r.reason = exit_reason_in(slots_.dir(c.slot));
		r.egress_bytes = count_of(c.slot).egress_bytes;
		r.net_in_bytes = count_of(c.slot).ingress_bytes;
		reported.insert(c.name);
		request.containers.push_back(std::move(r));
	}
	// A container waiting for its image's pull or its trees' fetch: starting,
	// on its slot, saying which.
	for (const auto &waiting : waiting_) {
		if (!reported.insert(waiting.first).second)
			continue;
		hosted::ContainerReport r;
		r.name = waiting.first;
		r.slot = waiting.second.desired.slot;
		r.state = hosted::ContainerState::Starting;
		r.waiting = waiting.second.cause;
		r.egress_bytes = count_of(r.slot).egress_bytes;
		r.net_in_bytes = count_of(r.slot).ingress_bytes;
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
		r.reason = failed.second.reason;
		r.egress_bytes = count_of(failed.second.slot).egress_bytes;
		r.net_in_bytes = count_of(failed.second.slot).ingress_bytes;
		request.containers.push_back(std::move(r));
	}
	return request;
}

int Agent::step() {
	if (!bootstrapped_ && !bootstrap())
		return static_cast<int>(bootstrap_retry_ms_);
	const uint64_t now = now_();
	poll_pulls(now);
	// The listing and the stats share the report's time.
	const uint64_t deadline = now + kReportBudgetMs;
	std::vector<ContainerInfo> observed;
	std::string error;
	if (!runtime_.list(observed, call_ms(deadline), error)) {
		// Nothing is done blind, but the service still hears from the agent (the
		// machine is not lost for a docker that does not answer): a sync that says
		// so, its answer not acted on.
		warn("runtime", "the containers cannot be listed: " + error);
		refresh_machine({}, now);
		hosted::SyncRequest unlisted;
		unlisted.agent_version = config_.agent_version;
		unlisted.agent_image = config_.agent_image;
		describe_machine(unlisted);
		unlisted.listing_failed = true;
		const net::HttpReply reply = link_.post(hosted::machine_sync_path(config_.machine_id),
				machine_.agent_secret, hosted::encode_sync_request(unlisted));
		if (!reply.transport_ok || reply.code != 200)
			warn("sync", "the sync failed: " + http_failure(reply));
		last_sync_ok_ = false;
		return hosted::kSyncIntervalMs;
	}
	recovered("runtime", "the containers list again");
	refresh_stats(observed, now, deadline);
	refresh_machine(observed, now);
	refresh_counts(now, deadline);

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
	if (delay != hosted::kBusySyncIntervalMs)
		prune_images(desired, observed);
	return delay;
}

void Agent::prune_images(
		const hosted::SyncReply &reply, const std::vector<ContainerInfo> &observed) {
	for (const hosted::DesiredContainer &d : reply.desired)
		if (std::find(named_images_.begin(), named_images_.end(), d.image) == named_images_.end())
			named_images_.push_back(d.image);
	const uint64_t now = now_();
	if (named_images_.empty() || (pruned_ && now - pruned_at_ < kImagePruneIntervalMs))
		return;
	pruned_ = true;
	pruned_at_ = now;
	// What a container uses, or will (a pull under way, a start waiting), and
	// the newest the replies named.
	std::set<std::string> keep;
	for (const hosted::DesiredContainer &d : reply.desired)
		keep.insert(d.image);
	for (const ContainerInfo &c : observed)
		keep.insert(c.image);
	for (const auto &w : waiting_)
		keep.insert(w.second.desired.image);
	for (const auto &p : pulls_)
		keep.insert(p.first);
	for (size_t i = 0; i < kImagesKept && i < named_images_.size(); ++i)
		keep.insert(named_images_[named_images_.size() - 1 - i]);
	std::set<std::string> repositories;
	for (const std::string &image : named_images_)
		repositories.insert(image_repository(image));
	const uint64_t deadline = now + kActBudgetMs;
	std::string error;
	for (const std::string &repository : repositories) {
		std::vector<std::string> held;
		int ms = call_ms(deadline);
		if (ms == 0)
			return;
		if (!runtime_.list_images(repository, held, ms, error)) {
			warn("prune", "the machine's images do not list: " + error);
			return;
		}
		recovered("prune", "the machine's images list again");
		for (const std::string &image : held) {
			if (keep.count(image) || (ms = call_ms(deadline)) == 0)
				continue;
			if (!runtime_.remove_image(image, ms, error)) {
				io::logf(LogLevel::kWarn, "%s", error.c_str());
				continue;
			}
			images_.erase(image);
			named_images_.erase(std::remove(named_images_.begin(), named_images_.end(), image),
					named_images_.end());
			io::logf(LogLevel::kInfo, "removed the image %s: no container uses it", image.c_str());
		}
	}
}

void Agent::poll_pulls(uint64_t now) {
	for (auto it = pulls_.begin(); it != pulls_.end();) {
		const std::string image = it->first;
		std::string error;
		PullState state = runtime_.poll_pull(image, error);
		if (state == PullState::Running) {
			if (now - it->second < kPullTimeoutMs) {
				++it;
				continue;
			}
			runtime_.cancel_pull(image);
			state = PullState::Failed;
			error = "it did not finish in " + std::to_string(kPullTimeoutMs / 1000) + " s";
		}
		it = pulls_.erase(it);
		if (state == PullState::Done) {
			io::logf(LogLevel::kInfo, "pulled %s", image.c_str());
			images_.insert(image);
			continue;
		}
		io::logf(LogLevel::kError, "%s did not pull: %s", image.c_str(), error.c_str());
		// Its containers never ran: reported exited 125, as any start that failed.
		for (auto w = waiting_.begin(); w != waiting_.end();) {
			const hosted::DesiredContainer &d = w->second.desired;
			if (d.image != image) {
				++w;
				continue;
			}
			io::logf(LogLevel::kError, "%s did not start on slot %d: its image did not pull",
					w->first.c_str(), d.slot);
			failed_starts_[w->first] = { d.slot, 125, std::string() };
			w = waiting_.erase(w);
		}
	}
}

void Agent::act(
		const hosted::SyncReply &reply, const std::vector<ContainerInfo> &observed, int &delay) {
	const uint64_t deadline = now_() + kActBudgetMs;
	// The reply's containers; one waiting for its image takes the files the
	// reply that started it carried (the later ones leave them out).
	std::vector<hosted::DesiredContainer> desired = reply.desired;
	for (hosted::DesiredContainer &d : desired) {
		const auto waiting = waiting_.find(d.name);
		if (waiting != waiting_.end() && !d.files)
			d.files = waiting->second.desired.files;
	}
	std::set<std::string> failed;
	for (const auto &f : failed_starts_)
		failed.insert(f.first);
	std::set<std::string> pulling;
	for (const auto &p : pulls_)
		pulling.insert(p.first);
	// The trees the reply names: fetched where the machine lacks them (one at
	// a time, in the background), so the plan knows where each stands.
	trees_.want(reply.trees);
	const std::map<int64_t, TreeStanding> tree_states = trees_.standings();
	PlanInput in;
	in.desired = &desired;
	in.observed = &observed;
	in.stop_requested_ms = &stop_requested_;
	in.failed_starts = &failed;
	in.pulling = &pulling;
	in.machine_slots = machine_.slots;
	in.trees = &tree_states;
	in.now_ms = now_();
	const Plan plan = plan_sync(in);

	// Each call in the act's time; what it does not reach waits for the next pass.
	bool deferred = false;
	const auto time_for = [&](int &ms) {
		ms = deferred ? 0 : call_ms(deadline);
		deferred = ms == 0;
		return !deferred;
	};
	std::string error;
	int ms = 0;
	for (const std::string &name : plan.kill) {
		if (!time_for(ms))
			break;
		io::logf(LogLevel::kWarn, "%s did not stop within %llu s: killing it", name.c_str(),
				static_cast<unsigned long long>(kStopTimeoutMs / 1000));
		if (!runtime_.kill(name, ms, error))
			io::logf(LogLevel::kError, "%s", error.c_str());
	}
	for (const std::string &name : plan.stop) {
		if (!time_for(ms))
			break;
		io::logf(LogLevel::kInfo, "stopping %s", name.c_str());
		if (runtime_.stop(name, ms, error))
			stop_requested_[name] = in.now_ms;
		else
			io::logf(LogLevel::kError, "%s", error.c_str());
	}
	for (const ContainerInfo &c : plan.remove) {
		if (!time_for(ms))
			break;
		if (!runtime_.remove(c.name, ms, error)) {
			io::logf(LogLevel::kError, "%s", error.c_str());
			continue;
		}
		io::logf(LogLevel::kInfo, "removed %s (slot %d)", c.name.c_str(), c.slot);
		stop_requested_.erase(c.name);
		if (!slots_.wipe(c.slot, error))
			io::logf(LogLevel::kError, "%s", error.c_str());
	}
	for (const auto &refused : plan.refused) {
		const std::string &name = desired[refused.first].name;
		if (refused_logged_.insert(name).second)
			io::logf(LogLevel::kWarn, "%s cannot start here: %s", name.c_str(),
					refused.second.c_str());
	}
	// A tree the store gave up on: its containers never ran, reported exited
	// 125, as any start that failed (nothing of theirs is on the machine yet),
	// with the fetch's reason, which the service shows the owner.
	for (const auto &failed_data : plan.data_failed) {
		const hosted::DesiredContainer &d = desired[failed_data.first];
		const std::string reason = "its data could not be fetched: " + failed_data.second;
		io::logf(LogLevel::kError, "%s did not start on slot %d: %s", d.name.c_str(), d.slot,
				reason.c_str());
		failed_starts_[d.name] = { d.slot, 125, reason_text(reason) };
		waiting_.erase(d.name);
	}
	for (const size_t index : plan.wait_for_tree) {
		const hosted::DesiredContainer &d = desired[index];
		std::string what = "its data (tree " + std::to_string(d.base_tree);
		if (d.expansion_tree)
			what += " and tree " + std::to_string(*d.expansion_tree);
		wait(d, hosted::ContainerWait::Trees, what + ") to fetch");
	}
	for (const size_t index : plan.wait_for_image)
		wait(desired[index], hosted::ContainerWait::Image,
				"its image " + desired[index].image + " to pull");
	for (const size_t index : plan.start) {
		if (deferred || !start(desired[index], deadline)) {
			deferred = true;
			break;
		}
	}

	// Forget what the reply no longer names (the trees too: the store lets go
	// of one unnamed for a while), and the stops of containers gone.
	std::set<std::string> desired_names;
	for (const hosted::DesiredContainer &d : reply.desired)
		desired_names.insert(d.name);
	std::set<int64_t> wanted_trees;
	for (const auto &tree : reply.trees)
		wanted_trees.insert(tree.first);
	trees_.gc(wanted_trees);
	std::set<std::string> observed_names;
	for (const ContainerInfo &c : observed)
		observed_names.insert(c.name);
	for (auto it = failed_starts_.begin(); it != failed_starts_.end();)
		it = desired_names.count(it->first) ? std::next(it) : failed_starts_.erase(it);
	for (auto it = waiting_.begin(); it != waiting_.end();)
		it = desired_names.count(it->first) ? std::next(it) : waiting_.erase(it);
	for (auto it = refused_logged_.begin(); it != refused_logged_.end();)
		it = desired_names.count(*it) ? std::next(it) : refused_logged_.erase(it);
	for (auto it = stop_requested_.begin(); it != stop_requested_.end();)
		it = observed_names.count(it->first) ? std::next(it) : stop_requested_.erase(it);
	if (plan.busy || deferred)
		delay = hosted::kBusySyncIntervalMs;
}

void Agent::wait(const hosted::DesiredContainer &desired, hosted::ContainerWait cause,
		const std::string &what) {
	const auto [it, added] = waiting_.emplace(desired.name, Waiting{ desired, cause });
	if (added || it->second.cause != cause)
		io::logf(LogLevel::kInfo, "%s waits for %s", desired.name.c_str(), what.c_str());
	it->second.cause = cause;
}

void Agent::start_failed(const hosted::DesiredContainer &desired, int exit_code,
		const std::string &why, uint64_t deadline) {
	io::logf(LogLevel::kError, "%s did not start on slot %d: %s", desired.name.c_str(),
			desired.slot, why.c_str());
	failed_starts_[desired.name] = { desired.slot, exit_code, std::string() };
	waiting_.erase(desired.name);
	std::string ignored;
	// A container the runtime made but could not start goes, and its files:
	// given 5 s past the act's time too, since one left behind would be
	// reported as starting.
	runtime_.remove(desired.name, std::max(call_ms(deadline), 5 * 1000), ignored);
	slots_.wipe(desired.slot, ignored);
}

bool Agent::start(const hosted::DesiredContainer &desired, uint64_t deadline) {
	std::string error;
	// Its image first: one the machine lacks is pulled in the background, and
	// the container waits for it.
	if (pulls_.count(desired.image)) {
		wait(desired, hosted::ContainerWait::Image, "its image " + desired.image + " to pull");
		return true;
	}
	if (!images_.count(desired.image)) {
		const int ms = call_ms(deadline);
		if (ms == 0)
			return false;
		bool present = false;
		if (!runtime_.has_image(desired.image, present, ms, error)) {
			// Asked again at the next pass.
			warn("image",
					"whether the machine holds " + desired.image + " cannot be told: " + error);
			return true;
		}
		recovered("image", "the machine's images read again");
		if (!present) {
			if (!runtime_.start_pull(desired.image, error)) {
				start_failed(desired, 125, error, deadline);
				return true;
			}
			io::logf(LogLevel::kInfo, "pulling %s", desired.image.c_str());
			pulls_[desired.image] = now_();
			wait(desired, hosted::ContainerWait::Image, "its image " + desired.image + " to pull");
			return true;
		}
		images_.insert(desired.image);
	}
	const int ms = call_ms(deadline);
	if (ms == 0)
		return false;
	ContainerSpec spec;
	spec.name = desired.name;
	spec.slot = desired.slot;
	spec.image = desired.image;
	spec.uid = desired.uid;
	spec.base_dir = trees_.dir_of(desired.base_tree);
	spec.work_dir = slots_.dir(desired.slot);
	spec.args = desired.args;
	spec.cpus = desired.cpus;
	spec.memory_mb = desired.memory_mb;
	spec.pids = desired.pids;
	spec.log_stream = desired.log_stream;

	const auto fail = [&](int exit_code, const std::string &why) {
		start_failed(desired, exit_code, why, deadline);
		return true;
	};
	if (desired.expansion_tree) {
		// The expansion's mount point under the base tree first: a read-only
		// bind cannot have one made in it.
		if (trees_.expansion_mount_point(desired.base_tree, desired.expansion_code, error).empty())
			return fail(125, error);
		spec.expansion_dir = trees_.dir_of(*desired.expansion_tree);
		spec.expansion_code = desired.expansion_code;
	}
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
	const bool ran = runtime_.run(spec, ms, exit_code, error);
	if (!spec.env_file.empty())
		::unlink(spec.env_file.c_str());
	if (!ran) {
		// Asked again before the next start: the image may have gone since.
		images_.erase(desired.image);
		return fail(exit_code, error);
	}
	waiting_.erase(desired.name);
	if (desired.expansion_tree)
		io::logf(LogLevel::kInfo, "started %s on slot %d (tree %lld, expansion %s tree %lld)",
				desired.name.c_str(), desired.slot, static_cast<long long>(desired.base_tree),
				desired.expansion_code.c_str(), static_cast<long long>(*desired.expansion_tree));
	else
		io::logf(LogLevel::kInfo, "started %s on slot %d (tree %lld)", desired.name.c_str(),
				desired.slot, static_cast<long long>(desired.base_tree));
	return true;
}

} // namespace opennova::hostd
