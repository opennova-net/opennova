// The joiner net readout: a debug instrument with no behaviour. One record per client net
// frame the joiner's ClientRuntime runs to its end (run_frame), for the joiner_net_trace probe
// (godot/probes/net) and its binding (Simulation::debug_joiner_net_trace). Nothing in the game
// reads it; every recording call hands its value back unchanged (trace_note) and adds no branch.
//
// A record carries:
// - the frame's clocks: the replica's frame tick (g_CurrentTick's stand-in), the 0x61-seeded
//   network tick, the connection's monotonic milliseconds at the frame's end (ticks one render
//   frame banked share it), and the retail loop the frame ran in;
// - the send holdoff: the countdown ahead of the frame's step, the dictated period, whether the
//   send block opened and whether the C2S 0x0C left in it [orig: Client_ProcessNetworkFrame
//   @0x42C180 - the send-holdoff gate @0x42C3DD, the 0x0C @0x42C46F..0x42C4A3; the countdown
//   NapiNPConnection+0x648, stepped by CNapiNPConnection_PumpFlags @0x629780 (@0x6297ac) and
//   reloaded @0x629802];
// - the S2C 0x0A frames the replica has applied, a running count (a step between two records
//   is that frame's receive) [orig: NapiNPClientMsg_0x00A @0x42FEC0];
// - the 62-frame quality fold when it ran in the frame, ahead of the receive: its inputs (the
//   measured frame rate, the three holds), the frame-rate and ping terms it pushed, the client
//   window's quality after it, the host scalar and the level it set [orig: Game_ProcessMainFrame
//   @0x5263F0 - the countdown @0x526537..0x526555, the CNetQuality_SetLevel call @0x52659b;
//   CNetQuality_UpdateMetrics @0x4C52C0 - the holds @0x4c5606..0x4c5630, the frame rate
//   g_StatsAvgFps read @0x4C5643];
// - the C2S 0x4C the frame queued and its level [orig: Client_ProcessNetworkFrame @0x42C180 -
//   the timer @0x42c23e, the queue @0x42c279].
#pragma once

#include <runtime/inmatch/joiner_connection.h> // JoinerConnection::SendPumpLoop

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::inmatch {

// Records `value` into `slot` and hands it back unchanged.
template <typename T>
inline T trace_note(T &slot, T value) {
	slot = value;
	return value;
}

struct JoinerNetTraceRecord {
	uint32_t seq = 0;
	uint32_t frame_tick = 0;  // ClientReplicaPipeline::frame_tick after the frame
	uint32_t client_tick = 0; // the 0x61-seeded network tick (0 unseeded)
	uint32_t wall_ms = 0;     // JoinerConnection::monotonic_milliseconds32 at the frame's end
	JoinerConnection::SendPumpLoop pump_loop = JoinerConnection::SendPumpLoop::NetworkFrame;
	uint32_t countdown_before = 0; // the send holdoff countdown ahead of the frame's step
	uint32_t holdoff_ticks = 0;    // the dictated period it reloads from
	bool block_open = false;
	bool uplink_sent = false;      // the C2S 0x0C left in this frame's send block
	uint32_t frames_applied = 0;   // ClientState::frames_applied after the frame
	bool quality_folded = false;
	int32_t fold_fps = 0;          // the measured frame rate the fold read
	bool fold_deploy_hold = false; // the deploy hold (not deployed)
	bool fold_spawn_gate = false;  // the round-over / spawn-success gate
	bool fold_preround = false;    // a pre-round timer up
	int32_t fold_bandwidth = 0;    // the frame-rate term pushed (0: held, nothing pushed)
	int32_t fold_ping = 0;         // the ping term pushed (0: held)
	int32_t window_quality = 0;    // the client window's quality after the fold
	int32_t host_metric = 0;       // the host scalar the fold read (the last S2C 0x79)
	uint8_t level = 0;             // the level the fold set
	bool report_queued = false;    // a C2S 0x4C queued in this frame
	uint8_t report_level = 0;      // its level byte
};

// The last kCapacity records (about 33 s of 62.5 Hz frames); seq counts every record appended.
// The ring lives on the heap, taken at the first record.
class JoinerNetTrace {
public:
	static constexpr std::size_t kCapacity = 2048;

	void append(JoinerNetTraceRecord record) {
		if (ring_.empty()) ring_.resize(kCapacity);
		record.seq = next_seq_++;
		ring_[record.seq % kCapacity] = record;
	}
	uint32_t next_seq() const { return next_seq_; }
	uint32_t oldest_seq() const {
		return next_seq_ > kCapacity ? next_seq_ - static_cast<uint32_t>(kCapacity) : 0u;
	}
	// The held records with seq >= from, oldest first.
	std::vector<JoinerNetTraceRecord> since(uint32_t from) const {
		std::vector<JoinerNetTraceRecord> out;
		for (uint32_t seq = from > oldest_seq() ? from : oldest_seq(); seq < next_seq_; ++seq)
			out.push_back(ring_[seq % kCapacity]);
		return out;
	}

private:
	std::vector<JoinerNetTraceRecord> ring_;
	uint32_t next_seq_ = 0;
};

inline const char *send_pump_loop_name(JoinerConnection::SendPumpLoop loop) {
	switch (loop) {
		case JoinerConnection::SendPumpLoop::NetworkFrame: return "network_frame";
		case JoinerConnection::SendPumpLoop::ServerInfoWait: return "server_info_wait";
		case JoinerConnection::SendPumpLoop::MissionDataWait: return "mission_data_wait";
		case JoinerConnection::SendPumpLoop::SyncTailWait: return "sync_tail_wait";
		case JoinerConnection::SendPumpLoop::WorldStreamWait: return "world_stream_wait";
		case JoinerConnection::SendPumpLoop::MissionStart: return "mission_start";
		case JoinerConnection::SendPumpLoop::WorldStateWait: return "world_state_wait";
	}
	return "unknown";
}

} // namespace opennova::inmatch
