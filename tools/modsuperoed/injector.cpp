#ifdef _WIN32

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr unsigned long long kExpectedExeSize = 2473984ull;
constexpr const char *kExpectedExeSha256 =
    "B3B07D1E4FF77EB7FAFBE4FFC97C182508460EBAA235C2B88F1CC0C64AAA8325";

struct Config {
    std::string target_dir;
    std::string proj_path;
    std::string out_path;
    std::string out_title;
    std::string log_dir;
    DWORD import_delay_ms = 5000;
    DWORD export_delay_ms = 5000;
    DWORD timeout_ms = 120000;
};

std::string last_error_message(const char *prefix) {
    std::ostringstream out;
    out << prefix << " (GetLastError=" << GetLastError() << ")";
    return out.str();
}

void trim(std::string &value) {
    const char *ws = " \t\r\n";
    const size_t begin = value.find_first_not_of(ws);
    if (begin == std::string::npos) {
        value.clear();
        return;
    }
    const size_t end = value.find_last_not_of(ws);
    value = value.substr(begin, end - begin + 1);
}

std::string dirname_of_exe() {
    char module_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    std::string base = module_path;
    const size_t pos = base.find_last_of("\\/");
    if (pos != std::string::npos) {
        base.erase(pos);
    }
    return base;
}

bool read_config(const std::string &path, Config &cfg, std::string &error) {
    std::ifstream file(path);
    if (!file) {
        error = "injector config not found: " + path;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (cfg.target_dir.empty()) {
                cfg.target_dir = line;
            }
            continue;
        }

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key);
        trim(val);
        if (key == "dir" || key == "target") {
            cfg.target_dir = val;
        } else if (key == "proj" || key == "3dp") {
            cfg.proj_path = val;
        } else if (key == "out" || key == "3di") {
            cfg.out_path = val;
        } else if (key == "title") {
            cfg.out_title = val;
        } else if (key == "log") {
            cfg.log_dir = val;
        } else if (key == "import_delay_ms") {
            cfg.import_delay_ms = std::strtoul(val.c_str(), nullptr, 10);
        } else if (key == "export_delay_ms") {
            cfg.export_delay_ms = std::strtoul(val.c_str(), nullptr, 10);
        } else if (key == "timeout_ms") {
            cfg.timeout_ms = std::strtoul(val.c_str(), nullptr, 10);
        }
    }

    if (cfg.target_dir.empty()) {
        error = "injector config missing dir=...";
        return false;
    }
    if (cfg.proj_path.empty()) {
        error = "injector config missing proj=...";
        return false;
    }
    return true;
}

bool file_exists(const std::string &path) {
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool file_size(const std::string &path, unsigned long long &size) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr)) {
        return false;
    }
    ULARGE_INTEGER value{};
    value.LowPart = attr.nFileSizeLow;
    value.HighPart = attr.nFileSizeHigh;
    size = value.QuadPart;
    return true;
}

std::string hex_digest(const BYTE *bytes, DWORD count) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(static_cast<size_t>(count) * 2);
    for (DWORD i = 0; i < count; ++i) {
        out.push_back(digits[(bytes[i] >> 4) & 0x0F]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

bool sha256_file(const std::string &path, std::string &digest, std::string &error) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        error = last_error_message("CryptAcquireContextA failed");
        return false;
    }
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        error = last_error_message("CryptCreateHash failed");
        CryptReleaseContext(provider, 0);
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "failed to open " + path;
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0 &&
            !CryptHashData(hash, reinterpret_cast<const BYTE *>(buffer.data()),
                           static_cast<DWORD>(got), 0)) {
            error = last_error_message("CryptHashData failed");
            CryptDestroyHash(hash);
            CryptReleaseContext(provider, 0);
            return false;
        }
    }

    BYTE bytes[32]{};
    DWORD byte_count = sizeof(bytes);
    if (!CryptGetHashParam(hash, HP_HASHVAL, bytes, &byte_count, 0)) {
        error = last_error_message("CryptGetHashParam failed");
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return false;
    }

    digest = hex_digest(bytes, byte_count);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return true;
}

