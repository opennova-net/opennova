// What an ECS task finds from its own metadata (apps/novaworld_common/ecs_metadata.h):
// the fleet's internal listener bind and a task's revision, against a fake task
// metadata endpoint (an HTTP server in this test on loopback, answering GET <uri>/task
// with a Fargate task's reply).
//
//   - a configured ONNET_INTERNAL_HTTP_BIND stands (the metadata never asked);
//   - no metadata: the environment's bind stands;
//   - the metadata: the task's private IPv4 at the internal port, and its task
//     definition's family and revision;
//   - each refusal: the endpoint answering an error or not at all, a URI that is no
//     http://, a body that is no JSON or names no address, no family or no revision;
//   - the task's facts (program_usage.h): its id from its ARN and its Limits, none
//     without them, the refusals.
//
// Database-free.

#include "ecs_metadata.h"

#include "common/http_reply_server.h"
#include "common/test_expect.h"

#include <net_sockets.h>

#include <cstdio>
#include <string>

namespace ecs = opennova::novaworld_server::ecs;
namespace net = opennova::net;

namespace {

// GET <uri>/task's reply for a Fargate task (awsvpc): its interface's private IPv4
// 10.44.1.37.
constexpr const char *kTask = R"({
  "Cluster": "arn:aws:ecs:us-east-1:111122223333:cluster/opennova-prod",
  "TaskARN": "arn:aws:ecs:us-east-1:111122223333:task/opennova-prod/0123456789abcdef",
  "Family": "opennova-prod-fleet",
  "Revision": "12",
  "Containers": [
    {
      "Name": "fleet",
      "Networks": [
        {
          "NetworkMode": "awsvpc",
          "IPv4Addresses": ["10.44.1.37"],
          "AttachmentIndex": 0,
          "MACAddress": "0e:9e:32:c7:48:85",
          "IPv4SubnetCIDRBlock": "10.44.1.0/24",
          "PrivateDNSName": "ip-10-44-1-37.ec2.internal",
          "SubnetGatewayIpv4Address": "10.44.1.1/24"
        }
      ]
    }
  ],
  "AvailabilityZone": "us-east-1b",
  "Limits": {"CPU": 0.25, "Memory": 512},
  "LaunchType": "FARGATE"
})";

// The task metadata endpoint: uri() is what ECS_CONTAINER_METADATA_URI_V4 would
// hold, its GET <uri>/task answered with the canned body.
class FakeMetadata : public test_http::ReplyServer {
public:
	using ReplyServer::ReplyServer;
	std::string uri() const { return base_url() + "/v4/abc-123"; }
};

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

int exercise_environment() {
	FakeMetadata metadata(200, kTask);
	ecs::Sources sources;
	sources.configured = "10.0.0.5:8070";
	sources.metadata_uri = metadata.uri();
	sources.port = 8070;
	std::string error;
	TEST_EXPECT(ecs::internal_bind(sources, error).empty() && error.empty());
	TEST_EXPECT(metadata.requests() == 0);
	sources.configured.clear();
	sources.metadata_uri.clear();
	TEST_EXPECT(ecs::internal_bind(sources, error).empty() && error.empty());
	std::printf("  the environment's bind stands: configured, or not on ECS\n");
	return 0;
}

int exercise_discovered() {
	FakeMetadata metadata(200, kTask);
	ecs::Sources sources;
	sources.metadata_uri = metadata.uri();
	sources.port = 8070;
	std::string error;
	TEST_EXPECT(ecs::internal_bind(sources, error) == "10.44.1.37:8070" && error.empty());
	TEST_EXPECT(metadata.last_line() == "GET /v4/abc-123/task HTTP/1.0");
	std::printf("  discovered: the fleet binds 10.44.1.37:8070\n");
	return 0;
}

// An internal_bind from `uri` that fails with `expect` in its error.
int refused(const std::string &uri, const char *expect) {
	ecs::Sources sources;
	sources.metadata_uri = uri;
	sources.port = 8070;
	sources.metadata_timeout_ms = 1000;
	std::string error;
	TEST_EXPECT(ecs::internal_bind(sources, error).empty());
	if (!contains(error, expect))
		std::fprintf(stderr, "  error '%s', expected '%s'\n", error.c_str(), expect);
	TEST_EXPECT(contains(error, expect));
	return 0;
}

