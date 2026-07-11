#pragma once

#include <parity/parity.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace opennova::parity_tool::detail {

enum class PcapngWriteStatus {
    complete,
    invalid_evidence,
    io_error,
};

struct PcapngWriteResult {
    PcapngWriteStatus status{PcapngWriteStatus::complete};
    std::size_t datagrams{};
    std::string detail{};

    [[nodiscard]] bool complete() const noexcept {
        return status == PcapngWriteStatus::complete;
    }
};

// Writes one pcapng Enhanced Packet Block per raw datagram. Packet data is a
// synthetic IPv4/UDP envelope around exactly the bytes captured in the trace.
// The writer retains no whole-file buffer.
[[nodiscard]] PcapngWriteResult write_pcapng(
    const parity::Trace& trace,
    const std::filesystem::path& output_path);

}  // namespace opennova::parity_tool::detail
