#include <opennova/retail_hook/windows/named_pipe_trace.h>
#include <parity/parity.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace opennova::retail_hook::windows {
namespace {

[[nodiscard]] std::wstring full_pipe_name(const std::wstring& name) {
    std::wstring kPrefix = L"\\\\.\\pipe";
    kPrefix.push_back(L'\\');
    if (name.rfind(kPrefix, 0) == 0) {
        return name;
    }
    return std::wstring(kPrefix) + name;
}

[[nodiscard]] PipeRecordResult failure(
    DWORD error,
    std::uint64_t bytes,
    std::string detail) {
    PipeRecordResult result{};
    result.success = false;
    result.win32_error = error;
    result.bytes_written = bytes;
    result.detail = std::move(detail);
    return result;
}

constexpr std::size_t kTraceHeaderSize = 8;
constexpr std::size_t kChunkHeaderSize = 12;
constexpr std::uint32_t kMaximumChunkPayload = 64U * 1024U * 1024U;

enum class ReadStatus {
    complete,
    clean_eof,
    truncated,
    failed,
};

struct ReadResult {
    ReadStatus status{ReadStatus::complete};
    DWORD error{ERROR_SUCCESS};
};

[[nodiscard]] ReadResult read_exact(
    HANDLE pipe,
    std::uint8_t* destination,
    std::size_t size,
    bool eof_at_start_is_clean) {
    std::size_t offset = 0;
    while (offset < size) {
        DWORD read = 0;
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(
                size - offset, std::numeric_limits<DWORD>::max()));
        if (ReadFile(
                pipe,
                destination + offset,
                requested,
                &read,
                nullptr) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                if (offset == 0 && eof_at_start_is_clean) {
                    return {ReadStatus::clean_eof, error};
                }
                return {ReadStatus::truncated, error};
            }
            return {ReadStatus::failed, error};
        }
        if (read == 0) {
            if (offset == 0 && eof_at_start_is_clean) {
                return {ReadStatus::clean_eof, ERROR_SUCCESS};
            }
            return {ReadStatus::truncated, ERROR_HANDLE_EOF};
        }
        offset += read;
    }
    return {};
}

[[nodiscard]] std::uint32_t read_u32_le(
    const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool same_identity(
    const parity::ProducerIdentity& lhs,
    const parity::ProducerIdentity& rhs) noexcept {
    return lhs.source == rhs.source &&
        lhs.role == rhs.role &&
        lhs.stream_id == rhs.stream_id;
}

[[nodiscard]] const parity::ProducerIdentity& event_identity(
    const parity::Event& event) {
    return std::visit(
        [](const auto& typed) -> const parity::ProducerIdentity& {
            return typed.identity;
        },
        event);
}

[[nodiscard]] bool complete_metadata(
    const parity::RunMetadata& metadata,
    const std::string& expected_scenario) {
    return !metadata.run_id.empty() &&
        !metadata.producer.empty() &&
        !metadata.build_id.empty() &&
        !metadata.scenario.empty() &&
        (expected_scenario.empty() ||
         metadata.scenario == expected_scenario) &&
        metadata.identity.source != parity::SourceKind::unknown &&
        metadata.identity.role != parity::RunRole::unknown &&
        !metadata.identity.stream_id.empty() &&
        !metadata.title.empty() &&
        !metadata.expansion.empty() &&
        !metadata.mission.empty();
}

struct BundleIdentity {
    std::string run_id{};
    std::string scenario{};
    std::string title{};
    std::string expansion{};
    std::string mission{};
};

[[nodiscard]] BundleIdentity bundle_identity(
    const parity::RunMetadata& metadata) {
    return BundleIdentity{
        metadata.run_id,
        metadata.scenario,
        metadata.title,
        metadata.expansion,
        metadata.mission,
    };
}

[[nodiscard]] bool same_bundle(
    const BundleIdentity& lhs,
    const BundleIdentity& rhs) noexcept {
    return lhs.run_id == rhs.run_id &&
        lhs.scenario == rhs.scenario &&
        lhs.title == rhs.title &&
        lhs.expansion == rhs.expansion &&
        lhs.mission == rhs.mission;
}

}  // namespace

