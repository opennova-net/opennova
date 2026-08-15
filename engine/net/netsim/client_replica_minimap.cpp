// Client-retained map-overlay banks: the 0x40 capture-zone/overlay batches,
// the 0x6B linked pulse markers, and their per-tick aging.
// [orig: sub_425A54 (0x40) -> MapOverlay_DecodeOverlayEntries @0x5BEBB0 ->
//  MapOverlay_UpdateOrCreateSlot @0x5BEA60 / sub_5BE970 @0x5BE970;
//  NapiNPClientMsg_0x06B @0x425520 -> update_minimap_overlay_entity @0x5BEC10;
//  update_map_overlay_timers @0x5BFCE0]

#include "netsim/client_replica_pipeline.h"

#include <npwire/ingame_decode.h>
#include <world/entity.h>          // retail_pool_capacity (the handle gates)
#include <world/minimap_overlay.h> // minimap_team_color (the ONE index home)

#include <unordered_map>

namespace opennova::netsim {

namespace {

// The 32-entry overlay color table, byte-witnessed.
// [orig: g_minimap_overlay_color_table @0x840A10]
constexpr uint32_t kOverlayColorTable[32] = {
	0xFF707070u, 0xFF204080u, 0xFF204080u, 0xFF204080u,
	0xFF802020u, 0xFF802020u, 0xFF808000u, 0xFF008000u,
	0xFF907000u, 0xFF802020u, 0xFF304080u, 0xFF208020u,
	0xFF208020u, 0xFF800080u, 0x7FFFFFFFu, 0x7F7F7F7Fu,
	0xFFFFFFFFu, 0xFF000000u, 0xFF208020u, 0xFF304080u,
	0xFFFFFF00u, 0xFF802020u, 0xFFFF027Fu, 0xFF7F4000u,
	0xFF3F3F3Fu, 0x00000000u, 0x007D8D90u, 0x007D8D80u,
	0x007D8D74u, 0x007D8D68u, 0x007D8D5Cu, 0x007D8D50u,
};

// Index < 0x20 reads the table directly; 33..42 alias onto entries 16..25
// (clamped); an alpha-0 entry rejects the marker outright.
// [orig: MapOverlay_UpdateOrCreateSlot @0x5beb16..0x5beb3e]
bool minimap_color(uint8_t index, uint32_t &out) {
	uint32_t argb;
	if (index < 0x20u) {
		argb = kOverlayColorTable[index];
	} else {
		uint8_t alias = static_cast<uint8_t>(index - 33u);
		if (alias > 9u) alias = 9u;
		argb = kOverlayColorTable[16u + alias];
	}
	if ((argb & 0xFF000000u) == 0) return false;
	out = argb;
	return true;
}

template <typename Bank>
ClientMinimapOverlaySlot *find_overlay(Bank &bank, uint16_t handle) {
	for (ClientMinimapOverlaySlot &slot : bank) {
		if (slot.active && slot.handle == handle) return &slot;
	}
	return nullptr;
}

// A slot is reusable when unclaimed or expired (special-bank expiry floors
// the lifetime at zero without clearing the handle).
// [orig: sub_5BE970 @0x5be9b0 — free = handle == 0xFFFF || lifetime == 0]
template <typename Bank>
ClientMinimapOverlaySlot *allocate_overlay(Bank &bank) {
	for (ClientMinimapOverlaySlot &slot : bank) {
		if (!slot.active || slot.remaining_ticks == 0) {
			slot = ClientMinimapOverlaySlot{};
			return &slot;
		}
	}
	return nullptr;
}

} // namespace

void ClientReplicaPipeline::apply_capture_zone_overlay(
		const std::vector<uint8_t> &body) {
	CaptureZoneOverlayBatch batch;
	if (!decode_capture_zone_overlay(body.data(), body.size(), batch)) {
		++malformed_bodies_;
		return;
	}
	bool changed = false;
	for (const CaptureZoneOverlay &entry : batch.entries) {
		// Retail gates BOTH arms — refresh and the 0x20 clear — on the same
		// validity chain before touching any slot: a real handle, a live pool,
		// a slot index inside that pool's capacity, and a table-valid color
		// byte. Any pool 0..4 handle passing those is accepted whether or not
		// an entity currently lives there.
		// [orig: @0x5beac0 handle != 0xFFFF && pool < 5; @0x5beade
		//  (handle & 0xFFF) < g_pool_list[pool].capacity; color resolve
		//  @0x5beb16..0x5beb3e ahead of the flags-0x20 branch @0x5beb4b]
		if (entry.handle == 0xFFFF || (entry.handle & 0xF000u) >= 0x5000u)
			continue;
		if (static_cast<size_t>(entry.handle & 0xFFFu) >=
				world::retail_pool_capacity(entry.handle >> 12))
			continue;
		uint32_t argb = 0;
		if (!minimap_color(entry.icon_color, argb)) continue;

		// The slot search is the witnessed walk: the special bank first, then
		// the contiguous transient -> persistent sweep. The FIRST match wins.
		// [orig: the 0x1F8 special walk @0x5bea68, then the 0x488-slot
		//  contiguous walk from the transient base @0x5bea83]
		ClientMinimapOverlaySlot *slot =
				find_overlay(state_.minimap.special, entry.handle);
		if (slot == nullptr)
			slot = find_overlay(state_.minimap.transient, entry.handle);
		if (slot == nullptr)
			slot = find_overlay(state_.minimap.persistent, entry.handle);

		if ((entry.flags & kZoneOverlayFlagClearSlot) != 0) {
			// The clear arm writes ONLY lifetime 0 + handle -1 on the found
			// slot — pose/color/icon fields SURVIVE — and never touches the
			// 0x6B link table: a surviving link re-arms the slot's lifetime
			// on the next timer tick and the marker keeps drawing (the
			// special draw walk gates on lifetime alone, no handle test).
			// A clear for an absent handle allocates nothing.
			// [orig: @0x5beb4b..0x5beb56 lifetime 0 + handle -1; the special
			//  draw gate @0x5be794 reads slot+24 only; sub_5BE970 @0x5be978
			//  returns 0 for flags & 0x20]
			if (slot != nullptr) {
				slot->remaining_ticks = 0;
				slot->handle = 0xFFFF;
				changed = true;
			}
			continue;
		}
		// A found slot refreshes IN PLACE wherever it lives — the bank (and
		// with it the aging class) is fixed at first allocation; a refresh
		// never migrates or cross-clears banks. Only a miss reaches the
		// flags-routed allocator: 0x40 special, 0x10 persistent, else
		// transient. [orig: the found path @0x5beb60..0x5beb7f writes
		//  pos/color/flags/param/source/lifetime only; sub_5BE970
		//  @0x5be97d..0x5be99f routes the fresh allocation]
		if (slot == nullptr) {
			if ((entry.flags & 0x40u) != 0)
				slot = allocate_overlay(state_.minimap.special);
			else if ((entry.flags & kZoneOverlayFlagPersistent) != 0)
				slot = allocate_overlay(state_.minimap.persistent);
			else
				slot = allocate_overlay(state_.minimap.transient);
		}
		if (slot == nullptr) continue; // bank full drops, no eviction [orig: @0x5be9ba]
		const ClientEntityState *entity = state_.find(entry.handle);
		slot->active = true;
		slot->handle = entry.handle;
		slot->param = entry.param;
		slot->icon_color = entry.icon_color;
		slot->flags = entry.flags;
		slot->source = entry.source;
		slot->argb = argb;
		slot->x = entity != nullptr ? entity->x : 0;
		slot->y = entity != nullptr ? entity->y : 0;
		slot->z = entity != nullptr ? entity->z : 0;
		slot->heading_bam = entity != nullptr ? entity->heading_bam : 0;
		slot->entity_known = entity != nullptr;
		slot->remaining_ticks = kMinimapOverlayLifetimeTicks;
		changed = true;
	}
	if (changed) {
		++state_.minimap.revision;
		state_.mark_changed();
	}
}

void ClientReplicaPipeline::apply_minimap_overlay_batch(
		const std::vector<uint8_t> &body) {
	MinimapOverlayBatch batch;
	if (!decode_minimap_overlay_batch(body.data(), body.size(), batch)) {
		++malformed_bodies_;
		return;
	}
	bool changed = false;
	for (const MinimapOverlayBatch::Entry &entry : batch.entries) {
		// The handler requires only a RANGE-VALID pool handle — retail
		// resolves the fixed pool-slot pointer and reads its team byte
		// whether or not an entity has been decoded there (an untouched
		// slot reads team 0 -> neutral). Records must not drop just because
		// the entity is not in this view yet.
		// [orig: @0x425573..0x42559d — handle != 0xFFFF, pool < 5,
		//  index < capacity; update_minimap_overlay_entity @0x5becb8 reads
		//  entity+354 unconditionally]
		if (entry.handle == 0xFFFF || (entry.handle & 0xF000u) >= 0x5000u)
			continue;
		if (static_cast<size_t>(entry.handle & 0xFFFu) >=
				world::retail_pool_capacity(entry.handle >> 12))
			continue;
		const ClientEntityState *entity = state_.find(entry.handle);
		ClientMinimapLinkedSlot *linked = nullptr;
		for (ClientMinimapLinkedSlot &candidate : state_.minimap.linked) {
			if (candidate.active && candidate.handle == entry.handle) {
				linked = &candidate;
				break;
			}
			if (!candidate.active && linked == nullptr) linked = &candidate;
		}
		if (linked == nullptr) continue; // 251-link table full [orig: @0x5bec52]
		const bool fresh_link = !linked->active;
		linked->active = true;
		linked->handle = entry.handle;
		// Wire lifetime is seconds; x62 to ticks. [orig: @0x4255c9..0x4255d6]
		linked->remaining_ticks =
				static_cast<uint32_t>(entry.lifetime_s) * 62u;

		// The link's STORED SLOT is the only key retail consults: an existing
		// link updates its own slot in place; a fresh link ALWAYS allocates a
		// new special slot, so a coexisting 0x40 special badge for the same
		// handle keeps its own slot and both markers draw.
		// [orig: overlay_obj = link[6] @0x5bece4 — in-place update
		//  @0x5bed63..0x5bed80; fresh-link alloc sub_5BE970 @0x5bed39]
		ClientMinimapOverlaySlot *slot = nullptr;
		if (!fresh_link && linked->slot_index >= 0 &&
				static_cast<size_t>(linked->slot_index) <
						state_.minimap.special.size()) {
			slot = &state_.minimap.special[
					static_cast<size_t>(linked->slot_index)];
		}
		bool fresh_slot = false;
		if (slot == nullptr) {
			slot = allocate_overlay(state_.minimap.special);
			linked->slot_index = slot != nullptr
					? static_cast<int16_t>(slot - state_.minimap.special.data())
					: static_cast<int16_t>(-1);
			fresh_slot = slot != nullptr;
		}
		if (slot == nullptr) continue; // special bank full [orig: @0x5be9ba]
		// Icon: type 3 -> 24, everything else (incl. type 1) -> 253.
		// [orig: @0x5bec59..0x5bec7e]
		const uint8_t icon = entry.type == 3 ? 24u : 253u;
		// Team color from the slot's team byte through the classifier's index
		// mapping (world::minimap_team_color — 1 -> table[10], 2 -> table[9],
		// else, including a not-yet-decoded slot, table[12] neutral); applied
		// only when nonzero. [orig: @0x5becb8..0x5bece4, @0x5bed71]
		const uint8_t team = entity != nullptr ? entity->team : 0;
		const uint8_t color_index = world::minimap_team_color(team);
		uint32_t argb = fresh_slot ? 0xFFFFFFFFu : slot->argb;
		minimap_color(color_index, argb);
		if (fresh_slot) {
			// Only the fresh allocation stamps identity; the in-place update
			// writes lifetime/icon/flags/color/pose alone (an aliased slot —
			// the allocator reusing an expired special slot a live link still
			// points at — keeps the other marker's identity, as retail does).
			// [orig: sub_5BE970 writes the handle; @0x5bed63.. writes none]
			slot->active = true;
			slot->handle = entry.handle;
			slot->source = 0;
		}
		slot->param = icon;
		slot->icon_color = color_index;
		slot->flags = 0xC4u; // [orig: @0x5bed2b / @0x5bed6d]
		slot->argb = argb;
		// Marker pose comes from the WIRE (whole units -> 16.16); the height
		// byte is the map ring radius. [orig: @0x42559f..0x4255bc, slot+28
		//  @0x5becaf..0x5becb2]
		slot->x = static_cast<int32_t>(entry.x) << 16;
		slot->y = static_cast<int32_t>(entry.y) << 16;
		slot->z = static_cast<int32_t>(entry.height) << 16;
		slot->heading_bam = entity != nullptr ? entity->heading_bam : 0;
		slot->entity_known = entity != nullptr;
		slot->remaining_ticks = kMinimapOverlayLifetimeTicks;
		changed = true;
	}
	if (changed) {
		++state_.minimap.revision;
		state_.mark_changed();
	}
}

void ClientReplicaPipeline::tick_minimap_overlays() {
	bool changed = false;
	// Transient bank ages and clears on expiry. [orig: @0x5bfce4..0x5bfd0e]
	for (ClientMinimapOverlaySlot &slot : state_.minimap.transient) {
		if (!slot.active) continue;
		if (slot.remaining_ticks > 0) --slot.remaining_ticks;
		if (slot.remaining_ticks == 0) {
			slot = ClientMinimapOverlaySlot{};
			changed = true;
		}
	}
	// Special bank ages but floors at zero keeping the handle claimed until
	// reuse. [orig: @0x5bfd10..0x5bfd38]
	for (ClientMinimapOverlaySlot &slot : state_.minimap.special) {
		if (!slot.active || slot.remaining_ticks == 0) continue;
		--slot.remaining_ticks;
		if (slot.remaining_ticks == 0) changed = true;
	}
	// The persistent bank is not aged. [orig: timers skip slot_data]
	// Links live purely on their own lifetime — retail's timer walk never
	// consults the entity — re-arming THEIR STORED SLOT's lifetime each tick
	// (through the link's slot pointer, never a handle search) and freeing
	// that slot when they lapse. [orig: @0x5bfd3a..0x5bfe21 —
	//  slot+24 = 1984 while linked @0x5bfd61; on expiry slot flags |= 0x20,
	//  lifetime 0, handle -1, link zeroed @0x5bfddb..0x5bfe15]
	for (ClientMinimapLinkedSlot &linked : state_.minimap.linked) {
		if (!linked.active) continue;
		if (linked.remaining_ticks > 0) --linked.remaining_ticks;
		ClientMinimapOverlaySlot *slot = linked.slot_index >= 0 &&
						static_cast<size_t>(linked.slot_index) <
								state_.minimap.special.size()
				? &state_.minimap.special[
						static_cast<size_t>(linked.slot_index)]
				: nullptr;
		if (linked.remaining_ticks == 0) {
			// The lapse writes flags |= 0x20, lifetime 0, handle -1 through
			// the stored pointer — the other slot fields survive here too.
			// [orig: @0x5bfddb..0x5bfe15]
			if (slot != nullptr && slot->active) {
				slot->flags |= 0x20u;
				slot->remaining_ticks = 0;
				slot->handle = 0xFFFF;
			}
			linked = ClientMinimapLinkedSlot{};
			changed = true;
			continue;
		}
		if (slot != nullptr && slot->active &&
				slot->remaining_ticks != kMinimapOverlayLifetimeTicks) {
			// A 0x40-cleared slot crossing 0 -> 1984 here is the witnessed
			// link RESURRECT — a visibility change, so bump the revision.
			if (slot->remaining_ticks == 0) changed = true;
			slot->remaining_ticks = kMinimapOverlayLifetimeTicks;
		}
	}
	// Regular markers render from the live entity; refresh the decoded pose
	// and the known/alive gate each tick (retail reads the pool slot at draw
	// time — O(1) pool arithmetic there, so the per-slot lookup here rides
	// a handle index built once per tick instead of a per-slot linear scan).
	// [orig: render_minimap_slot_blip @0x5be4ac..0x5be515]
	std::unordered_map<uint16_t, const ClientEntityState *> entity_index;
	entity_index.reserve(state_.entities.size());
	for (const ClientEntityState &entity : state_.entities) {
		entity_index.emplace(entity.handle, &entity);
	}
	auto refresh_live = [&](auto &bank) {
		for (ClientMinimapOverlaySlot &slot : bank) {
			if (!slot.active || (slot.flags & 0x40u) != 0) continue;
			const auto entity_it = entity_index.find(slot.handle);
			const ClientEntityState *entity =
					entity_it != entity_index.end() ? entity_it->second
													: nullptr;
			const bool known = entity != nullptr;
			if (known != slot.entity_known) {
				slot.entity_known = known;
				changed = true;
			}
			if (!known) continue;
			if (slot.x != entity->x || slot.y != entity->y ||
					slot.z != entity->z ||
					slot.heading_bam != entity->heading_bam) {
				slot.x = entity->x;
				slot.y = entity->y;
				slot.z = entity->z;
				slot.heading_bam = entity->heading_bam;
				changed = true;
			}
		}
	};
	refresh_live(state_.minimap.transient);
	refresh_live(state_.minimap.persistent);
	if (changed) {
		++state_.minimap.revision;
		state_.mark_changed();
	}
}

bool client_minimap_grid_origin(const ClientState &state, int32_t &out_x_q16,
		int32_t &out_y_q16) {
	// [orig: HUD_InitOverlaySystem @0x5a4999 — first pool-3 entity of
	//  type 2043]
	for (const ClientEntityState &entity : state.entities) {
		if ((entity.handle & 0xF000u) != 0x3000u || entity.type_id != 2043) {
			continue;
		}
		out_x_q16 = entity.x;
		out_y_q16 = entity.y;
		return true;
	}
	return false;
}

uint32_t minimap_team_argb(uint8_t team) {
	// The classifier's index mapping over the wire color table.
	// [orig: team -> index @0x5becb8..0x5bece4; table @0x840A10]
	uint32_t argb = 0xFFFFFFFFu;
	minimap_color(world::minimap_team_color(team), argb);
	return argb;
}

} // namespace opennova::netsim
