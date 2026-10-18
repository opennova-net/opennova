// opennova-hostd's agent (apps/hostd/agent.h) against an in-process fake of the service's
// two routes and a fake container runtime, on a hand-stepped clock: the bootstrap (the
// one-time token redeemed once, the reply saved owner-only in the agent's own format,
// apps/hostd/saved_machine.h, and read back by the next agent with no token, the spent
// token file gone; a service of the next protocol refusing the restarted agent's syncs
// never strands it), the sync (its envelope, the agent's own image among it; every
// container reported with its stats and its slot's counts; the machine's image, the
// agent's log stream, its use from fixture /proc files once read twice, every tree it
// holds; a running server's tick health from its fresh --stats-file, an exited one's
// --exit-file line, what a starting one waits for; the desired ones started on their
// slots with
// their files written 0600 and their environment in a file that lives for the start
// alone; the ones left out stopped, killed past the stop timeout, removed and their
// directories wiped; a slot's next container started only once its previous one is
// gone), a start that fails (reported exited 125, never retried, forgotten once dropped),
// an image pulled in the background while its containers wait (reported starting, their
// files kept; a pull that fails or overruns its bound failing them), the trees a reply
// names fetched through a fake fetcher while their containers wait the same way (a
// container started with its base and expansion trees' binds once both are present; a
// fetch that fails tried again while the container waits, the third failure failing the
// start with its reason on the report; a tree no
// reply names let go of after its grace, one still named kept), the pass's time
// shares, the refusals (no files, a slot the machine lacks), the busy cadence, the
// drain flag, and every way a sync can fail (the service down, a refusal, a reply that
// does not read or speaks another protocol, the runtime unlistable), none of which stops
// or removes anything; the files sent again to a restarted agent while its container
// waits, and the machine's serve images pruned (the ones in use and the two the replies
// named last kept). The planner (apps/hostd/reconcile.h) and the slots' directories
// (apps/hostd/slot_files.h, the files a server writes there for the agent read with no
// link followed and no FIFO waited on, a regular file within the bound alone) are checked
// on their own first; a FIFO and a link in the servers' files' places read as none in a
// pass too. Neither the secret nor a
// file's bytes reach the log. The real link (HttpServiceLink) tries each address the
// service's name lists until one takes the connection (the fleet's standby, listed beside
// the leader while it is replaced, takes none), against a test server on loopback.

#include "agent.h"
#include "reconcile.h"
#include "saved_machine.h"
#include "slot_files.h"
#include "tree_store.h"

#include <base/io/log.h>

#include "common/http_reply_server.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace hostd = opennova::hostd;
namespace hosted = opennova::novaworld_server::hosted;
namespace net = opennova::net;
namespace fs = std::filesystem;
using hosted::ContainerState;

