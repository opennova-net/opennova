#include <opennova/retail_hook/windows/file_sha256.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace opennova::retail_hook::windows {
namespace {

static_assert(sizeof(void*) == 4, "The retail hook must be compiled for 32-bit Windows.");

void set_error(std::uint32_t* destination, DWORD error) noexcept {
    if (destination != nullptr) {
        *destination = error;
    }
}

[[nodiscard]] bool nt_success(NTSTATUS status) noexcept {
    return status >= 0;
}

}  // namespace

bool compute_file_sha256(
    const wchar_t* path,
    FileSha256& digest,
    std::uint32_t* win32_error) {
    digest.fill(0);
    set_error(win32_error, ERROR_SUCCESS);
    if (path == nullptr || *path == L'\0') {
        set_error(win32_error, ERROR_INVALID_PARAMETER);
        return false;
    }

    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(win32_error, GetLastError());
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> hash_object;
    bool success = false;
    DWORD failure = ERROR_GEN_FAILURE;

    do {
        if (!nt_success(BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0))) {
            break;
        }

        DWORD object_size = 0;
        DWORD result_size = 0;
        if (!nt_success(BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &result_size,
                0)) ||
            result_size != sizeof(object_size) ||
            object_size == 0) {
            break;
        }

        DWORD digest_size = 0;
        if (!nt_success(BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_size),
                sizeof(digest_size),
                &result_size,
                0)) ||
            result_size != sizeof(digest_size) ||
            digest_size != digest.size()) {
            failure = ERROR_INVALID_DATA;
            break;
        }

        hash_object.resize(object_size);
        if (!nt_success(BCryptCreateHash(
                algorithm,
                &hash,
                hash_object.data(),
                static_cast<ULONG>(hash_object.size()),
                nullptr,
                0,
                0))) {
            break;
        }

        std::array<UCHAR, 64 * 1024> buffer{};
        for (;;) {
            DWORD bytes_read = 0;
            if (ReadFile(
                    file,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &bytes_read,
                    nullptr) == FALSE) {
                failure = GetLastError();
                break;
            }
            if (bytes_read == 0) {
                if (nt_success(BCryptFinishHash(
                        hash,
                        digest.data(),
                        static_cast<ULONG>(digest.size()),
                        0))) {
                    success = true;
                    failure = ERROR_SUCCESS;
                }
                break;
            }
            if (!nt_success(BCryptHashData(hash, buffer.data(), bytes_read, 0))) {
                break;
            }
        }
    } while (false);

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);
    if (!success) {
        digest.fill(0);
        set_error(win32_error, failure);
    }
    return success;
}

}  // namespace opennova::retail_hook::windows
