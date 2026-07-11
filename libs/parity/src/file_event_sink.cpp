#include <parity/file_event_sink.h>

#include <fstream>
#include <limits>
#include <system_error>

namespace opennova::parity {
namespace {

constexpr std::size_t kTraceHeaderSize = 8;

void set_open_error(std::string* destination, const std::string& detail) {
    if (destination != nullptr) {
        *destination = detail;
    }
}

}  // namespace

struct FileEventSink::Impl {
    std::ofstream stream{};
};

FileEventSink::FileEventSink(FileEventSinkOptions options)
    : options_(options), impl_(std::make_unique<Impl>()) {}

FileEventSink::~FileEventSink() = default;

std::unique_ptr<FileEventSink> FileEventSink::open(
    const std::filesystem::path& path,
    FileEventSinkOptions options,
    std::string* error) {
    set_open_error(error, {});
    auto sink = std::unique_ptr<FileEventSink>(new FileEventSink(options));
    if (!sink->initialize(path)) {
        set_open_error(error, sink->last_error());
        return nullptr;
    }
    return sink;
}

bool FileEventSink::initialize(const std::filesystem::path& path) {
    last_error_.clear();
    if (options_.max_file_bytes < kTraceHeaderSize) {
        last_error_ = "maximum trace file size is smaller than the ONPT header";
        return false;
    }

    std::error_code filesystem_error;
    if (std::filesystem::exists(path, filesystem_error)) {
        last_error_ = filesystem_error
                          ? "could not inspect the parity trace path"
                          : "refusing to overwrite an existing parity trace";
        return false;
    }
    if (filesystem_error) {
        last_error_ = "could not inspect the parity trace path";
        return false;
    }

    impl_->stream.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!impl_->stream) {
        last_error_ = "could not create the parity trace file";
        return false;
    }

    const TraceWriter header_writer;
    const auto& header = header_writer.bytes();
    if (header.size() != kTraceHeaderSize) {
        last_error_ = "internal ONPT header size mismatch";
        return false;
    }
    impl_->stream.write(reinterpret_cast<const char*>(header.data()),
                        static_cast<std::streamsize>(header.size()));
    if (!impl_->stream) {
        last_error_ = "could not write the ONPT header";
        return false;
    }
    bytes_written_ = header.size();
    if (options_.flush_each_event && !flush()) {
        return false;
    }
    return true;
}

bool FileEventSink::append(const Event& event) {
    last_error_.clear();
    if (!impl_ || !impl_->stream) {
        last_error_ = "parity trace file is not writable";
        return false;
    }

    TraceWriter event_writer;
    if (!event_writer.append(event)) {
        last_error_ = event_writer.last_error();
        return false;
    }
    const auto& encoded = event_writer.bytes();
    if (encoded.size() < kTraceHeaderSize) {
        last_error_ = "event encoder returned an incomplete ONPT record";
        return false;
    }
    const std::uint64_t chunk_size = encoded.size() - kTraceHeaderSize;
    if (chunk_size > options_.max_file_bytes - bytes_written_) {
        last_error_ = "event would exceed the maximum parity trace file size";
        return false;
    }
    if (chunk_size > static_cast<std::uint64_t>(
                         std::numeric_limits<std::streamsize>::max())) {
        last_error_ = "encoded parity event exceeds the stream write limit";
        return false;
    }

    impl_->stream.write(
        reinterpret_cast<const char*>(encoded.data() + kTraceHeaderSize),
        static_cast<std::streamsize>(chunk_size));
    if (!impl_->stream) {
        last_error_ = "could not append the parity event chunk";
        return false;
    }
    bytes_written_ += chunk_size;
    return !options_.flush_each_event || flush();
}

const std::string& FileEventSink::last_error() const noexcept {
    return last_error_;
}

bool FileEventSink::flush() {
    if (!impl_ || !impl_->stream) {
        last_error_ = "parity trace file is not writable";
        return false;
    }
    impl_->stream.flush();
    if (!impl_->stream) {
        last_error_ = "could not flush the parity trace file";
        return false;
    }
    return true;
}

std::uint64_t FileEventSink::bytes_written() const noexcept {
    return bytes_written_;
}

}  // namespace opennova::parity
