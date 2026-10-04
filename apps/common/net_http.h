#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net_sockets.h"

namespace opennova::net {

// One completed HTTP exchange, in the shape LobbyHttpFlow's on_*_response hooks take.
struct HttpReply {
	bool transport_ok = false;        // the exchange itself completed (a status line came back)
	int code = 0;                     // the HTTP status
	std::vector<std::string> headers; // raw "Name: value" lines
	std::vector<uint8_t> body;
	std::string error;                // why transport_ok is false
};

// Resolve an http:// URL's host into an endpoint (its port left alone); false refuses it.
using HttpResolver = std::function<bool(const std::string &host, Endpoint &out)>;

// One blocking HTTP/1.0 exchange on its own connection: GET, or POST when `post` (with
// Content-Length), the "Name: value" header lines as given, then the reply read to the server's
// close or to its Content-Length. http:// only. Every connect, send and recv waits at most
// `timeout_ms`.
HttpReply http_exchange(bool post, const std::string &url, const std::vector<std::string> &headers,
		const std::string &body, int timeout_ms, const HttpResolver &resolve);

} // namespace opennova::net
