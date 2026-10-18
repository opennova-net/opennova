// The fleet's machine drivers (apps/novaworld_common/hosted/model/machine_driver.h)
// and settings. The EC2 driver (hosted/cloud/ec2_machine_driver.h) on a stub
// CommandRunner that records every AWS CLI argv and answers it from a script:
// the launch's run-instances (the exact argv, each kind's launch template and
// a pinned version, the tags, the row's own client token, the user-data file
// with the machine's values filled in, deleted after; a call cut short made
// again the same; of two subnets the zone holding fewer live machines, ties to
// the first, a row's own machine's subnet first, each subnet recorded on the row
// before its first call (one unrecorded called in nothing), a run again calling
// first where its row recorded an earlier call (no describe: EC2 lists a launch
// late), else reading its first run's subnet afresh, and a zone without capacity
// handing the launch to the other under that subnet's token), describe's state mapping from
// one cached describe-instances (the launch grace by the row's age, judged
// alike by another worker's driver, the cache's lifetime, a failure reading
// Unknown and its back-off, describes at once waiting for one read),
// terminate's tag check (a machine
// without both of this deployment's tags never ended) and its idempotence,
// list_tagged's scope and its throw, the slots' ports, and every CLI failure,
// timeout and malformed reply; its config checks and the data sets' volume
// labels. The shipped machine script (hosted/cloud/machine/machine_user_data.sh):
// its blanks and their render, no xtrace, the token and the machine id written
// owner-only in an owner-only directory, the slots' ports reserved, and the
// ECS agent's config (the fleet cluster, the machine's id and kind as its
// instance attributes, task roles in host mode, no privileged task, never the
// cleanup of images ECS did not pull), with nothing of the machine set up
// before (no docker install, no fence, no unit, no pull). The settings
// (hosted/model/fleet_config.h): the defaults, every variable's read and its refusals. And
// the docker driver, the local stack's (hosted/cloud/docker_machine_driver.h), against an
// engine in memory: its config checks and profile, the machine's command line and create
// body, the token's archive, a launch's calls in order, a launch run again, its failures
// (each leaving nothing behind), describe's states, terminate's scope and list_tagged's
// filter. Crow-free and database-free: compiles the sources it needs directly, so it runs
// on every service build.

#include "command_runner.h"
#include "hosted/cloud/docker_machine_driver.h"
#include "hosted/cloud/ec2_machine_driver.h"
#include "hosted/model/fleet_config.h"
#include "hosted/model/hosted_settings.h"
#include "hosted/protocol/agent_protocol.h"

#include <base/io/json.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace fs = std::filesystem;
namespace hosted = opennova::novaworld_server::hosted;
using hosted::CloudState;
using hosted::MachineKind;
using opennova::proc::CommandResult;

namespace {

constexpr const char *kAws = "/usr/local/bin/aws";
constexpr const char *kRegion = "us-east-1";
constexpr const char *kCluster = "opennova-server-fleet";
constexpr const char *kShared = "lt-0123456789abcdef0";
constexpr const char *kOwn = "lt-0abcdef0123456789";
constexpr const char *kSubnet = "subnet-0123456789abcdef0";
// The second zone's.
constexpr const char *kSubnetB = "subnet-0fedcba9876543210";
constexpr const char *kDeployment = "opennova-server";
constexpr const char *kImage = "ghcr.io/opennova-net/opennova-serve@sha256:"
							   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char *kHostdImage =
		"ghcr.io/opennova-net/opennova-hostd@sha256:"
		"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
// The fleet's private address (its internal listener's bind, what admin.cfg
// admits) and its internal listener's URL by name, as the machines reach it.
constexpr const char *kService = "10.44.0.10";
constexpr const char *kServiceUrl = "http://fleet.prod.opennova.internal:8070";
constexpr const char *kMachineA = "i-0aaaaaaaaaaaaaaa1";
constexpr const char *kMachineB = "i-0bbbbbbbbbbbbbbb2";
constexpr uint64_t kTtl = 10 * 1000;
constexpr uint64_t kGrace = 3 * 60 * 1000;
// A launch's age past the grace (a row launched long ago, or none listed at all).
constexpr int64_t kLong = static_cast<int64_t>(kGrace) + 1;

// --- the stub ----------------------------------------------------------------------------

struct Call {
	std::vector<std::string> argv;
	int timeout_ms = 0;
	std::string user_data; // the --user-data file's text while the call ran
	std::string user_data_path;
};

class StubRunner : public opennova::proc::CommandRunner {
public:
	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		Call call;
		call.argv = argv;
		call.timeout_ms = timeout_ms;
		for (size_t i = 0; i + 1 < argv.size(); ++i) {
			if (argv[i] == "--user-data" && argv[i + 1].rfind("file://", 0) == 0) {
				call.user_data_path = argv[i + 1].substr(7);
				std::ifstream in(fs::u8path(call.user_data_path), std::ios::binary);
				call.user_data.assign(
						std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			}
		}
		calls.push_back(call);
		if (replies.empty()) {
			CommandResult unexpected;
			unexpected.outcome = CommandResult::Outcome::Exited;
			unexpected.exit_code = 255;
			unexpected.err = "the stub has no reply for this call";
			return unexpected;
		}
		CommandResult reply = replies.front();
		replies.pop_front();
		return reply;
	}

	std::vector<Call> calls;
	std::deque<CommandResult> replies;
};

// A runner several threads call at once: each call answers `reply`, held while the
// gate is closed, and is counted.
class GatedRunner : public opennova::proc::CommandRunner {
public:
	explicit GatedRunner(CommandResult reply) : reply_(std::move(reply)) {}

	CommandResult run(const std::vector<std::string> &, int) override {
		std::unique_lock<std::mutex> lock(mu_);
		++calls_;
		cv_.notify_all();
		cv_.wait(lock, [this] { return open_; });
		return reply_;
	}
	void open() {
		{
			std::lock_guard<std::mutex> lock(mu_);
			open_ = true;
		}
		cv_.notify_all();
	}
	// Whether `n` calls came within `timeout`.
	bool wait_calls(int n, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mu_);
		return cv_.wait_for(lock, timeout, [this, n] { return calls_ >= n; });
	}
	int calls() {
		std::lock_guard<std::mutex> lock(mu_);
		return calls_;
	}

private:
	std::mutex mu_;
	std::condition_variable cv_;
	bool open_ = false;
	int calls_ = 0;
	const CommandResult reply_;
};

CommandResult answered(const std::string &out) {
	CommandResult r;
	r.outcome = CommandResult::Outcome::Exited;
	r.out = out;
	return r;
}

CommandResult failed(uint32_t code, const std::string &err) {
	CommandResult r;
	r.outcome = CommandResult::Outcome::Exited;
	r.exit_code = code;
	r.err = err;
	return r;
}

CommandResult timed_out() {
	CommandResult r;
	r.outcome = CommandResult::Outcome::TimedOut;
	return r;
}

CommandResult not_started() {
	CommandResult r;
	r.outcome = CommandResult::Outcome::NotStarted;
	r.error = "cannot start /usr/local/bin/aws: No such file or directory";
	return r;
}

std::vector<std::string> globals() {
	return { "--region", kRegion, "--output", "json", "--no-cli-pager", "--cli-connect-timeout",
		"10", "--cli-read-timeout", "30" };
}

std::vector<std::string> with_globals(std::vector<std::string> argv) {
	for (const std::string &g : globals())
		argv.push_back(g);
	return argv;
}

const std::vector<std::string> &describe_argv() {
	static const std::vector<std::string> argv =
			with_globals({ kAws, "ec2", "describe-instances", "--filters",
					"[{\"Name\":\"tag:opennova:hosted\",\"Values\":[\"1\"]},"
					"{\"Name\":\"tag:opennova:deployment\",\"Values\":[\"opennova-server\"]}]" });
	return argv;
}

std::vector<std::string> terminate_argv(const std::string &id) {
	return with_globals({ kAws, "ec2", "terminate-instances", "--instance-ids", id });
}

// The check before a terminate: that one machine, on both tags.
std::vector<std::string> describe_one_argv(const std::string &id) {
	return with_globals({ kAws, "ec2", "describe-instances", "--instance-ids", id, "--filters",
			"[{\"Name\":\"tag:opennova:hosted\",\"Values\":[\"1\"]},"
			"{\"Name\":\"tag:opennova:deployment\",\"Values\":[\"opennova-server\"]}]" });
}

std::string instance_json(const std::string &id, const std::string &state,
		const std::string &address, const std::string &machine,
		const std::string &public_address = "", const std::string &deployment = kDeployment,
		bool hosted_tag = true) {
	std::string tags = "{\"Key\":\"opennova:deployment\",\"Value\":\"" + deployment + "\"}";
	if (hosted_tag)
		tags += ",{\"Key\":\"opennova:hosted\",\"Value\":\"1\"}";
	if (!machine.empty())
		tags += ",{\"Key\":\"opennova:machine-id\",\"Value\":\"" + machine + "\"}";
	std::string out =
			"{\"InstanceId\":\"" + id + "\",\"State\":{\"Code\":16,\"Name\":\"" + state + "\"}";
	if (!address.empty())
		out += ",\"PrivateIpAddress\":\"" + address + "\"";
	if (!public_address.empty())
		out += ",\"PublicIpAddress\":\"" + public_address + "\"";
	return out + ",\"Tags\":[" + tags + "]}";
}

std::string listing(const std::vector<std::string> &instances) {
	std::string out = "{\"Reservations\":[";
	for (size_t i = 0; i < instances.size(); ++i) {
		if (i != 0)
			out += ",";
		out += "{\"ReservationId\":\"r-" + std::to_string(i) + "\",\"Instances\":[" + instances[i] +
				"]}";
	}
	return out + "]}";
}

std::string run_instances_reply(
		const std::string &id, const std::string &address, const std::string &type = "t3a.large") {
	std::string instance = "{\"InstanceId\":\"" + id + "\",\"InstanceType\":\"" + type +
			"\",\"State\":{\"Code\":0,\"Name\":\"pending\"}";
	if (!address.empty())
		instance += ",\"PrivateIpAddress\":\"" + address + "\"";
	instance += "}";
	return "{\"Groups\":[],\"Instances\":[" + instance +
			"],\"OwnerId\":\"123456789012\",\"ReservationId\":\"r-9\"}";
}

std::string machine_script() {
	std::ifstream in(fs::u8path(std::string(OPENNOVA_SOURCE_DIR) +
							 "/apps/novaworld_common/hosted/cloud/machine/machine_user_data.sh"),
			std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

hosted::Ec2MachineConfig config_in(const std::string &scratch) {
	hosted::Ec2MachineConfig c;
	c.aws_cli = kAws;
	c.region = kRegion;
	c.subnets = { kSubnet };
	c.deployment = kDeployment;
	c.shared_template = kShared;
	c.own_template = std::string(kOwn) + ":7";
	c.serve_image = kImage;
	c.cluster = kCluster;
	c.admin_address = kService;
	c.machine_script = machine_script();
	c.scratch_dir = scratch;
	c.describe_ttl_ms = kTtl;
	c.launch_grace_ms = kGrace;
	return c;
}

hosted::MachineLaunch launch_of(int64_t machine_id, MachineKind kind = MachineKind::Shared) {
	hosted::MachineLaunch launch;
	launch.machine_id = machine_id;
	launch.kind = kind;
	launch.slots = kind == MachineKind::Own ? 1 : 12;
	launch.bootstrap_token = hosted::random_secret(hosted::kMachineTokenChars);
	return launch;
}

// The value the rendered script assigns `name` ("NAME='value'").
std::string assigned(const std::string &script, const std::string &name) {
	const std::string head = name + "='";
	const size_t at = script.find(head);
	if (at == std::string::npos)
		return {};
	const size_t start = at + head.size();
	const size_t end = script.find('\'', start);
	return end == std::string::npos ? std::string() : script.substr(start, end - start);
}

bool traces_commands(const std::string &script) {
	std::istringstream lines(script);
	std::string line;
	while (std::getline(lines, line)) {
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos || line[start] == '#')
			continue;
		std::istringstream words(line.substr(start));
		std::string word;
		if (!(words >> word) || word != "set")
			continue;
		while (words >> word) {
			if (word == "xtrace")
				return true;
			if (word.size() > 1 && word[0] == '-' && word[1] != '-' &&
					word.find('x') != std::string::npos)
				return true;
		}
	}
	return false;
}

// Where in `script` a command line (comments aside) holds `needle`; npos for none.
size_t line_at(const std::string &script, const std::string &needle) {
	size_t pos = 0;
	while (pos < script.size()) {
		size_t end = script.find('\n', pos);
		if (end == std::string::npos)
			end = script.size();
		const std::string line = script.substr(pos, end - pos);
		const size_t start = line.find_first_not_of(" \t");
		if (start != std::string::npos && line[start] != '#' &&
				line.find(needle, start) != std::string::npos)
			return pos;
		pos = end + 1;
	}
	return std::string::npos;
}

bool runs_line(const std::string &script, const std::string &needle) {
	return line_at(script, needle) != std::string::npos;
}

bool is_alnum_text(const std::string &text) {
	for (const char c : text) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!ok)
			return false;
	}
	return !text.empty();
}

