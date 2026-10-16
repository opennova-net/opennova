// What an ECS task finds from its own metadata (apps/novaworld_common/ecs_metadata.h):
// the lobby's endpoint and the controller's internal listener bind, against a fake
// task metadata endpoint (an HTTP server in this test on loopback, answering
// GET <uri>/task) and a stub CommandRunner that checks describe-network-interfaces'
// argv and answers it as the AWS CLI does.
//
//   - a configured ONNET_LOBBY_ENDPOINT stands as it is (neither the metadata nor the
//     CLI asked), and one that is no <host>:<port> is refused;
//   - no metadata: the lobby refuses to boot, the controller keeps the environment's bind;
//   - the metadata: the lobby announces its interface's public IPv4 with its port (the
//     CLI asked in the task ARN's region, by the private address and the MAC), the
//     controller binds the private IPv4 at the internal port;
//   - each refusal: the endpoint answering an error or not at all, a body that names no
//     address or no task ARN, the CLI failing, no interface or two, an interface with no
//     public address, a port of 0.
//
// Database-free.

#include "ecs_metadata.h"
#include "fake_ecs_metadata.h"

#include "common/test_expect.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ecs = opennova::novaworld_server::ecs;
namespace net = opennova::net;
namespace proc = opennova::proc;

using fake_ecs::FakeMetadata;
using fake_ecs::kInterfaces;
using fake_ecs::kTask;

