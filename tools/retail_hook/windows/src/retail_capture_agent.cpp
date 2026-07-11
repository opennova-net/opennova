#include <opennova/retail_hook/windows/retail_capture_agent.h>

#include <opennova/parity/windows/named_pipe_event_sink.h>
#include <opennova/retail_hook/parity_adapter.h>
#include <opennova/retail_hook/windows/winsock_parity.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

namespace opennova::retail_hook::windows {
namespace {

constexpr std::size_t kWireQueueCapacity = 1024;
constexpr std::size_t kEventQueueCapacity = 256;
constexpr auto kSnapshotInterval =
    std::chrono::nanoseconds(1'000'000'000 / 60);

[[nodiscard]] std::uint64_t unix_time_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

[[nodiscard]] std::uint64_t steady_time_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

[[nodiscard]] std::string utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            -1,
            result.data(),
            required,
            nullptr,
            nullptr) == 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

[[nodiscard]] parity::RunRole parity_role(HookRole role) noexcept {
    return role == HookRole::retail_host
        ? parity::RunRole::host
        : parity::RunRole::client;
}

[[nodiscard]] parity::StateLane parity_lane(HookRole role) noexcept {
    return role == HookRole::retail_host
        ? parity::StateLane::authoritative
        : parity::StateLane::presented;
}

void stamp_identity(
    parity::Event& event,
    const parity::ProducerIdentity& identity) {
    std::visit(
        [&identity](auto& typed) {
            typed.identity = identity;
        },
        event);
}

}  // namespace

struct RetailCaptureAgent::Impl {
    explicit Impl(
        const HookStartConfig& requested_config,
        ValidationSession* requested_session)
        : wire_sink(wire_queue),
          config(requested_config),
          identity{
              parity::SourceKind::retail,
              parity_role(config.role),
              utf8(config.stream_id),
          },
          session(requested_session) {}

    [[nodiscard]] parity::RunMetadata metadata() const {
        parity::RunMetadata result{};
        result.run_id = utf8(config.run_id);
        result.producer = "opennova-retail-hook";
        result.build_id = "jo-1.7.5.7-onhook-9a103544";
        result.scenario = utf8(config.scenario);
        result.started_unix_ns = unix_time_ns();
        result.identity = identity;
        result.title = "joint-operations";
        result.expansion = "revx02";
        result.mission = utf8(config.mission);
        return result;
    }

    void append_coverage_diagnostic(
        parity::windows::NamedPipeEventSink& sink) {
        parity::DiagnosticEvent diagnostic{};
        diagnostic.identity = identity;
        diagnostic.timestamp_ns = steady_time_ns();
        diagnostic.severity = parity::Severity::info;
        diagnostic.code = "retail_capture_coverage";
        diagnostic.message =
            "All five pool descriptors are validated; entity records are "
            "decoded only for complete pool 0. Other pool layouts remain opaque.";
        diagnostic.context.push_back(parity::Field{
            "descriptor_pool_count",
            static_cast<std::uint64_t>(5),
        });
        diagnostic.context.push_back(parity::Field{
            "complete_entity_pool",
            static_cast<std::uint64_t>(0),
        });
        (void)sink.append(diagnostic);
    }

    void append_loss_diagnostic(
        parity::windows::NamedPipeEventSink& sink) {
        const CaptureAgentStats wire = wire_queue.stats();
        const CaptureAgentStats typed = events.stats();
        const parity::windows::NamedPipeEventSinkStats pipe = sink.stats();
        parity::DiagnosticEvent diagnostic{};
        diagnostic.identity = identity;
        diagnostic.timestamp_ns = steady_time_ns();
        diagnostic.severity =
            wire.dropped_full != 0 ||
                wire.dropped_contended != 0 ||
                typed.dropped_full != 0 ||
                typed.dropped_contended != 0 ||
                pipe.dropped_full != 0 ||
                pipe.dropped_contended != 0 ||
                pipe.write_failures != 0
            ? parity::Severity::warning
            : parity::Severity::info;
        diagnostic.code = "retail_capture_counts";
        diagnostic.message =
            "Bounded retail capture shutdown counters.";
        diagnostic.context = {
            {"wire_accepted", wire.accepted},
            {"wire_dropped_full", wire.dropped_full},
            {"wire_dropped_contended", wire.dropped_contended},
            {"event_accepted", typed.accepted},
            {"event_dropped_full", typed.dropped_full},
            {"event_dropped_contended", typed.dropped_contended},
            {"pipe_accepted", pipe.accepted},
            {"pipe_dropped_full", pipe.dropped_full},
            {"pipe_dropped_contended", pipe.dropped_contended},
            {"pipe_write_failures", pipe.write_failures},
        };
        (void)sink.append(diagnostic);
    }

