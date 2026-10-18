#pragma once

// opennova-hostd's agent: one machine's side of the agent protocol
// (apps/novaworld_common/hosted/agent_protocol.h). It bootstraps once (the
// one-time token from the machine's user-data redeemed for its long-lived
// secret and the machine's slots, saved owner-only in a file of its own format,
// saved_machine.h, so a restart of the agent, of this protocol or a later one,
// never needs the token again), then syncs: every pass it lists its
// containers, reads their CPU and memory, the machine's own use (proc_stats.h)
// and each running server's tick health (its --stats-file in the slot's
// directory; at most every kStatsIntervalMs, a file older than kTickStaleMs
// none), each slot's counts (nft_table.h), an exited server's last line (its
// --exit-file; both files read as SlotFiles::read_written reads what a slot's
// user controls) and where each tree stands, reports them to the service with
// the machine's image and its own log stream, and does what the reply asks
// (reconcile.h): fetches the trees the reply names (tree_store.h), starts the
// containers the machine lacks, stops and removes the ones the reply leaves
// out. A sync that fails (the service down,
// a refusal, a reply that does not read, another protocol) changes nothing:
// the containers keep running as they are, and the agent keeps reporting; a
// pass that cannot list its containers syncs to say so and does nothing. The
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
// background, as a tree it lacks is fetched: the container waits, reported
// starting, its files kept here, and starts at the first pass after the pull
// or the fetch; a pull that fails, or still runs kPullTimeoutMs on, is the
// start's failure (exited 125), as a fetch the store gave up on is, whose
// reason then rides the container's report (ContainerReport::reason).

#include "container_runtime.h"
#include "nft_table.h"
#include "slot_files.h"
#include "tree_store.h"

#include "net_http.h"
#include "proc_stats.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::hostd {

// A server's --stats-file line (apps/serve/run_reports.h) as its tick health:
// nullopt when it does not read.
std::optional<hosted::TickHealth> parse_tick_stats(std::string_view text);

// How the agent reaches the service's internal listener.
class ServiceLink {
public:
	virtual ~ServiceLink() = default;
	// POSTs `body` (JSON) to `path` with `Authorization: Bearer <bearer>`.
	virtual net::HttpReply post(
			const std::string &path, const std::string &bearer, const std::string &body) = 0;
};

// The real link: one HTTP exchange per call (apps/common/net_http.h) to
// `base_url`, "http://<host>:<port>" (the fleet program's internal name, or a
// private address). Each call tries every IPv4 address the host resolves to,
// in turn, until one takes the connection: while the fleet leader is replaced,
// its name lists the standby beside the leader (Cloud Map, MULTIVALUE), and
// the standby does not listen. Only a connection never made moves on to the
// next address; a request that reached a server is never sent again.
class HttpServiceLink : public ServiceLink {
public:
	static constexpr int kTimeoutMs = 10 * 1000;
	// A host's addresses (net::resolve_ipv4_all; a test's own list).
	using Resolver = std::function<std::vector<net::Endpoint>(const std::string &host)>;

	explicit HttpServiceLink(std::string base_url, int timeout_ms = kTimeoutMs,
			Resolver resolve = net::resolve_ipv4_all);
	net::HttpReply post(
			const std::string &path, const std::string &bearer, const std::string &body) override;

private:
	std::string base_url_;
	int timeout_ms_;
	Resolver resolve_;
};

struct AgentConfig {
	int64_t machine_id = 0;
	std::string agent_version;
	// The image the agent's ECS task runs, as its task definition pins it (the
	// envelope's agent_image); "" off ECS.
	std::string agent_image;
	// The saved bootstrap (machine-<id>.json, 0600, saved_machine.h: a format of
	// the agent's own, no protocol in it) lives here (0700).
	std::string state_dir;
	// Where the environment files of a start are written for a moment (0700).
	std::string scratch_dir;
	// The one-time bootstrap token; deleted once redeemed.
	std::string token_file;
	// What every report says of the machine: its image (the instance's AMI,
	// read once at the start; "" off EC2) and the agent's own log stream (its
	// ECS task's; "" off ECS).
	std::string image_id;
	std::string log_stream;
	// The root the kernel's files are read under (proc_stats.h): "" on a
	// machine, a fixture tree in the tests.
	std::string proc_root;
};

class Agent {
public:
	using Clock = std::function<uint64_t()>; // a steady clock, ms

	static constexpr int kBootstrapRetryMs = 10 * 1000;
	// A bootstrap the service refuses is tried again twice as late each time,
	// up to this.
	static constexpr int kBootstrapBackoffMaxMs = 5 * 60 * 1000;
	static constexpr uint64_t kStatsIntervalMs = 5 * 1000;
	// A server's tick file (written once a minute) older than this says nothing.
	static constexpr uint64_t kTickStaleMs = 3 * 60 * 1000;
	// How often counters that do not read are put in place again, at most.
	static constexpr uint64_t kCountersEnsureMs = 60 * 1000;
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
	// The machine's serve images are pruned at most this often, at a pass with
	// no work under way: every image of a repository the replies named that no
	// desired or present container uses is removed, but the kImagesKept the
	// replies named last (the release's current and previous: a rollback or a
	// restart on either pulls nothing).
	static constexpr uint64_t kImagePruneIntervalMs = 10 * 60 * 1000;
	static constexpr size_t kImagesKept = 2;

	// `link`, `runtime`, `slots`, `trees` and `counters` must outlive the
	// agent. With no `counters` (the process runtime's machine), every slot
	// reports 0.
	Agent(AgentConfig config, ServiceLink &link, ContainerRuntime &runtime, SlotFiles &slots,
			TreeStore &trees, SlotCounters *counters = nullptr, Clock now_ms = {});
	~Agent();

