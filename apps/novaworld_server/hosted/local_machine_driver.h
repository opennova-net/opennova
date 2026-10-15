#pragma once

// The development and test MachineDriver (ONNET_HOSTED_PROVIDER=local): each
// fleet machine is a stand-in on the service's own machine, a thread of this
// driver that plays the machine's agent (opennova-hostd) against the service's
// internal listener over HTTP, the real protocol through the real routes
// (agent_route.h): it redeems the launch's one-time token at POST
// /hosted/machines/<id>/bootstrap, then syncs at POST
// /hosted/machines/<id>/sync with the secret it got, and runs what each reply
// asks. Its containers are opennova-serve child processes rather than docker
// containers, which a development system or a CI runner need not have:
//
//   <ONNET_HOSTED_SERVE_BIN> --resource-dir <the data set's resource_dir>
//       [--loose-root] <the desired container's arguments>
//
// in the slot's working directory, <work dir>/machine-<id>/slot-<k>, written
// from scratch with the reply's files (each owner-only) before the start; its
// output goes to <work dir>/machine-<id>/<container>.log. The environment is
// kServeEnvironment's variables of the service's and the reply's own, so none
// of the service's secrets reaches it. It follows the agent's rules
// (apps/hostd/README.md): a desired container starts once its slot is free and
// only with its files; one that exited is reported until the reply drops it,
// never restarted; one the reply leaves out gets a stop request and is killed
// stop_grace_ms later, then removed with its slot's directory; a start that
// fails is reported exited with 125 and never tried again. It reports no CPU,
// memory or egress (0), and keeps no state across a restart of the service: a
// terminate, or this driver's end, stops its containers.
//
// Its slots' ports are free ones probed on loopback at each slot_plan (two
// stand-ins share the one system), admin.cfg admits 127.0.0.1, and the service
// reaches every machine at 127.0.0.1, which is also where its agent's requests
// come from (the internal listener bound on loopback admits them).

#include "agent_protocol.h"
#include "data_sets.h"
#include "machine_driver.h"

#include <base/proc/child_process.h>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct LocalMachineConfig {
	std::string serve_binary; // ONNET_HOSTED_SERVE_BIN
	// The data sets it runs, each with its resource_dir (--resource-dir) and
	// loose_root (--loose-root).
	std::vector<DataSet> data_sets;
	std::string work_dir; // ONNET_HOSTED_WORK_DIR: machine-<id>/slot-<k>
	// The internal listener: "http://127.0.0.1:<port>" (set_service_url once
	// it bound).
	std::string service_url;
	std::string agent_version = "local";
	int sync_interval_ms = kSyncIntervalMs;
	int busy_sync_interval_ms = kBusySyncIntervalMs;
	int bootstrap_retry_ms = 1000;
	int stop_grace_ms = 10 * 1000;
	int http_timeout_ms = 10 * 1000;
};

// What one stand-in is doing (tests, and the log's view of it).
struct LocalMachineView {
	bool bootstrapped = false;
	int slots = 0;
	uint64_t syncs = 0; // syncs answered and read
	bool draining = false; // as the last reply said
	struct Container {
		std::string name;
		int slot = 0;
		ContainerState state = ContainerState::Starting;
		std::optional<int64_t> exit_code;
		std::vector<std::string> argv; // what it runs (no secret is in it)
	};
	std::vector<Container> containers;
};

class LocalMachineDriver : public MachineDriver {
public:
	// The variables a container's environment keeps from the service's (a name
	// ending in '*' is a prefix).
	static const std::vector<std::string> kServeEnvironment;
	static constexpr uint64_t kBootTimeoutMs = 60 * 1000;
	// How long terminate() waits for a container it asked to stop.
	static constexpr int kTerminateGraceMs = 5000;

	// The paths are made absolute against the service's working directory.
	explicit LocalMachineDriver(LocalMachineConfig config);
	~LocalMachineDriver() override;
	LocalMachineDriver(const LocalMachineDriver &) = delete;
	LocalMachineDriver &operator=(const LocalMachineDriver &) = delete;

	// The internal listener's URL (main(), once it bound); machines launched
	// after it use it.
	void set_service_url(const std::string &url);

	const char *name() const override { return "local"; }
	uint64_t boot_timeout_ms() const override { return kBootTimeoutMs; }
	std::optional<LaunchedMachine> launch(const MachineLaunch &launch, std::string &error) override;
	CloudMachine describe(const std::string &cloud_id) override;
	void terminate(const std::string &cloud_id) override;
	std::vector<TaggedCloudMachine> list_tagged() override;
	SlotPlan slot_plan(const std::string &cloud_id, int slot) override;

	// What stand-in `cloud_id` is doing; nullopt for one this driver does not run.
	std::optional<LocalMachineView> view(const std::string &cloud_id);

private:
	struct Machine;

	LocalMachineConfig config_;
	std::mutex mu_;
	std::map<std::string, std::unique_ptr<Machine>> machines_;
	uint64_t next_machine_ = 1;
};

} // namespace opennova::novaworld_server::hosted
