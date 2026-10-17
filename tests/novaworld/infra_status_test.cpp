// The admin's infrastructure view's AWS reads (apps/novaworld_web/infra/), no
// database and no network: the Query APIs' XML reader (elements, text,
// references, CDATA, prefixes; the malformed refused); the signed curl calls
// on a stub CommandRunner (argv exactly {curl, -q, --config, <file>}, the file
// naming the endpoint, the SigV4 scope, the target or the form, the credentials
// and the session token, owner-only and deleted after; no credential on argv or
// in an error); the credentials from the ECS agent's endpoint; AWS's error
// bodies (JSON and XML) in words; and InfraStatus over a deployment mid-deploy
// (infra_aws_stub.h): every section parsed, the calls each made once, a section
// that fails standing alone, the credentials failing stopping the read, the
// local stack reading nothing, and the cache: a read again within its window
// calls nothing, and callers at once share one read.

#include "infra/aws_api.h"
#include "infra/aws_xml.h"
#include "infra/infra_status.h"

#include "infra_aws_stub.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace infra = opennova::novaworld_server::infra;
namespace fs = std::filesystem;
using infra_stub::answer;
using infra_stub::AwsStub;

namespace {

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

bool holds_credential(const std::string &text) {
	return contains(text, infra_stub::kSecret) || contains(text, infra_stub::kToken);
}

infra::InfraConfig deployment_config(const std::string &scratch) {
	infra::InfraConfig c;
	c.aws.curl = "/usr/bin/curl";
	c.aws.region = "us-east-1";
	c.aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
	c.aws.scratch_dir = scratch;
	c.cluster = "opennova-prod";
	const std::string tg = infra_stub::kTargetGroupArn;
	c.target_groups = { { "gate", tg + "opennova-prod-gate/0123456789abcdef" },
		{ "lobby", tg + "opennova-prod-lobby/0123456789abcdef" },
		{ "http", tg + "opennova-prod-http/0123456789abcdef" },
		{ "https", tg + "opennova-prod-https/0123456789abcdef" } };
	c.database = "opennova-prod-db";
	c.cache = "opennova-prod-cache";
	c.release_parameter = "/opennova/prod/release";
	return c;
}

bool dir_empty(const std::string &dir) {
	std::error_code ec;
	return fs::is_empty(fs::u8path(dir), ec);
}

int test_xml() {
	infra::XmlNode root;
	std::string error;
	TEST_EXPECT(infra::parse_xml("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- a comment -->\n<a:Root "
								 "xmlns:a=\"urn:x\" b='1>2'>"
								 "<Item>one &amp; &lt;two&gt; &quot;&apos; &#65;&#x42;&#xE9;</Item>"
								 "<Empty/><Item><![CDATA[<raw> & text]]></Item>"
								 "<Nested><Deep><Leaf> trimmed </Leaf></Deep></Nested></a:Root>\n",
			root, error));
	TEST_EXPECT(root.name == "Root");
	TEST_EXPECT(root.children.size() == 4);
	TEST_EXPECT(root.child_text("Item") == "one & <two> \"' AB\xC3\xA9");
	TEST_EXPECT(root.children[1].name == "Empty" && root.children[1].children.empty());
	TEST_EXPECT(root.children[2].text == "<raw> & text");
	TEST_EXPECT(root.at({ "Nested", "Deep" })->child_text("Leaf") == "trimmed");
	TEST_EXPECT(root.at({ "Nested", "Missing" }) == nullptr);
	TEST_EXPECT(root.find("Leaf") != nullptr && root.find("Nowhere") == nullptr);

	const char *const bad[] = {
		"",
		"just text",
		"<a><b></a></b>",
		"<a>",
		"<a></a><b></b>",
		"<a></a>tail",
		"<a>&bogus;</a>",
		"<a>&#xD800;</a>",
		"<a x=1></a>",
		"<a x=\"1></a>",
		"<a><![CDATA[open</a>",
		"<a><!-- open</a>",
	};
	for (const char *text : bad) {
		error.clear();
		if (infra::parse_xml(text, root, error) || error.empty()) {
			std::fprintf(stderr, "  took %s\n", text);
			return 1;
		}
	}
	std::string deep;
	for (size_t i = 0; i <= infra::kMaxXmlDepth; ++i)
		deep += "<d>";
	TEST_EXPECT(!infra::parse_xml(deep, root, error));
	return 0;
}

int test_signed_calls() {
	test_temp::TempDir scratch("infra_signed");
	infra::AwsApiConfig config = deployment_config(scratch.root()).aws;
	TEST_EXPECT(infra::AwsApi::check_config(config).empty());
	{
		infra::AwsApiConfig c = config;
		c.curl = "curl";
		TEST_EXPECT(contains(infra::AwsApi::check_config(c), "not absolute"));
		c = config;
		c.region = "us east";
		TEST_EXPECT(contains(infra::AwsApi::check_config(c), "ONNET_INFRA_REGION"));
		c = config;
		c.credentials_uri.clear();
		TEST_EXPECT(contains(
				infra::AwsApi::check_config(c), "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI is unset"));
		c = config;
		c.credentials_uri = "/v2/../etc";
		TEST_EXPECT(contains(infra::AwsApi::check_config(c), "no path the ECS agent serves"));
		c = config;
		c.credentials_uri = "@evil/v2";
		TEST_EXPECT(!infra::AwsApi::check_config(c).empty());
		c = config;
		c.scratch_dir.clear();
		TEST_EXPECT(contains(infra::AwsApi::check_config(c), "scratch"));
	}

	AwsStub stub;
	stub.replies = infra_stub::deployment_replies();
	infra::AwsApi api(stub, config);

	infra::AwsCredentials creds;
	std::string error;
	TEST_EXPECT(api.credentials(creds, error));
	TEST_EXPECT(creds.key_id == infra_stub::kKeyId && creds.secret == infra_stub::kSecret &&
			creds.token == infra_stub::kToken);
	{
		const auto calls = stub.calls();
		TEST_EXPECT(calls.size() == 1);
		TEST_EXPECT(calls[0].argv.size() == 4 && calls[0].argv[0] == "/usr/bin/curl" &&
				calls[0].argv[1] == "-q" && calls[0].argv[2] == "--config");
		TEST_EXPECT(contains(calls[0].config,
				"url = "
				"\"http://169.254.170.2/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\"\n"));
		TEST_EXPECT(contains(calls[0].config, "proto = \"=http\"\n"));
		TEST_EXPECT(!contains(calls[0].config, "aws-sigv4"));
	}

	stub.clear();
	const infra::AwsReply reply =
			api.json_call(creds, "ecs", "AmazonEC2ContainerServiceV20141113.DescribeServices",
					"{\"cluster\":\"opennova-prod\",\"services\":[\"gate\"]}");
	TEST_EXPECT(
			reply.ok() && reply.status == 200 && contains(reply.body, "\"serviceName\":\"gate\""));
	{
		const auto calls = stub.calls();
		TEST_EXPECT(calls.size() == 1);
		const infra_stub::Call &c = calls[0];
		TEST_EXPECT(c.argv.size() == 4 && c.argv[1] == "-q" && c.argv[2] == "--config");
		for (const std::string &arg : c.argv)
			TEST_EXPECT(!holds_credential(arg) && !contains(arg, infra_stub::kKeyId));
		TEST_EXPECT(contains(c.config, "url = \"https://ecs.us-east-1.amazonaws.com/\"\n"));
		TEST_EXPECT(contains(c.config, "request = \"POST\"\n"));
		TEST_EXPECT(contains(c.config, "aws-sigv4 = \"aws:amz:us-east-1:ecs\"\n"));
		TEST_EXPECT(contains(c.config,
				std::string("user = \"") + infra_stub::kKeyId + ":" + infra_stub::kSecret +
						"\"\n"));
		TEST_EXPECT(contains(c.config, "header = \"Content-Type: application/x-amz-json-1.1\"\n"));
		TEST_EXPECT(contains(c.config,
				"header = \"X-Amz-Target: "
				"AmazonEC2ContainerServiceV20141113.DescribeServices\"\n"));
		TEST_EXPECT(contains(c.config,
				std::string("header = \"x-amz-security-token: ") + infra_stub::kToken + "\"\n"));
		TEST_EXPECT(contains(c.config,
				"data-raw = "
				"\"{\\\"cluster\\\":\\\"opennova-prod\\\",\\\"services\\\":[\\\"gate\\\"]}\"\n"));
		TEST_EXPECT(contains(c.config, "proto = \"=https\"\n"));
		TEST_EXPECT(c.key == "ecs DescribeServices");
		// The file went with its run.
		TEST_EXPECT(!fs::exists(fs::u8path(c.argv[3])));
		TEST_EXPECT(dir_empty(scratch.root()));
	}

	stub.clear();
	TEST_EXPECT(api.query_call(creds, "rds",
						   { { "Action", "DescribeDBInstances" }, { "Version", "2014-10-31" },
								   { "DBInstanceIdentifier", "opennova-prod-db" } })
					.ok());
	{
		const auto calls = stub.calls();
		TEST_EXPECT(calls.size() == 1);
		TEST_EXPECT(contains(calls[0].config, "url = \"https://rds.us-east-1.amazonaws.com/\"\n"));
		TEST_EXPECT(contains(calls[0].config, "aws-sigv4 = \"aws:amz:us-east-1:rds\"\n"));
		TEST_EXPECT(contains(calls[0].config,
				"header = \"Content-Type: application/x-www-form-urlencoded; charset=utf-8\"\n"));
		TEST_EXPECT(!contains(calls[0].config, "X-Amz-Target"));
		TEST_EXPECT(contains(calls[0].config,
				"data-raw = "
				"\"Action=DescribeDBInstances&Version=2014-10-31&DBInstanceIdentifier=opennova-"
				"prod-db\"\n"));
	}
	TEST_EXPECT(infra::AwsApi::query_encode({ { "TargetGroupArn", "arn:aws:x/y z~_.-" } }) ==
			"TargetGroupArn=arn%3Aaws%3Ax%2Fy%20z~_.-");

	// Without a session token, no token header.
	stub.clear();
	infra::AwsCredentials long_lived{ "AKIAEXAMPLEEXAMPLE00", "secret", "" };
	TEST_EXPECT(api.json_call(long_lived, "ssm", "AmazonSSM.GetParameter",
						   "{\"Name\":\"/opennova/prod/release\"}")
					.ok());
	TEST_EXPECT(!contains(stub.calls()[0].config, "x-amz-security-token"));

	// A control byte in a value is refused before curl runs.
	stub.clear();
	const infra::AwsReply refused = api.json_call(creds, "ecs", "X.Y", "{\"a\":\"\n\"}");
	TEST_EXPECT(refused.outcome == infra::AwsReply::Outcome::Unreachable &&
			contains(refused.error, "refused"));
	TEST_EXPECT(stub.calls().empty());

	// AWS's errors in words, never a credential.
	infra::AwsReply denied;
	denied.outcome = infra::AwsReply::Outcome::Answered;
	denied.status = 400;
	denied.body =
			R"({"__type":"com.amazonaws.ecs#AccessDeniedException","Message":"User: arn:aws:sts::123456789012:assumed-role/opennova-prod-web/x is not authorized to perform: ecs:ListTasks"})";
	TEST_EXPECT(infra::AwsApi::failure("ecs", denied) ==
			"ecs answered 400: AccessDeniedException: User: "
			"arn:aws:sts::123456789012:assumed-role/opennova-prod-web/x is "
			"not authorized to perform: ecs:ListTasks");
	denied.status = 403;
	denied.body =
			R"(<ErrorResponse xmlns="http://elasticloadbalancing.amazonaws.com/doc/2015-12-01/"><Error><Type>Sender</Type><Code>AccessDenied</Code><Message>not authorized &amp; denied</Message></Error><RequestId>x</RequestId></ErrorResponse>)";
	TEST_EXPECT(infra::AwsApi::failure("elasticloadbalancing", denied) ==
			"elasticloadbalancing answered 403: AccessDenied: not authorized & denied");
	denied.body =
			R"(<Response><Errors><Error><Code>UnauthorizedOperation</Code><Message>You are not authorized</Message></Error></Errors></Response>)";
	TEST_EXPECT(infra::AwsApi::failure("ec2", denied) ==
			"ec2 answered 403: UnauthorizedOperation: You are not authorized");
	denied.status = 503;
	denied.body = "Service Unavailable";
	TEST_EXPECT(infra::AwsApi::failure("rds", denied) == "rds answered 503");

	// curl failing: its exit code and message, the secret scrubbed.
	stub.replies["ecs DescribeServices"] = infra_stub::curl_failed(
			6, std::string("curl: (6) Could not resolve host ") + infra_stub::kSecret + "\nmore");
	const infra::AwsReply failed = api.json_call(
			creds, "ecs", "AmazonEC2ContainerServiceV20141113.DescribeServices", "{}");
	TEST_EXPECT(failed.outcome == infra::AwsReply::Outcome::Unreachable);
	TEST_EXPECT(failed.error == "curl exited with code 6: curl: (6) Could not resolve host ***");
	TEST_EXPECT(infra::AwsApi::failure("ecs", failed) == "ecs did not answer: " + failed.error);

	// The agent's credentials refused or unreadable.
	stub.replies["credentials"] = answer(404, "{}");
	TEST_EXPECT(!api.credentials(creds, error) && contains(error, "answered 404"));
	TEST_EXPECT(creds.key_id.empty() && creds.secret.empty());
	stub.replies["credentials"] = answer(200, R"({"AccessKeyId":"","SecretAccessKey":"x"})");
	TEST_EXPECT(!api.credentials(creds, error) && contains(error, "no usable credentials"));
	stub.replies["credentials"] = answer(200, "not json");
	TEST_EXPECT(!api.credentials(creds, error) && contains(error, "no JSON object"));
	TEST_EXPECT(dir_empty(scratch.root()));
	return 0;
}

int test_unconfigured() {
	AwsStub stub;
	infra::InfraConfig config; // the local stack's: nothing set
	infra::InfraStatus status(config, stub);
	TEST_EXPECT(!status.configured());
	const auto snap = status.snapshot({ "i-0aaaaaaaaaaaaaaa1" });
	TEST_EXPECT(snap && contains(snap->unavailable, "ONNET_INFRA_CLUSTER is unset"));
	TEST_EXPECT(snap->services.empty() && snap->tasks.empty() && snap->target_groups.empty() &&
			!snap->database);
	TEST_EXPECT(stub.calls().empty());

	// A cluster but no task role (off ECS): still nothing called.
	test_temp::TempDir scratch("infra_unconfigured");
	infra::InfraConfig no_role = deployment_config(scratch.root());
	no_role.aws.credentials_uri.clear();
	infra::InfraStatus status2(no_role, stub);
	TEST_EXPECT(!status2.configured());
	TEST_EXPECT(
			contains(status2.snapshot({})->unavailable, "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI"));
	TEST_EXPECT(stub.calls().empty());
	TEST_EXPECT(contains(infra::check_infra_config(deployment_config("")), "scratch"));
	infra::InfraConfig bad_group = deployment_config(scratch.root());
	bad_group.target_groups.push_back({ "x", "not-an-arn" });
	TEST_EXPECT(contains(infra::check_infra_config(bad_group), "ONNET_INFRA_TARGET_GROUPS"));
	return 0;
}

int test_snapshot() {
	test_temp::TempDir scratch("infra_snapshot");
	AwsStub stub;
	stub.replies = infra_stub::deployment_replies();
	int64_t clock = 1000;
	infra::InfraStatus status(deployment_config(scratch.root()), stub, [&clock] { return clock; });
	TEST_EXPECT(status.configured());
	const auto snap = status.snapshot({ "i-0aaaaaaaaaaaaaaa1", "docker-7", "i-bad id" });
	TEST_EXPECT(snap->unavailable.empty() && snap->error.empty());
	TEST_EXPECT(
			snap->cluster == "opennova-prod" && snap->region == "us-east-1" && snap->read_ms > 0);

	// Every call once, the credentials first among them.
	const auto calls = stub.calls();
	TEST_EXPECT(calls.size() == 13);
	TEST_EXPECT(calls.front().key == "credentials");
	for (const char *key : { "ecs DescribeServices", "ecs ListTasks RUNNING",
				 "ecs ListTasks STOPPED", "ecs DescribeTasks", "ssm GetParameter",
				 "elasticloadbalancing DescribeTargetHealth opennova-prod-gate",
				 "elasticloadbalancing DescribeTargetHealth opennova-prod-lobby",
				 "elasticloadbalancing DescribeTargetHealth opennova-prod-http",
				 "elasticloadbalancing DescribeTargetHealth opennova-prod-https",
				 "rds DescribeDBInstances", "elasticache DescribeReplicationGroups",
				 "ec2 DescribeInstances" })
		if (stub.count(key) != 1) {
			std::fprintf(stderr, "  %s made %zu times\n", key, stub.count(key));
			return 1;
		}
	for (const infra_stub::Call &c : calls) {
		if (c.key == "ecs DescribeServices")
			TEST_EXPECT(contains(c.config,
					R"(\"services\":[\"gate\",\"lobby\",\"web\",\"controller\",\"worker\"])"));
		if (c.key == "ecs DescribeTasks")
			TEST_EXPECT(contains(c.config, "9999ffff9999ffff9999ffff9999ffff") &&
					contains(c.config, "8888bbbb"));
		if (c.key == "ec2 DescribeInstances")
			// The rows' instance ids alone: not the docker machine's, not a malformed one.
			TEST_EXPECT(
					contains(c.config,
							"Filter.1.Name=instance-id&Filter.1.Value.1=i-0aaaaaaaaaaaaaaa1\"") &&
					!contains(c.config, "docker"));
		for (const std::string &arg : c.argv)
			TEST_EXPECT(!holds_credential(arg));
	}
	TEST_EXPECT(dir_empty(scratch.root()));

	// The programs, in their order.
	TEST_EXPECT(snap->services.size() == 5 && snap->missing_services.empty() &&
			snap->services_error.empty());
	const infra::EcsService &gate = snap->services[0];
	TEST_EXPECT(gate.name == "gate" && gate.status == "ACTIVE" && gate.desired == 1 &&
			gate.running == 1 && gate.pending == 0);
	TEST_EXPECT(gate.task_definition_arn ==
			"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-gate:7");
	TEST_EXPECT(gate.deployments.size() == 1 && gate.deployments[0].rollout_state == "COMPLETED" &&
			gate.deployments[0].created_at == "2026-10-17T10:40:00Z" &&
			gate.deployments[0].updated_at == "2026-10-17T10:45:00Z");
	TEST_EXPECT(gate.events.size() == 3 &&
			gate.events[0].message == "(service gate) has reached a steady state." &&
			gate.events[0].at == "2026-10-17T11:20:00Z");
	const infra::EcsService &web = snap->services[2];
	TEST_EXPECT(web.name == "web" && web.desired == 2 && web.running == 1 && web.pending == 1);
	TEST_EXPECT(web.deployments.size() == 2 && web.deployments[0].status == "PRIMARY" &&
			web.deployments[0].rollout_state == "IN_PROGRESS" && web.deployments[0].failed == 1 &&
			web.deployments[1].status == "ACTIVE");
	TEST_EXPECT(snap->services[4].name == "worker");

	// The tasks: running first by start (the pending one by its creation), then stopped.
	TEST_EXPECT(snap->tasks_error.empty());
	TEST_EXPECT(snap->tasks.size() == 8);
	TEST_EXPECT(snap->tasks[0].id == "1111aaaa1111aaaa1111aaaa1111aaaa" &&
			snap->tasks[0].program == "gate");
	TEST_EXPECT(
			snap->tasks[5].program == "" && snap->tasks[5].group == "family:opennova-prod-backup");
	TEST_EXPECT(snap->tasks[6].id == "3333cccc3333cccc3333cccc3333cccc" &&
			snap->tasks[6].last_status == "PENDING" && snap->tasks[6].started_at.empty() &&
			snap->tasks[6].private_ip == "10.40.1.13" && snap->tasks[6].containers.size() == 2 &&
			snap->tasks[6].containers[0].image_digest.empty());
	const infra::EcsTask &stopped = snap->tasks[7];
	TEST_EXPECT(stopped.id == "9999ffff9999ffff9999ffff9999ffff" &&
			stopped.desired_status == "STOPPED");
	TEST_EXPECT(stopped.stopped_reason == "Essential container in task exited" &&
			stopped.stop_code == "EssentialContainerExited");
	TEST_EXPECT(stopped.stopped_at == "2026-10-17T12:00:30Z" && stopped.private_ip == "10.40.0.19");
	TEST_EXPECT(stopped.containers.size() == 1 && stopped.containers[0].exit_code &&
			*stopped.containers[0].exit_code == 1);
	const infra::EcsTask &web_old = snap->tasks[2];
	TEST_EXPECT(web_old.program == "web" && web_old.health == "HEALTHY" &&
			web_old.zone == "us-east-1a" && web_old.private_ip == "10.40.0.14" &&
			web_old.cpu == "512" && web_old.memory == "1024" &&
			web_old.started_at == "2026-10-17T10:40:20Z");
	TEST_EXPECT(web_old.containers.size() == 1 && web_old.containers[0].name == "web" &&
			web_old.containers[0].image_digest ==
					"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" &&
			!web_old.containers[0].exit_code);

	// The balancer, in its order.
	TEST_EXPECT(snap->target_groups.size() == 4);
	TEST_EXPECT(snap->target_groups[0].key == "gate" &&
			snap->target_groups[0].targets.size() == 1 &&
			snap->target_groups[0].targets[0].id == "10.40.0.11" &&
			snap->target_groups[0].targets[0].port == 7597 &&
			snap->target_groups[0].targets[0].state == "healthy" &&
			snap->target_groups[0].targets[0].zone == "us-east-1a");
	const infra::TargetGroupHealth &https = snap->target_groups[3];
	TEST_EXPECT(https.key == "https" && https.error.empty() && https.targets.size() == 2);
	TEST_EXPECT(https.targets[1].state == "unhealthy" &&
			https.targets[1].reason == "Target.FailedHealthChecks" &&
			https.targets[1].description == "Health checks failed & the target is <draining>");
	TEST_EXPECT(snap->target_groups[2].targets[1].state == "initial" &&
			snap->target_groups[2].targets[1].reason == "Elb.RegistrationInProgress");

	// The database, the cache, the release, the machine.
	TEST_EXPECT(snap->database && snap->database_error.empty());
	TEST_EXPECT(snap->database->identifier == "opennova-prod-db" &&
			snap->database->status == "available" && snap->database->multi_az &&
			snap->database->instance_class == "db.t4g.small" &&
			snap->database->engine == "postgres" && snap->database->engine_version == "17.4" &&
			snap->database->zone == "us-east-1a" &&
			snap->database->secondary_zone == "us-east-1b" && snap->database->storage_gb == 20 &&
			snap->database->storage_type == "gp3");
	TEST_EXPECT(snap->cache && snap->cache->id == "opennova-prod-cache" &&
			snap->cache->status == "available" && snap->cache->multi_az == "enabled" &&
			snap->cache->automatic_failover == "enabled" &&
			snap->cache->node_type == "cache.t4g.micro" && snap->cache->engine == "valkey");
	TEST_EXPECT(snap->cache->nodes.size() == 2 &&
			snap->cache->nodes[0].id == "opennova-prod-cache-001" &&
			snap->cache->nodes[0].role == "primary" && snap->cache->nodes[1].zone == "us-east-1b");
	TEST_EXPECT(snap->release && !snap->release_none && snap->release_error.empty());
	TEST_EXPECT(snap->release->id == "20261017T120000Z-abc1234" &&
			snap->release->created == "2026-10-17T12:00:00Z" &&
			snap->release->revision == "abc1234def56");
	TEST_EXPECT(snap->release->task_definitions.size() == 6 &&
			snap->release->task_definitions.at("web") ==
					"arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:42");
	TEST_EXPECT(snap->machines.size() == 1 && snap->machines_error.empty());
	const infra::MachineInstance &m = snap->machines.at("i-0aaaaaaaaaaaaaaa1");
	TEST_EXPECT(m.state == "running" && m.zone == "us-east-1b" && m.type == "t3a.large");

	// Within the window the same read answers, calling nothing.
	stub.clear();
	clock += 19 * 1000;
	TEST_EXPECT(status.snapshot({}) == snap);
	TEST_EXPECT(stub.calls().empty());
	// Past it, a read of its own.
	clock += 2 * 1000;
	const auto again = status.snapshot({});
	TEST_EXPECT(again != snap && stub.count("ecs DescribeServices") == 1);
	// No machine to ask about: no EC2 call.
	TEST_EXPECT(stub.count("ec2 DescribeInstances") == 0 && again->machines.empty());
	return 0;
}

int test_failures() {
	test_temp::TempDir scratch("infra_failures");
	AwsStub stub;
	stub.replies = infra_stub::deployment_replies();
	// RDS refuses, ECS reports a missing service, one target group's call fails,
	// no release yet.
	stub.replies["rds DescribeDBInstances"] = answer(403,
			R"(<ErrorResponse><Error><Type>Sender</Type><Code>AccessDenied</Code><Message>User is not authorized to perform: rds:DescribeDBInstances</Message></Error></ErrorResponse>)");
	stub.replies["ecs DescribeServices"] = answer(200,
			R"({"services":[{"serviceName":"gate","status":"ACTIVE","desiredCount":1,"runningCount":0,"pendingCount":0,"deployments":[],"events":[]}],)"
			R"("failures":[{"arn":"arn:aws:ecs:us-east-1:123456789012:service/opennova-prod/lobby","reason":"MISSING"}]})");
	stub.replies.erase("elasticloadbalancing DescribeTargetHealth opennova-prod-lobby");
	stub.replies["ssm GetParameter"] =
			answer(200, R"({"Parameter":{"Name":"/opennova/prod/release","Value":"none"}})");
	infra::InfraStatus status(deployment_config(scratch.root()), stub);
	const auto snap = status.snapshot({});
	TEST_EXPECT(snap->error.empty());
	TEST_EXPECT(!snap->database &&
			snap->database_error ==
					"rds answered 403: AccessDenied: User is not authorized to perform: "
					"rds:DescribeDBInstances");
	TEST_EXPECT(snap->services.size() == 1 && snap->missing_services.size() == 1 &&
			snap->missing_services[0] == "lobby (MISSING)");
	TEST_EXPECT(snap->target_groups.size() == 4 && snap->target_groups[1].targets.empty() &&
			contains(snap->target_groups[1].error,
					"elasticloadbalancing did not answer: curl exited with code 7"));
	TEST_EXPECT(snap->target_groups[3].error.empty() && snap->target_groups[3].targets.size() == 2);
	TEST_EXPECT(!snap->release && snap->release_none && snap->release_error.empty());
	// The others stand.
	TEST_EXPECT(snap->cache && snap->tasks.size() == 8);
	for (const std::string &e : { snap->database_error, snap->target_groups[1].error })
		TEST_EXPECT(!holds_credential(e));

	// A release from before the programs names no revision.
	stub.replies["ssm GetParameter"] = answer(200,
			R"({"Parameter":{"Name":"/opennova/prod/release","Value":"20261001T000000Z-single"}})");
	infra::InfraConfig quick = deployment_config(scratch.root());
	quick.cache_ms = 0;
	infra::InfraStatus status2(quick, stub);
	const auto old_release = status2.snapshot({});
	TEST_EXPECT(old_release->release && old_release->release->id == "20261001T000000Z-single" &&
			old_release->release->task_definitions.empty());

	// The credentials failing stop the read: nothing else is called.
	stub.clear();
	stub.replies["credentials"] =
			infra_stub::curl_failed(7, "curl: (7) Failed to connect to 169.254.170.2 port 80");
	const auto none = status2.snapshot({ "i-0aaaaaaaaaaaaaaa1" });
	TEST_EXPECT(contains(none->error,
			"the ECS agent's credentials endpoint did not answer: curl exited with code 7"));
	TEST_EXPECT(none->services.empty() && none->tasks.empty() && !none->database && !none->cache);
	TEST_EXPECT(stub.calls().size() == 1);

	// Sections left unset are not read, and say so.
	stub.replies = infra_stub::deployment_replies();
	infra::InfraConfig bare = deployment_config(scratch.root());
	bare.database.clear();
	bare.cache.clear();
	bare.release_parameter.clear();
	bare.target_groups.clear();
	stub.clear();
	infra::InfraStatus status3(bare, stub);
	const auto partial = status3.snapshot({});
	TEST_EXPECT(contains(partial->database_error, "ONNET_INFRA_DATABASE is unset") &&
			contains(partial->cache_error, "ONNET_INFRA_CACHE is unset") &&
			contains(partial->release_error, "ONNET_INFRA_RELEASE_PARAMETER is unset"));
	TEST_EXPECT(partial->target_groups.empty() && stub.count("rds DescribeDBInstances") == 0 &&
			stub.count("ssm GetParameter") == 0 && stub.calls().size() == 5);
	return 0;
}

// A runner whose credentials call holds until released, the rest answering at once.
class HeldStub : public AwsStub {
public:
	opennova::proc::CommandResult run(
			const std::vector<std::string> &argv, int timeout_ms) override {
		if (infra_stub::key_of(read_config(argv)) == "credentials") {
			std::unique_lock<std::mutex> lock(gate_mu_);
			++waiting_;
			gate_cv_.notify_all();
			gate_cv_.wait(lock, [this] { return open_; });
		}
		return AwsStub::run(argv, timeout_ms);
	}
	void wait_for_one() {
		std::unique_lock<std::mutex> lock(gate_mu_);
		gate_cv_.wait(lock, [this] { return waiting_ > 0; });
	}
	void open() {
		std::lock_guard<std::mutex> lock(gate_mu_);
		open_ = true;
		gate_cv_.notify_all();
	}

private:
	static std::string read_config(const std::vector<std::string> &argv) {
		if (argv.size() != 4)
			return {};
		std::ifstream in(fs::u8path(argv[3]), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	std::mutex gate_mu_;
	std::condition_variable gate_cv_;
	int waiting_ = 0;
	bool open_ = false;
};

int test_shared_read() {
	test_temp::TempDir scratch("infra_shared");
	HeldStub stub;
	stub.replies = infra_stub::deployment_replies();
	infra::InfraStatus status(deployment_config(scratch.root()), stub);
	std::shared_ptr<const infra::CloudSnapshot> first;
	std::shared_ptr<const infra::CloudSnapshot> second;
	std::thread a([&] { first = status.snapshot({}); });
	stub.wait_for_one();
	std::thread b([&] { second = status.snapshot({}); });
	// b finds a's read in flight and waits for it.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	stub.open();
	a.join();
	b.join();
	TEST_EXPECT(first && first == second);
	TEST_EXPECT(stub.count("credentials") == 1 && stub.count("ecs DescribeServices") == 1);
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "xml", test_xml },
		{ "signed_calls", test_signed_calls },
		{ "unconfigured", test_unconfigured },
		{ "snapshot", test_snapshot },
		{ "failures", test_failures },
		{ "shared_read", test_shared_read },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the infrastructure view's AWS reads\n");
	return 0;
}