namespace {

const std::string kToken = "TokenTokenTokenTokenTokenTokenTokenTokenTokenTok"; // 48
const std::string kSecret = "SecretSecretSecretSecretSecretSecretSecretSecret"; // 48
const std::string kImage = "ghcr.io/opennova-net/opennova-serve@sha256:00112233";
// The agent's own image, as its ECS task definition pins it.
const std::string kHostdImage = "ghcr.io/opennova-net/opennova-hostd@sha256:feedface";
const std::string kCredentials = "NOVAWORLD_USER srv-1\nNOVAWORLD_PASS hunter2hunter2\n";
constexpr int64_t kMachine = 7;

std::string g_log;
void capture_log(opennova::io::LogLevel, const char *message) {
	g_log += message;
	g_log += '\n';
}

std::string read_text(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

// --- the trees ---------------------------------------------------------------------

using Files = std::map<std::string, std::string>; // name -> bytes

void write_tree(const std::string &dir, const Files &files) {
	fs::create_directories(dir);
	for (const auto &file : files)
		std::ofstream(dir + "/" + file.first, std::ios::binary) << file.second;
}

// The manifest of `files`, as the service's reply carries it.
hosted::TreeSpec spec_of(const Files &files, const std::string &url, const std::string &version) {
	hosted::TreeSpec spec;
	spec.url = url;
	spec.version = version;
	for (const auto &file : files) {
		hosted::TreeFileSpec f;
		f.name = file.first;
		f.size = file.second.size();
		spec.files.push_back(f);
	}
	return spec;
}

const Files kBase = { { "resource.pff", "RESOURCE" }, { "localres.pff", "LOCAL" },
	{ "language.pff", "LANG" }, { "version.txt", "1.7.5.3\r\n" } };
const Files kExpansion = { { "jox01.pff", "ESCALATION" }, { "jox01L.pff", "ESC-L" },
	{ "version.txt", "1.0.0.1\r\n" } };
// Tree 1 is stock JO (on the machine from the start), 2 Escalation, 3 a JO
// tree whose fetch fails.
const std::map<int64_t, hosted::TreeSpec> kSpecs = {
	{ 1, spec_of(kBase, "s3://bucket/data/test/trees/1/", "1.7.5.3") },
	{ 2, spec_of(kExpansion, "s3://bucket/data/test/trees/2/", "1.0.0.1") },
	{ 3, spec_of(kBase, "s3://bucket/data/test/trees/3/", "1.7.5.4") },
};

// Records each start; the test ends the fetch under way with end().
class FakeFetcher : public hostd::TreeFetcher {
public:
	struct Fetch {
		std::string url, dir;
	};
	std::vector<Fetch> starts;
	bool running = false;
	int cancels = 0;

	bool start(const std::string &url, const std::string &dir, std::string &) override {
		starts.push_back({ url, dir });
		running = true;
		outcome_.reset();
		return true;
	}
	hostd::FetchPoll poll() override {
		hostd::FetchPoll p;
		if (!running) {
			p.running = false;
			p.error = "no fetch is under way";
			return p;
		}
		if (!outcome_)
			return p;
		running = false;
		p = *outcome_;
		outcome_.reset();
		return p;
	}
	void cancel() override {
		++cancels;
		running = false;
		outcome_.reset();
	}
	// The fetch under way ends with `files` fetched, or with `error`.
	void end(const Files &files) {
		write_tree(starts.back().dir, files);
		hostd::FetchPoll p;
		p.running = false;
		p.ok = true;
		outcome_ = p;
	}
	void fail(const std::string &error) {
		hostd::FetchPoll p;
		p.running = false;
		p.error = error;
		outcome_ = p;
	}

private:
	std::optional<hostd::FetchPoll> outcome_;
};

unsigned mode_of(const std::string &path) {
	struct stat st{};
	if (::lstat(path.c_str(), &st) != 0)
		return 0;
	return static_cast<unsigned>(st.st_mode & 07777);
}

net::HttpReply answer(int code, const std::string &body) {
	net::HttpReply r;
	r.transport_ok = true;
	r.code = code;
	r.body.assign(body.begin(), body.end());
	return r;
}

net::HttpReply unreachable() {
	net::HttpReply r;
	r.error = "connect to 10.44.0.10:8070 failed";
	return r;
}

hosted::DesiredContainer desired(int64_t server, int64_t generation, int slot) {
	hosted::DesiredContainer d;
	d.name = hosted::hosted_container_name(server, generation);
	d.slot = slot;
	d.image = kImage;
	d.uid = hosted::slot_uid(slot);
	d.base_tree = 1;
	d.args = { "/HOST", "server.host", "--credentials", "credentials.txt", "/game", "jo" };
	d.files = std::map<std::string, std::string>{ { "server.host", "GameName \"Caf\xE9\"\r\n" },
		{ "credentials.txt", kCredentials } };
	d.log_stream = hosted::hosted_log_stream(server, generation);
	return d;
}

// The service's two routes, in process: the token redeemed until the first sync spends
// it, the secret checked, each report decoded, the files sent until a container was
// reported once, the trees the desired containers name sent with them (kSpecs).
struct FakeService {
	hosted::MachineBootstrap machine;
	std::vector<hosted::DesiredContainer> desired;
	std::set<std::string> reported_once;
	bool token_spent = false;
	int redeems = 0;
	bool lose_bootstrap_reply = false; // the next redeem is made, its reply lost
	// The service's files rule: sent until the agent reports a container running or
	// exited (true), or the older once-reported rule the agent's own copy covers (false).
	bool files_until_running = false;
	bool drain = false;
	bool down = false;
	int refuse = 0; // a sync's status instead of 200
	std::string raw_reply; // answered to a sync as is
	hosted::SyncRequest last;
	std::vector<std::string> bodies;
	int syncs = 0;
	int bad_reports = 0;

	FakeService() {
		machine.agent_secret = kSecret;
		machine.serve_image = kImage;
		machine.slots = 4;
	}

	net::HttpReply handle(
			const std::string &path, const std::string &bearer, const std::string &body) {
		if (down)
			return unreachable();
		bodies.push_back(body);
		if (path == hosted::machine_bootstrap_path(kMachine)) {
			hosted::AgentHello hello;
			std::string error;
			if (bearer != kToken || token_spent || !hosted::decode_agent_hello(body, hello, error))
				return answer(404, "");
			++redeems;
			if (lose_bootstrap_reply) {
				lose_bootstrap_reply = false;
				return unreachable();
			}
			return answer(200, hosted::encode_machine_bootstrap(machine));
		}
		if (path != hosted::machine_sync_path(kMachine) || bearer != kSecret)
			return answer(404, "");
		token_spent = true;
		std::string error;
		if (!hosted::decode_sync_request(body, last, error)) {
			std::fprintf(stderr, "a report did not decode: %s\n", error.c_str());
			++bad_reports;
			return answer(404, "");
		}
		++syncs;
		for (const hosted::ContainerReport &c : last.containers)
			if (!files_until_running || c.state != ContainerState::Starting)
				reported_once.insert(c.name);
		if (refuse)
			return answer(refuse, "");
		if (!raw_reply.empty())
			return answer(200, raw_reply);
		hosted::SyncReply reply;
		reply.drain = drain;
		reply.desired = desired;
		for (hosted::DesiredContainer &d : reply.desired) {
			if (reported_once.count(d.name))
				d.files.reset();
			reply.trees[d.base_tree] = kSpecs.at(d.base_tree);
			if (d.expansion_tree)
				reply.trees[*d.expansion_tree] = kSpecs.at(*d.expansion_tree);
		}
		return answer(200, hosted::encode_sync_reply(reply));
	}

	const hosted::ContainerReport *reported(const std::string &name) const {
		for (const hosted::ContainerReport &c : last.containers)
			if (c.name == name)
				return &c;
		return nullptr;
	}
};

class FakeLink : public hostd::ServiceLink {
public:
	explicit FakeLink(FakeService &service) : service_(service) {}
	net::HttpReply post(
			const std::string &path, const std::string &bearer, const std::string &body) override {
		++posts;
		return service_.handle(path, bearer, body);
	}
	int posts = 0;

private:
	FakeService &service_;
};

struct Started {
	hostd::ContainerSpec spec;
	std::string env_text; // the env file's bytes at the start
	unsigned env_mode = 0;
	std::map<std::string, std::string> files; // the slot directory at the start
	unsigned dir_mode = 0;
	std::map<std::string, unsigned> file_modes;
};

class FakeRuntime : public hostd::ContainerRuntime {
public:
	std::map<std::string, hostd::ContainerInfo> containers;
	std::vector<std::string> calls; // "run <name>", "stop <name>", "pull <image>", ...
	std::vector<std::string> image_calls; // the prune's: "images <repository>", "rmi <image>"
	std::vector<Started> started;
	std::map<std::string, int> run_fails; // name -> the CLI's exit code
	std::map<std::string, hostd::ContainerStats> stat_values;
	bool list_fails = false;
	bool exit_on_stop = false; // SIGTERM ends a container at once
	bool leave_created = false; // a failed run leaves a created container behind
	int64_t now_s = 1791981296;
	// The images the machine holds, the pulls under way (each one's state as
	// the next poll reads it), the image checks made.
	std::set<std::string> images = { kImage };
	std::map<std::string, hostd::PullState> pulls;
	bool pull_refused = false; // start_pull fails
	std::vector<std::string> image_checks;
	// Every call's timeout, and what each call costs on the agent's clock.
	std::vector<int> timeouts;
	uint64_t *clock_ms = nullptr;
	uint64_t call_cost_ms = 0;

	void took(int timeout_ms) {
		timeouts.push_back(timeout_ms);
		if (clock_ms)
			*clock_ms += call_cost_ms;
	}
	bool list(std::vector<hostd::ContainerInfo> &out, int timeout_ms, std::string &error) override {
		took(timeout_ms);
		if (list_fails) {
			error = "Cannot connect to the Docker daemon";
			return false;
		}
		out.clear();
		for (const auto &c : containers)
			out.push_back(c.second);
		return true;
	}
	bool stats(const std::vector<std::string> &names,
			std::map<std::string, hostd::ContainerStats> &out, int timeout_ms,
			std::string &) override {
		took(timeout_ms);
		out.clear();
		for (const std::string &name : names)
			if (stat_values.count(name))
				out[name] = stat_values[name];
		return true;
	}
	bool has_image(
			const std::string &image, bool &present, int timeout_ms, std::string &) override {
		took(timeout_ms);
		image_checks.push_back(image);
		present = images.count(image) != 0;
		return true;
	}
	bool start_pull(const std::string &image, std::string &error) override {
		calls.push_back("pull " + image);
		if (pull_refused) {
			error = "docker pull did not start: no such file";
			return false;
		}
		pulls.emplace(image, hostd::PullState::Running);
		return true;
	}
	hostd::PullState poll_pull(const std::string &image, std::string &error) override {
		const auto it = pulls.find(image);
		if (it == pulls.end())
			return hostd::PullState::Failed;
		const hostd::PullState state = it->second;
		if (state == hostd::PullState::Running)
			return state;
		pulls.erase(it);
		if (state == hostd::PullState::Done)
			images.insert(image);
		else
			error = "docker pull exited 1: manifest unknown";
		return state;
	}
	void cancel_pull(const std::string &image) override {
		calls.push_back("cancel " + image);
		pulls.erase(image);
	}
	bool run(const hostd::ContainerSpec &spec, int timeout_ms, int &exit_code,
			std::string &error) override {
		took(timeout_ms);
		calls.push_back("run " + spec.name);
		Started s;
		s.spec = spec;
		if (!spec.env_file.empty()) {
			s.env_text = read_text(spec.env_file);
			s.env_mode = mode_of(spec.env_file);
		}
		s.dir_mode = mode_of(spec.work_dir);
		std::error_code ec;
		for (const auto &entry : fs::directory_iterator(spec.work_dir, ec)) {
			const std::string path = entry.path().string();
			s.files[entry.path().filename().string()] = read_text(path);
			s.file_modes[entry.path().filename().string()] = mode_of(path);
		}
		started.push_back(s);
		const auto fails = run_fails.find(spec.name);
		if (fails != run_fails.end()) {
			exit_code = fails->second;
			error = "docker run exited " + std::to_string(fails->second);
			if (leave_created)
				containers[spec.name] = info(spec.name, spec.slot, ContainerState::Starting);
			return false;
		}
		hostd::ContainerInfo c = info(spec.name, spec.slot, ContainerState::Running);
		c.image = spec.image;
		c.started_at = now_s;
		containers[spec.name] = c;
		return true;
	}
	bool stop(const std::string &name, int timeout_ms, std::string &) override {
		took(timeout_ms);
		calls.push_back("stop " + name);
		if (exit_on_stop)
			end(name, 0);
		return true;
	}
	bool kill(const std::string &name, int timeout_ms, std::string &) override {
		took(timeout_ms);
		calls.push_back("kill " + name);
		end(name, 137);
		return true;
	}
	bool remove(const std::string &name, int timeout_ms, std::string &error) override {
		took(timeout_ms);
		calls.push_back("remove " + name);
		const auto it = containers.find(name);
		if (it == containers.end()) {
			error = "No such container: " + name;
			return false;
		}
		if (it->second.state == ContainerState::Running) {
			error = "cannot remove a running container";
			return false;
		}
		containers.erase(it);
		return true;
	}
	bool list_images(const std::string &repository, std::vector<std::string> &out, int timeout_ms,
			std::string &) override {
		took(timeout_ms);
		image_calls.push_back("images " + repository);
		out.clear();
		for (const std::string &image : images)
			if (image.rfind(repository + "@", 0) == 0)
				out.push_back(image);
		return true;
	}
	bool remove_image(const std::string &image, int timeout_ms, std::string &error) override {
		took(timeout_ms);
		image_calls.push_back("rmi " + image);
		for (const auto &c : containers)
			if (c.second.image == image) {
				error = "conflict: container " + c.first + " is using " + image;
				return false;
			}
		images.erase(image);
		return true;
	}
	void end(const std::string &name, int code, bool oom = false) {
		auto it = containers.find(name);
		if (it == containers.end())
			return;
		it->second.state = ContainerState::Exited;
		it->second.exit_code = code;
		it->second.oom = oom;
	}
	static hostd::ContainerInfo info(const std::string &name, int slot, ContainerState state) {
		hostd::ContainerInfo c;
		c.name = name;
		c.slot = slot;
		c.state = state;
		return c;
	}
	size_t count(const std::string &call) const {
		size_t n = 0;
		for (const std::string &c : calls)
			n += c == call;
		return n;
	}
};

class FakeCounters : public hostd::SlotCounters {
public:
	std::map<int, hostd::SlotCount> values = { { 0, { 1000, 10 } }, { 1, { 2000, 20 } } };
	bool fail_read = false;
	int ensures = 0;
	bool ensure(std::string &) override {
		++ensures;
		return true;
	}
	bool read(std::map<int, hostd::SlotCount> &out, std::string &error) override {
		if (fail_read) {
			error = "the table opennova lacks a slot's counters";
			return false;
		}
		out = values;
		return true;
	}
};

// The kernel's files a machine's use is read from (core/proc_stats.h): two CPUs that ran
// `busy` of `total` ticks, 4 GiB of memory with `available` free, and two interfaces.
void write_proc(const std::string &root, uint64_t busy, uint64_t total, uint64_t available_kb,
		uint64_t net_in, uint64_t net_out) {
	fs::create_directories(root + "/proc/net");
	std::ofstream(root + "/proc/stat")
			<< "cpu  " << busy << " 0 0 " << total - busy << " 0 0 0 0 0 0\n";
	std::ofstream(root + "/proc/meminfo")
			<< "MemTotal: 4194304 kB\nMemFree: 1 kB\nMemAvailable: " << available_kb << " kB\n";
	std::ofstream(root + "/proc/net/dev")
			<< "Inter-|\n face |\n    lo: 99 1 0 0 0 0 0 0 99 1 0 0 0 0 0 0\n  ens5: " << net_in
			<< " 1 0 0 0 0 0 0 " << net_out << " 1 0 0 0 0 0 0\n";
}

// One machine: the dirs, the fakes and an agent over them. Tree 1 is on the
// machine from the start (found by the store's scan).
struct Rig {
	test_temp::TempDir temp;
	std::string state_dir, scratch_dir, slots_root, trees_root, token_file, proc_root;
	FakeService service;
	FakeLink link{ service };
	FakeRuntime runtime;
	FakeFetcher fetcher;
	std::unique_ptr<hostd::SlotFiles> slots;
	std::unique_ptr<hostd::TreeStore> trees;
	FakeCounters counters;
	uint64_t clock_ms = 1000;
	std::unique_ptr<hostd::Agent> agent;

	explicit Rig(const char *name, bool with_token = true) : temp(name) {
		state_dir = temp.file("state");
		scratch_dir = temp.file("state/scratch");
		slots_root = temp.file("slots");
		trees_root = temp.file("trees");
		token_file = temp.file("hostd.token");
		proc_root = temp.file("proc-root");
		write_proc(proc_root, 100, 1000, 3145728, 5000, 7000);
		fs::create_directories(scratch_dir);
		if (with_token)
			std::ofstream(token_file) << kToken << "\n";
		slots = std::make_unique<hostd::SlotFiles>(slots_root, false);
		std::string error;
		slots->ensure_root(error);
		write_tree(trees_root + "/1", kBase);
		std::ofstream(trees_root + "/1/.ready");
		trees = std::make_unique<hostd::TreeStore>(
				trees_root, fetcher, [this] { return clock_ms; });
		runtime.clock_ms = &clock_ms;
		make_agent();
	}

	void make_agent() {
		hostd::AgentConfig config;
		config.machine_id = kMachine;
		config.agent_version = "test-1";
		config.agent_image = kHostdImage;
		config.state_dir = state_dir;
		config.scratch_dir = scratch_dir;
		config.token_file = token_file;
		config.image_id = "ami-0123456789abcdef0";
		config.log_stream = "hostd/hostd/0123456789abcdef";
		config.proc_root = proc_root;
		agent = std::make_unique<hostd::Agent>(
				config, link, runtime, *slots, *trees, &counters, [this] { return clock_ms; });
	}

	int step() { return agent->step(); }
};

// --- the planner -------------------------------------------------------------------

int test_plan() {
	const auto tree = [](hostd::TreeFetchState state, const std::string &error = "") {
		hostd::TreeStanding s;
		s.state = state;
		s.error = error;
		return s;
	};
	std::map<int64_t, hostd::TreeStanding> trees = { { 1, tree(hostd::TreeFetchState::Present) } };
	std::map<std::string, uint64_t> stops;
	std::set<std::string> failed;
	std::vector<hosted::DesiredContainer> want;
	std::vector<hostd::ContainerInfo> have;
	hostd::PlanInput in;
	in.desired = &want;
	in.observed = &have;
	in.stop_requested_ms = &stops;
	in.failed_starts = &failed;
	in.machine_slots = 4;
	in.trees = &trees;
	in.now_ms = 100000;

	// Running and wanted: nothing to do, nothing pending.
	want = { desired(1, 1, 0) };
	have = { FakeRuntime::info("srv-1-g1", 0, ContainerState::Running) };
	hostd::Plan plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.stop.empty() && plan.kill.empty() && plan.remove.empty() &&
			plan.start.empty() && !plan.busy);
	// Starting and wanted: pending.
	have[0].state = ContainerState::Starting;
	TEST_EXPECT(hostd::plan_sync(in).busy);
	// Exited and wanted: kept, reported, for the service to decide.
	have[0].state = ContainerState::Exited;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.remove.empty() && plan.start.empty() && !plan.busy);

