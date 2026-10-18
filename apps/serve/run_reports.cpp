#include "run_reports.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace opennova::serve {

void TickStats::frame(int64_t start_ns, int64_t end_ns, int64_t deadline_ns) {
	if (!window_start_ns_)
		window_start_ns_ = start_ns;
	frame_ms_.push_back(static_cast<double>(std::max<int64_t>(end_ns - start_ns, 0)) / 1e6);
	if (start_ns > deadline_ns)
		++late_;
}

std::optional<TickMinute> TickStats::take_if_due(int64_t now_ns) {
	if (!window_start_ns_ || now_ns - *window_start_ns_ < kWindowNs)
		return std::nullopt;
	TickMinute out;
	out.frames = frame_ms_.size();
	out.late = late_;
	out.max_ms = frame_ms_.empty() ? 0.0 : *std::max_element(frame_ms_.begin(), frame_ms_.end());
	out.p50_ms = quantile(frame_ms_, 0.50);
	out.p99_ms = quantile(frame_ms_, 0.99);
	frame_ms_.clear();
	late_ = 0;
	window_start_ns_.reset();
	return out;
}

double quantile(std::vector<double> &values, double q) {
	if (values.empty())
		return 0.0;
	// The smallest value at least q of them do not exceed.
	const double rank = std::ceil(q * static_cast<double>(values.size()));
	const size_t at =
			static_cast<size_t>(std::clamp(rank, 1.0, static_cast<double>(values.size()))) - 1;
	std::nth_element(
			values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
	return values[at];
}

std::string tick_minute_json(const TickMinute &m, int64_t at_unix) {
	char text[192];
	std::snprintf(text, sizeof(text),
			"{\"at\":%lld,\"p50_ms\":%.2f,\"p99_ms\":%.2f,\"max_ms\":%.2f,"
			"\"late\":%llu,\"frames\":%llu}\n",
			static_cast<long long>(at_unix), m.p50_ms, m.p99_ms, m.max_ms,
			static_cast<unsigned long long>(m.late), static_cast<unsigned long long>(m.frames));
	return text;
}

std::string exit_file_text(const std::string &error) {
	std::string line = error;
	for (char &c : line) {
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
			c = ' ';
	}
	return line + "\n";
}

} // namespace opennova::serve
