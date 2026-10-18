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
// agent redeems it; the token is spent at the agent's first sync, so a redeem
// whose reply was lost is made again (a fresh secret replacing the last):
//
//   POST /hosted/machines/<id>/bootstrap      Authorization: Bearer <token>
//   {"protocol": 3, "agent_version": "<the agent's build>"}
//   -> 200 {"protocol": 3, "agent_secret": "<kAgentSecretChars letters and digits>",
//           "serve_image": "ghcr.io/<owner>/opennova-serve@sha256:<digest>",
//           "slots": <the machine's slots, 1..kMaxSlots>}
//
// The secret is the agent's long-lived credential (the service keeps only its
// SHA-256, agent_secret_digest). The agent keeps it in a file of its own
// format (apps/hostd), never this reply's, so a later protocol reads it too.
//
// The sync. Every kSyncIntervalMs (kBusySyncIntervalMs while the agent has work:
// a container to start, stop or remove, a tree to fetch) the agent reports every
// container it runs, and gets back the containers it should run and the trees
// they need:
//
//   POST /hosted/machines/<id>/sync           Authorization: Bearer <agent secret>
//   {"protocol": 3, "agent_version": "...", "agent_image": "<its image>" | "",
//    "image_id": "<the machine's image (an AMI id)>" | "",
//    "log_stream": "<the agent's own log stream>" | "",
//    ["machine": {"cpu_percent": <number>, "memory_used_bytes": <int>,
//                 "memory_total_bytes": <int>, "disk_free_bytes": <int>,
//                 "disk_total_bytes": <int>, "net_in_bytes": <int>, "net_out_bytes": <int>},]
//    "trees": {"<tree id>": {"state": "fetching" | "ready" | "failed", "bytes": <int>,
//                            "tries": <int>, ["reason": "<the last failure>"]}},
//    "containers": [{"name", "slot", "state": "starting" | "running" | "exited",
//                    "waiting": "" | "image" | "trees",
//                    "exit_code": <int> | null, "oom": <bool>,
//                    "started_at": <Unix seconds> | null, ["reason": "<why>",]
//                    "cpu_percent": <number>, "memory_bytes": <int>, "egress_bytes": <int>,
//                    "net_in_bytes": <int>,
//                    ["tick": {"p50_ms": <number>, "p99_ms": <number>, "max_ms": <number>,
//                              "late": <int>}]}],
//    ["listing_failed": true]}
//   -> 200 {"protocol": 3, "drain": <bool>,
//           "desired": [{"name", "slot", "image", "uid",
//                        "base_tree": <tree id>, "expansion_tree": <tree id> | null,
//                        "expansion_code": "<code>" | "", "args": [...],
//                        "files": {"<name>": "<base64>"}, "env": {"<NAME>": "<value>"},
//                        "cpus": <number>, "memory_mb": <int>, "pids": <int>,
//                        "log": {"stream": "<server id>/g<generation>"}}],
//           "trees": {"<tree id>": {"url": "s3://<bucket>/<root>/trees/<id>/" | "file:///...",
//                                   "version": "<its version.txt's line>",
//                                   "files": [{"name", "size"}]}}}
//
// A tree (ADR 0064) is one immutable version of a data set's files in the
// object store, which the agent fetches from its `url` into <trees root>/<id>
// (its own `aws s3 sync`, or a directory copy for file://) and checks against
// the manifest (`files`: every name with its size), then binds into a
// container: the base tree at /opt/game, the expansion tree (when the container
// has one) at /opt/game/expansion/<expansion_code>. The reply's `trees` map names
// every tree its desired containers use (and only those), so the agent fetches
// what it lacks before starting a container (reported `starting` meanwhile) and
// may drop a tree no reply has named for a while.
//
// What the agent says of the machine itself: the machine's image (`image_id`,
// read once from the instance's metadata; "" off EC2), its own log stream
// (`log_stream`, its ECS task's; "" off ECS), its use (`machine`, present once
// the agent has read it twice: the CPU busy between two readings, 0..100 of
// the whole machine, the memory in use of the machine's, the trees'
// filesystem's room, every interface's bytes but loopback's since the machine
// booted) and every tree it holds (`trees`: fetching, a failure with a try
// left among them, ready, or failed for good; its bytes on disk, the fetches
// made and the last failure's reason).
//
// `egress_bytes` is the container's slot's, every byte its user sent since the
// machine booted (the agent's per-UID counters), so the service takes the
// difference itself; a count below the last one is a counter that started over
// (its rule put back), counted from 0. `net_in_bytes` is the same for what
// reached the slot's ports. `exit_code` is the container's own once it exited,
// or the docker CLI's (125) when its start failed (its image did not pull among
// the causes); such a container is reported as exited until the service drops
// it. An exited container may carry `reason`, why: the line opennova-serve
// wrote at its non-zero exit (its --exit-file), or why one never started (its
// data could not be fetched: the owner reads it in the server's events), at
// most kMaxContainerReasonBytes of printable ASCII, present only with an exit
// code. One waiting for its image's pull or its trees' fetch is reported
// starting, `waiting` saying which ("" for none). A running container carries
// its server's last minute of ticks (`tick`, its --stats-file: the frame
// time's p50, p99 and longest, and the late frames) while that file is fresh.
// Each desired container names its log stream (`log`), where its output goes
// (docker's awslogs driver, or the agent's --log-dir). `files` carries the slot's working directory
// (the bytes in standard base64: the host file is cp1252) and is sent until
// the agent reports the container running or exited (a container it reports
// starting waits for its image or its trees, and an agent replaced meanwhile
// needs them again); the agent keeps them until it starts the container,
// writes them before, and never needs them again. A desired container the
// agent does not report is not on the machine:
// the agent cannot start one whose files were left out (a lost agent state),
// and keeps none whose start failed past the report. The agent stops a
// container only when a reply leaves it out; a sync that fails changes nothing
// on the machine. `drain` says the service is moving the machine's servers off
// it.
//
// An agent that cannot list its containers (docker not answering) still
// syncs, with `listing_failed` (present only then) and no container: the
// service hears from the agent (the machine is not lost), keeps the slots' last
// reports, moves no booting machine to ready, and the agent acts on no reply
// until it lists again.
//
// The envelope. Every sync request opens with the same three members,
// whatever its protocol: `protocol`, `agent_version` and `agent_image` (the
// image the agent's ECS task runs, as its task definition pins it; "" off
// ECS). The service reads them first (decode_agent_envelope) and records them
// at every sync, matched or not. Every refusal of either route is a bare 404.
// A message whose `protocol` is not kAgentProtocol is not read further: the
// service holds a machine whose agent speaks another protocol, in either
// direction (heard, so it is not lost; its slots' last reports kept; nothing
// answered, so its agent acts on nothing and its servers keep running), and
// ends it only once that has lasted kProtocolMismatchEndS: terminated, its
// servers placed again elsewhere (a drain would run each server it moves twice,
// since the held agent stops none); the agent acts
// on no reply in another protocol, keeping what it runs. A hostd rolled ahead
// of the fleet program (or the fleet program ahead of it) so costs nothing
// while both roll within that window.

