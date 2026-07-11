#include <opennova/retail_hook/validation_session.h>
#include <opennova/retail_hook/windows/file_sha256.h>
#include <opennova/retail_hook/windows/hook_start_config.h>
#include <opennova/retail_hook/windows/local_process_memory.h>
#include <opennova/retail_hook/windows/retail_capture_agent.h>
#include <opennova/retail_hook/windows/validation_window.h>
#include <opennova/retail_hook/windows/winsock_capture.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <cwchar>
#include <memory>
#include <new>
#include <process.h>
#include <string>
#include <string_view>
#include <vector>

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:OpenNovaRetailHook_Start=_OpenNovaRetailHook_Start@4")
#endif

namespace opennova::retail_hook::windows {
namespace {

static_assert(sizeof(void*) == 4, "The retail hook DLL must be compiled for 32-bit Windows.");

HINSTANCE g_module = nullptr;
std::atomic_bool g_started{false};

struct WinsockCaptureGuard {
    void* image{};
    bool installed{};

    ~WinsockCaptureGuard() {
        restore();
    }

    void restore() noexcept {
        if (installed) {
            (void)uninstall_winsock_capture(image);
            installed = false;
        }
    }
};

[[nodiscard]] bool executable_path(std::wstring& path) {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD copied = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return false;
        }
        if (copied < buffer.size()) {
            path.assign(buffer.data(), copied);
            return true;
        }
        buffer.resize(buffer.size() * 2);
    }
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return false;
}

[[nodiscard]] bool image_size(HMODULE image, std::uint32_t& size) noexcept {
    size = 0;
    if (image == nullptr) {
        return false;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(image);
    const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew <= 0) {
        return false;
    }
    const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        base + static_cast<std::uint32_t>(dos_header->e_lfanew));
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE ||
        nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt_headers->OptionalHeader.SizeOfImage == 0) {
        return false;
    }
    size = nt_headers->OptionalHeader.SizeOfImage;
    return true;
}

