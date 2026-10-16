#pragma once

// The local stack's MachineDriver (ONNET_HOSTED_PROVIDER=docker): each fleet
// machine is a container of the serve image on the stack's own network, as each
// cloud machine is an EC2 machine (ec2_machine_driver.h), so the local stack runs
// prod's flow end to end: the launch hands the machine its id and its one-time
// token and nothing else; its agent (opennova-hostd, the image's own) redeems the
// token at the controller's internal listener (ONNET_INTERNAL_URL) and syncs
// from the machine's own address, which the routes check against its row; the
// supervisor places servers on the machine's slots at their fixed ports (slots.h);
// the controller's admin links reach each slot's admin port at the machine's
// address; the servers list through the gate (ONNET_PUBLIC_HOST). What differs is
// the machine: hostd runs each server as an opennova-serve child of its own
// (--runtime process: hostd/process_runtime.h), not a docker container, since the
// machine is itself one.
//
// The driver calls the Docker Engine API over HTTP (ONNET_HOSTED_DOCKER_URL,
// http://<host>:<port>; deploy/compose puts a socket proxy there that admits the
// containers API alone), with no shell and no CLI, its paths unversioned (the
// engine's own API version). A machine is the container
// <deployment>-machine-<id> (ONNET_HOSTED_DEPLOYMENT), labelled opennova.hosted=1,
// opennova.deployment, opennova.machine-id and opennova.machine-kind:
//
//   launch      POST /containers/create of the serve image (ONNET_HOSTED_SERVE_IMAGE)
//               running opennova-hostd (machine_command) on the network
//               ONNET_HOSTED_DOCKER_NETWORK, the data sets mounted read-only from
//               the container ONNET_HOSTED_DOCKER_DATA_FROM (each at
//               /opt/game/<data set>, as a cloud machine mounts its volumes;
//               machine_driver.h machine_data_set_path), docker's init as PID 1,
//               never restarted; then the token as a file (PUT
//               /containers/<name>/archive: the machine's user-data, never an
//               argument or a variable `inspect` shows), then POST .../start, then
//               its address on the network (GET .../json). A name the engine holds
//               already (a launch run again after one cut short) is that launch's
//               machine when its labels name this row: started where it is not yet,
//               its address read. A machine that gets no address is removed (its
//               agent could never be admitted) and the launch fails.
//   describe    GET /containers/<name>/json: created -> Pending; running, paused
//               and restarting -> Running; exited, dead, removing or unknown to
//               the engine (404) -> Gone; an engine that does not answer -> Unknown.
//               Its address on the network is both of the row's addresses.
//   terminate   GET .../json first: a container without both labels (this
//               deployment's) is never removed; then DELETE
//               /containers/<name>?force=1&v=1 (its servers end with it). A
//               machine the engine no longer knows is no error.
//   list_tagged GET /containers/json?all=1 filtered on both labels, in every state.
//               Throws std::runtime_error when the listing fails.
//   profile     kBootTimeoutMs, slot k's fixed ports (each machine has an address
//               of its own) and the controller's network for admin.cfg
//               (Ec2MachineDriver::admin_pattern of its internal listener's bind).
//
// Every failure is logged and handled here, never thrown, but by list_tagged.
// Platform plumbing for the development stack, not a port.

#include "machine_driver.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct DockerMachineConfig {
	std::string engine_url; // ONNET_HOSTED_DOCKER_URL: http://<host>:<port>
	std::string network; // ONNET_HOSTED_DOCKER_NETWORK: the network the machines join
	// ONNET_HOSTED_DOCKER_DATA_FROM: the container whose mounts (each data set at
	// /opt/game/<data set>) every machine takes, read-only.
	std::string data_from;
	std::string deployment; // ONNET_HOSTED_DEPLOYMENT: the label that scopes every call
	// ONNET_HOSTED_SERVE_IMAGE: the machines' image, opennova-hostd and
	// opennova-serve in it (apps/serve/Dockerfile).
	std::string image;
	// ONNET_INTERNAL_URL: the controller's internal listener as the machines
	// reach it (the agent's --service).
	std::string service_url;
	// The controller's internal listener's bind address (ONNET_INTERNAL_HTTP_BIND),
	// which its admin links connect from: the machines' admin.cfg admits its
	// network. The controller's profile alone reads it.
	std::string admin_address;
	// The data sets the deployment offers (the programs' hosting setup knows them).
	std::vector<std::string> data_sets;
	int call_timeout_ms = 20 * 1000;
};

