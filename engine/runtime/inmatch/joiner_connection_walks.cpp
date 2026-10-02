// JoinerConnection — the paged host lists a client walks with its own reply:
// each page the host sends answers one C2S request, and the client's handler
// queues the request for the next page itself. The entity folds of the same
// messages are the replica pipeline's (every admitted message reaches both, in
// wire order); this TU owns only the replies. Split out of
// joiner_connection.cpp; the class header is the shared declaration.
#include <runtime/inmatch/joiner_connection.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/entity.h> // retail_pool_capacity

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::inmatch {

void JoinerConnection::on_list_walk_page(const ProtocolMessage &m,
		std::vector<ProtocolMessage> &replies) {
	if (m.tag == s2c::WAIT_FOR_GAME_START_ACK) {
		// Every 0x1A stores its dword, 0 for a short body; only the kill-list
		// continuation reads it [orig: NapiNPClientMsg_0x01A @0x425eb0 ->
		// dword_A82364 @0x425ec3/@0x425ecb].
		game_start_ack_timestamp_ = 0;
		if (m.payload.size() >= 4)
			game_start_ack_timestamp_ = uint32_t(m.payload[0]) | (uint32_t(m.payload[1]) << 8) |
					(uint32_t(m.payload[2]) << 16) | (uint32_t(m.payload[3]) << 24);
	} else if (m.tag == s2c::KILL_BY_SLOT) {
		// The join-window kill-list walk: a page that carried at least one slot
		// after its leading resume word queues ONE reliable C2S 0x28 {the 0x19
		// window min, the 0x1A value, the resume word}, so the host serves the
		// next page from there; a bare page (FF FF from a walk that found
		// nothing, or any body under four bytes) ends the walk unanswered.
		// The window max is the 0x1A value, NOT the 0x0F tick the burst's
		// first 0x28 carried. [orig: NapiNPClientMsg_HandleBatchKill @0x431870 —
		// the resume word @0x43188a, the slot count @0x431893..0x43189a, the
		// reply {dword_A82360 @0x4318db, dword_A82364 @0x4318e8, resume
		// @0x4318ee} queued (0x28, 1, 0, .., 10) @0x4318ff, the bare-page
		// return @0x431904]
		if (m.payload.size() < 4) return;
		BurstLoadoutRequest next;
		next.loadout_filter = spawn_ack_timestamp_;
		next.flags = game_start_ack_timestamp_;
		next.extra = static_cast<uint16_t>(m.payload[0] | (m.payload[1] << 8));
		replies.push_back(make_protocol_message(c2s::LOADOUT_REQUEST,
				encode_burst_loadout_request(next)));
	} else if (m.tag == s2c::TEAM_CHANGE_CONFIRM) {
		// The team-change list walk: an entry whose handle resolves to a pool
		// slot queues ONE reliable C2S 0x29 {index + 1}, authority or not, so
		// the host answers with the next entry until its list runs out (an
		// index past the list draws nothing). The 0x0F burst's 0x29 {0} starts
		// the walk. [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 — the
		// handle gates @0x431c1a..0x431c49, CNapiNetwork_QueueReliableMessage
		// (0x29, 1, 0, {index + 1}, 2) @0x431c88..0x431c99; the host's walk
		// NapiNPServerMsg_0x029 @0x514F10]
		TeamChangeConfirm entry;
		decode_team_change_confirm(m.payload.data(), m.payload.size(), entry);
		const world::EntityHandle h{entry.assign.entity_handle};
		if (!h.valid() || h.pool() >= world::kEntityPoolCount ||
				static_cast<std::size_t>(h.slot()) >= world::retail_pool_capacity(h.pool()))
			return;
		const uint16_t next = static_cast<uint16_t>(entry.index + 1u);
		replies.push_back(make_protocol_message(c2s::TEAM_SPAWN_ACK,
				{static_cast<uint8_t>(next & 0xFFu), static_cast<uint8_t>(next >> 8)}));
	}
}

} // namespace opennova::inmatch
