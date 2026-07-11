#pragma once

#include <opennova/retail_hook/validation_session.h>
#include <parity/parity.h>

namespace opennova::retail_hook {

class CaptureCheckpointTracker {
public:
    [[nodiscard]] std::optional<parity::Checkpoint> accepted_frame(
        const parity::FrameSnapshot& frame);
    [[nodiscard]] std::optional<parity::Checkpoint> finish() const;

private:
    bool started_{};
    parity::ProducerIdentity identity_{};
    parity::StateLane lane_{parity::StateLane::unknown};
    std::uint64_t last_ready_frame_{};
};

[[nodiscard]] parity::FrameSnapshot to_parity_snapshot(
    const ValidationSnapshot& snapshot,
    const parity::ProducerIdentity& identity,
    parity::StateLane lane,
    std::uint64_t frame_index,
    std::uint64_t simulation_tick,
    std::uint64_t timestamp_ns);

[[nodiscard]] parity::MutationAudit to_parity_mutation_audit(
    const MutationResult& result,
    const parity::ProducerIdentity& identity,
    std::uint64_t simulation_tick,
    std::uint64_t timestamp_ns);

}  // namespace opennova::retail_hook
