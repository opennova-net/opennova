// The browser's per-row ping-result fold (engine/net/novaworld/ping_sweep.h):
// retail folds a raw code before display — -4 (never attempted) reads as -3,
// every other negative (send/timeout/error) reads as -2, and raw 0 carries the
// millisecond round-trip time through unchanged
// [orig: NapiGameList_OnPingResult @0x63bc60].

#include <net/novaworld/ping_sweep.h>

#include <cstdio>

using namespace opennova;

namespace {

int g_failures = 0;

bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
	return cond;
}

} // namespace

int main() {
	expect(fold_ping_result(-4, 0) == kPingNeverAttempted,
	       "raw -4 folds to never-attempted (-3)");
	expect(fold_ping_result(-4, 120) == kPingNeverAttempted,
	       "a stale ms value never leaks through the -4 fold");
	expect(fold_ping_result(-3, 0) == kPingFailed, "raw -3 folds to failed (-2)");
	expect(fold_ping_result(-2, 0) == kPingFailed, "raw -2 folds to failed (-2)");
	expect(fold_ping_result(-1, 0) == kPingFailed, "raw -1 folds to failed (-2)");
	expect(fold_ping_result(0, 0) == 0, "raw 0 with 0 ms reads as 0 ms");
	expect(fold_ping_result(0, 47) == 47, "raw 0 carries the entry's ms through");
	expect(kPingNeverAttempted == -3 && kPingFailed == -2,
	       "the folded display codes are the witnessed -3/-2");

	// The witnessed sweep constants [orig: NapiConnection_Init @0x63037c].
	expect(kPingTimeoutMs == 3000, "per-echo timeout is the stored 3000 ms");
	expect(kPingRetries == 2, "retry count is the stored 2");

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("ping_sweep_fold_test OK\n");
	return 0;
}
