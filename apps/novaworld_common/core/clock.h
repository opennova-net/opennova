#pragma once

// The service's clocks and a Unix time's text, one copy for every program (hostd too, which
// links the core): the wall clock in Unix milliseconds and seconds, a steady clock in
// milliseconds (for spans and timers, never a date), and a Unix time as ISO 8601 UTC.
// Platform plumbing, not a port.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace opennova::novaworld_server {

// The wall clock, Unix milliseconds.
inline int64_t wall_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// The wall clock, Unix seconds.
inline int64_t wall_now_s() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// A steady clock in milliseconds, from an unspecified start.
inline uint64_t steady_now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// A Unix time in seconds as ISO 8601 UTC, "2025-10-09T08:53:20Z", from its civil date, so every
// platform spells it alike.
inline std::string iso_utc(int64_t unix_seconds) {
	// The civil date of a day count (days since 1970-01-01), the proleptic
	// Gregorian calendar's.
	int64_t days = unix_seconds / 86400;
	int64_t secs = unix_seconds % 86400;
	if (secs < 0) {
		secs += 86400;
		--days;
	}
	days += 719468;
	const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	const int64_t doe = days - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int64_t day = doy - (153 * mp + 2) / 5 + 1;
	const int64_t month = mp < 10 ? mp + 3 : mp - 9;
	const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
	char text[96]; // room for any year the arithmetic can reach
	std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
			static_cast<long long>(year), static_cast<long long>(month),
			static_cast<long long>(day), static_cast<long long>(secs / 3600),
			static_cast<long long>(secs % 3600 / 60), static_cast<long long>(secs % 60));
	return text;
}

} // namespace opennova::novaworld_server
