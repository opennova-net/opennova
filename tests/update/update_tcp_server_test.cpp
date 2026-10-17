// The update listener's pump (apps/common/update_tcp_server) over loopback: a client that
// pipelines GETs without reading their replies is read no further once its replies back up,
// and what the listener holds for it stays within kOutboxCap and one of the server's
// kSendBudget calls (the requests it never read wait in the sockets); another client's
// pipelined GETs meanwhile all come back whole and in order, a budget at a time.

#include "common/test_expect.h"

#include <net/update/update_server.h>
#include <net/update/update_wire.h>

#include "net_sockets.h"
#include "update_tcp_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;

namespace {

class BigCatalog : public update::UpdateCatalog {
public:
	std::vector<uint8_t> big;
	bool has_product(std::string_view login) const override { return login == "JOTRus"; }
	std::vector<std::string> server_hosts(std::string_view) const override {
		return { "ftp.novaworld.com" };
	}
	std::string command_file_name(std::string_view) const override { return "JOPTR.CMD"; }
	bool read_file(std::string_view, std::string_view name,
			std::vector<uint8_t> &out) const override {
		if (name != "BIG.BIN")
			return false;
		out = big;
		return true;
	}
};

// `request` sent, then its replies read until `count` complete.
bool exchange(net::Socket &socket, const std::string &request, size_t count,
		std::vector<update::Reply> &out) {
	if (!net::tcp_send_all(
				socket, reinterpret_cast<const uint8_t *>(request.data()), request.size()))
		return false;
	std::vector<uint8_t> stream;
	uint8_t buffer[4096];
	out.clear();
	while (out.size() < count) {
		update::Reply reply;
		size_t consumed = 0;
		if (!stream.empty() &&
				update::decode_reply(stream.data(), stream.size(), reply, consumed) ==
						update::Frame::Complete) {
			out.push_back(reply);
			stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(consumed));
			continue;
		}
		const int n = net::tcp_recv(socket, buffer, sizeof buffer);
		if (n <= 0)
			return false;
		stream.insert(stream.end(), buffer, buffer + n);
	}
	return true;
}

} // namespace

int main() {
	TEST_EXPECT(net::startup() == 0);
	BigCatalog catalog;
	catalog.big.resize(192 * 1024 + 3);
	for (size_t i = 0; i < catalog.big.size(); ++i)
		catalog.big[i] = static_cast<uint8_t>(i * 31 + 7);
	update::UpdateServer server(catalog);
	net::UpdateTcpServer listener(server);
	TEST_EXPECT(listener.listen({ 0 }, true));
	if (listener.ports().empty())
		return 1;

	// What one connection may hold: its unsent replies under the cap, one budget's worth
	// more, and a reply header past the budget.
	const size_t per_connection =
			net::UpdateTcpServer::kOutboxCap + update::UpdateServer::kSendBudget + 4096;
	// The pump, which reads the listener's own high-water mark on its own thread; past a
	// fuse well over the bound it stops, so a listener that does not hold back fails here
	// rather than holding gigabytes.
	std::atomic<bool> stop{ false };
	std::atomic<size_t> high{ 0 };
	std::thread pump([&] {
		while (!stop) {
			if (high > 8 * per_connection) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			const bool busy = listener.pump();
			high = std::max<size_t>(high, listener.queued_bytes());
			if (!busy)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	net::Endpoint to;
	to.ip = { 127, 0, 0, 1 };
	to.port = listener.ports()[0];
	int rc = 0;
	std::vector<update::Reply> r;

	// The hog: logs in, then sends GETs for some terabytes of replies and reads none of
	// them, until its socket takes no more requests.
	net::Socket hog = net::tcp_connect(to, 1000);
	if (!hog.is_valid() ||
			!exchange(hog, update::encode_login("JOTRus", "", update::kIdServerLogin), 1, r) ||
			r[0].result != 0) {
		std::fprintf(stderr, "update_tcp_server: the hog's login failed\n");
		rc = 1;
	}
	if (rc == 0) {
		const std::vector<std::string> names(300, "BIG.BIN");
		std::string batch;
		for (int32_t id = 0; id < 64; ++id)
			batch += update::encode_get(names, id);
		bool stalled = false;
		for (int i = 0; i < 400 && !stalled; ++i)
			stalled = !net::tcp_send_all(
					hog, reinterpret_cast<const uint8_t *>(batch.data()), batch.size());
		if (!stalled) {
			std::fprintf(stderr, "update_tcp_server: the hog's requests were all read\n");
			rc = 1;
		}
		if (high > per_connection) {
			std::fprintf(stderr, "update_tcp_server: the hog's replies grew to %zu bytes\n",
					static_cast<size_t>(high));
			rc = 1;
		}
	}

	// A client beside it: three GETs of five files in one send, every reply back whole and
	// in order, the listener holding at most a connection's bound for each.
	net::Socket fetch = net::tcp_connect(to, 5000);
	if (rc == 0) {
		std::string pipelined = update::encode_login("JOTRus", "", update::kIdServerLogin);
		for (int32_t id = 0; id < 3; ++id)
			pipelined += update::encode_get(std::vector<std::string>(5, "BIG.BIN"), id);
		if (!fetch.is_valid() || !exchange(fetch, pipelined, 16, r)) {
			std::fprintf(stderr, "update_tcp_server: the client's replies did not come back\n");
			rc = 1;
		} else {
			TEST_EXPECT(r[0].verb == update::Verb::Login && r[0].result == 0);
			for (size_t i = 1; i < r.size(); ++i)
				TEST_EXPECT(r[i].verb == update::Verb::Get && r[i].id == int32_t((i - 1) / 5) &&
						r[i].payload == catalog.big);
		}
		if (high > 2 * per_connection) {
			std::fprintf(stderr, "update_tcp_server: the replies grew to %zu bytes\n",
					static_cast<size_t>(high));
			rc = 1;
		}
	}

	if (fetch.is_valid())
		net::close_socket(fetch);
	if (hog.is_valid())
		net::close_socket(hog);
	stop = true;
	pump.join();
	listener.close();
	net::shutdown();
	if (rc == 0)
		std::printf("update_tcp_server: ok (high water %zu bytes)\n", static_cast<size_t>(high));
	return rc;
}
