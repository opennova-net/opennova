#include <opennova/retail_hook/buffer_memory.h>

#include <cstring>
#include <utility>

namespace opennova::retail_hook {

BufferMemory::BufferMemory(ProcessAddress base_address, std::size_t size)
    : base_address_(base_address), bytes_(size) {}

BufferMemory::BufferMemory(ProcessAddress base_address, std::vector<std::byte> bytes)
    : base_address_(base_address), bytes_(std::move(bytes)) {}

ProcessAddress BufferMemory::base_address() const noexcept {
    return base_address_;
}

std::size_t BufferMemory::size() const noexcept {
    return bytes_.size();
}

const std::vector<std::byte>& BufferMemory::bytes() const noexcept {
    return bytes_;
}

std::vector<std::byte>& BufferMemory::bytes() noexcept {
    return bytes_;
}

bool BufferMemory::offset_for(ProcessAddress address,
                              std::size_t size,
                              std::size_t& offset) const noexcept {
    if (address < base_address_) {
        return false;
    }

    const auto distance = static_cast<std::uint64_t>(address) -
                          static_cast<std::uint64_t>(base_address_);
    if (distance > bytes_.size()) {
        return false;
    }

    offset = static_cast<std::size_t>(distance);
    return size <= bytes_.size() - offset;
}

bool BufferMemory::readable(ProcessAddress address, std::size_t size) const noexcept {
    std::size_t ignored{};
    return offset_for(address, size, ignored);
}

bool BufferMemory::read(ProcessAddress address,
                        void* destination,
                        std::size_t size) const noexcept {
    if (size != 0 && destination == nullptr) {
        return false;
    }

    std::size_t offset{};
    if (!offset_for(address, size, offset)) {
        return false;
    }

    if (size != 0) {
        std::memcpy(destination, bytes_.data() + offset, size);
    }
    return true;
}

bool BufferMemory::write(ProcessAddress address,
                         const void* source,
                         std::size_t size) noexcept {
    if (size != 0 && source == nullptr) {
        return false;
    }

    std::size_t offset{};
    if (!offset_for(address, size, offset)) {
        return false;
    }

    if (size != 0) {
        std::memcpy(bytes_.data() + offset, source, size);
    }
    return true;
}

}  // namespace opennova::retail_hook