struct Rig {
	explicit Rig(const std::string &scratch) :
			driver(config_in(scratch), runner, [this] { return now; }) {}

	// Launches machine `machine_id` as `id` at `address`; the user-data it carried.
	std::string launch(int64_t machine_id, const std::string &id, const std::string &address) {
		runner.replies.push_back(answered(run_instances_reply(id, address)));
		std::string error;
		const auto machine = driver.launch(launch_of(machine_id), error);
		if (!machine || machine->cloud_id != id) {
			std::fprintf(stderr, "  launch failed: %s\n", error.c_str());
			return {};
		}
		return runner.calls.back().user_data;
	}

	uint64_t now = 5'000'000;
	StubRunner runner;
	hosted::Ec2MachineDriver driver;
};

// --- the EC2 driver ------------------------------------------------------------------------

int test_config_checks(const std::string &scratch) {
	const hosted::Ec2MachineConfig good = config_in(scratch);
	const std::string why = hosted::Ec2MachineDriver::check_config(good);
	if (!why.empty())
		std::fprintf(stderr, "  %s\n", why.c_str());
	TEST_EXPECT(why.empty());
	const auto refused = [&good](void (*spoil)(hosted::Ec2MachineConfig &)) {
		hosted::Ec2MachineConfig c = good;
		spoil(c);
		return !hosted::Ec2MachineDriver::check_config(c).empty();
	};
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.aws_cli = "aws"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.region = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.region = "US East 1"; }));
	// The fleet cluster: required, and nothing that could leave its quotes.
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.cluster = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.cluster = "opennova x"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.cluster = "opennova'x"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.cluster = std::string(256, 'c'); }));
	TEST_EXPECT(!refused([](hosted::Ec2MachineConfig &c) { c.cluster = "opennova_dev-fleet"; }));
	TEST_EXPECT(
			refused([](hosted::Ec2MachineConfig &c) { c.subnets = { "sg-0123456789abcdef0" }; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.subnets.clear(); }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.subnets = { kSubnet, "subnet-" }; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.subnets = { kSubnet, kSubnet }; }));
	TEST_EXPECT(!refused([](hosted::Ec2MachineConfig &c) { c.subnets = { kSubnet, kSubnetB }; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.deployment = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.deployment = "prod,dev"; }));
	// The longest deployment still makes a client token EC2 takes (64 at most).
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.deployment = std::string(33, 'd'); }));
	TEST_EXPECT(!refused([](hosted::Ec2MachineConfig &c) { c.deployment = std::string(32, 'd'); }));
	TEST_EXPECT(hosted::Ec2MachineDriver::client_token(std::string(32, 'd'), INT64_MAX, kSubnet)
						.size() <= 64);
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.launch_attempts = 0; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.shared_template = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.shared_template = "my-template"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.own_template = "lt-XYZ"; }));
	TEST_EXPECT(refused(
			[](hosted::Ec2MachineConfig &c) { c.own_template = "lt-0abcdef0123456789:07"; }));
	TEST_EXPECT(
			refused([](hosted::Ec2MachineConfig &c) { c.own_template = "lt-0abcdef0123456789:"; }));
	TEST_EXPECT(refused(
			[](hosted::Ec2MachineConfig &c) { c.own_template = "lt-0abcdef0123456789:$Old"; }));
	TEST_EXPECT(!refused(
			[](hosted::Ec2MachineConfig &c) { c.own_template = "lt-0abcdef0123456789:$Latest"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.serve_image = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.serve_image = "--privileged"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.serve_image = "ghcr.io/x y"; }));
	{
		// The internal listener's URL as the docker driver reads it.
		std::string host;
		uint16_t port = 0;
		TEST_EXPECT(hosted::Ec2MachineDriver::parse_service_url(kServiceUrl, host, port) &&
				host == "fleet.prod.opennova.internal" && port == 8070);
		for (const char *url : { "", "fleet:8070", "https://fleet:8070", "http://fleet",
					 "http://fleet:", "http://fleet:0", "http://fleet:70000",
					 "http://user@fleet:8070", "http://fleet:8070/", "http://con troller:8070",
					 "http://con'troller:8070", "http://:8070", "http://-fleet:8070",
					 "http://fleet:80x" })
			TEST_EXPECT(!hosted::Ec2MachineDriver::parse_service_url(url, host, port));
	}
	// The admin address is the fleet's to check (its profile), and the scratch
	// directory the worker's to make (the driver): every program asks check_config alike.
	TEST_EXPECT(!refused([](hosted::Ec2MachineConfig &c) { c.admin_address = "127.0.0.1"; }));
	TEST_EXPECT(!refused([](hosted::Ec2MachineConfig &c) { c.scratch_dir = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.machine_script = "#!/bin/bash\n"; }));
	TEST_EXPECT(refused(
			[](hosted::Ec2MachineConfig &c) { c.machine_script += std::string(16 * 1024, '#'); }));
	return 0;
}

int test_machine_script(const std::string &) {
	using hosted::Ec2MachineDriver;
	const std::string script = machine_script();
	TEST_EXPECT(!script.empty());
	for (const char *blank : { Ec2MachineDriver::kBlankCluster, Ec2MachineDriver::kBlankMachineId,
				 Ec2MachineDriver::kBlankMachineKind, Ec2MachineDriver::kBlankMachineToken })
		TEST_EXPECT(script.find(blank) != std::string::npos);
	TEST_EXPECT(!traces_commands(script));
	TEST_EXPECT(traces_commands("set -euxo pipefail\n") && traces_commands("  set -o xtrace\n"));
	TEST_EXPECT(runs_line(script, "umask 077"));
	// The token and the machine id: written by a builtin to owner-only files in an
	// owner-only directory, the token then forgotten.
	TEST_EXPECT(runs_line(script, "install -d -m 0700 /etc/opennova"));
	TEST_EXPECT(runs_line(script, "printf '%s\\n' \"$MACHINE_TOKEN\" > /etc/opennova/hostd.token"));
	TEST_EXPECT(runs_line(script, "printf '%s\\n' \"$MACHINE_ID\" > /etc/opennova/machine-id"));
	TEST_EXPECT(
			runs_line(script, "chmod 0600 /etc/opennova/hostd.token /etc/opennova/machine-id") &&
			runs_line(script, "unset MACHINE_TOKEN"));
	// Every slot's ports leave the ephemeral range, now and at every boot.
	TEST_EXPECT(runs_line(script, "readonly SLOT_PORTS=32768-32831"));
	TEST_EXPECT(runs_line(script, "sysctl -w \"net.ipv4.ip_local_reserved_ports=$SLOT_PORTS\""));
	TEST_EXPECT(runs_line(script, "/etc/sysctl.d/60-opennova-slots.conf"));
	// The two port ranges are slots.h's, all 32 slots of both.
	TEST_EXPECT(hosted::slot_game_port(0) == 32768 &&
			hosted::slot_admin_port(hosted::kMaxSlots - 1) == 32831);
	TEST_EXPECT(hosted::slot_uid(0) == 10001 && hosted::slot_uid(hosted::kMaxSlots - 1) == 10032);
	// The ECS agent's config, written before the agent starts: the fleet cluster, the
	// machine's id and kind as instance attributes (hostd checks its id against them),
	// task roles in host mode, no privileged task.
	TEST_EXPECT(runs_line(script, "cat > /etc/ecs/ecs.config <<CONFIG"));
	TEST_EXPECT(runs_line(script, "ECS_CLUSTER=$CLUSTER"));
	TEST_EXPECT(runs_line(script,
			"ECS_INSTANCE_ATTRIBUTES={\"machine-id\": \"$MACHINE_ID\", \"kind\": "
			"\"$MACHINE_KIND\"}"));
	TEST_EXPECT(runs_line(script, "ECS_ENABLE_TASK_IAM_ROLE=true"));
	TEST_EXPECT(runs_line(script, "ECS_ENABLE_TASK_IAM_ROLE_NETWORK_HOST=true"));
	TEST_EXPECT(runs_line(script, "ECS_DISABLE_PRIVILEGED=true"));
	// Never the cleanup of images ECS did not pull: it would delete the serve images.
	TEST_EXPECT(script.find("ECS_ENABLE_UNTRACKED_IMAGE_CLEANUP") == std::string::npos);
	// Nothing of the old machine: the image and its ECS daemon bring the rest.
	for (const char *gone : { "apt", "docker ", "iptables", "systemctl enable", "awscli",
				 "opennova-hostd.service", "journalctl", "docker pull", "fence" })
		TEST_EXPECT(!runs_line(script, gone));
	TEST_EXPECT(script.size() < Ec2MachineDriver::kMaxUserDataBytes - 1024);

	hosted::MachineUserData values;
	values.cluster = kCluster;
	values.machine_id = 12;
	values.kind = MachineKind::Own;
	values.bootstrap_token = hosted::random_secret(hosted::kMachineTokenChars);
	const auto rendered = Ec2MachineDriver::render_user_data(script, values);
	TEST_EXPECT(rendered);
	TEST_EXPECT(rendered->find("@OPENNOVA_") == std::string::npos);
	TEST_EXPECT(assigned(*rendered, "CLUSTER") == kCluster);
	TEST_EXPECT(assigned(*rendered, "MACHINE_ID") == "12");
	TEST_EXPECT(assigned(*rendered, "MACHINE_KIND") == "own");
	TEST_EXPECT(assigned(*rendered, "MACHINE_TOKEN") == values.bootstrap_token);
	// The token appears once, in its assignment.
	TEST_EXPECT(rendered->find(values.bootstrap_token) == rendered->rfind(values.bootstrap_token));
	// A value that could leave its quotes, or one outside its shape, is refused.
	const auto refuses = [&script, &values](void (*spoil)(hosted::MachineUserData &)) {
		hosted::MachineUserData v = values;
		spoil(v);
		return !Ec2MachineDriver::render_user_data(script, v);
	};
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.bootstrap_token[3] = '\''; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.bootstrap_token.pop_back(); }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.cluster = "x'; reboot; '"; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.cluster = ""; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.machine_id = 0; }));
	TEST_EXPECT(!Ec2MachineDriver::render_user_data("#!/bin/bash\n", values));
	return 0;
}

int test_slot_plan(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(std::string(rig.driver.name()) == "ec2");
	// The supervisor's view, made with no driver (no cloud call).
	const hosted::MachineProfile profile = hosted::Ec2MachineDriver::profile(config_in(scratch));
	TEST_EXPECT(profile.provider == "ec2" &&
			profile.boot_timeout_ms == hosted::Ec2MachineDriver::kBootTimeoutMs);
	for (int slot : { 0, 5, hosted::kMaxSlots - 1 }) {
		const hosted::SlotPlan plan = profile.slot_plan(slot);
		TEST_EXPECT(plan.game_port == 32768 + slot && plan.admin_port == 32800 + slot);
		// The fleet's network, not its one address: a replaced leader's new
		// address is in it, and the server keeps the admin.cfg its start rendered.
		TEST_EXPECT(plan.admin_allowed_ip == "10.44.*");
	}
	TEST_EXPECT(hosted::Ec2MachineDriver::admin_pattern("172.31.5.9") == "172.31.*");
	TEST_EXPECT(hosted::Ec2MachineDriver::admin_pattern("not-an-ip") == "not-an-ip");
	TEST_EXPECT(rig.runner.calls.empty());
	return 0;
}

