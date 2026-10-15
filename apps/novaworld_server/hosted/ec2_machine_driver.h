#pragma once

// The cloud MachineDriver (ONNET_HOSTED_PROVIDER=ec2): each fleet machine is
// an EC2 machine launched from the launch template of its kind
// (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES, {"shared": "lt-...", "own": "lt-..."};
// infra/aws/fleet.tf: a stock Ubuntu image, the kind's machine type, every
// data set's private encrypted snapshot as a volume of its own (attached as
// /dev/sdf, /dev/sdg, ... in the data sets' sorted order), each file system
// labelled by volume_label, by which alone the machine mounts it, the
// machines' security group, their instance profile, IMDSv2
// required and InstanceInitiatedShutdownBehavior=terminate; no user-data).
// Its user-data is machine/machine_user_data.sh with the machine's values
// filled in: the service's internal listener, the machine's id and one-time
// bootstrap token, the serve image its containers run, the hostd image the
// agent is copied out of, and the data sets (sorted) with their volumes'
// labels. The script installs docker, mounts the data sets, fences the slots'
// users off the instance metadata and the service's internal port, and starts
// opennova-hostd (apps/hostd), which redeems the token. A machine is
// terminated at its end, never stopped.
//
// The driver runs the AWS CLI v2 through a CommandRunner (an argv array, no
// shell, a timeout), its credentials the service instance's role. Every call
// names the region, asks for JSON and turns off the pager; every failure is
// logged and handled here, never thrown, but by list_tagged. No lock is held
// across a CLI call: the driver's state is read and written around it.
//
//   launch      run-instances: the kind's launch template (its default
//               version, or the one the variable pins), the subnet, one
//               machine, the tags opennova:hosted=1, opennova:deployment,
//               opennova:machine-kind, opennova:machine-id and Name on the
//               machine and its volumes (fleet.tf's service policy admits
//               these keys alone), the row's own client token
//               (opennova-<deployment>-m<machine id>), and the user-data from
//               an owner-only file deleted once the call returns. A call cut
//               short (launch_timeout_ms, or the CLI's exit 255: the network)
//               is made again, the same, up to launch_attempts: EC2 answers a
//               client token it has seen with the machine it launched for it,
//               so a slow answer never leaves a second machine running. A
//               reply naming no machine fails the launch; one naming a
//               machine with no private address ends it at once (its agent
//               could never be admitted).
//   describe    from ONE describe-instances of every machine this deployment
//               tagged, read at most every describe_ttl_ms: pending ->
//               Pending; running -> Running; shutting-down, terminated,
//               stopping, stopped -> Gone. A machine the reply lacks is Gone,
//               but Pending within launch_grace_ms of this driver's launch. A
//               describe that fails reads Unknown (with the addresses last
//               read) and runs the CLI again only describe_ttl_ms later.
//   terminate   describe-instances of that machine on both tags first: one
//               that does not carry opennova:hosted=1 and this deployment's
//               opennova:deployment is never ended (a row's cloud id could name
//               any machine of the account), nor is one whose tags did not
//               read; then terminate-instances. A machine EC2 no longer knows
//               is no error, and an id that is no EC2 id is skipped.
//   list_tagged a fresh describe-instances on both tags; pending, running,
//               stopping and stopped machines. Throws std::runtime_error when
//               the listing fails.
//   slot_plan   slot k's ports (slots.h: 32768 + k, 32800 + k) and the
//               service's address for admin.cfg's ip_restrict.

#include "command_runner.h"
#include "machine_driver.h"

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
	std::string subnet; // ONNET_HOSTED_EC2_SUBNET (subnet-...)
	std::string deployment; // ONNET_HOSTED_DEPLOYMENT: the tag that scopes every call
	// The launch templates by kind (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES' "shared"
	// and "own"): "lt-<id>", or "lt-<id>:<version>" to pin one.
	std::string shared_template;
	std::string own_template;
	// The serve image the machines' containers run, pre-pulled at their boot
	// and named by their bootstrap (ONNET_HOSTED_SERVE_IMAGE, pinned at deploy).
	std::string serve_image;
	// The image opennova-hostd is copied out of (ONNET_HOSTED_HOSTD_IMAGE,
	// pinned at deploy: an opennova-serve build); empty, the serve image.
	std::string hostd_image;
	// The service's internal listener (ONNET_INTERNAL_HTTP_BIND): its private
	// IPv4 and port, where the agents post and the address admin.cfg admits.
	std::string service_address;
	uint16_t service_port = 8070;
	// The data sets every machine mounts, by their games slugs: the templates'
	// own (fleet.tf's var.hosted_data_snapshots), each one's volume attached in
	// their sorted order.
	std::vector<std::string> data_sets;
	std::string machine_script; // machine/machine_user_data.sh's text (ONNET_HOSTED_MACHINE_SCRIPT)
	std::string scratch_dir; // owner-only: the user-data files the CLI reads
	uint64_t describe_ttl_ms = 10 * 1000;
	uint64_t launch_grace_ms = 3 * 60 * 1000;
	// describe-instances and terminate-instances run inside the supervisor's pass
	// too, so each stays under its stall watchdog (health.h, 30 s).
	int command_timeout_ms = 20 * 1000;
	// A run-instances call, and how many make a launch (the class comment):
	// together under the supervisor's stall watchdog (health.h, 30 s), whose
	// pass the launch holds up.
	int launch_timeout_ms = 12 * 1000;
	int launch_attempts = 2;
};

