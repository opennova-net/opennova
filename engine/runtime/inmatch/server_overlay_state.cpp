#include <runtime/inmatch/server_overlay_state.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <base/gameprofile/game_type.h>
#include <base/io/le.h>                         // append_u16_le
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/server_designations.h> // Server_SendDesignationsToPlayer
#include <runtime/world/minimap_overlay.h>       // classify_minimap_overlay
#include <runtime/world/zone_capture.h>          // the recipient's capture bits

namespace opennova::inmatch {

namespace {

// The staging ring a visit queues its 6-byte entries into (shared by every
// visit, its count cleared as each visit opens). A flush sends the staged
// entries as one unreliable 0x40 to the slot it is handed (send mask 0x20)
// and clears the count ahead of the send; an empty ring sends nothing.
// [orig: the count byte_C86FFC, cleared @0x518012; the entries @0xC71A78;
//  Server_SendPendingOverlayState @0x50FE20 (the empty test @0x50FE27, the
//  clear @0x50FE47, mask 0x20 @0x50FE4E, SendFiltered(0x40, 0, 1) @0x50FE94);
//  NapiNPServer_SendFiltered's mask-0x20 target gates @0x4C8A06..0x4C8A38]
struct OverlayRing {
	std::vector<world::MinimapOverlayClassification> entries;

	void flush(NapiNPConnection &slot) {
		if (entries.empty()) return;
		std::vector<uint8_t> body;
		body.reserve(1 + entries.size() * 6);
		body.push_back(static_cast<uint8_t>(entries.size()));
		for (const world::MinimapOverlayClassification &entry : entries) {
			opennova::io::append_u16_le(body, entry.handle);
			body.push_back(entry.icon);
			body.push_back(entry.color);
			body.push_back(entry.flags);
			body.push_back(entry.source);
		}
		entries.clear();
		if (slot.link.transport == nullptr) return; // the slot's connection node
		slot.link.transport->host_send(
				s2c::CAPTURE_ZONE_STATE, std::move(body), /*reliable=*/false);
	}