	// Not wanted: running is asked to stop once, killed past the timeout.
	want.clear();
	have = { FakeRuntime::info("srv-1-g1", 0, ContainerState::Running) };
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.stop == std::vector<std::string>{ "srv-1-g1" } && plan.busy);
	stops["srv-1-g1"] = in.now_ms - hostd::kStopTimeoutMs + 1;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.stop.empty() && plan.kill.empty() && plan.busy);
	stops["srv-1-g1"] = in.now_ms - hostd::kStopTimeoutMs;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.kill == std::vector<std::string>{ "srv-1-g1" });
	// Exited or never started: removed.
	have = { FakeRuntime::info("srv-1-g1", 0, ContainerState::Exited),
		FakeRuntime::info("srv-2-g1", 1, ContainerState::Starting) };
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.remove.size() == 2 && plan.stop.empty() && plan.busy);

	// A slot still held by another container, of any state: its next one waits.
	want = { desired(3, 1, 0), desired(4, 1, 2) };
	have = { FakeRuntime::info("srv-1-g1", 0, ContainerState::Exited) };
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.remove.size() == 1 && plan.start == std::vector<size_t>{ 1 } && plan.busy);
	have.clear();
	plan = hostd::plan_sync(in);
	TEST_EXPECT((plan.start == std::vector<size_t>{ 0, 1 }));

	// A failed start is not made again.
	failed = { "srv-3-g1" };
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.start == std::vector<size_t>{ 1 });
	failed.clear();

	// What cannot start here: no files, a tree the store holds nothing of, a slot past
	// the machine's.
	want = { desired(5, 1, 0), desired(6, 1, 1), desired(7, 1, 3) };
	want[0].files.reset();
	want[1].base_tree = 9;
	in.machine_slots = 3;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.start.empty() && plan.refused.size() == 3 && !plan.busy);
	TEST_EXPECT(plan.refused[1].second == "tree 9 is not on this machine");
	in.machine_slots = 4;

	// A tree still fetching (the store's standing, a failure with a try left among
	// them): its container waits for it, holding its slot and no start of the pass's
	// share, busy; a free slot's next container still starts. Once the tree is present
	// it starts; a tree the store gave up on fails the start with the last fetch's
	// reason. The expansion tree counts the same as the base.
	trees[2] = tree(hostd::TreeFetchState::Fetching);
	trees[3] = tree(hostd::TreeFetchState::Failed, "aws s3 sync exited 1: NoSuchKey");
	want = { desired(8, 1, 0), desired(9, 1, 1), desired(10, 1, 2), desired(11, 1, 3) };
	want[0].base_tree = 2;
	want[1].expansion_tree = 2;
	want[1].expansion_code = "jox01";
	want[2].base_tree = 3;
	want[3].expansion_tree = 3;
	want[3].expansion_code = "jox01";
	plan = hostd::plan_sync(in);
	TEST_EXPECT((plan.wait_for_tree == std::vector<size_t>{ 0, 1 }) && plan.start.empty());
	TEST_EXPECT(plan.data_failed.size() == 2 && plan.data_failed[0].first == 2 &&
			plan.data_failed[1].first == 3);
	TEST_EXPECT(plan.data_failed[0].second == "aws s3 sync exited 1: NoSuchKey" && plan.busy);
	want.push_back(desired(12, 1, 0)); // the slot a waiting container holds
	want.push_back(desired(13, 1, 2)); // a free slot (the failed one holds none)
	plan = hostd::plan_sync(in);
	TEST_EXPECT((plan.wait_for_tree == std::vector<size_t>{ 0, 1 }) &&
			(plan.start == std::vector<size_t>{ 5 }));
	trees[2].state = hostd::TreeFetchState::Present;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.wait_for_tree.empty() && (plan.start == std::vector<size_t>{ 0, 1, 5 }));
	// Nothing of the trees given: nothing starts.
	in.trees = nullptr;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.start.empty() && plan.refused.size() == want.size());
	in.trees = &trees;
	trees.erase(2);
	trees.erase(3);

	// At most max_starts a pass; the rest wait (busy).
	want.clear();
	for (int slot = 0; slot < 4; ++slot)
		want.push_back(desired(10 + slot, 1, slot));
	in.max_starts = 3;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.start.size() == 3 && plan.busy);

	// One whose image is pulling waits for it, holding its slot and no start of
	// the pass's share; a free slot's next container still starts.
	std::set<std::string> pulling = { "ghcr.io/opennova-net/opennova-serve@sha256:44556677" };
	in.pulling = &pulling;
	in.max_starts = 2;
	want[0].image = want[1].image = *pulling.begin();
	plan = hostd::plan_sync(in);
	TEST_EXPECT((plan.wait_for_image == std::vector<size_t>{ 0, 1 }));
	TEST_EXPECT((plan.start == std::vector<size_t>{ 2, 3 }) && plan.busy);
	want[2].slot = 0; // the slot a waiting container holds
	plan = hostd::plan_sync(in);
	TEST_EXPECT((plan.wait_for_image == std::vector<size_t>{ 0, 1 }));
	TEST_EXPECT((plan.start == std::vector<size_t>{ 3 }));
	return 0;
}

// --- the slots' directories -----------------------------------------------------------

