#pragma once

// What a machine's agent (opennova-hostd, apps/hostd) and the NovaWorld service
// say to each other: JSON through engine/base/io/json.h, Crow-free, so the
// service and the agent build the one encoder and decoder each message has.
// Service policy, not a port. Both routes are on the service's internal
// listener (its private address, open to the machines' security group alone),
// never on the public port.
//
// The machine's bootstrap. The machine's user-data carries a one-time token
// the service minted for that machine (the service keeps its SHA-256). The
// agent redeems it once:
//
//   POST /hosted/machines/<id>/bootstrap      Authorization: Bearer <token>
//   {"protocol": 1, "agent_version": "<the agent's build>"}
//   -> 200 {"protocol": 1, "agent_secret": "<kAgentSecretChars letters and digits>",
//           "serve_image": "ghcr.io/<owner>/opennova-serve@sha256:<digest>",
//           "slots": <the machine's slots, 1..kMaxSlots>,
//           "data_sets": {"<data set>": "/opt/game/<data set>", ...}}
//
// The secret is the agent's long-lived credential (the service keeps only its
// SHA-256, agent_secret_digest); the data sets map each data set the machine
// mounts to its mount path, the bind source of that data set's containers.
//
// The sync. Every kSyncIntervalMs (kBusySyncIntervalMs while the agent has work:
// a container to start, stop or remove) the agent reports every container it
// runs and gets back the ones it should:
//
//   POST /hosted/machines/<id>/sync           Authorization: Bearer <agent secret>
//   {"protocol": 1, "agent_version": "...",
//    "containers": [{"name", "slot", "state": "starting" | "running" | "exited",
//                    "exit_code": <int> | null, "oom": <bool>,
//                    "started_at": <Unix seconds> | null, "cpu_percent": <number>,
//                    "memory_bytes": <int>, "egress_bytes": <int>}],
//    ["listing_failed": true]}
//   -> 200 {"protocol": 1, "drain": <bool>,
//           "desired": [{"name", "slot", "image", "uid", "data_set", "args": [...],
//                        "files": {"<name>": "<base64>"}, "env": {"<NAME>": "<value>"},
//                        "cpus": <number>, "memory_mb": <int>, "pids": <int>}]}
//
// `egress_bytes` is the container's slot's, every byte its user sent since the
// machine booted (the agent's per-UID counters), so the service takes the
// difference itself; a count below the last one is a counter that started over
// (its rule put back), counted from 0. `exit_code` is the container's own once
// it exited, or the docker CLI's (125) when its start failed (its image did not
// pull among the causes); such a container is reported as exited until the
// service drops it. One waiting for its image's pull is reported starting.
// `files` carries the slot's working directory (the bytes in standard base64:
// the host file is cp1252) and is sent until the agent has reported the
// container once, in any state; the agent keeps them until it starts the
// container, writes them before, and never needs them again.
// A desired container the agent does not report is not on the machine: the
// agent cannot start one whose files were left out (a lost agent state), and
// keeps none whose start failed past the report. The agent stops a container
// only when a reply leaves it out; a sync that fails changes nothing on the
// machine. `drain` says the service is moving the machine's servers off it.
//
// An agent that cannot list its containers (docker not answering) still
// syncs, with `listing_failed` (present only then) and no container: the
// service hears from the agent (the machine is not lost), keeps the slots' last
// reports, moves no booting machine to ready, and the agent acts on no reply
// until it lists again.
//
// Every refusal of either route is a bare 404. A message whose `protocol` is
// not kAgentProtocol is not read further (protocol_of): the service drains a
// machine whose agent speaks another protocol, and the agent acts on no reply
// in one, keeping what it runs.

#include "slots.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr int kAgentProtocol = 1;
inline constexpr int kSyncIntervalMs = 5000;
inline constexpr int kBusySyncIntervalMs = 1000;
// The bootstrap token and the agent's secret: letters and digits from the OS
// CSPRNG (~285 bits each).
inline constexpr size_t kMachineTokenChars = 48;
inline constexpr size_t kAgentSecretChars = 48;
// A desired container's caps: files, their bytes all told, arguments, the
// environment.
inline constexpr size_t kMaxContainerFiles = 16;
inline constexpr size_t kMaxContainerFileBytes = 1u << 20;
inline constexpr size_t kMaxContainerArgs = 64;
inline constexpr size_t kMaxContainerEnv = 32;

// "/hosted/machines/<id>/bootstrap" and "/hosted/machines/<id>/sync".
std::string machine_bootstrap_path(int64_t machine_id);
std::string machine_sync_path(int64_t machine_id);

