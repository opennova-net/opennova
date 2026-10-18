#pragma once

// AWS as the admin's infrastructure view reads it (apps/novaworld_web/infra/),
// behind its curl runs: a proc::CommandRunner that reads each run's curl config
// file, names the call it asks for ("ecs DescribeServices", "ecs ListTasks
// RUNNING", "elasticloadbalancing DescribeTargetHealth opennova-prod-https",
// "credentials" for the ECS agent's endpoint, ...), records it and answers the
// reply set for that name, as curl would print it (the body, then the status on
// a line of its own). Thread-safe: a read makes its calls at once. A deployment
// in the middle of a web deploy, the replies of a real one's shapes (the ECS and
// SSM JSON APIs, the Query APIs' XML): deployment_replies(). Shared by
// infra_status_test.cpp and infra_routes_test.cpp.

#include "command_runner.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace infra_stub {

using opennova::proc::CommandResult;

// The task role's credentials the agent hands out; no reply or error may carry
// any of them.
constexpr const char *kKeyId = "ASIAINFRASTUBKEY0001";
constexpr const char *kSecret = "infra-stub-secret/AbCdEf+0123456789";
constexpr const char *kToken = "infra-stub-session-token-QWERTY";

constexpr const char *kTargetGroupArn =
		"arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/";

struct Call {
	std::vector<std::string> argv;
	int timeout_ms = 0;
	std::string config; // the curl config file's text while it ran
	std::string key;
};

inline CommandResult answer(int status, const std::string &body) {
	CommandResult r;
	r.outcome = CommandResult::Outcome::Exited;
	r.exit_code = 0;
	r.out = body + "\n" + std::to_string(status);
	return r;
}

inline CommandResult curl_failed(uint32_t code, const std::string &err) {
	CommandResult r;
	r.outcome = CommandResult::Outcome::Exited;
	r.exit_code = code;
	r.err = err;
	return r;
}

// `text` with its %XX escapes decoded.
inline std::string percent_decoded(const std::string &text) {
	std::string out;
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '%' && i + 2 < text.size()) {
			out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
			i += 2;
		} else {
			out.push_back(text[i]);
		}
	}
	return out;
}

// The value of the config file's first `name = "..."` line (unescaped), "" for none.
inline std::string config_line(const std::string &config, const std::string &name) {
	const std::string head = "\n" + name + " = \"";
	const size_t at = ("\n" + config).find(head);
	if (at == std::string::npos)
		return {};
	std::string out;
	for (size_t i = at + head.size() - 1; i < config.size() && config[i] != '\n'; ++i) {
		if (config[i] == '\\' && i + 1 < config.size()) {
			out.push_back(config[++i]);
			continue;
		}
		if (config[i] == '"')
			break;
		out.push_back(config[i]);
	}
	return out;
}

// The name of the call a config file asks for.
inline std::string key_of(const std::string &config) {
	const std::string url = config_line(config, "url");
	if (url.rfind("http://169.254.170.2/", 0) == 0)
		return "credentials";
	const size_t host = url.find("://");
	const std::string service = host == std::string::npos
			? std::string()
			: url.substr(host + 3, url.find('.', host) - host - 3);
	const std::string data = config_line(config, "data-raw");
	const std::string target_head = "header = \"X-Amz-Target: ";
	const size_t target = config.find(target_head);
	if (target != std::string::npos) {
		std::string name = config.substr(target + target_head.size());
		name = name.substr(0, name.find('"'));
		name = name.substr(name.find('.') + 1);
		std::string key = service + " " + name;
		if (name == "ListTasks")
			key += data.find("\"STOPPED\"") != std::string::npos ? " STOPPED" : " RUNNING";
		// The fleet cluster's calls (hostd's daemon, the container instances).
		if (data.find("\"cluster\":\"opennova-prod-fleet\"") != std::string::npos)
			key += " fleet";
		return key;
	}
	const size_t action = data.find("Action=");
	std::string name = action == std::string::npos
			? std::string()
			: data.substr(action + 7, data.find('&', action) - action - 7);
	std::string key = service + " " + name;
	if (name == "DescribeTargetHealth") {
		const std::string arn = percent_decoded(data.substr(data.find("TargetGroupArn=") + 15));
		const size_t start = arn.find("targetgroup/") + 12;
		key += " " + arn.substr(start, arn.find('/', start) - start);
	}
	return key;
}

