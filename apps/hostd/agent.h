#pragma once

// opennova-hostd's agent: one machine's side of the agent protocol
// (apps/novaworld_server/hosted/agent_protocol.h). It bootstraps once (the
// one-time token from the machine's user-data redeemed for its long-lived
// secret, the machine's slots and data sets, all saved owner-only so a restart
// of the agent never needs the token again), then syncs: every pass it lists
// its containers, reads their CPU and memory (at most every kStatsIntervalMs)
// and each slot's egress, reports all of it to the service and does what the
// reply asks (reconcile.h): starts the containers the machine lacks, stops and
// removes the ones the reply leaves out. A sync that fails (the service down,
// a refusal, a reply that does not read, another protocol) changes nothing:
// the containers keep running as they are, and the agent keeps reporting. The
// agent never starts a container on its own, nor restarts one that exited:
// the service decides, and counts. The secret and the token reach no log line;
// the files a container gets are written for its slot's user alone. Service
// policy, not a port.
//
// No pass waits long: the service ends a machine whose agent stays silent
// (supervisor.h machine_silence_s, 60 s), so each docker call is cut at
// kDockerCallMs, the listing and the stats before a sync share kReportBudgetMs
// and the reply's acts kActBudgetMs (what the act's time does not reach waits
// for the next pass), and an image the machine lacks is pulled in the
// background: the container waits, reported starting, its files kept here,
// and starts at the first pass after the pull; a pull that fails, or still
// runs kPullTimeoutMs on, is the start's failure (exited 125).

#include "container_runtime.h"
#include "egress_counters.h"
#include "slot_files.h"

#include "net_http.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace opennova::hostd {

// How the agent reaches the service's internal listener.
class ServiceLink {
public:
	virtual ~ServiceLink() = default;
	// POSTs `body` (JSON) to `path` with `Authorization: Bearer <bearer>`.
	virtual net::HttpReply post(
			const std::string &path, const std::string &bearer, const std::string &body) = 0;
};

// The real link: one HTTP exchange per call (apps/common/net_http.h) to
// `base_url`, "http://<address>:<port>" (the service's private address).
class HttpServiceLink : public ServiceLink {
public:
	static constexpr int kTimeoutMs = 10 * 1000;

	explicit HttpServiceLink(std::string base_url, int timeout_ms = kTimeoutMs);
	net::HttpReply post(
			const std::string &path, const std::string &bearer, const std::string &body) override;

private:
	std::string base_url_;
	int timeout_ms_;
};

struct AgentConfig {
	int64_t machine_id = 0;
	std::string agent_version;
	// The saved bootstrap (machine-<id>.json, 0600) lives here (0700).
	std::string state_dir;
	// Where the environment files of a start are written for a moment (0700).
	std::string scratch_dir;
	// The one-time bootstrap token; deleted once redeemed.
	std::string token_file;
};

class Agent {
public:
	using Clock = std::function<uint64_t()>; // a steady clock, ms
	using EgressFactory = std::function<std::unique_ptr<EgressSource>(int slots)>;

	static constexpr int kBootstrapRetryMs = 10 * 1000;
	static constexpr uint64_t kStatsIntervalMs = 5 * 1000;
	// How often a counter that does not read is put in place again, at most.
	static constexpr uint64_t kEgressEnsureMs = 60 * 1000;
	// A warning that repeats unchanged is logged again only this much later.
	static constexpr uint64_t kRepeatLogMs = 5 * 60 * 1000;
	// The most one docker call may take, and the shares of a pass (the class
	// comment); a call is not made with less than kMinCallMs left.
	static constexpr uint64_t kDockerCallMs = 20 * 1000;
	static constexpr uint64_t kReportBudgetMs = 15 * 1000;
	static constexpr uint64_t kActBudgetMs = 10 * 1000;
	static constexpr uint64_t kMinCallMs = 1000;
	// A pull still running this long after it began is ended, a failure.
	static constexpr uint64_t kPullTimeoutMs = 4 * 60 * 1000;

