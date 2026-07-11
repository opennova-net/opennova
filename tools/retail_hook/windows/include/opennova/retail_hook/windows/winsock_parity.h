#pragma once

#include <opennova/retail_hook/windows/winsock_capture.h>
#include <parity/parity.h>

namespace opennova::retail_hook::windows {

[[nodiscard]] parity::NetworkDatagram to_parity_datagram(
    const CapturedWireDatagram& captured,
    const parity::ProducerIdentity& identity);

}  // namespace opennova::retail_hook::windows
