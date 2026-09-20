// The connection-quality metric [orig: CNetQuality_UpdateMetrics @0x4C52C0].
//
// Retail keeps TWO independent five-sample rolling windows on one CNetQuality
// object: the authority's SEND window (this+11 cursor, samples at +12/+17/+22,
// fed by its measured frame rate, the mean of every eligible player slot's
// ten-entry ping ring and the slots' summed loss counters) and the peer's
// RECEIVE window (this+27 cursor, samples at +28/+33/+38, fed by the same
// frame rate, the client's own ten-entry ping ring and its loss counter). Each
// window turns its three inputs into 0..255 sub-metrics, averages the five
// stored samples of each (integer /5, clamped 0..255), max-folds the three
// averages into one quality scalar (this+3 host / this+4 client), and the
// tail folds host and client into `combined = max(host, client)` before
// bucketing it to the 0..4 LEVEL the C2S 0x4C report carries and the
// connection icon draws. Both windows share every formula; only the inputs
// differ, so the joiner (client window, inmatch::ClientRuntime) and the host
// (send window) include this one header rather than two copies of the math.
#pragma once

#include <algorithm>
#include <cstdint>

namespace opennova::replication {

inline constexpr int kNetQualityWindow = 5;

struct NetQualityWindow {
	int32_t cursor = 0; // [orig: this+11 (send) / this+27 (recv)] pre-incremented, wraps at 5
	int32_t bandwidth[kNetQualityWindow] = {}; // [orig: +12.. / +28..]
	int32_t ping[kNetQualityWindow] = {};      // [orig: +17.. / +33..]
	int32_t loss[kNetQualityWindow] = {};      // [orig: +22.. / +38..]
	int32_t avg_bandwidth = 0; // [orig: +5 / +8]  the /5 averages, clamped 0..255
	int32_t avg_ping = 0;      // [orig: +6 / +9]
	int32_t avg_loss = 0;      // [orig: +7 / +10]
	int32_t quality = 0;       // [orig: +3 / +4]  max(avg_bandwidth, avg_ping, avg_loss)
};

// Frame-rate pressure: `256 - 16 * min(fps, 16)`, clamped to 1..255 — any
// rate at or above 16 fps scores the floor 1 [orig: send @0x4C531B..0x4C5350;
// the recv twin reads the same dword_24E1F10].
inline int32_t net_quality_bandwidth_metric(int32_t frame_rate) {
	const int32_t rate = frame_rate > 16 ? 16 : frame_rate;
	const int32_t metric = 256 - 16 * rate;
	if (metric < 1) return 1;
	return metric > 255 ? 255 : metric;
}

// The SEND window's ping term: the per-slot ring means averaged over the
// eligible slots, clamped 255 BEFORE the scale, then `255 * avg / 1000`, min 1
// [orig: @0x4C53FA..0x4C5457].
inline int32_t net_quality_host_ping_metric(uint32_t avg_ping_ms) {
	const uint32_t avg = avg_ping_ms > 0xFFu ? 255u : avg_ping_ms;
	const uint32_t metric = 255u * avg / 1000u;
	return metric == 0 ? 1 : static_cast<int32_t>(metric);
}

// The RECEIVE window's ping term: CNetStats_GetAveragePing over the client's
// ten-entry ring, clamped 1000 BEFORE the scale, then `255 * avg / 1000`,
// min 1 — 1000 ms maps to 255 [orig: the recv leg after
// CNetStats_GetAveragePing @0x4C2750].
inline int32_t net_quality_client_ping_metric(uint32_t avg_ping_ms) {
	const uint32_t avg = avg_ping_ms > 1000u ? 1000u : avg_ping_ms;
	const uint32_t metric = 255u * avg / 1000u;
	return metric == 0 ? 1 : static_cast<int32_t>(metric);
}

// Loss: the counter read (and zeroed) as a double, clamped 2.0, then
// `(int64)(loss * 255.0 * 0.5)`, min 1 — 2.0 maps to 255 [orig: @0x4C54C6].
inline int32_t net_quality_loss_metric(double loss) {
	if (loss > 2.0) loss = 2.0;
	const int64_t metric = static_cast<int64_t>(loss * 255.0 * 0.5);
	return metric == 0 ? 1 : static_cast<int32_t>(metric);
}

// One sample: pre-increment the cursor (wraps at 5), store the three terms in
// that slot, sum the five slots of each, divide by 5 (integer), clamp each
// average to 0..255, then max-fold the three averages into `quality`
// [orig: the sums and /5 @0x4C5555..0x4C557B / the clamps and the max-fold
//  @0x4C55CF..0x4C55DA].
inline void net_quality_window_push(NetQualityWindow &w, int32_t bandwidth, int32_t ping,
		int32_t loss) {
	if (++w.cursor >= kNetQualityWindow) w.cursor = 0;
	w.bandwidth[w.cursor] = bandwidth;
	w.ping[w.cursor] = ping;
	w.loss[w.cursor] = loss;
	int32_t sum_bandwidth = 0, sum_ping = 0, sum_loss = 0;
	for (int i = 0; i < kNetQualityWindow; ++i) {
		sum_bandwidth += w.bandwidth[i];
		sum_ping += w.ping[i];
		sum_loss += w.loss[i];
	}
	const auto clamp255 = [](int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
	w.avg_bandwidth = clamp255(sum_bandwidth / kNetQualityWindow);
	w.avg_ping = clamp255(sum_ping / kNetQualityWindow);
	w.avg_loss = clamp255(sum_loss / kNetQualityWindow);
	w.quality = std::max({w.avg_bandwidth, w.avg_ping, w.avg_loss});
}

// The spawn-gate clear: while the peer is held (spawn suspended, the gameplay
// dword, the spawn-success gate, the pre-round timer) every counter, sample,
// average, the cursor and the folded quality are zeroed rather than sampled
// [orig: CNetStats_ClearRecvCounters @0x4C2F90 for the recv window;
//  CNetStats_ClearSendCounters (the send-window call @0x4C55E3)].
inline void net_quality_window_clear(NetQualityWindow &w) { w = NetQualityWindow{}; }

// The combined scalar -> level bucket: 0 when the scalar is 0 (or out of
// 1..255), 1 for 1..84, 2 for 85..169, 3 above 169, clamped to 4 by the
// setter [orig: the tail (`quality_level = (combined > 169) + 2` @0x4C5893);
//  CNetQuality_SetLevel @0x4C3060 clamps 0..4 and raises the dirty flag].
inline int32_t net_quality_level(int32_t combined) {
	int32_t level = 0;
	if (combined >= 1 && combined <= 255) level = combined > 84 ? (combined > 169 ? 3 : 2) : 1;
	if (level < 0) return 0;
	return level > 4 ? 4 : level;
}

} // namespace opennova::replication
