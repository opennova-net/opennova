#include "instance_metadata.h"

#include "hosted/protocol/agent_protocol.h"

#include "net_http.h"

#include <base/io/strutil.h>

namespace opennova::hostd {

namespace {

namespace hosted = novaworld_server::hosted;

net::HttpReply call(const std::string &method, const std::string &url,
		const std::vector<std::string> &headers, int timeout_ms) {
	return net::http_exchange(method, url, headers, {}, timeout_ms,
			[](const std::string &host, net::Endpoint &ep) { return net::resolve_ipv4(host, ep); });
}

std::string failure(const char *what, const net::HttpReply &reply) {
	return std::string("the instance metadata's ") + what +
			(reply.transport_ok ? " answered HTTP " + std::to_string(reply.code)
								: " did not answer (" + reply.error + ")");
}

} // namespace

bool read_image_id(
		const std::string &base_url, std::string &out, std::string &error, int timeout_ms) {
	const net::HttpReply token = call("PUT", base_url + "/latest/api/token",
			{ "X-aws-ec2-metadata-token-ttl-seconds: 60" }, timeout_ms);
	const std::string session = strutil::trim(std::string(token.body.begin(), token.body.end()));
	if (!token.transport_ok || token.code != 200 || session.empty()) {
		error = failure("token", token);
		return false;
	}
	for (const char c : session) {
		if (c < 0x21 || c > 0x7E) {
			error = "the instance metadata's token is no token";
			return false;
		}
	}
	const net::HttpReply image = call("GET", base_url + "/latest/meta-data/ami-id",
			{ "X-aws-ec2-metadata-token: " + session }, timeout_ms);
	if (!image.transport_ok || image.code != 200) {
		error = failure("ami-id", image);
		return false;
	}
	const std::string id = strutil::trim(std::string(image.body.begin(), image.body.end()));
	if (!hosted::is_image_id(id)) {
		error = "the instance metadata's ami-id is no image id";
		return false;
	}
	out = id;
	return true;
}

} // namespace opennova::hostd
