#include "pcapng_export.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

namespace opennova::parity_tool::detail {
namespace {

constexpr std::uint32_t kSectionHeaderBlock = 0x0a0d0d0aU;
constexpr std::uint32_t kInterfaceDescriptionBlock = 0x00000001U;
constexpr std::uint32_t kEnhancedPacketBlock = 0x00000006U;
constexpr std::uint16_t kLinktypeRaw = 101;
constexpr std::size_t kIpv4HeaderBytes = 20;
constexpr std::size_t kUdpHeaderBytes = 8;
constexpr std::size_t kMaximumUdpPayloadBytes =
    std::numeric_limits<std::uint16_t>::max() -
    kIpv4HeaderBytes - kUdpHeaderBytes;

void put_u16_le(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_u32_le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void put_u16_be(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

bool write_bytes(std::ofstream& stream,
                 const std::vector<std::uint8_t>& bytes) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

std::vector<std::uint8_t> section_header_block() {
    std::vector<std::uint8_t> block;
    block.reserve(28);
    put_u32_le(block, kSectionHeaderBlock);
    put_u32_le(block, 28);
    put_u32_le(block, 0x1a2b3c4dU);
    put_u16_le(block, 1);
    put_u16_le(block, 0);
    put_u32_le(block, 0xffffffffU);
    put_u32_le(block, 0xffffffffU);
    put_u32_le(block, 28);
    return block;
}

std::vector<std::uint8_t> interface_description_block() {
    std::vector<std::uint8_t> block;
    block.reserve(32);
    put_u32_le(block, kInterfaceDescriptionBlock);
    put_u32_le(block, 32);
    put_u16_le(block, kLinktypeRaw);
    put_u16_le(block, 0);
    put_u32_le(block, 65535);
    // if_tsresol = 10^-9 seconds, followed by 4-byte option padding.
    put_u16_le(block, 9);
    put_u16_le(block, 1);
    block.push_back(9);
    block.insert(block.end(), 3, 0);
    put_u16_le(block, 0);
    put_u16_le(block, 0);
    put_u32_le(block, 32);
    return block;
}

bool parse_ipv4(std::string_view text, std::array<std::uint8_t, 4>& result) {
    std::size_t begin = 0;
    for (std::size_t octet = 0; octet < result.size(); ++octet) {
        const std::size_t end = text.find('.', begin);
        const bool last = octet + 1 == result.size();
        if ((last && end != std::string_view::npos) ||
            (!last && end == std::string_view::npos)) {
            return false;
        }
        const std::size_t component_end = last ? text.size() : end;
        if (component_end == begin) {
            return false;
        }
        unsigned value = 0;
        const char* first = text.data() + begin;
        const char* final = text.data() + component_end;
        const auto parsed = std::from_chars(first, final, value);
        if (parsed.ec != std::errc{} || parsed.ptr != final || value > 255) {
            return false;
        }
        result[octet] = static_cast<std::uint8_t>(value);
        begin = component_end + 1;
    }
    return true;
}

std::uint16_t ipv4_checksum(const std::vector<std::uint8_t>& header) {
    std::uint32_t sum = 0;
    for (std::size_t index = 0; index + 1 < header.size(); index += 2) {
        sum += (static_cast<std::uint32_t>(header[index]) << 8U) |
               header[index + 1];
    }
    while ((sum >> 16U) != 0) {
        sum = (sum & 0xffffU) + (sum >> 16U);
    }
    return static_cast<std::uint16_t>(~sum & 0xffffU);
}

bool packet_block(const parity::NetworkDatagram& datagram,
                  std::vector<std::uint8_t>& block,
                  std::string& detail) {
    const std::size_t captured_payload_size = datagram.payload.size();
    std::size_t original_payload_size = datagram.total_size == 0
                                            ? captured_payload_size
                                            : datagram.total_size;
    if (original_payload_size < captured_payload_size) {
        detail = "datagram total_size is smaller than captured payload";
        return false;
    }
    if (!datagram.truncated && original_payload_size != captured_payload_size) {
        detail = "non-truncated datagram size does not match captured payload";
        return false;
    }
    if (captured_payload_size > kMaximumUdpPayloadBytes ||
        original_payload_size > kMaximumUdpPayloadBytes) {
        detail = "datagram is too large for an IPv4/UDP packet";
        return false;
    }

    std::array<std::uint8_t, 4> source{};
    std::array<std::uint8_t, 4> destination{};
    if (!parse_ipv4(datagram.source.address, source) ||
        !parse_ipv4(datagram.destination.address, destination)) {
        detail = "pcapng export requires dotted IPv4 source and destination addresses";
        return false;
    }

    const auto original_ip_size = static_cast<std::uint16_t>(
        kIpv4HeaderBytes + kUdpHeaderBytes + original_payload_size);
    const auto original_udp_size = static_cast<std::uint16_t>(
        kUdpHeaderBytes + original_payload_size);

    std::vector<std::uint8_t> packet;
    packet.reserve(kIpv4HeaderBytes + kUdpHeaderBytes + captured_payload_size);
    packet.push_back(0x45);
    packet.push_back(0);
    put_u16_be(packet, original_ip_size);
    put_u16_be(packet, 0);
    put_u16_be(packet, 0x4000);
    packet.push_back(64);
    packet.push_back(17);
    put_u16_be(packet, 0);
    packet.insert(packet.end(), source.begin(), source.end());
    packet.insert(packet.end(), destination.begin(), destination.end());
    const std::uint16_t checksum = ipv4_checksum(packet);
    packet[10] = static_cast<std::uint8_t>(checksum >> 8U);
    packet[11] = static_cast<std::uint8_t>(checksum);
    put_u16_be(packet, datagram.source.port);
    put_u16_be(packet, datagram.destination.port);
    put_u16_be(packet, original_udp_size);
    put_u16_be(packet, 0);
    packet.insert(packet.end(), datagram.payload.begin(), datagram.payload.end());

    const std::size_t padded_packet_size = (packet.size() + 3U) & ~std::size_t{3U};
    const std::size_t block_size = 32U + padded_packet_size;
    if (block_size > std::numeric_limits<std::uint32_t>::max()) {
        detail = "pcapng packet block is too large";
        return false;
    }
    block.clear();
    block.reserve(block_size);
    put_u32_le(block, kEnhancedPacketBlock);
    put_u32_le(block, static_cast<std::uint32_t>(block_size));
    put_u32_le(block, 0);
    put_u32_le(block, static_cast<std::uint32_t>(datagram.timestamp_ns >> 32U));
    put_u32_le(block, static_cast<std::uint32_t>(datagram.timestamp_ns));
    put_u32_le(block, static_cast<std::uint32_t>(packet.size()));
    put_u32_le(block, static_cast<std::uint32_t>(original_ip_size));
    block.insert(block.end(), packet.begin(), packet.end());
    block.resize(28U + padded_packet_size, 0);
    put_u32_le(block, static_cast<std::uint32_t>(block_size));
    return true;
}

}  // namespace

PcapngWriteResult write_pcapng(const parity::Trace& trace,
                               const std::filesystem::path& output_path) {
    std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return {PcapngWriteStatus::io_error,
                0,
                "could not open pcapng output: " + output_path.string()};
    }
    if (!write_bytes(stream, section_header_block()) ||
        !write_bytes(stream, interface_description_block())) {
        return {PcapngWriteStatus::io_error,
                0,
                "could not write pcapng header: " + output_path.string()};
    }

    std::size_t count = 0;
    std::vector<std::uint8_t> block;
    for (const parity::Event& event : trace.events) {
        const auto* datagram = std::get_if<parity::NetworkDatagram>(&event);
        if (datagram == nullptr) {
            continue;
        }
        std::string detail;
        if (!packet_block(*datagram, block, detail)) {
            stream.close();
            std::error_code ignored;
            std::filesystem::remove(output_path, ignored);
            return {PcapngWriteStatus::invalid_evidence, count, std::move(detail)};
        }
        if (!write_bytes(stream, block)) {
            return {PcapngWriteStatus::io_error,
                    count,
                    "could not write pcapng packet: " + output_path.string()};
        }
        ++count;
    }
    stream.flush();
    if (!stream) {
        return {PcapngWriteStatus::io_error,
                count,
                "could not finish pcapng output: " + output_path.string()};
    }
    if (count == 0) {
        stream.close();
        std::error_code ignored;
        std::filesystem::remove(output_path, ignored);
        return {PcapngWriteStatus::invalid_evidence,
                0,
                "trace contains no raw network datagrams"};
    }
    return {PcapngWriteStatus::complete, count, {}};
}

}  // namespace opennova::parity_tool::detail