int test_slot_files() {
	test_temp::TempDir temp("hostd_slot_files");
	hostd::SlotFiles slots(temp.file("slots"), false);
	std::string error;
	TEST_EXPECT(slots.ensure_root(error) && slots.ensure_root(error));
	TEST_EXPECT(slots.dir(2) == temp.file("slots") + "/slot-2");
	TEST_EXPECT(slots.prepare(2, hosted::slot_uid(2),
			{ { "a.cfg", "one" }, { "credentials.txt", kCredentials } }, error));
	TEST_EXPECT(mode_of(slots.dir(2)) == 0700);
	TEST_EXPECT(read_text(slots.dir(2) + "/a.cfg") == "one" &&
			mode_of(slots.dir(2) + "/a.cfg") == 0600);
	TEST_EXPECT(mode_of(slots.dir(2) + "/credentials.txt") == 0600);

	// What the previous container left goes, a link it left too, never the link's target.
	const std::string outside = temp.file("outside.txt");
	std::ofstream(outside) << "keep";
	std::ofstream(slots.dir(2) + "/admin_log.txt") << "old";
	fs::create_directory_symlink(temp.path, fs::path(slots.dir(2)) / "escape");
	TEST_EXPECT(slots.prepare(2, hosted::slot_uid(2), { { "b.cfg", "two" } }, error));
	TEST_EXPECT(
			!fs::exists(slots.dir(2) + "/admin_log.txt") && !fs::exists(slots.dir(2) + "/a.cfg"));
	TEST_EXPECT(!fs::exists(fs::symlink_status(fs::path(slots.dir(2)) / "escape")));
	TEST_EXPECT(read_text(slots.dir(2) + "/b.cfg") == "two" && read_text(outside) == "keep");
	std::ofstream(slots.dir(2) + "/c") << "x";
	fs::create_directory_symlink(temp.path, fs::path(slots.dir(2)) / "escape");
	TEST_EXPECT(slots.wipe(2, error) && !fs::exists(slots.dir(2)) && read_text(outside) == "keep");
	TEST_EXPECT(slots.wipe(2, error));

	TEST_EXPECT(!slots.prepare(2, hosted::slot_uid(2), { { "../escape", "x" } }, error));
	TEST_EXPECT(
			!slots.prepare(32, hosted::slot_uid(0), {}, error) && !slots.prepare(-1, 0, {}, error));

	// What a server writes for the agent, read as root reads what the slot's user
	// controls: a regular file within the bound, its time its own; none, a link to
	// another file, a FIFO (which never blocks the read), a directory, a sparse file
	// far past the bound and one a byte past it each read as nothing written.
	TEST_EXPECT(slots.prepare(3, hosted::slot_uid(3), {}, error));
	const std::string at = slots.dir(3);
	std::string text = "stale";
	std::chrono::system_clock::time_point written;
	TEST_EXPECT(!slots.read_written(3, hosted::kExitFileName, text, written) && text.empty());
	std::ofstream(at + "/exit.txt") << "the session ended\n";
	const auto now = std::chrono::system_clock::now();
	TEST_EXPECT(slots.read_written(3, hosted::kExitFileName, text, written) &&
			text == "the session ended\n" && written > now - std::chrono::minutes(1));
	fs::last_write_time(at + "/exit.txt", fs::file_time_type::clock::now() - std::chrono::minutes(4));
	TEST_EXPECT(slots.read_written(3, hosted::kExitFileName, text, written) &&
			written < now - std::chrono::minutes(3));
	fs::remove(at + "/exit.txt");
	fs::create_symlink(outside, at + "/exit.txt");
	TEST_EXPECT(!slots.read_written(3, hosted::kExitFileName, text, written) && text.empty());
	TEST_EXPECT(::mkfifo((at + "/" + hosted::kTickStatsFileName).c_str(), 0600) == 0);
	TEST_EXPECT(!slots.read_written(3, hosted::kTickStatsFileName, text, written));
	fs::create_directory(at + "/dir");
	TEST_EXPECT(!slots.read_written(3, "dir", text, written));
	std::ofstream(at + "/sparse");
	fs::resize_file(at + "/sparse", uintmax_t(1) << 30);
	TEST_EXPECT(!slots.read_written(3, "sparse", text, written) && text.empty());
	std::ofstream(at + "/bound") << std::string(hostd::SlotFiles::kMaxWrittenBytes, 'x');
	TEST_EXPECT(slots.read_written(3, "bound", text, written) &&
			text.size() == hostd::SlotFiles::kMaxWrittenBytes);
	std::ofstream(at + "/bound", std::ios::app) << "x";
	TEST_EXPECT(!slots.read_written(3, "bound", text, written));
	TEST_EXPECT(!slots.read_written(32, hosted::kExitFileName, text, written));
	TEST_EXPECT(read_text(outside) == "keep" && slots.wipe(3, error) && !fs::exists(at));
	return 0;
}

// --- the agent ------------------------------------------------------------------------

int test_bootstrap() {
	{
		Rig rig("hostd_agent_no_token", false);
		TEST_EXPECT(rig.step() == hostd::Agent::kBootstrapRetryMs && rig.link.posts == 0 &&
				!rig.agent->bootstrapped());
		// A file that holds no token is not sent.
		std::ofstream(rig.token_file) << "not a token\n";
		TEST_EXPECT(rig.step() == hostd::Agent::kBootstrapRetryMs && rig.link.posts == 0);
	}
	Rig rig("hostd_agent_bootstrap");
	// The service down: the token stays for the next try.
	rig.service.down = true;
	TEST_EXPECT(rig.step() == hostd::Agent::kBootstrapRetryMs && fs::exists(rig.token_file));
	rig.service.down = false;
	// The redeem made, its reply lost: the next try redeems the token again (the
	// service spends it at the first sync).
	rig.service.lose_bootstrap_reply = true;
	TEST_EXPECT(rig.step() == hostd::Agent::kBootstrapRetryMs && !rig.agent->bootstrapped());
	TEST_EXPECT(rig.service.redeems == 1 && fs::exists(rig.token_file));
	rig.step();
	TEST_EXPECT(rig.agent->bootstrapped() && rig.service.redeems == 2);
	TEST_EXPECT(rig.service.token_spent && rig.service.syncs == 1);
	TEST_EXPECT(rig.service.bodies[0].find("\"agent_version\": \"test-1\"") != std::string::npos);
	TEST_EXPECT(rig.agent->machine().slots == 4 && rig.agent->machine().agent_secret == kSecret);
	// Saved owner-only; the spent token gone.
	TEST_EXPECT(mode_of(rig.agent->saved_path()) == 0600 && !fs::exists(rig.token_file));
	TEST_EXPECT(rig.agent->saved_path() == rig.state_dir + "/machine-7.json");
	// The slots' counters are put in place at the first pass.
	TEST_EXPECT(rig.counters.ensures == 1);
	// The saved file is the agent's own: no protocol in it.
	hostd::SavedMachine saved;
	std::string error;
	TEST_EXPECT(hostd::decode_saved_machine(read_text(rig.agent->saved_path()), saved, error));
	TEST_EXPECT(saved.machine_id == kMachine && saved.agent_secret == kSecret &&
			saved.serve_image == kImage && saved.slots == 4);
	TEST_EXPECT(read_text(rig.agent->saved_path()).find("protocol") == std::string::npos);
	TEST_EXPECT(!hostd::decode_saved_machine(
			hosted::encode_machine_bootstrap(rig.service.machine), saved, error));

	// A restarted agent reads what was saved: no token, no second bootstrap.
	const int posts = rig.link.posts;
	rig.make_agent();
	TEST_EXPECT(rig.agent->bootstrap() && rig.link.posts == posts &&
			rig.agent->machine().agent_secret == kSecret);
	rig.step();
	TEST_EXPECT(rig.service.syncs == 2 && rig.agent->last_sync_ok());

	// Across a protocol bump: the service now speaks the next protocol and holds the
	// machine (the bare 404), its replies another protocol's. A restarted agent still
	// reads its saved bootstrap, never asks for the spent token again, and keeps syncing
	// with its secret, acting on nothing.
	rig.make_agent();
	rig.service.refuse = 404;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && rig.agent->bootstrapped() &&
			!rig.agent->last_sync_ok());
	rig.service.refuse = 0;
	rig.service.raw_reply = "{\"protocol\": " + std::to_string(hosted::kAgentProtocol + 1) +
			", \"drain\": false, \"desired\": [], \"trees\": {}}";
	rig.make_agent();
	rig.step();
	TEST_EXPECT(rig.agent->bootstrapped() && !rig.agent->last_sync_ok() &&
			rig.service.redeems == 2 && rig.link.posts == posts + 3);
	rig.service.raw_reply.clear();
	rig.step();
	TEST_EXPECT(rig.agent->last_sync_ok());
	// The envelope opens every sync: the protocol, the build and the agent's own image.
	hosted::AgentEnvelope envelope;
	TEST_EXPECT(hosted::decode_agent_envelope(rig.service.bodies.back(), envelope, error));
	TEST_EXPECT(envelope.protocol == hosted::kAgentProtocol && envelope.agent_version == "test-1" &&
			envelope.agent_image == kHostdImage && rig.service.last.agent_image == kHostdImage);
	// A saved file of another machine is not this one's.
	saved.machine_id = kMachine + 1;
	std::ofstream(rig.agent->saved_path(), std::ios::trunc) << hostd::encode_saved_machine(saved);
	rig.make_agent();
	TEST_EXPECT(!rig.agent->bootstrap());

	// A token the service refuses (spent): no bootstrap, the file left as it was, and
	// each try twice as late as the one before, up to five minutes.
	Rig spent("hostd_agent_spent");
	spent.service.token_spent = true;
	TEST_EXPECT(!spent.agent->bootstrap() && fs::exists(spent.token_file) &&
			!fs::exists(spent.agent->saved_path()));
	std::vector<int> waits;
	for (int i = 0; i < 7; ++i)
		waits.push_back(spent.step());
	TEST_EXPECT((waits == std::vector<int>{ 20000, 40000, 80000, 160000, 300000, 300000, 300000 }));
	TEST_EXPECT(g_log.find("the bootstrap was refused") != std::string::npos);
	// Once the service answers otherwise, the pace is the usual one again.
	spent.service.down = true;
	TEST_EXPECT(spent.step() == hostd::Agent::kBootstrapRetryMs);
	return 0;
}

