#pragma once

#include <cmath>
#include <cstdint>

namespace godot {

// The one child of the MissionObjects container that holds every static population (the per-bin
// batches, the blended global batches, the shadow twins), so the container's own children stay the
// placed entity models.
inline constexpr const char *kStaticPopulationsName = "StaticPopulations";
// The 512-unit bins stay now that populations are dense per level (measured 2026-08-30 against one
// population per graphic x policy x level x submesh on 00TRa / CP01: the bins draw fewer primitives
// through their per-bin frustum cull for the same or a slightly lower frame time; the numbers live
// in docs/env/env-tod-re.md, the static-batching note).
inline constexpr float kStaticBatchBinSize = 512.0f;

inline int static_batch_bin_coord(float p_world) noexcept {
	return static_cast<int>(std::floor(p_world / kStaticBatchBinSize));
}

inline uint64_t static_batch_bin_key(int p_x, int p_z) noexcept {
	return (static_cast<uint64_t>(static_cast<uint32_t>(p_x)) << 32) | static_cast<uint32_t>(p_z);
}

// Composite identity keys shared by the placer's placement walk and its
// static-source read-back seams (two translation units of one class).
inline uint64_t entity_identity_key(int p_kind, int p_index) noexcept {
	return (static_cast<uint64_t>(static_cast<uint32_t>(p_kind)) << 32) |
			static_cast<uint32_t>(p_index);
}

inline uint64_t static_light_draw_key(int p_instance, int p_robj_index) noexcept {
	return (static_cast<uint64_t>(static_cast<uint32_t>(p_instance)) << 32) |
			static_cast<uint32_t>(p_robj_index);
}

} // namespace godot
