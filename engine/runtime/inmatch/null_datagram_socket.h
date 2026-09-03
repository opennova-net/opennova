// The socketless datagram seam every role falls back to: nothing to receive,
// every send dropped (the SP listen server, a test rig, a joiner before its
// shell dialed).
#pragma once

#include <net/npwire/idatagram_socket.h>

#include <cstddef>
#include <cstdint>

namespace opennova::inmatch {

class NullDatagramSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

inline opennova::IDatagramSocket &null_datagram_socket() {
	static NullDatagramSocket socket;
	return socket;
}

} // namespace opennova::inmatch