class AwsStub : public opennova::proc::CommandRunner {
public:
	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		Call call;
		call.argv = argv;
		call.timeout_ms = timeout_ms;
		if (argv.size() == 4) {
			std::ifstream in(std::filesystem::u8path(argv[3]), std::ios::binary);
			call.config.assign(
					std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		call.key = key_of(call.config);
		std::lock_guard<std::mutex> lock(mu_);
		calls_.push_back(call);
		const auto it = replies.find(call.key);
		if (it == replies.end())
			return curl_failed(7, "curl: (7) the stub has no reply for " + call.key);
		return it->second;
	}

	std::vector<Call> calls() {
		std::lock_guard<std::mutex> lock(mu_);
		return calls_;
	}
	size_t count(const std::string &key) {
		std::lock_guard<std::mutex> lock(mu_);
		size_t n = 0;
		for (const Call &c : calls_)
			n += c.key == key ? 1 : 0;
		return n;
	}
	void clear() {
		std::lock_guard<std::mutex> lock(mu_);
		calls_.clear();
	}

	// Set before a read; read-only while one runs.
	std::map<std::string, CommandResult> replies;

private:
	std::mutex mu_;
	std::vector<Call> calls_;
};

// ---- a deployment mid-deploy -----------------------------------------------------------
//
// Release 20261017T120000Z-abc1234 runs every program at revision 7 (gate,
// lobby), 42 (web), 5 (fleet, worker) or 2 (data). Web is rolling out revision 43:
// its new task is still starting beside the old one, and an earlier task of
// it stopped (its container exited 1). The nightly backup runs as a one-off
// task. The balancer: the gate, the lobby and the retail port healthy, 443 with
// the new web task still initial and an unhealthy one. The database and the
// cache Multi-AZ and available; one fleet machine running in zone b.

constexpr const char *kCredentials =
		R"({"RoleArn":"arn:aws:iam::123456789012:role/opennova-prod-web","AccessKeyId":"ASIAINFRASTUBKEY0001",)"
		R"("SecretAccessKey":"infra-stub-secret/AbCdEf+0123456789","Token":"infra-stub-session-token-QWERTY",)"
		R"("Expiration":"2026-10-17T18:00:00Z"})";

constexpr const char *kServices = R"json({"services":[
{"serviceName":"gate","status":"ACTIVE","desiredCount":1,"runningCount":1,"pendingCount":0,
 "taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-gate:7",
 "deployments":[{"id":"ecs-svc/1","status":"PRIMARY","taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-gate:7",
   "desiredCount":1,"pendingCount":0,"runningCount":1,"failedTasks":0,"createdAt":1792233600.5,"updatedAt":1792233900.25,
   "rolloutState":"COMPLETED","rolloutStateReason":"ECS deployment ecs-svc/1 completed."}],
 "events":[{"id":"e3","createdAt":1792236000.0,"message":"(service gate) has reached a steady state."},
   {"id":"e2","createdAt":1792233900.0,"message":"(service gate) registered 1 targets in (target-group opennova-prod-gate)"},
   {"id":"e1","createdAt":1792233600.0,"message":"(service gate) has started 1 tasks: (task 1111aaaa)."}]},
{"serviceName":"lobby","status":"ACTIVE","desiredCount":1,"runningCount":1,"pendingCount":0,
 "taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-lobby:7",
 "deployments":[{"id":"ecs-svc/2","status":"PRIMARY","taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-lobby:7",
   "desiredCount":1,"pendingCount":0,"runningCount":1,"failedTasks":0,"createdAt":1792233600,"updatedAt":1792233900,"rolloutState":"COMPLETED"}],
 "events":[]},
{"serviceName":"web","status":"ACTIVE","desiredCount":2,"runningCount":1,"pendingCount":1,
 "taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:43",
 "deployments":[
  {"id":"ecs-svc/4","status":"PRIMARY","taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:43",
   "desiredCount":1,"pendingCount":1,"runningCount":0,"failedTasks":1,"createdAt":1792238400,"updatedAt":1792238460,
   "rolloutState":"IN_PROGRESS","rolloutStateReason":"ECS deployment ecs-svc/4 in progress."},
  {"id":"ecs-svc/3","status":"ACTIVE","taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:42",
   "desiredCount":1,"pendingCount":0,"runningCount":1,"failedTasks":0,"createdAt":1792233600,"updatedAt":1792233900,"rolloutState":"COMPLETED"}],
 "events":[{"id":"w2","createdAt":1792238460,"message":"(service web) has started 1 tasks: (task 3333cccc)."},
   {"id":"w1","createdAt":1792238430,"message":"(service web) has stopped 1 running tasks: (task 9999ffff)."}]},
{"serviceName":"fleet","status":"ACTIVE","desiredCount":1,"runningCount":1,"pendingCount":0,
 "taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-fleet:5",
 "deployments":[{"id":"ecs-svc/5","status":"PRIMARY","taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-fleet:5",
   "desiredCount":1,"pendingCount":0,"runningCount":1,"failedTasks":0,"createdAt":1792233600,"updatedAt":1792233900,"rolloutState":"COMPLETED"}],
 "events":[]},
{"serviceName":"worker","status":"ACTIVE","desiredCount":1,"runningCount":1,"pendingCount":0,
 "taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:5",
 "deployments":[{"id":"ecs-svc/6","status":"PRIMARY","taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:5",
   "desiredCount":1,"pendingCount":0,"runningCount":1,"failedTasks":0,"createdAt":1792233600,"updatedAt":1792233900,"rolloutState":"COMPLETED"}],
 "events":[]},
{"serviceName":"data","status":"ACTIVE","desiredCount":1,"runningCount":1,"pendingCount":0,
 "taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-data:2",
 "deployments":[{"id":"ecs-svc/7","status":"PRIMARY","taskDefinition":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-data:2",
   "desiredCount":1,"pendingCount":0,"runningCount":1,"failedTasks":0,"createdAt":1792233600,"updatedAt":1792233900,"rolloutState":"COMPLETED"}],
 "events":[]}
],"failures":[]})json";

