// CountingDatagramSocket: the F3 Net window's traffic counter over the
// real-socket seam passes every datagram through unchanged and tallies
// packets and bytes in total and per peer, capping the peer table.
#include <net/npwire/counting_datagram_socket.h>

#include <cstdio>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c) \
	do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

struct FakeSocket : IDatagramSocket {
	std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> inbound;
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty()) return 0;
		auto front = std::move(inbound.front());
		inbound.pop_front();
		from = front.first;
		const std::size_t n = front.second.size() < cap ? front.second.size() : cap;
		for (std::size_t i = 0; i < n; ++i) buf[i] = front.second[i];
		return static_cast<int>(n);
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

PeerAddr peer(uint8_t last, uint16_t port) {
	return peer_addr_from_octets({192, 168, 1, last}, port);
}

}  // namespace

int main() {
	auto inner = std::make_unique<FakeSocket>();
	FakeSocket *fake = inner.get();
	CountingDatagramSocket socket(std::move(inner));

	const uint8_t payload[5] = {1, 2, 3, 4, 5};
	socket.send_to(peer(2, 7000), payload, 5);
	socket.send_to(peer(3, 7000), payload, 3);
	CHECK(fake->sent.size() == 2 && fake->sent[1].second.size() == 3);  // passed through unchanged
	CHECK(socket.totals().tx_packets == 2 && socket.totals().tx_bytes == 8);

	fake->inbound.emplace_back(peer(2, 7000), std::vector<uint8_t>(40, 0xAB));
	uint8_t buf[64];
	PeerAddr from;
	CHECK(socket.recv_from(buf, sizeof(buf), from) == 40 && from == peer(2, 7000) && buf[0] == 0xAB);
	CHECK(socket.recv_from(buf, sizeof(buf), from) == 0);  // nothing pending counts nothing
	CHECK(socket.totals().rx_packets == 1 && socket.totals().rx_bytes == 40);

	const DatagramTraffic *a = socket.peer(peer(2, 7000));
	const DatagramTraffic *b = socket.peer(peer(3, 7000));
	CHECK(a != nullptr && a->tx_bytes == 5 && a->rx_bytes == 40 && a->rx_packets == 1);
	CHECK(b != nullptr && b->tx_bytes == 3 && b->rx_packets == 0);
	CHECK(socket.peer(peer(9, 7000)) == nullptr);

	// A flood of sources stops growing the table at its cap; the totals still count.
	for (int i = 0; i < 300; ++i) socket.send_to(peer(static_cast<uint8_t>(i & 0xFF), static_cast<uint16_t>(8000 + i)), payload, 1);
	CHECK(socket.peers().size() == CountingDatagramSocket::kMaxPeers);
	CHECK(socket.totals().tx_packets == 302);

	std::printf("counting_datagram_socket: %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
