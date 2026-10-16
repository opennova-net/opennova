// opennova-hostd's agent (apps/hostd/agent.h) against an in-process fake of the service's
// two routes and a fake container runtime, on a hand-stepped clock: the bootstrap (the
// one-time token redeemed once, the reply saved owner-only and read back by the next
// agent with no token, the spent token file gone), the sync (every container reported
// with its stats and its slot's egress; the desired ones started on their slots with
// their files written 0600 and their environment in a file that lives for the start
// alone; the ones left out stopped, killed past the stop timeout, removed and their
// directories wiped; a slot's next container started only once its previous one is
// gone), a start that fails (reported exited 125, never retried, forgotten once dropped),
// an image pulled in the background while its containers wait (reported starting, their
// files kept; a pull that fails or overruns its bound failing them), the pass's time
// shares, the refusals (no files, a data set or slot the machine lacks), the busy cadence, the
// drain flag, and every way a sync can fail (the service down, a refusal, a reply that
// does not read or speaks another protocol, the runtime unlistable), none of which stops
// or removes anything. The planner (apps/hostd/reconcile.h) and the slots' directories
// (apps/hostd/slot_files.h) are checked on their own first. Neither the secret nor a
// file's bytes reach the log. The real link (HttpServiceLink) tries each address the
// service's name lists until one takes the connection (a controller's standby, listed beside
// the leader while it is replaced, takes none), against a test server on loopback.

#include "agent.h"
#include "reconcile.h"
#include "slot_files.h"

#include <base/io/log.h>

#include "common/http_reply_server.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <array>
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
	d.data_set = "jop_2_consumer";
	d.args = { "/HOST", "server.host", "--credentials", "credentials.txt", "/game", "jo" };
	d.files = std::map<std::string, std::string>{ { "server.host", "GameName \"Caf\xE9\"\r\n" },
		{ "credentials.txt", kCredentials } };
	return d;
}

// The service's two routes, in process: the token redeemed until the first sync spends
// it, the secret checked, each report decoded, the files sent until a container was
// reported once.
struct FakeService {
	hosted::MachineBootstrap machine;
	std::vector<hosted::DesiredContainer> desired;
	std::set<std::string> reported_once;
	bool token_spent = false;
	int redeems = 0;
	bool lose_bootstrap_reply = false; // the next redeem is made, its reply lost
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
		machine.data_sets = { { "jop_2_consumer", "/opt/game/jop_2_consumer" } };
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
			reported_once.insert(c.name);
		if (refuse)
			return answer(refuse, "");
		if (!raw_reply.empty())
			return answer(200, raw_reply);
		hosted::SyncReply reply;
		reply.drain = drain;
		reply.desired = desired;
		for (hosted::DesiredContainer &d : reply.desired)
			if (reported_once.count(d.name))
				d.files.reset();
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

class FakeEgress : public hostd::EgressSource {
public:
	std::map<int, uint64_t> values;
	bool fail_read = false;
	int ensures = 0;
	bool ensure(std::string &) override {
		++ensures;
		return true;
	}
	bool read(std::map<int, uint64_t> &out, std::string &error) override {
		if (fail_read) {
			error = "OPENNOVA-EGRESS lacks slot 0's rule";
			return false;
		}
		out = values;
		return true;
	}
};

// One machine: the dirs, the fakes and an agent over them.
struct Rig {
	test_temp::TempDir temp;
	std::string state_dir, scratch_dir, slots_root, token_file;
	FakeService service;
	FakeLink link{ service };
	FakeRuntime runtime;
	std::unique_ptr<hostd::SlotFiles> slots;
	FakeEgress *egress = nullptr; // the agent's, once it bootstrapped
	uint64_t clock_ms = 1000;
	std::unique_ptr<hostd::Agent> agent;

	explicit Rig(const char *name, bool with_token = true) : temp(name) {
		state_dir = temp.file("state");
		scratch_dir = temp.file("state/scratch");
		slots_root = temp.file("slots");
		token_file = temp.file("hostd.token");
		fs::create_directories(scratch_dir);
		if (with_token)
			std::ofstream(token_file) << kToken << "\n";
		slots = std::make_unique<hostd::SlotFiles>(slots_root, false);
		std::string error;
		slots->ensure_root(error);
		runtime.clock_ms = &clock_ms;
		make_agent();
	}

