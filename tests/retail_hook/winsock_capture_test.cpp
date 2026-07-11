#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>

#include <opennova/retail_hook/windows/winsock_capture.h>
#include <opennova/retail_hook/windows/winsock_parity.h>
#include <opennova/retail_hook/capture_agent.h>
#include <parity/parity.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace windows = opennova::retail_hook::windows;

static int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++failures; \
        } \
    } while (0)

struct Captured {
    std::uint64_t timestamp_ns{};
    windows::WireDirection direction{};
    std::uintptr_t socket{};
    windows::WireEndpoint local{};
    windows::WireEndpoint remote{};
    std::vector<std::uint8_t> payload{};
};

class RecordingSink final : public windows::IWireCaptureSink {
public:
    bool accept{true};
    unsigned attempts{};
    std::vector<Captured> records{};

    bool try_capture(const windows::WireCaptureView& view) noexcept override {
        ++attempts;
        if (!accept) {
            return false;
        }
        records.push_back(Captured{
            view.timestamp_ns,
            view.direction,
            view.socket,
            view.local,
            view.remote,
            std::vector<std::uint8_t>(
                view.payload, view.payload + view.payload_size),
        });
        return true;
    }
};

int main() {
    const auto imports = windows::winsock_capture_imports();
    CHECK(imports.size() == 4);
    CHECK(imports[0].call == windows::WinsockCall::receive);
    CHECK(imports[0].ordinal == 16);
    CHECK(imports[1].call == windows::WinsockCall::receive_from);
    CHECK(imports[1].ordinal == 17);
    CHECK(imports[2].call == windows::WinsockCall::send);
    CHECK(imports[2].ordinal == 19);
    CHECK(imports[3].call == windows::WinsockCall::send_to);
    CHECK(imports[3].ordinal == 20);

    const std::array<std::uint8_t, 5> bytes{1, 2, 3, 4, 5};
    RecordingSink sink;
    windows::capture_completed_winsock_call(
        sink,
        windows::WinsockCall::send,
        77,
        bytes.data(),
        bytes.size(),
        3,
        {},
        {});
    CHECK(sink.records.size() == 1);
    CHECK(sink.records[0].direction == windows::WireDirection::outbound);
    CHECK(sink.records[0].timestamp_ns != 0);
    CHECK(sink.records[0].socket == 77);
    CHECK(sink.records[0].payload ==
          std::vector<std::uint8_t>({1, 2, 3}));

    const windows::WireEndpoint remote{
        0x01020304U, 7597, true,
    };
    windows::capture_completed_winsock_call(
        sink,
        windows::WinsockCall::receive_from,
        88,
        bytes.data(),
        bytes.size(),
        5,
        {},
        remote);
    CHECK(sink.records.size() == 2);
    CHECK(sink.records[1].direction == windows::WireDirection::inbound);
    CHECK(sink.records[1].remote.valid);
    CHECK(sink.records[1].remote.port == 7597);

    windows::capture_completed_winsock_call(
        sink,
        windows::WinsockCall::receive,
        99,
        bytes.data(),
        bytes.size(),
        -1,
        {},
        {});
    CHECK(sink.records.size() == 2);

    sink.accept = false;
    const unsigned attempts_before = sink.attempts;
    windows::capture_completed_winsock_call(
        sink,
        windows::WinsockCall::send_to,
        100,
        bytes.data(),
        bytes.size(),
        5,
        {},
        remote);
    CHECK(sink.attempts == attempts_before + 1);

    opennova::retail_hook::CaptureAgent<
        windows::CapturedWireDatagram, 1> queue;
    windows::QueuedWireCaptureSink<1> queued_sink(queue);
    windows::capture_completed_winsock_call(
        queued_sink,
        windows::WinsockCall::send,
        101,
        bytes.data(),
        bytes.size(),
        5,
        {},
        {});
    windows::CapturedWireDatagram queued{};
    CHECK(queue.try_pop(queued));
    CHECK(queued.direction == windows::WireDirection::outbound);
    CHECK(queued.timestamp_ns != 0);
    CHECK(queued.socket == 101);
    CHECK(queued.total_size == 5);
    CHECK(queued.captured_size == 5);
    CHECK(!queued.truncated);
    CHECK(queued.payload[4] == 5);

    CHECK(queue.try_capture(queued));
    windows::capture_completed_winsock_call(
        queued_sink,
        windows::WinsockCall::receive,
        102,
        bytes.data(),
        bytes.size(),
        5,
        {},
        {});
    CHECK(queue.stats().dropped_full == 1);
    CHECK(queue.try_pop(queued));

    windows::CapturedWireDatagram parity_source{};
    parity_source.timestamp_ns = 1234;
    parity_source.direction = windows::WireDirection::outbound;
    parity_source.socket = 55;
    parity_source.local = {htonl(0x7f000001U), 7597, true};
    parity_source.remote = {htonl(0x01020304U), 4000, true};
    parity_source.total_size = 5;
    parity_source.captured_size = 3;
    parity_source.truncated = true;
    parity_source.payload[0] = 9;
    parity_source.payload[1] = 8;
    parity_source.payload[2] = 7;
    const opennova::parity::NetworkDatagram parity_datagram =
        windows::to_parity_datagram(
            parity_source,
            opennova::parity::ProducerIdentity{
                opennova::parity::SourceKind::retail,
                opennova::parity::RunRole::client,
                "wire-test",
            });
    CHECK(parity_datagram.identity.stream_id == "wire-test");
    CHECK(parity_datagram.timestamp_ns == 1234);
    CHECK(parity_datagram.socket_id == 55);
    CHECK(parity_datagram.total_size == 5);
    CHECK(parity_datagram.truncated);
    CHECK(parity_datagram.direction ==
          opennova::parity::DatagramDirection::outbound);
    CHECK(parity_datagram.source.address == "127.0.0.1");
    CHECK(parity_datagram.source.port == 7597);
    CHECK(parity_datagram.destination.address == "1.2.3.4");
    CHECK(parity_datagram.destination.port == 4000);
    CHECK(parity_datagram.payload ==
          std::vector<std::uint8_t>({9, 8, 7}));

    WSADATA winsock{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(receiver != INVALID_SOCKET);
    CHECK(sender != INVALID_SOCKET);
    DWORD timeout_ms = 1000;
    CHECK(setsockopt(
              receiver,
              SOL_SOCKET,
              SO_RCVTIMEO,
              reinterpret_cast<const char*>(&timeout_ms),
              sizeof(timeout_ms)) == 0);

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = 0;
    CHECK(bind(
              receiver,
              reinterpret_cast<const sockaddr*>(&destination),
              sizeof(destination)) == 0);
    int destination_size = sizeof(destination);
    CHECK(getsockname(
              receiver,
              reinterpret_cast<sockaddr*>(&destination),
              &destination_size) == 0);

    // Keep the connected-call imports present too; the supported retail image
    // imports all four calls even though this integration leg uses UDP.
    if (GetTickCount() == std::numeric_limits<DWORD>::max()) {
        char unused{};
        (void)send(sender, &unused, 1, 0);
        (void)recv(receiver, &unused, 1, 0);
    }

    opennova::retail_hook::CaptureAgent<
        windows::CapturedWireDatagram, 4> live_queue;
    windows::QueuedWireCaptureSink<4> live_sink(live_queue);
    const windows::WinsockHookInstallResult installed =
        windows::install_winsock_capture(GetModuleHandleW(nullptr), live_sink);
    CHECK(installed.installed(windows::WinsockCall::receive));
    CHECK(installed.installed(windows::WinsockCall::receive_from));
    CHECK(installed.installed(windows::WinsockCall::send));
    CHECK(installed.installed(windows::WinsockCall::send_to));

    CHECK(sendto(
              sender,
              reinterpret_cast<const char*>(bytes.data()),
              static_cast<int>(bytes.size()),
              0,
              reinterpret_cast<const sockaddr*>(&destination),
              sizeof(destination)) == static_cast<int>(bytes.size()));
    std::array<std::uint8_t, 16> received{};
    sockaddr_in source{};
    int source_size = sizeof(source);
    CHECK(recvfrom(
              receiver,
              reinterpret_cast<char*>(received.data()),
              static_cast<int>(received.size()),
              0,
              reinterpret_cast<sockaddr*>(&source),
              &source_size) == static_cast<int>(bytes.size()));

    windows::CapturedWireDatagram sent_record{};
    windows::CapturedWireDatagram received_record{};
    CHECK(live_queue.try_pop(sent_record));
    CHECK(live_queue.try_pop(received_record));
    CHECK(sent_record.direction == windows::WireDirection::outbound);
    CHECK(received_record.direction == windows::WireDirection::inbound);
    CHECK(sent_record.timestamp_ns != 0);
    CHECK(received_record.timestamp_ns >= sent_record.timestamp_ns);
    CHECK(sent_record.local.valid);
    CHECK(sent_record.remote.valid);
    CHECK(sent_record.remote.port == ntohs(destination.sin_port));
    CHECK(received_record.local.valid);
    CHECK(received_record.remote.valid);
    CHECK(received_record.local.port == ntohs(destination.sin_port));
    CHECK(sent_record.captured_size == bytes.size());
    CHECK(received_record.captured_size == bytes.size());
    CHECK(windows::uninstall_winsock_capture(GetModuleHandleW(nullptr)));

    closesocket(sender);
    closesocket(receiver);
    WSACleanup();

    std::printf("retail_hook_winsock_capture: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