int test_launch(const std::string &scratch) {
	Rig rig(scratch);
	rig.runner.replies.push_back(answered(run_instances_reply(kMachineA, "10.44.0.21")));
	const hosted::MachineLaunch launch = launch_of(12);
	std::string error;
	const auto machine = rig.driver.launch(launch, error);
	if (!machine)
		std::fprintf(stderr, "  launch failed: %s\n", error.c_str());
	TEST_EXPECT(
			machine && machine->cloud_id == kMachineA && machine->private_address == "10.44.0.21");
	TEST_EXPECT(machine->public_address.empty() && machine->instance_type == "t3a.large");
	TEST_EXPECT(machine->template_version == std::string(kShared) + ":$Default");
	TEST_EXPECT(rig.runner.calls.size() == 1);
	const Call &call = rig.runner.calls[0];
	const std::vector<std::string> &argv = call.argv;
	TEST_EXPECT(argv.size() == 15 + globals().size());
	// fleet.tf's service policy admits these tag keys alone.
	const std::string tags = "{\"Key\":\"opennova:hosted\",\"Value\":\"1\"},"
							 "{\"Key\":\"opennova:deployment\",\"Value\":\"opennova-server\"},"
							 "{\"Key\":\"opennova:machine-kind\",\"Value\":\"shared\"},"
							 "{\"Key\":\"opennova:machine-id\",\"Value\":\"12\"},"
							 "{\"Key\":\"Name\",\"Value\":\"opennova-server-shared-12\"}";
	const std::vector<std::string> head = { kAws, "ec2", "run-instances", "--launch-template",
		"{\"LaunchTemplateId\":\"lt-0123456789abcdef0\",\"Version\":\"$Default\"}", "--subnet-id",
		kSubnet, "--count", "1", "--client-token" };
	TEST_EXPECT(std::vector<std::string>(argv.begin(), argv.begin() + 10) == head);
	// The row's own client token in its subnet: the same for every call of its launch there.
	TEST_EXPECT(argv[10] == "opennova-server-m12-9abcdef0");
	TEST_EXPECT(argv[11] == "--tag-specifications");
	TEST_EXPECT(argv[12] ==
			"[{\"ResourceType\":\"instance\",\"Tags\":[" + tags +
					"]},{\"ResourceType\":\"volume\",\"Tags\":[" + tags + "]}]");
	TEST_EXPECT(argv[13] == "--user-data" && argv[14].rfind("file://", 0) == 0);
	TEST_EXPECT(std::vector<std::string>(argv.begin() + 15, argv.end()) == globals());
	// Short enough that a launch's calls stay under the supervisor's stall watchdog.
	TEST_EXPECT(call.timeout_ms == 12 * 1000);
	// The user-data: the machine script with this machine's values, and the file gone after.
	TEST_EXPECT(assigned(call.user_data, "MACHINE_ID") == "12");
	TEST_EXPECT(assigned(call.user_data, "MACHINE_TOKEN") == launch.bootstrap_token);
	// The fleet cluster its ECS agent joins, and its kind.
	TEST_EXPECT(assigned(call.user_data, "CLUSTER") == kCluster);
	TEST_EXPECT(assigned(call.user_data, "MACHINE_KIND") == "shared");
	TEST_EXPECT(!call.user_data.empty() && !fs::exists(fs::u8path(call.user_data_path)));
	// The token is in the user-data alone, never on the command line.
	for (const std::string &arg : argv)
		TEST_EXPECT(arg.find(launch.bootstrap_token) == std::string::npos);

	// Right after the launch EC2 may not list the machine yet: within the grace of its
	// launch as its row dates it, it reads Pending (its addresses the row's, from the
	// launch), and past the grace Gone.
	rig.runner.replies.push_back(answered(listing({})));
	const hosted::CloudMachine status = rig.driver.describe(kMachineA, 1000);
	TEST_EXPECT(status.state == CloudState::Pending);
	TEST_EXPECT(rig.runner.calls.size() == 2 && rig.runner.calls[1].argv == describe_argv());
	TEST_EXPECT(rig.driver.describe(kMachineA, static_cast<int64_t>(kGrace)).state ==
			CloudState::Pending);
	TEST_EXPECT(rig.driver.describe(kMachineA, kLong).state == CloudState::Gone);
	// Another worker's driver, which never launched it, judges it alike: the grace is the
	// row's, not a process's memory.
	{
		StubRunner other_runner;
		hosted::Ec2MachineDriver other(
				config_in(scratch), other_runner, [] { return uint64_t{ 9'000'000 }; });
		other_runner.replies.push_back(answered(listing({})));
		TEST_EXPECT(other.describe(kMachineA, 1000).state == CloudState::Pending);
		TEST_EXPECT(other.describe(kMachineA, kLong).state == CloudState::Gone);
		TEST_EXPECT(other.describe(kMachineA, -1).state == CloudState::Gone);
		TEST_EXPECT(other_runner.calls.size() == 1);
	}
	return 0;
}

// A run-instances the timeout cut short (its machine launched meanwhile) is made
// again with the same client token and user-data, and EC2's answer to it, the
// machine of the first call, is the launch's: one machine, never two.
int test_launch_retried(const std::string &scratch) {
	Rig rig(scratch);
	rig.runner.replies.push_back(timed_out());
	rig.runner.replies.push_back(answered(run_instances_reply(kMachineA, "10.44.0.21")));
	std::string error;
	const hosted::MachineLaunch launch = launch_of(31);
	const auto machine = rig.driver.launch(launch, error);
	TEST_EXPECT(machine && machine->cloud_id == kMachineA);
	TEST_EXPECT(rig.runner.calls.size() == 2);
	TEST_EXPECT(rig.runner.calls[0].argv == rig.runner.calls[1].argv);
	TEST_EXPECT(rig.runner.calls[1].argv[10] == "opennova-server-m31-9abcdef0");
	TEST_EXPECT(rig.runner.calls[0].user_data == rig.runner.calls[1].user_data &&
			assigned(rig.runner.calls[1].user_data, "MACHINE_TOKEN") == launch.bootstrap_token);
	TEST_EXPECT(!fs::exists(fs::u8path(rig.runner.calls[1].user_data_path)));
	return 0;
}

// `instance` (instance_json's) in `subnet`.
std::string in_subnet(const std::string &instance, const std::string &subnet) {
	return instance.substr(0, instance.size() - 1) + ",\"SubnetId\":\"" + subnet + "\"}";
}

// The subnet and the client token run-instances call `call` named.
std::pair<std::string, std::string> subnet_and_token(const Call &call) {
	return { call.argv.at(6), call.argv.at(10) };
}

