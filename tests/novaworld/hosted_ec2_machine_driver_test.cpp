// The fleet's machine drivers (apps/novaworld_server/hosted/machine_driver.h)
// and settings. The EC2 driver (hosted/ec2_machine_driver.h) on a stub
// CommandRunner that records every AWS CLI argv and answers it from a script:
// the launch's run-instances (the exact argv, each kind's launch template and
// a pinned version, the tags, the row's own client token, the user-data file
// with the machine's values filled in, deleted after; a call cut short made
// again the same), describe's state mapping from
// one cached describe-instances (the launch grace, the cache's lifetime, a
// failure reading Unknown and its back-off), terminate's tag check (a machine
// without both of this deployment's tags never ended) and its idempotence,
// list_tagged's scope and its throw, the slots' ports, and every CLI failure,
// timeout and malformed reply; its config checks and the data sets' volume
// labels. The shipped machine script (hosted/machine/machine_user_data.sh):
// its blanks and their render, no xtrace, the slots' ports reserved before
// anything runs, the data sets mounted by label read-only, the fences (the
// metadata service and the internal port, for every slot's user) applied and
// checked before the agent starts, the agent copied out of the serve image and
// its unit. The settings (hosted/fleet_config.h): the defaults, every
// variable's read and its refusals. And the local driver's machine life
// without a service (hosted/local_machine_driver.h; its agent end to end is
// hosted_agent_route's). Crow-free and database-free: compiles the sources it
// needs directly, so it runs on every service build.

#include "command_runner.h"
#include "hosted/agent_protocol.h"
#include "hosted/ec2_machine_driver.h"
#include "hosted/fleet_config.h"
#include "hosted/hosted_settings.h"
#include "hosted/local_machine_driver.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
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
constexpr const char *kShared = "lt-0123456789abcdef0";
constexpr const char *kOwn = "lt-0abcdef0123456789";
constexpr const char *kSubnet = "subnet-0123456789abcdef0";
constexpr const char *kDeployment = "opennova-server";
constexpr const char *kImage = "ghcr.io/opennova-net/opennova-serve@sha256:"
							   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char *kHostdImage =
		"ghcr.io/opennova-net/opennova-serve@sha256:"
		"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
constexpr const char *kService = "10.44.0.10";
constexpr const char *kMachineA = "i-0aaaaaaaaaaaaaaa1";
constexpr const char *kMachineB = "i-0bbbbbbbbbbbbbbb2";
constexpr uint64_t kTtl = 10 * 1000;
constexpr uint64_t kGrace = 3 * 60 * 1000;

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
							 "/apps/novaworld_server/hosted/machine/machine_user_data.sh"),
			std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

hosted::Ec2MachineConfig config_in(const std::string &scratch) {
	hosted::Ec2MachineConfig c;
	c.aws_cli = kAws;
	c.region = kRegion;
	c.subnet = kSubnet;
	c.deployment = kDeployment;
	c.shared_template = kShared;
	c.own_template = std::string(kOwn) + ":7";
	c.serve_image = kImage;
	c.service_address = kService;
	c.service_port = 8070;
	c.data_sets = { hosted::kDataSetJointOperations, hosted::kDataSetDeltaForceXtreme2 };
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
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.subnet = "sg-0123456789abcdef0"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.deployment = ""; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.deployment = "prod,dev"; }));
	// The longest deployment still makes a client token EC2 takes (64 at most).
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.deployment = std::string(33, 'd'); }));
	TEST_EXPECT(!refused([](hosted::Ec2MachineConfig &c) { c.deployment = std::string(32, 'd'); }));
	TEST_EXPECT(
			hosted::Ec2MachineDriver::client_token(std::string(32, 'd'), INT64_MAX).size() <= 64);
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
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.hostd_image = "--privileged"; }));
	TEST_EXPECT(!refused([](hosted::Ec2MachineConfig &c) { c.hostd_image = kHostdImage; }));
	// The internal listener the machines reach: a private address and a port.
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.service_address = "127.0.0.1"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.service_address = "54.1.2.3"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.service_address = "0.0.0.0"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.service_address = "10.44.0"; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.service_port = 0; }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.data_sets.clear(); }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.data_sets.push_back("Not A Set"); }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) {
		c.data_sets.push_back("a_much_too_long_data_set_name");
	}));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.machine_script = "#!/bin/bash\n"; }));
	TEST_EXPECT(refused(
			[](hosted::Ec2MachineConfig &c) { c.machine_script += std::string(16 * 1024, '#'); }));
	TEST_EXPECT(refused([](hosted::Ec2MachineConfig &c) { c.scratch_dir = ""; }));
	return 0;
}

