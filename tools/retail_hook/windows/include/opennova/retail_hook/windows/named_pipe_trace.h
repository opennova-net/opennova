#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::retail_hook::windows {

struct PipeRecordResult {
    bool success{};
    std::uint32_t win32_error{};
    std::uint64_t bytes_written{};
    std::uint32_t producers_connected{};
    std::uint32_t producers_completed{};
    std::uint64_t events_merged{};
    std::uint64_t invalid_chunks{};
    std::string detail{};
};

enum class PipeProducerTopology {
    any,
    retail_baseline,
    opennova_host_retail_client,
};

inline constexpr std::uint64_t kDefaultMaximumTraceBytes =
    256ULL * 1024ULL * 1024ULL;

struct PipeRecordOptions {
    std::uint32_t expected_producers{2};
    std::string expected_scenario{};
    PipeProducerTopology topology{PipeProducerTopology::any};
    std::uint32_t producer_connect_timeout_ms{180000};
    std::uint64_t maximum_trace_bytes{kDefaultMaximumTraceBytes};
};

// Accepts all producer streams concurrently, consumes each ONPT header, and
// serializes only complete validated chunks beneath one output header.
[[nodiscard]] PipeRecordResult record_trace_pipe(
    const std::wstring& pipe_name,
    const std::wstring& trace_path,
    const PipeRecordOptions& options = {});

}  // namespace opennova::retail_hook::windows
