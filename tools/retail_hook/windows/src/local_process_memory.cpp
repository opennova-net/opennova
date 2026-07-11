#include <opennova/retail_hook/windows/local_process_memory.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstring>
#include <limits>

namespace opennova::retail_hook::windows {
namespace {

static_assert(sizeof(void*) == 4, "The retail hook must be compiled for 32-bit Windows.");

[[nodiscard]] bool is_readable_protection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protection & 0xffU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_writable_protection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protection & 0xffU) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

}  // namespace

LocalProcessMemory::LocalProcessMemory(WritePolicy write_policy) noexcept
    : write_policy_(write_policy) {}

bool LocalProcessMemory::readable(ProcessAddress address, std::size_t size) const noexcept {
    return range_has_access(address, size, false);
}

bool LocalProcessMemory::read(
    ProcessAddress address,
    void* destination,
    std::size_t size) const noexcept {
    if (size == 0) {
        return true;
    }
    if (destination == nullptr || !range_has_access(address, size, false)) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        std::memcpy(
            destination,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
            size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(
        destination,
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
        size);
#endif
    return true;
}

bool LocalProcessMemory::write(
    ProcessAddress address,
    const void* source,
    std::size_t size) noexcept {
    if (write_policy_ != WritePolicy::permit_writable_pages) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (source == nullptr || !range_has_access(address, size, true)) {
        return false;
    }

    void* const destination =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, source, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, size);
#endif
    return FlushInstructionCache(GetCurrentProcess(), destination, size) != FALSE;
}

bool LocalProcessMemory::range_has_access(
    ProcessAddress address,
    std::size_t size,
    bool require_write) noexcept {
    if (size == 0) {
        return true;
    }
    if (address == 0) {
        return false;
    }

    constexpr std::uint64_t kAddressSpaceSize =
        static_cast<std::uint64_t>(std::numeric_limits<ProcessAddress>::max()) + 1ULL;
    const std::uint64_t first = address;
    const std::uint64_t end = first + static_cast<std::uint64_t>(size);
    if (end <= first || end > kAddressSpaceSize) {
        return false;
    }

    std::uint64_t cursor = first;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(cursor)),
                &information,
                sizeof(information)) != sizeof(information)) {
            return false;
        }
        if (information.State != MEM_COMMIT) {
            return false;
        }
        const bool allowed = require_write
            ? is_writable_protection(information.Protect)
            : is_readable_protection(information.Protect);
        if (!allowed) {
            return false;
        }

        const std::uint64_t region_begin =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        const std::uint64_t region_end = region_begin + information.RegionSize;
        if (cursor < region_begin || region_end <= cursor) {
            return false;
        }
        cursor = region_end < end ? region_end : end;
    }
    return true;
}

}  // namespace opennova::retail_hook::windows
