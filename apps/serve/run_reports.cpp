#include "run_reports.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace opennova::serve {

TickStats::TickStats() : bins_(static_cast<size_t>(kRangeNs / kBinNs) + 1, 0) {}

void TickStats::frame(int64_t start_ns, int64_t end_ns, int64_t deadline_ns) {
	if (!window_start_ns_)
		window_start_ns_ = start_ns;
	const int64_t work_ns = std::max<int64_t>(end_ns - start_ns, 0);
	++bins_[static_cast<size_t>(std::min(work_ns, kRangeNs) / kBinNs)];
	++frames_;
	max_ns_ = std::max(max_ns_, work_ns);
	if (start_ns > deadline_ns)
		++late_;
}

double TickStats::quantile_ms(double q) const {
	if (frames_ == 0)
		return 0.0;
	// The smallest value at least q of them do not exceed: the bin the rank falls in.
	const double rank = std::clamp(
			std::ceil(q * static_cast<double>(frames_)), 1.0, static_cast<double>(frames_));
	uint64_t below = 0;
	for (size_t bin = 0; bin < bins_.size(); ++bin) {
		below += bins_[bin];
		if (static_cast<double>(below) >= rank) {
			const int64_t ns =
					bin + 1 == bins_.size() ? max_ns_ : static_cast<int64_t>(bin) * kBinNs;
			return static_cast<double>(std::min(ns, max_ns_)) / 1e6;
		}
	}
	return static_cast<double>(max_ns_) / 1e6;
}

std::optional<TickMinute> TickStats::take_if_due(int64_t now_ns) {
	if (!window_start_ns_ || now_ns - *window_start_ns_ < kWindowNs)
		return std::nullopt;
	TickMinute out;
	out.frames = frames_;
	out.late = late_;
	out.max_ms = static_cast<double>(max_ns_) / 1e6;
	out.p50_ms = quantile_ms(0.50);
	out.p99_ms = quantile_ms(0.99);
	std::fill(bins_.begin(), bins_.end(), 0u);
	frames_ = 0;
	max_ns_ = 0;
	late_ = 0;
	window_start_ns_.reset();
	return out;
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

namespace {

// `text` as a JSON string: quoted, its quotes and backslashes escaped, every
// control character a space.
std::string json_text(const std::string &text) {
	std::string out = "\"";
	for (const char c : text) {
		if (c == '"' || c == '\\')
			out += '\\';
		out += static_cast<unsigned char>(c) < 0x20 || c == 0x7F ? ' ' : c;
	}
	return out + "\"";
}

} // namespace

const char *second_listing_state_text(SecondListingState state) {
	switch (state) {
		case SecondListingState::Off:
			return "off";
		case SecondListingState::Trying:
			return "trying";
		case SecondListingState::Listed:
			return "listed";
		case SecondListingState::Retrying:
			return "retrying";
		case SecondListingState::NotListed:
			return "not_listed";
	}
	return "off";
}

bool operator==(const SecondListingReport &a, const SecondListingReport &b) {
	return a.state == b.state && a.attempt == b.attempt && a.attempts == b.attempts &&
			a.next_try_at == b.next_try_at && a.reason == b.reason && a.tag == b.tag &&
			a.slots == b.slots;
}

std::string second_listing_json(const SecondListingReport &r) {
	using State = SecondListingState;
	std::string out = std::string("{\"state\":\"") + second_listing_state_text(r.state) + "\"";
	if (r.state == State::Trying || r.state == State::Retrying)
		out += ",\"try\":" + std::to_string(r.attempt) + ",\"tries\":" + std::to_string(r.attempts);
	if (r.state == State::Retrying)
		out += ",\"next_try_at\":" + std::to_string(r.next_try_at);
	if (r.state == State::Retrying || r.state == State::NotListed) {
		out += ",\"reason\":" + json_text(r.reason);
		if (!r.tag.empty())
			out += ",\"tag\":" + json_text(r.tag);
	}
	if (!r.slots.empty()) {
		out += ",\"slots\":[";
		for (size_t i = 0; i < r.slots.size(); ++i)
			out += (i == 0 ? "" : ",") + std::to_string(r.slots[i]);
		out += "]";
	}
	return out + "}\n";
}

std::string build_file_json(const OneBuild &build) {
	return "{\"version\":" + json_text(build.version.text()) +
			",\"commit\":" + json_text(build.commit) + "}\n";
}

} // namespace opennova::serve
