// opennova-hostd's platform legs on a stub command runner that checks every argv and
// answers it: the docker runtime (apps/hostd/docker_runtime.h: the exact `docker run`
// of a slot's container with its base tree's bind and, with one, its expansion tree's,
// the ps-then-inspect listing and its partial answers, the stats,
// the stop, kill and removal, every failure's exit code, each call's timeout, the image
// check and the background pull, run here by stand-in programs), docker's own texts read
// (inspect's states, exit codes, OOM flags, labels and times; stats' CPU and memory
// sizes), the per-slot egress counters (apps/hostd/egress_counters.h: the chain, its
// jump and the slots' rules put in place only where missing, the listings of iptables
// and ip6tables summed, a missing rule read as a failure), the process runtime
// (apps/hostd/process_runtime.h: a stand-in opennova-serve script started in its
// slot's directory over its data set with its arguments and the env file's
// variables alone, listed running then exited with its code, stopped by SIGTERM,
// killed, removed once ended, a start that cannot run exit 125) and the command
// line (apps/hostd/hostd_options.h: its log group and region among them, which
// send the containers' output to CloudWatch, a stream per slot). No docker and no
// iptables run.

#include "docker_runtime.h"
#include "egress_counters.h"
#include "hostd_options.h"
#include "process_runtime.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <cstdlib>
#include <iterator>
#include <optional>

#include <sys/stat.h>

namespace hostd = opennova::hostd;
namespace hosted = opennova::novaworld_server::hosted;
namespace proc = opennova::proc;

namespace {

constexpr const char *kDocker = "/usr/bin/docker";
const std::string kImage =
		"ghcr.io/opennova-net/"
		"opennova-serve@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// Answers each run from a script, in order, recording every argv; an unscripted
// run answers exit 99.
class StubRunner : public proc::CommandRunner {
public:
	proc::CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		calls.push_back(argv);
		timeouts.push_back(timeout_ms);
		if (replies.empty())
			return exited(99, "", "unscripted");
		proc::CommandResult reply = replies.front();
		replies.pop_front();
		return reply;
	}
	static proc::CommandResult exited(
			uint32_t code, const std::string &out, const std::string &err = "") {
		proc::CommandResult r;
		r.outcome = proc::CommandResult::Outcome::Exited;
		r.exit_code = code;
		r.out = out;
		r.err = err;
		return r;
	}
	void answer(uint32_t code, const std::string &out = "", const std::string &err = "") {
		replies.push_back(exited(code, out, err));
	}
	std::vector<std::vector<std::string>> calls;
	std::vector<int> timeouts;
	std::deque<proc::CommandResult> replies;
};

hostd::ContainerSpec sample_spec() {
	hostd::ContainerSpec spec;
	spec.name = "srv-41-g2";
	spec.slot = 3;
	spec.image = kImage;
	spec.uid = hosted::slot_uid(3);
	spec.base_dir = "/opt/game/12";
	spec.work_dir = "/srv/slots/slot-3";
	spec.args = { "/HOST", "server.host", "--credentials", "credentials.txt", "/game", "jo" };
	spec.cpus = 1.0;
	spec.memory_mb = 512;
	spec.pids = 128;
	return spec;
}

// One docker inspect entry, as docker writes it (trimmed to what is read, plus noise).
std::string inspect_entry(const std::string &name, const std::string &status, int exit_code,
		bool oom, const std::string &started, const std::string &labels) {
	return "{\"Id\":\"0123abcd\",\"Name\":\"/" + name + "\",\"State\":{\"Status\":\"" + status +
			"\",\"Running\":" + (status == "running" ? "true" : "false") +
			",\"OOMKilled\":" + (oom ? "true" : "false") +
			",\"ExitCode\":" + std::to_string(exit_code) + ",\"StartedAt\":\"" + started +
			"\",\"FinishedAt\":\"0001-01-01T00:00:00Z\"},\"Config\":{\"Image\":\"x\",\"Labels\":" +
			labels + "},\"HostConfig\":{\"NetworkMode\":\"host\"}}";
}

std::string labels(int slot) {
	return "{\"opennova.hostd\":\"1\",\"opennova.slot\":\"" + std::to_string(slot) + "\"}";
}