namespace {

// The AWS CLI as a stub: every argv kept, one canned result.
struct StubCli : proc::CommandRunner {
	proc::CommandResult result;
	std::vector<std::vector<std::string>> calls;
	proc::CommandResult run(const std::vector<std::string> &argv, int) override {
		calls.push_back(argv);
		return result;
	}
	static StubCli answering(
			const std::string &out, uint32_t exit_code = 0, const std::string &err = {}) {
		StubCli cli;
		cli.result.outcome = proc::CommandResult::Outcome::Exited;
		cli.result.exit_code = exit_code;
		cli.result.out = out;
		cli.result.err = err;
		return cli;
	}
};

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

int exercise_configured() {
	StubCli cli = StubCli::answering(kInterfaces);
	FakeMetadata metadata(200, kTask);
	ecs::Sources sources;
	sources.configured = "203.0.113.9:64206";
	sources.metadata_uri = metadata.uri();
	sources.port = 64206;
	std::string error;
	TEST_EXPECT(ecs::lobby_endpoint(sources, cli, error) == "203.0.113.9:64206" && error.empty());
	TEST_EXPECT(cli.calls.empty() && metadata.requests() == 0);
	sources.configured = "no-port";
	TEST_EXPECT(ecs::lobby_endpoint(sources, cli, error).empty());
	TEST_EXPECT(contains(error, "ONNET_LOBBY_ENDPOINT is no <host>:<port>"));
	// The controller with ONNET_INTERNAL_HTTP_BIND set: the environment's stands.
	sources.configured = "10.0.0.5:8070";
	error.clear();
	TEST_EXPECT(ecs::internal_bind(sources, error).empty() && error.empty());
	TEST_EXPECT(metadata.requests() == 0);
	std::printf("  configured: the endpoint as given, the environment's bind stands\n");
	return 0;
}

int exercise_no_metadata() {
	StubCli cli = StubCli::answering(kInterfaces);
	ecs::Sources sources;
	sources.port = 64206;
	std::string error;
	TEST_EXPECT(ecs::lobby_endpoint(sources, cli, error).empty());
	TEST_EXPECT(contains(error, "the lobby needs ONNET_LOBBY_ENDPOINT") &&
			contains(error, ecs::kMetadataUriVariable));
	TEST_EXPECT(cli.calls.empty());
	error.clear();
	sources.port = 8070;
	TEST_EXPECT(ecs::internal_bind(sources, error).empty() && error.empty());
	std::printf("  no metadata: the lobby refused, the controller's bind the environment's\n");
	return 0;
}

int exercise_discovered() {
	StubCli cli = StubCli::answering(kInterfaces);
	FakeMetadata metadata(200, kTask);
	ecs::Sources sources;
	sources.metadata_uri = metadata.uri();
	sources.port = 64206;
	sources.aws_cli = "/usr/local/bin/aws";
	std::string error;
	TEST_EXPECT(ecs::lobby_endpoint(sources, cli, error) == "3.91.20.7:64206");
	if (!error.empty())
		std::fprintf(stderr, "  %s\n", error.c_str());
	TEST_EXPECT(error.empty());
	TEST_EXPECT(metadata.last_line() == "GET /v4/abc-123/task HTTP/1.0");
	TEST_EXPECT(cli.calls.size() == 1);
	const std::vector<std::string> expected = { "/usr/local/bin/aws", "ec2",
		"describe-network-interfaces", "--region", "us-east-1", "--filters",
		"Name=addresses.private-ip-address,Values=10.44.1.37",
		"Name=mac-address,Values=0e:9e:32:c7:48:85", "--output", "json", "--no-cli-pager",
		"--cli-connect-timeout", "10", "--cli-read-timeout", "30" };
	if (cli.calls.front() != expected)
		for (const std::string &arg : cli.calls.front())
			std::fprintf(stderr, "  argv: %s\n", arg.c_str());
	TEST_EXPECT(cli.calls.front() == expected);

	// The controller: its private IPv4 at the internal port, no CLI.
	sources.port = 8070;
	TEST_EXPECT(ecs::internal_bind(sources, error) == "10.44.1.37:8070" && error.empty());
	TEST_EXPECT(cli.calls.size() == 1);

	// A task whose network names no MAC address: the address alone.
	const std::string no_mac = std::string(kTask).replace(std::string(kTask).find("\"MACAddress\""),
			std::string("\"MACAddress\": \"0e:9e:32:c7:48:85\",").size(), "");
	FakeMetadata metadata_no_mac(200, no_mac);
	StubCli cli2 = StubCli::answering(kInterfaces);
	sources.metadata_uri = metadata_no_mac.uri();
	sources.port = 64206;
	TEST_EXPECT(ecs::lobby_endpoint(sources, cli2, error) == "3.91.20.7:64206");
	TEST_EXPECT(cli2.calls.size() == 1 && cli2.calls.front().size() == expected.size() - 1);
	std::printf("  discovered: the lobby at 3.91.20.7:64206, the controller at 10.44.1.37:8070\n");
	return 0;
}

// A lobby_endpoint that fails with `expect` in its error.
int refused(FakeMetadata &metadata, StubCli &cli, const char *expect, uint16_t port = 64206) {
	ecs::Sources sources;
	sources.metadata_uri = metadata.uri();
	sources.port = port;
	sources.metadata_timeout_ms = 1000;
	std::string error;
	TEST_EXPECT(ecs::lobby_endpoint(sources, cli, error).empty());
	if (!contains(error, expect))
		std::fprintf(stderr, "  error '%s', expected '%s'\n", error.c_str(), expect);
	TEST_EXPECT(contains(error, expect));
	return 0;
}

int exercise_refusals() {
	{
		FakeMetadata metadata(500, "{}");
		StubCli cli = StubCli::answering(kInterfaces);
		if (refused(metadata, cli, "answers HTTP 500") != 0)
			return 1;
		ecs::Sources sources;
		sources.metadata_uri = metadata.uri();
		sources.port = 8070;
		std::string error;
		TEST_EXPECT(ecs::internal_bind(sources, error).empty() && contains(error, "HTTP 500"));
	}
	{
		// A port nothing listens on: the endpoint does not answer.
		uint16_t closed = 0;
		net::Socket probe = net::tcp_listen(0, 1, &closed, true);
		net::close_socket(probe);
		StubCli cli = StubCli::answering(kInterfaces);
		ecs::Sources sources;
		sources.metadata_uri = "http://127.0.0.1:" + std::to_string(closed) + "/v4/x";
		sources.port = 64206;
		sources.metadata_timeout_ms = 1000;
		std::string error;
		TEST_EXPECT(ecs::lobby_endpoint(sources, cli, error).empty());
		TEST_EXPECT(contains(error, "does not answer") && cli.calls.empty());
		sources.metadata_uri = "https://169.254.170.2/v4/x";
		TEST_EXPECT(ecs::lobby_endpoint(sources, cli, error).empty());
		TEST_EXPECT(contains(error, "is no http:// URI"));
	}
	{
		FakeMetadata metadata(200, "not json");
		StubCli cli = StubCli::answering(kInterfaces);
		if (refused(metadata, cli, "is no JSON") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200,
				R"({"TaskARN":"arn:aws:ecs:us-east-1:1:task/c/i","Containers":[{"Networks":[]}]})");
		StubCli cli = StubCli::answering(kInterfaces);
		if (refused(metadata, cli, "names no IPv4 address") != 0)
			return 1;
		TEST_EXPECT(cli.calls.empty());
	}
	{
		FakeMetadata metadata(
				200, R"({"Containers":[{"Networks":[{"IPv4Addresses":["10.0.0.1"]}]}]})");
		StubCli cli = StubCli::answering(kInterfaces);
		if (refused(metadata, cli, "no task ARN with a region") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, kTask);
		StubCli cli = StubCli::answering("", 255, "An error occurred (UnauthorizedOperation)");
		if (refused(metadata, cli,
					"describe-network-interfaces failed (exit 255: An error "
					"occurred (UnauthorizedOperation)") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, kTask);
		StubCli cli;
		cli.result.outcome = proc::CommandResult::Outcome::NotStarted;
		cli.result.error = "no such file";
		if (refused(metadata, cli, "describe-network-interfaces failed (no such file)") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, kTask);
		StubCli cli = StubCli::answering(R"({"NetworkInterfaces":[]})");
		if (refused(metadata, cli, "names 0 interfaces at 10.44.1.37, not one") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, kTask);
		StubCli cli = StubCli::answering(
				R"({"NetworkInterfaces":[{"Association":{"PublicIp":"3.3.3.3"}},{"Association":{"PublicIp":"4.4.4.4"}}]})");
		if (refused(metadata, cli, "names 2 interfaces") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, kTask);
		StubCli cli = StubCli::answering(
				R"({"NetworkInterfaces":[{"NetworkInterfaceId":"eni-1","PrivateIpAddress":"10.44.1.37"}]})");
		if (refused(metadata, cli, "has no public IPv4 address") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, kTask);
		StubCli cli = StubCli::answering("{\"NetworkInterfaces\": [ oops");
		if (refused(metadata, cli, "answered no JSON") != 0)
			return 1;
	}
	{
		FakeMetadata metadata(200, kTask);
		StubCli cli = StubCli::answering(kInterfaces);
		if (refused(metadata, cli, "ONNET_NW_UDP_PORT, which is 0", 0) != 0)
			return 1;
		TEST_EXPECT(metadata.requests() == 0);
	}
	std::printf("  refusals: each with its reason\n");
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(net::startup() == 0);
	int rc = exercise_configured();
	if (rc == 0)
		rc = exercise_no_metadata();
	if (rc == 0)
		rc = exercise_discovered();
	if (rc == 0)
		rc = exercise_refusals();
	net::shutdown();
	if (rc == 0)
		std::printf("OK: an ECS task's own addresses\n");
	return rc;
}
