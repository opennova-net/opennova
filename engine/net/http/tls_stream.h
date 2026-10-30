#pragma once

// TLS over the stream seam (net/socket/stream_socket.h): a connected stream wrapped in a TLS
// session that is itself a stream, so what rides one (the HTTP exchange's https://,
// http_exchange.h) speaks TLS 1.2 or 1.3 without knowing it. mbedTLS carries the protocol
// (third_party/mbedtls, built by third_party/mbedtls_config); no header names an mbedTLS type.
// Infrastructure, no port: retail speaks no TLS (OpenNova's own site does: the NovaWorld mods
// list, ADR 0074).

#include <atomic>
#include <memory>
#include <string>

#include <net/socket/stream_socket.h>

namespace opennova::net {

// What a TLS client trusts. The server's chain must lead to one of these certificates, and its
// leaf must name the host the client asked for: verification is never off.
struct TlsTrust {
	// PEM certificates trusted in place of the OS's (a test's CA, a pinned service); empty: the
	// OS's trust store, Windows' ROOT certificate store or the system bundle elsewhere
	// (tls_os_trust_count). No bundle ships with OpenNova.
	std::string ca_pem;
};

// The longest a TLS handshake waits between two looks at its `cancelled`.
inline constexpr int kTlsWaitSliceMs = 50;

// The client side over `inner`: the handshake for `host` (the name it sends, SNI, and the name the
// server's certificate must carry), the server's chain verified against `trust`, the whole
// handshake at most `timeout_ms` (0: no bound) and ended at its next wait once `cancelled` is set.
// Null with `error` (why, a failed verification's reasons among it) when it fails. Each recv of
// the stream then waits at most its own `wait_ms` for the next record.
std::unique_ptr<IStreamSocket> tls_client(std::unique_ptr<IStreamSocket> inner,
		const std::string &host, const TlsTrust &trust, int timeout_ms, std::string &error,
		const std::atomic<bool> *cancelled = nullptr);

// The server side over `inner`, presenting `cert_pem` (its chain, leaf first) with `key_pem`'s
// private key; the handshake at most `timeout_ms` (0: no bound). Null with `error` when it fails.
// A loopback HTTPS peer's (the ctests'), or any program's that serves TLS.
std::unique_ptr<IStreamSocket> tls_server(std::unique_ptr<IStreamSocket> inner,
		const std::string &cert_pem, const std::string &key_pem, int timeout_ms,
		std::string &error);

// How many certificates the OS's trust store gives a client that trusts it (TlsTrust with no
// PEM): 0 when none can be read, and then every such handshake fails.
int tls_os_trust_count();

} // namespace opennova::net
