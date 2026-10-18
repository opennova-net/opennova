// opennova-hostd's platform legs on a stub command runner that checks every argv and
// answers it: the docker runtime (apps/hostd/docker_runtime.h: the exact `docker run`
// of a slot's container with its base tree's bind and, with one, its expansion tree's,
// the ps-then-inspect listing and its partial answers, the stats,
// the stop, kill and removal, every failure's exit code, each call's timeout, the image
// check and the background pull, run here by stand-in programs), docker's own texts read
// (inspect's states, exit codes, OOM flags, labels and times; stats' CPU and memory
// sizes), the images of a repository listed and one removed, the slots' fences and
// counters (apps/hostd/nft_table.h: the whole table in one `nft -f`, its counters read
// from nft's JSON, a missing counter read as a failure) and the fences' self-test's
// targets and probe (apps/hostd/fence_probe.h, against loopback), the machine's identity
// (apps/hostd/machine_identity.h: the machine id file, the ECS agent's attribute, the
// alive file), the process runtime
// (apps/hostd/process_runtime.h: a stand-in opennova-serve script started in its
// slot's directory over its data set with its arguments and the env file's
// variables alone, listed running then exited with its code, stopped by SIGTERM,
// killed, removed once ended, a start that cannot run exit 125; with a log
// directory each server's output in its stream's file, each line after its
// time), the command line
// (apps/hostd/hostd_options.h: its log group and region among them, which send the
// containers' output to CloudWatch, the stream the service names each, and the
// process runtime's log directory), the machine's image from a stand-in instance
// metadata service (apps/hostd/instance_metadata.h) and the agent's own log stream
// from a stand-in ECS container metadata endpoint. No docker and no nft run.

#include "core/ecs_metadata.h"
#include "docker_runtime.h"
#include "fence_probe.h"
#include "hostd_options.h"
#include "instance_metadata.h"
#include "machine_identity.h"
#include "nft_table.h"
#include "process_runtime.h"

#include "net_sockets.h"

#include "common/http_reply_server.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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
	spec.log_stream = "41/g2";
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

	// A cloud machine's: the output to the servers' log group, the stream the service
	// named for this start, never blocking the server; the rest of the start the same.
	hostd::DockerRuntime cloud(
			runner, kDocker, {}, "/tmp", { "/opennova/staging/servers", "us-east-1" });
	std::vector<std::string> logged = expected;
	const auto journald = std::find(logged.begin(), logged.end(), "--log-driver");
	TEST_EXPECT(journald != logged.end() && *(journald + 1) == "journald");
	const auto at = logged.erase(journald, journald + 4);
	logged.insert(at,
			{ "--log-driver", "awslogs", "--log-opt", "awslogs-region=us-east-1", "--log-opt",
					"awslogs-group=/opennova/staging/servers", "--log-opt", "awslogs-stream=41/g2",
					"--log-opt", "mode=non-blocking", "--log-opt", "max-buffer-size=4m" });
	TEST_EXPECT(cloud.run_argv(sample_spec()) == logged);
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
	// The image each was run from (Config.Image).
	TEST_EXPECT(out[0].image == "x" && out[3].image == "x");
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

// nft's JSON of the table's counters (nft 1.0.9, AL2023's), slot 0 and 1 only.
const char *kNftCounters =
		"{\"nftables\": [{\"metainfo\": {\"version\": \"1.0.9\", \"release_name\": \"Old Doc "
		"Yak #3\", \"json_schema_version\": 1}}, "
		"{\"counter\": {\"family\": \"inet\", \"name\": \"egress-0\", \"table\": \"opennova\", "
		"\"handle\": 1, \"packets\": 120, \"bytes\": 45678}}, "
		"{\"counter\": {\"family\": \"inet\", \"name\": \"egress-1\", \"table\": \"opennova\", "
		"\"handle\": 2, \"packets\": 0, \"bytes\": 12345678901234}}, "
		"{\"counter\": {\"family\": \"inet\", \"name\": \"ingress-0\", \"table\": \"opennova\", "
		"\"handle\": 33, \"packets\": 9, \"bytes\": 900}}, "
		"{\"counter\": {\"family\": \"inet\", \"name\": \"ingress-1\", \"table\": \"opennova\", "
		"\"handle\": 34, \"packets\": 1, \"bytes\": 64}}, "
		// Another table's, and a name of no slot: none of the agent's.
		"{\"counter\": {\"family\": \"ip\", \"name\": \"egress-2\", \"table\": \"filter\", "
		"\"handle\": 5, \"packets\": 1, \"bytes\": 1}}, "
		"{\"counter\": {\"family\": \"inet\", \"name\": \"egress-32\", \"table\": \"opennova\", "
		"\"handle\": 6, \"packets\": 1, \"bytes\": 1}}]}";

