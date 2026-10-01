#include <runtime/inmatch/server_overlay_state.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <base/io/le.h>                         // append_u16_le
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/server_designations.h> // Server_SendDesignationsToPlayer
#include <runtime/world/minimap_overlay.h>       // classify_minimap_overlay
#include <runtime/world/zone_capture.h>          // the recipient's capture bits

namespace opennova::inmatch {

namespace {

void send_minimap_overlay_batches(NapiNPConnection &conn,
		const std::vector<world::MinimapOverlayClassification> &entries) {
	for (size_t first = 0; first < entries.size(); first += 16) {
		const size_t count = std::min<size_t>(16, entries.size() - first);
		std::vector<uint8_t> body;
		body.reserve(1 + count * 6);
		body.push_back(static_cast<uint8_t>(count));
		for (size_t i = 0; i < count; ++i) {
			const world::MinimapOverlayClassification &entry = entries[first + i];
			opennova::io::append_u16_le(body, entry.handle);
			body.push_back(entry.icon);
			body.push_back(entry.color);
			body.push_back(entry.flags);
			body.push_back(entry.source);
		}
		conn.link.transport->host_send(
				s2c::CAPTURE_ZONE_STATE, std::move(body), /*reliable=*/false);
	}
}

} // namespace

// Retail invokes Server_BuildOverlayStateForPlayer every 14 host ticks, then
// advances a 0..127 phase. The dynamic pool-1 loop visits phase, phase+128, ...
// rather than sweeping all actors every invocation. This produces 00TRg's five
// one-entry EWEAP packets close together and repeats them every 1792 ticks.
// [orig: Server_BuildOverlayStateForPlayer @0x517FC0 (the 14-tick cooldown, the
//  0..127 phase and the +128 stride); Entity_ClassifyForMinimap @0x50FA70 (the
//  item-type 1 / carrier-type 5 classification and the 0x10 persistent flag)]
void emit_minimap_overlay_state(NapiNPServerCtx &ctx, world::World &world) {
	if (!ctx.is_in_session) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if ((conn.type != NapiNPConnection::kTypeServerSide &&
				conn.link.mode != replication::TransportMode::Loopback) ||
				!is_in_match(conn) || conn.link.transport == nullptr)
			continue;

		SessionReplyState &reply = conn.reply;
		if (reply.minimap_overlay_cooldown != 0) {
			--reply.minimap_overlay_cooldown;
			continue;
		}
		// Each visit opens with the recipient team's designations (S2C 0x6B)
		// [orig: Server_BuildOverlayStateForPlayer @0x518002].
		Server_SendDesignationsToPlayer(conn, ctx.designations, world);

		std::vector<world::MinimapOverlayClassification> entries;
		auto append = [&](const world::Entity &e, bool persistent) {
			world::MinimapOverlayClassification entry =
					world::classify_minimap_overlay(e, &world);
			if (!entry.visible) return;
			if (persistent) entry.flags |= 0x10;
			entries.push_back(entry);
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

		uint8_t recipient_team = 0; // retail's player slot byte +416
		if (const world::Entity *recipient = world.registry.get(conn.link.owned_entity))
			recipient_team = recipient->team;
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
				entries.push_back(entry);
			}
		}
		const size_t pool1_capacity = world.registry.pool_capacity(1);
		for (size_t slot = reply.minimap_pool1_phase;
				slot < pool1_capacity; slot += 128) {
			const world::Entity *e = world.registry.get(
					world::EntityHandle::make(1, static_cast<int>(slot)));
			if (e == nullptr || !world::minimap_overlay_entity_enabled(*e) ||
					(e->item_attrib & world::kItemAttribSpawnPoint) != 0)
				continue;
			// In a live MP session retail suppresses occupied enemy vehicles.
			if (e->item_type == 1 && e->team != 0 && e->team != recipient_team)
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
			append(*e, false);
		}

		send_minimap_overlay_batches(conn, entries);
		reply.minimap_pool1_phase =
				static_cast<uint8_t>((reply.minimap_pool1_phase + 1u) & 0x7Fu);
		reply.minimap_overlay_cooldown = 13;
	}
}

} // namespace opennova::inmatch