// One answer of the engine.
struct EngineReply {
	bool answered = false; // a status line came back
	int code = 0;
	std::string body;
	std::string error; // why it did not answer
};

class DockerMachineDriver : public MachineDriver {
public:
	static constexpr const char *kLabelHosted = "opennova.hosted";
	static constexpr const char *kLabelDeployment = "opennova.deployment";
	static constexpr const char *kLabelMachineId = "opennova.machine-id";
	static constexpr const char *kLabelMachineKind = "opennova.machine-kind";
	// Where the machine's agent finds its token (written by the launch), keeps
	// its state and its slots: paths the serve image's user (10001) owns.
	static constexpr const char *kTokenDir = "/run/opennova";
	static constexpr const char *kTokenFile = "/run/opennova/hostd.token";
	static constexpr const char *kStateDir = "/srv/serve/hostd";
	static constexpr const char *kSlotsRoot = "/srv/serve/slots";
	static constexpr const char *kHostd = "/usr/local/bin/opennova-hostd";
	static constexpr const char *kServe = "/usr/local/bin/opennova-serve";
	// The serve image's user, which owns the token file.
	static constexpr uint32_t kMachineUid = 10001;
	// From the launch to the agent's first report: a container's start and a
	// bootstrap.
	static constexpr uint64_t kBootTimeoutMs = 2 * 60 * 1000;
	static constexpr size_t kMaxDeploymentBytes = 32;

	// One call of the engine's API: `method` and `path` (with its query) and a
	// body of `content_type` (none for an empty body).
	using EngineCall = std::function<EngineReply(const std::string &method, const std::string &path,
			const std::string &body, const std::string &content_type)>;

	// Over `call`, or, unset, HTTP to config.engine_url.
	explicit DockerMachineDriver(DockerMachineConfig config, EngineCall call = {});

	// "" when `config` can run, else why not (a boot warning; hosting off).
	// Every program asks it, so each decides alike whether hosting is on.
	static std::string check_config(const DockerMachineConfig &config);
	// The supervisor's view of the machines `config` launches: no driver needed.
	static MachineProfile profile(const DockerMachineConfig &config);
	// The container machine `machine_id` is: "<deployment>-machine-<id>".
	static std::string machine_name(const std::string &deployment, int64_t machine_id);
	// The agent's command line in machine `machine_id`.
	static std::vector<std::string> machine_command(
			const DockerMachineConfig &config, int64_t machine_id);
	// The POST /containers/create body for `launch`.
	static std::string create_body(const DockerMachineConfig &config, const MachineLaunch &launch);
	// A tar archive (ustar) of kTokenDir's last part holding the token file, both
	// kMachineUid's alone (0700, 0600): what the launch puts at /run.
	static std::string token_archive(const std::string &token);

	const char *name() const override { return "docker"; }
	std::optional<LaunchedMachine> launch(const MachineLaunch &launch, std::string &error) override;
	CloudMachine describe(const std::string &cloud_id, int64_t launched_ago_ms) override;
	void terminate(const std::string &cloud_id) override;
	std::vector<TaggedCloudMachine> list_tagged() override;

private:
	struct Inspected {
		bool found = false; // false: 404
		std::string status; // State.Status
		std::string address; // its IPv4 on config_.network
		std::string deployment;
		std::string hosted;
		int64_t machine_id = 0;
	};

	// GET /containers/<name>/json: false with `error` when the engine did not answer it.
	bool inspect(const std::string &name, Inspected &out, std::string &error);
	EngineReply call(const std::string &method, const std::string &path,
			const std::string &body = {}, const std::string &content_type = {});

	DockerMachineConfig config_;
	EngineCall call_;
};

} // namespace opennova::novaworld_server::hosted
