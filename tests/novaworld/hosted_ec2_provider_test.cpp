// The cloud MachineProvider (apps/novaworld_server/hosted/ec2_provider.h) on a stub
// CommandRunner that records every AWS CLI argv and answers it from a script: the
// launch's run-instances (the exact argv, the user-data file with its bootstrap URL and
// token, deleted after), describe's state mapping from one cached describe-instances (the
// bundle fetch gating Running, the launch grace, the cache's lifetime, a failure reading
// Pending), terminate's idempotence, list_tagged's scope and its throw, and every CLI
// failure, timeout and malformed reply, and the describe's back-off after one. Then the
// bootstrap tokens, the CIDR reads and the refusal log's throttle (hosted/bootstrap.h),
// the bundle's JSON, the shipped machine script's render and metadata fence, and the
// real ProcessCommandRunner against this binary itself (`--child <mode>`). Crow-free:
// compiles the hosted sources it needs directly, so it runs on every build.

#include "hosted/bootstrap.h"
#include "hosted/command_runner.h"
#include "hosted/ec2_provider.h"
#include "hosted/hosted_settings.h"
#include "hosted/owner_file.h"

#include "child_process.h"

#include <base/io/base64.h>
#include <base/io/json.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace fs = std::filesystem;
namespace hosted = opennova::novaworld_server::hosted;
using hosted::CommandResult;
using hosted::MachineState;

namespace {

std::string g_self;

constexpr const char *kAws = "/usr/local/bin/aws";
constexpr const char *kRegion = "us-east-1";
constexpr const char *kTemplate = "lt-0123456789abcdef0";
constexpr const char *kSubnet = "subnet-0123456789abcdef0";
constexpr const char *kDeployment = "opennova-server";
constexpr const char *kImage =
		"ghcr.io/opennova-net/opennova-serve@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char *kService = "10.44.0.10";
constexpr const char *kMachineA = "i-0aaaaaaaaaaaaaaa1";
constexpr const char *kMachineB = "i-0bbbbbbbbbbbbbbb2";
constexpr uint64_t kTtl = 10 * 1000;
constexpr uint64_t kGrace = 3 * 60 * 1000;

// --- the stub ----------------------------------------------------------------------------

struct Call {
	std::vector<std::string> argv;
	int timeout_ms = 0;
	std::string user_data;      // the --user-data file's text while the call ran
	std::string user_data_path; // its path
};

class StubRunner : public hosted::CommandRunner {
public:
	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		Call call;
		call.argv = argv;
		call.timeout_ms = timeout_ms;
		for (size_t i = 0; i + 1 < argv.size(); ++i) {
			if (argv[i] == "--user-data" && argv[i + 1].rfind("file://", 0) == 0) {
				call.user_data_path = argv[i + 1].substr(7);
				std::ifstream in(fs::u8path(call.user_data_path), std::ios::binary);
				call.user_data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

CommandResult answered(const std::string &out) {
	CommandResult r;
	r.outcome = CommandResult::Outcome::Exited;
	r.exit_code = 0;
	r.out = out;
	return r;
}

CommandResult failed(int code, const std::string &err) {
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

// The global options every call ends with.
std::vector<std::string> globals() {
	return { "--region", kRegion, "--output", "json", "--no-cli-pager", "--cli-connect-timeout", "10",
		"--cli-read-timeout", "30" };
}

std::vector<std::string> with_globals(std::vector<std::string> argv) {
	for (const std::string &g : globals()) argv.push_back(g);
	return argv;
}

const std::vector<std::string> &describe_argv() {
	static const std::vector<std::string> argv = with_globals({ kAws, "ec2", "describe-instances", "--filters",
			"[{\"Name\":\"tag:opennova:hosted\",\"Values\":[\"1\"]},"
			"{\"Name\":\"tag:opennova:deployment\",\"Values\":[\"opennova-server\"]}]" });
	return argv;
}

std::vector<std::string> terminate_argv(const std::string &id) {
	return with_globals({ kAws, "ec2", "terminate-instances", "--instance-ids", id });
}

// One instance of a describe-instances reply.
std::string instance_json(const std::string &id, const std::string &state, const std::string &address,
		const std::string &server, const std::string &deployment = kDeployment, bool hosted_tag = true) {
	std::string tags = "{\"Key\":\"opennova:deployment\",\"Value\":\"" + deployment + "\"}";
	if (hosted_tag) tags += ",{\"Key\":\"opennova:hosted\",\"Value\":\"1\"}";
	if (!server.empty()) tags += ",{\"Key\":\"opennova:server-id\",\"Value\":\"" + server + "\"}";
	std::string out = "{\"InstanceId\":\"" + id + "\",\"State\":{\"Code\":16,\"Name\":\"" + state + "\"}";
	if (!address.empty()) out += ",\"PrivateIpAddress\":\"" + address + "\"";
	return out + ",\"Tags\":[" + tags + "]}";
}

// A describe-instances reply: one reservation per instance.
std::string listing(const std::vector<std::string> &instances) {
	std::string out = "{\"Reservations\":[";
	for (size_t i = 0; i < instances.size(); ++i) {
		if (i != 0) out += ",";
		out += "{\"ReservationId\":\"r-" + std::to_string(i) + "\",\"Instances\":[" + instances[i] + "]}";
	}
	return out + "]}";
}

std::string run_instances_reply(const std::string &id, const std::string &address) {
	std::string instance = "{\"InstanceId\":\"" + id + "\",\"State\":{\"Code\":0,\"Name\":\"pending\"}";
	if (!address.empty()) instance += ",\"PrivateIpAddress\":\"" + address + "\"";
	instance += "}";
	return "{\"Groups\":[],\"Instances\":[" + instance + "],\"OwnerId\":\"123456789012\",\"ReservationId\":\"r-9\"}";
}

std::string machine_script() {
	std::ifstream in(fs::u8path(std::string(OPENNOVA_SOURCE_DIR) + "/apps/novaworld_server/hosted/machine/user_data.sh"),
			std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

hosted::Ec2Config config_in(const std::string &scratch) {
	hosted::Ec2Config c;
	c.aws_cli = kAws;
	c.region = kRegion;
	c.launch_template = kTemplate;
	c.subnet = kSubnet;
	c.deployment = kDeployment;
	c.serve_image = kImage;
	c.service_address = kService;
	c.bootstrap_port = 8080;
	c.machine_script = machine_script();
	c.scratch_dir = scratch;
	c.describe_ttl_ms = kTtl;
	c.launch_grace_ms = kGrace;
	c.command_timeout_ms = 90 * 1000;
	return c;
}

hosted::LaunchSpec spec_for(int64_t server_id) {
	hosted::LaunchSpec spec;
	spec.server_id = server_id;
	spec.files.push_back({ "server.host", "GameName \"Caf\xE9\"\r\nMission a.bms 0\r\n", true });
	spec.files.push_back({ "game.cfg", "version = \"29\"\n", false });
	spec.files.push_back({ "credentials.txt", "NOVAWORLD_USER=srv-7\nNOVAWORLD_PASS=secretpassword\n", true });
	spec.serve_args = { "/HOST", "server.host", "--credentials", "credentials.txt", "--master-host", "54.0.0.1",
		"--master-gate-port", "7597" };
	return spec;
}

// The value the rendered script assigns `name` ("NAME='value'").
std::string assigned(const std::string &script, const std::string &name) {
	const std::string head = name + "='";
	const size_t at = script.find(head);
	if (at == std::string::npos) return {};
	const size_t start = at + head.size();
	const size_t end = script.find('\'', start);
	return end == std::string::npos ? std::string() : script.substr(start, end - start);
}

// Whether a command line of `script` (comments aside) turns the shell's xtrace on:
// a `set` whose short flags hold x, or `set -o xtrace`.
bool traces_commands(const std::string &script) {
	std::istringstream lines(script);
	std::string line;
	while (std::getline(lines, line)) {
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos || line[start] == '#') continue;
		std::istringstream words(line.substr(start));
		std::string word;
		if (!(words >> word) || word != "set") continue;
		while (words >> word) {
			if (word == "xtrace") return true;
			if (word.size() > 1 && word[0] == '-' && word[1] != '-' && word.find('x') != std::string::npos)
				return true;
		}
	}
	return false;
}

// Whether a command line of `script` (comments aside) holds `needle`.
bool runs_line(const std::string &script, const std::string &needle) {
	std::istringstream lines(script);
	std::string line;
	while (std::getline(lines, line)) {
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos || line[start] == '#') continue;
		if (line.find(needle, start) != std::string::npos) return true;
	}
	return false;
}

bool is_alnum_text(const std::string &text) {
	for (const char c : text) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!ok) return false;
	}
	return !text.empty();
}

// A provider on a stub and a hand-stepped clock.
struct Rig {
	explicit Rig(const std::string &scratch) :
			bootstrap([this] { return now; }), provider(config_in(scratch), runner, bootstrap, [this] { return now; }) {}

	// Launches server `server_id` as `id` at `address`; the token its user-data carried.
	std::string launch(int64_t server_id, const std::string &id, const std::string &address) {
		runner.replies.push_back(answered(run_instances_reply(id, address)));
		std::string error;
		const auto machine = provider.launch(spec_for(server_id), error);
		if (!machine || *machine != id) {
			std::fprintf(stderr, "  launch failed: %s\n", error.c_str());
			return {};
		}
		return assigned(runner.calls.back().user_data, "BOOTSTRAP_TOKEN");
	}

	uint64_t now = 5'000'000;
	StubRunner runner;
	hosted::BootstrapStore bootstrap;
	hosted::Ec2Provider provider;
};

// --- the provider ---------------------------------------------------------------------------

int test_config_checks(const std::string &scratch) {
	const hosted::Ec2Config good = config_in(scratch);
	TEST_EXPECT(hosted::Ec2Provider::check_config(good).empty());
	const auto refused = [&good](void (*spoil)(hosted::Ec2Config &)) {
		hosted::Ec2Config c = good;
		spoil(c);
		return !hosted::Ec2Provider::check_config(c).empty();
	};
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.aws_cli = "aws"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.region = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.region = "US East 1"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.launch_template = "lt-XYZ"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.launch_template = "my-template"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.subnet = "sg-0123456789abcdef0"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.deployment = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.deployment = "prod,dev"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.serve_image = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.serve_image = "ghcr.io/x y"; }));
	// No reference docker would read as an option.
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.serve_image = "--privileged"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.serve_image = "-ghcr.io/opennova-net/opennova-serve"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.service_address = "10.44.0"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.service_address = "ip-10-44-0-10"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.machine_script = "#!/bin/bash\n"; }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.machine_script += std::string(16 * 1024, '#'); }));
	TEST_EXPECT(refused([](hosted::Ec2Config &c) { c.scratch_dir = ""; }));
	return 0;
}

