#include <opennova/retail_hook/windows/file_sha256.h>
#include <opennova/retail_hook/validation_session.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using opennova::retail_hook::windows::compute_file_sha256;
using opennova::retail_hook::windows::FileSha256;
using opennova::retail_hook::jo_1_7_5_7_profile;

static_assert(sizeof(void*) == 4, "The retail injector must be compiled for 32-bit Windows.");

constexpr DWORD kRemoteCallTimeoutMs = 30000;
constexpr int kModuleSnapshotAttempts = 50;
constexpr DWORD kModuleSnapshotRetryMs = 10;

void report_error(std::wstring_view operation, DWORD error) {
    wchar_t* system_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr);
    std::wcerr << operation << L" failed (" << error << L")";
    if (length != 0 && system_message != nullptr) {
        std::wcerr << L": " << system_message;
    } else {
        std::wcerr << L"\n";
    }
    if (system_message != nullptr) {
        LocalFree(system_message);
    }
}

[[nodiscard]] bool absolute_path(
    const wchar_t* input,
    std::wstring& output,
    DWORD& error) {
    error = ERROR_SUCCESS;
    const DWORD required = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (required == 0) {
        error = GetLastError();
        return false;
    }
    std::vector<wchar_t> buffer(required);
    const DWORD copied = GetFullPathNameW(
        input, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (copied == 0 || copied >= buffer.size()) {
        error = copied == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return false;
    }
    output.assign(buffer.data(), copied);
    return true;
}

[[nodiscard]] std::wstring_view filename(std::wstring_view path) noexcept {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] std::wstring parent_directory(std::wstring_view path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) {
        return L".";
    }
    if (slash == 2 && path[1] == L':') {
        return std::wstring(path.substr(0, 3));
    }
    return std::wstring(path.substr(0, slash));
}

[[nodiscard]] bool ansi_path(
    std::wstring_view input,
    std::string& output,
    DWORD& error) {
    error = ERROR_SUCCESS;
    if (input.empty() ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    BOOL used_default = FALSE;
    const int required = WideCharToMultiByte(
        CP_ACP,
        WC_NO_BEST_FIT_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0,
        nullptr,
        &used_default);
    if (required <= 0) {
        error = GetLastError();
        return false;
    }
    output.resize(static_cast<std::size_t>(required));
    used_default = FALSE;
    if (WideCharToMultiByte(
            CP_ACP,
            WC_NO_BEST_FIT_CHARS,
            input.data(),
            static_cast<int>(input.size()),
            output.data(),
            required,
            nullptr,
            &used_default) != required ||
        used_default != FALSE) {
        output.clear();
        error = ERROR_NO_UNICODE_TRANSLATION;
        return false;
    }
    return true;
}

[[nodiscard]] std::wstring quote_argument(std::wstring_view argument) {
    std::wstring quoted(1, L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
[[nodiscard]] std::uintptr_t remote_module_base(
    DWORD process_id,
    const wchar_t* module_name,
    DWORD& error) {
    error = ERROR_SUCCESS;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kModuleSnapshotAttempts; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
        if (snapshot != INVALID_HANDLE_VALUE) {
            break;
        }
        error = GetLastError();
        if (error != ERROR_BAD_LENGTH && error != ERROR_PARTIAL_COPY) {
            return 0;
        }
        Sleep(kModuleSnapshotRetryMs);
    }
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry) == FALSE) {
        error = GetLastError();
        CloseHandle(snapshot);
        return 0;
    }
    do {
        if (_wcsicmp(entry.szModule, module_name) == 0) {
            const std::uintptr_t base =
                reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            CloseHandle(snapshot);
            return base;
        }
    } while (Module32NextW(snapshot, &entry) != FALSE);

    error = ERROR_MOD_NOT_FOUND;
    CloseHandle(snapshot);
    return 0;
}

[[nodiscard]] bool remote_load_library_a(
    DWORD process_id,
    std::uintptr_t& address,
    DWORD& error) {
    address = 0;
    error = ERROR_SUCCESS;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC local_proc = kernel32 == nullptr
        ? nullptr
        : GetProcAddress(kernel32, "LoadLibraryA");
    if (local_proc == nullptr) {
        error = GetLastError();
        return false;
    }

    HMODULE owner = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(local_proc),
            &owner) == FALSE) {
        error = GetLastError();
        return false;
    }

    std::array<wchar_t, MAX_PATH> owner_path{};
    const DWORD copied = GetModuleFileNameW(
        owner, owner_path.data(), static_cast<DWORD>(owner_path.size()));
    if (copied == 0 || copied >= owner_path.size()) {
        error = copied == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return false;
    }
    const std::wstring owner_name(filename(
        std::wstring_view(owner_path.data(), copied)));
    const std::uintptr_t remote_owner =
        remote_module_base(process_id, owner_name.c_str(), error);
    if (remote_owner == 0) {
        return false;
    }

    const std::uintptr_t local_owner = reinterpret_cast<std::uintptr_t>(owner);
    const std::uintptr_t local_address =
        reinterpret_cast<std::uintptr_t>(local_proc);
    if (local_address < local_owner) {
        error = ERROR_INVALID_ADDRESS;
        return false;
    }
    const std::uint64_t remote_address =
        static_cast<std::uint64_t>(remote_owner) +
        static_cast<std::uint64_t>(local_address - local_owner);
    if (remote_address > std::numeric_limits<std::uintptr_t>::max()) {
        error = ERROR_ARITHMETIC_OVERFLOW;
        return false;
    }
    address = static_cast<std::uintptr_t>(remote_address);
    return true;
}

