#pragma once

#include <cstddef>
#include <vector>

#include <opennova/retail_hook/memory.h>

namespace opennova::retail_hook {

// Contiguous memory adapter for deterministic fixtures and offline captures.
// It intentionally crosses the same IMemory seam as the injected hook.
class BufferMemory final : public IMemory {
public:
    BufferMemory(ProcessAddress base_address, std::size_t size);
    BufferMemory(ProcessAddress base_address, std::vector<std::byte> bytes);

    [[nodiscard]] ProcessAddress base_address() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept;
    [[nodiscard]] std::vector<std::byte>& bytes() noexcept;

    [[nodiscard]] bool readable(ProcessAddress address,
                                std::size_t size) const noexcept override;
    [[nodiscard]] bool read(ProcessAddress address,
                            void* destination,
                            std::size_t size) const noexcept override;
    [[nodiscard]] bool write(ProcessAddress address,
                             const void* source,
                             std::size_t size) noexcept override;

private:
    [[nodiscard]] bool offset_for(ProcessAddress address,
                                  std::size_t size,
                                  std::size_t& offset) const noexcept;

    ProcessAddress base_address_{};
    std::vector<std::byte> bytes_{};
};

}  // namespace opennova::retail_hook
