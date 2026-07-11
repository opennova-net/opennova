#pragma once

#include <parity/parity.h>

#include <cstdint>
#include <memory>
#include <string>

namespace opennova::parity::windows {

struct NamedPipeEventSinkStats {
    std::uint64_t accepted{};
    std::uint64_t dropped_full{};
    std::uint64_t dropped_contended{};
    std::uint64_t write_failures{};
};

// Pointer-size-neutral Windows parity producer. connect() may wait during
// startup; append() only attempts a bounded in-process enqueue and never waits
// for named-pipe I/O.
class NamedPipeEventSink final : public IEventSink {
public:
    explicit NamedPipeEventSink(
        std::wstring pipe_name,
        std::uint32_t connect_timeout_ms = 3000);
    ~NamedPipeEventSink() override;

    NamedPipeEventSink(const NamedPipeEventSink&) = delete;
    NamedPipeEventSink& operator=(const NamedPipeEventSink&) = delete;

    [[nodiscard]] bool connect();
    [[nodiscard]] bool append(const Event& event) override;
    [[nodiscard]] const std::string& last_error() const noexcept override;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] NamedPipeEventSinkStats stats() const noexcept;
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace opennova::parity::windows