bool verify_modsuperoed(const std::string &exe, std::string &error) {
    if (!file_exists(exe)) {
        error = "ModSuperOED executable not found: " + exe;
        return false;
    }

    unsigned long long size = 0;
    if (!file_size(exe, size)) {
        error = last_error_message("failed to stat ModSuperOED");
        return false;
    }
    if (size != kExpectedExeSize) {
        std::ostringstream out;
        out << "unexpected ModSuperOED size: " << size << " != " << kExpectedExeSize;
        error = out.str();
        return false;
    }

    std::string digest;
    if (!sha256_file(exe, digest, error)) {
        return false;
    }
    if (digest != kExpectedExeSha256) {
        error = "unexpected ModSuperOED sha256: " + digest;
        return false;
    }
    return true;
}

std::string quote_arg(const std::string &value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool inject_dll(HANDLE process, const std::string &dll_path, std::string &error) {
    const size_t len = dll_path.size() + 1;
    LPVOID remote = VirtualAllocEx(process, nullptr, len, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!remote) {
        error = last_error_message("VirtualAllocEx failed");
        return false;
    }
    if (!WriteProcessMemory(process, remote, dll_path.c_str(), len, nullptr)) {
        error = last_error_message("WriteProcessMemory failed");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC load_library = kernel32 ? GetProcAddress(kernel32, "LoadLibraryA") : nullptr;
    if (!load_library) {
        error = "LoadLibraryA not found";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
        remote, 0, nullptr);
    if (!thread) {
        error = last_error_message("CreateRemoteThread failed");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(thread, INFINITE);

    DWORD load_result = 0;
    GetExitCodeThread(thread, &load_result);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (load_result == 0) {
        error = "remote LoadLibraryA returned NULL";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string base_dir = dirname_of_exe();
    const std::string cfg_path =
        argc > 1 && argv && argv[1] && argv[1][0]
            ? std::string(argv[1])
            : base_dir + "\\injector.cfg";

    Config cfg;
    std::string error;
    if (!read_config(cfg_path, cfg, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    const std::string exe = cfg.target_dir + "\\ModSuperOed.exe";
    const std::string dll = base_dir + "\\modsuperoed_hook.dll";
    if (!file_exists(dll)) {
        std::fprintf(stderr, "hook DLL not found: %s\n", dll.c_str());
        return 2;
    }
    if (!verify_modsuperoed(exe, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 125;
    }

    std::string cmdline = quote_arg(exe);
    cmdline += " --oed-proj=" + quote_arg(cfg.proj_path);
    if (!cfg.out_path.empty()) {
        cmdline += " --oed-3di=" + quote_arg(cfg.out_path);
    }
    if (!cfg.out_title.empty()) {
        cmdline += " --oed-title=" + quote_arg(cfg.out_title);
    }
    if (!cfg.log_dir.empty()) {
        cmdline += " --oed-log-dir=" + quote_arg(cfg.log_dir);
    }
    cmdline += " --oed-import-delay-ms=" + std::to_string(cfg.import_delay_ms);
    cmdline += " --oed-export-delay-ms=" + std::to_string(cfg.export_delay_ms);
    cmdline += " --oed-timeout-ms=" + std::to_string(cfg.timeout_ms);

    std::vector<char> cmd_buffer(cmdline.begin(), cmdline.end());
    cmd_buffer.push_back('\0');

    STARTUPINFOA startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    if (!CreateProcessA(exe.c_str(), cmd_buffer.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, cfg.target_dir.c_str(), &startup,
                        &process)) {
        std::fprintf(stderr, "CreateProcessA failed: %lu\n", GetLastError());
        return 1;
    }

    if (!inject_dll(process.hProcess, dll, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
    }

    ResumeThread(process.hThread);
    const DWORD wait_result = WaitForSingleObject(process.hProcess, cfg.timeout_ms + 10000);
    DWORD exit_code = 0;
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        exit_code = 124;
    } else {
        GetExitCodeProcess(process.hProcess, &exit_code);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

#else
int main() {
    return 0;
}
#endif
