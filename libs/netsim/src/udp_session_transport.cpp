#include "netsim/udp_session_transport.h"

#include <utility>

namespace opennova::netsim {

std::vector<uint8_t> UdpSessionTransport::frame(uint8_t tag, std::vector<uint8_t> body) {
	// Identity frame: prepend the 1-byte tag. The real ProtocolMessage/NWU/CRC/SCRK codec
	// replaces this in the real-socket increment (layered by the owner, not here).
	body.insert(body.begin(), tag);
	return body;
}

bool UdpSessionTransport::unframe(const std::vector<uint8_t> &raw, Datagram &out) {
	if (raw.empty()) return false;
	out.tag = raw[0];
	out.body.assign(raw.begin() + 1, raw.end());
	return true;
}

void UdpSessionTransport::host_send(uint8_t tag, std::vector<uint8_t> body) {
	outbound_.push_back(frame(tag, std::move(body)));
}

void UdpSessionTransport::client_send(uint8_t tag, std::vector<uint8_t> body) {
	outbound_.push_back(frame(tag, std::move(body)));
}

bool UdpSessionTransport::pop_inbound(Datagram &out) {
	if (inbound_.empty()) return false;
	out = std::move(inbound_.front());
	inbound_.pop_front();
	return true;
}

bool UdpSessionTransport::host_recv(Datagram &out) { return pop_inbound(out); }
bool UdpSessionTransport::client_recv(Datagram &out) { return pop_inbound(out); }

void UdpSessionTransport::push_inbound(const std::vector<uint8_t> &raw) {
	Datagram dg;
	if (unframe(raw, dg)) inbound_.push_back(std::move(dg));
}

void UdpSessionTransport::deliver_c2s(uint8_t tag, std::vector<uint8_t> body) {
	// Inject an already-decoded C2S {tag,body} straight onto the host-side inbound FIFO host_recv
	// pops (the consumer already unwrapped the 0x43 SESSION envelope, so this skips unframe).
	inbound_.push_back(Datagram{tag, std::move(body)});
}

bool UdpSessionTransport::pop_outbound(std::vector<uint8_t> &raw) {
	if (outbound_.empty()) return false;
	raw = std::move(outbound_.front());
	outbound_.pop_front();
	return true;
}

void UdpSessionTransport::clear() {
	outbound_.clear();
	inbound_.clear();
}

} // namespace opennova::netsim
