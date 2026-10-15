#pragma once

// The cloud MachineProvider (ONNET_HOSTED_PROVIDER=ec2; ADR 0053): one EC2
// machine per hosted server, launched from its data set's launch template
// (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES; infra/aws, "Hosted game servers": one per
// data set, each a stock Ubuntu image, the machine type, that data set's
// private encrypted snapshot as a second volume, the game security group, the
// machine's instance profile, IMDSv2 required, and
// InstanceInitiatedShutdownBehavior=terminate), running opennova-serve in its
// container (machine/user_data.sh). A machine is terminated at its server's
// end, never stopped, so each launch has a new public address.
//
// The provider runs the AWS CLI v2 through a CommandRunner (an argv array, no
// shell, a timeout; command_runner.h), its credentials the service instance's
// role (IMDSv2). Every call names the region, asks for JSON and turns off the
// pager; every failure (the CLI's exit, its timeout, a reply that is no JSON or
// lacks what is read) is logged and handled here, never thrown, but by
// list_tagged.
//
//   launch      run-instances: the spec's data set's launch template, its
//               default version, the subnet, one machine, the tags opennova:hosted=1,
//               opennova:deployment=<ONNET_HOSTED_DEPLOYMENT>,
//               opennova:server-id=<id> and Name on the machine and its
//               volumes, a client token (the CLI's own retries of the call
//               launch once; a later launch is a new call, and the
//               supervisor's sweep ends a machine no row holds), and
//               the user-data: machine/user_data.sh with the bootstrap URL (the
//               service's PRIVATE address) and a one-time token filled in, and
//               nothing else (bootstrap.h). The file the CLI reads it from is
//               owner-only and deleted once the call returns. A reply naming no
//               machine fails the launch (a machine it launched anyway carries
//               the tags, and the supervisor's sweep ends it).
//   describe    the machine's state from ONE describe-instances of every
//               machine this deployment tagged, read at most every
//               describe_ttl_ms (the supervisor describes each live server
//               every pass): pending -> Pending; running -> Running once it
//               has fetched its bundle (Pending before: the listing's clock
//               starts as the server is about to run); shutting-down,
//               terminated, stopping, stopped -> Gone. A machine the reply
//               lacks is Gone, but within launch_grace_ms of this provider's
//               launch, when EC2 may not list it yet, Pending. A describe
//               that fails reads Pending (with the address last read): the
//               supervisor's clocks still run, but a blip is never a crash.
//               After a failure the CLI runs again only describe_ttl_ms
//               later, so an API outage costs a pass one CLI run, not one per
//               server.
//               The address is the machine's PRIVATE IPv4, where the admin
//               link connects.
//   terminate   terminate-instances; a machine EC2 no longer knows is no
//               error, and a machine id that is no EC2 id (another provider's)
//               is skipped. The machine's token goes even when the call
//               fails. The service's IAM policy lets it end only machines
//               tagged opennova:hosted=1 and this deployment.
//   list_tagged a fresh describe-instances, filtered on opennova:hosted=1 AND
//               this deployment's tag, so a dev stack never ends prod's
//               machines; pending, running, stopping and stopped machines.
//               Throws std::runtime_error when the listing fails, so the
//               supervisor ends nothing on a reply it could not read.
//   plan        the game port kGamePort (game.cfg pins the NovaWorld window to
//               it; the game security group opens it to everyone), the admin
//               port kAdminPort (open to the service's security group alone)
//               and the service's private address for admin.cfg's ip_restrict.

#include "bootstrap.h"
#include "command_runner.h"
#include "data_sets.h"
#include "machine_provider.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct Ec2Config {
	std::string aws_cli = "/usr/local/bin/aws"; // ONNET_HOSTED_AWS_CLI: absolute, run as given
	std::string region;                         // ONNET_HOSTED_EC2_REGION
	// The data sets it runs, each with its launch_template (lt-...;
	// ONNET_HOSTED_EC2_LAUNCH_TEMPLATES).
	std::vector<DataSet> data_sets;
	std::string subnet;                         // ONNET_HOSTED_EC2_SUBNET (subnet-...)
	std::string deployment;                     // ONNET_HOSTED_DEPLOYMENT: the tag that scopes every call
	std::string serve_image;                    // ONNET_HOSTED_SERVE_IMAGE, pinned at deploy
	std::string service_address;                // ONNET_HOSTED_SERVICE_ADDRESS: the service's private IPv4
	uint16_t bootstrap_port = 8080;             // the HTTP listener's port, where the route answers
	std::string machine_script;                 // machine/user_data.sh's text (ONNET_HOSTED_MACHINE_SCRIPT)
	std::string scratch_dir;                    // owner-only: the user-data files the CLI reads
	uint64_t describe_ttl_ms = 10 * 1000;
	uint64_t launch_grace_ms = 3 * 60 * 1000;
	int command_timeout_ms = 90 * 1000;
};

