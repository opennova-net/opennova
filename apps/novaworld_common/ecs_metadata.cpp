#include "ecs_metadata.h"

#include "loggable.h"

#include <base/io/json.h>
#include <net_http.h>
#include <net_sockets.h>

#include <cstdio>
#include <cstdlib>

namespace opennova::novaworld_server::ecs {

namespace {

const io::JsonValue *member(
		const io::JsonValue &object, const char *key, io::JsonValue::Type type) {
	const io::JsonValue *v = object.is_object() ? object.get(key) : nullptr;
	return v != nullptr && v->type == type ? v : nullptr;
}

bool is_ipv4(const std::string &text) {
	net::Endpoint ep;
	return !text.empty() && net::resolve_ipv4(text, ep, /*allow_names=*/false);
}

// GET `url` of the metadata endpoint (<uri>/task, or <uri> for the container's
// own) into `body`; false with `error` when the endpoint does not answer 200.
bool get_metadata(const std::string &url, std::string &body, std::string &error, int timeout_ms) {
	if (url.rfind("http://", 0) != 0) {
		error = std::string(kMetadataUriVariable) + " is no http:// URI";
		return false;
	}
	const net::HttpReply reply = net::http_exchange("GET", url, {}, {}, timeout_ms,
			[](const std::string &host, net::Endpoint &ep) { return net::resolve_ipv4(host, ep); });
	if (!reply.transport_ok) {
		error = "the task metadata endpoint does not answer (" + loggable(reply.error, 120) + ")";
		return false;
	}
	if (reply.code != 200) {
		error = "the task metadata endpoint answers HTTP " + std::to_string(reply.code);
		return false;
	}
	body.assign(reply.body.begin(), reply.body.end());
	return true;
}

} // namespace

std::string metadata_uri_from_env() {
	const char *v = std::getenv(kMetadataUriVariable);
	return v != nullptr ? std::string(v) : std::string();
}

bool parse_task_address(const std::string &json, std::string &out, std::string &error) {
	io::JsonValue task;
	std::string parse_error;
	if (!io::json_parse(json, task, parse_error)) {
		error = "the task metadata is no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
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
			out = addresses->array.front().string;
			return true;
		}
	}
	error = "the task metadata names no IPv4 address (a task in awsvpc mode has one)";
	return false;
}

bool read_task_address(
		const std::string &uri, std::string &out, std::string &error, int timeout_ms) {
	std::string body;
	return get_metadata(uri + "/task", body, error, timeout_ms) &&
			parse_task_address(body, out, error);
}

bool parse_task_revision(const std::string &json, std::string &out, std::string &error) {
	io::JsonValue task;
	std::string parse_error;
	if (!io::json_parse(json, task, parse_error)) {
		error = "the task metadata is no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	const io::JsonValue *family = member(task, "Family", io::JsonValue::Type::String);
	const io::JsonValue *revision = member(task, "Revision", io::JsonValue::Type::String);
	if (family == nullptr || family->string.empty() || revision == nullptr ||
			revision->string.empty()) {
		error = "the task metadata names no task definition family and revision";
		return false;
	}
	out = family->string + ":" + revision->string;
	return true;
}

bool read_task_revision(
		const std::string &uri, std::string &out, std::string &error, int timeout_ms) {
	std::string body;
	return get_metadata(uri + "/task", body, error, timeout_ms) &&
			parse_task_revision(body, out, error);
}

bool parse_container_image(const std::string &json, std::string &out, std::string &error) {
	io::JsonValue container;
	std::string parse_error;
	if (!io::json_parse(json, container, parse_error)) {
		error = "the container metadata is no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	const io::JsonValue *image = member(container, "Image", io::JsonValue::Type::String);
	if (image == nullptr || image->string.empty()) {
		error = "the container metadata names no image";
		return false;
	}
	out = image->string;
	return true;
}

bool read_container_image(
		const std::string &uri, std::string &out, std::string &error, int timeout_ms) {
	std::string body;
	return get_metadata(uri, body, error, timeout_ms) && parse_container_image(body, out, error);
}

bool parse_container_log_stream(const std::string &json, std::string &out, std::string &error) {
	io::JsonValue container;
	std::string parse_error;
	if (!io::json_parse(json, container, parse_error)) {
		error = "the container metadata is no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	const io::JsonValue *options = member(container, "LogOptions", io::JsonValue::Type::Object);
	const io::JsonValue *stream = options != nullptr
			? member(*options, "awslogs-stream", io::JsonValue::Type::String)
			: nullptr;
	if (stream == nullptr || stream->string.empty()) {
		error = "the container metadata names no awslogs stream";
		return false;
	}
	out = stream->string;
	return true;
}

bool read_container_log_stream(
		const std::string &uri, std::string &out, std::string &error, int timeout_ms) {
	std::string body;
	return get_metadata(uri, body, error, timeout_ms) &&
			parse_container_log_stream(body, out, error);
}

std::string internal_bind(const Sources &sources, std::string &error) {
	if (!sources.configured.empty() || sources.metadata_uri.empty())
		return {};
	std::string address;
	std::string why;
	if (!read_task_address(sources.metadata_uri, address, why, sources.metadata_timeout_ms)) {
		error = "the fleet's internal listener bind from its ECS task: " + why;
		return {};
	}
	std::printf("[boot] fleet: the internal listener binds its ECS task's %s\n", address.c_str());
	return address + ":" + std::to_string(sources.port);
}

} // namespace opennova::novaworld_server::ecs