	// A full ring flushes to the slot the call is handed BEFORE the handle
	// test; a 0xFFFF handle then queues nothing and fails the call.
	// [orig: Server_QueueOverlayStateEntry @0x50FEA0 (the full test @0x50FEA0,
	//  the flush @0x50FEAE, the handle test @0x50FEC3, the store
	//  @0x50FEDD..0x50FEFF)]
	bool queue(NapiNPConnection &slot, const world::MinimapOverlayClassification &entry) {
		if (entries.size() == 16) flush(slot);
		if (entry.handle == world::EntityHandle::kInvalid) return false;
		entries.push_back(entry);
		return true;
	}
};

// One recipient's visit. Every entry goes through the staging ring, handed
// the recipient, and the visit ends by flushing what is left to it; the
// carrier pass hands the ring the WALKED slot instead, so a ring that fills
// there flushes the recipient's staged entries to that slot. A failed queue
// returns at once: no final flush, so the staged tail is dropped (the next
// visit clears the count). Only the carrier pass can fail, on a slot whose
// entity is in no pool; a pool row always has its handle.
// [orig: Server_BuildOverlayStateForPlayer @0x517FC0 (the queue calls
//  @0x51812A / @0x51829E / @0x5183EE / @0x5185A5 / @0x51871B / @0x5187BE, the
//  failed queue's return loc_518144, the final flush @0x5187EE)]
void build_overlay_state_for_player(NapiNPServerCtx &ctx, world::World &world,
		NapiNPConnection &conn) {
	SessionReplyState &reply = conn.reply;
	// Each visit opens with the recipient team's designations (S2C 0x6B)
	// [orig: Server_BuildOverlayStateForPlayer @0x518002].
	Server_SendDesignationsToPlayer(conn, ctx.designations, world);

	OverlayRing ring;
	auto append = [&](const world::Entity &e, bool persistent) {
		world::MinimapOverlayClassification entry =
				world::classify_minimap_overlay(e, &world);
		if (!entry.visible) return;
		if (persistent) entry.flags |= 0x10;
		ring.queue(conn, entry);
	};

	if (reply.minimap_initial_scan_pending) {
		const size_t capacity = world.registry.pool_capacity(2);
		for (size_t slot = 0; slot < capacity; ++slot) {
			const world::Entity *e = world.registry.get(
					world::EntityHandle::make(2, static_cast<int>(slot)));
			if (e == nullptr || !world::minimap_overlay_entity_enabled(*e) ||
					(e->item_attrib & world::kItemAttribSpawnPoint) != 0)
				continue;
			append(*e, true);
		}
		reply.minimap_initial_scan_pending = false;
	}

	// The recipient SLOT's team byte (+0x1A0), never its body's (D-NET-636):
	// the capture bits, the vehicle filter and FlagBall's carried-flag filter
	// below read it [orig: `movzx eax, [ebx+1A0h]` @0x51820C (pool 2), `movzx
	//  ecx, [ebx+1A0h]` @0x51835C (pool 1), `movzx edx, [ecx+1A0h]` @0x5184D7
	//  (the vehicle filter), `movzx ecx, [eax+1A0h]` @0x518537 (the flag)].
	const uint8_t recipient_team = conn.assigned_team;
	// SpawnPoint rows from both static and actor pools are persistent and
	// refreshed on every invocation. A numbered zone's source byte ORs the
	// recipient's capture bits: 0x80 when its team may take the zone, 0x40
	// when the other side (2 for team 1, else 1) may [orig:
	// Server_BuildOverlayStateForPlayer @0x5181ff..0x518248 (pool 2),
	// @0x51834f..0x518398 (pool 1); ZoneSlotChain_IsZoneCapturableByTeam @0x4A2450].
	for (const int pool : {2, 1}) {
		const size_t capacity = world.registry.pool_capacity(pool);
		for (size_t slot = 0; slot < capacity; ++slot) {
			const world::Entity *e = world.registry.get(
					world::EntityHandle::make(pool, static_cast<int>(slot)));
			if (e == nullptr || !world::minimap_overlay_entity_enabled(*e) ||
					(e->item_attrib & world::kItemAttribSpawnPoint) == 0)
				continue;
			world::MinimapOverlayClassification entry =
					world::classify_minimap_overlay(*e, &world);
			if (!entry.visible) continue;
			entry.flags |= 0x10;
			if (e->zone_number != 0) {
				if (world.zones.is_capturable(recipient_team, *e)) entry.source |= 0x80;
				if (world.zones.is_capturable(recipient_team == 1 ? 2 : 1, *e))
					entry.source |= 0x40;
			}
			ring.queue(conn, entry);
		}
	}
	const uint32_t gt = world.match.rules().game_type;
	const size_t pool1_capacity = world.registry.pool_capacity(1);
	for (size_t slot = reply.minimap_pool1_phase;
			slot < pool1_capacity; slot += 128) {
		const world::Entity *e = world.registry.get(
				world::EntityHandle::make(1, static_cast<int>(slot)));
		if (e == nullptr || !world::minimap_overlay_entity_enabled(*e) ||
				(e->item_attrib & world::kItemAttribSpawnPoint) != 0)
			continue;
		// In a live MP session retail suppresses occupied enemy vehicles: a
		// teamed one whose team byte, sign-extended, is not the recipient
		// slot's [orig: `movsx eax, al` @0x5184DE, the compare @0x5184E1..0x5184E3].
		if (e->item_type == 1 && e->team != 0 &&
				static_cast<int8_t>(e->team) != static_cast<int32_t>(recipient_team))
			continue;
		// A child EWEAP mounted under a non-Building parent is represented
		// by that carrier rather than as an independent map blip.
		if ((e->item_attrib & world::kItemAttribEweap) != 0) {
			const world::EntityHandle parent = e->emplacement_parent.valid()
					? e->emplacement_parent : e->mount_target;
			if (const world::Entity *carrier = world.registry.get(parent);
					carrier != nullptr && carrier->item_type != 5)
				continue;
		}
		const world::MinimapOverlayClassification entry =
				world::classify_minimap_overlay(*e, &world);
		if (!entry.visible) continue;
		// FlagBall shows a carried flag (icon 2) only to its carrier's team:
		// the occupant's team byte, sign-extended, against the recipient
		// slot's. Retail reads through the link whatever the row holds, so a
		// link left on a destroyed row reads that row's zeroed team.
		// [orig: `cmp al, 2` @0x518512, `cmp g_GameType, 10008h` @0x518516,
		//  the occupant `[esi+170h]` @0x518522..0x51852A, `movsx edx, [eax+162h]`
		//  @0x51852C, the compare @0x51853E..0x518540; Entity_Destroy's memset
		//  @0x43EA70]
		if (entry.icon == 2 && gt == game_type::kFlagBall &&
				e->primary_occupant.valid()) {
			const world::Entity *occupant = world.registry.get(e->primary_occupant);
			const int8_t occupant_team =
					occupant != nullptr ? static_cast<int8_t>(occupant->team) : int8_t{0};
			if (occupant_team != static_cast<int32_t>(recipient_team)) continue;
		}
		ring.queue(conn, entry);
	}

	// In FlagBall or Flag Me every slot (no active test) whose flag carry has
	// run past 60 seconds queues an icon-2, colour-12 entry naming its entity,
	// handed to the walked slot; a slot with no entity has no pool handle and
	// fails the queue (a connection's entity stays bound from its spawn and the
	// count only runs with one, so ours never takes that return). The walk is
	// the slot table's, in slot order.
	// [orig: @0x518755..0x518764 (the game types), @0x518766..0x518774 (the
	//  walk), `cmp [edi+15F10h], 3Ch` / `jle` @0x518776..0x51877D, the pool
	//  search @0x518781..0x5187A9, Server_QueueOverlayStateEntry(slot, handle,
	//  2, 0x0C, 0, 0) @0x5187BE, the failed queue's jnz loc_518144 @0x5187C8,
	//  the walk's end @0x5187DD]
	if (gt == game_type::kFlagBall || gt == game_type::kFlagMe) {
		std::vector<NapiNPConnection *> slots;
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) slots.push_back(&c);
		std::stable_sort(slots.begin(), slots.end(),
				[](const NapiNPConnection *a, const NapiNPConnection *b) {
					return a->reply.player_slot < b->reply.player_slot;
				});
		for (NapiNPConnection *slot : slots) {
			if (static_cast<int32_t>(slot->link.flag_carry_seconds) <= 60) continue;
			world::MinimapOverlayClassification carrier;
			carrier.handle = slot->link.owned_entity.valid()
					? slot->link.owned_entity.packed : world::EntityHandle::kInvalid;
			carrier.icon = 2;
			carrier.color = 0x0C;
			if (!ring.queue(*slot, carrier)) return;
		}
	}
	ring.flush(conn);
}

} // namespace

