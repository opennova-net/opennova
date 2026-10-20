#include "run_facts.h"

#include "normalization.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace opennova::parity {

namespace {

// Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar.
constexpr int64_t days_from_civil(int64_t y, int m, int d) {
	y -= m <= 2 ? 1 : 0;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const int64_t yoe = y - era * 400;
	const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}
static_assert(days_from_civil(1970, 1, 1) == 0);
static_assert(days_from_civil(2000, 3, 1) == 11017);

bool digits_at(std::string_view text, size_t pos, size_t count, int64_t &value) {
	if (pos + count > text.size())
		return false;
	value = 0;
	for (size_t i = 0; i < count; ++i) {
		const char c = text[pos + i];
		if (c < '0' || c > '9')
			return false;
		value = value * 10 + (c - '0');
	}
	return true;
}

bool file_exists(const std::string &path) {
	std::error_code ec;
	return !path.empty() && std::filesystem::is_regular_file(io::os_path(path), ec);
}

std::string generic(std::string path) {
	std::replace(path.begin(), path.end(), '\\', '/');
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	return path;
}

// The summary records absolute paths under its own run root; a run directory
// moved since keeps the same layout under its new root.
std::string resolve_capture(
		const std::string &recorded, const std::string &run_root, const std::string &run_dir) {
	if (file_exists(recorded))
		return recorded;
	const std::string path = generic(recorded);
	const std::string root = generic(run_root);
	if (!root.empty() && path.size() > root.size() && path[root.size()] == '/' &&
			strutil::iequals(std::string_view(path).substr(0, root.size()), root)) {
		const std::string rebased = generic(run_dir) + path.substr(root.size());
		if (file_exists(rebased))
			return rebased;
	}
	return {};
}

bool gameplay_identity(const wire::ParityEvent &event) {
	if (event.settings_update)
		return false;
	return (event.dir == 'C' && event.full_tag == 0x00C) ||
			(event.dir == 'S' && event.full_tag == 0x00A);
}

int bind_session(const wire::ParityTrace &trace, std::vector<EvidenceFinding> &evidence) {
	static_assert(has_normalization("trace.session"));
	std::map<int, size_t> gameplay;
	for (const wire::ParityEvent &event : trace.events)
		if (gameplay_identity(event))
			++gameplay[event.session];
	if (gameplay.size() == 1)
		return gameplay.begin()->first;
	if (gameplay.size() > 1) {
		auto busiest = std::max_element(gameplay.begin(), gameplay.end(),
				[](const auto &a, const auto &b) { return a.second < b.second; });
		std::string sessions;
		for (const auto &entry : gameplay)
			sessions += (sessions.empty() ? "" : ",") + std::to_string(entry.first);
		evidence.push_back({ "SESSION_AMBIGUOUS",
				"gameplay traffic rides more than one client port (" + sessions +
						"); the busiest, " + std::to_string(busiest->first) + ", is compared" });
		return busiest->first;
	}
	std::set<int> sessions;
	for (const wire::ParityPacket &packet : trace.packets)
		sessions.insert(packet.session);
	if (sessions.size() == 1)
		return *sessions.begin();
	evidence.push_back({ "NO_GAMEPLAY_SESSION",
			"the capture carries no C2S 0x0C / S2C 0x0A gameplay and " +
					std::to_string(sessions.size()) + " protocol sessions" });
	return sessions.empty() ? 0 : *sessions.begin();
}

template <typename T, typename Keep> void keep_if(std::vector<T> &records, Keep keep) {
	records.erase(std::remove_if(records.begin(), records.end(),
						  [&keep](const T &record) { return !keep(record); }),
			records.end());
}

constexpr uint64_t kBoundaryToleranceNs = 500000000ull; // a sample within 500 ms of each edge
constexpr uint64_t kMaximumGameplayGapNs = 1000000000ull; // and none further apart than 1 s

void check_gameplay_coverage(const RunFacts &facts, char dir, std::vector<EvidenceFinding> &out) {
	std::vector<uint64_t> samples;
	if (dir == 'C') {
		for (const wire::ParityUplinkState &state : facts.trace.uplinks)
			if (state.decoded && facts.in_steady(state.ts_ns))
				samples.push_back(state.ts_ns);
	} else {
		for (const wire::ParityFrameState &state : facts.trace.frames)
			if (state.decoded && facts.in_steady(state.ts_ns))
				samples.push_back(state.ts_ns);
	}
	// No samples at all is an absence the gameplay-cadence metric compares with RR
	// (a deploy hold carries no C2S uplink); a partial stream must cover the window.
	if (samples.empty())
		return;
	std::sort(samples.begin(), samples.end());
	const std::string label = dir == 'C' ? "C2S 0x0C" : "S2C 0x0A";
	if (samples.front() > facts.info.steady_start_ns + kBoundaryToleranceNs ||
			samples.back() + kBoundaryToleranceNs < facts.info.steady_end_ns)
		out.push_back({ "STEADY_BOUNDARY_GAMEPLAY_MISSING",
				label +
						" decoded gameplay has no sample within 500 ms of a steady "
						"window edge" });
	// E2's declared load gap: a run whose host rotates to its next mission stops
	// gameplay while that mission loads. The gap holding the next mission's
	// header (the run's second S2C 0x0B) is the load, not a stall
	// (window.rotation_load_gap).
	static_assert(has_normalization("window.rotation_load_gap"));
	uint64_t next_mission_ns = 0;
	if (facts.info.rotation) {
		int headers = 0;
		for (const wire::ParityEvent &event : facts.trace.events)
			if (event.dir == 'S' && event.full_tag == 0x00B && !event.settings_update && ++headers == 2) {
				next_mission_ns = event.ts_ns;
				break;
			}
	}
	uint64_t widest = 0;
	uint64_t previous = facts.info.steady_start_ns;
	for (const uint64_t ts : samples) {
		if (!(next_mission_ns > previous && next_mission_ns < ts))
			widest = std::max(widest, ts - previous);
		previous = ts;
	}
	widest = std::max(widest, facts.info.steady_end_ns - previous);
	if (widest > kMaximumGameplayGapNs) {
		char buf[160];
		std::snprintf(buf, sizeof(buf), " decoded gameplay has a %.1f ms gap in the steady window",
				double(widest) / 1e6);
		out.push_back({ "STEADY_GAMEPLAY_GAP", label + buf });
	}
}

void check_datagram_accounting(const RunFacts &facts, std::vector<EvidenceFinding> &out) {
	size_t bad = 0;
	std::string examples;
	std::vector<int> protocol_frames, packet_frames;
	for (const wire::ParityDatagram &datagram : facts.trace.datagrams) {
		if (!facts.in_comparison(datagram.ts_ns))
			continue;
		const bool invalid = datagram.datagram_class == CaptureDatagramClass::Invalid ||
				datagram.datagram_class == CaptureDatagramClass::Unknown;
		if (!datagram.outer_decoded || !datagram.decoded || invalid) {
			if (++bad <= 4)
				examples += (examples.empty() ? "" : ", ") + std::string("frame=") +
						std::to_string(datagram.frame) + "/" +
						wire::parity_datagram_class_name(datagram.datagram_class);
		}
		if (datagram.datagram_class == CaptureDatagramClass::ClientProtocol ||
				datagram.datagram_class == CaptureDatagramClass::ServerProtocol)
			protocol_frames.push_back(datagram.frame);
	}
	for (const wire::ParityPacket &packet : facts.trace.packets)
		if (facts.in_comparison(packet.ts_ns))
			packet_frames.push_back(packet.frame);
	std::sort(protocol_frames.begin(), protocol_frames.end());
	std::sort(packet_frames.begin(), packet_frames.end());
	if (bad != 0)
		out.push_back({ "RAW_DATAGRAM_DECODE_FAILED",
				std::to_string(bad) +
						" datagram(s) in the comparison interval were "
						"malformed, unknown or not decoded: " +
						examples });
	if (protocol_frames != packet_frames)
		out.push_back({ "DATAGRAM_PACKET_ACCOUNTING_MISMATCH",
				std::to_string(protocol_frames.size()) + " protocol datagrams vs " +
						std::to_string(packet_frames.size()) + " decoded packets" });
}

} // namespace