// "srv-<server id>-g<generation>": a hosted server's container, one name per
// start (a restart is a new generation), so a slot's next container never
// shares the name of the one before.
std::string hosted_container_name(int64_t server_id, int64_t generation);

// The stored form of an agent secret or a machine token: its SHA-256 in hex.
std::string agent_secret_digest(std::string_view secret);

// The shapes every field is checked against, the agent's and the service's
// alike: a container name (1..64 of letters, digits, '_', '.', '-', a letter
// or digit first); an image reference (letters, digits and ./:@_-, at most 255,
// a letter or digit first); a data set (1..64 of a-z, 0-9, '_'); a mount path
// (absolute, at most 255 of letters, digits and ./_-, no "." or ".." part); a
// working-directory file name (1..64 of letters, digits, '.', '_', '-', never
// "." or ".."); an environment name (a letter or '_' first, then letters,
// digits and '_', at most 128).
bool is_container_name(std::string_view name);
bool is_image_reference(std::string_view image);
bool is_data_set_name(std::string_view name);
bool is_mount_path(std::string_view path);
bool is_slot_file_name(std::string_view name);
bool is_env_name(std::string_view name);

// The `protocol` member of any of these messages, or nullopt when `json` is no
// object with a whole-number `protocol`.
std::optional<int> protocol_of(std::string_view json);

// The bootstrap's request.
struct AgentHello {
	int protocol = kAgentProtocol;
	std::string agent_version; // 1..64 printable ASCII
};

// The bootstrap's reply.
struct MachineBootstrap {
	int protocol = kAgentProtocol;
	std::string agent_secret;
	std::string serve_image;
	int slots = 0;
	std::map<std::string, std::string> data_sets; // data set -> mount path
};

enum class ContainerState : uint8_t {
	Starting, // created, not running yet
	Running,
	Exited, // ended (exit_code says how), or its start failed (125)
};
const char *container_state_text(ContainerState state); // "starting", "running", "exited"
std::optional<ContainerState> parse_container_state(std::string_view text);

struct ContainerReport {
	std::string name;
	int slot = 0;
	ContainerState state = ContainerState::Starting;
	std::optional<int64_t> exit_code; // set once it exited
	bool oom = false; // the kernel's OOM killer ended it
	std::optional<int64_t> started_at; // Unix seconds, once it started
	double cpu_percent = 0.0; // of one CPU (above 100 on more than one)
	uint64_t memory_bytes = 0;
	uint64_t egress_bytes = 0; // its slot's, since the machine booted
};

// The sync's request.
struct SyncRequest {
	int protocol = kAgentProtocol;
	std::string agent_version;
	std::vector<ContainerReport> containers;
	// The agent could not list its containers: `containers` is empty and says
	// nothing of the machine.
	bool listing_failed = false;
};

struct DesiredContainer {
	std::string name;
	int slot = 0;
	std::string image;
	uint32_t uid = 0; // slot_uid(slot), always
	std::string data_set;
	std::vector<std::string> args; // opennova-serve's arguments
	// The working directory's files, name -> bytes; absent once the agent has
	// reported the container.
	std::optional<std::map<std::string, std::string>> files;
	std::map<std::string, std::string> env;
	double cpus = 1.0; // docker run --cpus
	int memory_mb = 512; // --memory and --memory-swap
	int pids = 128; // --pids-limit
};

// The sync's reply.
struct SyncReply {
	int protocol = kAgentProtocol;
	std::vector<DesiredContainer> desired; // one per slot, one per name
	bool drain = false;
};

// Each message's text, and its strict read: a decoder refuses (false, `error`
// saying why and never quoting a secret) a message of another protocol, a
// member it does not know, a missing or mistyped member, and any field outside
// its shape, so nothing unchecked reaches a docker argument or a file.
std::string encode_agent_hello(const AgentHello &hello);
bool decode_agent_hello(std::string_view json, AgentHello &out, std::string &error);
std::string encode_machine_bootstrap(const MachineBootstrap &bootstrap);
bool decode_machine_bootstrap(std::string_view json, MachineBootstrap &out, std::string &error);
std::string encode_sync_request(const SyncRequest &request);
bool decode_sync_request(std::string_view json, SyncRequest &out, std::string &error);
std::string encode_sync_reply(const SyncReply &reply);
bool decode_sync_reply(std::string_view json, SyncReply &out, std::string &error);

} // namespace opennova::novaworld_server::hosted