int test_run_argv() {
	StubRunner runner;
	hostd::DockerRuntime docker(runner, kDocker, {}, "/tmp");
	hostd::ContainerSpec spec = sample_spec();
	// Never a pull inside the start: the agent pulls first, in the background.
	const std::vector<std::string> expected = { kDocker, "run", "--pull", "never", "-d", "--name",
		"srv-41-g2", "--label", "opennova.hostd=1", "--label", "opennova.slot=3", "--network",
		"host", "--user", "10004:10004", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
		"--read-only", "--tmpfs", "/tmp:size=64m,nosuid,nodev,noexec", "--cpus", "1", "--memory",
		"512m", "--memory-swap", "512m", "--pids-limit", "128", "--stop-timeout", "60",
		"--log-driver", "journald", "--log-opt", "tag=srv-41-g2", "--mount",
		"type=bind,source=/opt/game/12,target=/opt/game,readonly", "--mount",
		"type=bind,source=/srv/slots/slot-3,target=/srv/serve", kImage, "/HOST", "server.host",
		"--credentials", "credentials.txt", "/game", "jo" };
	TEST_EXPECT(docker.run_argv(spec) == expected);
	// The environment travels in a file, never on the command line.
	spec.env_file = "/var/lib/opennova-hostd/scratch/env-srv-41-g2";
	spec.cpus = 0.5;
	std::vector<std::string> argv = docker.run_argv(spec);
	TEST_EXPECT(argv[23] == "0.5");
	size_t image_at = 0;
	for (size_t i = 0; i < argv.size(); ++i)
		if (argv[i] == kImage)
			image_at = i;
	TEST_EXPECT(argv[image_at - 2] == "--env-file" && argv[image_at - 1] == spec.env_file);
	// An expansion tree: its bind right after the base tree's, read-only, at
	// the code's directory under /opt/game; none without one.
	spec.expansion_dir = "/opt/game/15";
	spec.expansion_code = "jox01";
	argv = docker.run_argv(spec);
	TEST_EXPECT(argv.size() == expected.size() + 4);
	TEST_EXPECT(argv[36] == "--mount" &&
			argv[37] == "type=bind,source=/opt/game/12,target=/opt/game,readonly");
	TEST_EXPECT(argv[38] == "--mount" &&
			argv[39] == "type=bind,source=/opt/game/15,target=/opt/game/expansion/jox01,readonly");
	TEST_EXPECT(argv[40] == "--mount" &&
			argv[41] == "type=bind,source=/srv/slots/slot-3,target=/srv/serve");

	int code = 0;
	std::string error;
	runner.answer(0, "0123abcd\n");
	TEST_EXPECT(docker.run(sample_spec(), 7000, code, error));
	TEST_EXPECT(runner.timeouts.back() == 7000);
	runner.answer(125, "", "docker: Error response from daemon: No such image: x\nmore\n");
	TEST_EXPECT(!docker.run(sample_spec(), 7000, code, error) && code == 125);
	TEST_EXPECT(error ==
			"docker run exited 125: docker: Error response from daemon: No such image: x");
	runner.answer(127, "", "exec: not found");
	TEST_EXPECT(!docker.run(sample_spec(), 7000, code, error) && code == 127);
	proc::CommandResult timed_out;
	timed_out.outcome = proc::CommandResult::Outcome::TimedOut;
	runner.replies.push_back(timed_out);
	TEST_EXPECT(!docker.run(sample_spec(), 7000, code, error) && code == 125 &&
			error == "docker run timed out");
	proc::CommandResult not_started;
	not_started.error = "no such file";
	runner.replies.push_back(not_started);
	TEST_EXPECT(!docker.run(sample_spec(), 7000, code, error) && code == 125);

	// A cloud machine's: the output to the machines' log group, the slot's own
	// stream, never blocking the server; the rest of the start the same.
	hostd::DockerRuntime cloud(
			runner, kDocker, {}, "/tmp", { "/opennova/staging/machines", "us-east-1", 12 });
	std::vector<std::string> logged = expected;
	const auto journald = std::find(logged.begin(), logged.end(), "--log-driver");
	TEST_EXPECT(journald != logged.end() && *(journald + 1) == "journald");
	const auto at = logged.erase(journald, journald + 4);
	logged.insert(at,
			{ "--log-driver", "awslogs", "--log-opt", "awslogs-region=us-east-1", "--log-opt",
					"awslogs-group=/opennova/staging/machines", "--log-opt",
					"awslogs-stream=12/slot-3", "--log-opt", "mode=non-blocking", "--log-opt",
					"max-buffer-size=4m" });
	TEST_EXPECT(cloud.run_argv(sample_spec()) == logged);
	TEST_EXPECT(hostd::DockerRuntime::slot_log_stream(12, 0) == "12/slot-0" &&
			hostd::DockerRuntime::slot_log_stream(7, 31) == "7/slot-31");
	return 0;
}

int test_stop_kill_remove() {
	StubRunner runner;
	hostd::DockerRuntime docker(runner, kDocker, {}, "/tmp");
	std::string error;
	runner.answer(0, "srv-1-g1\n");
	runner.answer(0, "srv-1-g1\n");
	runner.answer(0, "srv-1-g1\n");
	TEST_EXPECT(docker.stop("srv-1-g1", 3000, error) && docker.kill("srv-1-g1", 4000, error) &&
			docker.remove("srv-1-g1", 5000, error));
	TEST_EXPECT(runner.calls[0] ==
			(std::vector<std::string>{ kDocker, "kill", "--signal", "SIGTERM", "srv-1-g1" }));
	TEST_EXPECT(runner.calls[1] ==
			(std::vector<std::string>{ kDocker, "kill", "--signal", "SIGKILL", "srv-1-g1" }));
	// Never `rm -f`: a removal never ends a running container.
	TEST_EXPECT(runner.calls[2] == (std::vector<std::string>{ kDocker, "rm", "srv-1-g1" }));
	TEST_EXPECT((runner.timeouts == std::vector<int>{ 3000, 4000, 5000 }));
	runner.answer(1, "", "Error response from daemon: cannot remove a running container\n");
	TEST_EXPECT(!docker.remove("srv-1-g1", 5000, error) && error.find("docker rm exited 1") == 0);
	return 0;
}