constexpr const char *kRunningTasks = R"({"taskArns":[
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/1111aaaa1111aaaa1111aaaa1111aaaa",
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/2222bbbb2222bbbb2222bbbb2222bbbb",
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/3333cccc3333cccc3333cccc3333cccc",
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/4444dddd4444dddd4444dddd4444dddd",
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/5555eeee5555eeee5555eeee5555eeee",
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/6666ffff6666ffff6666ffff6666ffff",
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/7777aaaa7777aaaa7777aaaa7777aaaa",
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/8888bbbb8888bbbb8888bbbb8888bbbb"]})";

constexpr const char *kStoppedTasks = R"({"taskArns":[
"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/9999ffff9999ffff9999ffff9999ffff"]})";

// A running task of `program` at `revision`, started at `started` (Unix
// seconds), in `zone` at `ip`, health `health`.
inline std::string task_json(const std::string &id, const std::string &program,
		const std::string &revision, const std::string &started, const std::string &zone,
		const std::string &ip, const std::string &health, const std::string &cpu = "256",
		const std::string &memory = "512") {
	const std::string family =
			program == "backup" ? "family:opennova-prod-backup" : "service:" + program;
	return R"({"taskArn":"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/)" + id +
			R"(","group":")" + family +
			R"(","taskDefinitionArn":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-)" +
			program + ":" + revision +
			R"(","lastStatus":"RUNNING","desiredStatus":"RUNNING","healthStatus":")" + health +
			R"(","availabilityZone":")" + zone + R"(","createdAt":)" + started +
			R"(,"startedAt":)" + started + R"(,"cpu":")" + cpu + R"(","memory":")" + memory +
			R"(","attachments":[{"type":"ElasticNetworkInterface","status":"ATTACHED","details":[{"name":"subnetId","value":"subnet-0public0a"},{"name":"privateIPv4Address","value":")" +
			ip + R"("}]}],"containers":[{"name":")" +
			(program == "backup" ? std::string("backup") : program) +
			R"(","image":"ghcr.io/opennova-net/opennova-novaworld-)" + program +
			R"(@sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","imageDigest":"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","lastStatus":"RUNNING","healthStatus":")" +
			health + R"("}]})";
}

