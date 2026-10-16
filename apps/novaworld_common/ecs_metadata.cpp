#include "ecs_metadata.h"

#include "loggable.h"
#include "program.h"

#include <base/io/json.h>
#include <net_http.h>
#include <net_sockets.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::ecs {

namespace {

// The AWS CLI's run, bounded: its own connect and read timeouts, with its
// retries (AWS_MAX_ATTEMPTS) inside this.
constexpr int kCliTimeoutMs = 30 * 1000;

const io::JsonValue *member(
		const io::JsonValue &object, const char *key, io::JsonValue::Type type) {
	const io::JsonValue *v = object.is_object() ? object.get(key) : nullptr;
	return v != nullptr && v->type == type ? v : nullptr;
}

std::string text_member(const io::JsonValue &object, const char *key) {
	const io::JsonValue *v = member(object, key, io::JsonValue::Type::String);
	return v != nullptr ? v->string : std::string();
}

bool is_ipv4(const std::string &text) {
	net::Endpoint ep;
	return !text.empty() && net::resolve_ipv4(text, ep, /*allow_names=*/false);
}

// A region name as an ARN carries it: lowercase letters, digits and '-'.
bool is_region(std::string_view text) {
	if (text.empty() || text.size() > 32)
		return false;
	for (const char c : text)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			return false;
	return true;
}

// A MAC address as the metadata writes it: six two-digit hex groups and ':'.
bool is_mac(std::string_view text) {
	if (text.size() != 17)
		return false;
	for (size_t i = 0; i < text.size(); ++i) {
		const char c = text[i];
		const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (i % 3 == 2 ? c != ':' : !hex)
			return false;
	}
	return true;
}

} // namespace

std::string metadata_uri_from_env() {
	const char *v = std::getenv(kMetadataUriVariable);
	return v != nullptr ? std::string(v) : std::string();
}