// Whether the machine holds an image (docker image inspect: present, absent by
// docker's "No such image", or an error), and a pull in the background: a child
// of its own (here programs that stand in for the CLI), polled until it ends, its
// standard error's first line the failure's text, ended on the caller's word.
int test_images_and_pulls() {
	StubRunner runner;
	hostd::DockerRuntime docker(runner, kDocker, {}, "/tmp");
	bool present = false;
	std::string error;
	runner.answer(0, "sha256:0123\n");
	TEST_EXPECT(docker.has_image(kImage, present, 6000, error) && present);
	TEST_EXPECT(runner.calls[0] ==
			(std::vector<std::string>{
					kDocker, "image", "inspect", "--format", "{{.Id}}", kImage }));
	TEST_EXPECT(runner.timeouts[0] == 6000);
	runner.answer(1, "[]", "Error response from daemon: No such image: " + kImage + "\n");
	TEST_EXPECT(docker.has_image(kImage, present, 6000, error) && !present);
	runner.answer(1, "", "Cannot connect to the Docker daemon");
	TEST_EXPECT(!docker.has_image(kImage, present, 6000, error) &&
			error.find("Cannot connect") != std::string::npos);

	test_temp::TempDir temp("hostd_runtime_pulls");
	const auto poll_until_done = [](hostd::DockerRuntime &d, std::string &why) {
		for (int i = 0; i < 500; ++i) {
			const hostd::PullState state = d.poll_pull(kImage, why);
			if (state != hostd::PullState::Running)
				return state;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return hostd::PullState::Running;
	};
	// A pull that succeeds, one already under way being no second one.
	hostd::DockerRuntime pulls_ok(runner, "/bin/true", {}, temp.root());
	TEST_EXPECT(pulls_ok.start_pull(kImage, error) && pulls_ok.start_pull(kImage, error));
	TEST_EXPECT(poll_until_done(pulls_ok, error) == hostd::PullState::Done);
	// Ended, it is forgotten.
	TEST_EXPECT(pulls_ok.poll_pull(kImage, error) == hostd::PullState::Failed);
	// One that fails, with its standard error's first line.
	const std::string failing = temp.file("failing-docker");
	{
		std::ofstream script(failing);
		script << "#!/bin/sh\necho \"Error response from daemon: manifest unknown\" >&2\n"
				  "echo more >&2\nexit 1\n";
	}
	::chmod(failing.c_str(), 0755);
	hostd::DockerRuntime pulls_fail(runner, failing, {}, temp.root());
	TEST_EXPECT(pulls_fail.start_pull(kImage, error));
	TEST_EXPECT(poll_until_done(pulls_fail, error) == hostd::PullState::Failed);
	TEST_EXPECT(error == "docker pull exited 1: Error response from daemon: manifest unknown");
	// One that runs on until it is ended.
	const std::string slow = temp.file("slow-docker");
	{
		std::ofstream script(slow);
		script << "#!/bin/sh\nexec sleep 30\n";
	}
	::chmod(slow.c_str(), 0755);
	hostd::DockerRuntime pulls_slow(runner, slow, {}, temp.root());
	TEST_EXPECT(pulls_slow.start_pull(kImage, error));
	TEST_EXPECT(pulls_slow.poll_pull(kImage, error) == hostd::PullState::Running);
	pulls_slow.cancel_pull(kImage);
	TEST_EXPECT(pulls_slow.poll_pull(kImage, error) == hostd::PullState::Failed);
	// One whose CLI is not there does not start.
	hostd::DockerRuntime no_cli(runner, temp.file("no-such-docker"), {}, temp.root());
	TEST_EXPECT(!no_cli.start_pull(kImage, error) && error.find("docker pull did not start") == 0);
	// No error file stays behind.
	size_t left = 0;
	for (const auto &entry : std::filesystem::directory_iterator(temp.root()))
		left += entry.path().filename().string().rfind("pull-", 0) == 0;
	TEST_EXPECT(left == 0);
	return 0;
}

int test_list() {
	StubRunner runner;
	hostd::DockerRuntime docker(runner, kDocker, {}, "/tmp");
	std::vector<hostd::ContainerInfo> out;
	std::string error;
	// No container: no inspect.
	runner.answer(0, "\n");
	TEST_EXPECT(docker.list(out, 8000, error) && out.empty() && runner.calls.size() == 1);
	TEST_EXPECT(runner.calls[0] ==
			(std::vector<std::string>{ kDocker, "ps", "-a", "--no-trunc", "--filter",
					"label=opennova.hostd=1", "--format", "{{.Names}}" }));
	TEST_EXPECT(runner.timeouts[0] == 8000);

	const std::string a = inspect_entry(
			"srv-1-g1", "running", 0, false, "2026-10-14T12:34:56.123456789Z", labels(0));
	const std::string b =
			inspect_entry("srv-2-g4", "exited", 137, true, "2026-10-14T12:00:00Z", labels(5));
	runner.calls.clear();
	runner.answer(0, "srv-1-g1\nsrv-2-g4\n");
	runner.answer(0, "[" + a + "," + b + "]");
	TEST_EXPECT(docker.list(out, 8000, error) && out.size() == 2);
	TEST_EXPECT(runner.calls[1] ==
			(std::vector<std::string>{
					kDocker, "inspect", "--type", "container", "srv-1-g1", "srv-2-g4" }));
	TEST_EXPECT(out[0].name == "srv-1-g1" && out[0].slot == 0 &&
			out[0].state == hosted::ContainerState::Running);
	TEST_EXPECT(!out[0].exit_code && out[0].started_at == 1791981296);
	TEST_EXPECT(out[1].slot == 5 && out[1].state == hosted::ContainerState::Exited &&
			out[1].exit_code == 137 && out[1].oom);

	// One removed between the two calls: inspect exits 1 and answers the rest.
	runner.answer(0, "srv-1-g1\nsrv-9-g1\n");
	runner.answer(1, "[" + a + "]", "Error: No such container: srv-9-g1");
	TEST_EXPECT(docker.list(out, 8000, error) && out.size() == 1 && out[0].name == "srv-1-g1");
	// A listing that fails or does not read is an error: the agent acts on nothing.
	runner.answer(1, "", "Cannot connect to the Docker daemon");
	TEST_EXPECT(
			!docker.list(out, 8000, error) && error.find("Cannot connect") != std::string::npos);
	runner.answer(0, "srv-1-g1\n");
	runner.answer(1, "", "permission denied");
	TEST_EXPECT(!docker.list(out, 8000, error));
	runner.answer(0, "srv-1-g1\n");
	runner.answer(0, "not json");
	TEST_EXPECT(!docker.list(out, 8000, error));
	return 0;
}

int test_parse_inspect() {
	std::vector<hostd::ContainerInfo> out;
	std::string error;
	const std::string json = "[" +
			inspect_entry("srv-3-g1", "created", 0, false, "0001-01-01T00:00:00Z", labels(1)) +
			"," +
			inspect_entry("srv-4-g1", "restarting", 0, false, "2026-10-14T12:00:00Z", labels(2)) +
			"," + inspect_entry("srv-5-g1", "paused", 0, false, "2026-10-14T12:00:00Z", labels(3)) +
			"," + inspect_entry("srv-6-g1", "dead", 1, false, "2026-10-14T12:00:00Z", labels(4)) +
			"," +
			// Not the agent's: no label, or another value.
			inspect_entry("postgres", "running", 0, false, "2026-10-14T12:00:00Z", "{}") + "," +
			inspect_entry("other", "running", 0, false, "2026-10-14T12:00:00Z",
					"{\"opennova.hostd\":\"0\"}") +
			"," +
			// The agent's, its slot label gone wrong.
			inspect_entry("srv-7-g1", "running", 0, false, "2026-10-14T12:00:00Z",
					"{\"opennova.hostd\":\"1\",\"opennova.slot\":\"32\"}") +
			"," +
			inspect_entry("srv-8-g1", "running", 0, false, "2026-10-14T12:00:00Z",
					"{\"opennova.hostd\":\"1\",\"opennova.slot\":\"x\"}") +
			"]";
	TEST_EXPECT(hostd::parse_docker_inspect(json, out, error) && out.size() == 6);
	TEST_EXPECT(out[0].state == hosted::ContainerState::Starting && !out[0].started_at &&
			out[0].slot == 1);
	TEST_EXPECT(out[1].state == hosted::ContainerState::Starting);
	TEST_EXPECT(out[2].state == hosted::ContainerState::Running);
	TEST_EXPECT(
			out[3].state == hosted::ContainerState::Exited && out[3].exit_code == 1 && !out[3].oom);
	TEST_EXPECT(out[4].name == "srv-7-g1" && out[4].slot == -1 && out[5].slot == -1);
	TEST_EXPECT(!hostd::parse_docker_inspect("{}", out, error) &&
			!hostd::parse_docker_inspect("", out, error));
	TEST_EXPECT(hostd::parse_docker_inspect("[]", out, error) && out.empty());
	return 0;
}

int test_times_and_sizes() {
	TEST_EXPECT(hostd::parse_docker_time("1970-01-01T00:00:00Z") == 0);
	TEST_EXPECT(hostd::parse_docker_time("2026-10-14T12:34:56Z") == 1791981296);
	TEST_EXPECT(hostd::parse_docker_time("2026-10-14T12:34:56.999999999Z") == 1791981296);
	TEST_EXPECT(hostd::parse_docker_time("2024-02-29T00:00:00+00:00") == 1709164800);
	TEST_EXPECT(hostd::parse_docker_time("2000-03-01T00:00:00Z") == 951868800);
	for (const char *bad : { "0001-01-01T00:00:00Z", "", "2026-10-14", "2026-10-14 12:34:56Z",
				 "2026-13-01T00:00:00Z", "2026-10-14T12:34:56+02:00", "2026-10-14T12:34:56" })
		TEST_EXPECT(!hostd::parse_docker_time(bad));

	TEST_EXPECT(hostd::parse_docker_size("0B") == 0u && hostd::parse_docker_size("512B") == 512u);
	TEST_EXPECT(hostd::parse_docker_size("1.5KiB") == 1536u &&
			hostd::parse_docker_size("45.5MiB") == 47710208u);
	TEST_EXPECT(hostd::parse_docker_size("2GiB") == 2147483648u &&
			hostd::parse_docker_size("1.2kB") == 1200u);
	TEST_EXPECT(hostd::parse_docker_size("3MB") == 3000000u);
	for (const char *bad : { "", "MiB", "12", "12 MiB", "-1B", "1.5XB" })
		TEST_EXPECT(!hostd::parse_docker_size(bad));

	std::map<std::string, hostd::ContainerStats> stats;
	hostd::parse_docker_stats(
			"{\"BlockIO\":\"0B / "
			"0B\",\"CPUPerc\":\"12.34%\",\"Container\":\"ab\",\"ID\":\"ab\",\"MemPerc\":\"8.89%\","
			"\"MemUsage\":\"45.5MiB / 512MiB\",\"Name\":\"srv-1-g1\",\"NetIO\":\"0B / "
			"0B\",\"PIDs\":\"9\"}\n"
			"{\"CPUPerc\":\"--\",\"MemUsage\":\"-- / --\",\"Name\":\"srv-2-g1\"}\n"
			"garbage\n"
			"{\"CPUPerc\":\"150.00%\",\"MemUsage\":\"1GiB / 2GiB\",\"Name\":\"srv-3-g1\"}\n",
			stats);
	TEST_EXPECT(stats.size() == 3);
	TEST_EXPECT(
			stats["srv-1-g1"].cpu_percent == 12.34 && stats["srv-1-g1"].memory_bytes == 47710208u);
	TEST_EXPECT(stats["srv-2-g1"].cpu_percent == 0.0 && stats["srv-2-g1"].memory_bytes == 0u);
	TEST_EXPECT(stats["srv-3-g1"].cpu_percent == 150.0 &&
			stats["srv-3-g1"].memory_bytes == 1073741824u);

	StubRunner runner;
	hostd::DockerRuntime docker(runner, kDocker, {}, "/tmp");
	std::string error;
	TEST_EXPECT(docker.stats({}, stats, 8000, error) && stats.empty() && runner.calls.empty());
	runner.answer(
			0, "{\"CPUPerc\":\"1.00%\",\"MemUsage\":\"1MiB / 512MiB\",\"Name\":\"srv-1-g1\"}\n");
	TEST_EXPECT(docker.stats({ "srv-1-g1" }, stats, 8000, error) && stats.size() == 1);
	TEST_EXPECT(runner.calls[0] ==
			(std::vector<std::string>{ kDocker, "stats", "--no-stream", "--no-trunc", "--format",
					"{{json .}}", "srv-1-g1" }));
	runner.answer(1, "", "Error: No such container: srv-1-g1");
	TEST_EXPECT(!docker.stats({ "srv-1-g1" }, stats, 8000, error));
	return 0;
}

// iptables-nft's listing (Ubuntu 24.04), and the legacy one's columns.
const char *kNftListing =
		"Chain OPENNOVA-EGRESS (1 references)\n"
		"    pkts      bytes target     prot opt in     out     source               destination   "
		"      \n"
		// A private destination's rule: no slot's count.
		"      40     9000 RETURN     0    --  *      *       0.0.0.0/0            10.0.0.0/8      "
		"     \n"
		"     120    45678 RETURN     0    --  *      *       0.0.0.0/0            0.0.0.0/0       "
		"     owner UID match 10001\n"
		"       0        0 RETURN     0    --  *      *       0.0.0.0/0            0.0.0.0/0       "
		"     owner UID match 10002\n"
		"  999999 12345678901234 RETURN 0 -- * * 0.0.0.0/0 0.0.0.0/0 owner UID match 10003\n";
const char *kLegacy6Listing =
		"Chain OPENNOVA-EGRESS (1 references)\n"
		"    pkts      bytes target     prot opt in     out     source               destination\n"
		"       3      300 RETURN     all      *      *       ::/0                 ::/0            "
		"     owner UID match 10001\n"
		"       1      100 RETURN     all      *      *       ::/0                 ::/0            "
		"     owner UID match 10002\n"
		"       0        0 RETURN     all      *      *       ::/0                 ::/0            "
		"     owner UID match 10003\n"
		"       5      500 RETURN     all      *      *       ::/0                 ::/0            "
		"     owner UID match 10001-10003\n";

int test_chain_parse() {
	std::map<uint32_t, uint64_t> by_uid;
	hostd::parse_chain_counters(kNftListing, by_uid);
	TEST_EXPECT(by_uid.size() == 3 && by_uid[10001] == 45678u && by_uid[10002] == 0u &&
			by_uid[10003] == 12345678901234ull);
	by_uid.clear();
	hostd::parse_chain_counters(kLegacy6Listing, by_uid);
	// A UID range is none of the agent's rules.
	TEST_EXPECT(by_uid.size() == 3 && by_uid[10001] == 300u && by_uid[10002] == 100u);
	by_uid.clear();
	hostd::parse_chain_counters("iptables: No chain/target/match by that name.\n", by_uid);
	TEST_EXPECT(by_uid.empty());
	return 0;
}

int test_egress() {
	StubRunner runner;
	hostd::EgressCounters counters(
			runner, { { "/usr/sbin/iptables", false }, { "/usr/sbin/ip6tables", true } }, 3);
	std::string error;
	// Nothing in place yet: the chain made, OUTPUT's jump inserted first, each private
	// destination's rule inserted at the chain's head, each slot's rule appended, each
	// only after its check came back "not there" (exit 1).
	for (const size_t privates : { size_t{ 5 }, size_t{ 3 } }) {
		runner.answer(1, "", "No chain/target/match by that name.");
		runner.answer(0);
		runner.answer(1, "", "Bad rule");
		runner.answer(0);
		for (size_t i = 0; i < privates + 3; ++i) {
			runner.answer(1, "", "Bad rule");
			runner.answer(0);
		}
	}
	TEST_EXPECT(counters.ensure(error));
	TEST_EXPECT(runner.calls.size() == 36);
	using V = std::vector<std::string>;
	TEST_EXPECT(runner.calls[0] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-L", "OPENNOVA-EGRESS", "-n" }));
	TEST_EXPECT(runner.calls[1] == (V{ "/usr/sbin/iptables", "-w", "5", "-N", "OPENNOVA-EGRESS" }));
	TEST_EXPECT(runner.calls[2] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-C", "OUTPUT", "-j", "OPENNOVA-EGRESS" }));
	TEST_EXPECT(runner.calls[3] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-I", "OUTPUT", "1", "-j", "OPENNOVA-EGRESS" }));
	// The private destinations (the metadata service, the internal port, the admin
	// links, loopback) return uncounted, ahead of any slot's rule.
	TEST_EXPECT(runner.calls[4] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-C", "OPENNOVA-EGRESS", "-d", "10.0.0.0/8", "-j",
					"RETURN" }));
	TEST_EXPECT(runner.calls[5] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-I", "OPENNOVA-EGRESS", "1", "-d", "10.0.0.0/8",
					"-j", "RETURN" }));
	TEST_EXPECT(runner.calls[10][6] == "169.254.0.0/16" && runner.calls[12][6] == "127.0.0.0/8");
	TEST_EXPECT(runner.calls[14] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-C", "OPENNOVA-EGRESS", "-m", "owner",
					"--uid-owner", "10001", "-j", "RETURN" }));
	TEST_EXPECT(runner.calls[15] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-A", "OPENNOVA-EGRESS", "-m", "owner",
					"--uid-owner", "10001", "-j", "RETURN" }));
	TEST_EXPECT(runner.calls[19][8] == "10003" && runner.calls[20][0] == "/usr/sbin/ip6tables");
	// IPv6's own: unique-local (Nitro's metadata address), link-local, loopback.
	TEST_EXPECT(runner.calls[24][5] == "-d" && runner.calls[24][6] == "fc00::/7");
	TEST_EXPECT(runner.calls[27][7] == "fe80::/10" && runner.calls[29][7] == "::1/128");

	// All in place: checks alone, nothing added (the counters keep counting).
	runner.calls.clear();
	for (int i = 0; i < 18; ++i)
		runner.answer(0);
	TEST_EXPECT(counters.ensure(error) && runner.calls.size() == 18);
	for (const auto &call : runner.calls)
		TEST_EXPECT(call[3] == "-L" || call[3] == "-C");

	// A tool that fails otherwise (exit 2, 4) stops it.
	runner.calls.clear();
	runner.answer(0);
	runner.answer(2, "", "iptables v1.8.10 (nf_tables): unknown option \"--uid-owner\"");
	TEST_EXPECT(!counters.ensure(error) &&
			error ==
					"/usr/sbin/iptables -C OUTPUT exited 2: iptables v1.8.10 "
					"(nf_tables): unknown option \"--uid-owner\"");

	// The read: both tools' listings summed by slot.
	runner.calls.clear();
	runner.replies.clear();
	runner.answer(0, kNftListing);
	runner.answer(0, kLegacy6Listing);
	std::map<int, uint64_t> bytes;
	TEST_EXPECT(counters.read(bytes, error));
	TEST_EXPECT(runner.calls[0] ==
			(V{ "/usr/sbin/iptables", "-w", "5", "-L", "OPENNOVA-EGRESS", "-n", "-v", "-x" }));
	TEST_EXPECT(bytes.size() == 3 && bytes[0] == 45978u && bytes[1] == 100u &&
			bytes[2] == 12345678901234ull);
	// A slot's rule missing (someone flushed the chain) fails the read but keeps the rest.
	runner.answer(0,
			"Chain OPENNOVA-EGRESS (1 references)\n"
			"     1      10 RETURN     0    --  *      *       0.0.0.0/0            0.0.0.0/0      "
			"      owner UID match 10001\n");
	runner.answer(0, kLegacy6Listing);
	TEST_EXPECT(!counters.read(bytes, error) && bytes.size() == 3 && bytes[0] == 310u &&
			bytes[1] == 100u);
	runner.answer(1, "", "No chain/target/match by that name.");
	runner.answer(0, kLegacy6Listing);
	TEST_EXPECT(!counters.read(bytes, error) && bytes[0] == 300u);
	return 0;
}