// Every slot's pair of counters, slot k's bytes k * 10 out and k in.
std::string full_counters() {
	std::string out = "{\"nftables\": [";
	for (int slot = 0; slot < hosted::kMaxSlots; ++slot) {
		for (const char *which : { "egress", "ingress" }) {
			if (out.back() != '[')
				out += ", ";
			const int bytes = std::string(which) == "egress" ? slot * 10 : slot;
			out += "{\"counter\": {\"family\": \"inet\", \"name\": \"" + std::string(which) + "-" +
					std::to_string(slot) +
					"\", \"table\": \"opennova\", \"handle\": 1, \"packets\": 1, \"bytes\": " +
					std::to_string(bytes) + "}}";
		}
	}
	return out + "]}";
}

int test_nft_parse() {
	std::map<int, hostd::SlotCount> counts;
	std::string error;
	TEST_EXPECT(hostd::parse_nft_counters(kNftCounters, counts, error) && counts.size() == 2);
	TEST_EXPECT(counts[0].egress_bytes == 45678u && counts[0].ingress_bytes == 900u);
	TEST_EXPECT(counts[1].egress_bytes == 12345678901234ull && counts[1].ingress_bytes == 64u);
	// A slot with one of its two counters is not read.
	TEST_EXPECT(hostd::parse_nft_counters("{\"nftables\": [{\"counter\": {\"family\": \"inet\", "
										  "\"name\": \"egress-3\", \"table\": \"opennova\", "
										  "\"bytes\": 7}}]}",
						counts, error) &&
			counts.empty());
	TEST_EXPECT(!hostd::parse_nft_counters("Error: No such file or directory", counts, error));
	TEST_EXPECT(!hostd::parse_nft_counters("{\"nft\": []}", counts, error));
	return 0;
}

