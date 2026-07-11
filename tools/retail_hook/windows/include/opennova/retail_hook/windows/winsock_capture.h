#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <opennova/retail_hook/capture_agent.h>

namespace opennova::retail_hook::windows {

enum class WinsockCall : std::uint8_t {
    receive,
    receive_from,
    send,
    send_to,
};

struct WinsockImport {
    WinsockCall call{};
    std::uint16_t ordinal{};
    const char* name{};
};

[[nodiscard]] const std::array<WinsockImport, 4>&
winsock_capture_imports() noexcept;

enum class WireDirection : std::uint8_t {
    inbound,
    outbound,
};

struct WireEndpoint {
    // IPv4 address in network byte order and port in host byte order.
    std::uint32_t address_v4{};
    std::uint16_t port{};
    bool valid{};
};

struct WireCaptureView {
    // Monotonic steady-clock nanoseconds, suitable for ordering within a run.
    std::uint64_t timestamp_ns{};
    WireDirection direction{};
    std::uintptr_t socket{};
    WireEndpoint local{};
    WireEndpoint remote{};
    const std::uint8_t* payload{};
    std::size_t payload_size{};
};

class IWireCaptureSink {
public:
    virtual ~IWireCaptureSink() = default;
    [[nodiscard]] virtual bool try_capture(
        const WireCaptureView& capture) noexcept = 0;
};

inline constexpr std::size_t kMaximumCapturedDatagramBytes = 2048;

struct CapturedWireDatagram {
    std::uint64_t timestamp_ns{};
    WireDirection direction{};
    std::uintptr_t socket{};
    WireEndpoint local{};
    WireEndpoint remote{};
    std::size_t total_size{};
    std::size_t captured_size{};
    bool truncated{};
    std::array<std::uint8_t, kMaximumCapturedDatagramBytes> payload{};
};

template <std::size_t Capacity>
class QueuedWireCaptureSink final : public IWireCaptureSink {
public:
    explicit QueuedWireCaptureSink(
        CaptureAgent<CapturedWireDatagram, Capacity>& agent) noexcept
        : agent_(agent) {}

    [[nodiscard]] bool try_capture(
        const WireCaptureView& capture) noexcept override {
        CapturedWireDatagram record{};
        record.timestamp_ns = capture.timestamp_ns;
        record.direction = capture.direction;
        record.socket = capture.socket;
        record.local = capture.local;
        record.remote = capture.remote;
        record.total_size = capture.payload_size;
        record.captured_size = std::min(
            capture.payload_size, record.payload.size());
        record.truncated = record.captured_size != record.total_size;
        if (record.captured_size != 0) {
            std::memcpy(
                record.payload.data(), capture.payload, record.captured_size);
        }
        return agent_.try_capture(std::move(record));
    }

private:
    CaptureAgent<CapturedWireDatagram, Capacity>& agent_;
};

// Adapts the result of a synchronous imported Winsock call to the capture
// boundary. It performs one best-effort sink call and never retries or waits.
void capture_completed_winsock_call(
    IWireCaptureSink& sink,
    WinsockCall call,
    std::uintptr_t socket,
    const std::uint8_t* payload,
    std::size_t available_payload_size,
    int completed_result,
    WireEndpoint local,
    WireEndpoint remote) noexcept;

struct WinsockHookInstallResult {
    std::uint8_t installed_mask{};

    [[nodiscard]] bool installed(WinsockCall call) const noexcept {
        const auto bit = static_cast<std::uint8_t>(
            1U << static_cast<unsigned>(call));
        return (installed_mask & bit) != 0;
    }

    [[nodiscard]] bool complete() const noexcept {
        return installed_mask == 0x0fU;
    }
};

// Installs/restores IAT hooks in a loaded PE image. The supported retail image
// imports the four calls above from WS2_32; no other socket call is patched.
[[nodiscard]] WinsockHookInstallResult install_winsock_capture(
    void* image_module,
    IWireCaptureSink& sink) noexcept;
[[nodiscard]] bool uninstall_winsock_capture(void* image_module) noexcept;

}  // namespace opennova::retail_hook::windows
