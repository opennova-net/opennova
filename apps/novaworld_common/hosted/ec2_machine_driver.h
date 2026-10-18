#pragma once

// The cloud MachineDriver (ONNET_HOSTED_PROVIDER=ec2): each fleet machine is
// an EC2 machine launched from the launch template of its kind
// (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES, {"shared": "lt-...", "own": "lt-..."};
// infra/aws/fleet.tf: the ECS-optimized Amazon Linux 2023 image the
// deployment pins by name (fleet_ami), the kind's machine type, its root
// volume (the trees the agent fetches live there), the machines' security
// group, their instance profile (the ECS agent's registration and the logs),
// IMDSv2 required and InstanceInitiatedShutdownBehavior=terminate; no
// user-data). Its user-data is machine/machine_user_data.sh with the
// machine's values filled in: the fleet cluster it joins
// (ONNET_HOSTED_EC2_CLUSTER, infra/aws/fleet_agent.tf's), the machine's id and
// kind, and its one-time bootstrap token. The script writes the token and the
// machine id for the agent, reserves the slots' ports and points the image's
// ECS agent at the cluster with the machine's id and kind as its instance
// attributes; the cluster's daemon service then runs opennova-hostd
// (apps/hostd) on the machine, which fences the slots, redeems the token at
// the fleet's internal listener and runs the servers. A machine is terminated
// at its end, never stopped.
//
// The driver runs the AWS CLI v2 through a CommandRunner (an argv array, no
// shell, a timeout), its credentials the worker task's IAM role. Every call
// names the region, asks for JSON and turns off the pager; every failure is
// logged and handled here, never thrown, but by list_tagged. No lock is held
// across a CLI call: the driver's state is read and written around it.
//
//   launch      run-instances: the kind's launch template (its default
//               version, or the one the variable pins), a subnet, one
//               machine, the tags opennova:hosted=1, opennova:deployment,
//               opennova:machine-kind, opennova:machine-id and Name on the
//               machine and its volumes (fleet.tf's service policy admits
//               these keys alone), the row's own client token in that subnet
//               (client_token), and the user-data from an owner-only file
//               deleted once the call returns. The subnet: of several (one per
//               zone), the one holding fewer of the deployment's live machines
//               (pending or running, from a listing at most describe_ttl_ms
//               old), ties to the first listed; a machine this row already
//               started, its own. Each subnet's first call is recorded on
//               the row first (MachineLaunch::record_subnet; one that cannot
//               be recorded calls nothing more), and a run again (its job's
//               attempt past the first: a claim that lapsed, a database error)
//               puts the subnet its row recorded (MachineLaunch::subnet)
//               first, so it names the same token and EC2 answers with the
//               machine that call launched, however late EC2 lists it. A run
//               again whose row recorded none reads where an earlier run
//               launched from a describe-instances of its machine id alone,
//               read then, and puts that subnet first; one whose read fails
//               launches nothing (the other subnet's token would launch a
//               second machine). A zone that
//               cannot take the launch now (InsufficientInstanceCapacity, or
//               Unsupported: the type not offered there) hands it to the next
//               subnet, under that subnet's token (EC2 refuses a token it has
//               seen with other parameters). A call cut short (launch_timeout_ms, or the CLI's
//               exit 255: the network) is made again, the same, up to
//               launch_attempts: EC2 answers a client token it has seen with
//               the machine it launched for it, so a slow answer never leaves a
//               second machine running. A reply naming no machine fails the
//               launch; one naming a machine with no private address ends it
//               at once (its agent could never be admitted).
//   describe    from ONE describe-instances of every machine this deployment
//               tagged, read at most every describe_ttl_ms (the callers asking
//               while it is read wait for its answer): pending -> Pending;
//               running -> Running; shutting-down, terminated,
//               stopping, stopped -> Gone. A machine the reply lacks is Gone,
//               but Pending within launch_grace_ms of its launch, as its row
//               dates it (describe's launched_ago_ms: any worker judges it
//               alike, the launch's or another). A describe that fails reads
//               Unknown (with the addresses last read) and runs the CLI again
//               only describe_ttl_ms later.
//   terminate   describe-instances of that machine on both tags first: one
//               that does not carry opennova:hosted=1 and this deployment's
//               opennova:deployment is never ended (a row's cloud id could name
//               any machine of the account), nor is one whose tags did not
//               read; then terminate-instances. A machine EC2 no longer knows
//               is no error, and an id that is no EC2 id is skipped.
//   list_tagged a fresh describe-instances on both tags; pending, running,
//               stopping and stopped machines. Throws std::runtime_error when
//               the listing fails.
//   profile     the supervisor's view (machine_driver.h MachineProfile, the
//               fleet's): kBootTimeoutMs, and slot k's ports (slots.h:
//               32768 + k, 32800 + k) with the fleet's network
//               (admin_pattern of admin_address) for admin.cfg's ip_restrict.