[[nodiscard]] std::wstring widen_ascii(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

[[nodiscard]] std::wstring check_diagnostic(const CheckResult& check) {
    return L"Session rejected [" +
        widen_ascii(validation_error_name(check.error)) + L"]: " +
        widen_ascii(check.detail);
}

template <std::size_t Capacity>
[[nodiscard]] bool configured_text(
    const wchar_t (&value)[Capacity]) noexcept {
    return value[0] != L'\0' &&
        std::wmemchr(value, L'\0', Capacity) != nullptr;
}

[[nodiscard]] bool valid_start_config(
    const HookStartConfig& config) noexcept {
    const bool valid_role =
        config.role == HookRole::retail_host ||
        config.role == HookRole::retail_client;
    return config.structure_size == sizeof(HookStartConfig) &&
        config.version == kHookStartConfigVersion &&
        config.allow_writes <= 1 &&
        valid_role &&
        configured_text(config.pipe_name) &&
        configured_text(config.run_id) &&
        configured_text(config.stream_id) &&
        configured_text(config.scenario) &&
        configured_text(config.mission);
}

[[nodiscard]] DWORD run_worker(const HookStartConfig& config) {
    std::wstring path;
    if (!executable_path(path)) {
        return run_validation_window(
            g_module,
            nullptr,
            L"Could not determine the running executable path (error " +
                std::to_wstring(GetLastError()) + L").");
    }

    FileSha256 digest{};
    std::uint32_t hash_error = ERROR_SUCCESS;
    if (!compute_file_sha256(path.c_str(), digest, &hash_error)) {
        return run_validation_window(
            g_module,
            nullptr,
            L"Could not SHA-256 the running executable (error " +
                std::to_wstring(hash_error) + L").");
    }

    HMODULE executable = GetModuleHandleW(nullptr);
    std::uint32_t executable_image_size = 0;
    if (!image_size(executable, executable_image_size)) {
        return run_validation_window(
            g_module, nullptr, L"The running executable has invalid PE headers.");
    }

    ExecutableIdentity identity{};
    identity.sha256 = digest;
    identity.image_size = executable_image_size;

    const bool writes_enabled = config.allow_writes != 0;
    LocalProcessMemory memory(
        writes_enabled
        ? WritePolicy::permit_writable_pages
        : WritePolicy::deny);
    SessionOptions options{};
    options.writes_enabled = writes_enabled;
    const ProcessAddress image_base = static_cast<ProcessAddress>(
        reinterpret_cast<std::uintptr_t>(executable));
    OpenResult opened = ValidationSession::open(
        memory, image_base, identity, jo_1_7_5_7_profile(), options);
    if (!opened) {
        return run_validation_window(
            g_module, nullptr, check_diagnostic(opened.check));
    }

    RetailCaptureAgent capture(config, opened.session.get());
    const bool capture_started = capture.start();
    WinsockHookInstallResult winsock{};
    if (capture_started) {
        winsock = install_winsock_capture(
            executable, capture.wire_sink());
        if (!winsock.complete()) {
            parity::DiagnosticEvent diagnostic{};
            diagnostic.timestamp_ns =
                static_cast<std::uint64_t>(GetTickCount64()) * 1'000'000ULL;
            diagnostic.severity = parity::Severity::warning;
            diagnostic.code = "retail_winsock_hooks_incomplete";
            diagnostic.message =
                "Not every witnessed WS2_32 import boundary was hooked.";
            diagnostic.context.push_back(parity::Field{
                "installed_mask",
                static_cast<std::uint64_t>(winsock.installed_mask),
            });
            (void)capture.try_capture(diagnostic);
        }
    }
    WinsockCaptureGuard winsock_guard{
        executable, winsock.installed_mask != 0};

    std::wstring window_identity =
        config.role == HookRole::retail_host
        ? L"retail-host"
        : L"retail-client";
    window_identity += L" [";
    window_identity += config.stream_id;
    window_identity += L"]";
    std::wstring startup = L"Role: ";
    startup += config.role == HookRole::retail_host
        ? L"retail-host"
        : L"retail-client";
    startup += L" | stream: ";
    startup += config.stream_id;
    startup +=
        L". Supported executable profile accepted; parity capture is ";
    startup += capture_started ? L"active" : L"unavailable";
    startup += winsock.complete()
        ? L"; all four witnessed Winsock imports are captured"
        : L"; Winsock capture is incomplete";
    startup += writes_enabled && capture_started
        ? L"; typed mutation writes are EXPLICITLY ENABLED."
        : writes_enabled
            ? L"; mutation controls are suppressed because audit capture "
              L"is unavailable."
            : L"; mutation access is read-only.";
    const DWORD window_result = run_validation_window(
        g_module,
        opened.session.get(),
        capture_started ? &capture : nullptr,
        writes_enabled && capture_started,
        std::move(window_identity),
        std::move(startup));

    winsock_guard.restore();
    capture.stop();
    return window_result;
}

unsigned __stdcall validation_worker(void* parameter) {
    std::unique_ptr<HookStartConfig> config(
        static_cast<HookStartConfig*>(parameter));
    DWORD result = ERROR_SUCCESS;
    try {
        result = run_worker(*config);
    } catch (const std::exception& exception) {
        result = run_validation_window(
            g_module,
            nullptr,
            L"Validation worker exception: " + widen_ascii(exception.what()));
    } catch (...) {
        result = run_validation_window(
            g_module, nullptr, L"Validation worker failed with an unknown exception.");
    }
    g_started.store(false);
    return result;
}

}  // namespace
}  // namespace opennova::retail_hook::windows

extern "C" DWORD WINAPI
OpenNovaRetailHook_Start(void* parameter) {
    using namespace opennova::retail_hook::windows;
    if (parameter == nullptr) {
        return ERROR_INVALID_PARAMETER;
    }
    HookStartConfig received{};
    SIZE_T copied = 0;
    const BOOL read = ReadProcessMemory(
            GetCurrentProcess(),
            parameter,
            &received,
            sizeof(received),
            &copied);
    if (read == FALSE) {
        return GetLastError();
    }
    if (copied != sizeof(received)) {
        return ERROR_PARTIAL_COPY;
    }
    if (!valid_start_config(received)) {
        return ERROR_INVALID_DATA;
    }
    auto* owned = new (std::nothrow) HookStartConfig(received);
    if (owned == nullptr) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        delete owned;
        return ERROR_ALREADY_EXISTS;
    }

    const std::uintptr_t worker = _beginthreadex(
        nullptr, 0, &validation_worker, owned, 0, nullptr);
    if (worker == 0) {
        delete owned;
        g_started.store(false);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    CloseHandle(reinterpret_cast<HANDLE>(worker));
    return ERROR_SUCCESS;
}

BOOL APIENTRY DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        opennova::retail_hook::windows::g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