    void drain(
        parity::windows::NamedPipeEventSink& sink) {
        parity::Event event{};
        while (events.try_pop(event)) {
            stamp_identity(event, identity);
            (void)sink.append(event);
        }

        CapturedWireDatagram captured{};
        while (wire_queue.try_pop(captured)) {
            (void)sink.append(to_parity_datagram(captured, identity));
        }
    }

    void run() {
        parity::windows::NamedPipeEventSink sink(config.pipe_name, 3000);
        if (!sink.connect() || !sink.append(metadata())) {
            startup_state.store(-1, std::memory_order_release);
            sink.close();
            return;
        }
        startup_state.store(1, std::memory_order_release);
        append_coverage_diagnostic(sink);

        std::uint64_t frame_index = 0;
        CaptureCheckpointTracker checkpoints;
        const parity::StateLane lane = parity_lane(config.role);
        auto next_snapshot = std::chrono::steady_clock::now();
        while (!stop_requested.load(std::memory_order_acquire)) {
            drain(sink);

            const auto now = std::chrono::steady_clock::now();
            if (session != nullptr && now >= next_snapshot) {
                const ValidationSnapshot snapshot = session->sample();
                if (snapshot) {
                    parity::FrameSnapshot frame = to_parity_snapshot(
                        snapshot,
                        identity,
                        lane,
                        frame_index,
                        0,
                        steady_time_ns());
                    frame.extensions.push_back(parity::Field{
                        "capture.nominal_hz",
                        static_cast<std::uint64_t>(60),
                    });
                    frame.extensions.push_back(parity::Field{
                        "capture.has_retail_logic_tick",
                        false,
                    });
                    if (sink.append(frame)) {
                        const auto checkpoint =
                            checkpoints.accepted_frame(frame);
                        if (checkpoint.has_value()) {
                            (void)sink.append(*checkpoint);
                        }
                    }
                    ++frame_index;
                } else {
                    parity::DiagnosticEvent diagnostic{};
                    diagnostic.identity = identity;
                    diagnostic.timestamp_ns = steady_time_ns();
                    diagnostic.severity = parity::Severity::warning;
                    diagnostic.code = validation_error_name(
                        snapshot.check.error);
                    diagnostic.message = snapshot.check.detail;
                    (void)sink.append(diagnostic);
                }
                next_snapshot += kSnapshotInterval;
                if (next_snapshot <= now) {
                    next_snapshot = now + kSnapshotInterval;
                }
            }
            Sleep(1);
        }

        drain(sink);
        const auto capture_end = checkpoints.finish();
        if (capture_end.has_value()) {
            (void)sink.append(*capture_end);
        }
        append_loss_diagnostic(sink);
        sink.close();
    }

    CaptureAgent<CapturedWireDatagram, kWireQueueCapacity> wire_queue{};
    QueuedWireCaptureSink<kWireQueueCapacity> wire_sink;
    CaptureAgent<parity::Event, kEventQueueCapacity> events{};
    HookStartConfig config{};
    parity::ProducerIdentity identity{};
    ValidationSession* session{};
    std::atomic_bool stop_requested{};
    std::atomic_int startup_state{};
    std::thread worker{};
};

RetailCaptureAgent::RetailCaptureAgent(
    const HookStartConfig& config,
    ValidationSession* session)
    : impl_(std::make_unique<Impl>(config, session)) {}

RetailCaptureAgent::~RetailCaptureAgent() {
    stop();
}

bool RetailCaptureAgent::start() {
    if (impl_->worker.joinable()) {
        return false;
    }
    impl_->stop_requested.store(false, std::memory_order_release);
    impl_->startup_state.store(0, std::memory_order_release);
    try {
        impl_->worker = std::thread([this]() {
            impl_->run();
        });
    } catch (...) {
        return false;
    }
    for (std::uint32_t waited = 0; waited < 4000; ++waited) {
        const int state =
            impl_->startup_state.load(std::memory_order_acquire);
        if (state > 0) {
            return true;
        }
        if (state < 0) {
            stop();
            return false;
        }
        Sleep(1);
    }
    stop();
    return false;
}

void RetailCaptureAgent::stop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_release);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

IWireCaptureSink& RetailCaptureAgent::wire_sink() noexcept {
    return impl_->wire_sink;
}

bool RetailCaptureAgent::try_capture(parity::Event event) noexcept {
    return impl_->events.try_capture(std::move(event));
}

bool RetailCaptureAgent::capture_mutation(
    const MutationResult& result) noexcept {
    try {
        return impl_->events.try_capture(to_parity_mutation_audit(
            result, impl_->identity, 0, steady_time_ns()));
    } catch (...) {
        return false;
    }
}

CaptureAgentStats RetailCaptureAgent::wire_stats() const noexcept {
    return impl_->wire_queue.stats();
}

}  // namespace opennova::retail_hook::windows