[[nodiscard]] bool remote_call(
    HANDLE process,
    std::uintptr_t procedure,
    void* parameter,
    DWORD& exit_code,
    DWORD& error) {
    exit_code = 0;
    error = ERROR_SUCCESS;
    HANDLE thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(procedure),
        parameter,
        0,
        nullptr);
    if (thread == nullptr) {
        error = GetLastError();
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, kRemoteCallTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        CloseHandle(thread);
        return false;
    }
    if (GetExitCodeThread(thread, &exit_code) == FALSE) {
        error = GetLastError();
        CloseHandle(thread);
        return false;
    }
    CloseHandle(thread);
    return true;
}

[[nodiscard]] bool exported_start_rva(
    const std::wstring& hook_path,
    std::uint32_t& rva,
    DWORD& error) {
    rva = 0;
    error = ERROR_SUCCESS;
    HMODULE local_hook = LoadLibraryExW(
        hook_path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (local_hook == nullptr) {
        error = GetLastError();
        return false;
    }
    FARPROC start = GetProcAddress(local_hook, "OpenNovaRetailHook_Start");
    if (start == nullptr) {
        error = GetLastError();
        FreeLibrary(local_hook);
        return false;
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(local_hook);
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(start);
    if (address < base ||
        address - base > std::numeric_limits<std::uint32_t>::max()) {
        error = ERROR_ARITHMETIC_OVERFLOW;
        FreeLibrary(local_hook);
        return false;
    }
    rva = static_cast<std::uint32_t>(address - base);
    FreeLibrary(local_hook);
    return true;
}

[[nodiscard]] bool load_hook(
    HANDLE process,
    DWORD process_id,
    const std::string& hook_path,
    std::uintptr_t& remote_module,
    DWORD& error) {
    remote_module = 0;
    error = ERROR_SUCCESS;
    const SIZE_T path_size = hook_path.size() + 1;
    void* remote_path = VirtualAllocEx(
        process, nullptr, path_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (remote_path == nullptr) {
        error = GetLastError();
        return false;
    }

    SIZE_T written = 0;
    const BOOL write_result = WriteProcessMemory(
        process,
        remote_path,
        hook_path.c_str(),
        path_size,
        &written);
    const DWORD write_error = write_result == FALSE ? GetLastError() : ERROR_SUCCESS;
    if (write_result == FALSE || written != path_size) {
        error = write_result == FALSE ? write_error : ERROR_PARTIAL_COPY;
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    std::uintptr_t load_library = 0;
    if (!remote_load_library_a(process_id, load_library, error)) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    DWORD loaded = 0;
    const bool called =
        remote_call(process, load_library, remote_path, loaded, error);
    if (!called) {
        // A timeout does not stop the remote thread. Keep its argument alive;
        // the caller terminates the process on every failure path.
        return false;
    }
    const BOOL freed = VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    if (freed == FALSE) {
        error = GetLastError();
        return false;
    }
    if (loaded == 0) {
        error = ERROR_DLL_INIT_FAILED;
        return false;
    }
    remote_module = loaded;
    return true;
}

[[nodiscard]] bool start_hook(
    HANDLE process,
    std::uintptr_t remote_module,
    std::uint32_t start_rva,
    DWORD& error) {
    const std::uint64_t start =
        static_cast<std::uint64_t>(remote_module) + start_rva;
    if (start > std::numeric_limits<std::uintptr_t>::max()) {
        error = ERROR_ARITHMETIC_OVERFLOW;
        return false;
    }

    DWORD result = ERROR_SUCCESS;
    if (!remote_call(
            process,
            static_cast<std::uintptr_t>(start),
            nullptr,
            result,
            error)) {
        return false;
    }
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
        error = result;
        return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::wcerr
            << L"Usage: opennova_retail_injector.exe "
               L"<Jointops.exe> <opennova_retail_hook.dll> [game arguments...]\n";
        return 1;
    }

    DWORD error = ERROR_SUCCESS;
    std::wstring executable_path;
    std::wstring hook_path;
    if (!absolute_path(argv[1], executable_path, error)) {
        report_error(L"Resolve Jointops.exe path", error);
        return 2;
    }
    if (!absolute_path(argv[2], hook_path, error)) {
        report_error(L"Resolve hook DLL path", error);
        return 2;
    }

    const std::wstring executable_name(filename(executable_path));
    if (_wcsicmp(executable_name.c_str(), L"Jointops.exe") != 0) {
        std::wcerr << L"Refusing to launch a file not named Jointops.exe.\n";
        return 2;
    }
    const DWORD executable_attributes = GetFileAttributesW(executable_path.c_str());
    if (executable_attributes == INVALID_FILE_ATTRIBUTES ||
        (executable_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        const DWORD attribute_error = executable_attributes == INVALID_FILE_ATTRIBUTES
            ? GetLastError()
            : ERROR_DIRECTORY;
        report_error(L"Open Jointops.exe", attribute_error);
        return 2;
    }
    const DWORD hook_attributes = GetFileAttributesW(hook_path.c_str());
    if (hook_attributes == INVALID_FILE_ATTRIBUTES ||
        (hook_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        const DWORD attribute_error = hook_attributes == INVALID_FILE_ATTRIBUTES
            ? GetLastError()
            : ERROR_DIRECTORY;
        report_error(L"Open hook DLL", attribute_error);
        return 2;
    }

    FileSha256 actual_digest{};
    std::uint32_t hash_error = ERROR_SUCCESS;
    if (!compute_file_sha256(
            executable_path.c_str(), actual_digest, &hash_error)) {
        report_error(L"SHA-256 Jointops.exe", hash_error);
        return 2;
    }
    if (actual_digest != jo_1_7_5_7_profile().identity.sha256) {
        std::wcerr
            << L"Refusing to inject: Jointops.exe SHA-256 is not the "
               L"supported patched 9a1035440a53af2057ce0995ac42dced840d3b9fd"
               L"53c04dc86041a962b84fe57 build.\n";
        return 2;
    }

    std::string hook_path_ansi;
    if (!ansi_path(hook_path, hook_path_ansi, error)) {
        report_error(L"Encode hook path for LoadLibraryA", error);
        return 2;
    }
    std::uint32_t start_rva = 0;
    if (!exported_start_rva(hook_path, start_rva, error)) {
        report_error(L"Locate OpenNovaRetailHook_Start export", error);
        return 2;
    }

    std::wstring command_line = quote_argument(executable_path);
    for (int index = 3; index < argc; ++index) {
        command_line.push_back(L' ');
        command_line += quote_argument(argv[index]);
    }
    const std::wstring working_directory = parent_directory(executable_path);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            executable_path.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            working_directory.c_str(),
            &startup,
            &process) == FALSE) {
        report_error(L"Create Jointops.exe", GetLastError());
        return 3;
    }

    const auto abort_launch = [&process]() {
        DWORD exit_code = STILL_ACTIVE;
        const bool already_exited =
            GetExitCodeProcess(process.hProcess, &exit_code) != FALSE &&
            exit_code != STILL_ACTIVE;
        if (!already_exited &&
            TerminateProcess(process.hProcess, ERROR_CANCELLED) == FALSE) {
            report_error(L"Terminate failed launch", GetLastError());
            std::wcerr << L"Process " << process.dwProcessId
                       << L" may still be running and must be terminated manually.\n";
        } else if (!already_exited) {
            const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
            if (wait == WAIT_FAILED) {
                report_error(L"Wait for failed launch termination", GetLastError());
            } else if (wait == WAIT_TIMEOUT) {
                std::wcerr << L"Timed out waiting for failed process "
                           << process.dwProcessId << L" to terminate.\n";
            }
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 3;
    };

    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        report_error(L"Start Jointops.exe loader", GetLastError());
        return abort_launch();
    }
    const DWORD idle_wait = WaitForInputIdle(
        process.hProcess, kRemoteCallTimeoutMs);
    if (idle_wait == WAIT_FAILED) {
        report_error(L"Wait for Jointops.exe loader", GetLastError());
        return abort_launch();
    }
    if (idle_wait == WAIT_TIMEOUT) {
        report_error(L"Wait for Jointops.exe loader", ERROR_TIMEOUT);
        return abort_launch();
    }

    std::uintptr_t remote_hook = 0;
    if (!load_hook(
            process.hProcess,
            process.dwProcessId,
            hook_path_ansi,
            remote_hook,
            error)) {
        report_error(L"Inject hook with LoadLibraryA", error);
        return abort_launch();
    }
    if (!start_hook(process.hProcess, remote_hook, start_rva, error)) {
        report_error(L"Invoke OpenNovaRetailHook_Start", error);
        return abort_launch();
    }
    const DWORD process_id = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::wcout
        << L"Validated supported Jointops.exe, injected the read-only hook, "
           L"and started validation outside loader lock in process "
        << process_id << L".\n";
    return 0;
}