inline std::string described_tasks() {
	std::string out = R"({"tasks":[)";
	out += task_json("1111aaaa1111aaaa1111aaaa1111aaaa", "gate", "7", "1792233600", "us-east-1a",
				   "10.40.0.11", "HEALTHY") +
			",";
	out += task_json("2222bbbb2222bbbb2222bbbb2222bbbb", "lobby", "7", "1792233610", "us-east-1b",
				   "10.40.1.12", "HEALTHY") +
			",";
	out += task_json("4444dddd4444dddd4444dddd4444dddd", "web", "42", "1792233620", "us-east-1a",
				   "10.40.0.14", "HEALTHY", "512", "1024") +
			",";
	// The new web task: provisioned, not yet started (no startedAt), its health unknown.
	out += R"({"taskArn":"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/3333cccc3333cccc3333cccc3333cccc","group":"service:web",)"
		   R"("taskDefinitionArn":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:43","lastStatus":"PENDING",)"
		   R"("desiredStatus":"RUNNING","healthStatus":"UNKNOWN","availabilityZone":"us-east-1b","createdAt":1792238455,"cpu":"512","memory":"1024",)"
		   R"("attachments":[{"type":"ElasticNetworkInterface","status":"ATTACHED","details":[{"name":"privateIPv4Address","value":"10.40.1.13"}]}],)"
		   R"("containers":[{"name":"web","image":"ghcr.io/opennova-net/opennova-novaworld-web@sha256:fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","lastStatus":"PENDING","healthStatus":"UNKNOWN"},)"
		   R"({"name":"portal","image":"ghcr.io/opennova-net/novaworld-web@sha256:aaaa","lastStatus":"PENDING","healthStatus":"UNKNOWN"}]},)";
	out += task_json("5555eeee5555eeee5555eeee5555eeee", "fleet", "5", "1792233630",
				   "us-east-1b", "10.40.1.15", "HEALTHY") +
			",";
	out += task_json("6666ffff6666ffff6666ffff6666ffff", "worker", "5", "1792233640", "us-east-1a",
				   "10.40.0.16", "HEALTHY", "512", "1024") +
			",";
	out += task_json("7777aaaa7777aaaa7777aaaa7777aaaa", "backup", "3", "1792238000", "us-east-1a",
				   "10.40.0.17", "UNKNOWN") +
			",";
	// A web task ECS still lists as stopped: its container exited 1.
	out += R"({"taskArn":"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/9999ffff9999ffff9999ffff9999ffff","group":"service:web",)"
		   R"("taskDefinitionArn":"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:43","lastStatus":"STOPPED",)"
		   R"("desiredStatus":"STOPPED","healthStatus":"UNHEALTHY","availabilityZone":"us-east-1a","createdAt":1792238400,"startedAt":1792238405,)"
		   R"("stoppingAt":1792238428,"stoppedAt":1792238430,"stoppedReason":"Essential container in task exited","stopCode":"EssentialContainerExited",)"
		   R"("cpu":"512","memory":"1024","containers":[{"name":"web","image":"ghcr.io/opennova-net/opennova-novaworld-web@sha256:fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",)"
		   R"("lastStatus":"STOPPED","healthStatus":"UNHEALTHY","exitCode":1,"reason":"","networkInterfaces":[{"privateIpv4Address":"10.40.0.19"}]}]})";
	// 8888bbbb was listed and is gone by the description: ECS reports it a failure.
	out += R"(],"failures":[{"arn":"arn:aws:ecs:us-east-1:123456789012:task/opennova-prod/8888bbbb8888bbbb8888bbbb8888bbbb","reason":"MISSING"}]})";
	return out;
}

