#include "infra_status.h"

#include "aws_xml.h"
#include "billing/stripe_api.h"
#include "health.h" // Watchdog::steady_now_ms
#include "hosted/stats_repository.h"
#include "loggable.h"
#include "text_util.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>

namespace opennova::novaworld_server::infra {

namespace {

using io::JsonValue;

constexpr const char *kEcsTarget = "AmazonEC2ContainerServiceV20141113.";
// ListTasks pages of running tasks one read follows at most (100 a page).
constexpr int kTaskPages = 3;

int64_t wall_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

bool is_name_char(char c) {
	return is_alnum(c) || c == '-' || c == '_';
}

bool all_of(std::string_view text, bool (*ok)(char)) {
	for (const char c : text)
		if (!ok(c))
			return false;
	return true;
}

// `text` as a JSON string literal.
std::string json_text(std::string_view text) {
	std::string out = "\"";
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out.push_back('\\');
			out.push_back(c);
		} else if (byte < 0x20) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
			out += escaped;
		} else {
			out.push_back(c);
		}
	}
	out.push_back('"');
	return out;
}

std::string json_list(const std::vector<std::string> &items) {
	std::string out = "[";
	for (size_t i = 0; i < items.size(); ++i) {
		if (i != 0)
			out.push_back(',');
		out += json_text(items[i]);
	}
	out.push_back(']');
	return out;
}

const JsonValue *member(const JsonValue &object, const char *key, JsonValue::Type type) {
	const JsonValue *v = object.get(key);
	return v != nullptr && v->type == type ? v : nullptr;
}

std::string text_of(const JsonValue &object, const char *key) {
	const JsonValue *v = member(object, key, JsonValue::Type::String);
	return v != nullptr ? v->string : std::string();
}

int64_t count_of(const JsonValue &object, const char *key) {
	const JsonValue *v = member(object, key, JsonValue::Type::Number);
	return v != nullptr && std::isfinite(v->number) ? static_cast<int64_t>(v->number) : 0;
}

// ECS's times are Unix seconds (with a fraction): ISO 8601 UTC, "" for none.
std::string epoch_iso(const JsonValue &object, const char *key) {
	const JsonValue *v = member(object, key, JsonValue::Type::Number);
	if (v == nullptr || !std::isfinite(v->number) || v->number <= 0)
		return {};
	return hosted::iso_utc(static_cast<int64_t>(std::floor(v->number)));
}

const std::vector<JsonValue> &array_of(const JsonValue &object, const char *key) {
	static const std::vector<JsonValue> kNone;
	const JsonValue *v = member(object, key, JsonValue::Type::Array);
	return v != nullptr ? v->array : kNone;
}

std::string last_segment(const std::string &arn) {
	const size_t slash = arn.rfind('/');
	return slash == std::string::npos ? arn : arn.substr(slash + 1);
}

// A reply's JSON object; false with `error` (the call's failure, or the body
// not one).
bool json_reply(const char *service, const AwsReply &reply, JsonValue &out, std::string &error) {
	if (!reply.ok()) {
		error = AwsApi::failure(service, reply);
		return false;
	}
	std::string parse_error;
	if (!io::json_parse(reply.body, out, parse_error) || !out.is_object()) {
		error = std::string(service) + " answered no JSON object";
		return false;
	}
	return true;
}

bool xml_reply(const char *service, const AwsReply &reply, XmlNode &out, std::string &error) {
	if (!reply.ok()) {
		error = AwsApi::failure(service, reply);
		return false;
	}
	std::string parse_error;
	if (!parse_xml(reply.body, out, parse_error)) {
		error = std::string(service) + " answered no XML (" + parse_error + ")";
		return false;
	}
	return true;
}

int64_t number_text(const std::string &text) {
	return strutil::all_digits(text) && !text.empty() && text.size() < 18
			? std::strtoll(text.c_str(), nullptr, 10)
			: 0;
}

// ---- the parsers ----------------------------------------------------------------------