// The process runtime: each server a child process of the agent. A stand-in
// opennova-serve (a shell script) writes what it was started with, then runs until
// SIGTERM or exits with the code its arguments name.
int test_process_runtime() {
	test_temp::TempDir temp("hostd_process_runtime");
	const std::string serve = temp.file("fake-serve");
	{
		std::ofstream script(serve);
		script << "#!/bin/sh\n"
				  "# --resource-dir <dir> <mode> [<code>]\n"
				  "printf 'cwd=%s\\ndata=%s\\nnote=%s\\nplanted=%s\\n' \"$PWD\" \"$2\" "
				  "\"$OPENNOVA_SLOT_NOTE\" \"$HOSTD_TEST_PLANTED\" > started\n"
				  "case \"$3\" in exit) exit \"$4\";; esac\n"
				  "trap 'echo stopped > stopped; exit 0' TERM\n"
				  "while :; do sleep 0.05; done\n";
	}
	::chmod(serve.c_str(), 0755);
	// A variable of the agent's own, which no server may see.
	::setenv("HOSTD_TEST_PLANTED", "hunter2", 1);
	const std::string log = temp.file("servers.log");
	hostd::ProcessRuntime runtime(
			serve, { "PATH=/usr/bin:/bin" }, log, [] { return int64_t{ 1760000000 }; });

	const auto slot_dir = [&](int slot) {
		const std::string dir = temp.file(("slot-" + std::to_string(slot)).c_str());
		std::filesystem::create_directories(dir);
		return dir;
	};
	const auto read = [](const std::string &path) {
		std::ifstream in(path);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	};
	const auto wait_file = [](const std::string &path) {
		for (int i = 0; i < 200 && !std::filesystem::exists(path); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		return std::filesystem::exists(path);
	};
	const auto state_of = [&](const std::string &name) -> std::optional<hostd::ContainerInfo> {
		std::vector<hostd::ContainerInfo> listed;
		std::string why;
		if (!runtime.list(listed, 1000, why))
			return std::nullopt;
		for (const auto &c : listed)
			if (c.name == name)
				return c;
		return std::nullopt;
	};
	const auto exited_with = [&](const std::string &name, int64_t code) {
		for (int i = 0; i < 200; ++i) {
			const auto c = state_of(name);
			if (c && c->state == hosted::ContainerState::Exited)
				return c->exit_code == code;
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		return false;
	};

	// The argv: the binary, the data set's folder, then the server's arguments.
	hostd::ContainerSpec spec;
	spec.name = "srv-1-g1";
	spec.slot = 1;
	spec.base_dir = "/opt/game/7";
	spec.args = { "run", "/HOST", "server.host" };
	TEST_EXPECT(runtime.run_argv(spec) ==
			(std::vector<std::string>{ serve, "--resource-dir", "/opt/game/7", "run",
					"/HOST", "server.host" }));

	// Started in its slot's directory with the env file's variables and the system's alone.
	spec.work_dir = slot_dir(1);
	spec.env_file = temp.file("env-srv-1-g1");
	{
		std::ofstream env(spec.env_file);
		env << "OPENNOVA_SLOT_NOTE=slot one\n";
	}
	int exit_code = -1;
	std::string error;
	TEST_EXPECT(runtime.run(spec, 1000, exit_code, error) && exit_code == 0);
	TEST_EXPECT(wait_file(spec.work_dir + "/started"));
	const std::string seen = read(spec.work_dir + "/started");
	TEST_EXPECT(seen.find("cwd=" + std::filesystem::canonical(spec.work_dir).string() + "\n") !=
			std::string::npos);
	TEST_EXPECT(seen.find("data=/opt/game/7\n") != std::string::npos);
	TEST_EXPECT(seen.find("note=slot one\n") != std::string::npos);
	TEST_EXPECT(seen.find("planted=\n") != std::string::npos);
	const auto c = state_of("srv-1-g1");
	TEST_EXPECT(c && c->slot == 1 && c->state == hosted::ContainerState::Running && !c->exit_code &&
			c->started_at == int64_t{ 1760000000 });
	std::map<std::string, hostd::ContainerStats> stats;
	TEST_EXPECT(runtime.stats({ "srv-1-g1", "srv-9-g9" }, stats, 1000, error) &&
			stats.size() == 1 && stats.count("srv-1-g1") == 1);
	// A second start of one name is refused; a running one is not removed.
	TEST_EXPECT(!runtime.run(spec, 1000, exit_code, error) && exit_code == 125);
	TEST_EXPECT(!runtime.remove("srv-1-g1", 1000, error));
	// Every image is the machine's own: present, never pulled.
	bool present = false;
	TEST_EXPECT(runtime.has_image("anything:tag", present, 1000, error) && present);
	TEST_EXPECT(runtime.start_pull("anything:tag", error) &&
			runtime.poll_pull("anything:tag", error) == hostd::PullState::Done);

	// A stop is SIGTERM: it ends on its own, exit 0, and is then removed.
	TEST_EXPECT(runtime.stop("srv-1-g1", 1000, error));
	TEST_EXPECT(wait_file(spec.work_dir + "/stopped"));
	TEST_EXPECT(exited_with("srv-1-g1", 0));
	TEST_EXPECT(runtime.remove("srv-1-g1", 1000, error) && !state_of("srv-1-g1"));

	// One that exits on its own keeps its code until removed.
	hostd::ContainerSpec exits = spec;
	exits.name = "srv-2-g1";
	exits.slot = 0;
	exits.work_dir = slot_dir(0);
	exits.env_file.clear();
	exits.args = { "exit", "3" };
	TEST_EXPECT(runtime.run(exits, 1000, exit_code, error));
	TEST_EXPECT(exited_with("srv-2-g1", 3));
	TEST_EXPECT(runtime.remove("srv-2-g1", 1000, error));

	// A kill is SIGKILL (128 + 9).
	hostd::ContainerSpec killed = exits;
	killed.name = "srv-3-g1";
	killed.args = { "run" };
	std::filesystem::remove(killed.work_dir + "/started");
	TEST_EXPECT(runtime.run(killed, 1000, exit_code, error));
	TEST_EXPECT(wait_file(killed.work_dir + "/started"));
	TEST_EXPECT(runtime.kill("srv-3-g1", 1000, error));
	TEST_EXPECT(exited_with("srv-3-g1", 137));
	TEST_EXPECT(runtime.remove("srv-3-g1", 1000, error));

	// A start that cannot run (its slot's directory is missing) is docker's 125, nothing listed.
	hostd::ContainerSpec broken = exits;
	broken.name = "srv-4-g1";
	broken.work_dir = temp.file("no-such-slot");
	TEST_EXPECT(!runtime.run(broken, 1000, exit_code, error) && exit_code == 125 &&
			!error.empty() && !state_of("srv-4-g1"));
	// So is an env file that does not read.
	broken.work_dir = exits.work_dir;
	broken.env_file = temp.file("no-such-env");
	TEST_EXPECT(!runtime.run(broken, 1000, exit_code, error) && exit_code == 125);
	TEST_EXPECT(!runtime.stop("srv-4-g1", 1000, error) && !runtime.kill("srv-4-g1", 1000, error));

	// The env file's lines: NAME=value each, CR LF read, a line with no name skipped.
	TEST_EXPECT(hostd::read_env_file_lines("A=1\r\nB=two words\n=x\nnoequals\nC=\n") ==
			(std::vector<std::string>{ "A=1", "B=two words", "C=" }));
	::unsetenv("HOSTD_TEST_PLANTED");
	return 0;
}

int test_options() {
	using hostd::ParseOutcome;
	hostd::HostdOptions o;
	std::string error;
	TEST_EXPECT(
			hostd::parse_hostd_options({ "--service", "http://10.44.0.10:8070", "--machine", "12" },
					o, error) == ParseOutcome::Run);
	TEST_EXPECT(o.service_url == "http://10.44.0.10:8070" && o.machine_id == 12);
	TEST_EXPECT(o.token_file == "/etc/opennova/hostd.token" &&
			o.state_dir == "/var/lib/opennova-hostd");
	TEST_EXPECT(o.slots_root == "/srv/slots" && o.docker == "/usr/bin/docker" && !o.once);
	TEST_EXPECT(
			o.runtime == hostd::RuntimeKind::Docker && o.serve == "/usr/local/bin/opennova-serve");
	TEST_EXPECT(o.trees_root == "/opt/game" && o.aws == "/usr/local/bin/aws");
	o = {};
	TEST_EXPECT(hostd::parse_hostd_options(
						{ "--machine", "3", "--service", "http://127.0.0.1:8070", "--token-file",
								"/tmp/t", "--state-dir", "/tmp/s", "--slots-root", "/tmp/slots",
								"--trees-root", "/tmp/trees", "--docker", "/opt/docker", "--aws",
								"/opt/aws", "--iptables", "none", "--ip6tables", "none", "--once",
								"--verbose" },
						o, error) == ParseOutcome::Run);
	TEST_EXPECT(
			o.token_file == "/tmp/t" && o.state_dir == "/tmp/s" && o.slots_root == "/tmp/slots");
	TEST_EXPECT(o.iptables == "none" && o.ip6tables == "none" && o.once && o.verbose &&
			o.docker == "/opt/docker");
	o = {};
	TEST_EXPECT(
			hostd::parse_hostd_options({ "--machine", "4", "--service", "http://controller:8070",
											   "--runtime", "process", "--serve", "/opt/serve", "--trees-root",
											   "/tmp/trees", "--aws", "/opt/aws" },
					o, error) == ParseOutcome::Run);
	TEST_EXPECT(o.runtime == hostd::RuntimeKind::Process && o.serve == "/opt/serve");
	TEST_EXPECT(o.trees_root == "/tmp/trees" && o.aws == "/opt/aws");
	TEST_EXPECT(o.log_group.empty() && o.log_region.empty());
	o = {};
	TEST_EXPECT(hostd::parse_hostd_options(
						{ "--service", "http://controller:8070", "--machine", "12", "--log-group",
								"/opennova/prod/machines", "--log-region", "eu-west-1" },
						o, error) == ParseOutcome::Run);
	TEST_EXPECT(o.log_group == "/opennova/prod/machines" && o.log_region == "eu-west-1");
	TEST_EXPECT(hostd::parse_hostd_options({ "--help" }, o, error) == ParseOutcome::Help);
	TEST_EXPECT(hostd::parse_hostd_options({ "--version" }, o, error) == ParseOutcome::Version);
	const std::vector<std::vector<std::string>> bad = {
		{},
		{ "--machine", "3" },
		{ "--service", "http://10.0.0.1:8070" },
		{ "--service", "https://10.0.0.1", "--machine", "3" },
		{ "--service", "http://10.0.0.1", "--machine", "0" },
		{ "--service", "http://10.0.0.1", "--machine", "-3" },
		{ "--service", "http://10.0.0.1", "--machine", "x" },
		{ "--service", "http://10.0.0.1", "--machine" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--docker", "docker" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--iptables", "iptables" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--slots-root", "/srv/a,b" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--slots-root", "/srv/../etc" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--slots-root", "/srv/slots/" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--trees-root", "/opt/a,b" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--trees-root", "game" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--aws", "aws" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--frobnicate" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--runtime", "podman" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--serve", "opennova-serve" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--log-group", "/opennova/x" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--log-region", "us-east-1" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--log-group", "/a b", "--log-region",
				"us-east-1" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--log-group", "/a:b", "--log-region",
				"us-east-1" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--log-group", "/a", "--log-region",
				"US-East-1" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--log-group", "/a", "--log-region",
				"us-east-1", "--runtime", "process" },
	};
	for (const auto &args : bad) {
		hostd::HostdOptions fresh;
		TEST_EXPECT(hostd::parse_hostd_options(args, fresh, error) == ParseOutcome::Error &&
				!error.empty());
	}
	TEST_EXPECT(std::string(hostd::hostd_usage()).find("usage: opennova-hostd") == 0);
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "run_argv", test_run_argv },
		{ "stop_kill_remove", test_stop_kill_remove },
		{ "images_and_pulls", test_images_and_pulls },
		{ "list", test_list },
		{ "parse_inspect", test_parse_inspect },
		{ "times_and_sizes", test_times_and_sizes },
		{ "chain_parse", test_chain_parse },
		{ "egress", test_egress },
		{ "process_runtime", test_process_runtime },
		{ "options", test_options },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: opennova-hostd runtime\n");
	return 0;
}
