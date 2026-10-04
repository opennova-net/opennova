#include "net_http.h"

#include <base/io/strutil.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace opennova::net {

namespace {

constexpr size_t kMaxReplyBytes = 8u * 1024u * 1024u;

// Split the head (status line + header lines) off `raw`; false while it is incomplete.
bool parse_head(const std::string &raw, HttpReply &reply, size_t &body_at, long long &content_length) {
	const size_t head_end = raw.find("\r\n\r\n");
	if (head_end == std::string::npos) {
		return false;
	}
	body_at = head_end + 4;
	content_length = -1;
	size_t start = 0;
	bool status_line = true;
	while (start < head_end) {
		size_t end = raw.find("\r\n", start);
		if (end == std::string::npos || end > head_end) {
			end = head_end;
		}
		const std::string line = raw.substr(start, end - start);
		if (status_line) {
			const size_t space = line.find(' ');
			const auto code = space == std::string::npos
					? std::optional<int>()
					: strutil::parse_int(line.substr(space + 1, 3));
			reply.code = code.value_or(0);
			status_line = false;
		} else if (!line.empty()) {
			reply.headers.push_back(line);
			const size_t colon = line.find(':');
			if (colon != std::string::npos &&
					strutil::iequals(strutil::trim_view(std::string_view(line).substr(0, colon)),
							"Content-Length")) {
				const auto length = strutil::parse_ullong(strutil::trim(line.substr(colon + 1)));
				if (length) {
					content_length = static_cast<long long>(*length);
				}
			}
		}
		start = end + 2;
	}
	return true;
}

} // namespace

HttpReply http_exchange(bool post, const std::string &url, const std::vector<std::string> &headers,
		const std::string &body, int timeout_ms, const HttpResolver &resolve) {
	HttpReply reply;
	constexpr std::string_view kScheme = "http://";
	if (!strutil::starts_with_icase(url, kScheme)) {
		reply.error = "only http:// URLs are supported";
		return reply;
	}
	const size_t path_at = url.find('/', kScheme.size());
	const std::string authority = url.substr(kScheme.size(),
			path_at == std::string::npos ? std::string::npos : path_at - kScheme.size());
	const std::string path = path_at == std::string::npos ? "/" : url.substr(path_at);
	std::string host = authority;
	Endpoint to;
	to.port = 80;
	if (const size_t colon = authority.find(':'); colon != std::string::npos) {
		host = authority.substr(0, colon);
		const auto port = strutil::parse_int(authority.substr(colon + 1));
		if (!port || *port <= 0 || *port > 65535) {
			reply.error = "bad port in " + url;
			return reply;
		}
		to.port = static_cast<uint16_t>(*port);
	}
	if (!resolve(host, to)) {
		reply.error = "cannot reach " + host;
		return reply;
	}

	ScopedSocket socket(tcp_connect(to, timeout_ms));
	if (!socket.is_valid()) {
		reply.error = "connect to " + endpoint_to_string(to) + " failed";
		return reply;
	}
	std::string request = std::string(post ? "POST " : "GET ") + path + " HTTP/1.0\r\n";
	request += "Host: " + authority + "\r\n";
	for (const std::string &line : headers) {
		request += line + "\r\n";
	}
	if (post) {
		request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	}
	request += "\r\n";
	if (post) {
		request += body;
	}
	if (!tcp_send_all(socket.get(), reinterpret_cast<const uint8_t *>(request.data()), request.size())) {
		reply.error = "send failed";
		return reply;
	}

	std::string raw;
	bool head_done = false;
	size_t body_at = 0;
	long long content_length = -1;
	uint8_t buf[8192];
	for (;;) {
		if (!head_done) {
			head_done = parse_head(raw, reply, body_at, content_length);
		}
		if (head_done && content_length >= 0 &&
				raw.size() - body_at >= static_cast<size_t>(content_length)) {
			break;
		}
		const int n = tcp_recv(socket.get(), buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		raw.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
		if (raw.size() > kMaxReplyBytes) {
			reply.error = "reply too large";
			return reply;
		}
	}
	if (!head_done && !parse_head(raw, reply, body_at, content_length)) {
		reply.error = "no HTTP reply (" + std::to_string(raw.size()) + " bytes)";
		return reply;
	}
	size_t body_end = raw.size();
	if (content_length >= 0 && body_at + static_cast<size_t>(content_length) < body_end) {
		body_end = body_at + static_cast<size_t>(content_length);
	}
	reply.body.assign(raw.begin() + static_cast<std::ptrdiff_t>(body_at),
			raw.begin() + static_cast<std::ptrdiff_t>(body_end));
	reply.transport_ok = true;
	return reply;
}

} // namespace opennova::net