EcsTask parse_task(const JsonValue &t) {
	EcsTask task;
	task.id = last_segment(text_of(t, "taskArn"));
	task.group = text_of(t, "group");
	if (starts_with(task.group, "service:"))
		task.program = task.group.substr(8);
	task.task_definition_arn = text_of(t, "taskDefinitionArn");
	task.last_status = text_of(t, "lastStatus");
	task.desired_status = text_of(t, "desiredStatus");
	task.health = text_of(t, "healthStatus");
	task.created_at = epoch_iso(t, "createdAt");
	task.started_at = epoch_iso(t, "startedAt");
	task.stopping_at = epoch_iso(t, "stoppingAt");
	task.stopped_at = epoch_iso(t, "stoppedAt");
	task.stopped_reason = text_of(t, "stoppedReason");
	task.stop_code = text_of(t, "stopCode");
	task.zone = text_of(t, "availabilityZone");
	task.cpu = text_of(t, "cpu");
	task.memory = text_of(t, "memory");
	// The task's network interface (awsvpc): its private address.
	for (const JsonValue &a : array_of(t, "attachments")) {
		if (text_of(a, "type") != "ElasticNetworkInterface")
			continue;
		for (const JsonValue &d : array_of(a, "details"))
			if (text_of(d, "name") == "privateIPv4Address")
				task.private_ip = text_of(d, "value");
	}
	for (const JsonValue &c : array_of(t, "containers")) {
		EcsContainer container;
		container.name = text_of(c, "name");
		container.image = text_of(c, "image");
		container.image_digest = text_of(c, "imageDigest");
		container.last_status = text_of(c, "lastStatus");
		container.health = text_of(c, "healthStatus");
		container.reason = text_of(c, "reason");
		if (const JsonValue *code = member(c, "exitCode", JsonValue::Type::Number))
			container.exit_code = static_cast<int64_t>(code->number);
		if (task.private_ip.empty())
			for (const JsonValue &n : array_of(c, "networkInterfaces"))
				if (task.private_ip.empty())
					task.private_ip = text_of(n, "privateIpv4Address");
		task.containers.push_back(std::move(container));
	}
	return task;
}

EcsService parse_service(const JsonValue &v) {
	EcsService s;
	s.name = text_of(v, "serviceName");
	s.status = text_of(v, "status");
	s.desired = count_of(v, "desiredCount");
	s.running = count_of(v, "runningCount");
	s.pending = count_of(v, "pendingCount");
	s.task_definition_arn = text_of(v, "taskDefinition");
	for (const JsonValue &d : array_of(v, "deployments")) {
		EcsDeployment dep;
		dep.id = text_of(d, "id");
		dep.status = text_of(d, "status");
		dep.rollout_state = text_of(d, "rolloutState");
		dep.rollout_reason = text_of(d, "rolloutStateReason");
		dep.task_definition_arn = text_of(d, "taskDefinition");
		dep.desired = count_of(d, "desiredCount");
		dep.running = count_of(d, "runningCount");
		dep.pending = count_of(d, "pendingCount");
		dep.failed = count_of(d, "failedTasks");
		dep.created_at = epoch_iso(d, "createdAt");
		dep.updated_at = epoch_iso(d, "updatedAt");
		s.deployments.push_back(std::move(dep));
	}
	// ECS lists the newest event first.
	for (const JsonValue &e : array_of(v, "events")) {
		if (s.events.size() >= InfraStatus::kEventsKept)
			break;
		s.events.push_back({ epoch_iso(e, "createdAt"), text_of(e, "message") });
	}
	return s;
}

Target parse_target(const XmlNode &member) {
	Target t;
	if (const XmlNode *target = member.child("Target")) {
		t.id = target->child_text("Id");
		t.port = number_text(target->child_text("Port"));
		t.zone = target->child_text("AvailabilityZone");
	}
	if (const XmlNode *health = member.child("TargetHealth")) {
		t.state = health->child_text("State");
		t.reason = health->child_text("Reason");
		t.description = health->child_text("Description");
	}
	return t;
}