int test_machine_script() {
	const std::string script = machine_script();
	TEST_EXPECT(!script.empty());
	TEST_EXPECT(script.find(hosted::Ec2Provider::kUrlBlank) != std::string::npos);
	TEST_EXPECT(script.find(hosted::Ec2Provider::kTokenBlank) != std::string::npos);
	// Nothing may trace the token to the console: no `set` command line turns xtrace on.
	TEST_EXPECT(!traces_commands(script));
	TEST_EXPECT(traces_commands("set -euxo pipefail\n") && traces_commands("  set -o xtrace\n"));
	TEST_EXPECT(!traces_commands("# never set -x\nset -euo pipefail\n"));
	// The server shares the machine's network (and so its metadata service, the role's
	// credentials): it runs as UID 10001, and that UID is refused every packet to the
	// metadata addresses at every start of its unit, and checked before the first.
	TEST_EXPECT(runs_line(script, "readonly SERVE_UID=10001"));
	TEST_EXPECT(runs_line(script, "--network host --user 10001:10001"));
	TEST_EXPECT(runs_line(script, "iptables -I OUTPUT -d 169.254.169.254 -m owner --uid-owner 10001 -j REJECT"));
	TEST_EXPECT(runs_line(script, "ip6tables -I OUTPUT -d fd00:ec2::254 -m owner --uid-owner 10001 -j REJECT"));
	TEST_EXPECT(runs_line(script, "ExecStartPre=/usr/local/sbin/opennova-serve-fence"));
	TEST_EXPECT(runs_line(script, "install --no-install-recommends docker.io iptables"));
	const size_t fence_check = script.find("still reaches the metadata service\"");
	const size_t first_start = script.find("systemctl start --no-block opennova-serve.service");
	TEST_EXPECT(fence_check != std::string::npos && first_start != std::string::npos && fence_check < first_start);
	TEST_EXPECT(!runs_line("# iptables -I OUTPUT -d 169.254.169.254\n", "iptables -I OUTPUT"));

	const std::string url = hosted::Ec2Provider::bootstrap_url(kService, 8080, 7);
	TEST_EXPECT(url == "http://10.44.0.10:8080/hosted/bootstrap/7");
	const std::string token = hosted::random_secret(hosted::BootstrapStore::kTokenChars);
	const auto rendered = hosted::Ec2Provider::render_user_data(script, url, token);
	TEST_EXPECT(rendered);
	TEST_EXPECT(rendered->find("@OPENNOVA_") == std::string::npos);
	TEST_EXPECT(assigned(*rendered, "BOOTSTRAP_URL") == url);
	TEST_EXPECT(assigned(*rendered, "BOOTSTRAP_TOKEN") == token);
	TEST_EXPECT(rendered->size() <= hosted::Ec2Provider::kMaxUserDataBytes);
	// A value that could leave its single quotes is refused, as is a script with no blank.
	TEST_EXPECT(!hosted::Ec2Provider::render_user_data(script, url, "abc'def"));
	TEST_EXPECT(!hosted::Ec2Provider::render_user_data(script, url, ""));
	TEST_EXPECT(!hosted::Ec2Provider::render_user_data(script, "http://10.0.0.1/a b", token));
	TEST_EXPECT(!hosted::Ec2Provider::render_user_data(script, "http://10.0.0.1/'", token));
	TEST_EXPECT(!hosted::Ec2Provider::render_user_data("#!/bin/bash\n", url, token));
	return 0;
}

