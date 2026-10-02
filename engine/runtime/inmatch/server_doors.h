#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_connection.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The host side of a client's door request (C2S 0x1A -> S2C 0x37). The sender
// must hold a player slot; the body [u16 handle][i16 value][u8 number] (zero
// defaults) addresses record (first + number - 1) of the handle's pool row,
// gated on a def, a non-negative index, a non-zero number and number <= the
// def's door count. A closed or closing record (0/3) opens (1) only for value
// 1; an opening or open record (1/2) takes the value only when number is 3,
// literally. The record's resulting state goes back to the sender alone
// (mask 0x30) as S2C 0x37 {handle, state, number}; nothing else moves (no
// sound, no phase: the host's door tick carries an opening record on).
// [orig: NapiNPServerMsg_HandleVoteUpdate @0x514B20 (a misnomer) — the
//  authority @0x514b24, the player slot @0x514b31..0x514b40, the reads
//  @0x514b5f..0x514b91, the pool/def/index gates @0x514ba8..0x514c08, the
//  record switch @0x514c20..0x514c35, the reply NetPacket_WriteTwoShortsAndByte
//  @0x514c6f, SendFiltered(0x37, mask 48) @0x514c46..0x514c74]
std::vector<ProtocolMessage> Server_HandleDoorRowRequest(const NapiNPConnection &sender,
		const std::vector<uint8_t> &payload, world::World &world);

} // namespace opennova::inmatch