int test_volume_labels() {
	using hosted::Ec2MachineDriver;
	TEST_EXPECT(Ec2MachineDriver::volume_label("jop_2_consumer") == "od-jop2");
	TEST_EXPECT(Ec2MachineDriver::volume_label("dfx2_consumer") == "od-dfx2");
	TEST_EXPECT(Ec2MachineDriver::volume_label("opennova_0_consumer") == "od-opennova0");
	// Every data set the service knows has one, and ext4's 16 bytes hold it.
	for (const hosted::DataSet &d : hosted::known_data_sets()) {
		const std::string label = Ec2MachineDriver::volume_label(d.game);
		TEST_EXPECT(!label.empty() && label.size() <= Ec2MachineDriver::kMaxVolumeLabelBytes);
	}
	TEST_EXPECT(Ec2MachineDriver::volume_label("").empty());
	TEST_EXPECT(Ec2MachineDriver::volume_label("_").empty());
	TEST_EXPECT(Ec2MachineDriver::volume_label("abcdefghijklmnop_consumer").empty());
	TEST_EXPECT(Ec2MachineDriver::volume_label("UPPER").empty());
	return 0;
}

int test_machine_script(const std::string &) {
	using hosted::Ec2MachineDriver;
	const std::string script = machine_script();
	TEST_EXPECT(!script.empty());
	for (const char *blank : { Ec2MachineDriver::kBlankServiceAddress,
				 Ec2MachineDriver::kBlankServicePort, Ec2MachineDriver::kBlankMachineId,
				 Ec2MachineDriver::kBlankMachineToken, Ec2MachineDriver::kBlankServeImage,
				 Ec2MachineDriver::kBlankHostdImage, Ec2MachineDriver::kBlankDataSets })
		TEST_EXPECT(script.find(blank) != std::string::npos);
	TEST_EXPECT(!traces_commands(script));
	TEST_EXPECT(traces_commands("set -euxo pipefail\n") && traces_commands("  set -o xtrace\n"));
	// The token: written by a builtin to an owner-only file, then forgotten.
	TEST_EXPECT(runs_line(script, "printf '%s\\n' \"$MACHINE_TOKEN\" > \"$TOKEN_FILE\""));
	TEST_EXPECT(runs_line(script, "readonly TOKEN_FILE=/etc/opennova/hostd.token"));
	TEST_EXPECT(runs_line(script, "chmod 0600 \"$TOKEN_FILE\"") &&
			runs_line(script, "unset MACHINE_TOKEN"));
	TEST_EXPECT(runs_line(script, "umask 077"));
	// Every slot's ports leave the ephemeral range before anything runs (the
	// apt and pull sockets), and at every boot.
	const size_t reserve =
			line_at(script, "sysctl -w \"net.ipv4.ip_local_reserved_ports=$SLOT_PORTS\"");
	TEST_EXPECT(runs_line(script, "readonly SLOT_PORTS=32768-32831"));
	TEST_EXPECT(reserve != std::string::npos && reserve < line_at(script, "apt_get update"));
	TEST_EXPECT(runs_line(script, "/etc/sysctl.d/60-opennova-slots.conf"));
	// The two port ranges are slots.h's, all 32 slots of both.
	TEST_EXPECT(hosted::slot_game_port(0) == 32768 &&
			hosted::slot_admin_port(hosted::kMaxSlots - 1) == 32831);
	TEST_EXPECT(hosted::slot_uid(0) == 10001 && hosted::slot_uid(hosted::kMaxSlots - 1) == 10032);
	// Docker with no user-namespace remapping.
	TEST_EXPECT(runs_line(script, "install --no-install-recommends docker.io iptables curl"));
	TEST_EXPECT(runs_line(script, "end_machine \"docker remaps user namespaces\""));
	// Each data set by its volume's label, read-only, at /opt/game/<data set>, at every boot
	// too; an older snapshot's volume (opennova-data) by its place in the sorted order: its
	// device name (/dev/sdf, ...) where the kernel has one, else the other disks' order.
	TEST_EXPECT(runs_line(script, "for entry in $DATA_SETS; do"));
	TEST_EXPECT(runs_line(script, "if [ -e \"/dev/disk/by-label/$label\" ]; then"));
	TEST_EXPECT(runs_line(script, "source=\"LABEL=$label\""));
	TEST_EXPECT(runs_line(script, "readonly LEGACY_LABEL=opennova-data"));
	TEST_EXPECT(std::string(Ec2MachineDriver::kLegacyVolumeLabel) == "opennova-data");
	TEST_EXPECT(runs_line(script, "readonly DEVICE_LETTERS=fghijklmnop"));
	TEST_EXPECT(runs_line(script, "device=\"/dev/sd${DEVICE_LETTERS:$index:1}\""));
	TEST_EXPECT(runs_line(script, "[ -b \"$device\" ] || device=\"/dev/${disks[$index]:-none}\""));
	TEST_EXPECT(runs_line(script, "= \"$LEGACY_LABEL\" ]"));
	TEST_EXPECT(runs_line(script, "source=\"UUID=$(blkid -s UUID -o value \"$device\")\""));
	TEST_EXPECT(runs_line(
			script, "printf '%s %s ext4 ro,noload,nodev,nosuid,noexec,nofail 0 0\\n' \"$source\""));
	TEST_EXPECT(runs_line(script, "readonly GAME_ROOT=/opt/game"));
	TEST_EXPECT(runs_line(script, "mount \"$GAME_ROOT/$data_set\""));
	TEST_EXPECT(runs_line(script, "-ipath '*/expansion/jox01/version.txt'"));
	// The fences: every slot's UID refused the metadata service and the internal port,
	// at every start of the agent, and checked for the first and the last slot before it.
	TEST_EXPECT(runs_line(script, "-m owner --uid-owner 10001-10032 -j REJECT"));
	TEST_EXPECT(runs_line(script, "fence -d 169.254.169.254"));
	TEST_EXPECT(
			runs_line(script, "fence -p tcp -d \"$SERVICE_ADDRESS\" --dport \"$SERVICE_PORT\""));
	TEST_EXPECT(runs_line(script,
			"ip6tables -w 5 -I OUTPUT -d fd00:ec2::254 -m owner --uid-owner 10001-10032 -j "
			"REJECT"));
	TEST_EXPECT(runs_line(script, "ExecStartPre=/usr/local/sbin/opennova-machine-fence"));
	TEST_EXPECT(runs_line(script, "for uid in \"$FIRST_SLOT_UID\" \"$LAST_SLOT_UID\"; do"));
	TEST_EXPECT(runs_line(script, "readonly FIRST_SLOT_UID=10001") &&
			runs_line(script, "readonly LAST_SLOT_UID=10032"));
	TEST_EXPECT(runs_line(script, "\"http://$SERVICE_ADDRESS:$SERVICE_PORT/\""));
	TEST_EXPECT(runs_line(script, "[ \"$rc\" -eq 7 ] || end_machine"));
	const size_t check = line_at(script, "[ \"$rc\" -eq 7 ] || end_machine");
	const size_t agent = line_at(script, "systemctl enable --now opennova-hostd.service");
	TEST_EXPECT(agent != std::string::npos && check < agent);
	// The serve image pulled, the agent out of the hostd image, under systemd, restarted always.
	TEST_EXPECT(runs_line(script, "pull \"$SERVE_IMAGE\""));
	TEST_EXPECT(
			runs_line(script, "[ \"$HOSTD_IMAGE\" = \"$SERVE_IMAGE\" ] || pull \"$HOSTD_IMAGE\""));
	TEST_EXPECT(runs_line(
			script, "docker create --entrypoint /usr/local/bin/opennova-hostd \"$HOSTD_IMAGE\""));
	TEST_EXPECT(runs_line(
			script, "docker cp \"$container:/usr/local/bin/opennova-hostd\" \"$HOSTD.new\""));
	TEST_EXPECT(runs_line(script, "readonly HOSTD=/usr/local/bin/opennova-hostd"));
	TEST_EXPECT(runs_line(script,
			"ExecStart=$HOSTD --service http://$SERVICE_ADDRESS:$SERVICE_PORT --machine "
			"$MACHINE_ID"));
	TEST_EXPECT(runs_line(script, "Restart=always"));
	TEST_EXPECT(script.size() < Ec2MachineDriver::kMaxUserDataBytes - 1024);

	hosted::MachineUserData values;
	values.service_address = kService;
	values.service_port = 8070;
	values.machine_id = 12;
	values.bootstrap_token = hosted::random_secret(hosted::kMachineTokenChars);
	values.serve_image = kImage;
	values.hostd_image = kHostdImage;
	values.data_sets = { { "jop_2_consumer", "od-jop2" }, { "dfx2_consumer", "od-dfx2" } };
	const auto rendered = Ec2MachineDriver::render_user_data(script, values);
	TEST_EXPECT(rendered);
	TEST_EXPECT(rendered->find("@OPENNOVA_") == std::string::npos);
	TEST_EXPECT(assigned(*rendered, "SERVICE_ADDRESS") == kService);
	TEST_EXPECT(assigned(*rendered, "SERVICE_PORT") == "8070");
	TEST_EXPECT(assigned(*rendered, "MACHINE_ID") == "12");
	TEST_EXPECT(assigned(*rendered, "MACHINE_TOKEN") == values.bootstrap_token);
	TEST_EXPECT(assigned(*rendered, "SERVE_IMAGE") == kImage);
	TEST_EXPECT(assigned(*rendered, "HOSTD_IMAGE") == kHostdImage);
	TEST_EXPECT(assigned(*rendered, "DATA_SETS") == "jop_2_consumer=od-jop2 dfx2_consumer=od-dfx2");
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
	TEST_EXPECT(refuses(
			[](hosted::MachineUserData &v) { v.service_address = "10.0.0.1'; reboot; '"; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.service_port = 0; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.machine_id = 0; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.serve_image = "img'"; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.hostd_image = ""; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.hostd_image = "-x"; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.data_sets.clear(); }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.data_sets[0].second = "od jop2"; }));
	TEST_EXPECT(refuses([](hosted::MachineUserData &v) { v.data_sets[0].first = "a=b"; }));
	TEST_EXPECT(!Ec2MachineDriver::render_user_data("#!/bin/bash\n", values));
	return 0;
}

