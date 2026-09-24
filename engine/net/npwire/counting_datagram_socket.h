#pragma once

// A counting decorator over the real-socket seam (idatagram_socket.h): every
// datagram the owner loop sends or drains passes through unchanged while the
// decorator tallies packets and bytes, in total and per peer address. The
// wire is untouched — this is observation for the dev tools' Net window, not
// protocol. Header-only; the per-peer table is a short linear list (a listen
// server's peer count), capped so a flood of spoofed sources cannot grow it.

#include <net/npwire/idatagram_socket.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace opennova {

struct DatagramTraffic {
	uint64_t tx_packets = 0;
	uint64_t tx_bytes = 0;
	uint64_t rx_packets = 0;
	uint64_t rx_bytes = 0;
};

class CountingDatagramSocket : public IDatagramSocket {
public:
	static constexpr std::size_t kMaxPeers = 128;

	explicit CountingDatagramSocket(std::unique_ptr<IDatagramSocket> inner) : inner_(std::move(inner)) {}

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		const int n = inner_ != nullptr ? inner_->recv_from(buf, cap, from) : 0;
		if (n > 0) {
			++totals_.rx_packets;
			totals_.rx_bytes += static_cast<uint64_t>(n);
			if (DatagramTraffic *peer = peer_row(from)) {
				++peer->rx_packets;
				peer->rx_bytes += static_cast<uint64_t>(n);
			}
		}
		return n;
	}

	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		if (inner_ != nullptr) inner_->send_to(to, data, len);
		++totals_.tx_packets;
		totals_.tx_bytes += static_cast<uint64_t>(len);
		if (DatagramTraffic *peer = peer_row(to)) {
			++peer->tx_packets;
			peer->tx_bytes += static_cast<uint64_t>(len);
		}
	}

	const DatagramTraffic &totals() const { return totals_; }
	const std::vector<std::pair<PeerAddr, DatagramTraffic>> &peers() const { return peers_; }
	// The traffic to/from one address; null when it never exchanged a datagram.
	const DatagramTraffic *peer(const PeerAddr &addr) const {
		for (const auto &row : peers_) {
			if (row.first == addr) return &row.second;
		}
		return nullptr;
	}

private:
	DatagramTraffic *peer_row(const PeerAddr &addr) {
		for (auto &row : peers_) {
			if (row.first == addr) return &row.second;
		}
		if (peers_.size() >= kMaxPeers) return nullptr;
		peers_.emplace_back(addr, DatagramTraffic{});
		return &peers_.back().second;
	}

	std::unique_ptr<IDatagramSocket> inner_;
	DatagramTraffic totals_{};
	std::vector<std::pair<PeerAddr, DatagramTraffic>> peers_;
};

} // namespace opennova
