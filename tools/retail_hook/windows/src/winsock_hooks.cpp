#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>

#include <opennova/retail_hook/windows/winsock_capture.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace opennova::retail_hook::windows {
namespace {

static_assert(sizeof(void*) == 4, "retail Winsock hooks require 32-bit Windows");

using ReceiveFn = int (WSAAPI*)(SOCKET, char*, int, int);
using ReceiveFromFn =
    int (WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using SendFn = int (WSAAPI*)(SOCKET, const char*, int, int);
using SendToFn =
    int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);

std::atomic<IWireCaptureSink*> g_sink{};
std::array<void*, 4> g_originals{};
std::array<void**, 4> g_slots{};
std::uint8_t g_installed_mask{};
void* g_image_module{};

[[nodiscard]] constexpr std::size_t call_index(WinsockCall call) noexcept {
    return static_cast<std::size_t>(call);
}

[[nodiscard]] WireEndpoint endpoint_from(const sockaddr* address) noexcept {
    if (address == nullptr || address->sa_family != AF_INET) {
        return {};
    }
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    return WireEndpoint{
        ipv4->sin_addr.s_addr,
        ntohs(ipv4->sin_port),
        true,
    };
}

void capture_result(WinsockCall call,
                    SOCKET socket,
                    const void* payload,
                    int available,
                    int result,
                    const sockaddr* remote_hint) noexcept {
    IWireCaptureSink* sink = g_sink.load(std::memory_order_acquire);
    if (sink == nullptr || result <= 0) {
        return;
    }
    WireEndpoint local{};
    sockaddr_storage local_address{};
    int local_size = sizeof(local_address);
    if (getsockname(
            socket,
            reinterpret_cast<sockaddr*>(&local_address),
            &local_size) == 0) {
        local = endpoint_from(
            reinterpret_cast<const sockaddr*>(&local_address));
    }
    WireEndpoint remote = endpoint_from(remote_hint);
    if (!remote.valid) {
        sockaddr_storage peer_address{};
        int peer_size = sizeof(peer_address);
        if (getpeername(
                socket,
                reinterpret_cast<sockaddr*>(&peer_address),
                &peer_size) == 0) {
            remote = endpoint_from(
                reinterpret_cast<const sockaddr*>(&peer_address));
        }
    }
    const std::size_t available_size =
        available > 0 ? static_cast<std::size_t>(available) : 0;
    capture_completed_winsock_call(
        *sink,
        call,
        static_cast<std::uintptr_t>(socket),
        reinterpret_cast<const std::uint8_t*>(payload),
        available_size,
        result,
        local,
        remote);
}

int WSAAPI hook_receive(SOCKET socket, char* buffer, int length, int flags) {
    const auto original = reinterpret_cast<ReceiveFn>(
        g_originals[call_index(WinsockCall::receive)]);
    const int result = original != nullptr
        ? original(socket, buffer, length, flags)
        : SOCKET_ERROR;
    const int error = WSAGetLastError();
    capture_result(
        WinsockCall::receive, socket, buffer, length, result, nullptr);
    WSASetLastError(error);
    return result;
}

int WSAAPI hook_receive_from(SOCKET socket,
                             char* buffer,
                             int length,
                             int flags,
                             sockaddr* from,
                             int* from_length) {
    const auto original = reinterpret_cast<ReceiveFromFn>(
        g_originals[call_index(WinsockCall::receive_from)]);
    const int result = original != nullptr
        ? original(socket, buffer, length, flags, from, from_length)
        : SOCKET_ERROR;
    const int error = WSAGetLastError();
    capture_result(
        WinsockCall::receive_from,
        socket,
        buffer,
        length,
        result,
        from);
    WSASetLastError(error);
    return result;
}

int WSAAPI hook_send(
    SOCKET socket, const char* buffer, int length, int flags) {
    const auto original = reinterpret_cast<SendFn>(
        g_originals[call_index(WinsockCall::send)]);
    const int result = original != nullptr
        ? original(socket, buffer, length, flags)
        : SOCKET_ERROR;
    const int error = WSAGetLastError();
    capture_result(
        WinsockCall::send, socket, buffer, length, result, nullptr);
    WSASetLastError(error);
    return result;
}

int WSAAPI hook_send_to(SOCKET socket,
                        const char* buffer,
                        int length,
                        int flags,
                        const sockaddr* to,
                        int to_length) {
    const auto original = reinterpret_cast<SendToFn>(
        g_originals[call_index(WinsockCall::send_to)]);
    const int result = original != nullptr
        ? original(socket, buffer, length, flags, to, to_length)
        : SOCKET_ERROR;
    const int error = WSAGetLastError();
    capture_result(
        WinsockCall::send_to,
        socket,
        buffer,
        length,
        result,
        to);
    WSASetLastError(error);
    return result;
}

[[nodiscard]] void* replacement_for(WinsockCall call) noexcept {
    switch (call) {
    case WinsockCall::receive:
        return reinterpret_cast<void*>(&hook_receive);
    case WinsockCall::receive_from:
        return reinterpret_cast<void*>(&hook_receive_from);
    case WinsockCall::send:
        return reinterpret_cast<void*>(&hook_send);
    case WinsockCall::send_to:
        return reinterpret_cast<void*>(&hook_send_to);
    }
    return nullptr;
}

[[nodiscard]] bool range_in_image(
    std::uint32_t rva,
    std::size_t size,
    std::uint32_t image_size) noexcept {
    return static_cast<std::uint64_t>(rva) + size <= image_size;
}

[[nodiscard]] bool ws2_import_name(
    const std::uint8_t* base,
    std::uint32_t image_size,
    std::uint32_t name_rva) noexcept {
    constexpr char kName[] = "WS2_32.dll";
    if (!range_in_image(name_rva, sizeof(kName), image_size)) {
        return false;
    }
    return _strnicmp(
        reinterpret_cast<const char*>(base + name_rva),
        kName,
        sizeof(kName)) == 0;
}

[[nodiscard]] bool find_iat_slot(
    HMODULE image,
    FARPROC imported_function,
    void*** result) noexcept {
    *result = nullptr;
    if (image == nullptr || imported_function == nullptr) {
        return false;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        base + static_cast<std::uint32_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }
    const std::uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0 ||
        !range_in_image(
            imports.VirtualAddress, imports.Size, image_size)) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + imports.VirtualAddress);
    const std::size_t descriptor_count =
        imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t index = 0; index < descriptor_count; ++index) {
        if (descriptor[index].Name == 0) {
            break;
        }
        if (!ws2_import_name(base, image_size, descriptor[index].Name) ||
            descriptor[index].FirstThunk == 0 ||
            !range_in_image(
                descriptor[index].FirstThunk,
                sizeof(IMAGE_THUNK_DATA32),
                image_size)) {
            continue;
        }

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            base + descriptor[index].FirstThunk);
        const std::size_t maximum_thunks =
            (image_size - descriptor[index].FirstThunk) /
            sizeof(IMAGE_THUNK_DATA32);
        for (std::size_t thunk_index = 0;
             thunk_index < maximum_thunks &&
             thunk[thunk_index].u1.Function != 0;
             ++thunk_index) {
            const auto current = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(
                    thunk[thunk_index].u1.Function));
            if (current == reinterpret_cast<void*>(imported_function)) {
                *result = reinterpret_cast<void**>(
                    &thunk[thunk_index].u1.Function);
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool replace_slot(
    void** slot,
    void* replacement,
    void*& previous) noexcept {
    DWORD old_protection = 0;
    if (slot == nullptr || replacement == nullptr ||
        VirtualProtect(
            slot, sizeof(*slot), PAGE_READWRITE, &old_protection) == FALSE) {
        return false;
    }
    previous = InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(slot), replacement);
    DWORD ignored = 0;
    (void)VirtualProtect(
        slot, sizeof(*slot), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    return true;
}

}  // namespace

