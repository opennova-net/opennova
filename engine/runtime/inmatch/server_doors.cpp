// The host side of a client's door request -- see server_doors.h.

#include <runtime/inmatch/server_doors.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

std::vector<ProtocolMessage> Server_HandleDoorRowRequest(const NapiNPConnection &sender,
		const std::vector<uint8_t> &payload, world::World &world) {
	std::vector<ProtocolMessage> replies;
	// The sender's player slot [orig: @0x514b31..0x514b40].
	if (!sender.link.owned_entity.valid()) return replies;
	DoorSlotAction request;
	size_t consumed = 0;
	decode_door_slot_action(payload.data(), payload.size(), request, consumed);
	// The pool row the handle names [orig: @0x514ba8..0x514be5].
	const world::EntityHandle handle{request.entity_handle};
	if (!handle.valid() || handle.pool() >= world::kEntityPoolCount) return replies;
	const world::Entity *entity = world.registry.get(handle);
	if (entity == nullptr) return replies;
	int32_t state = 0;
	if (!world.doors.apply_request(*entity, request.number, request.state, state)) return replies;
	DoorSlotAction reply;
	reply.entity_handle = request.entity_handle;
	reply.state = static_cast<int16_t>(state);
	reply.number = request.number;
	replies.push_back(make_protocol_message(s2c::DOOR_SLOT_ACTION, encode_door_slot_action(reply)));
	return replies;
}

} // namespace opennova::inmatch