	// `link`, `runtime` and `slots` must outlive the agent. `make_egress`
	// builds the egress counters for the machine's slots once it is
	// bootstrapped; unset (or answering null), every slot reports 0.
	Agent(AgentConfig config, ServiceLink &link, ContainerRuntime &runtime, SlotFiles &slots,
			EgressFactory make_egress = {}, Clock now_ms = {});
	~Agent();

	// The saved bootstrap, or the token redeemed now and saved. False while
	// neither worked (the caller tries again kBootstrapRetryMs later).
	bool bootstrap();
	bool bootstrapped() const { return bootstrapped_; }
	const hosted::MachineBootstrap &machine() const { return machine_; }
	// <state_dir>/machine-<id>.json.
	std::string saved_path() const;

	// One pass (bootstrapping first while it has not): the wait before the
	// next, in ms (kBusySyncIntervalMs while there is work, kSyncIntervalMs
	// otherwise, kBootstrapRetryMs before the bootstrap).
	int step();

	// Whether the last pass's sync was answered and read.
	bool last_sync_ok() const { return last_sync_ok_; }
	// Whether the service drains this machine, as its last reply said.
	bool draining() const { return draining_; }

private:
	struct FailedStart {
		int slot = 0;
		int exit_code = 125;
	};

	bool load_saved();
	bool save(std::string &error) const;
	void bootstrapped_now();
	// What one docker call may take before `deadline` (the agent's clock): at
	// most kDockerCallMs, 0 with less than kMinCallMs left.
	int call_ms(uint64_t deadline) const;
	void refresh_stats(const std::vector<ContainerInfo> &observed, uint64_t now, uint64_t deadline);
	void refresh_egress(uint64_t now, uint64_t deadline);
	// The pulls under way: a finished one's image held, a failed or overdue
	// one's waiting containers failed.
	void poll_pulls(uint64_t now);
	hosted::SyncRequest report(const std::vector<ContainerInfo> &observed) const;
	void act(
			const hosted::SyncReply &reply, const std::vector<ContainerInfo> &observed, int &delay);
	// Starts `desired` (its image pulled first when the machine lacks it);
	// false when `deadline` left no time for it (the next pass tries).
	bool start(const hosted::DesiredContainer &desired, uint64_t deadline);
	// `desired` waits for its image's pull, its files kept here.
	void wait_for_image(const hosted::DesiredContainer &desired);
	// The start failed: reported exited `exit_code`, never made again, and
	// what the runtime made of it removed, its slot's files too.
	void start_failed(const hosted::DesiredContainer &desired, int exit_code,
			const std::string &why, uint64_t deadline);
	// A warning under `key`, logged when it changes or kRepeatLogMs after it was.
	void warn(const std::string &key, const std::string &text);
	// `key`'s warning is over: `text` is logged once when there was one.
	void recovered(const std::string &key, const std::string &text);

	AgentConfig config_;
	ServiceLink &link_;
	ContainerRuntime &runtime_;
	SlotFiles &slots_;
	EgressFactory make_egress_;
	Clock now_;

	bool bootstrapped_ = false;
	hosted::MachineBootstrap machine_;
	std::unique_ptr<EgressSource> egress_;
	bool egress_ready_ = false; // put in place once
	bool egress_attempted_ = false; // and when it was last tried
	uint64_t egress_attempt_at_ = 0;
	std::map<int, uint64_t> egress_bytes_; // by slot, the last good read
	std::map<std::string, ContainerStats> stats_; // by name, the last good read
	uint64_t stats_at_ = 0;
	bool stats_read_ = false;
	std::map<std::string, uint64_t> stop_requested_; // by name: when its SIGTERM went
	std::map<std::string, FailedStart> failed_starts_; // by name
	std::map<std::string, uint64_t> pulls_; // image -> when its pull began
	std::set<std::string> images_; // the images the machine holds, as checked
	// By name: the desired containers whose image pulls, as the reply that
	// started them gave them (their files with them).
	std::map<std::string, hosted::DesiredContainer> waiting_;
	std::set<std::string> refused_logged_; // the desired names whose refusal was logged
	std::map<std::string, std::pair<std::string, uint64_t>> warnings_; // key -> text, when
	bool last_sync_ok_ = false;
	bool draining_ = false;
};

} // namespace opennova::hostd
