#pragma once

// Where a hosted server runs: the supervisor's one seam to machines (ADR 0053,
// proposed: "provisioning in the service behind MachineProvider"). A machine
// runs one opennova-serve for one hosted server; it is a machine, never a host
// (CONTEXT.md). The supervisor renders everything the server needs into a
// provider-neutral LaunchSpec; a provider adds only what is its own (the
// opennova-serve binary, where the game data is, how the files reach the
// machine) and reports what it runs.
//
// Providers: LocalProcessProvider (local_process_provider.h, a child process on
// the service's own machine, for development and the tests), and the cloud one
// that comes with the infrastructure (an Ec2Provider fetching the LaunchSpec as
// its bootstrap bundle). The state-machine tests drive a fake
// (tests/novaworld/hosted_fake_provider.h).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

// One file of the server's working directory: a bare name and its bytes.
// `secret` marks one holding a credential (owner-only on the machine, never
// logged).
struct LaunchFile {
	std::string name;
	std::string content;
	bool secret = false;
};

// Everything a machine needs to run hosted server `server_id`, rendered from
// scratch at every start: the working directory's files and opennova-serve's
// arguments, relative to that directory. The provider puts the binary and its
// --resource-dir (and --loose-root) in front.
struct LaunchSpec {
	int64_t server_id = 0;
	std::vector<LaunchFile> files;
	std::vector<std::string> serve_args;
};

// What a provider decides for a server before its files are rendered: the
// NovaWorld UDP port game.cfg pins the server to, the remote-admin TCP port, and
// the address admin.cfg's ip_restrict admits (the service, as the machine sees
// it).
struct MachinePlan {
	uint16_t nw_port = 0;
	uint16_t admin_port = 0;
	std::string admin_allowed_ip;
};

enum class MachineState : uint8_t {
	Pending, // launched, not running yet (a cloud machine booting)
	Running, // opennova-serve runs
	Exited,  // opennova-serve exited (exit_code says how); the machine still exists
	Gone,    // no such machine (terminated, or never known)
};

struct MachineStatus {
	MachineState state = MachineState::Gone;
	std::optional<uint32_t> exit_code; // Exited: opennova-serve's code (0: it stopped cleanly)
	std::string address;          // where the service reaches the machine (the admin link)
};

// A machine the provider runs for a hosted server, with the server id its tag
// names.
struct TaggedMachine {
	std::string machine_id;
	int64_t server_id = 0;
};

class MachineProvider {
public:
	virtual ~MachineProvider() = default;

	// The provider's name, stored with the machine (hosted_servers.provider).
	virtual const char *name() const = 0;
	// The ports and the admin address for server `server_id`'s next start.
	virtual MachinePlan plan(int64_t server_id) = 0;
	// Starts a machine running `spec`: its id, or nullopt with `error` (nothing
	// runs then). The machine carries the server id as its tag.
	virtual std::optional<std::string> launch(const LaunchSpec &spec, std::string &error) = 0;
	virtual MachineStatus describe(const std::string &machine_id) = 0;
	// Ends the machine and whatever runs on it; idempotent, and a machine that is
	// gone already is no error.
	virtual void terminate(const std::string &machine_id) = 0;
	// Every machine the provider runs for hosted servers (reconciliation at the
	// service's boot).
	virtual std::vector<TaggedMachine> list_tagged() = 0;
};

} // namespace opennova::novaworld_server::hosted