PipeRecordResult record_trace_pipe(
    const std::wstring& pipe_name,
    const std::wstring& trace_path,
    const PipeRecordOptions& options) {
    if (pipe_name.empty() || trace_path.empty() ||
        options.expected_producers == 0 ||
        options.expected_producers > MAXIMUM_WAIT_OBJECTS ||
        options.producer_connect_timeout_ms == 0 ||
        options.maximum_trace_bytes < kTraceHeaderSize) {
        return failure(
            ERROR_INVALID_PARAMETER,
            0,
            "pipe, trace path, or producer count is invalid");
    }
    HANDLE output = CreateFileW(
        trace_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return failure(
            GetLastError(), 0, "could not create trace output");
    }

    parity::TraceWriter canonical_header;
    const auto& header_bytes = canonical_header.bytes();
    if (header_bytes.size() != kTraceHeaderSize) {
        CloseHandle(output);
        return failure(
            ERROR_INVALID_DATA, 0, "canonical trace header is invalid");
    }
    DWORD header_written = 0;
    if (WriteFile(
            output,
            header_bytes.data(),
            static_cast<DWORD>(header_bytes.size()),
            &header_written,
            nullptr) == FALSE ||
        header_written != header_bytes.size()) {
        const DWORD error = GetLastError();
        CloseHandle(output);
        return failure(
            error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error,
            0,
            "could not write canonical trace header");
    }

    const std::wstring full_name = full_pipe_name(pipe_name);
    std::vector<HANDLE> pipes;
    pipes.reserve(options.expected_producers);
    for (std::uint32_t index = 0;
         index < options.expected_producers;
         ++index) {
        DWORD access = PIPE_ACCESS_INBOUND;
        if (index == 0) {
            access |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        }
        HANDLE pipe = CreateNamedPipeW(
            full_name.c_str(),
            access,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            options.expected_producers,
            64 * 1024,
            64 * 1024,
            0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            for (HANDLE created : pipes) {
                CloseHandle(created);
            }
            CloseHandle(output);
            return failure(
                error, header_written, "could not create trace pipe instance");
        }
        pipes.push_back(pipe);
    }

    std::atomic_uint32_t producers_connected{};
    std::atomic_uint32_t producers_completed{};
    std::atomic_uint64_t events_merged{};
    std::atomic_uint64_t invalid_chunks{};
    std::atomic_uint64_t bytes_written{header_written};
    std::atomic_bool abort_requested{};
    std::mutex output_mutex;
    std::mutex state_mutex;
    DWORD first_error = ERROR_SUCCESS;
    std::string first_detail;
    std::vector<parity::ProducerIdentity> identities;
    std::optional<BundleIdentity> expected_bundle{};

    const auto set_failure =
        [&](DWORD error, std::string detail) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (first_detail.empty()) {
                first_error =
                    error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error;
                first_detail = std::move(detail);
            }
        };
    const auto set_invalid =
        [&](DWORD error, std::string detail) {
            invalid_chunks.fetch_add(1, std::memory_order_relaxed);
            set_failure(error, std::move(detail));
        };
    const auto write_complete_chunk =
        [&](const std::uint8_t* chunk, std::size_t size) {
            std::lock_guard<std::mutex> lock(output_mutex);
            const std::uint64_t current =
                bytes_written.load(std::memory_order_relaxed);
            if (size > options.maximum_trace_bytes - current) {
                set_failure(
                    ERROR_FILE_TOO_LARGE,
                    "merged trace exceeds the configured byte limit");
                return false;
            }
            DWORD written = 0;
            if (size > std::numeric_limits<DWORD>::max() ||
                WriteFile(
                    output,
                    chunk,
                    static_cast<DWORD>(size),
                    &written,
                    nullptr) == FALSE ||
                written != size) {
                const DWORD error = GetLastError();
                set_failure(
                    error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error,
                    "merged trace chunk write failed");
                return false;
            }
            bytes_written.fetch_add(written, std::memory_order_relaxed);
            events_merged.fetch_add(1, std::memory_order_relaxed);
            return true;
        };
    const auto write_synthesized_event =
        [&](const parity::Event& event) {
            parity::TraceWriter encoded;
            if (!encoded.append(event) ||
                encoded.bytes().size() <= kTraceHeaderSize) {
                set_failure(
                    ERROR_INVALID_DATA,
                    "could not encode synthesized checkpoint");
                return false;
            }
            return write_complete_chunk(
                encoded.bytes().data() + kTraceHeaderSize,
                encoded.bytes().size() - kTraceHeaderSize);
        };

    const auto collect = [&](HANDLE pipe, HANDLE connected_event) {
        if (abort_requested.load(std::memory_order_acquire)) {
            SetEvent(connected_event);
            CloseHandle(pipe);
            return;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (connected == FALSE && GetLastError() != ERROR_PIPE_CONNECTED) {
            set_failure(
                GetLastError(), "could not accept trace pipe producer");
            SetEvent(connected_event);
            CloseHandle(pipe);
            return;
        }
        producers_connected.fetch_add(1, std::memory_order_relaxed);
        SetEvent(connected_event);
        if (abort_requested.load(std::memory_order_acquire)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }

        std::array<std::uint8_t, kTraceHeaderSize> producer_header{};
        const ReadResult header_read = read_exact(
            pipe,
            producer_header.data(),
            producer_header.size(),
            false);
        if (header_read.status != ReadStatus::complete) {
            if (!abort_requested.load(std::memory_order_acquire)) {
                set_invalid(
                    header_read.error, "producer trace header is incomplete");
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }
        const parity::TraceReadResult decoded_header =
            parity::read_trace(
                producer_header.data(), producer_header.size());
        if (!decoded_header.complete() ||
            !decoded_header.trace.events.empty()) {
            set_invalid(
                ERROR_INVALID_DATA, "producer trace header is invalid");
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }

        bool metadata_seen = false;
        parity::ProducerIdentity producer_identity{};
        parity::StateLane comparable_lane{parity::StateLane::unknown};
        bool capture_start_written = false;
        std::optional<std::uint64_t> last_ready_frame{};
        bool producer_valid = true;
        for (;;) {
            std::array<std::uint8_t, kChunkHeaderSize> chunk_header{};
            const ReadResult chunk_header_read = read_exact(
                pipe,
                chunk_header.data(),
                chunk_header.size(),
                true);
            if (chunk_header_read.status == ReadStatus::clean_eof) {
                break;
            }
            if (chunk_header_read.status != ReadStatus::complete) {
                if (!abort_requested.load(std::memory_order_acquire)) {
                    set_invalid(
                        chunk_header_read.error,
                        "producer ended within a trace chunk header");
                }
                producer_valid = false;
                break;
            }

            const std::uint32_t payload_size =
                read_u32_le(chunk_header.data() + 4);
            if (payload_size > kMaximumChunkPayload) {
                set_invalid(
                    ERROR_INVALID_DATA,
                    "producer trace chunk exceeds the collector limit");
                producer_valid = false;
                break;
            }
            std::vector<std::uint8_t> chunk(
                kChunkHeaderSize + payload_size);
            std::memcpy(
                chunk.data(), chunk_header.data(), chunk_header.size());
            const ReadResult payload_read = read_exact(
                pipe,
                chunk.data() + kChunkHeaderSize,
                payload_size,
                false);
            if (payload_read.status != ReadStatus::complete) {
                if (!abort_requested.load(std::memory_order_acquire)) {
                    set_invalid(
                        payload_read.error,
                        "producer ended within a trace chunk payload");
                }
                producer_valid = false;
                break;
            }

            std::vector<std::uint8_t> framed;
            framed.reserve(header_bytes.size() + chunk.size());
            framed.insert(
                framed.end(), header_bytes.begin(), header_bytes.end());
            framed.insert(framed.end(), chunk.begin(), chunk.end());
            parity::TraceReadResult decoded = parity::read_trace(framed);
            if (!decoded.complete() ||
                decoded.trace.events.size() != 1) {
                set_invalid(
                    ERROR_INVALID_DATA,
                    "producer trace chunk is not one canonical typed event");
                producer_valid = false;
                break;
            }

            const parity::Event& event = decoded.trace.events.front();
            if (!metadata_seen) {
                const auto* metadata =
                    std::get_if<parity::RunMetadata>(&event);
                if (metadata == nullptr ||
                    !complete_metadata(
                        *metadata, options.expected_scenario)) {
                    set_invalid(
                        ERROR_INVALID_DATA,
                        "producer metadata is missing, incomplete, or mismatched");
                    producer_valid = false;
                    break;
                }
                producer_identity = metadata->identity;
                comparable_lane =
                    producer_identity.role == parity::RunRole::host
                    ? parity::StateLane::authoritative
                    : parity::StateLane::presented;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    const BundleIdentity current_bundle =
                        bundle_identity(*metadata);
                    const bool duplicate = std::any_of(
                        identities.begin(),
                        identities.end(),
                        [&](const parity::ProducerIdentity& identity) {
                            return same_identity(
                                identity, producer_identity);
                        });
                    const bool bundle_mismatch =
                        expected_bundle.has_value() &&
                        !same_bundle(
                            *expected_bundle, current_bundle);
                    if (duplicate || bundle_mismatch) {
                        if (first_detail.empty()) {
                            first_error = ERROR_INVALID_DATA;
                            first_detail = duplicate
                                ? "producer identity is duplicated"
                                : "producer bundle identity does not match";
                        }
                        invalid_chunks.fetch_add(
                            1, std::memory_order_relaxed);
                        producer_valid = false;
                    } else {
                        if (!expected_bundle.has_value()) {
                            expected_bundle = current_bundle;
                        }
                        identities.push_back(producer_identity);
                    }
                }
                if (!producer_valid) {
                    break;
                }
                metadata_seen = true;
            } else if (
                std::holds_alternative<parity::RunMetadata>(event) ||
                !same_identity(
                    event_identity(event), producer_identity)) {
                set_invalid(
                    ERROR_INVALID_DATA,
                    "producer event identity does not match its metadata");
                producer_valid = false;
                break;
            }

            const auto* checkpoint =
                std::get_if<parity::Checkpoint>(&event);
            const bool automatic_checkpoint =
                checkpoint != nullptr &&
                checkpoint->occurrence == 0 &&
                (checkpoint->name == "capture-start" ||
                 checkpoint->name == "capture-end");
            if (!automatic_checkpoint &&
                !write_complete_chunk(chunk.data(), chunk.size())) {
                producer_valid = false;
                break;
            }

            const auto* frame =
                std::get_if<parity::FrameSnapshot>(&event);
            const bool parity_ready =
                frame != nullptr &&
                frame->lane == comparable_lane &&
                frame->player.has_value() &&
                frame->player->present &&
                frame->weapon.has_value() &&
                frame->weapon->present;
            if (parity_ready) {
                last_ready_frame = frame->frame_index;
                if (!capture_start_written) {
                    parity::Checkpoint synthesized{};
                    synthesized.name = "capture-start";
                    synthesized.occurrence = 0;
                    synthesized.frame_index = frame->frame_index;
                    synthesized.identity = producer_identity;
                    synthesized.lane = comparable_lane;
                    if (!write_synthesized_event(synthesized)) {
                        producer_valid = false;
                        break;
                    }
                    capture_start_written = true;
                }
            }
        }

        if (producer_valid &&
            !abort_requested.load(std::memory_order_acquire) &&
            capture_start_written &&
            last_ready_frame.has_value()) {
            parity::Checkpoint synthesized{};
            synthesized.name = "capture-end";
            synthesized.occurrence = 0;
            synthesized.frame_index = *last_ready_frame;
            synthesized.identity = producer_identity;
            synthesized.lane = comparable_lane;
            if (!write_synthesized_event(synthesized)) {
                producer_valid = false;
            }
        }

        if (!metadata_seen) {
            if (producer_valid) {
                set_invalid(
                    ERROR_INVALID_DATA,
                    "producer disconnected before metadata");
            }
        } else if (
            producer_valid &&
            !abort_requested.load(std::memory_order_acquire)) {
            producers_completed.fetch_add(1, std::memory_order_relaxed);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    };

    std::vector<HANDLE> connection_events;
    connection_events.reserve(pipes.size());
    for (std::size_t index = 0; index < pipes.size(); ++index) {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            const DWORD error = GetLastError();
            for (HANDLE created : connection_events) {
                CloseHandle(created);
            }
            for (HANDLE pipe : pipes) {
                CloseHandle(pipe);
            }
            CloseHandle(output);
            return failure(
                error,
                bytes_written.load(std::memory_order_relaxed),
                "could not create producer connection event");
        }
        connection_events.push_back(event);
    }

    std::vector<std::thread> workers;
    workers.reserve(pipes.size());
    for (std::size_t index = 0; index < pipes.size(); ++index) {
        workers.emplace_back(
            collect, pipes[index], connection_events[index]);
    }
    const DWORD connection_wait = WaitForMultipleObjects(
        static_cast<DWORD>(connection_events.size()),
        connection_events.data(),
        TRUE,
        options.producer_connect_timeout_ms);
    if (connection_wait != WAIT_OBJECT_0) {
        abort_requested.store(true, std::memory_order_release);
        set_failure(
            connection_wait == WAIT_TIMEOUT
                ? ERROR_TIMEOUT
                : GetLastError(),
            connection_wait == WAIT_TIMEOUT
                ? "timed out waiting for all trace producers"
                : "failed while waiting for trace producers");
        for (std::size_t index = 0; index < workers.size(); ++index) {
            (void)CancelIoEx(pipes[index], nullptr);
            (void)CancelSynchronousIo(
                static_cast<HANDLE>(workers[index].native_handle()));
        }
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (HANDLE event : connection_events) {
        CloseHandle(event);
    }

    if (options.expected_producers == 2) {
        const parity::ProducerIdentity* host = nullptr;
        const parity::ProducerIdentity* client = nullptr;
        for (const parity::ProducerIdentity& identity : identities) {
            if (identity.role == parity::RunRole::host) {
                host = &identity;
            } else if (identity.role == parity::RunRole::client) {
                client = &identity;
            }
        }
        if (host == nullptr || client == nullptr) {
            set_failure(
                ERROR_INVALID_DATA,
                "producer set must contain one host and one client");
        } else {
            const bool baseline_mismatch =
                options.topology ==
                    PipeProducerTopology::retail_baseline &&
                (host->source != parity::SourceKind::retail ||
                 client->source != parity::SourceKind::retail);
            const bool candidate_mismatch =
                options.topology ==
                    PipeProducerTopology::
                        opennova_host_retail_client &&
                (host->source != parity::SourceKind::opennova ||
                 client->source != parity::SourceKind::retail);
            if (baseline_mismatch || candidate_mismatch) {
                set_failure(
                    ERROR_INVALID_DATA,
                    "producer topology does not match inspector role-set");
            }
        }
    }

    (void)FlushFileBuffers(output);
    CloseHandle(output);

    PipeRecordResult result{};
    result.success =
        first_detail.empty() &&
        producers_connected.load(std::memory_order_relaxed) ==
            options.expected_producers &&
        producers_completed.load(std::memory_order_relaxed) ==
            options.expected_producers;
    result.win32_error =
        result.success ? ERROR_SUCCESS : first_error;
    result.bytes_written =
        bytes_written.load(std::memory_order_relaxed);
    result.producers_connected =
        producers_connected.load(std::memory_order_relaxed);
    result.producers_completed =
        producers_completed.load(std::memory_order_relaxed);
    result.events_merged =
        events_merged.load(std::memory_order_relaxed);
    result.invalid_chunks =
        invalid_chunks.load(std::memory_order_relaxed);
    result.detail = std::move(first_detail);
    if (!result.success && result.detail.empty()) {
        result.win32_error = ERROR_INVALID_DATA;
        result.detail = "not all expected producers completed";
    }
    return result;
}

}  // namespace opennova::retail_hook::windows