int test_plan(const std::string &scratch) {
	Rig rig(scratch);
	const hosted::MachinePlan plan = rig.provider.plan(7);
	TEST_EXPECT(plan.nw_port == 32768 && plan.admin_port == 32800);
	TEST_EXPECT(plan.admin_allowed_ip == kService);
	TEST_EXPECT(std::string(rig.provider.name()) == "ec2");
	TEST_EXPECT(rig.runner.calls.empty());
	return 0;
}

int test_launch(const std::string &scratch) {
	Rig rig(scratch);
	rig.provider.set_bootstrap_port(8081);
	rig.runner.replies.push_back(answered(run_instances_reply(kMachineA, "10.44.0.21")));
	std::string error;
	const auto machine = rig.provider.launch(spec_for(7), error);
	if (!machine) std::fprintf(stderr, "  launch failed: %s\n", error.c_str());
	TEST_EXPECT(machine && *machine == kMachineA);
	TEST_EXPECT(rig.runner.calls.size() == 1);
	const Call &call = rig.runner.calls[0];
	const std::vector<std::string> &argv = call.argv;
	TEST_EXPECT(argv.size() == 15 + globals().size());
	const std::string tags = "{\"Key\":\"opennova:hosted\",\"Value\":\"1\"},"
							 "{\"Key\":\"opennova:deployment\",\"Value\":\"opennova-server\"},"
							 "{\"Key\":\"opennova:server-id\",\"Value\":\"7\"},"
							 "{\"Key\":\"Name\",\"Value\":\"opennova-server-hosted-7\"}";
	const std::vector<std::string> head = { kAws, "ec2", "run-instances", "--launch-template",
		"{\"LaunchTemplateId\":\"lt-0123456789abcdef0\",\"Version\":\"$Default\"}", "--subnet-id", kSubnet, "--count",
		"1", "--client-token" };
	TEST_EXPECT(std::vector<std::string>(argv.begin(), argv.begin() + 10) == head);
	// A client token of its own per launch (a retried call launches once).
	TEST_EXPECT(argv[10].rfind("opennova-", 0) == 0 && argv[10].size() == 41 && is_alnum_text(argv[10].substr(9)));
	TEST_EXPECT(argv[11] == "--tag-specifications");
	TEST_EXPECT(argv[12] == "[{\"ResourceType\":\"instance\",\"Tags\":[" + tags +
					"]},{\"ResourceType\":\"volume\",\"Tags\":[" + tags + "]}]");
	TEST_EXPECT(argv[13] == "--user-data" && argv[14].rfind("file://", 0) == 0);
	TEST_EXPECT(std::vector<std::string>(argv.begin() + 15, argv.end()) == globals());
	TEST_EXPECT(call.timeout_ms == 90 * 1000);
	// The user-data: the machine script, its URL and token filled in, and nothing of the spec.
	TEST_EXPECT(assigned(call.user_data, "BOOTSTRAP_URL") == "http://10.44.0.10:8081/hosted/bootstrap/7");
	const std::string token = assigned(call.user_data, "BOOTSTRAP_TOKEN");
	TEST_EXPECT(token.size() == hosted::BootstrapStore::kTokenChars && is_alnum_text(token));
	TEST_EXPECT(call.user_data.find("secretpassword") == std::string::npos);
	TEST_EXPECT(call.user_data.find("srv-7") == std::string::npos);
	// The file was there for the call and is gone after it.
	TEST_EXPECT(!call.user_data.empty() && !fs::exists(fs::u8path(call.user_data_path)));

	// The token: bound to its server and its machine's address, spent once.
	std::string why;
	TEST_EXPECT(rig.bootstrap.live_tokens() == 1);
	TEST_EXPECT(!rig.bootstrap.redeem(7, token, "10.44.0.99", why));
	TEST_EXPECT(!rig.bootstrap.redeem(8, token, "10.44.0.21", why));
	TEST_EXPECT(rig.bootstrap.awaiting_fetch(kMachineA));
	const auto redeemed = rig.bootstrap.redeem(7, token, "10.44.0.21", why);
	TEST_EXPECT(redeemed && redeemed->machine_id == kMachineA);
	TEST_EXPECT(redeemed->bundle.server_id == 7 && redeemed->bundle.image == kImage);
	TEST_EXPECT(redeemed->bundle.spec.files.size() == 3 && redeemed->bundle.spec.serve_args.size() == 8);
	TEST_EXPECT(!rig.bootstrap.redeem(7, token, "10.44.0.21", why));
	TEST_EXPECT(!rig.bootstrap.awaiting_fetch(kMachineA));

	// Right after the launch EC2 may not list the machine yet: within the grace it
	// reads Pending, with the address its launch read.
	rig.runner.replies.push_back(answered(listing({})));
	hosted::MachineStatus status = rig.provider.describe(kMachineA);
	TEST_EXPECT(status.state == MachineState::Pending && status.address == "10.44.0.21");
	TEST_EXPECT(rig.runner.calls.size() == 2 && rig.runner.calls[1].argv == describe_argv());
	return 0;
}