int test_nft_table() {
	test_temp::TempDir temp("hostd_nft_table");
	StubRunner runner;
	hostd::NftTable table(runner, "/usr/sbin/nft", 8070, temp.root());
	std::string error;
	const std::string rules = hostd::NftTable::ruleset(8070);
	// The table, every slot's two counters and both chains added where missing, then
	// the chains' rules written again: the counters live on.
	TEST_EXPECT(
			rules.rfind("add table inet opennova\nadd counter inet opennova egress-0\n", 0) == 0);
	TEST_EXPECT(rules.find("add counter inet opennova egress-31\n") != std::string::npos &&
			rules.find("add counter inet opennova ingress-31\n") != std::string::npos &&
			rules.find("egress-32") == std::string::npos);
	TEST_EXPECT(rules.find("add chain inet opennova output { type filter hook output priority 0; "
						   "policy accept; }\n") != std::string::npos);
	TEST_EXPECT(rules.find("flush chain inet opennova output\nflush chain inet opennova input\n") !=
			std::string::npos);
	// The fences: every slot user off the metadata services, and off new connections to the
	// fleet's port, the siblings' admin ports and the ECS agent's, on any destination.
	const size_t imds = rules.find("add rule inet opennova output meta skuid 10001-10032 ip daddr "
								   "{ 169.254.169.254, 169.254.170.2 } reject\n");
	const size_t imds6 = rules.find(
			"add rule inet opennova output meta skuid 10001-10032 ip6 daddr fd00:ec2::254 "
			"reject\n");
	const size_t ports =
			rules.find("add rule inet opennova output meta skuid 10001-10032 tcp dport "
					   "{ 8070, 32800-32831, 51678, 51679 } ct state new reject with "
					   "tcp reset\n");
	const size_t private4 =
			rules.find("add rule inet opennova output ip daddr { 10.0.0.0/8, "
					   "172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16, 127.0.0.0/8 } "
					   "return\n");
	const size_t private6 = rules.find(
			"add rule inet opennova output ip6 daddr { fc00::/7, fe80::/10, ::1 } return\n");
	const size_t egress0 = rules.find(
			"add rule inet opennova output meta skuid 10001 counter name \"egress-0\"\n");
	TEST_EXPECT(imds != std::string::npos && imds6 != std::string::npos &&
			ports != std::string::npos && private4 != std::string::npos &&
			private6 != std::string::npos && egress0 != std::string::npos);
	// Refused first, then the private destinations uncounted, then each slot's count.
	TEST_EXPECT(imds < private4 && ports < private4 && private4 < egress0 && private6 < egress0);
	TEST_EXPECT(rules.find("add rule inet opennova output meta skuid 10032 counter name "
						   "\"egress-31\"\n") != std::string::npos);
	TEST_EXPECT(rules.find("add rule inet opennova input udp dport 32768 counter name "
						   "\"ingress-0\"\nadd rule inet opennova input tcp dport 32800 counter "
						   "name \"ingress-0\"\n") != std::string::npos);
	TEST_EXPECT(rules.find("add rule inet opennova input udp dport 32799 counter name "
						   "\"ingress-31\"\nadd rule inet opennova input tcp dport 32831 counter "
						   "name \"ingress-31\"\n") != std::string::npos);
	// A fleet port the set holds already is named once.
	TEST_EXPECT(
			hostd::NftTable::ruleset(32805).find(
					"tcp dport { 32800-32831, 51678, 51679 } ct state new") != std::string::npos);

	// ensure(): the ruleset in one `nft -f` of a file of its own, gone after.
	runner.answer(0);
	TEST_EXPECT(table.ensure(error));
	using V = std::vector<std::string>;
	TEST_EXPECT(runner.calls.size() == 1 &&
			runner.calls[0] == (V{ "/usr/sbin/nft", "-f", temp.root() + "/opennova.nft" }));
	TEST_EXPECT(!std::filesystem::exists(temp.root() + "/opennova.nft"));
	runner.answer(1, "", "Error: Could not process rule: Operation not permitted\nmore");
	TEST_EXPECT(!table.ensure(error) &&
			error ==
					"/usr/sbin/nft -f exited 1: Error: Could not process rule: Operation not "
					"permitted");

	// read(): every slot's counts, from nft's JSON.
	runner.calls.clear();
	runner.answer(0, full_counters());
	std::map<int, hostd::SlotCount> counts;
	TEST_EXPECT(table.read(counts, error));
	TEST_EXPECT(runner.calls[0] ==
			(V{ "/usr/sbin/nft", "-j", "list", "counters", "table", "inet", "opennova" }));
	TEST_EXPECT(
			counts.size() == 32 && counts[3].egress_bytes == 30u && counts[3].ingress_bytes == 3u);
	// A counter missing (someone flushed the table), or no table: a failed read.
	runner.answer(0, kNftCounters);
	TEST_EXPECT(!table.read(counts, error) && counts.size() == 32);
	runner.answer(1, "", "Error: No such file or directory");
	TEST_EXPECT(!table.read(counts, error));
	return 0;
}

// The self-test's probe: a listening port on loopback answers, a closed one is refused.
int test_fence_probe() {
	opennova::net::Endpoint fleet;
	fleet.ip = { 10, 44, 0, 17 };
	const auto targets = hostd::fence_probe_targets({ fleet }, 8070);
	TEST_EXPECT(targets.size() == 4);
	TEST_EXPECT(targets[0].endpoint.ip == (std::array<uint8_t, 4>{ 169, 254, 169, 254 }) &&
			targets[0].endpoint.port == 80);
	TEST_EXPECT(targets[1].endpoint.ip == (std::array<uint8_t, 4>{ 169, 254, 170, 2 }) &&
			targets[1].endpoint.port == 80);
	TEST_EXPECT(targets[2].endpoint.ip == (std::array<uint8_t, 4>{ 127, 0, 0, 1 }) &&
			targets[2].endpoint.port == 51678);
	TEST_EXPECT(targets[3].endpoint.port == 8070 &&
			targets[3].what == "the fleet's internal port (10.44.0.17:8070)");

	const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_EXPECT(::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
			::listen(listener, 1) == 0);
	socklen_t len = sizeof addr;
	::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len);
	const uint16_t open_port = ntohs(addr.sin_port);
	// A port nothing listens on: bound and closed again.
	const int closed = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in closed_addr{};
	closed_addr.sin_family = AF_INET;
	closed_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	::bind(closed, reinterpret_cast<sockaddr *>(&closed_addr), sizeof closed_addr);
	len = sizeof closed_addr;
	::getsockname(closed, reinterpret_cast<sockaddr *>(&closed_addr), &len);
	const uint16_t closed_port = ntohs(closed_addr.sin_port);
	::close(closed);

	hostd::ProbeTarget refused{ "closed", {} };
	refused.endpoint.ip = { 127, 0, 0, 1 };
	refused.endpoint.port = closed_port;
	hostd::ProbeTarget answers{ "open", {} };
	answers.endpoint.ip = { 127, 0, 0, 1 };
	answers.endpoint.port = open_port;
	TEST_EXPECT(hostd::first_answering({ refused }, 1000).empty());
	TEST_EXPECT(hostd::first_answering({ refused, answers }, 1000) == "open");
	// As this test's own user (no fence here): the child reports the same.
	std::string answered;
	std::string error;
	TEST_EXPECT(hostd::probe_as(::getuid(), { refused, answers }, 1000, answered, error) &&
			answered == "open");
	TEST_EXPECT(
			hostd::probe_as(::getuid(), { refused }, 1000, answered, error) && answered.empty());
	::close(listener);
	return 0;
}

