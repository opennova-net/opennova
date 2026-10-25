#include <net/npwire/datagram_demux.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

namespace opennova {

DatagramDemux::DatagramDemux(IDatagramSocket &socket, std::size_t session_lanes)
	: socket_(socket), game_view_(*this, game_queue_) {
	const std::size_t lanes = std::max<std::size_t>(session_lanes, 1);
	for (std::size_t i = 0; i < lanes; ++i) lanes_.push_back(std::make_unique<Lane>(*this));
}

void DatagramDemux::set_session_claim(Claim claim, std::size_t lane) {
	assert(lane < lanes_.size());
	lanes_[lane]->claim = std::move(claim);
	lanes_[lane]->queue.clear();
}

IDatagramSocket &DatagramDemux::session(std::size_t lane) {
	assert(lane < lanes_.size());
	return lanes_[lane]->view;
}

void DatagramDemux::set_game_attached(bool attached) {
	game_attached_ = attached;
	game_queue_.clear();
}

// The manager's receive pump: each datagram to the first protocol that takes
// it, the sessions' claims first in lane order, then the game protocol when one
// is on the list; nothing else takes it.
// [orig: NapiNPManager_PumpReceive @0x623010 -> NapiNPManager_HandlePacket
//  @0x622f10 — the walk @0x622f6c..0x622fac, cb_on_error(4) @0x622fdc]
void DatagramDemux::pump() {
	uint8_t buf[65536];
	for (;;) {
		PeerAddr from{};
		const int n = socket_.recv_from(buf, sizeof(buf), from);
		if (n <= 0) return;
		const std::size_t len = static_cast<std::size_t>(n);
		std::deque<Datagram> *queue = nullptr;
		for (const std::unique_ptr<Lane> &lane : lanes_) {
			if (lane->claim && lane->claim(from, buf, len)) {
				queue = &lane->queue;
				break;
			}
		}
		if (queue == nullptr && game_attached_) queue = &game_queue_;
		if (queue == nullptr) {
			++dropped_;
			continue;
		}
		queue->push_back(Datagram{from, std::vector<uint8_t>(buf, buf + len)});
	}
}

int DatagramDemux::View::recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) {
	if (queue_.empty()) owner_.pump();
	if (queue_.empty()) return 0;
	Datagram dg = std::move(queue_.front());
	queue_.pop_front();
	from = dg.from;
	const std::size_t n = std::min(cap, dg.bytes.size());
	if (n > 0) std::memcpy(buf, dg.bytes.data(), n);
	return static_cast<int>(n);
}

void DatagramDemux::View::send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) {
	owner_.socket_.send_to(to, data, len);
}

} // namespace opennova