	// The saved bootstrap, or the token redeemed now and saved. False while
	// neither worked: the caller tries again kBootstrapRetryMs later, or, after
	// refusals in a row, twice as late each time up to kBootstrapBackoffMaxMs.
	bool bootstrap();
	bool bootstrapped() const { return bootstrapped_; }
	const hosted::MachineBootstrap &machine() const { return machine_; }
	// <state_dir>/machine-<id>.json.
	std::string saved_path() const;

	// One pass (bootstrapping first while it has not): the wait before the
	// next, in ms (kBusySyncIntervalMs while there is work, kSyncIntervalMs
	// otherwise, kBootstrapRetryMs before the bootstrap).
	int step();

	// Whether the last pass listed its containers and its sync was answered and
	// read.
	bool last_sync_ok() const { return last_sync_ok_; }
	// Whether the service drains this machine, as its last reply said.
	bool draining() const { return draining_; }

private:
	struct FailedStart {
		int slot = 0;
		int exit_code = 125;
		std::string reason; // reported with the exit: why ("" for docker's own failures)
	};
	// A desired container that waits for its image's pull or its trees' fetch,
	// as the reply that started it gave it (its files with it).
	struct Waiting {
		hosted::DesiredContainer desired;
		hosted::ContainerWait cause = hosted::ContainerWait::Image;
	};

	bool load_saved();
	bool save(std::string &error) const;
	void bootstrapped_now();
	// What one docker call may take before `deadline` (the agent's clock): at
	// most kDockerCallMs, 0 with less than kMinCallMs left.
	int call_ms(uint64_t deadline) const;
	void refresh_stats(const std::vector<ContainerInfo> &observed, uint64_t now, uint64_t deadline);
	// The machine's own use and the running servers' tick health, at most
	// every kStatsIntervalMs.
	void refresh_machine(const std::vector<ContainerInfo> &observed, uint64_t now);
	void refresh_counts(uint64_t now, uint64_t deadline);
	// The pulls under way: a finished one's image held, a failed or overdue
	// one's waiting containers failed.
	void poll_pulls(uint64_t now);
	hosted::SyncRequest report(const std::vector<ContainerInfo> &observed) const;
	// What every report says of the machine itself: its image, the agent's
	// log stream, its use and its trees.
	void describe_machine(hosted::SyncRequest &request) const;
	void act(
			const hosted::SyncReply &reply, const std::vector<ContainerInfo> &observed, int &delay);
	// Starts `desired` (its image pulled first when the machine lacks it);
	// false when `deadline` left no time for it (the next pass tries).
	bool start(const hosted::DesiredContainer &desired, uint64_t deadline);
	// `desired` waits for its image's pull or its trees' fetch (`cause`; `what`
	// says it for the log), its files kept here.
	void wait(const hosted::DesiredContainer &desired, hosted::ContainerWait cause,
			const std::string &what);
	// The start failed: reported exited `exit_code`, never made again, and
	// what the runtime made of it removed, its slot's files too.
	void start_failed(const hosted::DesiredContainer &desired, int exit_code,
			const std::string &why, uint64_t deadline);
	// The images no container uses removed (kImagePruneIntervalMs).
	void prune_images(const hosted::SyncReply &reply, const std::vector<ContainerInfo> &observed);
	// A warning under `key`, logged when it changes or kRepeatLogMs after it was.
	void warn(const std::string &key, const std::string &text);
	// `key`'s warning is over: `text` is logged once when there was one.
	void recovered(const std::string &key, const std::string &text);

	AgentConfig config_;
	ServiceLink &link_;
	ContainerRuntime &runtime_;
	SlotFiles &slots_;
	TreeStore &trees_;
	SlotCounters *counters_;
	Clock now_;

	bool bootstrapped_ = false;
	int bootstrap_retry_ms_ = kBootstrapRetryMs; // the wait before the next try
	bool bootstrap_refused_ = false; // the last try was refused (the backoff grows)
	hosted::MachineBootstrap machine_;
	bool counters_ready_ = false; // put in place once
	bool counters_attempted_ = false; // and when it was last tried
	uint64_t counters_attempt_at_ = 0;
	std::map<int, SlotCount> counts_; // by slot, the last good read
	std::map<std::string, ContainerStats> stats_; // by name, the last good read
	uint64_t stats_at_ = 0;
	bool stats_read_ = false;
	// The machine's use: the last CPU reading (a percent needs two), the last
	// good read of the rest, and the running servers' fresh tick health by name.
	std::optional<novaworld_server::proc_stats::CpuTimes> cpu_mark_;
	std::optional<hosted::MachineStats> machine_stats_;
	uint64_t machine_at_ = 0;
	bool machine_read_ = false;
	std::map<std::string, hosted::TickHealth> ticks_;
	std::map<std::string, uint64_t> stop_requested_; // by name: when its SIGTERM went
	std::map<std::string, FailedStart> failed_starts_; // by name
	std::map<std::string, uint64_t> pulls_; // image -> when its pull began
	std::set<std::string> images_; // the images the machine holds, as checked
	// The images the replies named, each once, in the order they were first
	// named (the newest last), and when the last prune ran.
	std::vector<std::string> named_images_;
	bool pruned_ = false;
	uint64_t pruned_at_ = 0;
	// By name: the desired containers whose image pulls or whose trees fetch.
	std::map<std::string, Waiting> waiting_;
	std::set<std::string> refused_logged_; // the desired names whose refusal was logged
	std::map<std::string, std::pair<std::string, uint64_t>> warnings_; // key -> text, when
	bool last_sync_ok_ = false;
	bool draining_ = false;
};

} // namespace opennova::hostd
