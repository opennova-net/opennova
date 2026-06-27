#include "netsim/loopback_channel.h"

#include <utility>

namespace opennova::netsim {

void LoopbackChannel::host_send(uint8_t tag, std::vector<uint8_t> body) {
	s2c_.push_back(Datagram{tag, std::move(body)});
}

void LoopbackChannel::client_send(uint8_t tag, std::vector<uint8_t> body) {
	c2s_.push_back(Datagram{tag, std::move(body)});
}

bool LoopbackChannel::host_recv(Datagram &out) {
	if (c2s_.empty()) return false;
	out = std::move(c2s_.front());
	c2s_.pop_front();
	return true;
}

bool LoopbackChannel::client_recv(Datagram &out) {
	if (s2c_.empty()) return false;
	out = std::move(s2c_.front());
	s2c_.pop_front();
	return true;
}

void LoopbackChannel::deliver_c2s(uint8_t tag, std::vector<uint8_t> body) {
	// Inject onto the C2S FIFO host_recv pops (same queue client_send feeds — for the in-process
	// loopback the host-side inject IS the C2S queue).
	c2s_.push_back(Datagram{tag, std::move(body)});
}

void LoopbackChannel::clear() {
	s2c_.clear();
	c2s_.clear();
}

} // namespace opennova::netsim