int test_sync() {
	Rig rig("hostd_agent_sync");
	rig.service.desired = { desired(1, 1, 0), desired(2, 1, 1) };
	rig.service.desired[1].env = { { "A", "1" }, { "B", "two words" } };
	rig.service.desired[1].cpus = 0.5;
	rig.service.desired[1].memory_mb = 256;
	rig.service.desired[1].pids = 64;
	// The first pass: nothing reported yet, both started on their slots (busy).
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	TEST_EXPECT(rig.service.last.containers.empty() && rig.service.last.agent_version == "test-1");
	TEST_EXPECT(rig.runtime.started.size() == 2);
	const Started &a = rig.runtime.started[0];
	TEST_EXPECT(a.spec.name == "srv-1-g1" && a.spec.slot == 0 && a.spec.uid == 10001 &&
			a.spec.image == kImage);
	TEST_EXPECT(a.spec.base_dir == rig.trees_root + "/1" && a.spec.expansion_dir.empty() &&
			a.spec.expansion_code.empty() && a.spec.work_dir == rig.slots_root + "/slot-0");
	TEST_EXPECT(rig.fetcher.starts.empty());
	TEST_EXPECT(a.spec.args == rig.service.desired[0].args && a.spec.env_file.empty());
	TEST_EXPECT(a.spec.cpus == 1.0 && a.spec.memory_mb == 512 && a.spec.pids == 128);
	// Its output goes to the stream the service named.
	TEST_EXPECT(a.spec.log_stream == "1/g1" && rig.runtime.started[1].spec.log_stream == "2/g1");
	TEST_EXPECT(a.dir_mode == 0700 && a.files.size() == 2 &&
			a.files.at("credentials.txt") == kCredentials);
	TEST_EXPECT(a.file_modes.at("server.host") == 0600 &&
			a.files.at("server.host") == "GameName \"Caf\xE9\"\r\n");
	const Started &b = rig.runtime.started[1];
	TEST_EXPECT(b.spec.uid == 10002 && b.spec.cpus == 0.5 && b.spec.memory_mb == 256 &&
			b.spec.pids == 64);
	// The environment in a file of the start's own, owner-only, gone after.
	TEST_EXPECT(b.env_text == "A=1\nB=two words\n" && b.env_mode == 0600 &&
			!fs::exists(b.spec.env_file));

	// The second: both reported running with their stats and their slots' egress; the
	// files no longer sent and nothing to do.
	rig.clock_ms += 5000;
	rig.runtime.stat_values["srv-1-g1"] = { 12.5, 47710208 };
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && rig.runtime.started.size() == 2);
	const hosted::ContainerReport *ra = rig.service.reported("srv-1-g1");
	const hosted::ContainerReport *rb = rig.service.reported("srv-2-g1");
	TEST_EXPECT(ra && rb && ra->state == ContainerState::Running && ra->slot == 0 &&
			ra->started_at == 1791981296);
	TEST_EXPECT(
			ra->cpu_percent == 12.5 && ra->memory_bytes == 47710208u && ra->egress_bytes == 1000u);
	TEST_EXPECT(rb->egress_bytes == 2000u && rb->cpu_percent == 0.0 && !rb->exit_code);
	TEST_EXPECT(ra->net_in_bytes == 10u && rb->net_in_bytes == 20u &&
			ra->waiting == hosted::ContainerWait::None);
	// The machine's one tree stands present in its store, and the report says so.
	TEST_EXPECT(rig.trees->state_of(1) && rig.trees->state_of(1)->state == hostd::TreeFetchState::Present &&
			rig.trees->state_of(1)->bytes == 26u);
	TEST_EXPECT(rig.service.last.trees.size() == 1 &&
			rig.service.last.trees.at(1).state == hosted::TreeReportState::Ready &&
			rig.service.last.trees.at(1).bytes == 26u &&
			rig.service.last.trees.at(1).reason.empty());
	TEST_EXPECT(rig.service.last.image_id == "ami-0123456789abcdef0" &&
			rig.service.last.log_stream == "hostd/hostd/0123456789abcdef");
	rig.clock_ms += 5000;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && rig.runtime.calls.size() == 2);

	// Its counters no longer read: the last counts stand.
	rig.counters.fail_read = true;
	rig.counters.values = { { 0, { 5, 5 } } };
	rig.clock_ms += 5000;
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1")->egress_bytes == 1000u);
	rig.counters.fail_read = false;
	rig.counters.values = { { 0, { 1500, 15 } }, { 1, { 2500, 25 } } };
	rig.clock_ms += 5000;
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1")->egress_bytes == 1500u &&
			rig.service.reported("srv-1-g1")->net_in_bytes == 15u);

	// The service drops srv-2: asked to stop once, killed once the stop timeout passed,
	// then removed with its directory.
	rig.service.desired.pop_back();
	rig.clock_ms += 5000;
	TEST_EXPECT(
			rig.step() == hosted::kBusySyncIntervalMs && rig.runtime.count("stop srv-2-g1") == 1);
	rig.clock_ms += 30000;
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs &&
			rig.runtime.count("stop srv-2-g1") == 1 && rig.runtime.count("kill srv-2-g1") == 0);
	rig.clock_ms += 30000;
	rig.step();
	TEST_EXPECT(rig.runtime.count("kill srv-2-g1") == 1);
	TEST_EXPECT(rig.service.reported("srv-2-g1") &&
			rig.service.reported("srv-2-g1")->state == ContainerState::Running);
	TEST_EXPECT(fs::exists(rig.slots_root + "/slot-1"));
	rig.clock_ms += 1000;
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-2-g1")->state == ContainerState::Exited &&
			rig.service.reported("srv-2-g1")->exit_code == 137);
	TEST_EXPECT(
			rig.runtime.count("remove srv-2-g1") == 1 && !fs::exists(rig.slots_root + "/slot-1"));
	rig.clock_ms += 1000;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && !rig.service.reported("srv-2-g1"));
	TEST_EXPECT(fs::exists(rig.slots_root + "/slot-0"));

	// A crash: the exited container stays, reported, until the service drops it.
	rig.runtime.end("srv-1-g1", 139);
	rig.clock_ms += 5000;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && rig.runtime.count("remove srv-1-g1") == 0);
	TEST_EXPECT(rig.service.reported("srv-1-g1")->exit_code == 139 &&
			fs::exists(rig.slots_root + "/slot-0"));
	TEST_EXPECT(rig.service.bad_reports == 0);
	return 0;
}

int test_slot_handover() {
	Rig rig("hostd_agent_handover");
	rig.runtime.exit_on_stop = true;
	rig.service.desired = { desired(1, 1, 0) };
	rig.step();
	std::ofstream(rig.slots_root + "/slot-0/admin_log.txt") << "the first run's";
	rig.step();
	// The server restarts as its next generation on the same slot: the old container
	// is stopped and removed before the new one starts, on a fresh directory.
	rig.service.desired = { desired(1, 2, 0) };
	rig.step();
	TEST_EXPECT(rig.runtime.count("stop srv-1-g1") == 1 && rig.runtime.count("run srv-1-g2") == 0);
	rig.step();
	TEST_EXPECT(
			rig.runtime.count("remove srv-1-g1") == 1 && rig.runtime.count("run srv-1-g2") == 0);
	TEST_EXPECT(
			rig.step() == hosted::kBusySyncIntervalMs && rig.runtime.count("run srv-1-g2") == 1);
	const Started &next = rig.runtime.started.back();
	TEST_EXPECT(next.files.size() == 2 && !next.files.count("admin_log.txt"));
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g2") && !rig.service.reported("srv-1-g1"));
	return 0;
}

int test_failed_start() {
	Rig rig("hostd_agent_failed_start");
	rig.runtime.run_fails["srv-1-g1"] = 125;
	rig.runtime.leave_created = true;
	rig.service.desired = { desired(1, 1, 0) };
	rig.step();
	// The container the runtime made goes, and the slot's files with it.
	TEST_EXPECT(
			rig.runtime.count("run srv-1-g1") == 1 && rig.runtime.count("remove srv-1-g1") == 1);
	TEST_EXPECT(rig.runtime.containers.empty() && !fs::exists(rig.slots_root + "/slot-0"));
	// Reported exited 125, never started again.
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs);
	const hosted::ContainerReport *r = rig.service.reported("srv-1-g1");
	TEST_EXPECT(r && r->state == ContainerState::Exited && r->exit_code == 125 && !r->started_at &&
			r->slot == 0 && r->reason.empty());
	rig.step();
	TEST_EXPECT(rig.runtime.count("run srv-1-g1") == 1);
	// The service restarts it as its next generation: that one starts; the failure is forgotten.
	rig.service.desired = { desired(1, 2, 0) };
	rig.step();
	TEST_EXPECT(rig.runtime.count("run srv-1-g2") == 1);
	rig.step();
	TEST_EXPECT(!rig.service.reported("srv-1-g1") && rig.service.reported("srv-1-g2"));
	return 0;
}