int test_machine_identity() {
	int64_t id = 0;
	TEST_EXPECT(hostd::parse_machine_id("12", id) && id == 12);
	TEST_EXPECT(!hostd::parse_machine_id("0", id) && !hostd::parse_machine_id("-3", id) &&
			!hostd::parse_machine_id("1234567890123456", id) && !hostd::parse_machine_id("", id));
	test_temp::TempDir temp("hostd_machine_identity");
	std::string error;
	std::ofstream(temp.file("machine-id")) << "12\n";
	TEST_EXPECT(hostd::read_machine_file(temp.file("machine-id"), id, error) && id == 12);
	std::ofstream(temp.file("bad-id")) << "twelve\n";
	TEST_EXPECT(!hostd::read_machine_file(temp.file("bad-id"), id, error) &&
			error.find("holds no machine id") != std::string::npos);
	TEST_EXPECT(!hostd::read_machine_file(temp.file("none"), id, error));

	// The user-data's ecs.config, as the ECS agent reads it (the last line counts).
	std::string registered;
	const std::string config =
			"ECS_CLUSTER=opennova-prod-fleet\n"
			"ECS_INSTANCE_ATTRIBUTES={\"machine-id\": \"11\", \"kind\": \"own\"}\n"
			"ECS_INSTANCE_ATTRIBUTES={\"machine-id\": \"12\", \"kind\": \"shared\"}\n"
			"ECS_DISABLE_PRIVILEGED=true\n";
	TEST_EXPECT(hostd::ecs_config_machine_id(config, registered, error) && registered == "12");
	TEST_EXPECT(!hostd::ecs_config_machine_id("ECS_CLUSTER=x\n", registered, error) &&
			error == "it sets no ECS_INSTANCE_ATTRIBUTES");
	TEST_EXPECT(!hostd::ecs_config_machine_id(
			"ECS_INSTANCE_ATTRIBUTES={\"kind\": \"own\"}\n", registered, error));
	TEST_EXPECT(!hostd::ecs_config_machine_id("ECS_INSTANCE_ATTRIBUTES=12\n", registered, error));

	// The alive file: fresh once touched, stale past its age.
	const std::string alive = hostd::alive_path(temp.root());
	TEST_EXPECT(alive == temp.root() + "/alive");
	TEST_EXPECT(!hostd::alive_within(alive, 120, std::time(nullptr)));
	TEST_EXPECT(hostd::touch_alive(alive));
	TEST_EXPECT(hostd::alive_within(alive, 120, std::time(nullptr)));
	TEST_EXPECT(!hostd::alive_within(alive, 120, std::time(nullptr) + 121));
	return 0;
}

