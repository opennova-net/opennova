#pragma once

#include <parity/parity.h>

#include <cstdint>
#include <vector>

namespace opennova::parity_tool::detail {

struct StreamCoverage {
    parity::ProducerIdentity identity{};
    std::uint16_t low_port{};
    std::uint16_t high_port{};
    std::uint64_t raw_datagrams{};
    std::uint64_t decoded_events{};
    std::uint64_t datagrams_with_completed_message{};
    bool typed_events_supplied{};

    [[nodiscard]] std::uint64_t raw_without_completed_message() const noexcept {
        return raw_datagrams - datagrams_with_completed_message;
    }
};

struct EnrichmentResult {
    parity::Trace trace{};
    std::vector<StreamCoverage> streams{};
    std::uint64_t raw_datagrams{};
    std::uint64_t decoded_events{};
    std::uint64_t raw_without_completed_message{};
    bool has_undecodable_stream{};
};

[[nodiscard]] EnrichmentResult enrich_network_events(parity::Trace trace);

}  // namespace opennova::parity_tool::detail
