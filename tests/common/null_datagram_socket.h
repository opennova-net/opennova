// The socketless in-match host the headless ctests drive: no datagrams in or
// out (the loopback carries the host's own client; a dedicated test host has
// no peers). One definition for every test that pumps inmatch::HostRole or
// inmatch::host_session_pump over a null wire.
#pragma once

#include <net/npwire/idatagram_socket.h>

#include <cstddef>
#include <cstdint>

namespace opennova::testrig {

class NullDatagramSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

} // namespace opennova::testrig