// An image the machine lacks is pulled in the background: the container waits,
// reported starting, while the passes go on at the busy cadence; once the pull is
// done it starts with the files its first reply carried (the later ones leave
// them out). Two containers of one image share its pull. A pull that fails, or
// runs past its bound, is the start's failure (exited 125, never retried); a
// pull that did not even start fails the start at once.
int test_pull() {
	const std::string newer = "ghcr.io/opennova-net/opennova-serve@sha256:44556677";
	Rig rig("hostd_agent_pull");
	rig.service.desired = { desired(1, 1, 0), desired(2, 1, 1) };
	for (hosted::DesiredContainer &d : rig.service.desired)
		d.image = newer;
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	TEST_EXPECT(rig.runtime.count("pull " + newer) == 1 && rig.runtime.count("run srv-1-g1") == 0);
	TEST_EXPECT(rig.runtime.image_checks == std::vector<std::string>{ newer });
	// Reported starting on their slots; the service stops sending the files.
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	const hosted::ContainerReport *a = rig.service.reported("srv-1-g1");
	const hosted::ContainerReport *b = rig.service.reported("srv-2-g1");
	TEST_EXPECT(a && b && a->state == ContainerState::Starting && a->slot == 0 && b->slot == 1);
	TEST_EXPECT(!a->exit_code && !a->started_at && a->waiting == hosted::ContainerWait::Image &&
			b->waiting == hosted::ContainerWait::Image);
	rig.clock_ms += 60 * 1000;
	rig.step();
	TEST_EXPECT(rig.runtime.count("pull " + newer) == 1 && rig.runtime.started.empty());
	// Pulled: both start, with their files.
	rig.runtime.pulls[newer] = hostd::PullState::Done;
	rig.clock_ms += 1000;
	rig.step();
	TEST_EXPECT(rig.runtime.started.size() == 2 && rig.runtime.started[0].spec.image == newer);
	TEST_EXPECT(rig.runtime.started[0].files.at("credentials.txt") == kCredentials);
	TEST_EXPECT(rig.runtime.image_checks.size() == 1);
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1")->state == ContainerState::Running);
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs);

	// A pull that fails: exited 125, never started; the next generation pulls again.
	const std::string broken = "ghcr.io/opennova-net/opennova-serve@sha256:deadbeef";
	rig.service.desired = { desired(3, 1, 2) };
	rig.service.desired[0].image = broken;
	rig.step();
	rig.runtime.pulls[broken] = hostd::PullState::Failed;
	rig.step();
	rig.step();
	const hosted::ContainerReport *c = rig.service.reported("srv-3-g1");
	TEST_EXPECT(c && c->state == ContainerState::Exited && c->exit_code == 125 && c->slot == 2);
	TEST_EXPECT(rig.runtime.count("run srv-3-g1") == 0 && rig.runtime.count("pull " + broken) == 1);
	rig.service.desired = { desired(3, 2, 2) };
	rig.service.desired[0].image = broken;
	rig.step();
	TEST_EXPECT(rig.runtime.count("pull " + broken) == 2);
	// Still running at its bound: ended, a failure.
	rig.clock_ms += hostd::Agent::kPullTimeoutMs;
	rig.step();
	rig.step();
	TEST_EXPECT(rig.runtime.count("cancel " + broken) == 1);
	const hosted::ContainerReport *d = rig.service.reported("srv-3-g2");
	TEST_EXPECT(d && d->state == ContainerState::Exited && d->exit_code == 125);
	TEST_EXPECT(rig.runtime.count("run srv-3-g2") == 0);

	// A pull that does not start fails the start at once.
	rig.runtime.pull_refused = true;
	rig.service.desired = { desired(4, 1, 3) };
	rig.service.desired[0].image = "ghcr.io/opennova-net/opennova-serve@sha256:0badf00d";
	rig.step();
	rig.step();
	const hosted::ContainerReport *e = rig.service.reported("srv-4-g1");
	TEST_EXPECT(e && e->state == ContainerState::Exited && e->exit_code == 125);
	TEST_EXPECT(rig.service.bad_reports == 0);
	return 0;
}

// An agent replaced while its container waits for its image (the fleet's daemon rolled
// mid-pull): the service sends the files until the container is reported running or
// exited, so the new agent, which kept none, still starts it.
int test_files_while_waiting() {
	const std::string newer = "ghcr.io/opennova-net/opennova-serve@sha256:44556677";
	Rig rig("hostd_agent_files_waiting");
	rig.service.files_until_running = true;
	rig.service.desired = { desired(1, 1, 0) };
	rig.service.desired[0].image = newer;
	rig.step();
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1")->state == ContainerState::Starting);
	// Replaced: the new agent holds no files of its own, the pull under way lost.
	rig.make_agent();
	rig.runtime.pulls.clear();
	rig.step();
	TEST_EXPECT(rig.runtime.count("pull " + newer) == 2);
	rig.runtime.pulls[newer] = hostd::PullState::Done;
	rig.step();
	TEST_EXPECT(rig.runtime.started.size() == 1 &&
			rig.runtime.started[0].files.at("credentials.txt") == kCredentials);
	TEST_EXPECT(g_log.find("srv-1-g1 cannot start here") == std::string::npos);
	return 0;
}

// The machine's serve images: at a pass with no work, at most every
// kImagePruneIntervalMs, each image of the repositories the replies named that no
// container uses goes, but the two the replies named last (current and previous).
int test_image_prune() {
	const std::string repo = "ghcr.io/opennova-net/opennova-serve";
	const std::string old1 = repo + "@sha256:0001";
	const std::string old2 = repo + "@sha256:0002";
	const std::string next = repo + "@sha256:0003";
	const std::string third = repo + "@sha256:0004";
	const std::string other = "ghcr.io/opennova-net/opennova-hostd@sha256:feedface";
	Rig rig("hostd_agent_prune");
	rig.runtime.exit_on_stop = true;
	const auto pruned = [&rig](const std::string &call) {
		return std::count(rig.runtime.image_calls.begin(), rig.runtime.image_calls.end(), call);
	};
	// The passes until one has no work.
	const auto settle = [&rig] {
		for (int i = 0; i < 8 && rig.step() == hosted::kBusySyncIntervalMs; ++i)
			rig.clock_ms += 1000;
	};
	rig.runtime.images = { kImage, old1, old2, other };
	rig.service.desired = { desired(1, 1, 0), desired(2, 1, 1) };
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs); // the starts
	TEST_EXPECT(pruned("images " + repo) == 0);
	rig.step(); // no work: the first prune
	TEST_EXPECT(pruned("images " + repo) == 1);
	TEST_EXPECT(pruned("rmi " + old1) == 1 && pruned("rmi " + old2) == 1);
	TEST_EXPECT(pruned("rmi " + kImage) == 0 && pruned("rmi " + other) == 0);
	TEST_EXPECT(rig.runtime.images == (std::set<std::string>{ kImage, other }));
	// Not again before its interval.
	rig.step();
	TEST_EXPECT(pruned("images " + repo) == 1);
	// A new release: the server on slot 0 starts again on the next image, the one on
	// slot 1 ends. kImage, the previous, stays while it is among the two named last.
	rig.runtime.images.insert(next);
	rig.service.desired = { desired(1, 2, 0) };
	rig.service.desired[0].image = next;
	settle();
	TEST_EXPECT(rig.runtime.containers.count("srv-1-g2") && rig.runtime.containers.size() == 1);
	rig.clock_ms += hostd::Agent::kImagePruneIntervalMs;
	settle();
	TEST_EXPECT(pruned("images " + repo) == 2 && rig.runtime.images.count(kImage));
	// A third: kImage is no longer among the two named last, and goes.
	rig.runtime.images.insert(third);
	rig.service.desired = { desired(1, 3, 0) };
	rig.service.desired[0].image = third;
	settle();
	rig.clock_ms += hostd::Agent::kImagePruneIntervalMs;
	settle();
	TEST_EXPECT(pruned("images " + repo) == 3 && pruned("rmi " + kImage) == 1);
	TEST_EXPECT(rig.runtime.images == (std::set<std::string>{ next, third, other }));
	return 0;
}

// No pass waits long: every docker call is cut at kDockerCallMs, the listing
// gets the report's time, and the acts stop once the act's time is spent (each
// call here costs 6 s of the agent's clock): what they did not reach is done at
// the next pass, which comes at the busy cadence.
int test_pass_budget() {
	Rig rig("hostd_agent_budget");
	rig.runtime.call_cost_ms = 6000;
	rig.service.desired = { desired(1, 1, 0), desired(2, 1, 1), desired(3, 1, 2) };
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	// The listing gets the report's 15 s and the stats what is left of them; the act's
	// 10 s hold the image check and one start (its run given the 4 s left), not a second.
	TEST_EXPECT((rig.runtime.timeouts == std::vector<int>{ 15000, 9000, 10000, 4000 }));
	TEST_EXPECT(rig.runtime.count("run srv-1-g1") == 1 && rig.runtime.count("run srv-2-g1") == 0);
	// The next pass starts the other two.
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	TEST_EXPECT(rig.runtime.count("run srv-1-g1") == 1 && rig.runtime.count("run srv-2-g1") == 1 &&
			rig.runtime.count("run srv-3-g1") == 1);
	for (const int t : rig.runtime.timeouts)
		TEST_EXPECT(t >= static_cast<int>(hostd::Agent::kMinCallMs) &&
				t <= static_cast<int>(hostd::Agent::kReportBudgetMs));
	return 0;
}

int test_refusals() {
	Rig rig("hostd_agent_refusals");
	rig.step();
	// Files omitted (the service believes it was reported), a slot past its four:
	// neither starts, neither is reported, nothing is busy.
	rig.service.desired = { desired(1, 1, 0), desired(3, 1, 3), desired(4, 1, 5) };
	rig.service.reported_once.insert("srv-1-g1");
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	TEST_EXPECT(rig.runtime.count("run srv-1-g1") == 0 && rig.runtime.count("run srv-3-g1") == 1 &&
			rig.runtime.count("run srv-4-g1") == 0);
	rig.step();
	TEST_EXPECT(!rig.service.reported("srv-1-g1") && !rig.service.reported("srv-4-g1"));
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs);
	return 0;
}