constexpr const char *kRelease =
		R"({"Parameter":{"Name":"/opennova/prod/release","Type":"String","Version":12,"LastModifiedDate":1792233950.1,)"
		R"("Value":"{\"id\":\"20261017T120000Z-abc1234\",\"deployment\":\"prod\",\"created\":\"2026-10-17T12:00:00Z\",)"
		R"(\"revision\":\"abc1234def56\",\"dirty\":false,\"previous\":\"20261016T090000Z-0f0f0f0\",\"programs\":[\"gate\",\"lobby\",\"web\",\"fleet\",\"worker\",\"data\"],)"
		R"(\"images\":{\"gate\":\"ghcr.io/opennova-net/opennova-novaworld-gate@sha256:0123\"},)"
		R"(\"task_definitions\":{\"gate\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-gate:7\",)"
		R"(\"lobby\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-lobby:7\",)"
		R"(\"web\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:42\",)"
		R"(\"fleet\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-fleet:5\",)"
		R"(\"worker\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:5\",)"
		R"(\"data\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-data:2\",)"
		R"(\"backup\":\"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup:3\"}}"}})";

// A DescribeTargetHealth reply of `members` (each a <member>...</member>).
inline std::string target_health(const std::string &members) {
	return R"(<?xml version="1.0" encoding="UTF-8"?>
<DescribeTargetHealthResponse xmlns="http://elasticloadbalancing.amazonaws.com/doc/2015-12-01/">
  <DescribeTargetHealthResult>
    <TargetHealthDescriptions>)" +
			members + R"(
    </TargetHealthDescriptions>
  </DescribeTargetHealthResult>
  <ResponseMetadata><RequestId>3f8c-stub</RequestId></ResponseMetadata>
</DescribeTargetHealthResponse>)";
}

inline std::string target(const std::string &ip, int port, const std::string &zone,
		const std::string &state, const std::string &reason = {},
		const std::string &description = {}) {
	std::string health = "<State>" + state + "</State>";
	if (!reason.empty())
		health += "<Reason>" + reason + "</Reason>";
	if (!description.empty())
		health += "<Description>" + description + "</Description>";
	return "\n      <member>\n        <HealthCheckPort>traffic-port</HealthCheckPort>\n        "
		   "<TargetHealth>" +
			health + "</TargetHealth>\n        <Target><Port>" + std::to_string(port) +
			"</Port><Id>" + ip + "</Id><AvailabilityZone>" + zone +
			"</AvailabilityZone></Target>\n      </member>";
}

constexpr const char *kDatabase =
		R"(<DescribeDBInstancesResponse xmlns="http://rds.amazonaws.com/doc/2014-10-31/">
  <DescribeDBInstancesResult>
    <DBInstances>
      <DBInstance>
        <DBInstanceIdentifier>opennova-prod-db</DBInstanceIdentifier>
        <DBInstanceClass>db.t4g.small</DBInstanceClass>
        <Engine>postgres</Engine>
        <DBInstanceStatus>available</DBInstanceStatus>
        <Endpoint><Address>opennova-prod-db.abcdefghijkl.us-east-1.rds.amazonaws.com</Address><Port>5432</Port></Endpoint>
        <AllocatedStorage>20</AllocatedStorage>
        <AvailabilityZone>us-east-1a</AvailabilityZone>
        <MultiAZ>true</MultiAZ>
        <EngineVersion>17.4</EngineVersion>
        <SecondaryAvailabilityZone>us-east-1b</SecondaryAvailabilityZone>
        <StorageType>gp3</StorageType>
        <PendingModifiedValues/>
      </DBInstance>
    </DBInstances>
  </DescribeDBInstancesResult>
  <ResponseMetadata><RequestId>7a1d-stub</RequestId></ResponseMetadata>
