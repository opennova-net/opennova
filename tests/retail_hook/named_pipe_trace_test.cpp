#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <opennova/parity/windows/named_pipe_event_sink.h>
#include <opennova/retail_hook/windows/named_pipe_trace.h>
#include <parity/parity.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace windows = opennova::retail_hook::windows;
namespace parity = opennova::parity;
namespace parity_windows = opennova::parity::windows;

static int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++failures; \
        } \
    } while (0)

int main() {
    const std::wstring pipe_name =
        L"opennova-retail-pipe-test-" +
        std::to_wstring(GetCurrentProcessId());
    wchar_t temp_path[MAX_PATH]{};
    CHECK(GetTempPathW(MAX_PATH, temp_path) != 0);
    const std::wstring trace_path =
        std::wstring(temp_path) + L"opennova-retail-pipe-test-" +
        std::to_wstring(GetCurrentProcessId()) + L".onpt";

    windows::PipeRecordResult recorded{};
    std::thread server([&]() {
        windows::PipeRecordOptions options{};
        options.expected_producers = 2;
        options.expected_scenario = "pipe-test";
        options.topology =
            windows::PipeProducerTopology::retail_baseline;
        options.producer_connect_timeout_ms = 3000;
        recorded =
            windows::record_trace_pipe(pipe_name, trace_path, options);
    });

    std::atomic_int producers_ok{};
    const auto produce = [&](parity::RunRole role, std::string stream_id) {
        parity_windows::NamedPipeEventSink sink(pipe_name, 3000);
        if (!sink.connect()) {
            return;
        }
        const auto append = [&](const auto& typed) {
            const parity::Event event{typed};
            for (std::uint32_t attempt = 0; attempt < 1000; ++attempt) {
                if (sink.append(event)) {
                    return true;
                }
                Sleep(1);
            }
            return false;
        };
        parity::RunMetadata metadata{};
        metadata.run_id = "run-1";
        metadata.producer = "pipe-test";
        metadata.build_id = "build";
        metadata.scenario = "pipe-test";
        metadata.started_unix_ns = 123;
        metadata.identity = {
            parity::SourceKind::retail, role, std::move(stream_id)};
        metadata.title = "joint-operations";
        metadata.expansion = "revx02";
        metadata.mission = "training-range";
        if (!append(metadata)) {
            sink.close();
            return;
        }

        parity::FrameSnapshot frame{};
        frame.identity = metadata.identity;
        frame.lane = role == parity::RunRole::host
            ? parity::StateLane::authoritative
            : parity::StateLane::presented;
        frame.frame_index =
            role == parity::RunRole::host ? 10 : 20;
        frame.timestamp_ns = frame.frame_index * 100;
        if (!append(frame)) {
            sink.close();
            return;
        }
        ++frame.frame_index;
        frame.timestamp_ns = frame.frame_index * 100;
        frame.player = parity::PlayerState{};
        frame.player->present = true;
        frame.weapon = parity::WeaponState{};
        frame.weapon->present = true;
        if (!append(frame)) {
            sink.close();
            return;
        }
        ++frame.frame_index;
        frame.timestamp_ns = frame.frame_index * 100;
        frame.player.reset();
        frame.weapon.reset();
        if (!append(frame)) {
            sink.close();
            return;
        }
        ++producers_ok;
        sink.close();
    };

    std::thread host(
        produce, parity::RunRole::host, "retail-host");
    std::thread client(
        produce, parity::RunRole::client, "retail-client");
    host.join();
    client.join();
    server.join();

    CHECK(producers_ok.load() == 2);
    CHECK(recorded.success);
    CHECK(recorded.producers_connected == 2);
    CHECK(recorded.producers_completed == 2);
    CHECK(recorded.events_merged == 12);
    CHECK(recorded.invalid_chunks == 0);

    std::ifstream file(trace_path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>(),
    };
    const parity::TraceReadResult decoded = parity::read_trace(bytes);
    CHECK(decoded.complete());
    CHECK(decoded.trace.events.size() == 12);

    bool saw_host = false;
    bool saw_client = false;
    std::uint32_t automatic_checkpoints = 0;
    for (const parity::Event& event : decoded.trace.events) {
        if (const auto* metadata =
                std::get_if<parity::RunMetadata>(&event)) {
            saw_host = saw_host ||
                metadata->identity.role == parity::RunRole::host;
            saw_client = saw_client ||
                metadata->identity.role == parity::RunRole::client;
        } else if (const auto* checkpoint =
                       std::get_if<parity::Checkpoint>(&event)) {
            ++automatic_checkpoints;
            CHECK(checkpoint->occurrence == 0);
            CHECK(
                checkpoint->name == "capture-start" ||
                checkpoint->name == "capture-end");
            CHECK(checkpoint->frame_index ==
                  (checkpoint->identity.role == parity::RunRole::host
                       ? 11
                       : 21));
        }
    }
    CHECK(saw_host);
    CHECK(saw_client);
    CHECK(automatic_checkpoints == 4);

    DeleteFileW(trace_path.c_str());

    const std::wstring timeout_pipe =
        pipe_name + L"-timeout";
    const std::wstring timeout_trace =
        trace_path + L".timeout";
    windows::PipeRecordOptions timeout_options{};
    timeout_options.expected_producers = 2;
    timeout_options.expected_scenario = "pipe-test";
    timeout_options.producer_connect_timeout_ms = 50;
    const auto timeout_started = std::chrono::steady_clock::now();
    const windows::PipeRecordResult timed_out =
        windows::record_trace_pipe(
            timeout_pipe, timeout_trace, timeout_options);
    const auto timeout_elapsed =
        std::chrono::steady_clock::now() - timeout_started;
    CHECK(!timed_out.success);
    CHECK(timed_out.win32_error == ERROR_TIMEOUT);
    CHECK(timeout_elapsed < std::chrono::seconds(2));
    DeleteFileW(timeout_trace.c_str());

    const std::wstring partial_pipe =
        pipe_name + L"-partial";
    const std::wstring partial_trace =
        trace_path + L".partial";
    windows::PipeRecordResult partial_result{};
    std::thread partial_server([&]() {
        windows::PipeRecordOptions partial_options{};
        partial_options.expected_producers = 2;
        partial_options.expected_scenario = "pipe-test";
        partial_options.producer_connect_timeout_ms = 100;
        partial_result = windows::record_trace_pipe(
            partial_pipe, partial_trace, partial_options);
    });
    parity_windows::NamedPipeEventSink lone_sink(partial_pipe, 3000);
    CHECK(lone_sink.connect());
    parity::RunMetadata lone_metadata{};
    lone_metadata.run_id = "partial-run";
    lone_metadata.producer = "pipe-test";
    lone_metadata.build_id = "build";
    lone_metadata.scenario = "pipe-test";
    lone_metadata.identity = {
        parity::SourceKind::retail,
        parity::RunRole::host,
        "partial-host",
    };
    lone_metadata.title = "joint-operations";
    lone_metadata.expansion = "revx02";
    lone_metadata.mission = "training-range";
    bool lone_metadata_queued = false;
    const parity::Event lone_event{lone_metadata};
    for (std::uint32_t attempt = 0;
         attempt < 1000 && !lone_metadata_queued;
         ++attempt) {
        lone_metadata_queued = lone_sink.append(lone_event);
        if (!lone_metadata_queued) {
            Sleep(1);
        }
    }
    CHECK(lone_metadata_queued);
    partial_server.join();
    lone_sink.close();
    CHECK(!partial_result.success);
    CHECK(partial_result.win32_error == ERROR_TIMEOUT);
    CHECK(partial_result.producers_connected == 1);
    DeleteFileW(partial_trace.c_str());

    const std::wstring mismatch_pipe =
        pipe_name + L"-bundle-mismatch";
    const std::wstring mismatch_trace =
        trace_path + L".bundle-mismatch";
    windows::PipeRecordResult mismatch_result{};
    std::thread mismatch_server([&]() {
        windows::PipeRecordOptions mismatch_options{};
        mismatch_options.expected_producers = 2;
        mismatch_options.expected_scenario = "pipe-test";
        mismatch_options.topology =
            windows::PipeProducerTopology::
                opennova_host_retail_client;
        mismatch_options.producer_connect_timeout_ms = 3000;
        mismatch_result = windows::record_trace_pipe(
            mismatch_pipe, mismatch_trace, mismatch_options);
    });
    const auto mismatched_producer =
        [&](parity::RunRole role, const char* mission) {
            parity_windows::NamedPipeEventSink sink(
                mismatch_pipe, 3000);
            CHECK(sink.connect());
            parity::RunMetadata metadata{};
            metadata.run_id = "same-run";
            metadata.producer = role == parity::RunRole::host
                ? "opennova-runtime"
                : "retail-hook";
            metadata.build_id = role == parity::RunRole::host
                ? "host-build"
                : "client-build";
            metadata.scenario = "pipe-test";
            metadata.identity = {
                role == parity::RunRole::host
                    ? parity::SourceKind::opennova
                    : parity::SourceKind::retail,
                role,
                role == parity::RunRole::host
                    ? "candidate-host"
                    : "retail-client",
            };
            metadata.title = "joint-operations";
            metadata.expansion = "revx02";
            metadata.mission = mission;
            const parity::Event event{metadata};
            bool queued = false;
            for (std::uint32_t attempt = 0;
                 attempt < 1000 && !queued;
                 ++attempt) {
                queued = sink.append(event);
                if (!queued) {
                    Sleep(1);
                }
            }
            CHECK(queued);
            sink.close();
        };
    std::thread mismatched_host(
        mismatched_producer,
        parity::RunRole::host,
        "training-range");
    std::thread mismatched_client(
        mismatched_producer,
        parity::RunRole::client,
        "wrong-mission");
    mismatched_host.join();
    mismatched_client.join();
    mismatch_server.join();
    CHECK(!mismatch_result.success);
    CHECK(mismatch_result.invalid_chunks != 0);
    CHECK(
        mismatch_result.detail.find("bundle identity") !=
        std::string::npos);
    DeleteFileW(mismatch_trace.c_str());

    const std::wstring topology_pipe =
        pipe_name + L"-topology";
    const std::wstring topology_trace =
        trace_path + L".topology";
    windows::PipeRecordResult topology_result{};
    std::thread topology_server([&]() {
        windows::PipeRecordOptions topology_options{};
        topology_options.expected_producers = 2;
        topology_options.expected_scenario = "pipe-test";
        topology_options.topology =
            windows::PipeProducerTopology::
                opennova_host_retail_client;
        topology_options.producer_connect_timeout_ms = 3000;
        topology_result = windows::record_trace_pipe(
            topology_pipe, topology_trace, topology_options);
    });
    const auto wrong_topology_producer =
        [&](parity::RunRole role) {
            parity_windows::NamedPipeEventSink sink(
                topology_pipe, 3000);
            CHECK(sink.connect());
            parity::RunMetadata metadata{};
            metadata.run_id = "topology-run";
            metadata.producer = "retail";
            metadata.build_id = "retail-build";
            metadata.scenario = "pipe-test";
            metadata.identity = {
                parity::SourceKind::retail,
                role,
                role == parity::RunRole::host
                    ? "wrong-retail-host"
                    : "retail-client",
            };
            metadata.title = "joint-operations";
            metadata.expansion = "revx02";
            metadata.mission = "training-range";
            const parity::Event event{metadata};
            bool queued = false;
            for (std::uint32_t attempt = 0;
                 attempt < 1000 && !queued;
                 ++attempt) {
                queued = sink.append(event);
                if (!queued) {
                    Sleep(1);
                }
            }
            CHECK(queued);
            sink.close();
        };
    std::thread wrong_host(
        wrong_topology_producer, parity::RunRole::host);
    std::thread right_client(
        wrong_topology_producer, parity::RunRole::client);
    wrong_host.join();
    right_client.join();
    topology_server.join();
    CHECK(!topology_result.success);
    CHECK(
        topology_result.detail.find("topology") !=
        std::string::npos);
    DeleteFileW(topology_trace.c_str());

    const std::wstring limit_pipe =
        pipe_name + L"-limit";
    const std::wstring limit_trace =
        trace_path + L".limit";
    windows::PipeRecordResult limit_result{};
    std::thread limit_server([&]() {
        windows::PipeRecordOptions limit_options{};
        limit_options.expected_producers = 1;
        limit_options.expected_scenario = "pipe-test";
        limit_options.producer_connect_timeout_ms = 3000;
        limit_options.maximum_trace_bytes = 8;
        limit_result = windows::record_trace_pipe(
            limit_pipe, limit_trace, limit_options);
    });
    parity_windows::NamedPipeEventSink limited_sink(limit_pipe, 3000);
    CHECK(limited_sink.connect());
    parity::RunMetadata limited_metadata{};
    limited_metadata.run_id = "limit-run";
    limited_metadata.producer = "retail-hook";
    limited_metadata.build_id = "retail-build";
    limited_metadata.scenario = "pipe-test";
    limited_metadata.identity = {
        parity::SourceKind::retail,
        parity::RunRole::host,
        "limited-host",
    };
    limited_metadata.title = "joint-operations";
    limited_metadata.expansion = "revx02";
    limited_metadata.mission = "training-range";
    bool limited_queued = false;
    const parity::Event limited_event{limited_metadata};
    for (std::uint32_t attempt = 0;
         attempt < 1000 && !limited_queued;
         ++attempt) {
        limited_queued = limited_sink.append(limited_event);
        if (!limited_queued) {
            Sleep(1);
        }
    }
    CHECK(limited_queued);
    limited_sink.close();
    limit_server.join();
    CHECK(!limit_result.success);
    CHECK(limit_result.win32_error == ERROR_FILE_TOO_LARGE);
    CHECK(limit_result.bytes_written <= 8);
    DeleteFileW(limit_trace.c_str());

    std::printf("retail_hook_named_pipe_trace: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
