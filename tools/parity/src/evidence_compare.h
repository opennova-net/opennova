#pragma once

#include <parity/parity.h>

#include <cstddef>
#include <string>

namespace opennova::parity_tool::detail {

struct EvidenceValidationResult {
    bool valid{};
    std::size_t producers{};
    std::size_t checkpoints{};
    std::size_t capture_intervals{};
    std::size_t warning_diagnostics{};
    std::string detail{};
    bool client_semantic_available{};
    bool stance_change_observed{};
    bool fired_round_observed{};
    bool reload_request_observed{};
    std::size_t guided_action_warnings{};
};

// Applies the parity core's metadata/checkpoint/state validator, then enforces
// the guided capture contract that the generic core intentionally does not:
// selected checkpoints must carry player and weapon observations, the guided
// client/presented interval must prove a meaningful movement excursion, and a
// multi-producer bundle is exactly one host plus one client with shared run
// context and per-producer raw network evidence for player-combat-loop.
[[nodiscard]] EvidenceValidationResult validate_evidence(
    const parity::Trace& trace);

// Compares already-enriched evidence. Raw NetworkDatagram bytes are never part
// of a verdict; decoded message direction/tag/name ordering is compared within
// capture intervals. Repetition counts are ignored while semantic phase order
// is retained. Manual endpoints and dynamic world/entity transforms are
// neutralized after each trace independently proves meaningful movement; the
// capture-start local-player anchor remains strict.
[[nodiscard]] parity::ComparisonReport compare_evidence(
    parity::Trace reference,
    parity::Trace candidate);

}  // namespace opennova::parity_tool::detail
