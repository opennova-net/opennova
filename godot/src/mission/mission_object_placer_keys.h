#pragma once

#include <cstdint>

namespace godot {

// Composite identity keys shared by the placer's placement walk and its
// static-source read-back seams (two translation units of one class).
inline uint64_t entity_identity_key(int p_kind, int p_index) noexcept {
	return (static_cast<uint64_t>(static_cast<uint32_t>(p_kind)) << 32) |
			static_cast<uint32_t>(p_index);
}

} // namespace godot