int test_slot_plan(const std::string &scratch) {
	Rig rig(scratch);
	TEST_EXPECT(std::string(rig.driver.name()) == "ec2");
	TEST_EXPECT(rig.driver.boot_timeout_ms() == hosted::Ec2MachineDriver::kBootTimeoutMs);
	for (int slot : { 0, 5, hosted::kMaxSlots - 1 }) {
		const hosted::SlotPlan plan = rig.driver.slot_plan(kMachineA, slot);
		TEST_EXPECT(plan.game_port == 32768 + slot && plan.admin_port == 32800 + slot);
		TEST_EXPECT(plan.admin_allowed_ip == kService);
	}
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
	// The row's own client token: the same for every call of its launch.
	TEST_EXPECT(argv[10] == "opennova-opennova-server-m12");
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
	TEST_EXPECT(assigned(call.user_data, "SERVICE_ADDRESS") == kService);
	TEST_EXPECT(assigned(call.user_data, "SERVICE_PORT") == "8070");
	TEST_EXPECT(assigned(call.user_data, "SERVE_IMAGE") == kImage);
	// No hostd image of its own: the serve image's agent.
	TEST_EXPECT(assigned(call.user_data, "HOSTD_IMAGE") == kImage);
	// In the order the template attaches the volumes: the data sets' sorted one.
	TEST_EXPECT(assigned(call.user_data, "DATA_SETS") ==
			"dfx2_consumer=od-dfx2 jop_2_consumer=od-jop2");
	TEST_EXPECT(!call.user_data.empty() && !fs::exists(fs::u8path(call.user_data_path)));
	// The token is in the user-data alone, never on the command line.
	for (const std::string &arg : argv)
		TEST_EXPECT(arg.find(launch.bootstrap_token) == std::string::npos);

	// Right after the launch EC2 may not list the machine yet: within the grace it
	// reads Pending, with the address its launch read.
	rig.runner.replies.push_back(answered(listing({})));
	const hosted::CloudMachine status = rig.driver.describe(kMachineA);
	TEST_EXPECT(status.state == CloudState::Pending && status.private_address == "10.44.0.21");
	TEST_EXPECT(rig.runner.calls.size() == 2 && rig.runner.calls[1].argv == describe_argv());
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
	TEST_EXPECT(rig.runner.calls[1].argv[10] == "opennova-opennova-server-m31");
	TEST_EXPECT(rig.runner.calls[0].user_data == rig.runner.calls[1].user_data &&
			assigned(rig.runner.calls[1].user_data, "MACHINE_TOKEN") == launch.bootstrap_token);
	TEST_EXPECT(!fs::exists(fs::u8path(rig.runner.calls[1].user_data_path)));
	return 0;
}