int test_image_prune_calls() {
	StubRunner runner;
	hostd::DockerRuntime docker(runner, kDocker, {}, "/tmp");
	std::vector<std::string> images;
	std::string error;
	const std::string repo = "ghcr.io/opennova-net/opennova-serve";
	runner.answer(0, repo + "@sha256:aaaa\n" + repo + "@<none>\n" + repo + "@sha256:bbbb\n");
	TEST_EXPECT(docker.list_images(repo, images, 8000, error));
	TEST_EXPECT(runner.calls[0] ==
			(std::vector<std::string>{ kDocker, "image", "ls", "--no-trunc", "--format",
					"{{.Repository}}@{{.Digest}}", repo }));
	// An image held by a tag alone (no digest) is none of these.
	TEST_EXPECT(
			images == (std::vector<std::string>{ repo + "@sha256:aaaa", repo + "@sha256:bbbb" }));
	runner.answer(0);
	TEST_EXPECT(docker.remove_image(repo + "@sha256:aaaa", 8000, error));
	TEST_EXPECT(runner.calls[1] ==
			(std::vector<std::string>{ kDocker, "image", "rm", repo + "@sha256:aaaa" }));
	runner.answer(1, "",
			"Error response from daemon: conflict: unable to remove repository "
			"reference (must force) - container abc is using its referenced image");
	TEST_EXPECT(!docker.remove_image(repo + "@sha256:bbbb", 8000, error));
	runner.answer(1, "", "Cannot connect to the Docker daemon");
	TEST_EXPECT(!docker.list_images(repo, images, 8000, error));
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
	TEST_EXPECT(read(log).find("started") == std::string::npos);

	// With a log directory (the local stack's), each server's output goes to its
	// stream's file under servers/, the directories made; the shared file gets none.
	// The directory stands as the stack's log volume does: 1777.
	const std::string logs = temp.file("logs");
	TEST_EXPECT(::mkdir(logs.c_str(), 0700) == 0 && ::chmod(logs.c_str(), 01777) == 0);
	hostd::ProcessRuntime logged(
			serve, { "PATH=/usr/bin:/bin" }, log, [] { return int64_t{ 1760000000 }; }, logs);
	hostd::ContainerSpec chatty = exits;
	chatty.name = "srv-5-g7";
	chatty.log_stream = "5/g7";
	chatty.args = { "exit", "0" };
	TEST_EXPECT(logged.output_of(chatty) == logs + "/servers/5/g7.log");
	TEST_EXPECT(runtime.output_of(chatty) == log);
	std::ofstream(serve + "-echo") << "#!/bin/sh\necho \"hello from $PWD\"\nexit 0\n";
	::chmod((serve + "-echo").c_str(), 0755);
	hostd::ProcessRuntime echoing(
			serve + "-echo", { "PATH=/usr/bin:/bin" }, log, [] { return int64_t{ 1760000000 }; },
			logs);
	// Under an owner-only umask what it makes is still readable by the site's
	// log reader, another user (directories 0755, the file 0644); the log
	// volume's own mode stands.
	const mode_t umask_was = ::umask(077);
	const bool ran = echoing.run(chatty, 1000, exit_code, error);
	::umask(umask_was);
	TEST_EXPECT(ran);
	const auto mode_of = [](const std::string &path) {
		struct stat st {};
		return ::stat(path.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 07777) : -1;
	};
	TEST_EXPECT(mode_of(logs + "/servers") == 0755 && mode_of(logs + "/servers/5") == 0755);
	TEST_EXPECT(mode_of(logs + "/servers/5/g7.log") == 0644 && mode_of(logs) == 01777);
	std::string written;
	for (int i = 0; i < 200 && written.find("hello from") == std::string::npos; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		written = read(logs + "/servers/5/g7.log");
	}
	TEST_EXPECT(written.find("hello from") != std::string::npos);
	TEST_EXPECT(read(log).find("hello from") == std::string::npos);
	// Each line after the time it was written, as the programs' copies are.
	TEST_EXPECT(written.size() > 25 && written[4] == '-' && written[10] == 'T' &&
			written[19] == '.' && written[23] == 'Z' &&
			written.compare(24, 11, " hello from") == 0);
	// The agent's own copy (main.cpp's machines/<id>/hostd.log) opens the same way,
	// and a file an earlier agent made owner-only is made readable again.
	{
		const mode_t was = ::umask(077);
		const std::string own = logs + "/machines/12/hostd.log";
		std::string why;
		const int fd = hostd::open_log_file(own, why);
		TEST_EXPECT(fd >= 0 && ::close(fd) == 0);
		TEST_EXPECT(mode_of(logs + "/machines") == 0755 &&
				mode_of(logs + "/machines/12") == 0755 && mode_of(own) == 0644);
		TEST_EXPECT(::chmod(own.c_str(), 0600) == 0);
		const int again = hostd::open_log_file(own, why);
		TEST_EXPECT(again >= 0 && ::close(again) == 0 && mode_of(own) == 0644);
		// A directory where a file stands: no file, and why.
		std::ofstream(logs + "/plain") << "x";
		TEST_EXPECT(hostd::open_log_file(logs + "/plain/sub/f.log", why) < 0 && !why.empty());
		::umask(was);
	}
	TEST_EXPECT(hostd::line_stamp().size() == 24 && hostd::line_stamp()[23] == 'Z');
	std::string partial;
	std::string stamped;
	hostd::stamp_lines("one\r\ntwo\nthr", "T", partial, stamped);
	TEST_EXPECT(stamped == "T one\nT two\n" && partial == "thr");
	hostd::stamp_lines("ee\n", "U", partial, stamped);
	TEST_EXPECT(stamped == "T one\nT two\nU three\n" && partial.empty());
	// A stream that is no stream name falls back to the shared file.
	chatty.log_stream = "../x";
	TEST_EXPECT(logged.output_of(chatty) == log);
	::unsetenv("HOSTD_TEST_PLANTED");
	return 0;
}