bool parse_utc_ns(std::string_view text, uint64_t &out) {
	int64_t year, month, day, hour, minute, second;
	if (text.size() < 20 || !digits_at(text, 0, 4, year) || text[4] != '-' ||
			!digits_at(text, 5, 2, month) || text[7] != '-' || !digits_at(text, 8, 2, day) ||
			text[10] != 'T' || !digits_at(text, 11, 2, hour) || text[13] != ':' ||
			!digits_at(text, 14, 2, minute) || text[16] != ':' || !digits_at(text, 17, 2, second))
		return false;
	size_t pos = 19;
	int64_t fraction_ns = 0;
	if (text[pos] == '.') {
		++pos;
		size_t count = 0;
		while (pos + count < text.size() && text[pos + count] >= '0' && text[pos + count] <= '9')
			++count;
		if (count == 0 || count > 9)
			return false;
		int64_t fraction = 0;
		digits_at(text, pos, count, fraction);
		for (size_t i = count; i < 9; ++i)
			fraction *= 10;
		fraction_ns = fraction;
		pos += count;
	}
	if (pos + 1 != text.size() || text[pos] != 'Z')
		return false;
	static const int kDays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	if (year < 1970 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 ||
			second > 59 || day > kDays[month - 1] || (month == 2 && day == 29 && !leap))
		return false;
	const int64_t days = days_from_civil(year, int(month), int(day));
	const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
	out = uint64_t(seconds) * 1000000000ull + uint64_t(fraction_ns);
	return true;
}

