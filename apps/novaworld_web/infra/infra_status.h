#pragma once

// The admin's infrastructure view, AWS's side (infra_routes.h serves it beside
// the database's: the lobby instances, the fleet leader, the fleet's machines):
// what the deployment's cloud runs now, read with web's task role through
// aws_api.h and kept for InfraConfig::cache_ms, so however many admins look and
// however often their pages poll, one web process reads AWS at most once in
// that window (a read in flight is waited for, never doubled). What it reads,
// every call scoped to the deployment's own resources (infra/aws/programs.tf
// grants web these reads alone):
//
//   programs   ECS DescribeServices of the five services: their counts,
//              status, deployments (each one's rollout state) and task
//              definition, and their newest events; and SSM GetParameter of
//              the release (the manifest `app deploy` records, DEPLOY.md),
//              whose revisions say whether each service runs the release's
//   tasks      ECS ListTasks (the running ones, and the stopped ones ECS
//              still lists, about an hour) and DescribeTasks: each one's
//              status, health, start, zone, private address and size, and its
//              containers' images, digests, health and exit codes
//   balancer   Elastic Load Balancing DescribeTargetHealth of each target group
//   database   RDS DescribeDBInstances: status, class, Multi-AZ and zones
//   cache      ElastiCache DescribeReplicationGroups: status, Multi-AZ,
//              failover, its nodes' roles and zones
//   machines   EC2 DescribeInstances of the fleet's instance ids (the rows'
//              cloud ids): each one's zone and state as EC2 has it
//
// The calls of one read run at once, each on a thread of its own; one that
// fails leaves its section's error and the others stand. Without AWS (the local
// stack: no ONNET_INFRA_CLUSTER, or no task role) nothing is called and the
// snapshot says why: the view never shows made-up data. Service policy, not a
// port.

#include "aws_api.h"
#include "parts.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::infra {

struct InfraConfig {
	AwsApiConfig aws;
	std::string cluster; // ONNET_INFRA_CLUSTER: the deployment's ECS cluster
	// The services, the programs' names.
	std::vector<std::string> programs = { "gate", "lobby", "web", "controller", "worker" };
	// ONNET_INFRA_TARGET_GROUPS, {"<listener>": "<target group ARN>"}: the
	// balancer's target groups (gate, lobby, http, https), shown in that order.
	std::vector<std::pair<std::string, std::string>> target_groups;
	std::string database; // ONNET_INFRA_DATABASE: the RDS instance's identifier
	std::string cache; // ONNET_INFRA_CACHE: the ElastiCache replication group's id
	// ONNET_INFRA_RELEASE_PARAMETER: the SSM parameter holding the release
	// (/opennova/<deployment>/release).
	std::string release_parameter;
	int64_t cache_ms = 20 * 1000;
};

// The settings from the environment (the ONNET_INFRA_* variables,
// infra/aws/programs.tf; the region, and AWS_CONTAINER_CREDENTIALS_RELATIVE_URI,
// the ECS agent's), the curl calls' files under `scratch_dir`.
InfraConfig infra_config_from_env(const std::string &scratch_dir);

// "" when `config` reads the cloud, else why not, for the page: no cluster (the
// local stack has none), and then aws_api.h's checks.
std::string check_infra_config(const InfraConfig &config);

struct EcsContainer {
	std::string name;
	std::string image; // as the task definition names it (the release pins a digest)
	std::string image_digest; // the digest ECS pulled ("sha256:...")
	std::string last_status;
	std::string health; // HEALTHY, UNHEALTHY or UNKNOWN
	std::optional<int64_t> exit_code;
	std::string reason;
};

struct EcsTask {
	std::string id; // its ARN's last segment
	std::string group; // "service:<name>" or "family:<task definition family>"
	std::string program; // the service it runs for; "" for a one-off task
	std::string task_definition_arn;
	std::string last_status;
	std::string desired_status;
	std::string health;
	// ISO 8601 UTC; "" when ECS gave none.
	std::string created_at;
	std::string started_at;
	std::string stopping_at;
	std::string stopped_at;
	std::string stopped_reason;
	std::string stop_code;
	std::string zone;
	std::string private_ip;
	std::string cpu; // CPU units ("512": half a vCPU)
	std::string memory; // MiB
	std::vector<EcsContainer> containers;
};

struct EcsDeployment {
	std::string id;
	std::string status; // PRIMARY, ACTIVE or INACTIVE
	std::string rollout_state; // COMPLETED, IN_PROGRESS or FAILED
	std::string rollout_reason;
	std::string task_definition_arn;
	int64_t desired = 0;
	int64_t running = 0;
	int64_t pending = 0;
	int64_t failed = 0;
	std::string created_at;
	std::string updated_at;
};