// Two subnets, one per zone: a launch takes the one holding fewer of the deployment's
// live machines (ties to the first), a row's own machine's subnet first; a zone that
// cannot take it hands it to the other under that subnet's own token, while a call cut
// short is made again in its subnet with the same one.
int test_launch_zones(const std::string &scratch) {
	hosted::Ec2MachineConfig config = config_in(scratch);
	config.subnets = { kSubnet, kSubnetB };
	uint64_t now = 1'000'000;
	StubRunner runner;
	hosted::Ec2MachineDriver driver(config, runner, [&now] { return now; });
	const std::pair<std::string, std::string> first = { kSubnet, "opennova-server-m40-9abcdef0" };
	const std::pair<std::string, std::string> second = { kSubnetB, "opennova-server-m40-76543210" };
	const std::string capacity =
			"\nAn error occurred (InsufficientInstanceCapacity) when calling the RunInstances "
			"operation (reached max retries: 2): We currently do not have sufficient c7a.medium "
			"capacity in the Availability Zone you requested (us-east-1a).\n";
	const std::string unsupported =
			"\nAn error occurred (Unsupported) when calling the RunInstances operation: Your "
			"requested instance type (c7a.medium) is not supported in your requested Availability "
			"Zone (us-east-1e).\n";

	// One live machine in the first zone, one ending there, one ended in the second:
	// the second.
	runner.replies.push_back(answered(listing({
			in_subnet(instance_json(kMachineA, "running", "10.44.0.21", "7"), kSubnet),
			in_subnet(instance_json(kMachineB, "shutting-down", "10.44.0.22", "8"), kSubnet),
			in_subnet(instance_json("i-0ccccccccccccccc3", "terminated", "", "9"), kSubnetB),
	})));
	runner.replies.push_back(answered(run_instances_reply("i-0ddddddddddddddd4", "10.44.1.20")));
	std::string error;
	TEST_EXPECT(driver.launch(launch_of(40), error));
	TEST_EXPECT(runner.calls.size() == 2 && runner.calls[0].argv == describe_argv());
	TEST_EXPECT(subnet_and_token(runner.calls[1]) == second);
	// The listing is kept kTtl: the next launch counts the one just made, pending (one
	// live in each zone), so ties go to the first, with no describe.
	runner.replies.push_back(answered(run_instances_reply("i-0eeeeeeeeeeeeeee5", "10.44.0.23")));
	TEST_EXPECT(driver.launch(launch_of(41), error));
	TEST_EXPECT(runner.calls.size() == 3 &&
			subnet_and_token(runner.calls[2]) ==
					std::make_pair(
							std::string(kSubnet), std::string("opennova-server-m41-9abcdef0")));

	// No capacity in the first zone: the second, under its own token, with the same
	// user-data; the file gone after.
	now += kTtl;
	runner.calls.clear();
	runner.replies.push_back(answered(listing({})));
	runner.replies.push_back(failed(254, capacity));
	runner.replies.push_back(answered(run_instances_reply(kMachineA, "10.44.1.21")));
	const auto moved = driver.launch(launch_of(40), error);
	TEST_EXPECT(moved && moved->cloud_id == kMachineA && runner.calls.size() == 3);
	TEST_EXPECT(subnet_and_token(runner.calls[1]) == first);
	TEST_EXPECT(subnet_and_token(runner.calls[2]) == second);
	TEST_EXPECT(runner.calls[1].user_data == runner.calls[2].user_data &&
			!runner.calls[2].user_data.empty() &&
			!fs::exists(fs::u8path(runner.calls[2].user_data_path)));
	// Everything else the same call.
	std::vector<std::string> a = runner.calls[1].argv;
	std::vector<std::string> b = runner.calls[2].argv;
	a[6] = b[6] = a[10] = b[10] = "";
	TEST_EXPECT(a == b);

	// The type not offered in the first zone: the second likewise.
	now += kTtl;
	runner.calls.clear();
	runner.replies.push_back(answered(listing({})));
	runner.replies.push_back(failed(254, unsupported));
	runner.replies.push_back(answered(run_instances_reply(kMachineB, "10.44.1.22")));
	TEST_EXPECT(driver.launch(launch_of(40), error) && runner.calls.size() == 3);
	TEST_EXPECT(subnet_and_token(runner.calls[2]) == second);

	// Neither zone has room: the launch fails after one call in each.
	now += kTtl;
	runner.calls.clear();
	runner.replies.push_back(answered(listing({})));
	runner.replies.push_back(failed(254, capacity));
	runner.replies.push_back(failed(254, capacity));
	TEST_EXPECT(!driver.launch(launch_of(40), error));
	TEST_EXPECT(runner.calls.size() == 3 &&
			error.find("InsufficientInstanceCapacity") != std::string::npos);

	// Any other refusal is the launch's: no other zone.
	now += kTtl;
	runner.calls.clear();
	runner.replies.push_back(answered(listing({})));
	runner.replies.push_back(failed(254,
			"\nAn error occurred (UnauthorizedOperation) when calling the RunInstances "
			"operation: You are not authorized.\n"));
	TEST_EXPECT(!driver.launch(launch_of(40), error) && runner.calls.size() == 2);

	// A call cut short is made again in its subnet with the same token: a plain retry
	// stays idempotent.
	now += kTtl;
	runner.calls.clear();
	runner.replies.push_back(answered(listing({})));
	runner.replies.push_back(timed_out());
	runner.replies.push_back(answered(run_instances_reply(kMachineA, "10.44.0.21")));
	TEST_EXPECT(driver.launch(launch_of(40), error) && runner.calls.size() == 3);
	TEST_EXPECT(runner.calls[1].argv == runner.calls[2].argv);
	TEST_EXPECT(subnet_and_token(runner.calls[2]) == first);

	// A launch run again (its job's claim lapsed) whose machine EC2 lists already: that
	// machine's subnet first, so the same token, though the other zone holds fewer.
	now += kTtl;
	runner.calls.clear();
	runner.replies.push_back(answered(listing({
			in_subnet(instance_json(kMachineB, "pending", "10.44.1.22", "40"), kSubnetB),
			in_subnet(instance_json("i-0ccccccccccccccc3", "running", "10.44.1.23", "9"), kSubnetB),
	})));
	runner.replies.push_back(answered(run_instances_reply(kMachineB, "10.44.1.22")));
	TEST_EXPECT(driver.launch(launch_of(40), error) && runner.calls.size() == 2);
	TEST_EXPECT(subnet_and_token(runner.calls[1]) == second);

	// A run again on another worker, whose listing predates its first run's launch (that
	// launch in the first zone; the listing counts two live there, none in the second):
	// a describe-instances of its machine id alone, read now, finds that machine, so the
	// first zone's token again, never a second machine under the second's.
	{
		StubRunner other_runner;
		uint64_t other_now = 2'000'000;
		hosted::Ec2MachineDriver other(config, other_runner, [&other_now] { return other_now; });
		other_runner.replies.push_back(answered(listing({
				in_subnet(instance_json(kMachineA, "running", "10.44.0.21", "7"), kSubnet),
				in_subnet(instance_json("i-0ccccccccccccccc3", "running", "10.44.0.25", "9"),
						kSubnet),
		})));
		TEST_EXPECT(other.describe(kMachineA, kLong).state == CloudState::Running);
		other_now += 1000;
		const std::string first_run = "i-0ddddddddddddddd4";
		hosted::MachineLaunch again = launch_of(44);
		again.attempt = 2;
		other_runner.replies.push_back(answered(listing(
				{ in_subnet(instance_json(first_run, "pending", "10.44.0.26", "44"), kSubnet) })));
		other_runner.replies.push_back(answered(run_instances_reply(first_run, "10.44.0.26")));
		const auto relaunched = other.launch(again, error);
		TEST_EXPECT(
				relaunched && relaunched->cloud_id == first_run && other_runner.calls.size() == 3);
		TEST_EXPECT(other_runner.calls[1].argv ==
				with_globals({ kAws, "ec2", "describe-instances", "--filters",
						"[{\"Name\":\"tag:opennova:hosted\",\"Values\":[\"1\"]},"
						"{\"Name\":\"tag:opennova:deployment\",\"Values\":[\"opennova-server\"]},"
						"{\"Name\":\"tag:opennova:machine-id\",\"Values\":[\"44\"]}]" }));
		TEST_EXPECT(subnet_and_token(other_runner.calls[2]) ==
				std::make_pair(std::string(kSubnet), std::string("opennova-server-m44-9abcdef0")));
		// A first run there takes the second zone, which holds fewer.
		other_runner.replies.push_back(
				answered(run_instances_reply("i-0eeeeeeeeeeeeeee5", "10.44.1.27")));
		TEST_EXPECT(other.launch(launch_of(45), error) && other_runner.calls.size() == 4 &&
				subnet_and_token(other_runner.calls[3]).first == kSubnetB);
		// A run again that cannot read where its first run launched launches nothing.
		other_runner.replies.push_back(failed(255, "Could not connect to the endpoint URL"));
		hosted::MachineLaunch blind = launch_of(46);
		blind.attempt = 3;
		TEST_EXPECT(!other.launch(blind, error) && other_runner.calls.size() == 5 &&
				error.find("where its first run launched") != std::string::npos);
		// A run again whose row recorded its first run's call in the first zone (that
		// run's mark_launched failed; EC2 lists its machine late, so a describe of its
		// machine id would find none and the second zone, holding fewer, would launch a
		// second machine under its own token): the first zone and its token, with no
		// describe.
		hosted::MachineLaunch recorded_again = launch_of(48);
		recorded_again.attempt = 2;
		recorded_again.subnet = kSubnet;
		other_runner.replies.push_back(
				answered(run_instances_reply("i-0bbbbbbbbbbbbbbb8", "10.44.0.29")));
		TEST_EXPECT(other.launch(recorded_again, error) && other_runner.calls.size() == 6);
		TEST_EXPECT(subnet_and_token(other_runner.calls[5]) ==
				std::make_pair(std::string(kSubnet), std::string("opennova-server-m48-9abcdef0")));
	}

	// Each subnet goes on the row before its first call there (a zone without room
	// records the next one too); a subnet that cannot be recorded is called in
	// nothing, and the launch fails.
	now += kTtl;
	runner.calls.clear();
	std::vector<std::string> recorded;
	hosted::MachineLaunch recording = launch_of(47);
	recording.record_subnet = [&recorded](const std::string &subnet) {
		recorded.push_back(subnet);
		return true;
	};
	runner.replies.push_back(answered(listing({})));
	runner.replies.push_back(failed(254, capacity));
	runner.replies.push_back(answered(run_instances_reply("i-0aaaaaaaaaaaaaaa7", "10.44.1.28")));
	TEST_EXPECT(driver.launch(recording, error) && runner.calls.size() == 3);
	TEST_EXPECT(recorded == std::vector<std::string>({ kSubnet, kSubnetB }));
	recording.record_subnet = [](const std::string &) { return false; };
	runner.calls.clear();
	TEST_EXPECT(!driver.launch(recording, error) && runner.calls.empty() &&
			error.find("could not be recorded") != std::string::npos);

	// A listing that fails leaves the counts as they were read: the launch goes on.
	now += kTtl;
	runner.calls.clear();
	runner.replies.push_back(failed(255, "Could not connect to the endpoint URL"));
	runner.replies.push_back(answered(run_instances_reply("i-0fffffffffffffff6", "10.44.0.24")));
	TEST_EXPECT(driver.launch(launch_of(42), error) && runner.calls.size() == 2);
	TEST_EXPECT(runner.calls[1].argv.at(2) == "run-instances");
	return 0;
}

int test_launch_per_kind(const std::string &scratch) {
	hosted::Ec2MachineConfig config = config_in(scratch);
	uint64_t now = 1000;
	StubRunner runner;
	hosted::Ec2MachineDriver driver(config, runner, [&now] { return now; });
	struct {
		StubRunner &runner;
		hosted::Ec2MachineDriver &driver;
	} rig{ runner, driver };
	rig.runner.replies.push_back(answered(run_instances_reply(kMachineA, "10.44.0.21")));
	rig.runner.replies.push_back(
			answered(run_instances_reply(kMachineB, "10.44.0.22", "c7a.medium")));
	std::string error;
	const auto shared = rig.driver.launch(launch_of(1), error);
	const auto own = rig.driver.launch(launch_of(2, MachineKind::Own), error);
	TEST_EXPECT(shared && own && rig.runner.calls.size() == 2);
	TEST_EXPECT(rig.runner.calls[0].argv[4] ==
			"{\"LaunchTemplateId\":\"lt-0123456789abcdef0\",\"Version\":\"$Default\"}");
	// The own template pins version 7.
	TEST_EXPECT(rig.runner.calls[1].argv[4] ==
			"{\"LaunchTemplateId\":\"lt-0abcdef0123456789\",\"Version\":\"7\"}");
	TEST_EXPECT(own->template_version == "lt-0abcdef0123456789:7" &&
			own->instance_type == "c7a.medium");
	TEST_EXPECT(rig.runner.calls[1].argv[12].find("opennova-server-own-2") != std::string::npos);
	TEST_EXPECT(
			rig.runner.calls[1].argv[12].find(
					"{\"Key\":\"opennova:machine-kind\",\"Value\":\"own\"}") != std::string::npos);
	// Each machine's ECS attributes name its kind.
	TEST_EXPECT(assigned(rig.runner.calls[0].user_data, "MACHINE_KIND") == "shared");
	TEST_EXPECT(assigned(rig.runner.calls[1].user_data, "MACHINE_KIND") == "own");
	return 0;
}

int test_launch_failures(const std::string &scratch) {
	Rig rig(scratch);
	const auto launch_fails = [&rig](CommandResult reply, const char *expect) {
		rig.runner.replies.push_back(std::move(reply));
		const size_t first = rig.runner.calls.size();
		std::string error;
		const auto machine = rig.driver.launch(launch_of(7), error);
		if (machine || error.find(expect) == std::string::npos) {
			std::fprintf(stderr, "  launch error '%s', expected '%s'\n", error.c_str(), expect);
			return false;
		}
		// No user-data file stays behind.
		const Call &call = rig.runner.calls.at(first);
		return call.argv.size() > 2 && call.argv[2] == "run-instances" && !call.user_data.empty() &&
				!fs::exists(fs::u8path(call.user_data_path));
	};
	TEST_EXPECT(launch_fails(failed(254,
									 "\nAn error occurred (UnauthorizedOperation) when calling the "
									 "RunInstances operation: You are not authorized.\n"),
			"exited with code 254: An error occurred (UnauthorizedOperation)"));
	// EC2's own refusal (254) is final: one call. A call cut short is made again.
	TEST_EXPECT(rig.runner.calls.size() == 1);
	rig.runner.replies.push_back(timed_out());
	TEST_EXPECT(launch_fails(timed_out(), "timed out"));
	TEST_EXPECT(
			rig.runner.calls.size() == 3 && rig.runner.calls[1].argv == rig.runner.calls[2].argv);
	rig.runner.replies.push_back(failed(255, "Could not connect to the endpoint URL"));
	TEST_EXPECT(launch_fails(
			failed(255, "Could not connect to the endpoint URL"), "exited with code 255"));
	TEST_EXPECT(rig.runner.calls.size() == 5);
	TEST_EXPECT(launch_fails(not_started(), "did not start"));
	TEST_EXPECT(rig.runner.calls.size() == 6);
	TEST_EXPECT(launch_fails(answered("not json"), "answered no machine id"));
	TEST_EXPECT(launch_fails(answered("{}"), "answered no machine id"));
	TEST_EXPECT(launch_fails(answered("{\"Instances\":[]}"), "answered no machine id"));
	TEST_EXPECT(launch_fails(
			answered("{\"Instances\":[{\"InstanceId\":\"i-XYZ\"}]}"), "answered no machine id"));
	TEST_EXPECT(
			launch_fails(answered(std::string("{\"Instances\":[{\"InstanceId\":\"") + kMachineA +
								 "\",\"PrivateIpAddress\":\"10.44.0.21\"},{\"InstanceId\":\"" +
								 kMachineB + "\",\"PrivateIpAddress\":\"10.44.0.22\"}]}"),
					"answered no machine id"));
	// A machine with no private address: no agent of it could be admitted, so it ends at once.
	rig.runner.replies.push_back(answered(run_instances_reply(kMachineA, "")));
	rig.runner.replies.push_back(answered("{}"));
	{
		std::string error;
		const size_t first = rig.runner.calls.size();
		TEST_EXPECT(!rig.driver.launch(launch_of(7), error));
		TEST_EXPECT(error.find("with no private address") != std::string::npos);
		TEST_EXPECT(rig.runner.calls.size() == first + 2);
		TEST_EXPECT(rig.runner.calls.back().argv == terminate_argv(kMachineA));
	}
	const size_t calls = rig.runner.calls.size();
	TEST_EXPECT(rig.driver.describe(kMachineA, 0).state == CloudState::Gone);
	TEST_EXPECT(rig.runner.calls.size() == calls);
	// No machine id, or a token that is not one: nothing is called.
	std::string error;
	hosted::MachineLaunch none = launch_of(7);
	none.machine_id = 0;
	TEST_EXPECT(!rig.driver.launch(none, error) && !error.empty());
	hosted::MachineLaunch bad_token = launch_of(7);
	bad_token.bootstrap_token = "short";
	TEST_EXPECT(!rig.driver.launch(bad_token, error) &&
			error.find("did not render") != std::string::npos);
	TEST_EXPECT(rig.runner.calls.size() == calls);
	return 0;
}