bool read_run_info(const std::string &run_dir, const std::string &label, EvidenceSource source,
		RunInfo &out, std::string &error) {
	out = RunInfo{};
	out.label = label;
	out.run_dir = run_dir;
	const std::string summary_path = io::utf8_join(run_dir, "run-summary.json");
	std::string text;
	if (!io::read_file_text(summary_path, text, error)) {
		error = label + ": " + error;
		return false;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) {
		error = label + ": " + summary_path + " is not a JSON object: " + parse_error;
		return false;
	}
	auto need_string = [&](const char *key, std::string &value) {
		const io::JsonValue *member = json.get(key);
		if (member && member->is_string() && !member->string.empty()) {
			value = member->string;
			return true;
		}
		error = label + ": " + summary_path + " has no " + key;
		return false;
	};
	std::string started, completed, recorded_capture;
	if (!need_string("topology", out.topology) || !need_string("steady_started_utc", started) ||
			!need_string("steady_completed_utc", completed) || !need_string("mission", out.mission))
		return false;
	if (source == EvidenceSource::Summary && !need_string("evidence_capture", recorded_capture))
		return false;
	out.run_id = json.get_string("run_id", "");
	out.perspective = json.get_string("evidence_perspective", "");
	out.integrity_profile = json.get_string("integrity_profile", "");
	const io::JsonValue *game_type = json.get("game_type");
	if (game_type && game_type->is_string()) {
		out.game_type = game_type->string;
	} else if (game_type && json_exact_whole(*game_type)) {
		out.game_type = std::to_string(static_cast<uint64_t>(game_type->number));
	}
	out.port = json.get_int("port", 0);
	const io::JsonValue *rotation = json.get("rotation");
	out.rotation = rotation && rotation->is_array() && !rotation->array.empty();
	out.steady_seconds = json.get_number("steady_seconds", 0.0);
	if (!(out.steady_seconds > 0.0) || out.steady_seconds > 3600.0) {
		error = label + ": steady_seconds must be a positive number of seconds";
		return false;
	}
	if (!parse_utc_ns(started, out.steady_start_ns) ||
			!parse_utc_ns(completed, out.steady_completed_ns)) {
		error = label + ": steady_started_utc / steady_completed_utc are not strict ISO-8601 UTC";
		return false;
	}
	out.steady_end_ns =
			out.steady_start_ns + static_cast<uint64_t>(std::llround(out.steady_seconds * 1e9));
	const std::string run_root = json.get_string("run_root", "");
	out.external_capture =
			resolve_capture(json.get_string("external_capture", ""), run_root, run_dir);
	if (out.external_capture.empty() &&
			file_exists(io::utf8_join(run_dir, "external.pcapng")))
		out.external_capture = io::utf8_join(run_dir, "external.pcapng");
	if (source == EvidenceSource::External) {
		out.capture = out.external_capture;
		out.perspective = "external";
	} else if (source == EvidenceSource::Late) {
		// E3: the late joiner's own capture and session; its steady window opens
		// at its readiness (it joins mid-window) and closes with the cell's.
		const io::JsonValue *late = json.get("late_joiner");
		if (!late || !late->is_object()) {
			error = label + ": " + summary_path + " records no late_joiner";
			return false;
		}
		recorded_capture = late->get_string("capture_path", "");
		out.capture = resolve_capture(recorded_capture, run_root, run_dir);
		out.session = static_cast<int>(late->get_number("client_port", 0.0));
		out.perspective = "late-joiner";
		uint64_t ready_ns = 0;
		if (out.session <= 0 || !parse_utc_ns(late->get_string("ready_utc", ""), ready_ns) ||
				ready_ns >= out.steady_end_ns) {
			error = label + ": late_joiner needs a client_port and a ready_utc inside the steady window";
			return false;
		}
		out.steady_start_ns = std::max(out.steady_start_ns, ready_ns);
		out.steady_seconds = double(out.steady_end_ns - out.steady_start_ns) / 1e9;
	} else {
		out.capture = resolve_capture(recorded_capture, run_root, run_dir);
	}
	if (out.capture.empty()) {
		error = label + ": the evidence capture is missing (" +
				(source == EvidenceSource::External ? std::string("external.pcapng")
													: recorded_capture) +
				")";
		return false;
	}
	return true;
}

