#include "foliage/dispatcher.h"

// Engine: jodemo.exe sub_5C1940@0x5C1940
// [orig: sub_5C1940 @ 0x5C1940 (jodemo); the retail foliage dispatch is the sibling of generate_foliage_instances_0 @ 0x600197, see docs/foliage/foliage-re.md]
// docs/engine_spec_foliage.md 4.3

namespace opennova::foliage {

Dispatcher::Dispatcher() { reset(); }

void Dispatcher::reset() noexcept {
	for (auto &slot : slots_) {
		slot = LRUEntry{};
	}
}

int Dispatcher::lru_occupancy() const noexcept {
	int n = 0;
	for (const auto &slot : slots_) {
		if (slot.occupied) {
			++n;
		}
	}
	return n;
}

bool Dispatcher::is_staggered_bake_frame(int32_t frame_counter, int slot_index) noexcept {
	// Engine @ 0x5c1a7f: `((dword_154A6B0 + 2 * slot_index) & 7) == 0`.
	// Creates a per-slot stagger offset by 2 frames between slots.
	return ((frame_counter + 2 * slot_index) & DISPATCHER_STAGGER_MASK) == 0;
}

namespace {

// Return the LRU slot index whose `last_touched` is oldest (smallest), or -1 if the
// LRU is empty. Mirrors the engine's eviction loop @ 0x5c1ac3..0x5c1b17 which walks
// 128 slots in groups of 4 tracking max(now - last_touched).
int find_oldest_slot(const std::array<LRUEntry, DISPATCHER_LRU_SLOTS> &slots,
                     int32_t now) noexcept {
	int best_idx = -1;
	int32_t best_age = -1;
	for (int i = 0; i < DISPATCHER_LRU_SLOTS; ++i) {
		if (!slots[i].occupied) {
			return i;  // empty slot beats any occupied one
		}
		const int32_t age = now - slots[i].last_touched;
		if (age > best_age) {
			best_age = age;
			best_idx = i;
		}
	}
	return best_idx;
}

} // namespace

void Dispatcher::dispatch(int slot_index,
                          Fixed16_16 view_center_x,
                          Fixed16_16 view_center_z,
                          float view_camera_z,
                          int32_t view_radius,
                          int32_t frame_counter,
                          const PlacementConfig &config,
                          const PlacementSamplers &samplers,
                          std::vector<ZSortInstance> &out_zsort) noexcept {
	if (slot_index < 0 || slot_index >= FOLIAGE_MAX_DEFS) {
		return;
	}

	// Near-plane reject. Engine @ 0x5c19a3: `if (v23 >= 38.0)`, else entire dispatch
	// is skipped - no LRU update, nothing renders for this entity this frame.
	if (view_camera_z < DISPATCHER_NEAR_Z) {
		return;
	}

	const bool should_rebake = is_staggered_bake_frame(frame_counter, slot_index);

	// Four quadrants: +/-8u X/Z offsets. Engine @ 0x5c19cf..0x5c1a1d.
	for (int quad = 0; quad < DISPATCHER_QUADRANTS; ++quad) {
		const Fixed16_16 x_off = (quad & 1) ? -DISPATCHER_QUADRANT_OFFSET : DISPATCHER_QUADRANT_OFFSET;
		const Fixed16_16 z_off = (quad & 2) ? -DISPATCHER_QUADRANT_OFFSET : DISPATCHER_QUADRANT_OFFSET;

		// Cell-align. Engine applies: (center + offset) & 0xFFF00000, with z getting
		// an extra +0x100000 after the mask (to bias the cell boundary).
		const Fixed16_16 cell_x = (view_center_x + x_off) & static_cast<Fixed16_16>(DISPATCHER_CELL_MASK);
		const Fixed16_16 cell_z_pre = view_center_z + z_off;
		const Fixed16_16 cell_z =
		    (cell_z_pre & static_cast<Fixed16_16>(DISPATCHER_CELL_MASK))
		    + static_cast<Fixed16_16>(DISPATCHER_CELL_SIZE);

		const uint32_t key = pack_cell_key(cell_x, cell_z);

		// Linear LRU scan for matching key.
		LRUEntry *entry = nullptr;
		for (int i = 0; i < DISPATCHER_LRU_SLOTS; ++i) {
			if (slots_[i].occupied && slots_[i].cell_key == key) {
				entry = &slots_[i];
				entry->last_touched = frame_counter;
				if (should_rebake) {
					entry->cached = place_cell(slot_index, key, view_center_x, view_center_z,
					                           view_radius, config, samplers);
				}
				break;
			}
		}

		// Miss: evict and populate.
		if (entry == nullptr) {
			const int victim = find_oldest_slot(slots_, frame_counter);
			if (victim < 0) {
				continue;  // should never happen with 128 slots, but matches engine's LABEL_24
			}
			entry = &slots_[victim];
			entry->cell_key = key;
			entry->last_touched = frame_counter;
			entry->occupied = true;
			entry->cached = place_cell(slot_index, key, view_center_x, view_center_z,
			                           view_radius, config, samplers);
		}

		// Emit placed instances to the caller's Z-sort list. Engine @ 0x5c1b71 calls
		// sub_5C1790 per cell with count > 0; our port defers sort + render to the
		// caller so unit tests can inspect the flat list directly.
		for (int i = 0; i < entry->cached.count; ++i) {
			ZSortInstance z{};
			z.instance = entry->cached.instances[i];
			z.slot_index = slot_index;
			z.camera_z = view_camera_z;
			out_zsort.push_back(z);
		}
	}
}

} // namespace opennova::foliage