int test_describe(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(!rig.launch(7, kMachineA, "10.44.0.21").empty());
	rig.runner.replies.push_back(answered(listing({
			instance_json(kMachineA, "running", "10.44.0.21", "7", "54.1.2.3"),
			instance_json(kMachineB, "pending", "10.44.0.22", "8"),
			instance_json("i-0ccccccccccccccc3", "shutting-down", "10.44.0.23", "9"),
			instance_json("i-0ddddddddddddddd4", "terminated", "", "10"),
			instance_json("i-0eeeeeeeeeeeeeee5", "stopped", "10.44.0.25", "11"),
			instance_json("i-0fffffffffffffff6", "running", "10.44.0.26", "12", "", "opennova-dev"),
			instance_json(
					"i-0aaaaaaaaaaaaaa07", "running", "10.44.0.27", "13", "", kDeployment, false),
	})));
	const hosted::CloudMachine a = rig.driver.describe(kMachineA, 0);
	TEST_EXPECT(a.state == CloudState::Running && a.private_address == "10.44.0.21");
	TEST_EXPECT(a.public_address == "54.1.2.3");
	TEST_EXPECT(rig.runner.calls.size() == 2 && rig.runner.calls[1].argv == describe_argv());
	TEST_EXPECT(rig.driver.describe(kMachineB, 0).state == CloudState::Pending);
	for (const char *gone : { "i-0ccccccccccccccc3", "i-0ddddddddddddddd4", "i-0eeeeeeeeeeeeeee5",
				 "i-0fffffffffffffff6", "i-0aaaaaaaaaaaaaa07", "i-0123456789abcdef9", "local-3-1" })
		TEST_EXPECT(rig.driver.describe(gone, kLong).state == CloudState::Gone);
	TEST_EXPECT(rig.driver.describe("i-0ccccccccccccccc3", kLong).private_address.empty());
	// All of that from the one describe-instances: the listing is kept kTtl.
	TEST_EXPECT(rig.runner.calls.size() == 2);
	rig.now += kTtl;
	rig.runner.replies.push_back(
			answered(listing({ instance_json(kMachineA, "shutting-down", "10.44.0.21", "7") })));
	TEST_EXPECT(rig.driver.describe(kMachineA, 0).state == CloudState::Gone);
	TEST_EXPECT(rig.runner.calls.size() == 3);
	return 0;
}

int test_describe_failures(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(!rig.launch(7, kMachineA, "10.44.0.21").empty());
	// Every failure reads Unknown, with the addresses last known, and is tried again kTtl later.
	for (CommandResult reply :
			{ failed(255, "Could not connect to the endpoint URL"), timed_out(), not_started(),
					answered("not json"), answered("{}"), answered("{\"Reservations\":{}}") }) {
		rig.runner.replies.push_back(std::move(reply));
		const size_t calls = rig.runner.calls.size();
		const hosted::CloudMachine status = rig.driver.describe(kMachineA, 0);
		TEST_EXPECT(rig.runner.calls.size() == calls + 1);
		TEST_EXPECT(status.state == CloudState::Unknown && status.private_address == "10.44.0.21");
		rig.now += kTtl - 1;
		TEST_EXPECT(rig.driver.describe(kMachineA, 0).state == CloudState::Unknown);
		TEST_EXPECT(rig.driver.describe(kMachineB, 0).state == CloudState::Unknown);
		TEST_EXPECT(rig.runner.calls.size() == calls + 1);
		rig.now += 1;
	}
	// The sweep's list_tagged reads fresh, back-off or not, and answers what EC2 lists
	// alone: a launch it does not list yet is the sweep's to describe with its row's age
	// (fleet_jobs.h), never this driver's memory.
	rig.runner.replies.push_back(
			answered(listing({ instance_json(kMachineB, "running", "10.44.0.22", "8") })));
	{
		const size_t calls = rig.runner.calls.size();
		const auto tagged = rig.driver.list_tagged();
		TEST_EXPECT(rig.runner.calls.size() == calls + 1 && tagged.size() == 1 &&
				tagged[0].cloud_id == kMachineB);
	}
	// A launched machine the listing lacks: Pending within the grace of its launch (its
	// row's age), Gone past it.
	rig.now += kTtl;
	rig.runner.replies.push_back(
			answered(listing({ instance_json(kMachineB, "running", "10.44.0.22", "8") })));
	TEST_EXPECT(rig.driver.describe(kMachineA, static_cast<int64_t>(kGrace)).state ==
			CloudState::Pending);
	TEST_EXPECT(rig.driver.describe(kMachineA, kLong).state == CloudState::Gone);
	return 0;
}

// Describes at once in one listing's window (the describe jobs on a worker's threads):
// one describe-instances runs, and the others wait for its answer rather than each
// running the CLI.
int test_describes_at_once(const std::string &scratch) {
	GatedRunner runner(
			answered(listing({ instance_json(kMachineA, "running", "10.44.0.21", "7") })));
	hosted::Ec2MachineDriver driver(
			config_in(scratch), runner, [] { return uint64_t{ 5'000'000 }; });
	std::atomic<int> running{ 0 };
	std::vector<std::thread> threads;
	for (int i = 0; i < 8; ++i) {
		threads.emplace_back([&driver, &running] {
			if (driver.describe(kMachineA, kLong).state == CloudState::Running)
				++running;
		});
	}
	const bool reading = runner.wait_calls(1, std::chrono::seconds(10));
	// The others arrive while it is read.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const int while_reading = runner.calls();
	runner.open();
	for (std::thread &t : threads)
		t.join();
	TEST_EXPECT(reading && while_reading == 1 && runner.calls() == 1);
	TEST_EXPECT(running.load() == 8);
	return 0;
}

int test_terminate(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(!rig.launch(7, kMachineA, "10.44.0.21").empty());
	// The machine's tags checked first (that machine, on both tags), then the end.
	rig.runner.replies.push_back(
			answered(listing({ instance_json(kMachineA, "running", "10.44.0.21", "7") })));
	rig.runner.replies.push_back(answered("{\"TerminatingInstances\":[]}"));
	size_t calls = rig.runner.calls.size();
	rig.driver.terminate(kMachineA);
	TEST_EXPECT(rig.runner.calls.size() == calls + 2);
	TEST_EXPECT(rig.runner.calls[calls].argv == describe_one_argv(kMachineA));
	TEST_EXPECT(rig.runner.calls.back().argv == terminate_argv(kMachineA));
	calls = rig.runner.calls.size();
	TEST_EXPECT(rig.driver.describe(kMachineA, 0).state == CloudState::Gone);
	rig.now += kTtl;
	TEST_EXPECT(rig.driver.describe(kMachineA, 0).state == CloudState::Gone);
	TEST_EXPECT(rig.runner.calls.size() == calls);
	// A machine EC2 no longer knows, at the check or at the end: no error, and Gone.
	rig.runner.replies.push_back(failed(254,
			"An error occurred (InvalidInstanceID.NotFound) when calling "
			"the DescribeInstances operation"));
	rig.driver.terminate(kMachineB);
	TEST_EXPECT(rig.runner.calls.size() == calls + 1);
	rig.runner.replies.push_back(answered(
			listing({ instance_json("i-0ddddddddddddddd4", "running", "10.44.0.24", "10") })));
	rig.runner.replies.push_back(failed(254,
			"An error occurred (InvalidInstanceID.NotFound) when calling "
			"the TerminateInstances operation"));
	rig.driver.terminate("i-0ddddddddddddddd4");
	calls = rig.runner.calls.size();
	TEST_EXPECT(rig.driver.describe(kMachineB, kLong).state == CloudState::Gone &&
			rig.driver.describe("i-0ddddddddddddddd4", kLong).state == CloudState::Gone &&
			rig.runner.calls.size() == calls);
	// One terminated already: nothing to end.
	rig.runner.replies.push_back(
			answered(listing({ instance_json("i-0eeeeeeeeeeeeeee5", "terminated", "", "11") })));
	rig.driver.terminate("i-0eeeeeeeeeeeeeee5");
	TEST_EXPECT(rig.runner.calls.size() == calls + 1);
	// One without both of this deployment's tags (another deployment's, or none):
	// never ended, whatever the row says.
	for (const std::string &foreign :
			{ listing({}), listing({ instance_json("i-0fffffffffffffff6", "running", "10.44.0.26",
									  "12", "", "opennova-dev") }),
					listing({ instance_json("i-0fffffffffffffff6", "running", "10.44.0.26", "12",
							"", kDeployment, false) }) }) {
		rig.runner.replies.push_back(answered(foreign));
		calls = rig.runner.calls.size();
		rig.driver.terminate("i-0fffffffffffffff6");
		TEST_EXPECT(rig.runner.calls.size() == calls + 1 &&
				rig.runner.calls.back().argv == describe_one_argv("i-0fffffffffffffff6"));
	}
	// A check that does not answer: not ended either (the sweep tries again).
	for (CommandResult reply : { failed(255, "throttled"), timed_out(), answered("not json") }) {
		rig.runner.replies.push_back(std::move(reply));
		calls = rig.runner.calls.size();
		rig.driver.terminate("i-0fffffffffffffff6");
		TEST_EXPECT(rig.runner.calls.size() == calls + 1);
	}
	// A refusal leaves the machine as EC2 says it is (the sweep tries again).
	rig.runner.replies.push_back(answered(
			listing({ instance_json("i-0ccccccccccccccc3", "running", "10.44.0.23", "9") })));
	rig.runner.replies.push_back(failed(254, "An error occurred (UnauthorizedOperation)"));
	rig.driver.terminate("i-0ccccccccccccccc3");
	rig.now += kTtl;
	rig.runner.replies.push_back(answered(
			listing({ instance_json("i-0ccccccccccccccc3", "running", "10.44.0.23", "9") })));
	TEST_EXPECT(rig.driver.describe("i-0ccccccccccccccc3", 0).state == CloudState::Running);
	// Another driver's id: no call at all.
	calls = rig.runner.calls.size();
	rig.driver.terminate("local-3-1");
	rig.driver.terminate("");
	TEST_EXPECT(rig.runner.calls.size() == calls);
	return 0;
}