// What the agent reads of the machine once at its start: its image from the instance
// metadata service (a token PUT, then the GET with it) and its own log stream from its
// ECS container's metadata (its awslogs options), each against a stand-in on loopback,
// and every refusal.
int test_machine_metadata() {
	TEST_EXPECT(opennova::net::startup() == 0);
	std::string image;
	std::string error;
	{
		test_http::ReplyServer imds(200, "ami-0123456789abcdef0\n");
		TEST_EXPECT(hostd::read_image_id(imds.base_url(), image, error));
		TEST_EXPECT(image == "ami-0123456789abcdef0" && imds.requests() == 2);
		TEST_EXPECT(imds.last_line() == "GET /latest/meta-data/ami-id HTTP/1.0");
	}
	{
		test_http::ReplyServer refusing(401, "");
		TEST_EXPECT(!hostd::read_image_id(refusing.base_url(), image, error) &&
				error.find("token answered HTTP 401") != std::string::npos);
	}
	{
		test_http::ReplyServer odd(200, "ami/0123");
		TEST_EXPECT(!hostd::read_image_id(odd.base_url(), image, error) &&
				error.find("no image id") != std::string::npos);
	}
	// Nothing listening (off EC2): no image, quickly.
	TEST_EXPECT(!hostd::read_image_id("http://127.0.0.1:1", image, error, 500));

	namespace ecs = opennova::novaworld_server::ecs;
	std::string stream;
	{
		test_http::ReplyServer metadata(200,
				R"({"DockerId":"abc","Name":"hostd","LogDriver":"awslogs","LogOptions":)"
				R"({"awslogs-group":"/opennova/prod/machines","awslogs-region":"us-east-1",)"
				R"("awslogs-stream":"hostd/hostd/0123456789abcdef"}})");
		TEST_EXPECT(ecs::read_container_log_stream(metadata.base_url() + "/v4/abc", stream, error));
		TEST_EXPECT(stream == "hostd/hostd/0123456789abcdef");
		TEST_EXPECT(metadata.last_line() == "GET /v4/abc HTTP/1.0");
	}
	TEST_EXPECT(!ecs::parse_container_log_stream(R"({"LogDriver":"json-file"})", stream, error) &&
			error.find("names no awslogs stream") != std::string::npos);
	TEST_EXPECT(!ecs::parse_container_log_stream("[", stream, error));
	TEST_EXPECT(!ecs::read_container_log_stream("ftp://x", stream, error));
	{
		test_http::ReplyServer failing(500, "{}");
		TEST_EXPECT(!ecs::read_container_log_stream(failing.base_url(), stream, error) &&
				error.find("answers HTTP 500") != std::string::npos);
	}
	opennova::net::shutdown();
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
	TEST_EXPECT(o.nft == "/usr/sbin/nft" && o.machine_file.empty() && o.ecs_config.empty());
	o = {};
	// The fleet cluster's task definition: the id from the user-data's file, checked
	// against the ECS agent's attribute.
	TEST_EXPECT(hostd::parse_hostd_options(
						{ "--service", "http://fleet.prod.opennova.internal:8070", "--machine-file",
								"/etc/opennova/machine-id", "--ecs-config", "/etc/ecs/ecs.config" },
						o, error) == ParseOutcome::Run);
	TEST_EXPECT(o.machine_id == 0 && o.machine_file == "/etc/opennova/machine-id" &&
			o.ecs_config == "/etc/ecs/ecs.config");
	// The health check needs no service and no machine.
	o = {};
	TEST_EXPECT(
			hostd::parse_hostd_options({ "--check-alive" }, o, error) == ParseOutcome::CheckAlive &&
			o.state_dir == "/var/lib/opennova-hostd");
	TEST_EXPECT(hostd::parse_hostd_options({ "--check-alive", "--state-dir", "/tmp/s" }, o,
						error) == ParseOutcome::CheckAlive &&
			o.state_dir == "/tmp/s");
	std::string host;
	uint16_t port = 0;
	TEST_EXPECT(hostd::service_host_port("http://fleet.prod.opennova.internal:8070", host, port) &&
			host == "fleet.prod.opennova.internal" && port == 8070);
	TEST_EXPECT(hostd::service_host_port("http://10.0.0.1", host, port) && host == "10.0.0.1" &&
			port == 80);
	TEST_EXPECT(!hostd::service_host_port("http://10.0.0.1:x", host, port) &&
			!hostd::service_host_port("http://:8070", host, port) &&
			!hostd::service_host_port("https://10.0.0.1", host, port));
	o = {};
	TEST_EXPECT(hostd::parse_hostd_options(
						{ "--machine", "3", "--service", "http://127.0.0.1:8070", "--token-file",
								"/tmp/t", "--state-dir", "/tmp/s", "--slots-root", "/tmp/slots",
								"--trees-root", "/tmp/trees", "--docker", "/opt/docker", "--aws",
								"/opt/aws", "--nft", "none", "--once", "--verbose" },
						o, error) == ParseOutcome::Run);
	TEST_EXPECT(
			o.token_file == "/tmp/t" && o.state_dir == "/tmp/s" && o.slots_root == "/tmp/slots");
	TEST_EXPECT(o.nft == "none" && o.once && o.verbose && o.docker == "/opt/docker");
	o = {};
	TEST_EXPECT(
			hostd::parse_hostd_options({ "--machine", "4", "--service", "http://fleet:8070",
											   "--runtime", "process", "--serve", "/opt/serve", "--trees-root",
											   "/tmp/trees", "--aws", "/opt/aws" },
					o, error) == ParseOutcome::Run);
	TEST_EXPECT(o.runtime == hostd::RuntimeKind::Process && o.serve == "/opt/serve");
	TEST_EXPECT(o.trees_root == "/tmp/trees" && o.aws == "/opt/aws");
	TEST_EXPECT(o.log_group.empty() && o.log_region.empty());
	o = {};
	TEST_EXPECT(hostd::parse_hostd_options(
						{ "--service", "http://fleet:8070", "--machine", "12", "--log-group",
								"/opennova/prod/servers", "--log-region", "eu-west-1" },
						o, error) == ParseOutcome::Run);
	TEST_EXPECT(o.log_group == "/opennova/prod/servers" && o.log_region == "eu-west-1" &&
			o.log_dir.empty());
	o = {};
	TEST_EXPECT(hostd::parse_hostd_options(
						{ "--service", "http://fleet:8070", "--machine", "12", "--runtime",
								"process", "--log-dir", "/var/log/opennova" },
						o, error) == ParseOutcome::Run);
	TEST_EXPECT(o.log_dir == "/var/log/opennova");
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
		{ "--service", "http://10.0.0.1", "--machine", "3", "--nft", "nft" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--iptables", "none" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--machine-file", "/etc/opennova/id" },
		{ "--service", "http://10.0.0.1", "--machine-file", "machine-id" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--ecs-config", "ecs.config" },
		{ "--check-alive", "--state-dir", "state" },
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
		{ "--service", "http://10.0.0.1", "--machine", "3", "--log-dir", "/var/log/opennova" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--runtime", "process", "--log-dir",
				"logs" },
		{ "--service", "http://10.0.0.1", "--machine", "3", "--runtime", "process", "--log-dir" },
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
		{ "nft_parse", test_nft_parse },
		{ "nft_table", test_nft_table },
		{ "fence_probe", test_fence_probe },
		{ "machine_identity", test_machine_identity },
		{ "image_prune_calls", test_image_prune_calls },
		{ "process_runtime", test_process_runtime },
		{ "machine_metadata", test_machine_metadata },
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