#include "slots.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr int kAgentProtocol = 3;
// How long a machine whose agent speaks another protocol is held before it
// ends (the envelope, above).
inline constexpr int64_t kProtocolMismatchEndS = 10 * 60;
inline constexpr int kSyncIntervalMs = 5000;
inline constexpr int kBusySyncIntervalMs = 1000;
// The bootstrap token and the agent's secret: letters and digits from the OS
// CSPRNG (~285 bits each).
inline constexpr size_t kMachineTokenChars = 48;
inline constexpr size_t kAgentSecretChars = 48;
// A desired container's caps: files, their bytes all told, arguments, the
// environment; and the longest reason a report carries.
inline constexpr size_t kMaxContainerFiles = 16;
inline constexpr size_t kMaxContainerFileBytes = 1u << 20;
inline constexpr size_t kMaxContainerArgs = 64;
inline constexpr size_t kMaxContainerEnv = 32;
inline constexpr size_t kMaxContainerReasonBytes = 200;
// The trees: how many a reply may name, how many files one holds, the longest
// fetch URL.
inline constexpr size_t kMaxReplyTrees = 32;
inline constexpr size_t kMaxTreeFiles = 64;
inline constexpr size_t kMaxTreeUrlBytes = 512;
inline constexpr size_t kMaxExpansionCodeChars = 8;
// The trees a report may name (the store keeps an unnamed tree a while).
inline constexpr size_t kMaxReportedTrees = 2 * kMaxReplyTrees;

