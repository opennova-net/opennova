#include <parity/parity.h>

#include <utility>

namespace opennova::parity {

namespace {

bool metadata_is_complete(const RunMetadata& metadata) {
    return metadata.identity.source != SourceKind::unknown &&
           metadata.identity.role != RunRole::unknown &&
           !metadata.identity.stream_id.empty() && !metadata.scenario.empty() &&
           !metadata.title.empty() && !metadata.expansion.empty() &&
           !metadata.mission.empty();
}

CaptureRecordResult sink_failure(std::size_t samples_recorded,
                                 std::size_t request_index,
                                 const IEventSink& sink) {
    return {CaptureRecordStatus::sink_failed,
            samples_recorded,
            request_index,
            sink.last_error()};
}

}  // namespace

CaptureRecordResult record_capture(
    ICaptureSource& source,
    IEventSink& sink,
    const std::vector<CaptureRequest>& requests) {
    const RunMetadata metadata = source.metadata();
    if (!metadata_is_complete(metadata)) {
        return {CaptureRecordStatus::invalid_metadata,
                0,
                static_cast<std::size_t>(-1),
                "capture metadata is incomplete"};
    }
    if (!sink.append(Event{metadata})) {
        return sink_failure(0, static_cast<std::size_t>(-1), sink);
    }

    std::size_t samples_recorded = 0;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const CaptureRequest& request = requests[index];
        CaptureSample sample = source.capture(request);
        if (!sample.frame) {
            return {CaptureRecordStatus::capture_failed,
                    samples_recorded,
                    index,
                    std::move(sample.detail)};
        }

        sample.frame->identity = metadata.identity;
        sample.frame->lane = request.lane;
        sample.frame->frame_index = request.frame_index;
        sample.frame->timestamp_ns = request.timestamp_ns;
        if (!sink.append(Event{std::move(*sample.frame)})) {
            return sink_failure(samples_recorded, index, sink);
        }
        ++samples_recorded;
    }
    return {CaptureRecordStatus::complete,
            samples_recorded,
            static_cast<std::size_t>(-1),
            {}};
}

bool TraceWriter::append(const Event& event) {
    return std::visit(
        [this](const auto& value) { return append(value); }, event);
}

}  // namespace opennova::parity