WinsockHookInstallResult install_winsock_capture(
    void* image_module,
    IWireCaptureSink& sink) noexcept {
    if (g_installed_mask != 0) {
        g_sink.store(&sink, std::memory_order_release);
        return WinsockHookInstallResult{g_installed_mask};
    }
    if (image_module == nullptr) {
        return {};
    }
    HMODULE winsock = GetModuleHandleW(L"ws2_32.dll");
    if (winsock == nullptr) {
        return {};
    }

    for (const WinsockImport& imported : winsock_capture_imports()) {
        FARPROC target = GetProcAddress(winsock, imported.name);
        void** slot = nullptr;
        if (!find_iat_slot(
                static_cast<HMODULE>(image_module), target, &slot)) {
            continue;
        }

        void* previous = nullptr;
        if (!replace_slot(
                slot, replacement_for(imported.call), previous)) {
            continue;
        }
        const std::size_t index = call_index(imported.call);
        g_originals[index] = previous;
        g_slots[index] = slot;
        g_installed_mask |= static_cast<std::uint8_t>(1U << index);
    }

    g_image_module = image_module;
    g_sink.store(&sink, std::memory_order_release);
    return WinsockHookInstallResult{g_installed_mask};
}

bool uninstall_winsock_capture(void* image_module) noexcept {
    if (image_module == nullptr || image_module != g_image_module) {
        return false;
    }
    g_sink.store(nullptr, std::memory_order_release);
    bool restored_all = true;
    for (std::size_t index = 0; index < g_slots.size(); ++index) {
        const auto bit = static_cast<std::uint8_t>(1U << index);
        if ((g_installed_mask & bit) == 0) {
            continue;
        }
        void* ignored = nullptr;
        if (!replace_slot(
                g_slots[index], g_originals[index], ignored)) {
            restored_all = false;
            continue;
        }
        g_slots[index] = nullptr;
        g_originals[index] = nullptr;
        g_installed_mask &= static_cast<std::uint8_t>(~bit);
    }
    if (g_installed_mask == 0) {
        g_image_module = nullptr;
    }
    return restored_all && g_installed_mask == 0;
}

}  // namespace opennova::retail_hook::windows