// A tree the machine lacks is fetched in the background: the container waits for it,
// reported starting at the busy cadence, and starts with both its trees' binds once
// they are present (the expansion's mount point made under the base tree first). A
// fetch that fails is tried again a minute later, the container still waiting, and
// the third failure is the start's failure (exited 125, the fetch's reason logged). A
// tree no reply names goes after its grace; one still named stays.
int test_trees() {
	Rig rig("hostd_agent_trees");
	rig.service.desired = { desired(1, 1, 0) };
	rig.service.desired[0].expansion_tree = 2;
	rig.service.desired[0].expansion_code = "jox01";
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	TEST_EXPECT(rig.fetcher.starts.size() == 1 && rig.runtime.count("run srv-1-g1") == 0);
	TEST_EXPECT(rig.fetcher.starts[0].url == "s3://bucket/data/test/trees/2/" &&
			rig.fetcher.starts[0].dir == rig.trees_root + "/2.partial");
	// Reported starting on its slot, the tree fetching in the store.
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	const hosted::ContainerReport *a = rig.service.reported("srv-1-g1");
	TEST_EXPECT(a && a->state == ContainerState::Starting && a->slot == 0 && !a->exit_code &&
			a->waiting == hosted::ContainerWait::Trees);
	TEST_EXPECT(rig.trees->state_of(1) && rig.trees->state_of(2) &&
			rig.trees->state_of(2)->state == hostd::TreeFetchState::Fetching);
	// The report names the fetch, its bytes so far and its try.
	TEST_EXPECT(rig.service.last.trees.count(2) == 1 &&
			rig.service.last.trees.at(2).state == hosted::TreeReportState::Fetching &&
			rig.service.last.trees.at(2).tries == 1 && rig.service.last.trees.at(2).bytes == 0u);
	std::ofstream(rig.trees_root + "/2.partial/jox01.pff") << "ESCAL";
	rig.step();
	TEST_EXPECT(rig.service.last.trees.at(2).bytes == 5u);
	TEST_EXPECT(g_log.find("srv-1-g1 waits for its data (tree 1 and tree 2) to fetch") !=
			std::string::npos);
	// Fetched: checked, and the container starts with both binds.
	rig.fetcher.end(kExpansion);
	rig.clock_ms += 1000;
	rig.step();
	TEST_EXPECT(rig.runtime.count("run srv-1-g1") == 1);
	const Started &s = rig.runtime.started.back();
	TEST_EXPECT(s.spec.base_dir == rig.trees_root + "/1" &&
			s.spec.expansion_dir == rig.trees_root + "/2" && s.spec.expansion_code == "jox01");
	TEST_EXPECT(fs::is_directory(rig.trees_root + "/1/expansion/jox01"));
	TEST_EXPECT(fs::is_regular_file(rig.trees_root + "/2/.ready") &&
			s.files.at("credentials.txt") == kCredentials);
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1")->state == ContainerState::Running);
	TEST_EXPECT(rig.trees->state_of(2)->state == hostd::TreeFetchState::Present &&
			rig.trees->state_of(2)->bytes == 24u);
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs);

	// A fetch that fails: the store holds the tree failed with the reason at once,
	// but the container keeps waiting (starting, busy) through the store's retries,
	// a minute apart, and only the third failure fails its start: never run, exited
	// 125 with the last reason on its report.
	rig.service.desired.push_back(desired(2, 1, 1));
	rig.service.desired[1].base_tree = 3;
	rig.step();
	TEST_EXPECT(rig.fetcher.starts.size() == 2 &&
			rig.fetcher.starts[1].url.find("/trees/3/") != std::string::npos);
	rig.fetcher.fail("aws s3 sync exited 1: fatal error: An error occurred (NoSuchKey)");
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	rig.step();
	const hosted::ContainerReport *b = rig.service.reported("srv-2-g1");
	TEST_EXPECT(b && b->state == ContainerState::Starting && b->slot == 1 && !b->exit_code &&
			b->reason.empty());
	// A failure with a try left is still a fetch, its reason riding along.
	TEST_EXPECT(rig.service.last.trees.at(3).state == hosted::TreeReportState::Fetching &&
			rig.service.last.trees.at(3).tries == 1 &&
			rig.service.last.trees.at(3).reason ==
					"aws s3 sync exited 1: fatal error: An error occurred (NoSuchKey)");
	TEST_EXPECT(rig.trees->state_of(3) &&
			rig.trees->state_of(3)->state == hostd::TreeFetchState::Failed &&
			rig.trees->state_of(3)->error ==
					"aws s3 sync exited 1: fatal error: An error occurred (NoSuchKey)");
	TEST_EXPECT(g_log.find("its data could not be fetched") == std::string::npos);
	// Not before the minute; then the second try, and the third.
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs - 1;
	rig.step();
	TEST_EXPECT(rig.fetcher.starts.size() == 2);
	rig.clock_ms += 1;
	rig.step();
	TEST_EXPECT(rig.fetcher.starts.size() == 3 && rig.service.reported("srv-2-g1") &&
			rig.service.reported("srv-2-g1")->state == ContainerState::Starting);
	rig.fetcher.fail("aws s3 sync exited 1: fatal error: An error occurred (NoSuchKey)");
	rig.step();
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs;
	rig.step();
	TEST_EXPECT(rig.fetcher.starts.size() == 4 && rig.runtime.count("run srv-2-g1") == 0);
	rig.fetcher.fail("aws s3 sync exited 1: fatal error: An error occurred (NoSuchKey) again");
	rig.step();
	rig.step();
	b = rig.service.reported("srv-2-g1");
	TEST_EXPECT(b && b->state == ContainerState::Exited && b->exit_code == 125 && b->slot == 1);
	TEST_EXPECT(b && !b->started_at &&
			b->reason ==
					"its data could not be fetched: aws s3 sync exited 1: fatal error: An error "
					"occurred (NoSuchKey) again");
	TEST_EXPECT(rig.runtime.count("run srv-2-g1") == 0 && rig.fetcher.starts.size() == 4);
	TEST_EXPECT(rig.service.last.trees.at(3).state == hosted::TreeReportState::Failed &&
			rig.service.last.trees.at(3).tries == 3);
	TEST_EXPECT(rig.trees->state_of(3)->error ==
			"aws s3 sync exited 1: fatal error: An error occurred (NoSuchKey) again");
	TEST_EXPECT(
			g_log.find("srv-2-g1 did not start on slot 1: its data could not be fetched: "
					   "aws s3 sync exited 1: fatal error: An error occurred (NoSuchKey) again") !=
			std::string::npos);
	// Spent, the tree stays failed: no fourth try.
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs * 5;
	rig.step();
	TEST_EXPECT(rig.fetcher.starts.size() == 4);
	// Dropped by the service, the failed tree is forgotten at once, and so is the
	// failed start.
	rig.service.desired.pop_back();
	rig.step();
	rig.step();
	TEST_EXPECT(!rig.trees->state_of(3) && rig.trees->standings().size() == 2);
	TEST_EXPECT(!rig.service.reported("srv-2-g1"));

	// The server moves on to stock JO alone: tree 2 goes unnamed, and after its grace
	// goes; tree 1, still named, stays.
	rig.runtime.exit_on_stop = true;
	rig.service.desired = { desired(1, 2, 0) };
	rig.step();
	rig.step();
	rig.step();
	TEST_EXPECT(rig.runtime.count("run srv-1-g2") == 1 && rig.trees->standings().size() == 2);
	rig.clock_ms += hostd::TreeStore::kUnnamedGraceMs - 1;
	rig.step();
	TEST_EXPECT(fs::is_directory(rig.trees_root + "/2"));
	rig.clock_ms += 1;
	rig.step();
	TEST_EXPECT(!fs::exists(rig.trees_root + "/2") && fs::is_directory(rig.trees_root + "/1"));
	rig.step();
	TEST_EXPECT(rig.trees->standings().size() == 1 && rig.trees->state_of(1));
	TEST_EXPECT(rig.service.bad_reports == 0);
	return 0;
}