int exercise_refusals() {
	{
		FakeMetadata metadata(500, "{}");
		if (refused(metadata.uri(), "answers HTTP 500") != 0)
			return 1;
	}
	{
		// A port nothing listens on: the endpoint does not answer.
		uint16_t closed = 0;
		net::Socket probe = net::tcp_listen(0, 1, &closed, true);
		net::close_socket(probe);
		if (refused("http://127.0.0.1:" + std::to_string(closed) + "/v4/x", "does not answer") != 0)
			return 1;
		if (refused("https://169.254.170.2/v4/x", "is no http:// URI") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, "not json");
		if (refused(metadata.uri(), "is no JSON") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, R"({"Containers":[{"Networks":[]}]})");
		if (refused(metadata.uri(), "names no IPv4 address") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, R"({"TaskARN":"x"})");
		if (refused(metadata.uri(), "lists no containers") != 0)
			return 1;
	}
	std::printf("  refusals: each with its reason\n");
	return 0;
}

// The task's definition: its family and revision, alike in every task of one
// revision; a reply that names either not is refused.
int exercise_revision() {
	FakeMetadata metadata(200, kTask);
	std::string revision;
	std::string error;
	TEST_EXPECT(ecs::read_task_revision(metadata.uri(), revision, error, 1000));
	TEST_EXPECT(revision == "opennova-prod-fleet:12" && error.empty());
	TEST_EXPECT(metadata.last_line() == "GET /v4/abc-123/task HTTP/1.0");
	TEST_EXPECT(!ecs::parse_task_revision(R"({"Family":"opennova-prod-fleet"})", revision, error));
	TEST_EXPECT(contains(error, "no task definition family and revision"));
	TEST_EXPECT(!ecs::parse_task_revision(R"({"Family":"","Revision":"3"})", revision, error));
	TEST_EXPECT(
			!ecs::parse_task_revision("not json", revision, error) && contains(error, "is no JSON"));
	FakeMetadata failing(500, "{}");
	TEST_EXPECT(!ecs::read_task_revision(failing.uri(), revision, error, 1000));
	TEST_EXPECT(contains(error, "answers HTTP 500"));
	std::printf("  the task's revision: opennova-prod-fleet:12, a reply without one refused\n");
	return 0;
}

int exercise_task_facts() {
	FakeMetadata metadata(200, kTask);
	ecs::TaskFacts facts;
	std::string error;
	TEST_EXPECT(ecs::read_task_facts(metadata.uri(), facts, error, 1000));
	TEST_EXPECT(facts.id == "0123456789abcdef" && facts.cpu == 0.25 && facts.memory_mib == 512);
	TEST_EXPECT(metadata.last_line() == "GET /v4/abc-123/task HTTP/1.0");
	// No Limits (an EC2 task's): the id alone.
	TEST_EXPECT(ecs::parse_task_facts(
			R"({"TaskARN":"arn:aws:ecs:us-east-1:1:task/c/feed"})", facts, error));
	TEST_EXPECT(facts.id == "feed" && facts.cpu == 0 && facts.memory_mib == 0);
	TEST_EXPECT(!ecs::parse_task_facts(R"({"Limits":{"CPU":1}})", facts, error) &&
			contains(error, "names no task ARN"));
	TEST_EXPECT(!ecs::parse_task_facts(R"({"TaskARN":"arn:aws:ecs:x:task/c/"})", facts, error));
	TEST_EXPECT(!ecs::parse_task_facts("[", facts, error) && contains(error, "is no JSON"));
	FakeMetadata failing(404, "{}");
	TEST_EXPECT(!ecs::read_task_facts(failing.uri(), facts, error, 1000) &&
			contains(error, "answers HTTP 404"));
	std::printf("  the task's facts: its id and Limits\n");
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(net::startup() == 0);
	int rc = exercise_environment();
	if (rc == 0)
		rc = exercise_discovered();
	if (rc == 0)
		rc = exercise_refusals();
	if (rc == 0)
		rc = exercise_revision();
	if (rc == 0)
		rc = exercise_task_facts();
	net::shutdown();
	if (rc == 0)
		std::printf("OK: an ECS task's own address, revision and facts\n");
	return rc;
}
