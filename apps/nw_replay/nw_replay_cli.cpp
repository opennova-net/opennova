#include "nw_replay_cli.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace opennova::replay {

namespace {

// Whole-token strict conversions. `end == s` catches "no digits at all",
// `*end != '\0'` catches a trailing tail ("5x", "1.0 "), ERANGE catches
// overflow, and isfinite catches "nan"/"inf" — which strtod accepts happily.
bool parse_double_opt(const char *s, const char *what, double &out, std::string &error) {
	char *end = nullptr;
	errno = 0;
	const double v = std::strtod(s, &end);
	if (end == s || *end != '\0' || errno == ERANGE || !std::isfinite(v)) {
		error = std::string(what) + ": expected a finite number, got '" + s + "'";
		return false;
	}
	out = v;
	return true;
}

bool parse_int_opt(const char *s, const char *what, long lo, long hi, long &out,
                   std::string &error) {
	char *end = nullptr;
	errno = 0;
	const long v = std::strtol(s, &end, 10);
	if (end == s || *end != '\0' || errno == ERANGE || v < lo || v > hi) {
		error = std::string(what) + ": expected an integer in [" + std::to_string(lo) + ", " +
		        std::to_string(hi) + "], got '" + s + "'";
		return false;
	}
	out = v;
	return true;
}

} // namespace

bool parse_args(int argc, const char *const *argv, Args &out, std::string &error) {
	Args a;
	for (int i = 1; i < argc; ++i) {
		const std::string s = argv[i];
		auto next = [&](const char *what) -> const char * {
			if (i + 1 >= argc) {
				error = std::string("missing value for ") + what;
				return nullptr;
			}
			return argv[++i];
		};
		if (s == "--items") {
			const char *v = next("--items");
			if (!v) return false;
			a.items = v;
		} else if (s == "--control-port") {
			const char *v = next("--control-port");
			if (!v) return false;
			long n = 0;
			if (!parse_int_opt(v, "--control-port", 1, 65535, n, error)) return false;
			a.control_port = static_cast<uint16_t>(n);
		} else if (s == "--speed") {
			const char *v = next("--speed");
			if (!v) return false;
			double d = 0.0;
			if (!parse_double_opt(v, "--speed", d, error)) return false;
			if (d <= 0.0) {
				error = std::string("--speed: must be greater than 0, got '") + v + "'";
				return false;
			}
			a.speed = d;
		} else if (s == "--max-gap-ms") {
			const char *v = next("--max-gap-ms");
			if (!v) return false;
			long n = 0;
			if (!parse_int_opt(v, "--max-gap-ms", 0, 3600000, n, error)) return false;
			a.max_gap_ms = static_cast<int>(n);
		} else if (s == "--spectator-timeout-ms") {
			const char *v = next("--spectator-timeout-ms");
			if (!v) return false;
			long n = 0;
			if (!parse_int_opt(v, "--spectator-timeout-ms", 0, 86400000, n, error)) return false;
			a.spectator_timeout_ms = static_cast<int>(n);
		} else if (s == "--print-roles") {
			a.print_roles = true;
		} else if (s == "--validate") {
			a.validate = true;
		} else if (s == "--loop") {
			a.loop = true;
		} else if (!s.empty() && s[0] == '-') {
			error = "unknown flag " + s;
			return false;
		} else if (a.pcap.empty()) {
			a.pcap = s;
		} else {
			error = "unexpected arg " + s;
			return false;
		}
	}
	if (a.pcap.empty()) {
		error = "no capture file given";
		return false;
	}
	out = a;
	return true;
}

double paced_gap_ns(uint64_t gap_ns, double speed, int max_gap_ms) {
	double gap_ms = static_cast<double>(gap_ns) / 1.0e6 / speed;
	if (gap_ms > static_cast<double>(max_gap_ms)) {
		gap_ms = static_cast<double>(max_gap_ms);
	}
	return gap_ms * 1.0e6;
}

PlaybackReport run_playback(const StreamFn &stream, const SelectFn &select, const SendFn &send,
                            const SleepFn &sleep_until_ns, double speed, int max_gap_ms) {
	PlaybackReport report;
	uint64_t prev_ts = 0;
	bool first = true;
	double sched_ns = 0.0;
	report.read_ok = stream([&](const net::PcapDatagram &d) -> bool {
		if (!select(d)) return true;
		if (first) {
			first = false;
			prev_ts = d.ts_nanos;
		}
		const uint64_t gap = d.ts_nanos >= prev_ts ? d.ts_nanos - prev_ts : 0;
		sched_ns += paced_gap_ns(gap, speed, max_gap_ms);
		prev_ts = d.ts_nanos;
		sleep_until_ns(static_cast<int64_t>(sched_ns));
		if (send(d) < 0) {
			++report.send_failures;
		} else {
			++report.sent;
		}
		return true;
	});
	return report;
}

} // namespace opennova::replay
