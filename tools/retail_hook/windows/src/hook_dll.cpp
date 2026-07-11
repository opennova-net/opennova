#include <opennova/retail_hook/validation_session.h>
#include <opennova/retail_hook/windows/file_sha256.h>
#include <opennova/retail_hook/windows/local_process_memory.h>
#include <opennova/retail_hook/windows/validation_window.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
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

[[nodiscard]] DWORD run_worker() {
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

    LocalProcessMemory memory;
    SessionOptions options{};
    options.writes_enabled = false;
    const ProcessAddress image_base = static_cast<ProcessAddress>(
        reinterpret_cast<std::uintptr_t>(executable));
    OpenResult opened = ValidationSession::open(
        memory, image_base, identity, jo_1_7_5_7_profile(), options);
    if (!opened) {
        return run_validation_window(
            g_module, nullptr, check_diagnostic(opened.check));
    }

    return run_validation_window(
        g_module,
        opened.session.get(),
        L"Supported executable profile accepted; mutation access is disabled.");
}

unsigned __stdcall validation_worker(void*) {
    DWORD result = ERROR_SUCCESS;
    try {
        result = run_worker();
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

extern "C" __declspec(dllexport) DWORD WINAPI
OpenNovaRetailHook_Start(void*) {
    using namespace opennova::retail_hook::windows;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return ERROR_ALREADY_EXISTS;
    }

    const std::uintptr_t worker = _beginthreadex(
        nullptr, 0, &validation_worker, nullptr, 0, nullptr);
    if (worker == 0) {
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
