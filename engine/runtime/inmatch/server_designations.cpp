// The host's designation table -- see server_designations.h.

#include <runtime/inmatch/server_designations.h>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

bool Server_RegisterDesignation(ServerDesignationTable &table, world::EntityHandle owner,
		const int32_t point[3], int32_t ticks, int32_t radius_units, uint8_t mode) {
	ServerDesignation *row = nullptr;
	bool fresh = false;
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
				fresh = true;
				break;
			}
		}
	}
	if (row == nullptr) return false; // a full table drops the mark [orig: @0x51164c]
	// The mode-1 notice: a fresh row, or the owner's row not yet in mode 1
	// [orig: @0x5115D2 / @0x51168D, `cmp byte ptr [slot+24], 1` @0x5115DC].
	const bool notice = mode == 1 && (fresh || row->mode != 1);
	// [orig: @0x511656..0x511676]
	row->owner = owner;
	row->remaining_ticks = ticks;
	row->point[0] = point[0];
	row->point[1] = point[1];
	row->point[2] = point[2];
	row->radius_q16 = static_cast<int32_t>(static_cast<uint32_t>(radius_units) << 16);
	row->mode = mode;
	return notice;
}

void Server_RouteDesignationMarks(std::vector<NapiNPConnection> &roster,
		ServerDesignationTable &table, world::World &world) {
	for (const world::DesignationMark &mark : world.out.designation_marks) {
		const world::Entity *firer = world.registry.get(mark.firer);
		if (firer == nullptr) continue;
		if (!Server_RegisterDesignation(table, mark.firer, mark.point, mark.ticks,
					mark.radius_units, /*mode=*/1))
			continue;
		// The filter is the firer entity's team byte, sign-extended; the
		// mask's 0x100 arm compares each slot's own team byte, zero-extended.
		// [orig: entity+354 @0x5115E8; NapiNPServer_SendFiltered
		//  @0x4C896A..0x4C8977]
		const int32_t team = static_cast<int8_t>(firer->team);
		const std::vector<uint8_t> body = {0x30, world::pool0_index_byte(mark.firer),
				0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
		for (NapiNPConnection &c : roster) {
			if (!active_player_recipient(c) ||
					static_cast<int32_t>(c.assigned_team_valid ? c.assigned_team : 0) != team)
				continue;
			c.link.transport->host_send(s2c::GAME_EVENT, body);
		}
	}
	world.out.designation_marks.clear();
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
	// The recipient slot's own team byte, zero-extended, never its body's
	// (D-NET-636) [orig: `movzx eax, [eax+1A0h]` @0x517f83, pushed @0x517f8a].
	const int32_t team = conn.assigned_team;
	MinimapOverlayBatch batch;
	for (const ServerDesignation &row : table) {
		if (row.remaining_ticks == 0 || !row.owner.valid()) continue; // [orig: @0x5116c1 / @0x5116cf]
		// The owner ENTITY's team, read live and sign-extended [orig: `movsx edx,
		//  [eax+162h]` @0x5116d5, the compare @0x5116dc..0x5116e3].
		const world::Entity *owner = world.registry.get(row.owner);
		if (owner == nullptr || static_cast<int8_t>(owner->team) != team) continue;
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