// The values one machine's user-data carries.
struct MachineUserData {
	std::string service_address; // the internal listener's IPv4
	uint16_t service_port = 0;
	int64_t machine_id = 0;
	std::string bootstrap_token;
	std::string serve_image;
	std::string hostd_image;
	// Each data set the machine mounts and its volume's label, in the order
	// the launch template attaches the volumes (the data sets' sorted order).
	std::vector<std::pair<std::string, std::string>> data_sets;
};

class Ec2MachineDriver : public MachineDriver {
public:
	static constexpr const char *kTagHosted = "opennova:hosted";
	static constexpr const char *kTagDeployment = "opennova:deployment";
	static constexpr const char *kTagMachineKind = "opennova:machine-kind";
	static constexpr const char *kTagMachineId = "opennova:machine-id";
	// The machine script's blanks, each filled in single quotes.
	static constexpr const char *kBlankServiceAddress = "@OPENNOVA_SERVICE_ADDRESS@";
	static constexpr const char *kBlankServicePort = "@OPENNOVA_SERVICE_PORT@";
	static constexpr const char *kBlankMachineId = "@OPENNOVA_MACHINE_ID@";
	static constexpr const char *kBlankMachineToken = "@OPENNOVA_MACHINE_TOKEN@";
	static constexpr const char *kBlankServeImage = "@OPENNOVA_SERVE_IMAGE@";
	static constexpr const char *kBlankHostdImage = "@OPENNOVA_HOSTD_IMAGE@";
	static constexpr const char *kBlankDataSets = "@OPENNOVA_DATA_SETS@";
	// EC2's raw user-data cap.
	static constexpr size_t kMaxUserDataBytes = 16 * 1024;
	// ONNET_HOSTED_DEPLOYMENT's longest: its client tokens then fit EC2's 64.
	static constexpr size_t kMaxDeploymentBytes = 32;
	// An ext4 label holds at most 16 bytes.
	static constexpr size_t kMaxVolumeLabelBytes = 16;
	// From the launch to the agent's first report: EC2's pending, cloud-init,
	// the docker install (its apt retries), the data volumes' wait, the serve
	// image's pull and the agent's bootstrap.
	static constexpr uint64_t kBootTimeoutMs = 15 * 60 * 1000;

	using Clock = std::function<uint64_t()>;

	// `runner` must outlive the driver.
	Ec2MachineDriver(Ec2MachineConfig config, proc::CommandRunner &runner, Clock now_ms = {});

	// "" when `config` can run, else why not (a boot warning; hosting off).
	static std::string check_config(const Ec2MachineConfig &config);
	// The label of data set `data_set`'s volume: "od-" and the slug without
	// its "_consumer" suffix and its underscores (jop_2_consumer: od-jop2), or
	// "" when that is no ext4 label (over kMaxVolumeLabelBytes, or a character
	// outside a-z, 0-9 and '-'). deploy/hosted-data writes the same.
	static std::string volume_label(std::string_view data_set);
	// `script` with its blanks filled from `values`; nullopt when a value holds
	// anything outside its shape (each goes in single quotes), the script lacks
	// a blank, or the result passes kMaxUserDataBytes.
	static std::optional<std::string> render_user_data(
			const std::string &script, const MachineUserData &values);

	const char *name() const override { return "ec2"; }
	uint64_t boot_timeout_ms() const override { return kBootTimeoutMs; }
	std::optional<LaunchedMachine> launch(const MachineLaunch &launch, std::string &error) override;
	CloudMachine describe(const std::string &cloud_id) override;
	void terminate(const std::string &cloud_id) override;
	std::vector<TaggedCloudMachine> list_tagged() override;
	SlotPlan slot_plan(const std::string &cloud_id, int slot) override;

	// Why the CLI call `what` failed, for the log: its outcome and the start of
	// its standard error.
	static std::string cli_failure(const char *what, const proc::CommandResult &result);
	// A launch's client token: "opennova-<deployment>-m<machine id>", the same
	// for every call of the row's launch.
	static std::string client_token(const std::string &deployment, int64_t machine_id);

private:
	struct Instance {
		std::string state; // EC2's State.Name
		std::string private_address;
		std::string public_address;
		int64_t machine_id = 0;
	};

	// argv: the CLI, `ec2`, the operation and its parameters, then the global
	// options every call takes.
	std::vector<std::string> command(
			const std::string &operation, std::vector<std::string> params) const;
	// describe-instances' --filters on both tags (this deployment's).
	std::string tag_filters() const;
	// A describe-instances reply's machines that carry both tags, by id; false
	// with `error` when it does not read.
	bool parse_listing(const std::string &out, std::map<std::string, Instance> &listed,
			std::string &error) const;
	// Every tagged machine (the CLI call; no lock held); false with `error`.
	bool read_listing(std::map<std::string, Instance> &listed, std::string &error);
	// A listing read at `now_ms` made the driver's view (under mu_).
	void take_listing(std::map<std::string, Instance> listed, uint64_t now_ms);
	void note_describe_failure(uint64_t now_ms, const std::string &error);
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
	// This driver's launches (the grace) and terminations (Gone at once).
	std::map<std::string, uint64_t> launched_ms_;
	std::map<std::string, uint64_t> terminated_ms_;
	bool describe_failing_ = false;
	uint64_t describe_failed_ms_ = 0;
	uint64_t describe_failure_logged_ms_ = 0;
};

} // namespace opennova::novaworld_server::hosted