int test_launch_failures(const std::string &scratch) {
	Rig rig(scratch);
	const auto launch_fails = [&rig](CommandResult reply, const char *expect) {
		rig.runner.replies.push_back(std::move(reply));
		const size_t first = rig.runner.calls.size();
		std::string error;
		const auto machine = rig.provider.launch(spec_for(7), error);
		if (machine || error.find(expect) == std::string::npos) {
			std::fprintf(stderr, "  launch error '%s', expected '%s'\n", error.c_str(), expect);
			return false;
		}
		// No token outlives a failed launch, and no user-data file stays behind.
		const Call &call = rig.runner.calls.at(first);
		return call.argv.size() > 2 && call.argv[2] == "run-instances" && rig.bootstrap.live_tokens() == 0 &&
				!call.user_data.empty() && !fs::exists(fs::u8path(call.user_data_path));
	};
	TEST_EXPECT(launch_fails(failed(254,
									 "\nAn error occurred (UnauthorizedOperation) when calling the RunInstances "
									 "operation: You are not authorized to perform this operation.\n"),
			"exited with code 254: An error occurred (UnauthorizedOperation)"));
	TEST_EXPECT(launch_fails(timed_out(), "timed out"));
	TEST_EXPECT(launch_fails(not_started(), "did not start"));
	TEST_EXPECT(launch_fails(answered("not json"), "answered no machine id"));
	TEST_EXPECT(launch_fails(answered("{}"), "answered no machine id"));
	TEST_EXPECT(launch_fails(answered("{\"Instances\":[]}"), "answered no machine id"));
	TEST_EXPECT(launch_fails(answered("{\"Instances\":[{\"InstanceId\":\"i-XYZ\"}]}"), "answered no machine id"));
	// One machine asked for, two answered: no one machine to bind the token to (both carry
	// the tags, and the supervisor's sweep ends what no row holds).
	TEST_EXPECT(launch_fails(answered(std::string("{\"Instances\":[{\"InstanceId\":\"") + kMachineA +
											 "\",\"PrivateIpAddress\":\"10.44.0.21\"},{\"InstanceId\":\"" + kMachineB +
											 "\",\"PrivateIpAddress\":\"10.44.0.22\"}]}"),
			"answered no machine id"));
	// A machine with no private address: nothing could bootstrap it, so it is ended at once.
	rig.runner.replies.push_back(answered(run_instances_reply(kMachineA, "")));
	rig.runner.replies.push_back(answered("{}")); // the terminate-instances it then makes
	{
		std::string error;
		const size_t first = rig.runner.calls.size();
		TEST_EXPECT(!rig.provider.launch(spec_for(7), error));
		TEST_EXPECT(error.find("with no private address") != std::string::npos);
		TEST_EXPECT(rig.runner.calls.size() == first + 2 && rig.bootstrap.live_tokens() == 0);
		TEST_EXPECT(!fs::exists(fs::u8path(rig.runner.calls.at(first).user_data_path)));
	}
	TEST_EXPECT(rig.runner.calls.back().argv == terminate_argv(kMachineA));
	const size_t calls = rig.runner.calls.size();
	TEST_EXPECT(rig.provider.describe(kMachineA).state == MachineState::Gone);
	TEST_EXPECT(rig.runner.calls.size() == calls);

	// A spec with no server id launches nothing.
	std::string error;
	hosted::LaunchSpec none = spec_for(7);
	none.server_id = 0;
	TEST_EXPECT(!rig.provider.launch(none, error) && !error.empty());
	TEST_EXPECT(rig.runner.calls.size() == calls);
	return 0;
}

int test_describe(const std::string &scratch) {
	Rig rig(scratch);
	const std::string token = rig.launch(7, kMachineA, "10.44.0.21");
	TEST_EXPECT(!token.empty());
	rig.runner.replies.push_back(answered(listing({
			instance_json(kMachineA, "running", "10.44.0.21", "7"),
			instance_json(kMachineB, "running", "10.44.0.22", "8"),
			instance_json("i-0ccccccccccccccc3", "pending", "10.44.0.23", "9"),
			instance_json("i-0ddddddddddddddd4", "shutting-down", "10.44.0.24", "10"),
			instance_json("i-0eeeeeeeeeeeeeee5", "terminated", "", "11"),
			instance_json("i-0fffffffffffffff6", "stopped", "10.44.0.26", "12"),
			instance_json("i-0aaaaaaaaaaaaaa07", "stopping", "10.44.0.27", "13"),
			// Not this deployment's, or not hosted: never read, whatever the filter let through.
			instance_json("i-0aaaaaaaaaaaaaa08", "running", "10.44.0.28", "14", "opennova-dev"),
			instance_json("i-0aaaaaaaaaaaaaa09", "running", "10.44.0.29", "15", kDeployment, false),
	})));
	const auto state_of = [&rig](const std::string &id) { return rig.provider.describe(id).state; };
	// Running in EC2, but its bundle not fetched yet: still booting.
	hosted::MachineStatus a = rig.provider.describe(kMachineA);
	TEST_EXPECT(a.state == MachineState::Pending && a.address == "10.44.0.21");
	TEST_EXPECT(rig.runner.calls.size() == 2 && rig.runner.calls[1].argv == describe_argv());
	// A machine an earlier run of the service launched has no token here: running is Running.
	const hosted::MachineStatus b = rig.provider.describe(kMachineB);
	TEST_EXPECT(b.state == MachineState::Running && b.address == "10.44.0.22");
	TEST_EXPECT(state_of("i-0ccccccccccccccc3") == MachineState::Pending);
	for (const char *gone : { "i-0ddddddddddddddd4", "i-0eeeeeeeeeeeeeee5", "i-0fffffffffffffff6",
				 "i-0aaaaaaaaaaaaaa07", "i-0aaaaaaaaaaaaaa08", "i-0aaaaaaaaaaaaaa09", "i-0123456789abcdef9" })
		TEST_EXPECT(state_of(gone) == MachineState::Gone);
	TEST_EXPECT(rig.provider.describe("i-0ddddddddddddddd4").address.empty());
	// Another provider's machine id is no EC2 machine.
	TEST_EXPECT(state_of("local-3-1") == MachineState::Gone);
	// All of that from the one describe-instances: the listing is kept kTtl.
	TEST_EXPECT(rig.runner.calls.size() == 2);

	// The fetch: running is Running at once (the store is asked, not the cache).
	std::string why;
	TEST_EXPECT(rig.bootstrap.redeem(7, token, "10.44.0.21", why));
	TEST_EXPECT(state_of(kMachineA) == MachineState::Running);
	TEST_EXPECT(rig.runner.calls.size() == 2);

	// Past the TTL the listing is read again: the machine went.
	rig.now += kTtl;
	rig.runner.replies.push_back(answered(listing({ instance_json(kMachineA, "shutting-down", "10.44.0.21", "7") })));
	TEST_EXPECT(state_of(kMachineA) == MachineState::Gone);
	TEST_EXPECT(rig.runner.calls.size() == 3);
	return 0;
}

