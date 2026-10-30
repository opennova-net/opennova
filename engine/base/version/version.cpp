#include <base/version/version.h>

#include <cstdio>

#include <base/io/strutil.h>

namespace opennova {

namespace {

constexpr size_t kMaxVersionText = 32;
constexpr uint32_t kMaxPart = 0xFFFFFFFFu;

} // namespace

std::optional<Version> Version::parse(std::string_view text) {
	if (text.empty() || text.size() > kMaxVersionText)
		return std::nullopt;
	Version version;
	size_t start = 0;
	while (true) {
		if (version.count == version.parts.size())
			return std::nullopt;
		const size_t dot = text.find('.', start);
		const std::string_view part = text.substr(
				start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
		// all_digits first: strtoull would take a blank, a sign or a "0x" too.
		if (!strutil::all_digits(part))
			return std::nullopt;
		const std::optional<unsigned long long> number = strutil::parse_ullong(std::string(part));
		if (!number || *number > kMaxPart)
			return std::nullopt;
		version.parts[version.count++] = static_cast<uint32_t>(*number);
		if (dot == std::string_view::npos)
			return version;
		start = dot + 1;
	}
}

std::string Version::text() const {
	std::string out;
	for (uint8_t i = 0; i < count; ++i) {
		if (i > 0)
			out += '.';
		out += std::to_string(parts[i]);
	}
	return out;
}

std::string Version::retail_text() const {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "V%u.%u.%u.%u", static_cast<unsigned>(parts[0]),
			static_cast<unsigned>(parts[1]), static_cast<unsigned>(parts[2]),
			static_cast<unsigned>(parts[3]));
	return buffer;
}

uint32_t Version::packed() const {
	uint32_t out = 0;
	for (const uint32_t part : parts) {
		if (part > 0xFF)
			return 0;
		out = (out << 8) | part;
	}
	return out;
}

bool Version::operator==(const Version &other) const {
	return count == other.count && parts == other.parts;
}

std::string build_date_text(int64_t unix_utc) {
	static const char *const kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
		"Sep", "Oct", "Nov", "Dec" };
	// Floor division, so a time before 1970 lands on its own day.
	int64_t days = unix_utc / 86400;
	int64_t seconds = unix_utc % 86400;
	if (seconds < 0) {
		seconds += 86400;
		--days;
	}
	// The proleptic Gregorian date of a day count from 1970-01-01 (Howard Hinnant's
	// civil_from_days), so no gmtime and no locale is involved.
	days += 719468;
	const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	const int64_t day_of_era = days - era * 146097;
	const int64_t year_of_era =
			(day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	const int64_t day_of_year =
			day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	const int64_t shifted_month = (5 * day_of_year + 2) / 153; // 0 is March
	const int64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
	const int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
	const int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%s %2d %lld %02d:%02d:%02d", kMonths[month - 1],
			static_cast<int>(day), static_cast<long long>(year), static_cast<int>(seconds / 3600),
			static_cast<int>(seconds / 60 % 60), static_cast<int>(seconds % 60));
	return buffer;
}

} // namespace opennova
