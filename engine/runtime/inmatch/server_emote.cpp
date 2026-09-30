// The host side of an emote -- see server_emote.h.

#include <runtime/inmatch/server_emote.h>

#include <cstdlib>

#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// 100 world units, 16.16 [orig: `cmp .., 640000h` @0x501eee / @0x501f01 / @0x501f14].
constexpr int32_t kEmoteRangePerAxisFixed = 0x640000;
// The re-armed cooldown, whole seconds [orig: `mov dword ptr [ebp+178h], 2` @0x501f53].
constexpr int32_t kEmoteCooldownSeconds = 2;

// Pool_GetIndexFromPtr(0, entity): the raw pool-0 index, -1 (0xFF as the
// byte) for an entity outside pool 0 [orig: @0x501e87].
uint8_t pool0_index_byte(world::EntityHandle h) {
	return h.pool() == 0 ? static_cast<uint8_t>(h.slot()) : uint8_t{0xFF};
}

} // namespace

std::vector<ProtocolMessage> Server_HandleEmoteRequest(NapiNPConnection &sender,
		const EmoteRequest &request, std::vector<NapiNPConnection> &roster,
		const world::World &world) {
	std::vector<ProtocolMessage> replies;
	// The sender's player slot and its live player [orig: @0x501e16..0x501e40].
	if (!sender.link.owned_entity.valid()) return replies;
	if (sender.link.spectator) return replies; // +100567 @0x501e33
	const world::Entity *entity = world.registry.get(sender.link.owned_entity);
	if (entity == nullptr) return replies;
	// A dead player, or one still cooling down, sends nothing [orig: @0x501e55].
	if (((entity->flags | entity->engine_flags) & world::kEntityFlagDead) != 0) return replies;
	if (sender.link.emote_cooldown_seconds != 0) return replies;

	EmoteBroadcast broadcast;
	broadcast.emote = static_cast<uint8_t>(request.value & 0xFF); // the low byte @0x501e7a
	broadcast.player_index = pool0_index_byte(entity->handle);    // @0x501e87..0x501e92
	const std::vector<uint8_t> body = encode_emote_broadcast(broadcast);
	const int32_t sx = world::to_fixed(entity->position.x);
	const int32_t sy = world::to_fixed(entity->position.y);
	const int32_t sz = world::to_fixed(entity->position.z);
	for (NapiNPConnection &recipient : roster) {
		// Slot active and NetPlayer state 10/11 [orig: @0x501ec0..0x501edb].
		if (!is_in_match(recipient) || recipient.link.transport == nullptr) continue;
		const world::Entity *other = world.registry.get(recipient.link.owned_entity);
		if (other == nullptr) continue;
		if (std::abs(world::to_fixed(other->position.x) - sx) > kEmoteRangePerAxisFixed ||
				std::abs(world::to_fixed(other->position.y) - sy) > kEmoteRangePerAxisFixed ||
				std::abs(world::to_fixed(other->position.z) - sz) > kEmoteRangePerAxisFixed)
			continue;
		// msgClass 0: the unreliable class [orig: SendFiltered(0x2D, 0, 1)
		// @0x501f38].
		if (&recipient == &sender) {
			ProtocolMessage own = make_protocol_message(s2c::EMOTE_BROADCAST, body);
			own.reliable = false;
			replies.push_back(std::move(own));
		} else {
			recipient.link.transport->host_send(s2c::EMOTE_BROADCAST, body, false);
		}
	}
	sender.link.emote_cooldown_seconds = kEmoteCooldownSeconds;
	return replies;
}

} // namespace opennova::inmatch
