#pragma once

#include <parity/parity.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace opennova::parity {

inline constexpr std::uint64_t kDefaultMaxTraceFileBytes =
    256ULL * 1024ULL * 1024ULL;

struct FileEventSinkOptions {
    std::uint64_t max_file_bytes{kDefaultMaxTraceFileBytes};
    bool flush_each_event{true};
};

// A bounded, append-only trace sink for long-running capture producers. A new
// file gets one ONPT header. Each append encodes only that Event with a fresh
// TraceWriter, strips the fresh writer's header, and writes the complete chunk;
// no previously written event remains resident in memory.
//
// This class is not thread-safe. Producers that append from multiple threads
// must serialize access at their ownership boundary.
class FileEventSink final : public IEventSink {
public:
    ~FileEventSink() override;

    FileEventSink(const FileEventSink&) = delete;
    FileEventSink& operator=(const FileEventSink&) = delete;

    [[nodiscard]] static std::unique_ptr<FileEventSink> open(
        const std::filesystem::path& path,
        FileEventSinkOptions options = {},
        std::string* error = nullptr);

    [[nodiscard]] bool append(const Event& event) override;
    [[nodiscard]] const std::string& last_error() const noexcept override;
    [[nodiscard]] bool flush();
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;

private:
    struct Impl;

    explicit FileEventSink(FileEventSinkOptions options);
    [[nodiscard]] bool initialize(const std::filesystem::path& path);

    FileEventSinkOptions options_{};
    std::unique_ptr<Impl> impl_{};
    std::string last_error_{};
    std::uint64_t bytes_written_{};
};

}  // namespace opennova::parity
