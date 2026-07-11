#include <opennova/retail_hook/windows/winsock_capture.h>

#include <algorithm>
#include <chrono>

namespace opennova::retail_hook::windows {
namespace {

// The supported Jointops.exe imports these four calls from WS2_32 by ordinal.
// No WSARecv/WSASend family entry is present in its import table.
constexpr std::array<WinsockImport, 4> kCapturedImports{{
    {WinsockCall::receive, 16, "recv"},
    {WinsockCall::receive_from, 17, "recvfrom"},
    {WinsockCall::send, 19, "send"},
    {WinsockCall::send_to, 20, "sendto"},
}};

}  // namespace

const std::array<WinsockImport, 4>& winsock_capture_imports() noexcept {
    return kCapturedImports;
}

void capture_completed_winsock_call(
    IWireCaptureSink& sink,
    WinsockCall call,
    std::uintptr_t socket,
    const std::uint8_t* payload,
    std::size_t available_payload_size,
    int completed_result,
    WireEndpoint local,
    WireEndpoint remote) noexcept {
    if (payload == nullptr || completed_result <= 0) {
        return;
    }

    const std::size_t completed = static_cast<std::size_t>(completed_result);
    const std::size_t payload_size =
        std::min(completed, available_payload_size);
    if (payload_size == 0) {
        return;
    }

    const bool inbound =
        call == WinsockCall::receive || call == WinsockCall::receive_from;
    const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const WireCaptureView view{
        timestamp > 0 ? static_cast<std::uint64_t>(timestamp) : 1U,
        inbound ? WireDirection::inbound : WireDirection::outbound,
        socket,
        local,
        remote,
        payload,
        payload_size,
    };
    (void)sink.try_capture(view);
}

}  // namespace opennova::retail_hook::windows
