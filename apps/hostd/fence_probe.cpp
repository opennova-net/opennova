#include "fence_probe.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace opennova::hostd {

namespace {

net::Endpoint endpoint(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
	net::Endpoint ep;
	ep.ip = { a, b, c, d };
	ep.port = port;
	return ep;
}

// Whether a TCP connection to `ep` is made within `timeout_ms`.
bool connects(const net::Endpoint &ep, int timeout_ms) {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ep.port);
	std::memcpy(&addr.sin_addr, ep.ip.data(), 4);
	bool made = ::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof addr) == 0;
	if (!made && errno == EINPROGRESS) {
		pollfd p{ fd, POLLOUT, 0 };
		if (::poll(&p, 1, timeout_ms) == 1) {
			int err = 0;
			socklen_t len = sizeof err;
			made = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
		}
	}
	::close(fd);
	return made;
}

} // namespace

std::vector<ProbeTarget> fence_probe_targets(
		const std::vector<net::Endpoint> &fleet_addresses, uint16_t fleet_port) {
	std::vector<ProbeTarget> out = {
		{ "the instance metadata service (169.254.169.254:80)", endpoint(169, 254, 169, 254, 80) },
		{ "the ECS task credentials (169.254.170.2:80)", endpoint(169, 254, 170, 2, 80) },
		{ "the ECS agent (127.0.0.1:51678)", endpoint(127, 0, 0, 1, 51678) },
	};
	for (net::Endpoint ep : fleet_addresses) {
		ep.port = fleet_port;
		out.push_back({ "the fleet's internal port (" + net::endpoint_to_string(ep) + ")", ep });
	}
	return out;
}

std::string first_answering(const std::vector<ProbeTarget> &targets, int timeout_ms) {
	for (const ProbeTarget &target : targets)
		if (connects(target.endpoint, timeout_ms))
			return target.what;
	return {};
}

bool probe_as(uint32_t uid, const std::vector<ProbeTarget> &targets, int timeout_ms,
		std::string &answered, std::string &error) {
	// The child reports by its exit code: 0 every target refused, 1 + i target
	// i answered, 255 it could not become `uid`.
	const pid_t pid = ::fork();
	if (pid < 0) {
		error = std::string("fork failed: ") + std::strerror(errno);
		return false;
	}
	if (pid == 0) {
		if (::setgroups(0, nullptr) != 0 || ::setgid(uid) != 0 || ::setuid(uid) != 0)
			::_exit(255);
		for (size_t i = 0; i < targets.size() && i < 250; ++i)
			if (connects(targets[i].endpoint, timeout_ms))
				::_exit(static_cast<int>(1 + i));
		::_exit(0);
	}
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) == 255) {
		error = "the probe could not run as UID " + std::to_string(uid);
		return false;
	}
	const int code = WEXITSTATUS(status);
	answered = code == 0 ? std::string() : targets[static_cast<size_t>(code - 1)].what;
	return true;
}

} // namespace opennova::hostd
