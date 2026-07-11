#include <parity/file_event_sink.h>
#include <parity/parity.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace parity = opennova::parity;
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

fs::path temporary_trace(const char* label) {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return fs::temp_directory_path() /
           (std::string{"opennova-parity-"} + label + "-" +
            std::to_string(nonce) + ".ontrace");
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

parity::RunMetadata metadata() {
    parity::RunMetadata value{};
    value.run_id = "run-1";
    value.producer = "file-sink-test";
    value.build_id = "synthetic";
    value.scenario = "player-combat-loop";
    value.started_unix_ns = 10;
    value.identity = {
        parity::SourceKind::opennova,
        parity::RunRole::host,
        "candidate-host",
    };
    value.title = "joint-operations";
    value.expansion = "revx02";
    value.mission = "ASH_G3D.bms";
    return value;
}

parity::Checkpoint checkpoint() {
    parity::Checkpoint value{};
    value.name = "spawn-idle";
    value.occurrence = 0;
    value.frame_index = 12;
    value.identity = metadata().identity;
    value.lane = parity::StateLane::authoritative;
    return value;
}

void appends_one_header_then_one_complete_chunk_per_event() {
    const fs::path path = temporary_trace("incremental");
    std::error_code ignored;
    fs::remove(path, ignored);

    std::string error;
    std::unique_ptr<parity::FileEventSink> sink =
        parity::FileEventSink::open(path, {}, &error);
    CHECK(sink != nullptr);
    CHECK(error.empty());
    if (!sink) return;

    const std::vector<std::uint8_t> header = read_bytes(path);
    CHECK(header == std::vector<std::uint8_t>(
                        {0x4f, 0x4e, 0x50, 0x54, 0x01, 0x00, 0x00, 0x00}));

    parity::TraceWriter metadata_writer;
    CHECK(metadata_writer.append(metadata()));
    CHECK(sink->append(parity::Event{metadata()}));
    const std::vector<std::uint8_t> after_metadata = read_bytes(path);
    CHECK(after_metadata == metadata_writer.bytes());
    CHECK(sink->bytes_written() == after_metadata.size());

    parity::TraceWriter checkpoint_writer;
    CHECK(checkpoint_writer.append(checkpoint()));
    CHECK(sink->append(parity::Event{checkpoint()}));
    CHECK(sink->flush());

    const std::vector<std::uint8_t> complete = read_bytes(path);
    CHECK(complete.size() == after_metadata.size() +
                                 checkpoint_writer.bytes().size() -
                                 header.size());
    const parity::TraceReadResult read = parity::read_trace(complete);
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 2);

    sink.reset();
    fs::remove(path, ignored);
}

void rejects_an_event_before_exceeding_the_file_bound() {
    parity::TraceWriter one_event;
    CHECK(one_event.append(metadata()));

    parity::FileEventSinkOptions options{};
    options.max_file_bytes = one_event.bytes().size();
    const fs::path path = temporary_trace("bounded");
    std::error_code ignored;
    fs::remove(path, ignored);

    std::string error;
    std::unique_ptr<parity::FileEventSink> sink =
        parity::FileEventSink::open(path, options, &error);
    CHECK(sink != nullptr);
    if (!sink) return;

    CHECK(sink->append(parity::Event{metadata()}));
    const std::uintmax_t full_size = fs::file_size(path);
    CHECK(full_size == options.max_file_bytes);
    CHECK(!sink->append(parity::Event{checkpoint()}));
    CHECK(sink->last_error().find("maximum") != std::string::npos);
    CHECK(fs::file_size(path) == full_size);

    const parity::TraceReadResult read = parity::read_trace(read_bytes(path));
    CHECK(read.complete());
    CHECK(read.trace.events.size() == 1);

    sink.reset();
    fs::remove(path, ignored);
}

}  // namespace

int main() {
    appends_one_header_then_one_complete_chunk_per_event();
    rejects_an_event_before_exceeding_the_file_bound();
    std::printf("parity_file_event_sink: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
