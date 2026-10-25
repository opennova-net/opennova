#pragma once

// What a supervised run of opennova-serve leaves its supervisor (a hosted
// server's machine agent, apps/hostd) in its working directory, none of it
// retail's: the run's tick health (--stats-file), rewritten once a minute, the
// line it exits with (--exit-file), written at a non-zero exit, and its second
// listing's state (--second-listing-file), rewritten whenever it changes.
//
// Tick health. The loop wakes once per logic quantum (62.5 Hz, main.cpp); each
// frame's time is its work, from its start to the loop's sleep. A frame is late
// when it starts past its own end's deadline: the loop is a whole quantum
// behind (a slow frame before it, or a machine that gave the server no CPU).
// Every kWindowMs the window's frames are summed up as their p50, p99 and
// longest frame time and the late frames, and the window starts over. The
// times are the caller's steady clock in nanoseconds, so a test steps them by
// hand. Instrumentation only: the loop runs as it did. Service policy, not a
// port.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::serve {

// One window of frames, summed up.
struct TickMinute {
	double p50_ms = 0.0;
	double p99_ms = 0.0;
	double max_ms = 0.0;
	uint64_t late = 0;
	uint64_t frames = 0;
};

class TickStats {
public:
	static constexpr int64_t kWindowNs = 60LL * 1000 * 1000 * 1000;

	// A frame that started at `start_ns`, ended its work at `end_ns`, and was
	// due to end by `deadline_ns` (late when it started past that).
	void frame(int64_t start_ns, int64_t end_ns, int64_t deadline_ns);
	// The window's summary once kWindowNs have passed since its first frame at
	// `now_ns`, and a new window begun; nullopt before that (or with no frame).
	std::optional<TickMinute> take_if_due(int64_t now_ns);

private:
	std::vector<double> frame_ms_;
	uint64_t late_ = 0;
	std::optional<int64_t> window_start_ns_;
};

// The nearest-rank `q` quantile (0 < q <= 1) of `values` (reordered); 0 for none.
double quantile(std::vector<double> &values, double q);

// The --stats-file's text, one JSON line: {"at": <Unix seconds>, "p50_ms",
// "p99_ms", "max_ms", "late", "frames"} (the times to 0.01 ms).
std::string tick_minute_json(const TickMinute &minute, int64_t at_unix);

// The --exit-file's text: `error` as one line (every control character a
// space), newline-terminated.
std::string exit_file_text(const std::string &error);

// The second listing's state (ServeOptions::also_master_host: a hosted server's
// listing on NovaLogic's NovaWorld beside OpenNova's, ADR 0068), as its
// supervisor shows the server's owner whether and why it is listed there. Off:
// the run has none (no --also-master-host, or the LAN network type). Trying: a
// try under way (its gate's lookup, the gate, the session, the sign-in and the
// HOSTKEY, the host request). Listed: it hosts. Retrying: a try ended on a fault
// and the next waits out its backoff. NotListed: over for the run, its service's
// decision (a refusal, a stop) or its tries spent (Server::tick_second_listing).
enum class SecondListingState { Off, Trying, Listed, Retrying, NotListed };
// "off", "trying", "listed", "retrying", "not_listed".
const char *second_listing_state_text(SecondListingState state);

struct SecondListingReport {
	SecondListingState state = SecondListingState::Off;
	// Trying and Retrying: the try under way or the next, of the run's most
	// (kSecondListingTriesMax).
	int attempt = 0;
	int attempts = 0;
	// Retrying: when the next try starts, Unix seconds.
	int64_t next_try_at = 0;
	// Retrying and NotListed: why, in plain words ("the service did not accept
	// the server name", "the gate did not answer"), and the NWEC tag the session
	// or the host request named for it ("" for none). Never a secret.
	std::string reason;
	std::string tag;
	// In any state: the PlayerList slots (the remote admin's PLAYER LIST numbers) of the
	// players in the match the second listing's service sent, ascending; they stay in the
	// match when the listing ends.
	std::vector<int> slots;
};
bool operator==(const SecondListingReport &a, const SecondListingReport &b);
inline bool operator!=(const SecondListingReport &a, const SecondListingReport &b) {
	return !(a == b);
}

// The --second-listing-file's text, one JSON line: {"state": "<text>"}, with
// "try" and "tries" while trying or retrying, "next_try_at" (Unix seconds)
// while retrying, "reason" (every control character a space) with "tag" when
// there is one while retrying or not listed, and "slots" while there are any.
std::string second_listing_json(const SecondListingReport &report);

} // namespace opennova::serve
