#include "http_client.h"
#include "log.h"
#include "udp.h"

#include "socket_compat.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

namespace opennova::lister {

HttpResponse http_request(bool post, const std::string &url, const std::vector<std::string> &headers,
                          const std::string &body, int timeout_ms) {
	HttpResponse r;
	const std::string scheme = "http://";
	if (url.compare(0, scheme.size(), scheme) != 0) {
		r.error = "only http:// URLs are supported";
		return r;
	}
	const size_t host_start = scheme.size();
	const size_t path_start = url.find('/', host_start);
	const std::string hostport = url.substr(host_start, path_start == std::string::npos
	                                                         ? std::string::npos
	                                                         : path_start - host_start);
	const std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
	std::string host = hostport;
	uint16_t port = 80;
	if (const size_t c = hostport.find(':'); c != std::string::npos) {
		host = hostport.substr(0, c);
		port = static_cast<uint16_t>(std::atoi(hostport.c_str() + c + 1));
	}
	Addr addr;
	if (!resolve_checked(host, port, addr, "HTTP")) {
		r.error = "destination refused by policy";
		return r;
	}

	const SockHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == kInvalidSock) {
		r.error = "socket() failed";
		return r;
	}
#ifdef _WIN32
	DWORD tmo = static_cast<DWORD>(timeout_ms);
#else
	timeval tmo{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tmo), sizeof(tmo));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&tmo), sizeof(tmo));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = addr.ip_be;
	sa.sin_port = htons(port);
	if (::connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
		close_sock(s);
		r.error = "connect to " + addr.str() + " failed";
		return r;
	}

	std::string req = std::string(post ? "POST " : "GET ") + path + " HTTP/1.0\r\n";
	req += "Host: " + hostport + "\r\n";
	for (const auto &h : headers) req += h + "\r\n";
	if (post) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	req += "\r\n";
	if (post) req += body;
	size_t sent = 0;
	while (sent < req.size()) {
		const auto n = ::send(s, req.data() + sent, static_cast<int>(req.size() - sent), 0);
		if (n <= 0) {
			close_sock(s);
			r.error = "send failed";
			return r;
		}
		sent += static_cast<size_t>(n);
	}
	std::string raw;
	char buf[8192];
	for (;;) {
		const auto n = ::recv(s, buf, sizeof(buf), 0);
		if (n <= 0) break;
		raw.append(buf, static_cast<size_t>(n));
		if (raw.size() > 8 * 1024 * 1024) break;
	}
	close_sock(s);

	const size_t hdr_end = raw.find("\r\n\r\n");
	if (hdr_end == std::string::npos) {
		r.error = "malformed HTTP response (" + std::to_string(raw.size()) + " bytes)";
		return r;
	}
	const std::string head = raw.substr(0, hdr_end);
	size_t line_start = 0;
	bool first = true;
	while (line_start < head.size()) {
		size_t line_end = head.find("\r\n", line_start);
		if (line_end == std::string::npos) line_end = head.size();
		const std::string line = head.substr(line_start, line_end - line_start);
		if (first) {
			const size_t sp = line.find(' ');
			r.code = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
			first = false;
		} else if (!line.empty()) {
			r.headers.push_back(line);
		}
		line_start = line_end + 2;
	}
	r.body.assign(raw.begin() + static_cast<std::ptrdiff_t>(hdr_end + 4), raw.end());
	r.transport_ok = true;
	return r;
}

} // namespace opennova::lister
