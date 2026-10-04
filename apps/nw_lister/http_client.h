#pragma once

#include <string>
#include <vector>

namespace opennova::lister {

struct HttpResponse {
	bool transport_ok = false;
	int code = 0;
	std::vector<std::string> headers; // raw "Name: value" lines
	std::vector<uint8_t> body;
	std::string error;
};

// Minimal blocking HTTP/1.0 client (retail's IB3 browser speaks HTTP/1.0 and
// sends one "Cookie:" line per cookie, which OpenNova's HttpRequestSpec already
// models). GET when body is empty and post == false. Destinations go through
// the same loopback-only policy as UDP (udp.h).
HttpResponse http_request(bool post, const std::string &url, const std::vector<std::string> &headers,
                          const std::string &body, int timeout_ms = 15000);

} // namespace opennova::lister