</DescribeDBInstancesResponse>)";

constexpr const char *kCache =
		R"(<DescribeReplicationGroupsResponse xmlns="http://elasticache.amazonaws.com/doc/2015-02-02/">
  <DescribeReplicationGroupsResult>
    <ReplicationGroups>
      <ReplicationGroup>
        <ReplicationGroupId>opennova-prod-cache</ReplicationGroupId>
        <Description>OpenNova prod: web&apos;s sessions &amp; rate limits</Description>
        <Status>available</Status>
        <MultiAZ>enabled</MultiAZ>
        <AutomaticFailover>enabled</AutomaticFailover>
        <CacheNodeType>cache.t4g.micro</CacheNodeType>
        <Engine>valkey</Engine>
        <NodeGroups>
          <NodeGroup>
            <NodeGroupId>0001</NodeGroupId>
            <Status>available</Status>
            <NodeGroupMembers>
              <NodeGroupMember>
                <CacheClusterId>opennova-prod-cache-001</CacheClusterId>
                <CacheNodeId>0001</CacheNodeId>
                <PreferredAvailabilityZone>us-east-1a</PreferredAvailabilityZone>
                <CurrentRole>primary</CurrentRole>
              </NodeGroupMember>
              <NodeGroupMember>
                <CacheClusterId>opennova-prod-cache-002</CacheClusterId>
                <CacheNodeId>0001</CacheNodeId>
                <PreferredAvailabilityZone>us-east-1b</PreferredAvailabilityZone>
                <CurrentRole>replica</CurrentRole>
              </NodeGroupMember>
            </NodeGroupMembers>
          </NodeGroup>
        </NodeGroups>
      </ReplicationGroup>
    </ReplicationGroups>
  </DescribeReplicationGroupsResult>
</DescribeReplicationGroupsResponse>)";

constexpr const char *kMachines = R"(<?xml version="1.0" encoding="UTF-8"?>
<DescribeInstancesResponse xmlns="http://ec2.amazonaws.com/doc/2016-11-15/">
    <requestId>8f7e-stub</requestId>
    <reservationSet>
        <item>
            <reservationId>r-0123456789abcdef0</reservationId>
            <ownerId>123456789012</ownerId>
            <instancesSet>
                <item>
                    <instanceId>i-0aaaaaaaaaaaaaaa1</instanceId>
                    <imageId>ami-0123456789abcdef0</imageId>
                    <instanceState><code>16</code><name>running</name></instanceState>
                    <instanceType>t3a.large</instanceType>
                    <placement><availabilityZone>us-east-1b</availabilityZone><tenancy>default</tenancy></placement>
                    <privateIpAddress>10.40.1.40</privateIpAddress>
                </item>
            </instancesSet>
        </item>
    </reservationSet>
</DescribeInstancesResponse>)";

// The fleet cluster: hostd's daemon service, and two machines registered there.
constexpr const char *kFleetServices = R"({"services": [{
  "serviceName": "hostd", "status": "ACTIVE", "desiredCount": 2, "runningCount": 2,
  "pendingCount": 0, "schedulingStrategy": "DAEMON",
  "taskDefinition": "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-hostd:5",
  "deployments": [{"id": "ecs-svc/2", "status": "PRIMARY", "rolloutState": "COMPLETED",
    "taskDefinition": "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-hostd:5",
    "desiredCount": 2, "runningCount": 2, "pendingCount": 0, "failedTasks": 0,
    "createdAt": 1792231200.0, "updatedAt": 1792231500.0}],
  "events": []}], "failures": []})";
