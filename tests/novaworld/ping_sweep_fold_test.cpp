// The browser's per-row ping-result fold (engine/net/novaworld/ping_sweep.h):
// retail folds a raw code before display — -4 (never attempted) reads as -3,
// every other negative (send/timeout/error) reads as -2, and raw 0 carries the
// millisecond round-trip time through unchanged
// [orig: NapiGameList_OnPingResult @0x63bc60].

#include <net/novaworld/ping_sweep.h>

#include <cstdio>
#include <string>
#include <vector>

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

	// The pass loop over an echo primitive: unparseable and 0.0.0.0 rows are
	// never attempted; an answered row is not driven again; an unanswered row
	// is driven on the initial pass plus each of the retries; the fold reads
	// every row back.
	{
		std::vector<PingEcho> echoes = make_ping_echoes({
				{1, "10.0.0.1"},      // answers on the first pass
				{2, "10.0.0.2"},      // never answers
				{3, "0.0.0.0"},       // unreported: never attempted
				{4, "not-an-address"}, // unparseable: never attempted
				{5, "10.0.0.5"},      // answers on the second pass
		});
		expect(echoes.size() == 5, "one slot per row");
		expect(echoes[0].attempted && echoes[1].attempted && !echoes[2].attempted &&
		               !echoes[3].attempted && echoes[4].attempted,
		       "the sweep drives only the parseable, reported addresses");
		int passes = 0;
		int drives[6] = {0, 0, 0, 0, 0, 0};
		run_ping_passes(echoes, [&](std::vector<PingEcho *> &chunk) {
			++passes;
			for (PingEcho *e : chunk) {
				++drives[e->row_id];
				if (e->row_id == 1) { e->answered = true; e->ms = 12; }
				if (e->row_id == 5 && passes == 2) { e->answered = true; e->ms = 40; }
			}
		});
		expect(passes == 1 + kPingRetries, "the initial pass plus the two retries ran");
		expect(drives[1] == 1, "an answered row is not driven again");
		expect(drives[2] == 3, "an unanswered row rides every pass");
		expect(drives[3] == 0 && drives[4] == 0, "never-attempted rows are never driven");
		expect(drives[5] == 2, "a row answered on the second pass stops there");
		const auto folded = fold_ping_sweep(echoes);
		expect(folded.size() == 5, "the fold covers every row");
		expect(folded[0].second == 12 && folded[4].second == 40, "answered rows read their ms");
		expect(folded[1].second == kPingFailed, "an attempted, unanswered row reads failed (-2)");
		expect(folded[2].second == kPingNeverAttempted && folded[3].second == kPingNeverAttempted,
		       "rows the sweep could not drive read never-attempted (-3)");
	}
	// Chunking under the device wait cap, and the cancel poll between chunks.
	{
		std::vector<std::pair<int64_t, std::string>> many;
		for (int i = 0; i < 130; ++i) many.emplace_back(i, "10.1.0." + std::to_string(i % 250 + 1));
		std::vector<PingEcho> echoes = make_ping_echoes(many);
		size_t largest = 0;
		int chunks = 0;
		run_ping_passes(echoes, [&](std::vector<PingEcho *> &chunk) {
			++chunks;
			if (chunk.size() > largest) largest = chunk.size();
			for (PingEcho *e : chunk) { e->answered = true; e->ms = 1; }
		});
		expect(largest == kPingPassChunk && chunks == 3, "130 rows ride as 60/60/10 in one pass");
		std::vector<PingEcho> cancelled = make_ping_echoes(many);
		int seen = 0;
		run_ping_passes(cancelled, [&](std::vector<PingEcho *> &chunk) { seen += static_cast<int>(chunk.size()); },
		                [&]() { return seen > 0; });
		expect(seen == static_cast<int>(kPingPassChunk), "a cancel between chunks ends the sweep");
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("ping_sweep_fold_test OK\n");
	return 0;
}