#include "command_runner.h"
#include "machine_driver.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct Ec2MachineConfig {
	std::string aws_cli = "/usr/local/bin/aws"; // ONNET_HOSTED_AWS_CLI: absolute, run as given
	std::string region; // ONNET_HOSTED_EC2_REGION
	// ONNET_HOSTED_EC2_SUBNETS (subnet-..., comma-separated): the subnets machines
	// launch into, one per zone, each giving its machines a public IPv4.
	std::vector<std::string> subnets;
	std::string deployment; // ONNET_HOSTED_DEPLOYMENT: the tag that scopes every call
	// The launch templates by kind (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES' "shared"
	// and "own"): "lt-<id>", or "lt-<id>:<version>" to pin one.
	std::string shared_template;
	std::string own_template;
	// The serve image the machines' containers run, named by their bootstrap
	// (ONNET_HOSTED_SERVE_IMAGE, pinned at deploy).
	std::string serve_image;
	// The fleet cluster the machines join as ECS container instances
	// (ONNET_HOSTED_EC2_CLUSTER, opennova-<deployment>-fleet), whose daemon
	// service runs opennova-hostd on each.
	std::string cluster;
	// The fleet's private IPv4, its internal listener's bind
	// (ONNET_INTERNAL_HTTP_BIND), which its admin links connect from: the
	// machines' admin.cfg admits its network (admin_pattern). The
	// supervisor's profile alone reads it; check_config does not.
	std::string admin_address;
	std::string machine_script; // machine/machine_user_data.sh's text (ONNET_HOSTED_MACHINE_SCRIPT)
	// Owner-only: the user-data files the CLI reads. The worker's, made before
	// it constructs the driver (check_config does not ask for it).
	std::string scratch_dir;
	uint64_t describe_ttl_ms = 10 * 1000;
	uint64_t launch_grace_ms = 3 * 60 * 1000;
	// A describe-instances or terminate-instances call: a job's worker waits on
	// it, no watched loop.
	int command_timeout_ms = 20 * 1000;
	// A run-instances call, and how many make a launch (the class comment).
	int launch_timeout_ms = 12 * 1000;
	int launch_attempts = 2;
};

// The values one machine's user-data carries.
struct MachineUserData {
	std::string cluster; // the fleet cluster its ECS agent joins
	int64_t machine_id = 0;
	MachineKind kind = MachineKind::Shared;
	std::string bootstrap_token;
};

class Ec2MachineDriver : public MachineDriver {
public:
	static constexpr const char *kTagHosted = "opennova:hosted";
	static constexpr const char *kTagDeployment = "opennova:deployment";
	static constexpr const char *kTagMachineKind = "opennova:machine-kind";
	static constexpr const char *kTagMachineId = "opennova:machine-id";
	// The machine script's blanks, each filled in single quotes.
	static constexpr const char *kBlankCluster = "@OPENNOVA_ECS_CLUSTER@";
	static constexpr const char *kBlankMachineId = "@OPENNOVA_MACHINE_ID@";
	static constexpr const char *kBlankMachineKind = "@OPENNOVA_MACHINE_KIND@";
	static constexpr const char *kBlankMachineToken = "@OPENNOVA_MACHINE_TOKEN@";
	// EC2's raw user-data cap.
	static constexpr size_t kMaxUserDataBytes = 16 * 1024;
	// ONNET_HOSTED_DEPLOYMENT's longest: its client tokens then fit EC2's 64.
	static constexpr size_t kMaxDeploymentBytes = 32;
	// From the launch to the agent's first report: EC2's pending, cloud-init,
	// the ECS agent's registration, the hostd image's pull, the agent's start
	// and its bootstrap.
	static constexpr uint64_t kBootTimeoutMs = 15 * 60 * 1000;

	using Clock = std::function<uint64_t()>;

	// `runner` must outlive the driver.
	Ec2MachineDriver(Ec2MachineConfig config, proc::CommandRunner &runner, Clock now_ms = {});

	// "" when `config` can run, else why not (a boot warning; hosting off).
	// Every program asks it, so each decides alike whether hosting is on.
	static std::string check_config(const Ec2MachineConfig &config);
	// An internal listener's URL, http://<name>:<port> with a name of
	// letters, digits, '.' and '-' (a DNS name or a dotted quad) and an
	// explicit port; its name and port, false for anything else (a path, a user, a
	// character a single-quoted shell word or a URL would read otherwise).
	static bool parse_service_url(std::string_view url, std::string &name, uint16_t &port);
	// `script` with its blanks filled from `values`; nullopt when a value holds
	// anything outside its shape (each goes in single quotes), the script lacks
	// a blank, or the result passes kMaxUserDataBytes.
	static std::optional<std::string> render_user_data(
			const std::string &script, const MachineUserData &values);
	// An ECS cluster's name: 1..255 letters, digits, '-' and '_'.
	static bool is_cluster_name(std::string_view name);

