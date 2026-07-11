#include <opennova/parity/windows/named_pipe_event_sink.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace opennova::parity::windows {
namespace {

constexpr std::size_t kQueueCapacity = 1024;
constexpr std::size_t kTraceHeaderSize = 8;

const std::string kNoError{};
const std::string kAlreadyConnected =
    "parity pipe sink is one-shot and was already connected";
const std::string kHeaderWriteFailed =
    "parity pipe stream header write failed";
const std::string kWriterThreadFailed =
    "could not start parity pipe writer thread";
const std::string kQueueCopyFailed =
    "could not copy parity event into bounded pipe queue";
const std::string kEventEncodeFailed =
    "canonical parity event encoding failed";
const std::string kEventWriteFailed =
    "canonical parity event chunk write failed";

[[nodiscard]] std::wstring full_pipe_name(const std::wstring& name) {
    std::wstring prefix{L'\\', L'\\', L'.', L'\\'};
    prefix += L"pipe";
    prefix.push_back(L'\\');
    if (name.rfind(prefix, 0) == 0) {
        return name;
    }
    return prefix + name;
}

[[nodiscard]] std::string win32_detail(
    const char* operation,
    DWORD error) {
    return std::string(operation) + " failed with Win32 error " +
        std::to_string(error);
}

}  // namespace

struct NamedPipeEventSink::Impl {
    explicit Impl(std::wstring requested_name, std::uint32_t timeout)
        : pipe_name(std::move(requested_name)),
          connect_timeout_ms(timeout) {}

    ~Impl() {
        close();
    }

    [[nodiscard]] bool write_one(
        const std::uint8_t* bytes,
        std::size_t size) {
        if (pipe == INVALID_HANDLE_VALUE ||
            size > std::numeric_limits<DWORD>::max()) {
            return false;
        }
        DWORD written = 0;
        if (WriteFile(
                pipe,
                bytes,
                static_cast<DWORD>(size),
                &written,
                nullptr) == FALSE ||
            written != size) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool open_pipe() {
        if (pipe_name.empty()) {
            connect_error = "parity pipe name is empty";
            error.store(&connect_error, std::memory_order_release);
            return false;
        }
        const std::wstring path = full_pipe_name(pipe_name);
        const ULONGLONG deadline = GetTickCount64() + connect_timeout_ms;
        for (;;) {
            pipe = CreateFileW(
                path.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                return true;
            }

            const DWORD win32_error = GetLastError();
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                connect_error =
                    win32_detail("parity pipe connect", ERROR_TIMEOUT);
                error.store(&connect_error, std::memory_order_release);
                return false;
            }
            const DWORD remaining = static_cast<DWORD>(
                std::min<ULONGLONG>(deadline - now, 50));
            if (win32_error == ERROR_PIPE_BUSY) {
                (void)WaitNamedPipeW(path.c_str(), remaining);
            } else if (win32_error == ERROR_FILE_NOT_FOUND) {
                Sleep(std::max<DWORD>(1, remaining));
            } else {
                connect_error =
                    win32_detail("parity pipe connect", win32_error);
                this->error.store(
                    &connect_error, std::memory_order_release);
                return false;
            }
        }
    }