	void make_agent() {
		hostd::AgentConfig config;
		config.machine_id = kMachine;
		config.agent_version = "test-1";
		config.state_dir = state_dir;
		config.scratch_dir = scratch_dir;
		config.token_file = token_file;
		agent = std::make_unique<hostd::Agent>(
				config, link, runtime, *slots,
				[this](int) {
					auto made = std::make_unique<FakeEgress>();
					egress = made.get();
					egress->values = { { 0, 1000 }, { 1, 2000 } };
					return made;
				},
				[this] { return clock_ms; });
	}

	int step() { return agent->step(); }
};

// --- the planner -------------------------------------------------------------------

int test_plan() {
	const std::map<std::string, std::string> data_sets = { { "jop_2_consumer",
			"/opt/game/jop_2_consumer" } };
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
	in.data_sets = &data_sets;
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

	// What cannot start here: no files, a data set not mounted, a slot past the machine's.
	want = { desired(5, 1, 0), desired(6, 1, 1), desired(7, 1, 3) };
	want[0].files.reset();
	want[1].data_set = "dfx2_consumer";
	in.machine_slots = 3;
	plan = hostd::plan_sync(in);
	TEST_EXPECT(plan.start.empty() && plan.refused.size() == 3 && !plan.busy);
	in.machine_slots = 4;

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
	// The egress counters are made for the machine's slots and put in place first.
	TEST_EXPECT(rig.egress && rig.egress->ensures == 1);

	// A restarted agent reads what was saved: no token, no second bootstrap.
	const int posts = rig.link.posts;
	rig.make_agent();
	TEST_EXPECT(rig.agent->bootstrap() && rig.link.posts == posts &&
			rig.agent->machine().agent_secret == kSecret);
	rig.step();
	TEST_EXPECT(rig.service.syncs == 2 && rig.agent->last_sync_ok());

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
	TEST_EXPECT(a.spec.data_dir == "/opt/game/jop_2_consumer" &&
			a.spec.work_dir == rig.slots_root + "/slot-0");
	TEST_EXPECT(a.spec.args == rig.service.desired[0].args && a.spec.env_file.empty());
	TEST_EXPECT(a.spec.cpus == 1.0 && a.spec.memory_mb == 512 && a.spec.pids == 128);
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
	rig.clock_ms += 5000;
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs && rig.runtime.calls.size() == 2);

	// Its egress no longer reads: the last counts stand.
	rig.egress->fail_read = true;
	rig.egress->values = { { 0, 5 } };
	rig.clock_ms += 5000;
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1")->egress_bytes == 1000u);
	rig.egress->fail_read = false;
	rig.egress->values = { { 0, 1500 }, { 1, 2500 } };
	rig.clock_ms += 5000;
	rig.step();
	TEST_EXPECT(rig.service.reported("srv-1-g1")->egress_bytes == 1500u);

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
			r->slot == 0);
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
	TEST_EXPECT(!a->exit_code && !a->started_at);
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
	// Files omitted (the service believes it was reported), a data set this machine
	// lacks, a slot past its four: none starts, none is reported, nothing is busy.
	rig.service.desired = { desired(1, 1, 0), desired(2, 1, 1), desired(3, 1, 3) };
	rig.service.reported_once.insert("srv-1-g1");
	rig.service.desired[1].data_set = "dfx2_consumer";
	rig.service.desired.push_back(desired(4, 1, 5));
	TEST_EXPECT(rig.step() == hosted::kBusySyncIntervalMs);
	TEST_EXPECT(rig.runtime.count("run srv-1-g1") == 0 && rig.runtime.count("run srv-2-g1") == 0);
	TEST_EXPECT(rig.runtime.count("run srv-3-g1") == 1 && rig.runtime.count("run srv-4-g1") == 0);
	rig.step();
	TEST_EXPECT(!rig.service.reported("srv-1-g1") && !rig.service.reported("srv-2-g1") &&
			!rig.service.reported("srv-4-g1"));
	TEST_EXPECT(rig.step() == hosted::kSyncIntervalMs);
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
// the test lists: an address nothing listens on (127.0.0.2, as the controller's standby is
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
				if (host == "controller.test" && net::resolve_ipv4(quad, ep, false))
					out.push_back(ep);
			}
			return out;
		};
	};
	const std::string url = "http://controller.test:" + std::to_string(service.port());
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
		TEST_EXPECT(!reply.transport_ok && reply.error == "cannot reach controller.test");
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
		{ "pass_budget", test_pass_budget },
		{ "refusals", test_refusals },
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
