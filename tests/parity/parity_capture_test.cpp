#include <parity/parity.h>

#include <cstdio>
#include <string>
#include <vector>

namespace parity = opennova::parity;

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

class ScriptedCaptureSource final : public parity::ICaptureSource {
public:
    parity::RunMetadata metadata() const override {
        parity::RunMetadata result{};
        result.run_id = "retail-run";
        result.producer = "retail-hook";
        result.build_id = "1.7.5.7";
        result.scenario = "manual-combat";
        result.identity = {
            parity::SourceKind::retail,
            parity::RunRole::client,
            "retail-client"};
        result.title = "joint-operations";
        result.expansion = "revx02";
        result.mission = "operation-barracuda";
        return result;
    }

    parity::CaptureSample capture(
        const parity::CaptureRequest& request) override {
        if (request.frame_index == 12) {
            return {{}, "retail frame was not readable"};
        }
        parity::FrameSnapshot snapshot{};
        snapshot.identity = {
            parity::SourceKind::opennova,
            parity::RunRole::host,
            "wrong-stream"};
        snapshot.lane = parity::StateLane::unknown;
        snapshot.frame_index = 999;
        snapshot.timestamp_ns = 999;
        snapshot.simulation_tick = request.frame_index * 2;
        parity::PlayerState player{};
        player.present = true;
        player.health = 75;
        snapshot.player = player;
        return {std::move(snapshot), {}};
    }
};

void recorder_stamps_provenance_and_writes_through_event_sink() {
    ScriptedCaptureSource source;
    parity::TraceWriter writer;
    parity::IEventSink& sink = writer;
    const std::vector<parity::CaptureRequest> requests{
        {parity::StateLane::authoritative, 10, 1000},
        {parity::StateLane::presented, 11, 1100},
    };

    const parity::CaptureRecordResult result =
        parity::record_capture(source, sink, requests);
    CHECK(result.complete());
    CHECK(result.status == parity::CaptureRecordStatus::complete);
    CHECK(result.samples_recorded == 2);

    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 3);
    if (read.trace.events.size() == 3) {
        const auto& metadata = std::get<parity::RunMetadata>(read.trace.events[0]);
        CHECK(metadata.identity.source == parity::SourceKind::retail);
        CHECK(metadata.identity.stream_id == "retail-client");
        const auto& first = std::get<parity::FrameSnapshot>(read.trace.events[1]);
        const auto& second = std::get<parity::FrameSnapshot>(read.trace.events[2]);
        CHECK(first.identity.source == parity::SourceKind::retail);
        CHECK(first.identity.role == parity::RunRole::client);
        CHECK(first.identity.stream_id == "retail-client");
        CHECK(first.lane == parity::StateLane::authoritative);
        CHECK(first.frame_index == 10);
        CHECK(first.timestamp_ns == 1000);
        CHECK(first.simulation_tick == 20);
        CHECK(second.identity.source == parity::SourceKind::retail);
        CHECK(second.identity.role == parity::RunRole::client);
        CHECK(second.identity.stream_id == "retail-client");
        CHECK(second.lane == parity::StateLane::presented);
        CHECK(second.frame_index == 11);
        CHECK(second.timestamp_ns == 1100);
    }
}

void recorder_stops_on_missing_sample_with_specific_evidence() {
    ScriptedCaptureSource source;
    parity::TraceWriter writer;
    const std::vector<parity::CaptureRequest> requests{
        {parity::StateLane::authoritative, 10, 1000},
        {parity::StateLane::authoritative, 12, 1200},
        {parity::StateLane::authoritative, 13, 1300},
    };

    const parity::CaptureRecordResult result =
        parity::record_capture(source, writer, requests);
    CHECK(!result.complete());
    CHECK(result.status == parity::CaptureRecordStatus::capture_failed);
    CHECK(result.samples_recorded == 1);
    CHECK(result.failed_request_index == 1);
    CHECK(result.detail == "retail frame was not readable");

    const parity::TraceReadResult read = parity::read_trace(writer.bytes());
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 2);
}

}  // namespace

int main() {
    recorder_stamps_provenance_and_writes_through_event_sink();
    recorder_stops_on_missing_sample_with_specific_evidence();
    std::printf("parity_capture: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
