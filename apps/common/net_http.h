#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net_sockets.h"

namespace opennova::net {

// One completed HTTP exchange, in the shape LobbyHttpFlow's on_*_response hooks take.
struct HttpReply {
	bool transport_ok = false;        // the exchange itself completed (a status line came back)
	bool connected = false; // the connection was made (the request may have reached a server)
	int code = 0;                     // the HTTP status
	std::vector<std::string> headers; // raw "Name: value" lines
	std::vector<uint8_t> body;
	std::string error;                // why transport_ok is false
};

// Resolve an http:// URL's host into an endpoint (its port left alone); false refuses it.
using HttpResolver = std::function<bool(const std::string &host, Endpoint &out)>;

// One blocking HTTP/1.0 exchange on its own connection: `method` ("GET", "POST", "PUT",
// "DELETE"; every method but GET sends `body` with a Content-Length, after the given lines unless
// they carry one), the request line and Host first (the URL's authority, unless the given lines
// carry a Host line of their own), then the "Name: value" header lines as given,
// then the reply read to the server's close or to its Content-Length. http://
// only. Every connect, send and recv waits at most `timeout_ms`; a reply past `max_reply_bytes`
// is refused, and with `total_ms` > 0 so is one still arriving `total_ms` after the send (a
// peer that trickles bytes inside each recv's wait).
inline constexpr size_t kHttpMaxReplyBytes = 8u * 1024u * 1024u;
HttpReply http_exchange(const std::string &method, const std::string &url,
		const std::vector<std::string> &headers, const std::string &body, int timeout_ms,
		const HttpResolver &resolve, size_t max_reply_bytes = kHttpMaxReplyBytes, int total_ms = 0);

} // namespace opennova::net