class Ec2Provider : public MachineProvider {
public:
	// game.cfg's NovaWorld window, pinned to one port (mpnovaworldportmin =
	// max), and the remote-admin port: the game security group's two ports
	// (infra/aws, "Hosted game servers"), and the serve image's EXPOSE.
	static constexpr uint16_t kGamePort = 32768;
	static constexpr uint16_t kAdminPort = 32800;
	static constexpr const char *kTagHosted = "opennova:hosted";
	static constexpr const char *kTagDeployment = "opennova:deployment";
	static constexpr const char *kTagServerId = "opennova:server-id";
	// The machine script's two blanks.
	static constexpr const char *kUrlBlank = "@OPENNOVA_BOOTSTRAP_URL@";
	static constexpr const char *kTokenBlank = "@OPENNOVA_BOOTSTRAP_TOKEN@";
	// EC2's raw user-data cap is 16 KB; the script stays well inside it.
	static constexpr size_t kMaxUserDataBytes = 12 * 1024;
	// The supervisor's clocks for a cloud machine (main() sets them with this
	// provider; SupervisorConfig's defaults fit the local one). The boot covers
	// EC2's pending, cloud-init, the docker install (its apt retries), the
	// data volume's wait and the bundle's fetch; the listing covers the image
	// pull, opennova-serve's own boot and its first listing. DEPLOY.md's smoke
	// test records the real durations.
	static constexpr uint64_t kBootTimeoutMs = 10 * 60 * 1000;
	static constexpr uint64_t kListingTimeoutMs = 5 * 60 * 1000;

	using Clock = std::function<uint64_t()>;

	// `runner` and `bootstrap` must outlive the provider.
	Ec2Provider(Ec2Config config, proc::CommandRunner &runner, BootstrapStore &bootstrap, Clock now_ms = {});

	// "" when `config` can run, else why not (a boot warning; hosting off).
	static std::string check_config(const Ec2Config &config);
	// http://<address>:<port>/hosted/bootstrap/<server id>
	static std::string bootstrap_url(const std::string &address, uint16_t port, int64_t server_id);
	// `script` with its two blanks filled; nullopt when a value holds anything
	// but the URL's or the token's characters (each goes in single quotes) or
	// the script lacks a blank.
	static std::optional<std::string> render_user_data(const std::string &script, const std::string &url,
			const std::string &token);

	// The port the HTTP listener serves (main(), once it bound).
	void set_bootstrap_port(uint16_t port);

	const char *name() const override { return "ec2"; }
	MachinePlan plan(int64_t server_id) override;
	std::optional<std::string> launch(const LaunchSpec &spec, std::string &error) override;
	MachineStatus describe(const std::string &machine_id) override;
	void terminate(const std::string &machine_id) override;
	std::vector<TaggedMachine> list_tagged() override;

private:
	struct Instance {
		std::string state;   // EC2's State.Name
		std::string address; // PrivateIpAddress
		int64_t server_id = 0;
	};

	// argv: the CLI, `ec2`, the operation and its parameters, then the global
	// options every call takes.
	std::vector<std::string> command(const std::string &operation, std::vector<std::string> params) const;
	// The tagged machines, read into instances_; false with `error`.
	bool refresh(uint64_t now_ms, std::string &error);
	void note_describe_failure(uint64_t now_ms, const std::string &error);

	Ec2Config config_;
	proc::CommandRunner &runner_;
	BootstrapStore &bootstrap_;
	Clock now_;
	std::mutex mu_;
	std::map<std::string, Instance> instances_;
	bool have_listing_ = false;
	uint64_t listed_ms_ = 0;
	// This provider's launches (the grace) and terminations (Gone at once).
	std::map<std::string, uint64_t> launched_ms_;
	std::map<std::string, uint64_t> terminated_ms_;
	bool describe_failing_ = false;
	uint64_t describe_failed_ms_ = 0;
	uint64_t describe_failure_logged_ms_ = 0;
};

// Why the CLI call `what` failed, for the log: its outcome and the start of
// its standard error.
std::string aws_failure(const char *what, const proc::CommandResult &result);

} // namespace opennova::novaworld_server::hosted