struct EcsEvent {
	std::string at;
	std::string message;
};

struct EcsService {
	std::string name;
	std::string status; // ACTIVE, DRAINING or INACTIVE
	int64_t desired = 0;
	int64_t running = 0;
	int64_t pending = 0;
	std::string task_definition_arn;
	std::vector<EcsDeployment> deployments;
	std::vector<EcsEvent> events; // the newest kEventsKept, newest first
};

struct Target {
	std::string id; // the task's address
	int64_t port = 0;
	std::string zone;
	std::string state; // initial, healthy, unhealthy, draining, unused, unavailable
	std::string reason;
	std::string description;
};

struct TargetGroupHealth {
	std::string key; // gate, lobby, http, https
	std::string arn;
	std::vector<Target> targets;
	std::string error;
};

struct DatabaseStatus {
	std::string identifier;
	std::string status; // available, modifying, backing-up, rebooting, ...
	std::string engine;
	std::string engine_version;
	std::string instance_class;
	bool multi_az = false;
	std::string zone;
	std::string secondary_zone;
	std::string storage_type;
	int64_t storage_gb = 0;
};

struct CacheNode {
	std::string id;
	std::string role; // primary or replica
	std::string zone;
};

struct CacheStatus {
	std::string id;
	std::string status; // available, modifying, snapshotting, ...
	std::string multi_az; // enabled or disabled
	std::string automatic_failover; // enabled, disabled, enabling, disabling
	std::string node_type;
	std::string engine;
	std::vector<CacheNode> nodes;
};

struct Release {
	std::string id;
	std::string created;
	std::string revision;
	std::map<std::string, std::string> task_definitions; // program (and backup) -> revision ARN
};

struct MachineInstance {
	std::string id;
	std::string state; // pending, running, shutting-down, terminated, stopping, stopped
	std::string zone;
	std::string type;
};

struct CloudSnapshot {
	// Why the cloud is not read here ("" when it is): the local stack's view.
	std::string unavailable;
	// Why the read failed as a whole (the task role's credentials).
	std::string error;
	int64_t read_ms = 0; // Unix epoch ms the read began
	std::string cluster;
	std::string region;

	std::vector<EcsService> services; // in the programs' order
	std::vector<std::string> missing_services; // the ones ECS reported missing
	std::string services_error;
	// Every task ECS listed: running first by start, then stopped, newest first.
	std::vector<EcsTask> tasks;
	std::string tasks_error;
	std::vector<TargetGroupHealth> target_groups;
	std::optional<DatabaseStatus> database;
	std::string database_error;
	std::optional<CacheStatus> cache;
	std::string cache_error;
	std::optional<Release> release; // none recorded yet when release_none
	bool release_none = false;
	std::string release_error;
	std::map<std::string, MachineInstance> machines; // by instance id
	std::string machines_error;
};

class InfraStatus {
public:
	// The newest events each service keeps.
	static constexpr size_t kEventsKept = 10;
	// The most tasks one read describes (DescribeTasks' limit): the running
	// ones first.
	static constexpr size_t kTasksDescribed = 100;
	// The most instance ids one read asks EC2 about.
	static constexpr size_t kMachinesAsked = 100;

	// Milliseconds on a steady clock, for the cache's age.
	using Clock = std::function<int64_t()>;

	// `runner` must outlive the status.
	InfraStatus(InfraConfig config, proc::CommandRunner &runner, Clock now_ms = {});
	InfraStatus(const InfraStatus &) = delete;
	InfraStatus &operator=(const InfraStatus &) = delete;

	// Whether the cloud is read here (check_infra_config passed).
	bool configured() const { return unavailable_.empty(); }

	// The cloud as last read, read again first when that read is older than
	// cache_ms (and waiting for a read another caller has in flight).
	// `instance_ids` are the fleet's machines EC2 is asked about (the ones
	// starting "i-"). Never throws but std::bad_alloc.
	std::shared_ptr<const CloudSnapshot> snapshot(const std::vector<std::string> &instance_ids);

private:
	CloudSnapshot read(const std::vector<std::string> &instance_ids);

	const InfraConfig config_;
	const std::string unavailable_;
	AwsApi api_;
	Clock now_;
	std::mutex mu_;
	std::condition_variable read_done_;
	std::shared_ptr<const CloudSnapshot> current_;
	int64_t current_at_ = 0;
	bool reading_ = false;
};

// The status a web process runs: its scratch directory and curl runner, and the
// status over them (removed in that order's reverse).
struct InfraStack {
	ScratchDir scratch;
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<InfraStatus> status;
};

// The status over the environment's settings: configured or not (the local
// stack's says why), logged at boot either way. A scratch directory that cannot
// be made leaves it unconfigured.
InfraStack make_infra();

} // namespace opennova::novaworld_server::infra
