// The host's designation table -- see server_designations.h.

#include <runtime/inmatch/server_designations.h>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

void Server_RegisterDesignation(ServerDesignationTable &table, world::EntityHandle owner,
		const int32_t point[3], int32_t ticks, int32_t radius_units, uint8_t mode) {
	ServerDesignation *row = nullptr;
	// The owner's own row wins [orig: @0x51158d..0x5115a5].
	for (ServerDesignation &candidate : table) {
		if (candidate.owner.valid() && candidate.owner == owner) {
			row = &candidate;
			break;
		}
	}
	// Else the first row with no life and no owner [orig: @0x5115a9..0x5115c9].
	if (row == nullptr) {
		for (ServerDesignation &candidate : table) {
			if (candidate.remaining_ticks == 0 && !candidate.owner.valid()) {
				row = &candidate;
				break;
			}
		}
	}
	if (row == nullptr) return; // a full table drops the mark [orig: @0x51164c]
	// [orig: @0x511656..0x511676]
	row->owner = owner;
	row->remaining_ticks = ticks;
	row->point[0] = point[0];
	row->point[1] = point[1];
	row->point[2] = point[2];
	row->radius_q16 = static_cast<int32_t>(static_cast<uint32_t>(radius_units) << 16);
	row->mode = mode;
}

void Server_TickDesignations(ServerDesignationTable &table) {
	// [orig: Server_TickUpdate @0x51e4a0..0x51e4af — `jle` skips a row whose
	//  life is already zero, so its owner stays]
	for (ServerDesignation &row : table) {
		if (row.remaining_ticks <= 0) continue;
		if (--row.remaining_ticks == 0) row.owner = world::EntityHandle{};
	}
}

namespace {

// The serializer's signed division toward zero, `cdq; and edx, 0FFFFh; add;
// sar 10h` [orig: @0x511738..0x511741].
int32_t whole_units(int32_t q16) {
	return q16 / 0x10000;
}

} // namespace

void Server_SendDesignationsToPlayer(NapiNPConnection &conn, const ServerDesignationTable &table,
		const world::World &world) {
	if (conn.link.transport == nullptr) return;
	// The recipient slot's team byte [orig: playerSlot[416] @0x517f8a]; the
	// port's slot team is its player's.
	uint8_t team = 0;
	if (const world::Entity *player = world.registry.get(conn.link.owned_entity))
		team = player->team;
	MinimapOverlayBatch batch;
	for (const ServerDesignation &row : table) {
		if (row.remaining_ticks == 0 || !row.owner.valid()) continue; // [orig: @0x5116c1 / @0x5116cf]
		// The owner row's team, read live [orig: entity+354 @0x5116dc..0x5116e3].
		const world::Entity *owner = world.registry.get(row.owner);
		if (owner == nullptr || owner->team != team) continue;
		MinimapOverlayBatch::Entry entry;
		entry.handle = row.owner.packed; // the pool walk @0x5116e9..0x51180a
		entry.x = static_cast<int16_t>(whole_units(row.point[0])); // @0x51175a
		entry.y = static_cast<int16_t>(whole_units(row.point[1])); // @0x51176d
		entry.z = static_cast<int16_t>(whole_units(row.point[2])); // @0x511786
		entry.lifetime_s = static_cast<uint16_t>(row.remaining_ticks / 62); // @0x51171f..0x511732
		entry.type = row.mode;                                             // @0x51178a
		entry.height = static_cast<uint8_t>(whole_units(row.radius_q16));  // @0x511744
		batch.entries.push_back(entry);
	}
	if (batch.entries.empty()) return; // [orig: `if (payload_len > 0)` @0x517fa4]
	conn.link.transport->host_send(s2c::MINIMAP_OVERLAY,
			encode_minimap_overlay_batch(batch), /*reliable=*/false);
}

} // namespace opennova::inmatch