int test_describe_failures(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(!rig.launch(7, kMachineA, "10.44.0.21").empty());
	// Every failure reads Pending, with the address last known, and is tried again kTtl
	// later: until then no describe runs the CLI (an API outage costs a supervisor pass one
	// run, not one per server).
	for (CommandResult reply :
			{ failed(255, "Could not connect to the endpoint URL"), timed_out(), not_started(),
					answered("not json"), answered("{}"), answered("{\"Reservations\":{}}") }) {
		rig.runner.replies.push_back(std::move(reply));
		const size_t calls = rig.runner.calls.size();
		const hosted::MachineStatus status = rig.provider.describe(kMachineA);
		TEST_EXPECT(rig.runner.calls.size() == calls + 1);
		TEST_EXPECT(status.state == MachineState::Pending && status.address == "10.44.0.21");
		rig.now += kTtl - 1;
		const hosted::MachineStatus again = rig.provider.describe(kMachineA);
		TEST_EXPECT(again.state == MachineState::Pending && again.address == "10.44.0.21");
		TEST_EXPECT(rig.provider.describe(kMachineB).state == MachineState::Pending);
		TEST_EXPECT(rig.runner.calls.size() == calls + 1);
		rig.now += 1;
	}
	// A machine it never knew reads Pending too: no word is no crash.
	rig.runner.replies.push_back(timed_out());
	TEST_EXPECT(rig.provider.describe(kMachineB).state == MachineState::Pending);
	// The sweep's list_tagged reads fresh, back-off or not.
	rig.runner.replies.push_back(answered(listing({ instance_json(kMachineB, "running", "10.44.0.22", "8") })));
	{
		const size_t calls = rig.runner.calls.size();
		const std::vector<hosted::TaggedMachine> tagged = rig.provider.list_tagged();
		// B, and A within its launch grace.
		TEST_EXPECT(rig.runner.calls.size() == calls + 1 && tagged.size() == 2);
	}

	// Junk inside a well-formed reply is skipped, not fatal.
	rig.now += kTtl;
	rig.runner.replies.push_back(answered("{\"Reservations\":[7,{\"Instances\":{}},{\"Instances\":[\"x\","
										  "{\"State\":{\"Name\":\"running\"}},{\"InstanceId\":\"i-XYZ\"}," +
			instance_json(kMachineB, "running", "10.44.0.22", "8") + "]}]}"));
	TEST_EXPECT(rig.provider.describe(kMachineB).state == MachineState::Running);
	// Within the grace, a launched machine the listing lacks is Pending; past it, Gone.
	TEST_EXPECT(rig.provider.describe(kMachineA).state == MachineState::Pending);
	rig.now += kGrace + 1;
	rig.runner.replies.push_back(answered(listing({ instance_json(kMachineB, "running", "10.44.0.22", "8") })));
	TEST_EXPECT(rig.provider.describe(kMachineA).state == MachineState::Gone);
	return 0;
}

int test_terminate(const std::string &scratch) {
	Rig rig(scratch);
	const std::string token = rig.launch(7, kMachineA, "10.44.0.21");
	TEST_EXPECT(!token.empty());
	rig.runner.replies.push_back(answered("{\"TerminatingInstances\":[{\"InstanceId\":\"i-0aaaaaaaaaaaaaaa1\"}]}"));
	rig.provider.terminate(kMachineA);
	TEST_EXPECT(rig.runner.calls.back().argv == terminate_argv(kMachineA));
	// Gone at once, with no call, and its token and record forgotten.
	size_t calls = rig.runner.calls.size();
	TEST_EXPECT(rig.provider.describe(kMachineA).state == MachineState::Gone);
	TEST_EXPECT(rig.runner.calls.size() == calls);
	std::string why;
	TEST_EXPECT(!rig.bootstrap.redeem(7, token, "10.44.0.21", why) && rig.bootstrap.live_tokens() == 0);
	TEST_EXPECT(!rig.bootstrap.awaiting_fetch(kMachineA));
	// Still Gone past the listing's lifetime, whatever EC2 would list a moment later: no call.
	rig.now += kTtl;
	TEST_EXPECT(rig.provider.describe(kMachineA).state == MachineState::Gone);
	TEST_EXPECT(rig.runner.calls.size() == calls);

	// A machine EC2 no longer knows: no error, and Gone.
	rig.runner.replies.push_back(failed(254,
			"\nAn error occurred (InvalidInstanceID.NotFound) when calling the TerminateInstances operation: "
			"The instance ID 'i-0bbbbbbbbbbbbbbb2' does not exist\n"));
	rig.provider.terminate(kMachineB);
	TEST_EXPECT(rig.runner.calls.back().argv == terminate_argv(kMachineB));
	calls = rig.runner.calls.size();
	TEST_EXPECT(rig.provider.describe(kMachineB).state == MachineState::Gone && rig.runner.calls.size() == calls);

	// A refusal leaves the machine as EC2 says it is (the supervisor's sweep tries again),
	// but its token goes all the same: nothing fetches a bundle for a machine being ended.
	const std::string c_token = rig.launch(9, "i-0ccccccccccccccc3", "10.44.0.23");
	TEST_EXPECT(!c_token.empty() && rig.bootstrap.awaiting_fetch("i-0ccccccccccccccc3"));
	rig.runner.replies.push_back(failed(254, "An error occurred (UnauthorizedOperation)"));
	rig.provider.terminate("i-0ccccccccccccccc3");
	TEST_EXPECT(!rig.bootstrap.redeem(9, c_token, "10.44.0.23", why) && rig.bootstrap.live_tokens() == 0);
	TEST_EXPECT(!rig.bootstrap.awaiting_fetch("i-0ccccccccccccccc3"));
	rig.now += kTtl;
	rig.runner.replies.push_back(answered(listing({ instance_json("i-0ccccccccccccccc3", "running", "10.44.0.23", "9") })));
	TEST_EXPECT(rig.provider.describe("i-0ccccccccccccccc3").state == MachineState::Running);
	rig.runner.replies.push_back(timed_out());
	rig.provider.terminate("i-0ccccccccccccccc3");

	// Another provider's machine id: no call at all.
	calls = rig.runner.calls.size();
	rig.provider.terminate("local-3-1");
	rig.provider.terminate("");
	TEST_EXPECT(rig.runner.calls.size() == calls);
	return 0;
}