// "/hosted/machines/<id>/bootstrap" and "/hosted/machines/<id>/sync".
std::string machine_bootstrap_path(int64_t machine_id);
std::string machine_sync_path(int64_t machine_id);

// "srv-<server id>-g<generation>": a hosted server's container, one name per
// start (a restart is a new generation), so a slot's next container never
// shares the name of the one before.
std::string hosted_container_name(int64_t server_id, int64_t generation);
// "<server id>/g<generation>": the log stream a hosted server's start writes
// (the servers' log group's, or the file under the agent's --log-dir), one per
// start as its container's name is.
std::string hosted_log_stream(int64_t server_id, int64_t generation);

// The stored form of an agent secret or a machine token: its SHA-256 in hex.
std::string agent_secret_digest(std::string_view secret);

// The shapes every field is checked against, the agent's and the service's
// alike: a container name (1..64 of letters, digits, '_', '.', '-', a letter
// or digit first); an image reference (letters, digits and ./:@_-, at most 255,
// a letter or digit first); a working-directory file name and a tree's file
// name (1..64 of letters, digits, '.', '_', '-', never "." or ".."); an
// expansion code (1..kMaxExpansionCodeChars of a-z, 0-9); a tree's fetch URL
// (s3://... or file:///..., printable ASCII, at most kMaxTreeUrlBytes); an
// environment name (a letter or '_' first, then letters, digits and '_', at
// most 128); a log stream, the agent's own or a container's (1..512 of
// letters, digits and ._-, in parts separated by '/', none empty, "." or "..":
// a container's names a file under the agent's --log-dir); a machine's image
// id (1..64 of letters, digits and '-'). And the servers' log group and its
// region, which the agent's command line names (not the protocol): a
// CloudWatch log group (1..512 of letters, digits and ./_-) and an AWS region
// (1..32 of a-z, 0-9 and '-', a letter first).
bool is_container_name(std::string_view name);
bool is_image_reference(std::string_view image);
bool is_slot_file_name(std::string_view name);
bool is_tree_file_name(std::string_view name);
bool is_expansion_code(std::string_view code);
bool is_tree_url(std::string_view url);
bool is_env_name(std::string_view name);
bool is_log_stream_name(std::string_view name);
bool is_image_id(std::string_view id);
bool is_log_group_name(std::string_view name);
bool is_aws_region(std::string_view region);

// The envelope a sync request opens with, in any protocol: read before the
// protocol is checked, and every other member ignored.
struct AgentEnvelope {
	int protocol = 0;
	std::string agent_version; // 1..64 printable ASCII
	std::string agent_image; // "" or an image reference (is_image_reference)
};
// The envelope of `json`: false (with `error`) when it is no object, its
// `protocol` no whole number, its `agent_version` not 1..64 printable
// characters, or a present `agent_image` neither "" nor an image reference.
bool decode_agent_envelope(std::string_view json, AgentEnvelope &out, std::string &error);

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
};

enum class ContainerState : uint8_t {
	Starting, // created, not running yet
	Running,
	Exited, // ended (exit_code says how), or its start failed (125)
};
const char *container_state_text(ContainerState state); // "starting", "running", "exited"
std::optional<ContainerState> parse_container_state(std::string_view text);

// What a starting container waits for.
enum class ContainerWait : uint8_t {
	None,
	Image, // its image's pull
	Trees, // its trees' fetch
};
const char *container_wait_text(ContainerWait wait); // "", "image", "trees"
std::optional<ContainerWait> parse_container_wait(std::string_view text);

