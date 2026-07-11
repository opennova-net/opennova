#pragma once

#include <opennova/retail_hook/validation_session.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>

namespace opennova::retail_hook::windows {

class RetailCaptureAgent;

// Runs a modeless validation window and its message pump on the calling thread.
// A null session displays startup diagnostics without attempting to sample.
[[nodiscard]] DWORD run_validation_window(
    HINSTANCE module,
    ValidationSession* session,
    RetailCaptureAgent* capture,
    bool writes_enabled,
    std::wstring window_identity,
    std::wstring startup_diagnostic);

[[nodiscard]] DWORD run_validation_window(
    HINSTANCE module,
    ValidationSession* session,
    std::wstring startup_diagnostic);

}  // namespace opennova::retail_hook::windows