int test_list_tagged(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(!rig.launch(7, kMachineA, "10.44.0.21").empty());
	rig.runner.replies.push_back(answered("{}")); // the terminate's reply
	rig.provider.terminate("i-0aaaaaaaaaaaaaa07");
	const std::string all = listing({
			instance_json(kMachineA, "running", "10.44.0.21", "7"),
			instance_json(kMachineB, "pending", "10.44.0.22", "8"),
			instance_json("i-0ccccccccccccccc3", "stopping", "10.44.0.23", "x9"),
			instance_json("i-0ddddddddddddddd4", "stopped", "10.44.0.24", ""),
			instance_json("i-0eeeeeeeeeeeeeee5", "shutting-down", "10.44.0.25", "11"),
			instance_json("i-0fffffffffffffff6", "terminated", "", "12"),
			// Ended by this provider a moment ago, still listed running: not listed again.
			instance_json("i-0aaaaaaaaaaaaaa07", "running", "10.44.0.27", "13"),
			instance_json("i-0aaaaaaaaaaaaaa08", "running", "10.44.0.28", "14", "opennova-dev"),
	});
	// Read fresh every time, whatever the cache holds.
	for (int round = 0; round < 2; ++round) {
		rig.runner.replies.push_back(answered(all));
		const size_t calls = rig.runner.calls.size();
		const std::vector<hosted::TaggedMachine> tagged = rig.provider.list_tagged();
		TEST_EXPECT(rig.runner.calls.size() == calls + 1 && rig.runner.calls.back().argv == describe_argv());
		TEST_EXPECT(tagged.size() == 4);
		std::map<std::string, int64_t> by_id;
		for (const hosted::TaggedMachine &m : tagged) by_id[m.machine_id] = m.server_id;
		TEST_EXPECT(by_id.at(kMachineA) == 7 && by_id.at(kMachineB) == 8);
		// A server-id tag that is no number, or none: 0, which no live row names (the sweep ends it).
		TEST_EXPECT(by_id.at("i-0ccccccccccccccc3") == 0 && by_id.at("i-0ddddddddddddddd4") == 0);
	}
	// A listing that fails throws: the supervisor ends nothing on it.
	for (CommandResult reply : { failed(255, "throttled"), timed_out(), answered("[]") }) {
		rig.runner.replies.push_back(std::move(reply));
		bool threw = false;
		try {
			(void)rig.provider.list_tagged();
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TEST_EXPECT(threw);
	}
	return 0;
}

// --- the bootstrap ---------------------------------------------------------------------------

int test_bootstrap_store() {
	uint64_t now = 1000;
	hosted::BootstrapStore store([&now] { return now; });
	hosted::BootstrapBundle bundle;
	bundle.server_id = 7;
	bundle.image = kImage;
	bundle.spec = spec_for(7);
	const std::string token = store.issue(bundle);
	const std::string other = store.issue(bundle);
	TEST_EXPECT(token.size() == hosted::BootstrapStore::kTokenChars && is_alnum_text(token) && token != other);
	TEST_EXPECT(store.live_tokens() == 2);
	std::string why;
	// Unbound: no machine may redeem it yet.
	TEST_EXPECT(!store.redeem(7, token, "10.44.0.21", why) && !why.empty());
	TEST_EXPECT(store.bind(token, kMachineA, "10.44.0.21"));
	TEST_EXPECT(store.awaiting_fetch(kMachineA) && !store.awaiting_fetch(kMachineB));
	// Not the token: too short, foreign characters, another token.
	TEST_EXPECT(!store.redeem(7, "", "10.44.0.21", why));
	TEST_EXPECT(!store.redeem(7, token.substr(1), "10.44.0.21", why));
	TEST_EXPECT(!store.redeem(7, token.substr(1) + "'", "10.44.0.21", why));
	TEST_EXPECT(!store.redeem(7, other, "10.44.0.21", why));
	TEST_EXPECT(!store.redeem(7, token, "::ffff:10.44.0.22", why));
	// The mapped spelling of its own address is its address.
	const auto redeemed = store.redeem(7, token, "::ffff:10.44.0.21", why);
	TEST_EXPECT(redeemed && redeemed->machine_id == kMachineA && redeemed->bundle.spec.files.size() == 3);
	TEST_EXPECT(!store.redeem(7, token, "10.44.0.21", why));
	TEST_EXPECT(!store.awaiting_fetch(kMachineA));

	// A token lapses kTokenLifetimeMs after it was issued.
	TEST_EXPECT(store.bind(other, kMachineB, "10.44.0.22"));
	now += hosted::BootstrapStore::kTokenLifetimeMs;
	TEST_EXPECT(!store.redeem(7, other, "10.44.0.22", why) && store.live_tokens() == 0);
	TEST_EXPECT(!store.bind(other, kMachineB, "10.44.0.22"));
	// The machine it was for still never fetched: it reads as booting until it ends, or
	// kUnfetchedRecordMs past its token's lapse (the supervisor ended it long before).
	TEST_EXPECT(store.awaiting_fetch(kMachineB));
	now += hosted::BootstrapStore::kUnfetchedRecordMs - 1;
	TEST_EXPECT(store.awaiting_fetch(kMachineB));
	now += 1;
	TEST_EXPECT(!store.awaiting_fetch(kMachineB));
	store.forget_machine(kMachineB);
	TEST_EXPECT(!store.awaiting_fetch(kMachineB));

	// Revoked, and forgotten with its machine.
	const std::string third = store.issue(bundle);
	store.revoke(third);
	TEST_EXPECT(!store.bind(third, kMachineA, "10.44.0.21") && store.live_tokens() == 0);
	const std::string fourth = store.issue(bundle);
	TEST_EXPECT(store.bind(fourth, kMachineB, "10.44.0.22"));
	store.forget_machine(kMachineB);
	TEST_EXPECT(!store.redeem(7, fourth, "10.44.0.22", why) && store.live_tokens() == 0);
	// A token revoked after its bind takes its machine's record with it.
	const std::string fifth = store.issue(bundle);
	TEST_EXPECT(store.bind(fifth, kMachineA, "10.44.0.21") && store.awaiting_fetch(kMachineA));
	store.revoke(fifth);
	TEST_EXPECT(!store.awaiting_fetch(kMachineA) && store.live_tokens() == 0);
	return 0;
}

int test_log_throttle() {
	uint64_t now = 1000;
	hosted::LogThrottle throttle([&now] { return now; });
	TEST_EXPECT(throttle.admit("10.44.0.21") && !throttle.admit("10.44.0.21"));
	TEST_EXPECT(throttle.admit("10.44.0.22"));
	now += hosted::LogThrottle::kWindowMs - 1;
	TEST_EXPECT(!throttle.admit("10.44.0.21"));
	now += 1;
	TEST_EXPECT(throttle.admit("10.44.0.21"));
	// At most kMaxKeys peers at once: one more is not logged until one lapses.
	now += hosted::LogThrottle::kWindowMs;
	for (size_t i = 0; i < hosted::LogThrottle::kMaxKeys; ++i)
		TEST_EXPECT(throttle.admit("10.45.0." + std::to_string(i)));
	TEST_EXPECT(!throttle.admit("10.46.0.1"));
	now += hosted::LogThrottle::kWindowMs;
	TEST_EXPECT(throttle.admit("10.46.0.1"));
	return 0;
}

int test_cidrs() {
	using hosted::parse_ipv4;
	using hosted::parse_ipv4_cidr;
	TEST_EXPECT(parse_ipv4("10.44.0.5") == uint32_t{ 0x0A2C0005 });
	TEST_EXPECT(parse_ipv4("0.0.0.0") == uint32_t{ 0 } && parse_ipv4("255.255.255.255") == uint32_t{ 0xFFFFFFFF });
	for (const char *bad : { "", "10.44.0", "10.44.0.5.1", "256.0.0.1", "01.2.3.4", " 1.2.3.4", "1.2.3.4 ", "1..2.3",
				 "a.b.c.d", "1.2.3.-4", "1.2.3.4/32", "1000.2.3.4" })
		TEST_EXPECT(!parse_ipv4(bad));
	const auto vpc = parse_ipv4_cidr("10.44.0.0/16");
	TEST_EXPECT(vpc && vpc->network == 0x0A2C0000 && vpc->mask == 0xFFFF0000);
	TEST_EXPECT(parse_ipv4_cidr("0.0.0.0/0") && parse_ipv4_cidr("0.0.0.0/0")->mask == 0);
	const auto host = parse_ipv4_cidr("10.44.0.5");
	TEST_EXPECT(host && host->mask == 0xFFFFFFFF);
	for (const char *bad : { "10.44.0.1/16", "10.44.0.0/33", "10.44.0.0/", "10.44.0.0/x", "10.44.0.0/016",
				 "10.44.0.0/16/1", "/16" })
		TEST_EXPECT(!parse_ipv4_cidr(bad));
	const std::vector<hosted::Ipv4Cidr> admitted = { *vpc, *parse_ipv4_cidr("192.168.7.0/24") };
	TEST_EXPECT(hosted::ipv4_admitted("10.44.3.4", admitted));
	TEST_EXPECT(hosted::ipv4_admitted("192.168.7.200", admitted));
	TEST_EXPECT(hosted::ipv4_admitted("::ffff:10.44.3.4", admitted));
	TEST_EXPECT(hosted::ipv4_admitted("::FFFF:10.44.3.4", admitted));
	for (const char *outside : { "10.45.0.1", "127.0.0.1", "192.168.8.1", "54.162.79.147", "::1", "", "::ffff:" })
		TEST_EXPECT(!hosted::ipv4_admitted(outside, admitted));
	TEST_EXPECT(!hosted::ipv4_admitted("10.44.3.4", {}));

	// ONNET_HOSTED_BOOTSTRAP_CIDRS: the private networks alone, no prefix under /8.
	const auto is_private = [](const char *text) {
		const auto cidr = parse_ipv4_cidr(text);
		return cidr && hosted::ipv4_cidr_private(*cidr);
	};
	for (const char *inside : { "10.0.0.0/8", "10.44.0.0/16", "10.44.0.5", "172.16.0.0/12", "172.31.32.0/20",
				 "192.168.0.0/16", "192.168.7.0/24" })
		TEST_EXPECT(is_private(inside));
	for (const char *outside : { "0.0.0.0/0", "8.0.0.0/7", "10.0.0.0/7", "172.0.0.0/8", "172.32.0.0/16",
				 "192.168.0.0/15", "54.162.0.0/16", "100.64.0.0/10", "127.0.0.0/8" }) {
		TEST_EXPECT(parse_ipv4_cidr(outside));
		TEST_EXPECT(!is_private(outside));
	}
	TEST_EXPECT(hosted::ipv4_cidr_text(*vpc) == "10.44.0.0/16");
	TEST_EXPECT(hosted::ipv4_cidr_text(*host) == "10.44.0.5/32");
	TEST_EXPECT(hosted::ipv4_cidr_text(*parse_ipv4_cidr("0.0.0.0/0")) == "0.0.0.0/0");
	return 0;
}

int test_bundle_json() {
	hosted::BootstrapBundle bundle;
	bundle.server_id = 7;
	bundle.image = kImage;
	bundle.spec = spec_for(7);
	opennova::io::JsonValue json;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse(hosted::bootstrap_bundle_json(bundle), json, error));
	TEST_EXPECT(json.get_int("server_id", 0) == 7 && json.get_string("image", "") == kImage);
	const opennova::io::JsonValue *args = json.get("args");
	TEST_EXPECT(args && args->is_array() && args->array.size() == bundle.spec.serve_args.size());
	for (size_t i = 0; i < args->array.size(); ++i) TEST_EXPECT(args->array[i].string == bundle.spec.serve_args[i]);
	const opennova::io::JsonValue *files = json.get("files");
	TEST_EXPECT(files && files->is_array() && files->array.size() == 3);
	for (size_t i = 0; i < files->array.size(); ++i) {
		const opennova::io::JsonValue &f = files->array[i];
		const hosted::LaunchFile &want = bundle.spec.files[i];
		TEST_EXPECT(f.get_string("name", "") == want.name);
		TEST_EXPECT(f.get_bool("secret", !want.secret) == want.secret);
		// The bytes as they are (the host file's cp1252 0xE9 among them), in base64.
		TEST_EXPECT(f.get_string("content_base64", "") ==
				opennova::io::base64_encode(reinterpret_cast<const uint8_t *>(want.content.data()),
						want.content.size()));
	}
	return 0;
}