bool parse_task_network(const std::string &json, TaskNetwork &out, std::string &error) {
	io::JsonValue task;
	std::string parse_error;
	if (!io::json_parse(json, task, parse_error)) {
		error = "the task metadata is no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	// arn:aws:ecs:<region>:<account>:task/<cluster>/<id>
	const std::string arn = text_member(task, "TaskARN");
	std::vector<std::string_view> fields;
	for (size_t at = 0; fields.size() < 5;) {
		const size_t colon = arn.find(':', at);
		if (colon == std::string::npos)
			break;
		fields.push_back(std::string_view(arn).substr(at, colon - at));
		at = colon + 1;
	}
	if (fields.size() < 5 || fields[0] != "arn" || fields[2] != "ecs" || !is_region(fields[3])) {
		error = "the task metadata names no task ARN with a region ('" + loggable(arn, 120) + "')";
		return false;
	}
	out.region = std::string(fields[3]);
	const io::JsonValue *containers = member(task, "Containers", io::JsonValue::Type::Array);
	if (containers == nullptr) {
		error = "the task metadata lists no containers";
		return false;
	}
	for (const io::JsonValue &container : containers->array) {
		const io::JsonValue *networks = member(container, "Networks", io::JsonValue::Type::Array);
		if (networks == nullptr)
			continue;
		for (const io::JsonValue &network : networks->array) {
			const io::JsonValue *addresses =
					member(network, "IPv4Addresses", io::JsonValue::Type::Array);
			if (addresses == nullptr || addresses->array.empty() ||
					!addresses->array.front().is_string() ||
					!is_ipv4(addresses->array.front().string))
				continue;
			out.private_ipv4 = addresses->array.front().string;
			const std::string mac = text_member(network, "MACAddress");
			out.mac_address = is_mac(mac) ? mac : std::string();
			return true;
		}
	}
	error = "the task metadata names no IPv4 address (a task in awsvpc mode has one)";
	return false;
}

bool read_task_network(
		const std::string &uri, TaskNetwork &out, std::string &error, int timeout_ms) {
	if (uri.rfind("http://", 0) != 0) {
		error = std::string(kMetadataUriVariable) + " is no http:// URI";
		return false;
	}
	const net::HttpReply reply = net::http_exchange("GET", uri + "/task", {}, {}, timeout_ms,
			[](const std::string &host, net::Endpoint &ep) { return net::resolve_ipv4(host, ep); });
	if (!reply.transport_ok) {
		error = "the task metadata endpoint does not answer (" + loggable(reply.error, 120) + ")";
		return false;
	}
	if (reply.code != 200) {
		error = "the task metadata endpoint answers HTTP " + std::to_string(reply.code);
		return false;
	}
	return parse_task_network(std::string(reply.body.begin(), reply.body.end()), out, error);
}

bool public_ipv4(proc::CommandRunner &runner, const std::string &aws_cli, const TaskNetwork &task,
		std::string &out, std::string &error) {
	std::vector<std::string> argv = { aws_cli, "ec2", "describe-network-interfaces", "--region",
		task.region, "--filters", "Name=addresses.private-ip-address,Values=" + task.private_ipv4 };
	if (!task.mac_address.empty())
		argv.push_back("Name=mac-address,Values=" + task.mac_address);
	for (const char *arg : { "--output", "json", "--no-cli-pager", "--cli-connect-timeout", "10",
				 "--cli-read-timeout", "30" })
		argv.push_back(arg);
	const proc::CommandResult result = runner.run(argv, kCliTimeoutMs);
	if (!result.ok()) {
		const std::string why = result.outcome == proc::CommandResult::Outcome::NotStarted
				? result.error
				: result.outcome == proc::CommandResult::Outcome::TimedOut
				? "timed out"
				: "exit " + std::to_string(result.exit_code) + ": " + result.err;
		error = "describe-network-interfaces failed (" + loggable(why, 300) + ")";
		return false;
	}
	io::JsonValue reply;
	std::string parse_error;
	if (!io::json_parse(result.out, reply, parse_error)) {
		error = "describe-network-interfaces answered no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	const io::JsonValue *interfaces =
			member(reply, "NetworkInterfaces", io::JsonValue::Type::Array);
	if (interfaces == nullptr || interfaces->array.size() != 1) {
		error = "describe-network-interfaces names " +
				std::to_string(interfaces ? interfaces->array.size() : 0) + " interfaces at " +
				task.private_ipv4 + ", not one";
		return false;
	}
	const io::JsonValue *association =
			member(interfaces->array.front(), "Association", io::JsonValue::Type::Object);
	const std::string address = association ? text_member(*association, "PublicIp") : std::string();
	if (!is_ipv4(address)) {
		error = "the task's interface at " + task.private_ipv4 +
				" has no public IPv4 address (the service's assign_public_ip)";
		return false;
	}
	out = address;
	return true;
}

std::string lobby_endpoint(
		const Sources &sources, proc::CommandRunner &runner, std::string &error) {
	if (!sources.configured.empty()) {
		if (!program::is_endpoint(sources.configured)) {
			error = "ONNET_LOBBY_ENDPOINT is no <host>:<port>";
			return {};
		}
		return sources.configured;
	}
	if (sources.metadata_uri.empty()) {
		error = std::string("the lobby needs ONNET_LOBBY_ENDPOINT, <host>:<port>, the endpoint the "
							"gates hand its clients (an ECS task finds its own; here ") +
				kMetadataUriVariable + " is unset)";
		return {};
	}
	if (sources.port == 0) {
		error = "the lobby announces its task's address with ONNET_NW_UDP_PORT, which is 0";
		return {};
	}
	TaskNetwork task;
	std::string address;
	std::string why;
	if (!read_task_network(sources.metadata_uri, task, why, sources.metadata_timeout_ms) ||
			!public_ipv4(runner, sources.aws_cli, task, address, why)) {
		error = "the lobby's endpoint from its ECS task: " + why;
		return {};
	}
	std::printf("[boot] lobby: its ECS task's interface %s (%s) is public at %s\n",
			task.private_ipv4.c_str(), task.region.c_str(), address.c_str());
	return address + ":" + std::to_string(sources.port);
}

std::string internal_bind(const Sources &sources, std::string &error) {
	if (!sources.configured.empty() || sources.metadata_uri.empty())
		return {};
	TaskNetwork task;
	std::string why;
	if (!read_task_network(sources.metadata_uri, task, why, sources.metadata_timeout_ms)) {
		error = "the controller's internal listener bind from its ECS task: " + why;
		return {};
	}
	std::printf("[boot] controller: the internal listener binds its ECS task's %s\n",
			task.private_ipv4.c_str());
	return task.private_ipv4 + ":" + std::to_string(sources.port);
}

} // namespace opennova::novaworld_server::ecs