int test_launch_per_kind(const std::string &scratch) {
	hosted::Ec2MachineConfig config = config_in(scratch);
	config.hostd_image = kHostdImage;
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
	// A hostd image of its own: the agent comes out of it, the containers run the serve image.
	TEST_EXPECT(assigned(rig.runner.calls[1].user_data, "HOSTD_IMAGE") == kHostdImage);
	TEST_EXPECT(assigned(rig.runner.calls[1].user_data, "SERVE_IMAGE") == kImage);
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
	TEST_EXPECT(rig.driver.describe(kMachineA).state == CloudState::Gone);
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
	const hosted::CloudMachine a = rig.driver.describe(kMachineA);
	TEST_EXPECT(a.state == CloudState::Running && a.private_address == "10.44.0.21");
	TEST_EXPECT(a.public_address == "54.1.2.3");
	TEST_EXPECT(rig.runner.calls.size() == 2 && rig.runner.calls[1].argv == describe_argv());
	TEST_EXPECT(rig.driver.describe(kMachineB).state == CloudState::Pending);
	for (const char *gone : { "i-0ccccccccccccccc3", "i-0ddddddddddddddd4", "i-0eeeeeeeeeeeeeee5",
				 "i-0fffffffffffffff6", "i-0aaaaaaaaaaaaaa07", "i-0123456789abcdef9", "local-3-1" })
		TEST_EXPECT(rig.driver.describe(gone).state == CloudState::Gone);
	TEST_EXPECT(rig.driver.describe("i-0ccccccccccccccc3").private_address.empty());
	// All of that from the one describe-instances: the listing is kept kTtl.
	TEST_EXPECT(rig.runner.calls.size() == 2);
	rig.now += kTtl;
	rig.runner.replies.push_back(
			answered(listing({ instance_json(kMachineA, "shutting-down", "10.44.0.21", "7") })));
	TEST_EXPECT(rig.driver.describe(kMachineA).state == CloudState::Gone);
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
		const hosted::CloudMachine status = rig.driver.describe(kMachineA);
		TEST_EXPECT(rig.runner.calls.size() == calls + 1);
		TEST_EXPECT(status.state == CloudState::Unknown && status.private_address == "10.44.0.21");
		rig.now += kTtl - 1;
		TEST_EXPECT(rig.driver.describe(kMachineA).state == CloudState::Unknown);
		TEST_EXPECT(rig.driver.describe(kMachineB).state == CloudState::Unknown);
		TEST_EXPECT(rig.runner.calls.size() == calls + 1);
		rig.now += 1;
	}
	// The sweep's list_tagged reads fresh, back-off or not.
	rig.runner.replies.push_back(
			answered(listing({ instance_json(kMachineB, "running", "10.44.0.22", "8") })));
	{
		const size_t calls = rig.runner.calls.size();
		const auto tagged = rig.driver.list_tagged();
		TEST_EXPECT(rig.runner.calls.size() == calls + 1 && tagged.size() == 2);
	}
	// Past the grace, a launched machine the listing lacks is Gone.
	rig.now += kGrace + 1;
	rig.runner.replies.push_back(
			answered(listing({ instance_json(kMachineB, "running", "10.44.0.22", "8") })));
	TEST_EXPECT(rig.driver.describe(kMachineA).state == CloudState::Gone);
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
	TEST_EXPECT(rig.driver.describe(kMachineA).state == CloudState::Gone);
	rig.now += kTtl;
	TEST_EXPECT(rig.driver.describe(kMachineA).state == CloudState::Gone);
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
	TEST_EXPECT(rig.driver.describe(kMachineB).state == CloudState::Gone &&
			rig.driver.describe("i-0ddddddddddddddd4").state == CloudState::Gone &&
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
	TEST_EXPECT(rig.driver.describe("i-0ccccccccccccccc3").state == CloudState::Running);
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
	// The defaults: a loopback internal listener admitting loopback, the brief's knobs.
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
		TEST_EXPECT(
				c.slot_pids == 128 && c.machine_script_path == "/app/hosted/machine_user_data.sh");
		TEST_EXPECT(c.ec2.service_address == "127.0.0.1" && c.ec2.service_port == 8070);
	}
	// A deployment's.
	{
		hosted::FleetConfig c;
		std::vector<std::string> errors;
		const bool ok = hosted::parse_fleet_config(
				env_of({ { "ONNET_INTERNAL_HTTP_BIND", "10.44.0.10:8070" },
						{ "ONNET_HOSTED_BOOTSTRAP_CIDRS", "10.44.0.0/16, 10.45.0.0/20" },
						{ "ONNET_FLEET_SHARED_SLOTS", "16" }, { "ONNET_FLEET_MAX_SHARED", "3" },
						{ "ONNET_FLEET_MAX_OWN", "0" }, { "ONNET_FLEET_IDLE_MINUTES", "45" },
						{ "ONNET_FLEET_SLOT_MEMORY_MB", "768" }, { "ONNET_FLEET_SLOT_CPUS", "0.5" },
						{ "ONNET_HOSTED_EC2_LAUNCH_TEMPLATES",
								"{\"own\": \" lt-0abcdef0123456789:3 \", \"shared\": \"" +
										std::string(kShared) + "\"}" },
						{ "ONNET_HOSTED_EC2_REGION", kRegion },
						{ "ONNET_HOSTED_EC2_SUBNET", kSubnet },
						{ "ONNET_HOSTED_DEPLOYMENT", kDeployment },
						{ "ONNET_HOSTED_SERVE_IMAGE", kImage },
						{ "ONNET_HOSTED_HOSTD_IMAGE", kHostdImage },
						{ "ONNET_HOSTED_AWS_CLI", "/opt/aws/bin/aws" },
						{ "ONNET_HOSTED_MACHINE_SCRIPT", "/srv/machine.sh" } }),
				c, errors);
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
		TEST_EXPECT(
				c.ec2.shared_template == kShared && c.ec2.own_template == "lt-0abcdef0123456789:3");
		TEST_EXPECT(c.ec2.region == kRegion && c.ec2.subnet == kSubnet &&
				c.ec2.deployment == kDeployment);
		TEST_EXPECT(c.ec2.serve_image == kImage && c.ec2.aws_cli == "/opt/aws/bin/aws");
		TEST_EXPECT(c.ec2.hostd_image == kHostdImage);
		TEST_EXPECT(c.ec2.service_address == "10.44.0.10" && c.ec2.service_port == 8070);
		TEST_EXPECT(c.machine_script_path == "/srv/machine.sh");
		// What the agent routes take from it.
		const hosted::AgentRouteConfig routes =
				hosted::agent_route_config(c, { "jop_2_consumer", "dfx2_consumer" });
		TEST_EXPECT(routes.admitted.size() == 2 && routes.data_sets.size() == 2);
		TEST_EXPECT(routes.data_sets.at("jop_2_consumer") == "/opt/game/jop_2_consumer");
	}
	// Every refusal names its variable and keeps the default.
	const auto refused = [](const char *name, const char *value) {
		hosted::FleetConfig c;
		const hosted::FleetConfig defaults;
		std::vector<std::string> errors;
		const bool ok = hosted::parse_fleet_config(env_of({ { name, value } }), c, errors);
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

// --- the local driver, no service -------------------------------------------------------------

int test_local_driver_without_service(const std::string &scratch) {
	hosted::LocalMachineConfig config;
	config.serve_binary = "/bin/true";
	config.work_dir = scratch + "/local";
	config.service_url = "http://127.0.0.1:1"; // nothing listens: the agent never bootstraps
	config.bootstrap_retry_ms = 50;
	hosted::LocalMachineDriver driver(config);
	TEST_EXPECT(std::string(driver.name()) == "local");
	std::string error;
	TEST_EXPECT(!driver.launch(hosted::MachineLaunch{}, error) && !error.empty());
	const auto a = driver.launch(launch_of(3), error);
	const auto b = driver.launch(launch_of(4, MachineKind::Own), error);
	TEST_EXPECT(a && b && a->cloud_id != b->cloud_id);
	TEST_EXPECT(a->private_address == "127.0.0.1" && a->instance_type == "local");
	TEST_EXPECT(driver.describe(a->cloud_id).state == CloudState::Running);
	TEST_EXPECT(driver.describe("local-99-1").state == CloudState::Gone);
	const auto view = driver.view(a->cloud_id);
	TEST_EXPECT(view && !view->bootstrapped && view->syncs == 0 && view->containers.empty());
	auto tagged = driver.list_tagged();
	TEST_EXPECT(tagged.size() == 2);
	// Slot ports: free ones, a game and an admin port apart, admitting loopback.
	const hosted::SlotPlan plan = driver.slot_plan(a->cloud_id, 0);
	TEST_EXPECT(
			plan.game_port != 0 && plan.admin_port != 0 && plan.admin_allowed_ip == "127.0.0.1");
	driver.terminate(a->cloud_id);
	driver.terminate(a->cloud_id);
	TEST_EXPECT(
			driver.describe(a->cloud_id).state == CloudState::Gone && !driver.view(a->cloud_id));
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
		{ "volume_labels", [](const std::string &) { return test_volume_labels(); } },
		{ "machine_script", test_machine_script },
		{ "slot_plan", test_slot_plan },
		{ "launch", test_launch },
		{ "launch_retried", test_launch_retried },
		{ "launch_per_kind", test_launch_per_kind },
		{ "launch_failures", test_launch_failures },
		{ "describe", test_describe },
		{ "describe_failures", test_describe_failures },
		{ "terminate", test_terminate },
		{ "list_tagged", test_list_tagged },
		{ "fleet_config", [](const std::string &) { return test_fleet_config(); } },
		{ "local_driver_without_service", test_local_driver_without_service },
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