// --- the real runner ----------------------------------------------------------------------------

#ifdef _WIN32
void set_env(const char *name, const char *value) { _putenv_s(name, value); }
#else
void set_env(const char *name, const char *value) { setenv(name, value, 1); }
#endif

int test_process_runner(const std::string &scratch) {
	const fs::path dir = fs::u8path(scratch) / "runner";
	std::string error;
	TEST_EXPECT(hosted::make_owner_directory(dir, error));
	hosted::ProcessCommandRunner runner(opennova::proc::current_environment(), dir.u8string());

	CommandResult r = runner.run({ g_self, "--child", "streams" }, 20000);
	TEST_EXPECT(r.ok() && r.outcome == CommandResult::Outcome::Exited);
	TEST_EXPECT(r.out.find("{\"on\":\"stdout\"}") != std::string::npos && r.out.find("diag") == std::string::npos);
	TEST_EXPECT(r.err.find("diag on stderr") != std::string::npos && r.err.find("stdout") == std::string::npos);

	r = runner.run({ g_self, "--child", "exit", "3" }, 20000);
	TEST_EXPECT(r.outcome == CommandResult::Outcome::Exited && r.exit_code == 3 && !r.ok());

	const auto started = std::chrono::steady_clock::now();
	r = runner.run({ g_self, "--child", "sleep" }, 300);
	TEST_EXPECT(r.outcome == CommandResult::Outcome::TimedOut && !r.ok());
	TEST_EXPECT(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));

	r = runner.run({ (dir / "no-such-program").u8string() }, 20000);
	TEST_EXPECT(r.outcome == CommandResult::Outcome::NotStarted && !r.error.empty() && !r.ok());

	// Nothing of a run stays in the scratch directory.
	TEST_EXPECT(fs::is_empty(dir));

	// The CLI's environment: the AWS variables and the system's, never the service's own,
	// and the standard retry mode unless the service names one.
	set_env("AWS_OPENNOVA_TEST_VAR", "kept");
	set_env("ONNET_OPENNOVA_TEST_SECRET", "hunter2");
	std::vector<std::string> env = hosted::ProcessCommandRunner::service_environment();
	const auto has = [&env](const std::string &entry) {
		for (const std::string &e : env)
			if (e == entry) return true;
		return false;
	};
	const auto names = [&env](const std::string &prefix) {
		for (const std::string &e : env)
			if (e.rfind(prefix, 0) == 0) return true;
		return false;
	};
	TEST_EXPECT(has("AWS_OPENNOVA_TEST_VAR=kept"));
	TEST_EXPECT(!names("ONNET_OPENNOVA_TEST_SECRET="));
	TEST_EXPECT(names("AWS_RETRY_MODE=") && names("AWS_MAX_ATTEMPTS="));
	set_env("AWS_RETRY_MODE", "adaptive");
	env = hosted::ProcessCommandRunner::service_environment();
	TEST_EXPECT(has("AWS_RETRY_MODE=adaptive") && !has("AWS_RETRY_MODE=standard"));
	return 0;
}

