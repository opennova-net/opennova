#pragma once

#include <array>
#include <cstdint>

namespace opennova::retail_hook::windows {

using FileSha256 = std::array<std::uint8_t, 32>;

// Hashes the named file with Windows CNG. When win32_error is non-null, it is
// set to ERROR_SUCCESS on success or a diagnostic Win32 error on failure.
[[nodiscard]] bool compute_file_sha256(
    const wchar_t* path,
    FileSha256& digest,
    std::uint32_t* win32_error = nullptr);

}  // namespace opennova::retail_hook::windows
