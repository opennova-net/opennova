#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h> // SessionSequencing
#include <net/npwire/session_hello.h>    // CsField, the two template field lists

namespace opennova {

// The cs_dir0 slots a NAPI connection's own pumps read (retail's NapiCSConfig at conn+0x17C),
// one per connection. The member defaults are the JOINTOPERATIONS game-session template; the
// NOVAWORLDUDP lobby connection starts from novaworld_service_cs_config(). Either is then
// overlaid by the peer's 0x82 CS block and by later H:0x00 CS updates (apply_cs_field).
// Each slot's consumer:
//   0  timeout_ms               the receive-silence reap [orig: PumpStateMachine @0x62934c
//                               (state 1) / @0x6295a2 (state 5)]
//   1  recv_max_per_tick        the teardown's disconnect-packet burst
//                               [orig: TeardownActiveConnection @0x6253ef]
//   4  idle_send_interval_ms    PumpSendIntervals' EMPTY leg [orig: @0x629041]
//   5  active_send_interval_ms  PumpSendIntervals' ACTIVE leg [orig: @0x628ff1]
//   6  packet_queue_interval_ms PumpSendIntervals' timed missing-sequence leg
//                               [orig: @0x628fe9, the test @0x629032]
//   10 packet_queue_max         HandleSessionPacket's out-of-order queue bound
//                               [orig: @0x626c18]
//   11 msg_out_max              NapiNPMessage_Create's outbound-node pool bound
//                               [orig: @0x628048]
//   13 max_packet_bytes         BuildOutgoingPackets' packet ceiling [orig: @0x628436]
//   14 max_packets_per_tick     BuildOutgoingPackets' packets per call [orig: @0x62844e]
// [orig: CNapiNetwork_Init @0x4cab60 (4), @0x4cab88 (30000), @0x4cab98 (10000),
//  @0x4cabe0 (100), @0x4cab3c (mpmaxpacketsize, 1300 by default), @0x4cac18 (-1, the
//  `or ecx, -1` @0x4caad5); CNapiGameSession_InitNPConnection @0x4d3e1f..0x4d3f64]
struct CsConfig {
	int32_t timeout_ms = 120000;              // CS field 0
	int32_t recv_max_per_tick = 4;            // CS field 1
	int32_t idle_send_interval_ms = 30000;    // CS field 4
	int32_t active_send_interval_ms = 10000;  // CS field 5
	int32_t packet_queue_interval_ms = -1;    // CS field 6
	int32_t packet_queue_max = 100;           // CS field 10
	int32_t msg_out_max = 0x4B0;              // CS field 11
	int32_t max_packet_bytes = 1300;          // CS field 13
	int32_t max_packets_per_tick = -1;        // CS field 14
};

// Store one CS slot the way CNapiNPConnection_HandleCSConfigUpdate and
// NapiNP_HandleServerJoinResponse store it into cs_dir0: the raw dword, whatever its value.
// Slots this struct does not carry have no consumer here.
// [orig: HandleCSConfigUpdate @0x6219c8; HandleServerJoinResponse @0x629b63]
void apply_cs_field(CsConfig &cfg, uint32_t slot, int32_t value);

// The two connection templates as a CsConfig: the same witnessed field lists the 0x82
// advertises (session_hello.h), so a connection and the block it sends never disagree.
// `max_packet_bytes` is the configured game.cfg `mpmaxpacketsize` (0 -> 1300).
CsConfig novaworld_service_cs_config(int32_t max_packet_bytes = 0);
CsConfig jointoperations_cs_config(int32_t max_packet_bytes = 0);

// The teardown of an active connection sends its disconnect packet cs_dir0.recv_max_per_tick
// (CS field 1) times, clamped to [0, 32]; the first send goes whatever the count, so a count of
// 0 still sends one.
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 — the clamp @0x6253ef..0x625403,
//  the first send @0x625406 and the loop @0x625412..0x625424 (state 1), @0x62549e..0x6254d3
//  (state 5)]
inline constexpr std::size_t disconnect_burst_count(int32_t recv_max_per_tick) {
	const int32_t clamped =
			recv_max_per_tick > 32 ? 32 : (recv_max_per_tick < 0 ? 0 : recv_max_per_tick);
	return clamped < 1 ? std::size_t{1} : static_cast<std::size_t>(clamped);
}

// A negative cs_dir msg_out_max (the `_NSTMOUT.TXT` NEVER form) is unbounded: NapiNPMessage_Create
// only checks the pool when `msg_out_max >= 0` [orig: @0x628048], and zero is
// session_outbound_message_prefix_count's unbounded sentinel.
inline std::size_t outbound_message_limit_for(int32_t msg_out_max) {
	return msg_out_max < 0 ? std::size_t{0} : static_cast<std::size_t>(msg_out_max);
}

// The cs_dir0 slots the shared session sequencing enforces, pushed onto a connection's
// sequencing whenever its block changes: the outbound pool bound (field 11) and the
// out-of-order queue bound (field 10).
inline void sync_session_sequencing_limits(SessionSequencing &sequencing, const CsConfig &cs) {
	sequencing.outbound_message_limit = outbound_message_limit_for(cs.msg_out_max);
	sequencing.packet_queue_max = cs.packet_queue_max;
}

// BuildOutgoingPackets' packets per call, cs_dir0 field 14: negative is unbounded, and the
// loop builds one packet before it first tests the count, so 0 still builds one.
// [orig: BuildOutgoingPackets @0x62844e (the load), @0x62860b..0x628619 (`max < 0` or
//  `built < max` loops again)]
inline std::size_t max_packets_per_build(int32_t max_packets_per_tick) {
	if (max_packets_per_tick < 0) return static_cast<std::size_t>(-1);
	return max_packets_per_tick < 1 ? std::size_t{1}
	                                : static_cast<std::size_t>(max_packets_per_tick);
}

// BuildOutgoingPackets' packet ceiling, cs_dir0 field 13 floored at 26.
// [orig: BuildOutgoingPackets @0x628440..0x628446]
inline std::size_t packet_ceiling_bytes(int32_t max_packet_bytes) {
	return max_packet_bytes < 26 ? std::size_t{26} : static_cast<std::size_t>(max_packet_bytes);
}

} // namespace opennova
