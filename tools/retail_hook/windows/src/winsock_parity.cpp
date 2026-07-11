#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>

#include <opennova/retail_hook/windows/winsock_parity.h>

#include <algorithm>
#include <limits>
#include <string>

namespace opennova::retail_hook::windows {
namespace {

[[nodiscard]] parity::NetworkEndpoint to_parity_endpoint(
    const WireEndpoint& endpoint) {
    if (!endpoint.valid) {
        return {};
    }
    const std::uint32_t host_address = ntohl(endpoint.address_v4);
    return parity::NetworkEndpoint{
        std::to_string((host_address >> 24U) & 0xffU) + "." +
            std::to_string((host_address >> 16U) & 0xffU) + "." +
            std::to_string((host_address >> 8U) & 0xffU) + "." +
            std::to_string(host_address & 0xffU),
        endpoint.port,
    };
}

}  // namespace

parity::NetworkDatagram to_parity_datagram(
    const CapturedWireDatagram& captured,
    const parity::ProducerIdentity& identity) {
    parity::NetworkDatagram datagram{};
    datagram.identity = identity;
    datagram.timestamp_ns = captured.timestamp_ns;
    datagram.socket_id = captured.socket;
    datagram.total_size = captured.total_size >
            std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(captured.total_size);
    datagram.truncated = captured.truncated;
    const bool inbound = captured.direction == WireDirection::inbound;
    datagram.direction = inbound
        ? parity::DatagramDirection::inbound
        : parity::DatagramDirection::outbound;
    const parity::NetworkEndpoint local =
        to_parity_endpoint(captured.local);
    const parity::NetworkEndpoint remote =
        to_parity_endpoint(captured.remote);
    datagram.source = inbound ? remote : local;
    datagram.destination = inbound ? local : remote;
    const std::size_t captured_size = std::min(
        captured.captured_size, captured.payload.size());
    datagram.payload.assign(
        captured.payload.begin(),
        captured.payload.begin() + static_cast<std::ptrdiff_t>(captured_size));
    return datagram;
}

}  // namespace opennova::retail_hook::windows
