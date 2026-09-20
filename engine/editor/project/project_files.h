#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::editor {

// The editor's plain file plumbing: whole-file reads and the atomic write every
// editor save uses (write `<path>.tmp`, then rename over `path`, so a crash leaves
// either the old file or the new one). `error` carries the OS reason on failure.
bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out, std::string &error);
bool read_file_text(const std::string &path, std::string &out, std::string &error);
bool write_file_atomic(const std::string &path, const void *data, size_t size, std::string &error);
bool write_file_atomic(const std::string &path, const std::string &text, std::string &error);

// mkdir -p; true when the directory exists afterwards.
bool ensure_directory(const std::string &path, std::string &error);

} // namespace opennova::editor
