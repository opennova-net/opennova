#include <opennova/parity_tool/tool.h>
#include <parity/parity.h>

#include "pcap_reader.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace parity = opennova::parity;
namespace tool = opennova::parity_tool;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

fs::path temporary_path(const char* label, const char* extension) {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return fs::temp_directory_path() /
           (std::string{"opennova-parity-export-"} + label + "-" +
            std::to_string(nonce) + extension);
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes,
                          std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

void write_trace(const fs::path& path) {
    parity::ProducerIdentity identity{
        parity::SourceKind::retail,
        parity::RunRole::client,
        "baseline-client",
    };
    parity::TraceWriter writer;

    parity::NetworkDatagram inbound{};
    inbound.identity = identity;
    inbound.timestamp_ns = 123456789;
    inbound.total_size = 1;
    inbound.direction = parity::DatagramDirection::inbound;
    inbound.source = {"192.0.2.1", 1111};
    inbound.destination = {"198.51.100.2", 2222};
    inbound.payload = {0xaa};
    CHECK(writer.append(inbound));

    parity::NetworkDatagram outbound{};
    outbound.identity = identity;
    outbound.timestamp_ns = 987654321;
    outbound.total_size = 5;
    outbound.truncated = true;
    outbound.direction = parity::DatagramDirection::outbound;
    outbound.source = {"198.51.100.2", 2222};
    outbound.destination = {"192.0.2.1", 1111};
    outbound.payload = {0xbb, 0xcc};
    CHECK(writer.append(outbound));

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    const auto& bytes = writer.bytes();
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void export_preserves_endpoints_direction_timestamps_and_captured_payload() {
    const fs::path trace_path = temporary_path("source", ".ontrace");
    const fs::path pcapng_path = temporary_path("output", ".pcapng");
    std::error_code ignored;
    fs::remove(trace_path, ignored);
    fs::remove(pcapng_path, ignored);
    write_trace(trace_path);

    std::ostringstream output;
    std::ostringstream error;
    CHECK(tool::run(
              {"export-pcapng", trace_path.string(), pcapng_path.string()},
              output,
              error) == 0);
    CHECK(output.str().find("datagrams=2") != std::string::npos);
    CHECK(error.str().empty());

    std::vector<opennova::net::PcapDatagram> datagrams;
    CHECK(opennova::net::read_pcap_udp_file(pcapng_path.string(), datagrams));
    CHECK(datagrams.size() == 2);
    if (datagrams.size() == 2) {
        CHECK(datagrams[0].srcport == 1111);
        CHECK(datagrams[0].dstport == 2222);
        CHECK(datagrams[0].ts_nanos == 123456789);
        CHECK(datagrams[0].payload == std::vector<std::uint8_t>({0xaa}));
        CHECK(datagrams[1].srcport == 2222);
        CHECK(datagrams[1].dstport == 1111);
        CHECK(datagrams[1].ts_nanos == 987654321);
        CHECK(datagrams[1].payload ==
              std::vector<std::uint8_t>({0xbb, 0xcc}));
    }

    const std::vector<std::uint8_t> bytes = read_bytes(pcapng_path);
    CHECK(bytes.size() > 108);
    if (bytes.size() >= 4) {
        CHECK(read_u32_le(bytes, 0) == 0x0a0d0d0aU);
    } else {
        CHECK(false);
    }
    // SHB (28) + IDB (32) + EPB header/body prefix (28) -> raw IPv4.
    constexpr std::size_t first_ipv4 = 88;
    if (bytes.size() >= first_ipv4 + 20) {
        CHECK(bytes[first_ipv4 + 12] == 192);
        CHECK(bytes[first_ipv4 + 13] == 0);
        CHECK(bytes[first_ipv4 + 14] == 2);
        CHECK(bytes[first_ipv4 + 15] == 1);
        CHECK(bytes[first_ipv4 + 16] == 198);
        CHECK(bytes[first_ipv4 + 17] == 51);
        CHECK(bytes[first_ipv4 + 18] == 100);
        CHECK(bytes[first_ipv4 + 19] == 2);
    }

    fs::remove(trace_path, ignored);
    fs::remove(pcapng_path, ignored);
}

}  // namespace

int main() {
    export_preserves_endpoints_direction_timestamps_and_captured_payload();
    std::printf("parity_tool_export: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