RunFacts make_run_facts(RunInfo info, wire::ParityTrace trace) {
	RunFacts facts;
	facts.info = std::move(info);
	facts.session = facts.info.session != 0 ? facts.info.session : bind_session(trace, facts.evidence);
	const int session = facts.session;
	facts.trace = std::move(trace);
	wire::ParityTrace &t = facts.trace;
	keep_if(t.handshakes, [session](const auto &r) { return r.session == session; });
	keep_if(t.datagrams, [session](const wire::ParityDatagram &r) {
		return r.src_port == session || r.dst_port == session;
	});
	keep_if(t.packets, [session](const auto &r) { return r.session == session; });
	keep_if(t.events, [session](const auto &r) { return r.session == session; });
	keep_if(t.uplinks, [session](const auto &r) { return r.session == session; });
	keep_if(t.frames, [session](const auto &r) { return r.session == session; });
	keep_if(t.entities, [session](const auto &r) { return r.session == session; });

	static_assert(has_normalization("trace.ts_ns"));
	bool any = false;
	for (const wire::ParityPacket &packet : t.packets) {
		if (!any || packet.ts_ns < facts.first_packet_ns)
			facts.first_packet_ns = packet.ts_ns;
		any = true;
	}
	if (any && facts.info.steady_end_ns > facts.first_packet_ns)
		facts.comparison_ms = double(facts.info.steady_end_ns - facts.first_packet_ns) / 1e6;

	if (facts.info.steady_completed_ns < facts.info.steady_end_ns)
		facts.evidence.push_back({ "STEADY_WINDOW_TOO_SHORT",
				"the endpoints closed the steady window before "
				"steady_seconds elapsed" });
	bool steady_traffic = false;
	for (const wire::ParityPacket &packet : t.packets)
		steady_traffic = steady_traffic || facts.in_steady(packet.ts_ns);
	if (!steady_traffic) {
		facts.evidence.push_back({ "STEADY_WINDOW_EMPTY",
				"no session packet falls inside the steady window (is the "
				"capture from this run?)" });
	} else {
		check_gameplay_coverage(facts, 'C', facts.evidence);
		check_gameplay_coverage(facts, 'S', facts.evidence);
	}
	check_datagram_accounting(facts, facts.evidence);
	return facts;
}

void attach_external(RunFacts &facts, const wire::ParityTrace &external) {
	static_assert(has_normalization("trace.read_time"));
	std::vector<EvidenceFinding> ignored; // the evidence capture's binding is the one judged
	const int session = bind_session(external, ignored);
	facts.external_loaded = true;
	facts.external_events.clear();
	for (const wire::ParityEvent &event : external.events) {
		if (event.session != session || !facts.in_comparison(event.ts_ns))
			continue;
		facts.external_events.push_back({ event.dir,
				identity_of(static_cast<uint8_t>(event.full_tag & 0xFFu), event.settings_update),
				event.ts_ns });
	}
}

bool load_run_facts(const RunInfo &info, const wire::ItemClassMap *item_classes, RunFacts &out,
		std::string &error) {
	wire::ParityTrace trace;
	if (!wire::read_parity_trace(info.capture, item_classes, trace)) {
		error = info.label + ": cannot read the capture " + info.capture;
		return false;
	}
	const bool evidence_is_external = !info.external_capture.empty() &&
			generic(info.external_capture) == generic(info.capture);
	wire::ParityTrace external;
	if (evidence_is_external)
		external = trace;
	const bool external_read = evidence_is_external ||
			(!info.external_capture.empty() &&
					wire::read_parity_trace(info.external_capture, item_classes, external));
	out = make_run_facts(info, std::move(trace));
	if (external_read)
		attach_external(out, external);
	return true;
}

char datagram_direction(const wire::ParityDatagram &datagram, int session) {
	switch (datagram.datagram_class) {
		case CaptureDatagramClass::ClientHello:
		case CaptureDatagramClass::ClientAuth:
		case CaptureDatagramClass::ClientProtocol:
			return 'C';
		case CaptureDatagramClass::ServerHello:
		case CaptureDatagramClass::ServerAuth:
		case CaptureDatagramClass::ServerProtocol:
			return 'S';
		default:
			return datagram.src_port == session ? 'C' : 'S';
	}
}

} // namespace opennova::parity