int child_main(int argc, char **argv) {
	const std::string mode = argc > 2 ? argv[2] : "";
	if (mode == "streams") {
		std::fprintf(stdout, "{\"on\":\"stdout\"}\n");
		std::fflush(stdout);
		std::fprintf(stderr, "diag on stderr\n");
		return 0;
	}
	if (mode == "exit") return argc > 3 ? std::atoi(argv[3]) : 0;
	if (mode == "sleep") {
		std::this_thread::sleep_for(std::chrono::seconds(60));
		return 9;
	}
	return 99;
}

} // namespace

int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return child_main(argc, argv);
	if (argc < 2) {
		std::fprintf(stderr, "usage: hosted_ec2_provider_test <this binary's path>\n");
		return 2;
	}
	g_self = argv[1];
	test_temp::TempDir temp("hosted_ec2_provider");
	const std::string scratch = temp.root();
	struct Case {
		const char *name;
		int (*fn)(const std::string &);
	};
	const Case cases[] = {
		{ "config_checks", test_config_checks },
		{ "machine_script", [](const std::string &) { return test_machine_script(); } },
		{ "plan", test_plan },
		{ "launch", test_launch },
		{ "launch_failures", test_launch_failures },
		{ "describe", test_describe },
		{ "describe_failures", test_describe_failures },
		{ "terminate", test_terminate },
		{ "list_tagged", test_list_tagged },
		{ "bootstrap_store", [](const std::string &) { return test_bootstrap_store(); } },
		{ "log_throttle", [](const std::string &) { return test_log_throttle(); } },
		{ "cidrs", [](const std::string &) { return test_cidrs(); } },
		{ "bundle_json", [](const std::string &) { return test_bundle_json(); } },
		{ "process_runner", test_process_runner },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn(scratch) != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted ec2 provider\n");
	return 0;
}