// Retail invokes Server_BuildOverlayStateForPlayer every 14 host ticks, then
// advances a 0..127 phase. The dynamic pool-1 loop visits phase, phase+128, ...
// rather than sweeping all actors every invocation. This produces 00TRg's five
// one-entry EWEAP packets close together and repeats them every 1792 ticks.
// [orig: Server_BuildOverlayStateForPlayer @0x517FC0 (the 14-tick cooldown, the
//  0..127 phase and the +128 stride); Entity_ClassifyForMinimap @0x50FA70 (the
//  item-type 1 / carrier-type 5 classification and the 0x10 persistent flag);
//  Server_UpdateAllActivePlayerSlots @0x518820 (the cooldown test @0x5188CB,
//  the call @0x5188D6, the phase and the cooldown re-arm @0x5188E5..0x5188FB
//  on every return)]
void emit_minimap_overlay_for_slot(NapiNPServerCtx &ctx, world::World &world,
		NapiNPConnection &conn) {
	if (!ctx.is_in_session) return;
	if ((conn.type != NapiNPConnection::kTypeServerSide &&
			conn.link.mode != replication::TransportMode::Loopback) ||
			!is_in_match(conn) || conn.link.transport == nullptr)
		return;
	SessionReplyState &reply = conn.reply;
	if (reply.minimap_overlay_cooldown != 0) {
		--reply.minimap_overlay_cooldown;
		return;
	}
	build_overlay_state_for_player(ctx, world, conn);
	reply.minimap_pool1_phase =
			static_cast<uint8_t>((reply.minimap_pool1_phase + 1u) & 0x7Fu);
	reply.minimap_overlay_cooldown = 13;
}

} // namespace opennova::inmatch
