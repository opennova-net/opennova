// The CNetQuality window math (runtime/replication/net_quality.h): the three
// 0..255 sub-metrics, the five-sample push with its integer /5 averages and
// max-fold, the spawn-gate clear, and the 0..4 level buckets at their exact
// boundaries [orig: CNetQuality_UpdateMetrics @0x4C52C0; CNetQuality_SetLevel
// @0x4C3060].

#include <runtime/replication/net_quality.h>

#include <cstdio>

namespace {

namespace ns = opennova::replication;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

bool run_level_buckets() {
	bool ok = true;
	ok &= expect(ns::net_quality_level(0) == 0, "0 -> level 0");
	ok &= expect(ns::net_quality_level(1) == 1, "1 -> level 1");
	ok &= expect(ns::net_quality_level(84) == 1, "84 -> level 1");
	ok &= expect(ns::net_quality_level(85) == 2, "85 -> level 2");
	ok &= expect(ns::net_quality_level(169) == 2, "169 -> level 2");
	ok &= expect(ns::net_quality_level(170) == 3, "170 -> level 3");
	ok &= expect(ns::net_quality_level(255) == 3, "255 -> level 3");
	ok &= expect(ns::net_quality_level(256) == 0, "out of 1..255 -> level 0");
	ok &= expect(ns::net_quality_level(-1) == 0, "negative -> level 0");
	return ok;
}

bool run_metrics() {
	bool ok = true;
	ok &= expect(ns::net_quality_bandwidth_metric(62) == 1, "62 fps scores the floor 1");
	ok &= expect(ns::net_quality_bandwidth_metric(16) == 1, "16 fps scores the floor 1");
	ok &= expect(ns::net_quality_bandwidth_metric(8) == 128, "8 fps scores 256 - 128");
	ok &= expect(ns::net_quality_bandwidth_metric(0) == 255, "0 fps clamps to 255");
	ok &= expect(ns::net_quality_client_ping_metric(0) == 1, "0 ms ping scores the floor 1");
	ok &= expect(ns::net_quality_client_ping_metric(400) == 102, "400 ms -> 255*400/1000");
	ok &= expect(ns::net_quality_client_ping_metric(5000) == 255, "the client clamps at 1000 ms");
	ok &= expect(ns::net_quality_host_ping_metric(5000) == 65,
	             "the host clamps the average at 255 BEFORE the scale");
	ok &= expect(ns::net_quality_loss_metric(0.0) == 1, "no loss scores the floor 1");
	ok &= expect(ns::net_quality_loss_metric(1.0) == 127, "1.0 -> (int)(127.5)");
	ok &= expect(ns::net_quality_loss_metric(9.0) == 255, "loss clamps at 2.0 -> 255");
	return ok;
}

bool run_window_push_and_clear() {
	ns::NetQualityWindow w;
	// One push of (1, 102, 1): the /5 averages truncate the lone sample.
	ns::net_quality_window_push(w, 1, 102, 1);
	bool ok = expect(w.cursor == 1 && w.avg_bandwidth == 0 && w.avg_ping == 20 &&
	                         w.avg_loss == 0 && w.quality == 20,
	                 "first push: pre-incremented cursor, truncated averages, max-fold");
	for (int i = 0; i < 4; ++i) ns::net_quality_window_push(w, 1, 102, 1);
	ok &= expect(w.cursor == 0 && w.avg_ping == 102 && w.quality == 102,
	             "five pushes fill the window: the cursor wrapped, the average is whole");
	ns::net_quality_window_push(w, 300, 102, 1);
	ok &= expect(w.avg_bandwidth == 60 && w.quality == 102,
	             "a sample beyond 255 is not clamped until the average");
	ns::net_quality_window_clear(w);
	ok &= expect(w.cursor == 0 && w.quality == 0 && w.avg_ping == 0 && w.ping[1] == 0,
	             "the spawn-gate clear zeroes every slot, average and the fold");
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_level_buckets();
	ok &= run_metrics();
	ok &= run_window_push_and_clear();
	if (ok) std::printf("net_quality_test: OK\n");
	return ok ? 0 : 1;
}
