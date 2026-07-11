#pragma once

#include <opennova/retail_hook/memory.h>

#include <cstddef>
#include <cstdint>

namespace opennova::retail_hook::windows {

// The injected hook constructs this adapter with the default read-only policy.
// permit_writable_pages never changes protection; it only permits ranges which
// VirtualQuery already reports writable.
enum class WritePolicy : std::uint8_t {
    deny,
    permit_writable_pages,
};

class LocalProcessMemory final : public IMemory {
public:
    explicit LocalProcessMemory(WritePolicy write_policy = WritePolicy::deny) noexcept;

    [[nodiscard]] bool readable(ProcessAddress address, std::size_t size) const noexcept override;
    [[nodiscard]] bool read(ProcessAddress address, void* destination, std::size_t size) const noexcept override;
    [[nodiscard]] bool write(ProcessAddress address, const void* source, std::size_t size) noexcept override;

private:
    [[nodiscard]] static bool range_has_access(
        ProcessAddress address,
        std::size_t size,
        bool require_write) noexcept;

    WritePolicy write_policy_;
};

}  // namespace opennova::retail_hook::windows