DatabaseStatus parse_database(const XmlNode &db) {
	DatabaseStatus d;
	d.identifier = db.child_text("DBInstanceIdentifier");
	d.status = db.child_text("DBInstanceStatus");
	d.engine = db.child_text("Engine");
	d.engine_version = db.child_text("EngineVersion");
	d.instance_class = db.child_text("DBInstanceClass");
	d.multi_az = db.child_text("MultiAZ") == "true";
	d.zone = db.child_text("AvailabilityZone");
	d.secondary_zone = db.child_text("SecondaryAvailabilityZone");
	d.storage_type = db.child_text("StorageType");
	d.storage_gb = number_text(db.child_text("AllocatedStorage"));
	return d;
}

CacheStatus parse_cache(const XmlNode &group) {
	CacheStatus c;
	c.id = group.child_text("ReplicationGroupId");
	c.status = group.child_text("Status");
	c.multi_az = group.child_text("MultiAZ");
	c.automatic_failover = group.child_text("AutomaticFailover");
	c.node_type = group.child_text("CacheNodeType");
	c.engine = group.child_text("Engine");
	if (const XmlNode *groups = group.child("NodeGroups"))
		for (const XmlNode &g : groups->children)
			if (const XmlNode *members = g.child("NodeGroupMembers"))
				for (const XmlNode &m : members->children)
					c.nodes.push_back({ m.child_text("CacheClusterId"), m.child_text("CurrentRole"),
							m.child_text("PreferredAvailabilityZone") });
	return c;
}

// The target groups in the balancer's order: gate, lobby, http, https, then
// any other by name.
int listener_rank(const std::string &key) {
	static const char *const kOrder[] = { "gate", "lobby", "http", "https" };
	for (int i = 0; i < 4; ++i)
		if (key == kOrder[i])
			return i;
	return 4;
}

} // namespace

