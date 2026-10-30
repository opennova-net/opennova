#include "http_exchange.h"

#include <base/io/strutil.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>

namespace opennova::net {

namespace {

// The header lines of a head (its status line dropped), each cut at its LF and a CR before it.
std::vector<std::string> head_lines(std::string_view head) {
	std::vector<std::string> lines;
	size_t start = head.find('\n');
	while (start != std::string_view::npos && start + 1 < head.size()) {
		++start;
		size_t end = head.find('\n', start);
		std::string_view line = head.substr(start, end == std::string_view::npos ? end : end - start);
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		if (!line.empty()) lines.emplace_back(line);
		start = end;
	}
	return lines;
}

// The C runtime's atol over `text` from `at`: leading whitespace, a sign, the digits.
long c_atol(const std::string &text, size_t at) {
	return at < text.size() ? std::atol(text.c_str() + at) : 0;
}

// --- the plain framing ---

// Split the head (status line + header lines) off `raw`; false while it is incomplete.
bool parse_plain_head(const std::string &raw, HttpReply &reply, size_t &body_at,
		long long &content_length) {
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

// The plain framing's reader state over the bytes so far.
struct PlainReader {
	bool head_done = false;
	size_t body_at = 0;
	long long content_length = -1;

	// True once the reply is whole (its length reached).
	bool feed(const std::string &raw, HttpReply &reply) {
		if (!head_done) head_done = parse_plain_head(raw, reply, body_at, content_length);
		return head_done && content_length >= 0 &&
				raw.size() - body_at >= static_cast<size_t>(content_length);
	}

	void finish(const std::string &raw, HttpReply &reply) {
		if (!head_done && !parse_plain_head(raw, reply, body_at, content_length)) {
			reply.error = "no HTTP reply (" + std::to_string(raw.size()) + " bytes)";
			return;
		}
		size_t body_end = raw.size();
		if (content_length >= 0 && body_at + static_cast<size_t>(content_length) < body_end) {
			body_end = body_at + static_cast<size_t>(content_length);
		}
		reply.body.assign(raw.begin() + static_cast<std::ptrdiff_t>(body_at),
				raw.begin() + static_cast<std::ptrdiff_t>(body_end));
		reply.transport_ok = true;
	}
};

// --- the menu browser's framing (HttpFraming::Browser carries the witness) ---

struct BrowserReader {
	// -1 until the head is split off, then its CONTENT-LENGTH's number, -2 for none.
	long content_length = -1;
	std::optional<std::string> head;
	std::string body;

	// The bytes so far are `body` (the head moved out once split); true once the reply is whole.
	bool feed(const std::string &chunk) {
		body += chunk;
		if (content_length == -1 && strutil::starts_with_icase(body, "HTTP")) {
			const size_t head_end = body.find("\r\n\r\n");
			if (head_end != std::string::npos) {
				head = body.substr(0, head_end + 4);
				body.erase(0, head_end + 4);
				const size_t length_at = strutil::ifind(*head, "CONTENT-LENGTH");
				content_length = length_at != std::string_view::npos ? c_atol(*head, length_at + 15) : -2;
			}
		}
		// `body_len >= content_length` compares unsigned: -1 and -2 never complete.
		return content_length >= 0 && body.size() >= static_cast<size_t>(content_length);
	}

	// The server closed: a reply with no head yet that begins "HTTP" splits at the first other
	// separator, else its whole text is the head (the body left whole).
	void close() {
		if (content_length != -1 || body.empty() || !strutil::starts_with_icase(body, "HTTP")) return;
		for (const char *sep : {"\r\n\r\n", "\n\r\n\r", "\r\r", "\n\n"}) {
			const size_t at = body.find(sep);
			if (at == std::string::npos) continue;
			const size_t end = at + std::char_traits<char>::length(sep);
			head = body.substr(0, end) + "\r\n\r\n";
			body.erase(0, end);
			return;
		}
		head = body + "\r\n\r\n";
	}

	void finish(HttpReply &reply) {
		if (!head) {
			reply.code = 100;
		} else {
			size_t at = 0;
			while (at < head->size() && !std::isspace(static_cast<unsigned char>((*head)[at]))) ++at;
			reply.code = static_cast<int>(c_atol(*head, at));
			reply.headers = head_lines(*head);
		}
		reply.body.assign(body.begin(), body.end());
		reply.transport_ok = true;
	}
};

} // namespace

HttpReply http_exchange(IStreamConnector &connector, const std::string &method,
		const std::string &url, const std::vector<std::string> &headers, const std::string &body,
		const HttpResolver &resolve, const HttpExchangeOptions &options) {
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

	const std::unique_ptr<IStreamSocket> stream = connector.connect(to, options.connect_ms);
	if (stream == nullptr) {
		reply.error = "connect to " + endpoint_to_string(to) + " failed";
		return reply;
	}
	reply.connected = true;
	const bool has_body = method != "GET";
	std::string request = method + " " + path + " HTTP/1.0\r\n";
	// A caller that names its own Host line (the NovaWorld menu browser's, which drops the URL's
	// port: browser_request_host) gets none from the transport, its line leading the caller's.
	bool host_given = false;
	for (const std::string &line : headers) {
		if (strutil::starts_with_icase(line, "Host:")) host_given = true;
	}
	if (!host_given) request += "Host: " + authority + "\r\n";
	// A caller that places its own Content-Length (the NovaWorld web flow's form post, which writes
	// it where the menu browser does) gets no second one.
	bool length_given = false;
	for (const std::string &line : headers) {
		request += line + "\r\n";
		if (strutil::starts_with_icase(line, "Content-Length:")) length_given = true;
	}
	if (has_body && !length_given) {
		request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	}
	request += "\r\n";
	if (has_body) {
		request += body;
	}
	if (!stream->send_all(reinterpret_cast<const uint8_t *>(request.data()), request.size())) {
		reply.error = "send failed";
		return reply;
	}

	const bool browser = options.framing == HttpFraming::Browser;
	PlainReader plain;
	BrowserReader framed;
	std::string raw; // the plain reader's bytes so far
	size_t received = 0;
	const auto deadline =
			std::chrono::steady_clock::now() + std::chrono::milliseconds(options.total_ms);
	uint8_t buf[0x4000];
	bool whole = false;
	while (!whole) {
		if (options.total_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
			reply.error = "reply too slow";
			return reply;
		}
		// One wait for bytes, in slices between which a cancel is seen. A bound wait that runs out
		// ends a plain reply as the server's close would, and fails a browser one (the menu
		// browser's own wait has no bound: only serve sets one).
		int n = kStreamRecvTimedOut;
		int waited = 0;
		while (n == kStreamRecvTimedOut) {
			if (options.cancelled != nullptr && options.cancelled->load()) {
				reply.error = "cancelled";
				return reply;
			}
			if (options.io_ms > 0 && waited >= options.io_ms) break;
			int slice = kHttpCancelSliceMs;
			if (options.io_ms > 0 && options.io_ms - waited < slice) slice = options.io_ms - waited;
			n = stream->recv(buf, sizeof(buf), slice);
			waited += slice;
		}
		if (n == kStreamRecvTimedOut && browser) {
			reply.error = "reply timed out";
			return reply;
		}
		if (n == kStreamRecvTimedOut || n == 0) break;
		if (n < 0) {
			if (browser) {
				// A receive error frees what came, and the pump hands the page none.
				// [orig: HTTP_RecvResponse @0x666EF0 - the -1 test @0x666F77, the body freed
				//  @0x6673F0, E_FAIL @0x667410; CGopherWnd_PumpRequests @0x659740 - the head freed
				//  @0x65997F on the failure]
				reply.error = "receive failed";
				return reply;
			}
			break;
		}
		received += static_cast<size_t>(n);
		if (received > options.max_reply_bytes) {
			reply.error = "reply too large";
			return reply;
		}
		const std::string chunk(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
		if (browser) {
			whole = framed.feed(chunk);
		} else {
			raw += chunk;
			whole = plain.feed(raw, reply);
		}
	}
	if (browser) {
		if (!whole) framed.close();
		framed.finish(reply);
	} else {
		plain.finish(raw, reply);
	}
	return reply;
}

HttpReply http_exchange(const std::string &method, const std::string &url,
		const std::vector<std::string> &headers, const std::string &body, int timeout_ms,
		const HttpResolver &resolve, size_t max_reply_bytes, int total_ms) {
	HttpExchangeOptions options;
	options.connect_ms = timeout_ms;
	options.io_ms = timeout_ms;
	options.total_ms = total_ms;
	options.max_reply_bytes = max_reply_bytes;
	return http_exchange(tcp_stream_connector(), method, url, headers, body, resolve, options);
}

} // namespace opennova::net