constexpr const char *kContainerInstanceArns = R"({"containerInstanceArns": [
  "arn:aws:ecs:us-east-1:123456789012:container-instance/opennova-prod-fleet/aaaa1111",
  "arn:aws:ecs:us-east-1:123456789012:container-instance/opennova-prod-fleet/bbbb2222"]})";
constexpr const char *kContainerInstances = R"({"containerInstances": [
  {"containerInstanceArn": "arn:aws:ecs:us-east-1:123456789012:container-instance/opennova-prod-fleet/aaaa1111",
   "ec2InstanceId": "i-0aaaaaaaaaaaaaaa1", "status": "ACTIVE", "agentConnected": true,
   "runningTasksCount": 1, "pendingTasksCount": 0,
   "versionInfo": {"agentVersion": "1.98.0", "dockerVersion": "DockerVersion: 25.0.8"},
   "attributes": [{"name": "ecs.ami-id", "value": "ami-0123456789abcdef0"},
     {"name": "machine-id", "value": "12"}, {"name": "kind", "value": "shared"},
     {"name": "ecs.instance-type", "value": "t3a.medium"}]},
  {"containerInstanceArn": "arn:aws:ecs:us-east-1:123456789012:container-instance/opennova-prod-fleet/bbbb2222",
   "ec2InstanceId": "i-0bbbbbbbbbbbbbbb2", "status": "DRAINING", "agentConnected": false,
   "runningTasksCount": 0, "pendingTasksCount": 1,
   "attributes": [{"name": "machine-id", "value": "13"}]}],
  "failures": []})";

// The calls of a read of the deployment above and their replies.
inline std::map<std::string, CommandResult> deployment_replies() {
	std::map<std::string, CommandResult> r;
	r["credentials"] = answer(200, kCredentials);
	r["ecs DescribeServices"] = answer(200, kServices);
	r["ecs ListTasks RUNNING"] = answer(200, kRunningTasks);
	r["ecs ListTasks STOPPED"] = answer(200, kStoppedTasks);
	r["ecs DescribeTasks"] = answer(200, described_tasks());
	r["ssm GetParameter"] = answer(200, kRelease);
	r["elasticloadbalancing DescribeTargetHealth opennova-prod-gate"] =
			answer(200, target_health(target("10.40.0.11", 7597, "us-east-1a", "healthy")));
	r["elasticloadbalancing DescribeTargetHealth opennova-prod-lobby"] =
			answer(200, target_health(target("10.40.1.12", 64206, "us-east-1b", "healthy")));
	r["elasticloadbalancing DescribeTargetHealth opennova-prod-http"] = answer(200,
			target_health(target("10.40.0.14", 8080, "us-east-1a", "healthy") +
					target("10.40.1.13", 8080, "us-east-1b", "initial",
							"Elb.RegistrationInProgress", "Target registration is in progress")));
	r["elasticloadbalancing DescribeTargetHealth opennova-prod-https"] = answer(200,
			target_health(target("10.40.0.14", 443, "us-east-1a", "healthy") +
					target("10.40.0.19", 443, "us-east-1a", "unhealthy",
							"Target.FailedHealthChecks",
							"Health checks failed &amp; the target is &lt;draining&gt;")));
	r["rds DescribeDBInstances"] = answer(200, kDatabase);
	r["elasticache DescribeReplicationGroups"] = answer(200, kCache);
	r["ec2 DescribeInstances"] = answer(200, kMachines);
	r["ecs DescribeServices fleet"] = answer(200, kFleetServices);
	r["ecs ListContainerInstances fleet"] = answer(200, kContainerInstanceArns);
	r["ecs DescribeContainerInstances fleet"] = answer(200, kContainerInstances);
	return r;
}

} // namespace infra_stub