InfraConfig infra_config_from_env(const std::string &scratch_dir) {
	const auto env = [](const char *name) {
		const char *v = std::getenv(name);
		return v != nullptr ? std::string(strutil::trim_view(v)) : std::string();
	};
	InfraConfig c;
	c.aws.region = env("ONNET_INFRA_REGION");
	c.aws.credentials_uri = env("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
	c.aws.scratch_dir = scratch_dir;
	c.cluster = env("ONNET_INFRA_CLUSTER");
	c.database = env("ONNET_INFRA_DATABASE");
	c.cache = env("ONNET_INFRA_CACHE");
	c.release_parameter = env("ONNET_INFRA_RELEASE_PARAMETER");
	if (const std::string groups = env("ONNET_INFRA_TARGET_GROUPS"); !groups.empty()) {
		JsonValue json;
		std::string error;
		if (io::json_parse(groups, json, error) && json.is_object()) {
			for (const io::JsonMember &m : json.object)
				if (m.value.is_string())
					c.target_groups.emplace_back(m.key, m.value.string);
		} else {
			std::fprintf(stderr,
					"[boot] WARN infrastructure: ONNET_INFRA_TARGET_GROUPS is no JSON object; no "
					"target group is read\n");
		}
	}
	std::stable_sort(
			c.target_groups.begin(), c.target_groups.end(), [](const auto &a, const auto &b) {
				return listener_rank(a.first) < listener_rank(b.first);
			});
	return c;
}

std::string check_infra_config(const InfraConfig &c) {
	if (c.cluster.empty())
		return "AWS is not read here: ONNET_INFRA_CLUSTER is unset (the local stack runs no ECS "
			   "services, load balancer, RDS or ElastiCache)";
	if (c.cluster.size() > 255 || !all_of(c.cluster, is_name_char))
		return "ONNET_INFRA_CLUSTER is no cluster name";
	if (const std::string why = AwsApi::check_config(c.aws); !why.empty())
		return "AWS is not read here: " + why;
	for (const auto &[key, arn] : c.target_groups)
		if (!starts_with(arn, "arn:") || has_control_byte(arn) || has_control_byte(key))
			return "ONNET_INFRA_TARGET_GROUPS names something that is no target group ARN";
	if (has_control_byte(c.database) || has_control_byte(c.cache) ||
			has_control_byte(c.release_parameter))
		return "an ONNET_INFRA_* setting holds a control byte";
	if (c.cache_ms < 0)
		return "the cache window is negative";
	return {};
}

InfraStatus::InfraStatus(InfraConfig config, proc::CommandRunner &runner, Clock now_ms) :
		config_(std::move(config)),
		unavailable_(check_infra_config(config_)),
		api_(runner, config_.aws),
		now_(now_ms ? std::move(now_ms) : Clock(Watchdog::steady_now_ms)) {}

std::shared_ptr<const CloudSnapshot> InfraStatus::snapshot(
		const std::vector<std::string> &instance_ids) {
	std::unique_lock<std::mutex> lock(mu_);
	for (;;) {
		if (current_ && now_() - current_at_ < config_.cache_ms)
			return current_;
		if (!reading_)
			break;
		// Another caller's read: its answer is as fresh as one of our own.
		read_done_.wait(lock);
		if (current_ && !reading_)
			return current_;
	}
	reading_ = true;
	lock.unlock();
	std::shared_ptr<const CloudSnapshot> fresh;
	try {
		fresh = std::make_shared<const CloudSnapshot>(read(instance_ids));
	} catch (...) {
		lock.lock();
		reading_ = false;
		read_done_.notify_all();
		throw;
	}
	lock.lock();
	current_ = fresh;
	current_at_ = now_();
	reading_ = false;
	read_done_.notify_all();
	return current_;
}

CloudSnapshot InfraStatus::read(const std::vector<std::string> &instance_ids) {
	CloudSnapshot s;
	s.read_ms = wall_now_ms();
	s.cluster = config_.cluster;
	s.region = config_.aws.region;
	if (!configured()) {
		s.unavailable = unavailable_;
		return s;
	}
	AwsCredentials creds;
	if (!api_.credentials(creds, s.error))
		return s;

	const std::string cluster = json_text(config_.cluster);
	// The fleet's machines EC2 is asked about.
	std::vector<std::string> ids;
	for (const std::string &id : instance_ids)
		if (starts_with(id, "i-") && id.size() <= 32 && all_of(id, is_name_char) &&
				ids.size() < kMachinesAsked)
			ids.push_back(id);
	// Declared after everything the calls read: a call still running when an
	// exception leaves is waited for by its future's destructor.
	std::vector<std::future<void>> calls;
	const auto run = [&calls](auto fn) {
		calls.push_back(std::async(std::launch::async, std::move(fn)));
	};

	// The programs' services.
	run([&] {
		const std::string body =
				"{\"cluster\":" + cluster + ",\"services\":" + json_list(config_.programs) + "}";
		JsonValue reply;
		if (!json_reply("ecs",
					api_.json_call(
							creds, "ecs", std::string(kEcsTarget) + "DescribeServices", body),
					reply, s.services_error))
			return;
		std::vector<EcsService> listed;
		for (const JsonValue &v : array_of(reply, "services"))
			listed.push_back(parse_service(v));
		for (const std::string &p : config_.programs)
			for (EcsService &svc : listed)
				if (svc.name == p)
					s.services.push_back(std::move(svc));
		for (const JsonValue &f : array_of(reply, "failures"))
			s.missing_services.push_back(
					last_segment(text_of(f, "arn")) + " (" + text_of(f, "reason") + ")");
	});

	// The tasks: the running ones (every page up to kTaskPages), the stopped
	// ones ECS still lists, then one description of up to kTasksDescribed.
	run([&] {
		const auto list = [&](const char *desired, int pages, std::vector<std::string> &arns) {
			std::string token;
			for (int page = 0; page < pages; ++page) {
				std::string body = "{\"cluster\":" + cluster + ",\"desiredStatus\":\"" + desired +
						"\",\"maxResults\":100";
				if (!token.empty())
					body += ",\"nextToken\":" + json_text(token);
				body += "}";
				JsonValue reply;
				if (!json_reply("ecs",
							api_.json_call(
									creds, "ecs", std::string(kEcsTarget) + "ListTasks", body),
							reply, s.tasks_error))
					return false;
				for (const JsonValue &a : array_of(reply, "taskArns"))
					if (a.is_string())
						arns.push_back(a.string);
				token = text_of(reply, "nextToken");
				if (token.empty())
					break;
			}
			return true;
		};
		std::vector<std::string> running;
		std::vector<std::string> stopped;
		if (!list("RUNNING", kTaskPages, running) || !list("STOPPED", 1, stopped))
			return;
		std::vector<std::string> arns = running;
		for (const std::string &a : stopped)
			if (std::find(arns.begin(), arns.end(), a) == arns.end())
				arns.push_back(a);
		if (arns.size() > kTasksDescribed)
			arns.resize(kTasksDescribed);
		if (arns.empty())
			return;
		const std::string body = "{\"cluster\":" + cluster + ",\"tasks\":" + json_list(arns) + "}";
		JsonValue reply;
		if (!json_reply("ecs",
					api_.json_call(creds, "ecs", std::string(kEcsTarget) + "DescribeTasks", body),
					reply, s.tasks_error))
			return;
		for (const JsonValue &t : array_of(reply, "tasks"))
			s.tasks.push_back(parse_task(t));
		// Running first, the oldest start first (a pending task, with none, by its
		// creation); then the stopped, the newest stop first.
		std::stable_sort(s.tasks.begin(), s.tasks.end(), [](const EcsTask &a, const EcsTask &b) {
			const bool a_stopped = a.desired_status == "STOPPED";
			const bool b_stopped = b.desired_status == "STOPPED";
			if (a_stopped != b_stopped)
				return !a_stopped;
			if (a_stopped)
				return a.stopped_at > b.stopped_at;
			const std::string &a_at = a.started_at.empty() ? a.created_at : a.started_at;
			const std::string &b_at = b.started_at.empty() ? b.created_at : b.started_at;
			return a_at < b_at;
		});
	});

	// The balancer's target groups, one call each.
	s.target_groups.resize(config_.target_groups.size());
	for (size_t i = 0; i < config_.target_groups.size(); ++i) {
		s.target_groups[i].key = config_.target_groups[i].first;
		s.target_groups[i].arn = config_.target_groups[i].second;
		run([&, i] {
			TargetGroupHealth &group = s.target_groups[i];
			XmlNode reply;
			if (!xml_reply("elasticloadbalancing",
						api_.query_call(creds, "elasticloadbalancing",
								{ { "Action", "DescribeTargetHealth" }, { "Version", "2015-12-01" },
										{ "TargetGroupArn", group.arn } }),
						reply, group.error))
				return;
			if (const XmlNode *list =
							reply.at({ "DescribeTargetHealthResult", "TargetHealthDescriptions" }))
				for (const XmlNode &m : list->children)
					group.targets.push_back(parse_target(m));
		});
	}

	// The database.
	if (config_.database.empty()) {
		s.database_error = "not read: ONNET_INFRA_DATABASE is unset";
	} else {
		run([&] {
			XmlNode reply;
			if (!xml_reply("rds",
						api_.query_call(creds, "rds",
								{ { "Action", "DescribeDBInstances" }, { "Version", "2014-10-31" },
										{ "DBInstanceIdentifier", config_.database } }),
						reply, s.database_error))
				return;
			const XmlNode *list = reply.at({ "DescribeDBInstancesResult", "DBInstances" });
			if (list != nullptr && !list->children.empty())
				s.database = parse_database(list->children.front());
			else
				s.database_error = "rds lists no instance " + loggable(config_.database, 80);
		});
	}

	// The cache.
	if (config_.cache.empty()) {
		s.cache_error = "not read: ONNET_INFRA_CACHE is unset";
	} else {
		run([&] {
			XmlNode reply;
			if (!xml_reply("elasticache",
						api_.query_call(creds, "elasticache",
								{ { "Action", "DescribeReplicationGroups" },
										{ "Version", "2015-02-02" },
										{ "ReplicationGroupId", config_.cache } }),
						reply, s.cache_error))
				return;
			const XmlNode *list =
					reply.at({ "DescribeReplicationGroupsResult", "ReplicationGroups" });
			if (list != nullptr && !list->children.empty())
				s.cache = parse_cache(list->children.front());
			else
				s.cache_error =
						"elasticache lists no replication group " + loggable(config_.cache, 80);
		});
	}

	// The release: the manifest `app deploy` records ("none" before the first).
	if (config_.release_parameter.empty()) {
		s.release_error = "not read: ONNET_INFRA_RELEASE_PARAMETER is unset";
	} else {
		run([&] {
			JsonValue reply;
			if (!json_reply("ssm",
						api_.json_call(creds, "ssm", "AmazonSSM.GetParameter",
								"{\"Name\":" + json_text(config_.release_parameter) + "}"),
						reply, s.release_error))
				return;
			const JsonValue *parameter = member(reply, "Parameter", JsonValue::Type::Object);
			const std::string value =
					parameter != nullptr ? text_of(*parameter, "Value") : std::string();
			const std::string trimmed(strutil::trim_view(value));
			if (trimmed.empty() || trimmed == "none") {
				s.release_none = true;
				return;
			}
			Release release;
			JsonValue manifest;
			std::string parse_error;
			if (io::json_parse(trimmed, manifest, parse_error) && manifest.is_object()) {
				release.id = text_of(manifest, "id");
				release.created = text_of(manifest, "created");
				release.revision = text_of(manifest, "revision");
				if (const JsonValue *defs =
								member(manifest, "task_definitions", JsonValue::Type::Object))
					for (const io::JsonMember &m : defs->object)
						if (m.value.is_string())
							release.task_definitions[m.key] = m.value.string;
			} else {
				// A release before the programs (the single instance's id): it names
				// no revision.
				release.id = loggable(trimmed, 100);
			}
			s.release = std::move(release);
		});
	}

	// The fleet's machines as EC2 has them.
	if (!ids.empty()) {
		run([&] {
			std::vector<std::pair<std::string, std::string>> params = {
				{ "Action", "DescribeInstances" }, { "Version", "2016-11-15" },
				{ "Filter.1.Name", "instance-id" }
			};
			for (size_t i = 0; i < ids.size(); ++i)
				params.emplace_back("Filter.1.Value." + std::to_string(i + 1), ids[i]);
			XmlNode reply;
			if (!xml_reply("ec2", api_.query_call(creds, "ec2", params), reply, s.machines_error))
				return;
			if (const XmlNode *reservations = reply.child("reservationSet"))
				for (const XmlNode &r : reservations->children)
					if (const XmlNode *instances = r.child("instancesSet"))
						for (const XmlNode &i : instances->children) {
							MachineInstance m;
							m.id = i.child_text("instanceId");
							if (const XmlNode *state = i.child("instanceState"))
								m.state = state->child_text("name");
							if (const XmlNode *placement = i.child("placement"))
								m.zone = placement->child_text("availabilityZone");
							m.type = i.child_text("instanceType");
							if (!m.id.empty())
								s.machines[m.id] = std::move(m);
						}
		});
	}

	for (std::future<void> &call : calls)
		call.get();
	return s;
}

InfraStack make_infra() {
	InfraStack stack;
	std::string error;
	stack.scratch = ScratchDir::make("opennova-infra-", error);
	if (stack.scratch.empty())
		std::fprintf(stderr,
				"[boot] WARN infrastructure: no scratch directory for the AWS calls%s%s\n",
				error.empty() ? "" : ": ", error.c_str());
	stack.runner = std::make_unique<proc::ProcessCommandRunner>(
			billing::curl_environment(), stack.scratch.path().u8string());
	InfraConfig config = infra_config_from_env(stack.scratch.path().u8string());
	stack.status = std::make_unique<InfraStatus>(config, *stack.runner);
	if (stack.status->configured())
		std::printf("[boot] infrastructure: cluster %s in %s, %zu target groups, read at most "
					"every %lld s\n",
				config.cluster.c_str(), config.aws.region.c_str(), config.target_groups.size(),
				static_cast<long long>(config.cache_ms / 1000));
	else
		std::printf("[boot] infrastructure: %s\n", check_infra_config(config).c_str());
	return stack;
}

} // namespace opennova::novaworld_server::infra