    [[nodiscard]] bool connect() {
        if (connect_attempted.exchange(true, std::memory_order_acq_rel)) {
            error.store(&kAlreadyConnected, std::memory_order_release);
            return false;
        }
        TraceWriter header_writer;
        if (!open_pipe()) {
            return false;
        }
        const auto& header = header_writer.bytes();
        if (!write_one(header.data(), header.size())) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            error.store(&kHeaderWriteFailed, std::memory_order_release);
            return false;
        }
        stopping.store(false, std::memory_order_release);
        failed.store(false, std::memory_order_release);
        active.store(true, std::memory_order_release);
        try {
            worker = std::thread([this]() {
                run();
            });
        } catch (...) {
            active.store(false, std::memory_order_release);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            error.store(&kWriterThreadFailed, std::memory_order_release);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool enqueue(const Event& event) {
        if (!active.load(std::memory_order_acquire) ||
            failed.load(std::memory_order_acquire)) {
            return false;
        }
        std::unique_lock<std::mutex> lock(queue_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            dropped_contended.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (queue_size == queue.size()) {
            dropped_full.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        try {
            queue[queue_tail].emplace(event);
        } catch (...) {
            lock.unlock();
            error.store(&kQueueCopyFailed, std::memory_order_release);
            return false;
        }
        queue_tail = (queue_tail + 1) % queue.size();
        ++queue_size;
        accepted.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        queue_ready.notify_one();
        return true;
    }

    void run() {
        for (;;) {
            std::optional<Event> event;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_ready.wait(lock, [this]() {
                    return stopping.load(std::memory_order_acquire) ||
                        queue_size != 0;
                });
                if (queue_size == 0 &&
                    stopping.load(std::memory_order_acquire)) {
                    break;
                }
                event = std::move(queue[queue_head]);
                queue[queue_head].reset();
                queue_head = (queue_head + 1) % queue.size();
                --queue_size;
            }

            TraceWriter encoded;
            if (!event.has_value() || !encoded.append(*event)) {
                error.store(&kEventEncodeFailed, std::memory_order_release);
                failed.store(true, std::memory_order_release);
                break;
            }
            const auto& bytes = encoded.bytes();
            if (bytes.size() <= kTraceHeaderSize ||
                !write_one(
                    bytes.data() + kTraceHeaderSize,
                    bytes.size() - kTraceHeaderSize)) {
                error.store(&kEventWriteFailed, std::memory_order_release);
                write_failures.fetch_add(1, std::memory_order_relaxed);
                failed.store(true, std::memory_order_release);
                break;
            }
        }
        active.store(false, std::memory_order_release);
    }

    void close() noexcept {
        stopping.store(true, std::memory_order_release);
        queue_ready.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        if (pipe != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        active.store(false, std::memory_order_release);
    }

    std::wstring pipe_name{};
    std::uint32_t connect_timeout_ms{};
    HANDLE pipe{INVALID_HANDLE_VALUE};
    std::atomic_bool connect_attempted{};

    std::array<std::optional<Event>, kQueueCapacity> queue{};
    std::size_t queue_head{};
    std::size_t queue_tail{};
    std::size_t queue_size{};
    std::mutex queue_mutex{};
    std::condition_variable queue_ready{};
    std::thread worker{};
    std::atomic_bool stopping{};
    std::atomic_bool active{};
    std::atomic_bool failed{};

    std::atomic_uint64_t accepted{};
    std::atomic_uint64_t dropped_full{};
    std::atomic_uint64_t dropped_contended{};
    std::atomic_uint64_t write_failures{};

    std::string connect_error{};
    std::atomic<const std::string*> error{&kNoError};
};

NamedPipeEventSink::NamedPipeEventSink(
    std::wstring pipe_name,
    std::uint32_t connect_timeout_ms)
    : impl_(std::make_unique<Impl>(
          std::move(pipe_name), connect_timeout_ms)) {}

NamedPipeEventSink::~NamedPipeEventSink() = default;

bool NamedPipeEventSink::connect() {
    return impl_->connect();
}

bool NamedPipeEventSink::append(const Event& event) {
    return impl_->enqueue(event);
}

const std::string& NamedPipeEventSink::last_error() const noexcept {
    return *impl_->error.load(std::memory_order_acquire);
}

bool NamedPipeEventSink::connected() const noexcept {
    return impl_->active.load(std::memory_order_acquire) &&
        !impl_->failed.load(std::memory_order_acquire);
}

NamedPipeEventSinkStats NamedPipeEventSink::stats() const noexcept {
    return NamedPipeEventSinkStats{
        impl_->accepted.load(std::memory_order_relaxed),
        impl_->dropped_full.load(std::memory_order_relaxed),
        impl_->dropped_contended.load(std::memory_order_relaxed),
        impl_->write_failures.load(std::memory_order_relaxed),
    };
}

void NamedPipeEventSink::close() noexcept {
    impl_->close();
}

}  // namespace opennova::parity::windows