// A server's last minute of ticks (opennova-serve's --stats-file).
struct TickHealth {
	double p50_ms = 0.0;
	double p99_ms = 0.0;
	double max_ms = 0.0;
	uint64_t late = 0; // frames that started a whole quantum behind
};

struct ContainerReport {
	std::string name;
	int slot = 0;
	ContainerState state = ContainerState::Starting;
	ContainerWait waiting = ContainerWait::None; // starting alone
	std::optional<int64_t> exit_code; // set once it exited
	bool oom = false; // the kernel's OOM killer ended it
	std::optional<int64_t> started_at; // Unix seconds, once it started
	// Why an exited container ended, or never started ("" for none): sent only
	// with an exit code, 1..kMaxContainerReasonBytes printable.
	std::string reason;
	double cpu_percent = 0.0; // of one CPU (above 100 on more than one)
	uint64_t memory_bytes = 0;
	uint64_t egress_bytes = 0; // its slot's, since the machine booted
	uint64_t net_in_bytes = 0; // its slot's ports', since the machine booted
	std::optional<TickHealth> tick; // running, with a fresh --stats-file
};

// The machine's own use, as its agent read it (proc_stats.h).
struct MachineStats {
	double cpu_percent = 0.0; // of the whole machine, 0..100
	uint64_t memory_used_bytes = 0;
	uint64_t memory_total_bytes = 0;
	uint64_t disk_free_bytes = 0; // the trees' filesystem
	uint64_t disk_total_bytes = 0;
	uint64_t net_in_bytes = 0; // since the machine booted, every interface but loopback
	uint64_t net_out_bytes = 0;
};

// Where a tree the agent holds stands.
enum class TreeReportState : uint8_t {
	Fetching, // in flight, waiting its turn, or failed with a try left
	Ready,
	Failed, // its tries spent
};
const char *tree_report_state_text(TreeReportState state); // "fetching", "ready", "failed"
std::optional<TreeReportState> parse_tree_report_state(std::string_view text);

struct TreeReport {
	TreeReportState state = TreeReportState::Fetching;
	uint64_t bytes = 0; // on disk: the tree's, or the fetch's so far
	int tries = 0; // the fetches made
	std::string reason; // the last failure's ("" for none), 1..kMaxContainerReasonBytes printable
};

// The sync's request.
struct SyncRequest {
	int protocol = kAgentProtocol;
	std::string agent_version;
	std::string agent_image; // "" or the image the agent's ECS task runs
	std::string image_id; // "" off EC2
	std::string log_stream; // "" off ECS
	std::optional<MachineStats> machine;
	std::map<int64_t, TreeReport> trees; // at most kMaxReportedTrees
	std::vector<ContainerReport> containers;
	// The agent could not list its containers: `containers` is empty and says
	// nothing of the machine.
	bool listing_failed = false;
};

// One file of a tree, as its manifest lists it.
struct TreeFileSpec {
	std::string name;
	uint64_t size = 0;
};

// A tree a desired container uses: where to fetch it and what it holds.
struct TreeSpec {
	std::string url;
	std::string version;
	std::vector<TreeFileSpec> files;
};

struct DesiredContainer {
	std::string name;
	int slot = 0;
	std::string image;
	uint32_t uid = 0; // slot_uid(slot), always
	int64_t base_tree = 0; // a key of the reply's trees
	std::optional<int64_t> expansion_tree; // a key of the reply's trees, with the code
	std::string expansion_code; // "" with no expansion tree
	std::vector<std::string> args; // opennova-serve's arguments
	// The working directory's files, name -> bytes; absent once the agent has
	// reported the container.
	std::optional<std::map<std::string, std::string>> files;
	std::map<std::string, std::string> env;
	double cpus = 1.0; // docker run --cpus
	int memory_mb = 512; // --memory and --memory-swap
	int pids = 128; // --pids-limit
	std::string log_stream; // where its output goes: "<server id>/g<generation>"
};

// The sync's reply.
struct SyncReply {
	int protocol = kAgentProtocol;
	std::vector<DesiredContainer> desired; // one per slot, one per name
	std::map<int64_t, TreeSpec> trees; // every tree the desired containers use
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