int test_list_tagged(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(!rig.launch(7, kMachineA, "10.44.0.21").empty());
	rig.runner.replies.push_back(answered(
			listing({ instance_json("i-0aaaaaaaaaaaaaa07", "running", "10.44.0.27", "13") })));
	rig.runner.replies.push_back(answered("{}"));
	rig.driver.terminate("i-0aaaaaaaaaaaaaa07");
	const std::string all = listing({
			instance_json(kMachineA, "running", "10.44.0.21", "7"),
			instance_json(kMachineB, "pending", "10.44.0.22", "8"),
			instance_json("i-0ccccccccccccccc3", "stopping", "10.44.0.23", "x9"),
			instance_json("i-0ddddddddddddddd4", "stopped", "10.44.0.24", ""),
			instance_json("i-0eeeeeeeeeeeeeee5", "shutting-down", "10.44.0.25", "11"),
			instance_json("i-0fffffffffffffff6", "terminated", "", "12"),
			instance_json("i-0aaaaaaaaaaaaaa07", "running", "10.44.0.27", "13"),
			instance_json("i-0aaaaaaaaaaaaaa08", "running", "10.44.0.28", "14", "", "opennova-dev"),
	});
	for (int round = 0; round < 2; ++round) {
		rig.runner.replies.push_back(answered(all));
		const size_t calls = rig.runner.calls.size();
		const std::vector<hosted::TaggedCloudMachine> tagged = rig.driver.list_tagged();
		TEST_EXPECT(rig.runner.calls.size() == calls + 1 &&
				rig.runner.calls.back().argv == describe_argv());
		TEST_EXPECT(tagged.size() == 4);
		std::map<std::string, int64_t> by_id;
		for (const hosted::TaggedCloudMachine &m : tagged)
			by_id[m.cloud_id] = m.machine_id;
		TEST_EXPECT(by_id.at(kMachineA) == 7 && by_id.at(kMachineB) == 8);
		// A machine-id tag that is no number, or none: 0, which no row names.
		TEST_EXPECT(by_id.at("i-0ccccccccccccccc3") == 0 && by_id.at("i-0ddddddddddddddd4") == 0);
	}
	for (CommandResult reply : { failed(255, "throttled"), timed_out(), answered("[]") }) {
		rig.runner.replies.push_back(std::move(reply));
		bool threw = false;
		try {
			(void)rig.driver.list_tagged();
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TEST_EXPECT(threw);
	}
	return 0;
}

// --- the settings ------------------------------------------------------------------------------

hosted::EnvLookup env_of(const std::map<std::string, std::string> &vars) {
	return [vars](const char *name) -> std::optional<std::string> {
		const auto it = vars.find(name);
		if (it == vars.end() || it->second.empty())
			return std::nullopt;
		return it->second;
	};
}

int test_fleet_config() {
	// The defaults: a loopback internal listener admitting loopback, the brief's knobs;
	// the drivers' with no URL unless set (each driver's check asks for one).
	{
		hosted::FleetConfig c;
		std::vector<std::string> errors;
		TEST_EXPECT(hosted::parse_fleet_config(env_of({}), c, errors) && errors.empty());
		TEST_EXPECT(c.internal_address == "127.0.0.1" && c.internal_port == 8070);
		TEST_EXPECT(c.agent_cidrs.size() == 1 &&
				hosted::ipv4_cidr_text(c.agent_cidrs[0]) == "127.0.0.0/8");
		TEST_EXPECT(c.placement.shared_slots == 12 && c.placement.max_shared == 2 &&
				c.placement.max_own == 2);
		TEST_EXPECT(c.idle_minutes == 30 && c.slot_memory_mb == 512 && c.slot_cpus == 1.0);
		TEST_EXPECT(c.slot_pids == 128 && c.serve_image.empty() && c.image_id.empty());
		// No static addresses sold, and no zone.
		TEST_EXPECT(c.placement.max_addresses == 0 && c.address_zone.empty());
		hosted::DriverConfig d;
		TEST_EXPECT(hosted::parse_driver_config(env_of({}), c, d, errors) && errors.empty());
		TEST_EXPECT(d.machine_script_path == "/app/hosted/machine_user_data.sh");
		TEST_EXPECT(d.addresses.zone_id.empty() && d.addresses.zone_file.empty() &&
				d.addresses.docker_range.empty());
		TEST_EXPECT(d.internal_url.empty() && d.ec2.cluster.empty() &&
				d.docker.service_url.empty() && d.docker.engine_url.empty());
		TEST_EXPECT(d.ec2.admin_address == "127.0.0.1");
	}
	// A deployment's: every variable set, the policy reading its own and the drivers'
	// theirs, never the other set's.
	{
		const auto env = env_of({ { "ONNET_INTERNAL_HTTP_BIND", "10.44.0.10:8070" },
				{ "ONNET_INTERNAL_URL", kServiceUrl },
				{ "ONNET_HOSTED_BOOTSTRAP_CIDRS", "10.44.0.0/16, 10.45.0.0/20" },
				{ "ONNET_FLEET_SHARED_SLOTS", "16" }, { "ONNET_FLEET_MAX_SHARED", "3" },
				{ "ONNET_FLEET_MAX_OWN", "0" }, { "ONNET_FLEET_IDLE_MINUTES", "45" },
				{ "ONNET_FLEET_SLOT_MEMORY_MB", "768" }, { "ONNET_FLEET_SLOT_CPUS", "0.5" },
				{ "ONNET_HOSTED_EC2_LAUNCH_TEMPLATES",
						"{\"own\": \" lt-0abcdef0123456789:3 \", \"shared\": \"" +
								std::string(kShared) + "\"}" },
				{ "ONNET_HOSTED_EC2_REGION", kRegion }, { "ONNET_HOSTED_EC2_CLUSTER", kCluster },
				{ "ONNET_FLEET_IMAGE_ID", "ami-0123456789abcdef0" },
				{ "ONNET_HOSTED_EC2_SUBNETS", std::string(" ") + kSubnet + " , " + kSubnetB + "," },
				{ "ONNET_HOSTED_DEPLOYMENT", kDeployment }, { "ONNET_HOSTED_SERVE_IMAGE", kImage },
				{ "ONNET_HOSTED_HOSTD_IMAGE", kHostdImage },
				{ "ONNET_HOSTED_AWS_CLI", "/opt/aws/bin/aws" },
				{ "ONNET_HOSTED_MACHINE_SCRIPT", "/srv/machine.sh" },
				{ "ONNET_HOSTED_DOCKER_URL", " http://docker-api:2375 " },
				{ "ONNET_HOSTED_DOCKER_NETWORK", "opennova-dev" },
				{ "ONNET_HOSTED_DOCKER_DATA_FROM", "opennova-dev-machine-data-1" },
				{ "ONNET_FLEET_MAX_ADDRESSES", "4" },
				{ "ONNET_HOSTED_ADDRESS_ZONE", "play.opennova.net" },
				{ "ONNET_HOSTED_ADDRESS_ZONE_ID", "Z0123456789ABC" },
				{ "ONNET_HOSTED_ADDRESS_ZONE_FILE", "/var/lib/opennova/zone/play.localhost.zone" },
				{ "ONNET_HOSTED_DOCKER_ADDRESSES", "172.30.77.64/28" } });
		hosted::FleetConfig c;
		std::vector<std::string> errors;
		const bool ok = hosted::parse_fleet_config(env, c, errors);
		for (const std::string &e : errors)
			std::fprintf(stderr, "  %s\n", e.c_str());
		TEST_EXPECT(ok && errors.empty());
		TEST_EXPECT(c.internal_address == "10.44.0.10" && c.internal_port == 8070);
		// No loopback beside a private bind.
		TEST_EXPECT(c.agent_cidrs.size() == 2 &&
				hosted::ipv4_cidr_text(c.agent_cidrs[1]) == "10.45.0.0/20");
		TEST_EXPECT(c.placement.shared_slots == 16 && c.placement.max_shared == 3 &&
				c.placement.max_own == 0);
		TEST_EXPECT(c.idle_minutes == 45 && c.slot_memory_mb == 768 && c.slot_cpus == 0.5);
		TEST_EXPECT(c.serve_image == kImage && c.image_id == "ami-0123456789abcdef0" &&
				c.hostd_image == kHostdImage);
		// The static addresses' policy: how many are sold and the zone they are named in.
		TEST_EXPECT(c.placement.max_addresses == 4 && c.address_zone == "play.opennova.net");
		// What the agent routes take from it: its networks.
		const hosted::AgentRouteConfig routes = hosted::agent_route_config(c);
		TEST_EXPECT(routes.admitted.size() == 2);
		hosted::DriverConfig d;
		TEST_EXPECT(hosted::parse_driver_config(env, c, d, errors) && errors.empty());
		TEST_EXPECT(
				d.ec2.shared_template == kShared && d.ec2.own_template == "lt-0abcdef0123456789:3");
		TEST_EXPECT(d.ec2.region == kRegion &&
				d.ec2.subnets == std::vector<std::string>({ kSubnet, kSubnetB }) &&
				d.ec2.deployment == kDeployment);
		TEST_EXPECT(d.ec2.serve_image == kImage && d.ec2.aws_cli == "/opt/aws/bin/aws");
		TEST_EXPECT(d.ec2.cluster == kCluster);
		// The local machines reach the fleet by its URL; its admin links come from its bind.
		TEST_EXPECT(d.internal_url == kServiceUrl);
		TEST_EXPECT(d.ec2.admin_address == "10.44.0.10");
		TEST_EXPECT(d.machine_script_path == "/srv/machine.sh");
		// The docker driver's: its own three and its image (the hostd image), the
		// deployment, URL and bind as ec2's.
		TEST_EXPECT(d.docker.engine_url == "http://docker-api:2375" &&
				d.docker.network == "opennova-dev" &&
				d.docker.data_from == "opennova-dev-machine-data-1");
		TEST_EXPECT(d.docker.deployment == kDeployment && d.docker.image == kHostdImage &&
				d.docker.service_url == kServiceUrl && d.docker.admin_address == "10.44.0.10");
		// The static addresses' drivers' own: the zone's id (ec2), its file and the
		// addresses (docker).
		TEST_EXPECT(d.addresses.zone_id == "Z0123456789ABC" &&
				d.addresses.zone_file == "/var/lib/opennova/zone/play.localhost.zone" &&
				d.addresses.docker_range == "172.30.77.64/28");
	}
	// Every refusal names its variable and keeps the default, the policy's and the
	// drivers' alike.
	const auto refused = [](const char *name, const char *value) {
		hosted::FleetConfig c;
		const hosted::FleetConfig defaults;
		hosted::DriverConfig d;
		std::vector<std::string> errors;
		const bool ok = hosted::parse_fleet_config(env_of({ { name, value } }), c, errors) &&
				hosted::parse_driver_config(env_of({ { name, value } }), c, d, errors);
		const bool named = errors.size() == 1 && errors[0].find(name) != std::string::npos;
		if (ok || !named)
			std::fprintf(stderr, "  %s=%s was not refused by name\n", name, value);
		return !ok && named && c.placement.shared_slots == defaults.placement.shared_slots &&
				c.slot_memory_mb == defaults.slot_memory_mb && c.slot_cpus == defaults.slot_cpus &&
				c.internal_address == defaults.internal_address;
	};
	// The templates by kind, as fleet.tf writes them; never the old per-data-set map.
	TEST_EXPECT(refused("ONNET_HOSTED_EC2_LAUNCH_TEMPLATES", "lt-0123456789abcdef0"));
	TEST_EXPECT(refused("ONNET_HOSTED_EC2_LAUNCH_TEMPLATES", "[\"lt-0123456789abcdef0\"]"));
	TEST_EXPECT(refused(
			"ONNET_HOSTED_EC2_LAUNCH_TEMPLATES", "{\"jop_2_consumer\": \"lt-0123456789abcdef0\"}"));
	TEST_EXPECT(refused("ONNET_HOSTED_EC2_LAUNCH_TEMPLATES", "{\"shared\": 7}"));
	{
		std::string shared, own, why;
		TEST_EXPECT(hosted::parse_launch_templates(
				"{\"shared\": \"lt-0123456789abcdef0\"}", shared, own, why));
		TEST_EXPECT(shared == "lt-0123456789abcdef0" && own.empty());
	}
	TEST_EXPECT(refused("ONNET_INTERNAL_HTTP_BIND", "10.44.0.10"));
	TEST_EXPECT(refused("ONNET_INTERNAL_HTTP_BIND", "10.44.0.10:0"));
	TEST_EXPECT(refused("ONNET_INTERNAL_HTTP_BIND", "10.44.0.10:70000"));
	TEST_EXPECT(refused("ONNET_INTERNAL_HTTP_BIND", "0.0.0.0:8070"));
	TEST_EXPECT(refused("ONNET_INTERNAL_HTTP_BIND", "54.1.2.3:8070"));
	TEST_EXPECT(refused("ONNET_INTERNAL_HTTP_BIND", "localhost:8070"));
	TEST_EXPECT(refused("ONNET_INTERNAL_URL", "fleet:8070"));
	TEST_EXPECT(refused("ONNET_INTERNAL_URL", "http://fleet"));
	TEST_EXPECT(refused("ONNET_INTERNAL_URL", "http://fleet:8070/agent"));
	TEST_EXPECT(refused("ONNET_HOSTED_BOOTSTRAP_CIDRS", "0.0.0.0/0"));
	TEST_EXPECT(refused("ONNET_HOSTED_BOOTSTRAP_CIDRS", "10.44.0.0/16,54.0.0.0/8"));
	TEST_EXPECT(refused("ONNET_HOSTED_BOOTSTRAP_CIDRS", "10.44.0.1/16"));
	TEST_EXPECT(refused("ONNET_FLEET_SHARED_SLOTS", "0"));
	TEST_EXPECT(refused("ONNET_FLEET_SHARED_SLOTS", "33"));
	TEST_EXPECT(refused("ONNET_FLEET_SHARED_SLOTS", "12x"));
	TEST_EXPECT(refused("ONNET_FLEET_SHARED_SLOTS", "-1"));
	TEST_EXPECT(refused("ONNET_FLEET_MAX_SHARED", "65"));
	TEST_EXPECT(refused("ONNET_FLEET_MAX_OWN", " "));
	TEST_EXPECT(refused("ONNET_FLEET_IDLE_MINUTES", "0"));
	TEST_EXPECT(refused("ONNET_FLEET_SLOT_MEMORY_MB", "32"));
	TEST_EXPECT(refused("ONNET_FLEET_SLOT_CPUS", "0"));
	TEST_EXPECT(refused("ONNET_FLEET_SLOT_CPUS", "65"));
	TEST_EXPECT(refused("ONNET_FLEET_SLOT_CPUS", "1e3"));
	TEST_EXPECT(refused("ONNET_FLEET_SLOT_CPUS", ".5"));
	// A private bind with no network to admit is refused too (nothing could reach it).
	{
		hosted::FleetConfig c;
		std::vector<std::string> errors;
		TEST_EXPECT(!hosted::parse_fleet_config(
				env_of({ { "ONNET_INTERNAL_HTTP_BIND", "10.44.0.10:8070" } }), c, errors));
		TEST_EXPECT(errors.size() == 1 &&
				errors[0].find("ONNET_HOSTED_BOOTSTRAP_CIDRS") != std::string::npos);
	}
	// The networks the routes may answer.
	const auto allowed = [](const char *text) {
		const auto cidr = hosted::parse_ipv4_cidr(text);
		return cidr && hosted::agent_cidr_allowed(*cidr);
	};
	for (const char *ok :
			{ "10.44.0.0/16", "172.16.0.0/12", "192.168.1.0/24", "127.0.0.0/8", "127.0.0.1" })
		TEST_EXPECT(allowed(ok));
	for (const char *no :
			{ "0.0.0.0/0", "126.0.0.0/7", "54.0.0.0/8", "100.64.0.0/10", "169.254.0.0/16" })
		TEST_EXPECT(!allowed(no));
	return 0;
}

// --- the docker driver (the local stack's machines) ------------------------------------------

constexpr const char *kEngineUrl = "http://docker-api:2375";
constexpr const char *kNetwork = "opennova-dev";
constexpr const char *kDataFrom = "opennova-dev-machine-data-1";
constexpr const char *kDevDeployment = "dev";
constexpr const char *kMachineImage = "opennova-hostd:local";
constexpr const char *kDevServiceUrl = "http://novaworld-fleet:8070";

hosted::DockerMachineConfig docker_config() {
	hosted::DockerMachineConfig c;
	c.engine_url = kEngineUrl;
	c.network = kNetwork;
	c.data_from = kDataFrom;
	c.deployment = kDevDeployment;
	c.image = kMachineImage;
	c.service_url = kDevServiceUrl;
	c.admin_address = "172.30.0.10";
	return c;
}

// An engine in memory: the containers API's calls the driver makes, each recorded.
struct EngineCall {
	std::string method;
	std::string path;
	std::string body;
	std::string content_type;
};

struct FakeEngine {
	struct Container {
		std::string status = "created";
		std::string address;
		std::map<std::string, std::string> labels;
		std::string create_body;
		std::string archive;
	};
	std::vector<EngineCall> calls;
	std::map<std::string, Container> containers;
	int next_address = 2;
	bool down = false; // no answer at all
	int fail_create = 0; // a status for the create
	int fail_start = 0;
	bool start_without_address = false;

	hosted::DockerMachineDriver::EngineCall seam() {
		return [this](const std::string &method, const std::string &path, const std::string &body,
					   const std::string &content_type) {
			return answer(method, path, body, content_type);
		};
	}

	static hosted::EngineReply reply(int code, std::string body = {}) {
		hosted::EngineReply r;
		r.answered = true;
		r.code = code;
		r.body = std::move(body);
		return r;
	}

	// The container a path names: /containers/<name>/...
	static std::string name_in(const std::string &path) {
		const std::string head = "/containers/";
		const size_t end = path.find_first_of("/?", head.size());
		return path.substr(
				head.size(), end == std::string::npos ? std::string::npos : end - head.size());
	}

	std::string inspect_json(const Container &c) const {
		std::string labels;
		for (const auto &[k, v] : c.labels)
			labels += (labels.empty() ? "" : ",") + std::string("\"") + k + "\":\"" + v + "\"";
		return "{\"State\":{\"Status\":\"" + c.status + "\"},\"Config\":{\"Labels\":{" + labels +
				"}},\"NetworkSettings\":{\"Networks\":{\"" + kNetwork + "\":{\"IPAddress\":\"" +
				c.address + "\"}}}}";
	}

	hosted::EngineReply answer(const std::string &method, const std::string &path,
			const std::string &body, const std::string &content_type) {
		calls.push_back({ method, path, body, content_type });
		if (down) {
			hosted::EngineReply r;
			r.error = "connection refused";
			return r;
		}
		if (method == "POST" && path.rfind("/containers/create?name=", 0) == 0) {
			if (fail_create)
				return reply(fail_create, "{\"message\":\"No such image: opennova-serve:local\"}");
			const std::string name = path.substr(std::string("/containers/create?name=").size());
			if (containers.count(name))
				return reply(
						409, "{\"message\":\"Conflict. The container name is already in use\"}");
			Container c;
			c.create_body = body;
			opennova::io::JsonValue v;
			std::string error;
			if (opennova::io::json_parse(body, v, error))
				if (const auto *labels = v.get("Labels"); labels && labels->is_object())
					for (const auto &m : labels->object)
						c.labels[m.key] = m.value.string;
			containers[name] = c;
			return reply(201, "{\"Id\":\"0123abcd\",\"Warnings\":[]}");
		}
		if (path.rfind("/containers/json?", 0) == 0) {
			std::string out = "[";
			for (const auto &[name, c] : containers) {
				std::string labels;
				for (const auto &[k, v] : c.labels)
					labels += (labels.empty() ? "" : ",") + std::string("\"") + k + "\":\"" + v +
							"\"";
				out += (out.size() > 1 ? "," : "") + std::string("{\"Names\":[\"/") + name +
						"\"],\"State\":\"" + c.status + "\",\"Labels\":{" + labels + "}}";
			}
			return reply(200, out + "]");
		}
		const std::string name = name_in(path);
		const auto it = containers.find(name);
		if (it == containers.end())
			return reply(404, "{\"message\":\"No such container: " + name + "\"}");
		Container &c = it->second;
		if (method == "PUT" && path.find("/archive?") != std::string::npos) {
			c.archive = body;
			return reply(200);
		}
		if (method == "POST" && path == "/containers/" + name + "/start") {
			if (fail_start)
				return reply(fail_start, "{\"message\":\"failed to set up the container\"}");
			if (c.status == "running")
				return reply(304);
			c.status = "running";
			if (!start_without_address)
				c.address = "172.30.0." + std::to_string(next_address++);
			return reply(204);
		}
		if (method == "GET" && path == "/containers/" + name + "/json")
			return reply(200, inspect_json(c));
		if (method == "DELETE" && path == "/containers/" + name + "?force=1&v=1") {
			containers.erase(it);
			return reply(204);
		}
		return reply(400, "{\"message\":\"the fake engine does not know this call\"}");
	}

	size_t count(const std::string &method, const std::string &path_head) const {
		size_t n = 0;
		for (const auto &c : calls)
			n += c.method == method && c.path.rfind(path_head, 0) == 0;
		return n;
	}
};

// A ustar archive's members: name -> (mode, uid, its bytes), each header's checksum checked.
struct TarMember {
	unsigned mode = 0;
	unsigned uid = 0;
	char type = 0;
	std::string data;
};

std::optional<std::map<std::string, TarMember>> read_tar(const std::string &tar) {
	if (tar.size() % 512 != 0)
		return std::nullopt;
	std::map<std::string, TarMember> out;
	size_t at = 0;
	while (at + 512 <= tar.size()) {
		const std::string h = tar.substr(at, 512);
		if (h == std::string(512, '\0'))
			return out;
		unsigned sum = 0;
		for (size_t i = 0; i < 512; ++i)
			sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
		const auto octal = [&h](size_t from, size_t width) {
			return static_cast<unsigned>(std::strtoul(h.substr(from, width).c_str(), nullptr, 8));
		};
		if (octal(148, 8) != sum || h.compare(257, 5, "ustar") != 0)
			return std::nullopt;
		TarMember m;
		m.mode = octal(100, 8);
		m.uid = octal(108, 8);
		m.type = h[156];
		const size_t size = octal(124, 12);
		m.data = tar.substr(at + 512, size);
		out[h.substr(0, h.find('\0'))] = m;
		at += 512 + (size + 511) / 512 * 512;
	}
	return std::nullopt;
}

int test_docker_config_and_profile(const std::string &) {
	using Driver = hosted::DockerMachineDriver;
	TEST_EXPECT(Driver::check_config(docker_config()).empty());
	const auto refused = [](void (*change)(hosted::DockerMachineConfig &), const char *names) {
		hosted::DockerMachineConfig c = docker_config();
		change(c);
		const std::string why = Driver::check_config(c);
		if (why.find(names) == std::string::npos)
			std::fprintf(stderr, "  want a refusal naming '%s', got '%s'\n", names, why.c_str());
		return why.find(names) != std::string::npos;
	};
	TEST_EXPECT(refused([](auto &c) { c.engine_url = "unix:///var/run/docker.sock"; },
			"ONNET_HOSTED_DOCKER_URL"));
	TEST_EXPECT(refused(
			[](auto &c) { c.engine_url = "http://docker-api"; }, "ONNET_HOSTED_DOCKER_URL"));
	TEST_EXPECT(refused([](auto &c) { c.network.clear(); }, "ONNET_HOSTED_DOCKER_NETWORK"));
	TEST_EXPECT(refused([](auto &c) { c.network = "-net"; }, "ONNET_HOSTED_DOCKER_NETWORK"));
	TEST_EXPECT(refused([](auto &c) { c.data_from = "a b"; }, "ONNET_HOSTED_DOCKER_DATA_FROM"));
	TEST_EXPECT(refused([](auto &c) { c.deployment.clear(); }, "ONNET_HOSTED_DEPLOYMENT"));
	TEST_EXPECT(refused(
			[](auto &c) { c.deployment = std::string(33, 'd'); }, "ONNET_HOSTED_DEPLOYMENT"));
	TEST_EXPECT(refused([](auto &c) { c.image = "-v"; }, "ONNET_HOSTED_HOSTD_IMAGE"));
	TEST_EXPECT(refused([](auto &c) { c.service_url = "fleet:8070"; }, "ONNET_INTERNAL_URL"));

	// The slots' fixed ports (every machine has an address of its own), admitting
	// the fleet's network.
	const hosted::MachineProfile profile = Driver::profile(docker_config());
	TEST_EXPECT(profile.provider == "docker" && profile.boot_timeout_ms == Driver::kBootTimeoutMs);
	const hosted::SlotPlan plan = profile.slot_plan(3);
	TEST_EXPECT(plan.game_port == 32771 && plan.admin_port == 32803 &&
			plan.admin_allowed_ip == "172.30.*");

	// The machine: its name, the agent's command line, the create's body.
	TEST_EXPECT(Driver::machine_name("dev", 7) == "dev-machine-7");
	TEST_EXPECT(Driver::machine_command(docker_config(), 7) ==
			(std::vector<std::string>{ "--runtime", "process", "--serve",
					"/usr/local/bin/opennova-serve", "--service", kDevServiceUrl, "--machine", "7",
					"--token-file", "/run/opennova/hostd.token", "--state-dir", "/srv/serve/hostd",
					"--slots-root", "/srv/serve/slots", "--nft", "none", "--log-dir",
					"/var/log/opennova" }));
	hosted::MachineLaunch launch = launch_of(7, MachineKind::Own);
	opennova::io::JsonValue body;
	std::string error;
	TEST_EXPECT(
			opennova::io::json_parse(Driver::create_body(docker_config(), launch), body, error));
	TEST_EXPECT(body.get_string("Image", "") == kMachineImage);
	// The image's serve user, not its root.
	TEST_EXPECT(body.get_string("User", "") == "10001:10001");
	const auto *entrypoint = body.get("Entrypoint");
	TEST_EXPECT(entrypoint && entrypoint->array.size() == 1 &&
			entrypoint->array[0].string == "/usr/local/bin/opennova-hostd");
	const auto *cmd = body.get("Cmd");
	TEST_EXPECT(cmd && cmd->array.size() == Driver::machine_command(docker_config(), 7).size());
	const auto *labels = body.get("Labels");
	TEST_EXPECT(labels && labels->get_string("opennova.hosted", "") == "1" &&
			labels->get_string("opennova.deployment", "") == "dev" &&
			labels->get_string("opennova.machine-id", "") == "7" &&
			labels->get_string("opennova.machine-kind", "") == "own");
	const auto *host = body.get("HostConfig");
	TEST_EXPECT(host && host->get_string("NetworkMode", "") == kNetwork &&
			host->get_bool("Init", false));
	const auto *from = host ? host->get("VolumesFrom") : nullptr;
	TEST_EXPECT(from && from->array.size() == 1 &&
			from->array[0].string == std::string(kDataFrom));
	const auto *restart = host ? host->get("RestartPolicy") : nullptr;
	TEST_EXPECT(restart && restart->get_string("Name", "") == "no");
	const auto *networking = body.get("NetworkingConfig");
	const auto *endpoints = networking ? networking->get("EndpointsConfig") : nullptr;
	TEST_EXPECT(endpoints && endpoints->get(kNetwork) != nullptr);
	// The token is in no part of it.
	TEST_EXPECT(Driver::create_body(docker_config(), launch).find(launch.bootstrap_token) ==
			std::string::npos);

	// The token's archive: opennova/ (0700) and opennova/hostd.token (0600), both the
	// machine user's.
	const auto tar = read_tar(Driver::token_archive("tok-123"));
	TEST_EXPECT(tar && tar->size() == 2);
	if (tar && tar->size() == 2) {
		const TarMember &dir = tar->at("opennova/");
		const TarMember &file = tar->at("opennova/hostd.token");
		TEST_EXPECT(dir.type == '5' && dir.mode == 0700 && dir.uid == Driver::kMachineUid);
		TEST_EXPECT(file.type == '0' && file.mode == 0600 && file.uid == Driver::kMachineUid &&
				file.data == "tok-123");
	}
	return 0;
}

int test_docker_launch(const std::string &) {
	FakeEngine engine;
	hosted::DockerMachineDriver driver(docker_config(), engine.seam());
	TEST_EXPECT(std::string(driver.name()) == "docker");
	std::string error;
	TEST_EXPECT(!driver.launch(hosted::MachineLaunch{}, error) && !error.empty());

	// A launch: create, the token, start, its address.
	const hosted::MachineLaunch launch = launch_of(3);
	const auto machine = driver.launch(launch, error);
	TEST_EXPECT(machine && machine->cloud_id == "dev-machine-3");
	TEST_EXPECT(
			machine->private_address == "172.30.0.2" && machine->public_address == "172.30.0.2");
	TEST_EXPECT(
			machine->instance_type == "container" && machine->template_version == kMachineImage);
	TEST_EXPECT(engine.calls.size() == 4);
	TEST_EXPECT(engine.calls[0].method == "POST" &&
			engine.calls[0].path == "/containers/create?name=dev-machine-3" &&
			engine.calls[0].content_type == "application/json");
	TEST_EXPECT(engine.calls[1].method == "PUT" &&
			engine.calls[1].path == "/containers/dev-machine-3/archive?path=%2Frun&copyUIDGID=1" &&
			engine.calls[1].content_type == "application/x-tar");
	TEST_EXPECT(engine.calls[2].method == "POST" &&
			engine.calls[2].path == "/containers/dev-machine-3/start");
	TEST_EXPECT(engine.calls[3].method == "GET" &&
			engine.calls[3].path == "/containers/dev-machine-3/json");
	const auto tar = read_tar(engine.containers["dev-machine-3"].archive);
	TEST_EXPECT(tar && tar->count("opennova/hostd.token") &&
			tar->at("opennova/hostd.token").data == launch.bootstrap_token);
	// The token rides the archive alone.
	for (const auto &c : engine.calls)
		if (c.method != "PUT")
			TEST_EXPECT(c.path.find(launch.bootstrap_token) == std::string::npos &&
					c.body.find(launch.bootstrap_token) == std::string::npos);

	// Run again (its first run cut short): the container its first run made is this
	// row's, already running: no second one, no second start, its address read.
	engine.calls.clear();
	const auto again = driver.launch(launch, error);
	TEST_EXPECT(
			again && again->cloud_id == "dev-machine-3" && again->private_address == "172.30.0.2");
	TEST_EXPECT(engine.count("POST", "/containers/create") == 1 &&
			engine.count("POST", "/containers/dev-machine-3/start") == 0 &&
			engine.count("PUT", "/containers/") == 0 && engine.containers.size() == 1);
	// One its first run created but never started: given the token and started now.
	engine.containers["dev-machine-4"].status = "created";
	engine.containers["dev-machine-4"].labels = { { "opennova.hosted", "1" },
		{ "opennova.deployment", "dev" }, { "opennova.machine-id", "4" } };
	const auto created = driver.launch(launch_of(4), error);
	TEST_EXPECT(created && created->private_address == "172.30.0.3" &&
			!engine.containers["dev-machine-4"].archive.empty());
	// A name held by a container that is not this row's machine: refused, left alone.
	engine.containers["dev-machine-5"].labels = { { "opennova.hosted", "1" },
		{ "opennova.deployment", "dev" }, { "opennova.machine-id", "55" } };
	TEST_EXPECT(!driver.launch(launch_of(5), error) &&
			error.find("not this machine's") != std::string::npos);
	TEST_EXPECT(engine.containers.count("dev-machine-5") == 1);
	return 0;
}

int test_docker_launch_failures(const std::string &) {
	std::string error;
	{
		// The engine refuses the create: nothing is left.
		FakeEngine engine;
		engine.fail_create = 404;
		hosted::DockerMachineDriver driver(docker_config(), engine.seam());
		TEST_EXPECT(!driver.launch(launch_of(3), error));
		TEST_EXPECT(error == "create dev-machine-3: HTTP 404: No such image: opennova-serve:local");
		TEST_EXPECT(engine.containers.empty());
	}
	{
		// The start fails: the container is removed.
		FakeEngine engine;
		engine.fail_start = 500;
		hosted::DockerMachineDriver driver(docker_config(), engine.seam());
		TEST_EXPECT(!driver.launch(launch_of(3), error) &&
				error.rfind("start dev-machine-3: HTTP 500", 0) == 0);
		TEST_EXPECT(engine.containers.empty() &&
				engine.count("DELETE", "/containers/dev-machine-3") == 1);
	}
	{
		// It runs with no address on the network: removed, since its agent could
		// never be admitted.
		FakeEngine engine;
		engine.start_without_address = true;
		hosted::DockerMachineDriver driver(docker_config(), engine.seam());
		TEST_EXPECT(!driver.launch(launch_of(3), error) &&
				error == "dev-machine-3 has no address on opennova-dev (running)");
		TEST_EXPECT(engine.containers.empty());
	}
	{
		// No engine answers.
		FakeEngine engine;
		engine.down = true;
		hosted::DockerMachineDriver driver(docker_config(), engine.seam());
		TEST_EXPECT(!driver.launch(launch_of(3), error) &&
				error == "create dev-machine-3: no answer (connection refused)");
	}
	return 0;
}

int test_docker_describe_terminate_list(const std::string &) {
	FakeEngine engine;
	hosted::DockerMachineDriver driver(docker_config(), engine.seam());
	std::string error;
	const auto a = driver.launch(launch_of(3), error);
	const auto b = driver.launch(launch_of(4, MachineKind::Own), error);
	TEST_EXPECT(a && b);

	// describe: the engine's states, its address both of the row's.
	auto seen = driver.describe(a->cloud_id, 0);
	TEST_EXPECT(seen.state == CloudState::Running && seen.private_address == a->private_address &&
			seen.public_address == a->private_address);
	engine.containers["dev-machine-3"].status = "created";
	TEST_EXPECT(driver.describe(a->cloud_id, 0).state == CloudState::Pending);
	for (const char *gone : { "exited", "dead", "removing" }) {
		engine.containers["dev-machine-3"].status = gone;
		TEST_EXPECT(driver.describe(a->cloud_id, 0).state == CloudState::Gone);
	}
	engine.containers["dev-machine-3"].status = "running";
	TEST_EXPECT(driver.describe("dev-machine-99", 0).state == CloudState::Gone);
	engine.down = true;
	TEST_EXPECT(driver.describe(a->cloud_id, 0).state == CloudState::Unknown);

	// list_tagged: this deployment's machines, by both labels, in every state.
	bool threw = false;
	try {
		driver.list_tagged();
	} catch (const std::runtime_error &) {
		threw = true;
	}
	TEST_EXPECT(threw);
	engine.down = false;
	engine.containers["other-machine-1"].labels = { { "opennova.hosted", "1" },
		{ "opennova.deployment", "other" }, { "opennova.machine-id", "1" } };
	engine.calls.clear();
	auto tagged = driver.list_tagged();
	TEST_EXPECT(engine.calls.size() == 1 &&
			engine.calls[0].path ==
					"/containers/"
					"json?all=1&filters=%7B%22label%22%3A%5B%22opennova.hosted%3D1%22%2C%"
					"22opennova.deployment%3Ddev%22%5D%7D");
	TEST_EXPECT(tagged.size() == 2 && tagged[0].cloud_id == "dev-machine-3" &&
			tagged[0].machine_id == 3 && tagged[1].cloud_id == "dev-machine-4" &&
			tagged[1].machine_id == 4);

	// terminate: never a container that is not this deployment's machine; ours
	// removed with its volumes; one the engine no longer knows is no error.
	driver.terminate("other-machine-1");
	TEST_EXPECT(engine.containers.count("other-machine-1") == 1 &&
			engine.count("DELETE", "/containers/other-machine-1") == 0);
	driver.terminate(a->cloud_id);
	TEST_EXPECT(engine.containers.count("dev-machine-3") == 0 &&
			engine.count("DELETE", "/containers/dev-machine-3?force=1&v=1") == 1);
	driver.terminate(a->cloud_id);
	TEST_EXPECT(engine.count("DELETE", "/containers/dev-machine-3") == 1);
	tagged = driver.list_tagged();
	TEST_EXPECT(tagged.size() == 1 && tagged[0].machine_id == 4);
	return 0;
}

} // namespace

int main() {
	test_temp::TempDir temp("hosted_ec2_machine_driver");
	const std::string scratch = temp.root();
	struct Case {
		const char *name;
		int (*fn)(const std::string &);
	};
	const Case cases[] = {
		{ "config_checks", test_config_checks },
		{ "machine_script", test_machine_script },
		{ "slot_plan", test_slot_plan },
		{ "launch", test_launch },
		{ "launch_retried", test_launch_retried },
		{ "launch_zones", test_launch_zones },
		{ "launch_per_kind", test_launch_per_kind },
		{ "launch_failures", test_launch_failures },
		{ "describe", test_describe },
		{ "describe_failures", test_describe_failures },
		{ "describes_at_once", test_describes_at_once },
		{ "terminate", test_terminate },
		{ "list_tagged", test_list_tagged },
		{ "fleet_config", [](const std::string &) { return test_fleet_config(); } },
		{ "docker_config_and_profile", test_docker_config_and_profile },
		{ "docker_launch", test_docker_launch },
		{ "docker_launch_failures", test_docker_launch_failures },
		{ "docker_describe_terminate_list", test_docker_describe_terminate_list },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn(scratch) != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted machine drivers\n");
	return 0;
}
