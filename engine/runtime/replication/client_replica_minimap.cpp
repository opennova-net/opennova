// Client-retained map-overlay banks: the 0x40 capture-zone/overlay batches,
// the 0x6B linked pulse markers, and their per-tick aging.
// [orig: NapiNPClientMsg_0x040_Impl @0x425a54 (0x40) -> MapOverlay_DecodeOverlayEntries @0x5BEBB0 ->
//  MapOverlay_UpdateOrCreateSlot @0x5BEA60 / MapOverlay_AllocSlot @0x5BE970;
//  NapiNPClientMsg_0x06B @0x425520 -> Minimap_UpdateOverlayEntity @0x5BEC10;
//  MapOverlay_UpdateTimers @0x5BFCE0]

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h>
#include <runtime/world/entity.h>          // retail_pool_capacity (the handle gates)
#include <runtime/world/minimap_overlay.h> // minimap_team_color (the ONE index home)

#include <unordered_map>

namespace opennova::replication {

namespace {

// The 32-entry overlay color table, byte-witnessed.
// [orig: g_MinimapOverlayColorTable @0x840A10]
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

// Index < 0x20 reads the table directly with NO alpha test; only the 33..42
// alias branch (clamped onto entries 16..25) rejects an alpha-0 entry — a
// direct alpha-0 wire byte still creates/refreshes the slot (the draw path
// forces alpha 0xFF anyway).
// [orig: MapOverlay_UpdateOrCreateSlot — direct @0x5beb16..0x5beb1b straight,
//  alias alpha test only @0x5beb31..0x5beb3e]
bool minimap_color(uint8_t index, uint32_t &out) {
	if (index < 0x20u) {
		out = kOverlayColorTable[index];
		return true;
	}
	uint8_t alias = static_cast<uint8_t>(index - 33u);
	if (alias > 9u) alias = 9u;
	const uint32_t argb = kOverlayColorTable[16u + alias];
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
// [orig: MapOverlay_AllocSlot @0x5be9b0 — free = handle == 0xFFFF || lifetime == 0]
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
		//  (handle & 0xFFF) < g_PoolList[pool].capacity; color resolve
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
			//  draw gate @0x5be794 reads slot+24 only; MapOverlay_AllocSlot @0x5be978
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
		//  pos/color/flags/param/source/lifetime only; MapOverlay_AllocSlot
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
		//  index < capacity; Minimap_UpdateOverlayEntity @0x5becb8 reads
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
		linked->type = entry.type;
		linked->x = int32_t(entry.x) * 65536;
		linked->y = int32_t(entry.y) * 65536;
		linked->radius_q16 = int32_t(entry.height) * 65536;

		// The link's STORED SLOT is the only key retail consults: an existing
		// link updates its own slot in place; a fresh link ALWAYS allocates a
		// new special slot, so a coexisting 0x40 special badge for the same
		// handle keeps its own slot and both markers draw.
		// [orig: overlay_obj = link[6] @0x5bece4 — in-place update
		//  @0x5bed63..0x5bed80; fresh-link alloc MapOverlay_AllocSlot @0x5bed39]
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
			// [orig: MapOverlay_AllocSlot writes the handle; @0x5bed63.. writes none]
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

void ClientReplicaPipeline::age_minimap_overlays(uint32_t elapsed) {
	if (elapsed == 0) return;
	const int64_t d = static_cast<int64_t>(elapsed);
	bool changed = false;
	// Transient bank: a claimed slot ages and frees at or under zero.
	// [orig: MapOverlay_UpdateTimers @0x5bfce4..0x5bfd0e]
	for (ClientMinimapOverlaySlot &slot : state_.minimap.transient) {
		if (!slot.active) continue;
		if (static_cast<int64_t>(slot.remaining_ticks) - d <= 0) {
			slot = ClientMinimapOverlaySlot{};
			changed = true;
		} else {
			slot.remaining_ticks = static_cast<uint16_t>(slot.remaining_ticks - elapsed);
		}
	}
	// Special bank: a live lifetime ages and floors at zero, the handle kept
	// claimed until reuse. [orig: @0x5bfd10..0x5bfd38]
	for (ClientMinimapOverlaySlot &slot : state_.minimap.special) {
		if (!slot.active || slot.remaining_ticks == 0) continue;
		const int64_t left = static_cast<int64_t>(slot.remaining_ticks) - d;
		slot.remaining_ticks = static_cast<uint16_t>(left < 0 ? 0 : left);
		if (slot.remaining_ticks == 0) changed = true;
	}
	// The persistent bank is not aged. [orig: the walk has no leg for it]
	// Links live purely on their own lifetime. A live link first re-arms its
	// STORED slot (through the link's slot pointer, never a handle search):
	// lifetime 1984, a slot flagged 0x20 zeroes lifetime and handle, the link's
	// entity pointer folds back into the slot's handle (so a 0x40 flags-0x20
	// clear is undone and a later 0x40 for the handle finds this slot), and a
	// lifetime left at or under zero floors at 1; then the link ages and, at or
	// under zero, lapses: the slot's flags byte is ASSIGNED 0x20, its lifetime
	// and handle cleared, the link zeroed.
	// [orig: @0x5bfd3a..0x5bfe21 — re-arm @0x5bfd61, the 0x20 test
	//  @0x5bfd8b..0x5bfd90, the handle rewrite @0x5bfd95..0x5bfdc8, the floor
	//  @0x5bfdd2..0x5bfdd4, the age @0x5bfddf, the lapse: `mov byte [eax+3],
	//  20h` @0x5bfdf3, lifetime / handle / link @0x5bfdf7..0x5bfe15]
	for (ClientMinimapLinkedSlot &linked : state_.minimap.linked) {
		if (!linked.active || linked.remaining_ticks == 0) continue;
		ClientMinimapOverlaySlot *slot = linked.slot_index >= 0 &&
						static_cast<size_t>(linked.slot_index) <
								state_.minimap.special.size()
				? &state_.minimap.special[
						static_cast<size_t>(linked.slot_index)]
				: nullptr;
		if (slot != nullptr && slot->active) {
			const uint16_t before = slot->remaining_ticks;
			int32_t lifetime = kMinimapOverlayLifetimeTicks;
			if ((slot->flags & 0x20u) != 0) {
				lifetime = 0;
				slot->handle = 0xFFFF;
			}
			if (linked.handle != 0xFFFF) slot->handle = linked.handle;
			if (lifetime <= 0) lifetime = 1;
			slot->remaining_ticks = static_cast<uint16_t>(lifetime);
			// A 0x40-cleared slot crossing 0 -> live here is the witnessed
			// link RESURRECT — a visibility change, so bump the revision.
			if (before == 0) changed = true;
		}
		if (static_cast<int64_t>(linked.remaining_ticks) - d <= 0) {
			if (slot != nullptr && slot->active) {
				slot->flags = 0x20u;
				slot->remaining_ticks = 0;
				slot->handle = 0xFFFF;
			}
			linked = ClientMinimapLinkedSlot{};
			changed = true;
			continue;
		}
		linked.remaining_ticks -= elapsed;
	}
	if (changed) {
		++state_.minimap.revision;
		state_.mark_changed();
	}
}

void ClientReplicaPipeline::refresh_minimap_live_markers() {
	bool changed = false;
	// Regular markers render from the live entity; refresh the decoded pose
	// and the known/alive gate each tick (retail reads the pool slot at draw
	// time — O(1) pool arithmetic there, so the per-slot lookup here rides
	// a handle index built once per tick instead of a per-slot linear scan).
	// [orig: Render_MinimapSlotBlip @0x5be4ac..0x5be515]
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

} // namespace opennova::replication
