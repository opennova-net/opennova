#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace opennova::retail {

// An address in a 32-bit retail process. Keeping the representation independent
// of the host pointer width lets layout checks run in both 32-bit and 64-bit
// builds. Resolving an Address32 is the responsibility of a memory adapter.
template <typename T = void>
struct Address32 {
    std::uint32_t raw_value;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return raw_value;
    }

    [[nodiscard]] constexpr bool is_null() const noexcept {
        return raw_value == 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return !is_null();
    }
};

template <typename T, typename U>
[[nodiscard]] constexpr bool operator==(Address32<T> lhs, Address32<U> rhs) noexcept {
    return lhs.raw_value == rhs.raw_value;
}

template <typename T, typename U>
[[nodiscard]] constexpr bool operator!=(Address32<T> lhs, Address32<U> rhs) noexcept {
    return !(lhs == rhs);
}

static_assert(sizeof(Address32<void>) == 0x4);
static_assert(alignof(Address32<void>) == alignof(std::uint32_t));
static_assert(offsetof(Address32<void>, raw_value) == 0x0);
static_assert(std::is_standard_layout_v<Address32<void>>);
static_assert(std::is_trivially_copyable_v<Address32<void>>);

}  // namespace opennova::retail
