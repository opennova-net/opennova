#pragma once

// nw_replay's command line and playback core, kept out of main() so both halves
// are directly testable: parse_args is pure (it reports through an out-param
// instead of printing), and run_playback takes its capture reader, its send, and
// its clock as parameters so a test can inject failures that are impractical to
// stage with real sockets.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <pcapio/pcap_reader.h>

namespace opennova::replay {

struct Args {
	std::string pcap;
	std::string items; // items.def for the --validate motion check
	uint16_t control_port = 42000;
	double speed = 1.0;
	int max_gap_ms = 250;
	int spectator_timeout_ms = 120000;
	bool print_roles = false;
	bool validate = false;
	bool loop = false;
};

// Parse argv into `out`. Returns false with a one-line reason in `error` when an
// option is missing, unknown, or malformed; the caller prints it with usage.
//
// Strict on numbers on purpose. atoi/atof report failure as 0 and stop at the
// first bad character, so "--speed junk" silently meant 0 and "--max-gap-ms 5x"
// silently meant 5. Worse, "--speed nan" parsed to NaN and slipped through a
// `speed <= 0` guard (every comparison against NaN is false) into the pacing
// math, where int64_t(NaN) is undefined behavior. Every numeric option now
// requires the WHOLE token to convert to a finite in-range value.
bool parse_args(int argc, const char *const *argv, Args &out, std::string &error);

// What a playback pass actually did. Both counts used to be discarded, so a
// mid-file read error or a dead spectator socket still reported "playback
// complete" and exited 0.
struct PlaybackReport {
	size_t sent = 0;
	size_t send_failures = 0;
	bool read_ok = false;
	bool ok() const { return read_ok && send_failures == 0; }
};

// Walks the capture, invoking the callback per datagram; returns false on bad
// magic / IO (an early stop requested by the callback is NOT a failure).
using StreamFn = std::function<bool(const std::function<bool(const net::PcapDatagram &)> &)>;
// Is this datagram part of the spectator stream?
using SelectFn = std::function<bool(const net::PcapDatagram &)>;
// Send one datagram; < 0 means the send failed.
using SendFn = std::function<int(const net::PcapDatagram &)>;
// Wait until `ns` nanoseconds after playback start.
using SleepFn = std::function<void(int64_t)>;

// Nanoseconds of playback time a capture gap is worth: scaled by `speed`, then
// clamped to `max_gap_ms` so a long idle stretch in the capture doesn't stall
// the spectator. Exposed for testing the clamp/scale directly.
double paced_gap_ns(uint64_t gap_ns, double speed, int max_gap_ms);

// Paced playback of the selected datagrams. Deterministic given its callbacks:
// no sockets, no wall clock, no globals.
PlaybackReport run_playback(const StreamFn &stream, const SelectFn &select, const SendFn &send,
                            const SleepFn &sleep_until_ns, double speed, int max_gap_ms);

} // namespace opennova::replay
