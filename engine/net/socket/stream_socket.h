#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <net/socket/socket.h> // net::Endpoint

namespace opennova::net {

// The stream-socket seam, the TCP twin of npwire's IDatagramSocket (ADR 0077 d2): what rides a
// stream (the HTTP exchange, net/http/http_exchange.h) speaks to these two and holds no socket of
// its own. The platform's TCP (tcp_stream_connector, net/socket/socket.cpp) is the one
// implementation the programs and the game run; a test may hand the exchange its own.

// One connected stream.
class IStreamSocket {
public:
	virtual ~IStreamSocket() = default;

	// Send all of `data`: false on an error, or when the peer takes none of it for the connect's
	// wait.
	virtual bool send_all(const uint8_t *data, std::size_t len) = 0;

	// One receive, waiting at most `wait_ms` for bytes: the count read (>0), 0 when the peer
	// closed, kStreamRecvTimedOut when none came in the wait, -1 on an error (a reset).
	virtual int recv(uint8_t *buf, std::size_t cap, int wait_ms) = 0;
};

inline constexpr int kStreamRecvTimedOut = -2;

// Opens streams.
class IStreamConnector {
public:
	virtual ~IStreamConnector() = default;

	// A stream to `to`, its connect waiting at most `timeout_ms`; null when the connect fails.
	virtual std::unique_ptr<IStreamSocket> connect(const Endpoint &to, int timeout_ms) = 0;
};

// The platform's TCP as the seam's connector: tcp_connect's stream, its sends bounded by the
// connect's wait (net/socket/socket.h). One process-wide instance, safe from any thread.
IStreamConnector &tcp_stream_connector();

} // namespace opennova::net