// What the reports say of the machine and of each server beyond the runtime's view: the
// machine's use once read twice (the CPU busy between the readings), a running server's
// tick health while its --stats-file is fresh, an exited one's --exit-file line.
int test_machine_report() {
	Rig rig("hostd_agent_machine");
	rig.service.desired = { desired(1, 1, 0), desired(2, 1, 1) };
	rig.step();
	// One reading: no percent yet, so no machine in the report.
	TEST_EXPECT(!rig.service.last.machine);
	rig.clock_ms += hostd::Agent::kStatsIntervalMs;
	write_proc(rig.proc_root, 1100, 3000, 2097152, 9000, 15000);
	rig.step();
	const std::optional<hosted::MachineStats> m = rig.service.last.machine;
	TEST_EXPECT(m && m->cpu_percent == 50.0 && m->memory_total_bytes == 4294967296ull &&
			m->memory_used_bytes == 2147483648ull && m->net_in_bytes == 9000u &&
			m->net_out_bytes == 15000u);
	TEST_EXPECT(m && m->disk_total_bytes > 0 && m->disk_free_bytes <= m->disk_total_bytes);
	// Read again only kStatsIntervalMs on: the last stands between.
	write_proc(rig.proc_root, 3000, 4000, 2097152, 9500, 15500);
	rig.clock_ms += 1000;
	rig.step();
	TEST_EXPECT(rig.service.last.machine && rig.service.last.machine->net_in_bytes == 9000u);
	// A file that no longer reads keeps the last good read, and warns once.
	fs::remove(rig.proc_root + "/proc/meminfo");
	rig.clock_ms += hostd::Agent::kStatsIntervalMs;
	rig.step();
	TEST_EXPECT(rig.service.last.machine && rig.service.last.machine->net_in_bytes == 9000u);
	TEST_EXPECT(g_log.find("the machine's use does not read") != std::string::npos);

	// The servers' tick health, written in their slots' directories.
	std::ofstream(rig.slots_root + "/slot-0/tick-stats.json")
			<< "{\"at\":1792000000,\"p50_ms\":1.25,\"p99_ms\":15.50,\"max_ms\":31.75,"
			   "\"late\":4,\"frames\":3750}\n";
	std::ofstream(rig.slots_root + "/slot-1/tick-stats.json") << "not json\n";
	rig.clock_ms += hostd::Agent::kStatsIntervalMs;
	rig.step();
	const hosted::ContainerReport *a = rig.service.reported("srv-1-g1");
	const hosted::ContainerReport *b = rig.service.reported("srv-2-g1");
	TEST_EXPECT(a && a->tick && a->tick->p50_ms == 1.25 && a->tick->p99_ms == 15.5 &&
			a->tick->max_ms == 31.75 && a->tick->late == 4u);
	TEST_EXPECT(b && !b->tick);
	// A file left minutes behind (a server stuck) says nothing.
	fs::last_write_time(rig.slots_root + "/slot-0/tick-stats.json",
			fs::file_time_type::clock::now() - std::chrono::minutes(4));
	rig.clock_ms += hostd::Agent::kStatsIntervalMs;
	rig.step();
	TEST_EXPECT(!rig.service.reported("srv-1-g1")->tick);
	// A FIFO in its place (the slot's user may make one) neither holds the pass up
	// nor reads.
	fs::remove(rig.slots_root + "/slot-0/tick-stats.json");
	TEST_EXPECT(::mkfifo((rig.slots_root + "/slot-0/tick-stats.json").c_str(), 0600) == 0);
	rig.clock_ms += hostd::Agent::kStatsIntervalMs;
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1") && !rig.service.reported("srv-1-g1")->tick);
	fs::remove(rig.slots_root + "/slot-0/tick-stats.json");
	TEST_EXPECT(
			hostd::parse_tick_stats("{\"p50_ms\":1,\"p99_ms\":2,\"max_ms\":3}") == std::nullopt);
	TEST_EXPECT(hostd::parse_tick_stats("{\"p50_ms\":-1,\"p99_ms\":2,\"max_ms\":3,\"late\":0}") ==
			std::nullopt);

	// An exit: its --exit-file line rides the report; one that exited 0 wrote none,
	// and a link in the file's place (to a file root may read) gives no line.
	std::ofstream(rig.slots_root + "/slot-0/exit.txt")
			<< "the session ended: the NovaWorld service stopped the hosting\n";
	const std::string root_only = rig.temp.file("root-only.txt");
	std::ofstream(root_only) << "a line of root's\n";
	rig.runtime.end("srv-1-g1", 3);
	rig.runtime.end("srv-2-g1", 0);
	rig.step();
	a = rig.service.reported("srv-1-g1");
	b = rig.service.reported("srv-2-g1");
	TEST_EXPECT(a && a->state == ContainerState::Exited && a->exit_code == 3 && !a->tick &&
			a->reason == "the session ended: the NovaWorld service stopped the hosting");
	TEST_EXPECT(b && b->exit_code == 0 && b->reason.empty());
	fs::remove(rig.slots_root + "/slot-0/exit.txt");
	fs::create_symlink(root_only, rig.slots_root + "/slot-0/exit.txt");
	rig.step();
	a = rig.service.reported("srv-1-g1");
	TEST_EXPECT(a && a->exit_code == 3 && a->reason.empty());
	TEST_EXPECT(rig.service.bad_reports == 0);
	return 0;
}

int test_sync_failures() {
	Rig rig("hostd_agent_failures");
	rig.service.desired = { desired(1, 1, 0), desired(2, 1, 1) };
	rig.step();
	rig.step();
	TEST_EXPECT(rig.agent->last_sync_ok() && rig.runtime.containers.size() == 2);
	const size_t calls = rig.runtime.calls.size();
	// Whatever goes wrong, the containers keep running as they are.
	rig.service.desired.clear();
	rig.service.down = true;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && !rig.agent->last_sync_ok());
	rig.service.down = false;
	rig.service.refuse = 404;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && !rig.agent->last_sync_ok());
	rig.service.refuse = 503;
	rig.step();
	rig.service.refuse = 0;
	rig.service.raw_reply = "{\"protocol\": 2, \"desired\": [], \"drain\": true}";
	rig.step();
	TEST_EXPECT(!rig.agent->last_sync_ok() && !rig.agent->draining());
	rig.service.raw_reply = "<html>bad gateway</html>";
	rig.step();
	rig.service.raw_reply = "{\"protocol\": 1, \"desired\": [{\"name\": \"x\"}], \"drain\": false}";
	rig.step();
	TEST_EXPECT(rig.runtime.calls.size() == calls && rig.runtime.containers.size() == 2);
	// The runtime unlistable: a sync that says so, nothing reported or done, and the
	// reply (here one that wants nothing) not acted on.
	rig.service.raw_reply.clear();
	rig.runtime.list_fails = true;
	const int syncs = rig.service.syncs;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && rig.service.syncs == syncs + 1 &&
			!rig.agent->last_sync_ok());
	TEST_EXPECT(rig.service.last.listing_failed && rig.service.last.containers.empty());
	TEST_EXPECT(rig.runtime.calls.size() == calls && rig.runtime.containers.size() == 2);
	rig.runtime.list_fails = false;
	// The service back, draining and wanting nothing: now they stop.
	rig.service.drain = true;
	rig.step();
	TEST_EXPECT(!rig.service.last.listing_failed && rig.service.last.containers.size() == 2);
	TEST_EXPECT(rig.agent->last_sync_ok() && rig.agent->draining());
	TEST_EXPECT(rig.runtime.count("stop srv-1-g1") == 1 && rig.runtime.count("stop srv-2-g1") == 1);
	rig.service.drain = false;
	rig.step();
	TEST_EXPECT(!rig.agent->draining());
	return 0;
}

int test_log_hygiene() {
	// Everything the runs above logged: never the secret, the token or a file's bytes.
	TEST_EXPECT(!g_log.empty());
	TEST_EXPECT(
			g_log.find(kSecret) == std::string::npos && g_log.find(kToken) == std::string::npos);
	TEST_EXPECT(g_log.find("hunter2") == std::string::npos &&
			g_log.find("GameName") == std::string::npos);
	TEST_EXPECT(g_log.find("srv-1-g1") != std::string::npos);
	return 0;
}

} // namespace

// The real link against the service on 127.0.0.1 alone, reached by a name whose addresses
// the test lists: an address nothing listens on (127.0.0.2, as the fleet's standby is
// while it is replaced) is passed over for the next, which answers, and the request reaches
// the service once; with no address taking the connection the call fails at the connect,
// and with none at all it cannot reach the name.
int test_link_addresses() {
	test_http::ReplyServer service(200, "{}");
	const auto listing = [](std::vector<std::string> quads) {
		return [quads](const std::string &host) {
			std::vector<net::Endpoint> out;
			for (const std::string &quad : quads) {
				net::Endpoint ep;
				if (host == "fleet.test" && net::resolve_ipv4(quad, ep, false))
					out.push_back(ep);
			}
			return out;
		};
	};
	const std::string url = "http://fleet.test:" + std::to_string(service.port());
	{
		hostd::HttpServiceLink link(url, 2000, listing({ "127.0.0.2", "127.0.0.1" }));
		const net::HttpReply reply = link.post("/internal/agent/sync", kToken, "{}");
		TEST_EXPECT(reply.transport_ok && reply.connected && reply.code == 200);
		TEST_EXPECT(service.requests() == 1);
		TEST_EXPECT(service.last_line() == "POST /internal/agent/sync HTTP/1.0");
	}
	{
		hostd::HttpServiceLink link(url, 2000, listing({ "127.0.0.2", "127.0.0.3" }));
		const net::HttpReply reply = link.post("/internal/agent/sync", kToken, "{}");
		TEST_EXPECT(!reply.transport_ok && !reply.connected);
		TEST_EXPECT(reply.error.find("connect to 127.0.0.3:") == 0);
		TEST_EXPECT(service.requests() == 1);
	}
	{
		hostd::HttpServiceLink link(url, 2000, listing({}));
		const net::HttpReply reply = link.post("/internal/agent/sync", kToken, "{}");
		TEST_EXPECT(!reply.transport_ok && reply.error == "cannot reach fleet.test");
	}
	// The real resolver: a dotted quad is its one address.
	const std::vector<net::Endpoint> quad = net::resolve_ipv4_all("127.0.0.1");
	TEST_EXPECT(quad.size() == 1 && quad[0].ip == (std::array<uint8_t, 4>{ 127, 0, 0, 1 }));
	return 0;
}

int main() {
	opennova::io::set_log_sink(capture_log);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "plan", test_plan },
		{ "slot_files", test_slot_files },
		{ "bootstrap", test_bootstrap },
		{ "sync", test_sync },
		{ "slot_handover", test_slot_handover },
		{ "failed_start", test_failed_start },
		{ "pull", test_pull },
		{ "files_while_waiting", test_files_while_waiting },
		{ "image_prune", test_image_prune },
		{ "pass_budget", test_pass_budget },
		{ "refusals", test_refusals },
		{ "trees", test_trees },
		{ "machine_report", test_machine_report },
		{ "sync_failures", test_sync_failures },
		{ "log_hygiene", test_log_hygiene },
		{ "link_addresses", test_link_addresses },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n%s", c.name, g_log.c_str());
			return 1;
		}
	}
	std::printf("OK: opennova-hostd agent\n");
	return 0;
}