	// The supervisor's view of the machines `config` launches: no driver needed.
	static MachineProfile profile(const Ec2MachineConfig &config);
	// The ip_restrict pattern a machine's admin.cfg admits the fleet by: its
	// private address's /16, "10.44.*" for 10.44.0.17 (the deployment's VPC).
	// The fleet leader's address moves with each replacement of its task, and
	// a server keeps the admin.cfg its start rendered, so a pattern naming one
	// address would refuse a new leader's admin links (and its health checks
	// would then fail every running server); the machines' security group
	// admits the admin ports from the fleet's group alone. `fleet_address`
	// itself when it is no dotted quad.
	static std::string admin_pattern(const std::string &fleet_address);

	const char *name() const override { return "ec2"; }
	std::optional<LaunchedMachine> launch(const MachineLaunch &launch, std::string &error) override;
	CloudMachine describe(const std::string &cloud_id, int64_t launched_ago_ms) override;
	void terminate(const std::string &cloud_id) override;
	std::vector<TaggedCloudMachine> list_tagged() override;

	// Why the CLI call `what` failed, for the log: its outcome and the start of
	// its standard error.
	static std::string cli_failure(const char *what, const proc::CommandResult &result);
	// A launch's client token in `subnet`: "<deployment>-m<machine id>-<the
	// subnet id's last 8 hex digits>", the same for every call of the row's
	// launch there, and at most 64 bytes (EC2's cap) for the longest deployment.
	static std::string client_token(
			const std::string &deployment, int64_t machine_id, const std::string &subnet);

private:
	struct Instance {
		std::string state; // EC2's State.Name
		std::string private_address;
		std::string public_address;
		std::string subnet; // SubnetId
		int64_t machine_id = 0;
	};

	// argv: the CLI, `ec2`, the operation and its parameters, then the global
	// options every call takes.
	std::vector<std::string> command(
			const std::string &operation, std::vector<std::string> params) const;
	// describe-instances' --filters on both tags (this deployment's), and with
	// `machine_id` its machine-id tag too.
	std::string tag_filters(int64_t machine_id = 0) const;
	// A describe-instances reply's machines that carry both tags, by id; false
	// with `error` when it does not read.
	bool parse_listing(const std::string &out, std::map<std::string, Instance> &listed,
			std::string &error) const;
	// Every tagged machine (the CLI call; no lock held); false with `error`.
	bool read_listing(std::map<std::string, Instance> &listed, std::string &error);
	// A listing read at `now_ms` made the driver's view (under mu_).
	void take_listing(std::map<std::string, Instance> listed, uint64_t now_ms);
	void note_describe_failure(uint64_t now_ms, const std::string &error);
	// The listing made at most describe_ttl_ms before `now_ms`: read now when it
	// is older, unless a failed read is backing off, or the answer of the read
	// another caller has in flight waited for. Whether it is that fresh.
	bool refresh_listing(uint64_t now_ms);
	// The subnets `launch` tries, in order (the class comment); nullopt with
	// `error` for a run again whose row recorded no subnet when where an
	// earlier run launched did not read.
	std::optional<std::vector<std::string>> subnet_order(
			const MachineLaunch &launch, std::string &error);
	// terminate-instances on `cloud_id`, unchecked (terminate checks the tags
	// first; a launch's own machine carries them); false with `why`.
	bool terminate_now(const std::string &cloud_id, std::string &why);
	// The machine ended (or gone already, as `how` says): Gone from now on.
	void forget(const std::string &cloud_id, const char *how);

	const Ec2MachineConfig config_; // never changes, so read without the lock
	proc::CommandRunner &runner_;
	Clock now_;
	// Guards what follows, never held across a CLI call.
	std::mutex mu_;
	std::map<std::string, Instance> instances_;
	bool have_listing_ = false;
	uint64_t listed_ms_ = 0;
	// A refresh_listing's describe-instances runs; the others wait on
	// listing_read_ for its answer.
	bool listing_in_flight_ = false;
	std::condition_variable listing_read_;
	// This driver's terminations (Gone at once). Its launches' grace is the
	// rows' (describe's launched_ago_ms), never this process's memory.
	std::map<std::string, uint64_t> terminated_ms_;
	bool describe_failing_ = false;
	uint64_t describe_failed_ms_ = 0;
	uint64_t describe_failure_logged_ms_ = 0;
};

} // namespace opennova::novaworld_server::hosted
