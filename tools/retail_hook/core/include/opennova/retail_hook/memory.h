#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova::retail_hook {

// Joint Operations is a 32-bit process. Keeping process addresses explicitly
// 32-bit makes the validation core behave the same in x86 production builds
// and x64 fixture tests.
using ProcessAddress = std::uint32_t;

// Seam between typed validation logic and a memory source. Production uses an
// in-process adapter; tests use BufferMemory. Reads and writes may still fail
// after a successful range check because live process mappings can change.
class IMemory {
public:
    virtual ~IMemory() = default;

    [[nodiscard]] virtual bool readable(ProcessAddress address,
                                        std::size_t size) const noexcept = 0;
    [[nodiscard]] virtual bool read(ProcessAddress address,
                                    void* destination,
                                    std::size_t size) const noexcept = 0;
    [[nodiscard]] virtual bool write(ProcessAddress address,
                                     const void* source,
                                     std::size_t size) noexcept = 0;
};

}  // namespace opennova::retail_hook
