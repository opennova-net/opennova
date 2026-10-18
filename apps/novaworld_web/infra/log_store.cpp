#include "log_store.h"

#include "health.h" // Watchdog::steady_now_ms
#include "infra_status.h"
#include "loggable.h"
#include "program_log.h"
#include "text_util.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::infra {

namespace fs = std::filesystem;

namespace {

using io::JsonValue;

constexpr const char *kLogsTarget = "Logs_20140328.";
// FilterLogEvents' page (its largest) and the pages one window may take before
// a narrower one is read instead.
constexpr int kFilterPage = 10000;
constexpr int kPagesPerWindow = 2;
// The narrowest window a dense log is read in.
constexpr int64_t kNarrowestWindowMs = 1000;
// A file's bytes read at a time, backwards.
constexpr int64_t kScanBlock = 64 * 1024;

int64_t wall_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string json_text(std::string_view text) {
	std::string out = "\"";
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out.push_back('\\');
			out.push_back(c);
		} else if (byte < 0x20) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
			out += escaped;
		} else {
			out.push_back(c);
		}
	}
	out.push_back('"');
	return out;
}

const JsonValue *member(const JsonValue &object, const char *key, JsonValue::Type type) {
	const JsonValue *v = object.is_object() ? object.get(key) : nullptr;
	return v != nullptr && v->type == type ? v : nullptr;
}

std::string text_of(const JsonValue &object, const char *key) {
	const JsonValue *v = member(object, key, JsonValue::Type::String);
	return v != nullptr ? v->string : std::string();
}

// `text` as a line's: its line break dropped, cut at kMaxLineBytes.
std::string line_text(std::string_view text) {
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.remove_suffix(1);
	if (text.size() > LogStore::kMaxLineBytes)
		text = text.substr(0, LogStore::kMaxLineBytes);
	return std::string(text);
}

// "<kind>:<value>"; false when `cursor` is none.
bool split_cursor(const std::string &cursor, char &kind, std::string &value) {
	if (cursor.size() < 3 || cursor.size() > 1024 || cursor[1] != ':')
		return false;
	kind = cursor[0];
	value = cursor.substr(2);
	return true;
}

std::optional<int64_t> cursor_number(const std::string &value) {
	if (value.empty() || value.size() > 18 || !strutil::all_digits(value))
		return std::nullopt;
	return strutil::parse_llong(value);
}

// A token AWS handed out (GetLogEvents' "b/<digits>/s"): letters, digits and / + = _ -.
bool is_token_char(char c) {
	return is_alnum(c) || c == '/' || c == '+' || c == '=' || c == '_' || c == '-';
}

// The events of a GetLogEvents or FilterLogEvents reply, in its order.
std::vector<LogLine> events_of(const JsonValue &reply) {
	std::vector<LogLine> out;
	if (const JsonValue *events = member(reply, "events", JsonValue::Type::Array))
		for (const JsonValue &e : events->array) {
			const JsonValue *at = member(e, "timestamp", JsonValue::Type::Number);
			LogLine line;
			line.at_ms = at != nullptr && std::isfinite(at->number)
					? static_cast<int64_t>(at->number)
					: -1;
			line.text = line_text(text_of(e, "message"));
			out.push_back(std::move(line));
		}
	return out;
}

bool reply_json(const AwsReply &reply, JsonValue &out, std::string &error) {
	if (!reply.ok()) {
		error = AwsApi::failure("logs", reply);
		return false;
	}
	std::string parse_error;
	if (!io::json_parse(reply.body, out, parse_error) || !out.is_object()) {
		error = "logs answered no JSON object";
		return false;
	}
	return true;
}

// Whether `reply` is AWS's word that the stream (or the group) is not there yet.
bool not_found(const AwsReply &reply) {
	return reply.outcome == AwsReply::Outcome::Answered && reply.status == 400 &&
			reply.body.find("ResourceNotFoundException") != std::string::npos;
}

// The newest `limit` of `lines` (oldest first, sorted by time) and the cursor of
// what is older: a millisecond the cut falls inside goes whole to the older page
// (so the next page, ending before it, loses none of it), unless every line kept
// shares it.
void keep_newest(std::vector<LogLine> &lines, size_t limit, LogPage &page) {
	const size_t drop = lines.size() - limit;
	const int64_t cut = lines[drop].at_ms;
	size_t first = drop;
	if (lines[drop - 1].at_ms == cut) {
		while (first < lines.size() && lines[first].at_ms == cut)
			++first;
		if (first == lines.size())
			first = drop;
	}
	page.before = "t:" + std::to_string(first == drop ? cut : cut + 1);
	page.lines.assign(std::make_move_iterator(lines.begin() + static_cast<std::ptrdiff_t>(first)),
			std::make_move_iterator(lines.end()));
}

// ---- reading a file backwards -----------------------------------------------------------

// How a backward scan ended.
struct ScanEnd {
	int64_t at = 0; // the offset older lines end before: the cursor
	bool reached_start = false;
	bool error = false;
};

// `path`'s lines that end before `end` (its size when -1), newest first, each with
// the offset it starts at, handed to `take` until it returns false (the scan stops
// before that line's older ones), the file's start, or kScanBytes read.
template <typename Take> ScanEnd scan_back(const fs::path &path, int64_t end, Take take) {
	ScanEnd done;
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		done.error = true;
		return done;
	}
	in.seekg(0, std::ios::end);
	const int64_t size = static_cast<int64_t>(in.tellg());
	int64_t top = end < 0 ? size : std::min(end, size);
	std::string buf; // the file's bytes [top - buf.size(), top)
	int64_t scanned = 0;
	if (top <= 0) {
		done.reached_start = true;
		return done;
	}
	// The line break ending the last line is no part of it.
	char last = 0;
	in.seekg(top - 1);
	in.read(&last, 1);
	if (last == '\n')
		--top;
	for (;;) {
		const size_t nl = buf.rfind('\n');
		const int64_t base = top - static_cast<int64_t>(buf.size());
		if (nl != std::string::npos || base == 0) {
			const size_t from = nl == std::string::npos ? 0 : nl + 1;
			const int64_t start = top - static_cast<int64_t>(buf.size() - from);
			if (!take(std::string_view(buf).substr(from), start)) {
				done.at = start;
				done.reached_start = start == 0;
				return done;
			}
			if (nl == std::string::npos) {
				done.reached_start = true;
				return done;
			}
			buf.resize(nl);
			top = start - 1;
			continue;
		}
		if (scanned >= DirectoryLogStore::kScanBytes) {
			done.at = top;
			return done;
		}
		const int64_t len = std::min(kScanBlock, base);
		std::string block(static_cast<size_t>(len), '\0');
		in.seekg(base - len);
		in.read(block.data(), len);
		if (in.gcount() != len) {
			done.error = true;
			return done;
		}
		scanned += len;
		buf.insert(0, block);
	}
}

bool holds(const std::string &text, const std::string &q) {
	return q.empty() || text.find(q) != std::string::npos;
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const auto yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

} // namespace

LogLine parse_stamped_line(std::string_view line) {
	LogLine out;
	// "2026-10-18T14:05:03.123Z "
	static constexpr char kShape[] = "dddd-dd-ddTdd:dd:dd.dddZ ";
	bool stamped = line.size() >= sizeof(kShape) - 1;
	for (size_t i = 0; stamped && i + 1 < sizeof(kShape); ++i)
		stamped = kShape[i] == 'd' ? (line[i] >= '0' && line[i] <= '9') : line[i] == kShape[i];
	if (!stamped) {
		out.text = line_text(line);
		return out;
	}
	const auto num = [&line](size_t at, size_t len) {
		int64_t v = 0;
		for (size_t i = at; i < at + len; ++i)
			v = v * 10 + (line[i] - '0');
		return v;
	};
	const int64_t month = num(5, 2);
	const int64_t day = num(8, 2);
	if (month < 1 || month > 12 || day < 1 || day > 31) {
		out.text = line_text(line);
		return out;
	}
	const int64_t days =
			days_from_civil(num(0, 4), static_cast<unsigned>(month), static_cast<unsigned>(day));
	out.at_ms = ((days * 24 + num(11, 2)) * 60 + num(14, 2)) * 60 * 1000 + num(17, 2) * 1000 +
			num(20, 3);
	out.text = line_text(line.substr(sizeof(kShape) - 1));
	return out;
}

// ---- CloudWatch Logs ------------------------------------------------------------------

CloudWatchLogStore::CloudWatchLogStore(proc::CommandRunner &runner, AwsApiConfig aws,
		std::string prefix, Clock wall_ms, Clock steady_ms) :
		api_(runner, std::move(aws)),
		prefix_(std::move(prefix)),
		wall_(wall_ms ? std::move(wall_ms) : Clock(wall_now_ms)),
		steady_(steady_ms ? std::move(steady_ms)
						  : Clock([] { return static_cast<int64_t>(Watchdog::steady_now_ms()); })) {
}

LogPage CloudWatchLogStore::read(const LogQuery &query) {
	const LogSource &s = query.source;
	const std::string key = std::to_string(static_cast<int>(s.subject)) + '\x1f' +
			std::to_string(s.id) + '\x1f' + std::to_string(s.generation) + '\x1f' + s.program +
			'\x1f' + s.task + '\x1f' + s.stream + '\x1f' + query.before + '\x1f' +
			std::to_string(query.limit) + '\x1f' + query.q;
	{
		std::lock_guard<std::mutex> lock(mu_);
		const int64_t now = steady_();
		for (auto it = cache_.begin(); it != cache_.end();)
			it = now - it->second.at >= kCacheMs ? cache_.erase(it) : std::next(it);
		if (const auto it = cache_.find(key); it != cache_.end())
			return it->second.page;
	}
	LogPage page = read_uncached(query);
	if (page.error.empty() && !page.bad_cursor) {
		std::lock_guard<std::mutex> lock(mu_);
		if (cache_.size() >= kCacheEntries) {
			auto oldest = cache_.begin();
			for (auto it = cache_.begin(); it != cache_.end(); ++it)
				if (it->second.at < oldest->second.at)
					oldest = it;
			cache_.erase(oldest);
		}
		cache_[key] = { steady_(), page };
	}
	return page;
}

LogPage CloudWatchLogStore::read_uncached(const LogQuery &query) {
	LogPage page;
	const LogSource &s = query.source;
	std::string group;
	std::string stream;
	std::string prefix;
	switch (s.subject) {
		case LogSource::Subject::Server:
			group = prefix_ + "/servers";
			stream = s.stream;
			break;
		case LogSource::Subject::Machine:
			group = prefix_ + "/machines";
			stream = s.stream;
			break;
		case LogSource::Subject::Program:
			// ECS's awslogs streams: <prefix>/<container>/<task id>, the program's
			// container named after it.
			group = prefix_ + "/service";
			if (s.task.empty())
				prefix = s.program + "/" + s.program + "/";
			else
				stream = s.program + "/" + s.program + "/" + s.task;
			break;
	}
	if (stream.empty() && prefix.empty())
		return page; // the row names no stream yet: nothing to read, and nowhere read
	page.group = std::move(group);
	page.stream = prefix.empty() ? stream : prefix;
	const bool filtered = !query.q.empty() || !prefix.empty();
	if (!query.before.empty()) {
		char kind = 0;
		std::string value;
		const bool ok = split_cursor(query.before, kind, value) &&
				(filtered ? kind == 't' && cursor_number(value).has_value()
						  : kind == 'g' && all_of(value, is_token_char));
		if (!ok) {
			page.bad_cursor = true;
			return page;
		}
	}
	AwsCredentials creds;
	if (!api_.credentials(creds, page.error))
		return page;
	if (filtered)
		read_filtered(creds, page.group, stream, prefix, query, page);
	else
		read_stream(creds, page.group, stream, query, page);
	return page;
}

void CloudWatchLogStore::read_stream(const AwsCredentials &creds, const std::string &group,
		const std::string &stream, const LogQuery &query, LogPage &page) {
	std::string token = query.before.empty() ? std::string() : query.before.substr(2);
	const int64_t until = steady_() + kDeadlineMs;
	for (int call = 0; call < kMaxCalls && steady_() < until; ++call) {
		std::string body = "{\"logGroupName\":" + json_text(group) +
				",\"logStreamName\":" + json_text(stream) +
				",\"startFromHead\":false,\"limit\":" + std::to_string(query.limit);
		if (!token.empty())
			body += ",\"nextToken\":" + json_text(token);
		body += "}";
		const AwsReply reply =
				api_.json_call(creds, "logs", std::string(kLogsTarget) + "GetLogEvents", body);
		if (not_found(reply)) {
			page.before.clear(); // no stream yet: nothing older either
			return;
		}
		JsonValue json;
		if (!reply_json(reply, json, page.error))
			return;
		std::vector<LogLine> lines = events_of(json);
		const std::string back = text_of(json, "nextBackwardToken");
		// GetLogEvents hands the same token back once nothing is older.
		const bool more = !back.empty() && back != token && all_of(back, is_token_char);
		if (!lines.empty()) {
			page.lines = std::move(lines);
			page.before = more ? "g:" + back : std::string();
			return;
		}
		if (!more) {
			page.before.clear();
			return;
		}
		// A stretch with no event: on to the one before it.
		token = back;
	}
	page.before = token.empty() ? std::string() : "g:" + token;
}

void CloudWatchLogStore::read_filtered(const AwsCredentials &creds, const std::string &group,
		const std::string &stream, const std::string &prefix, const LogQuery &query,
		LogPage &page) {
	const int64_t now = wall_();
	const int64_t horizon = now - kHorizonMs;
	int64_t end = now + 1; // exclusive
	if (!query.before.empty())
		end = *cursor_number(query.before.substr(2));
	int64_t window = kFirstWindowMs;
	std::vector<LogLine> collected; // oldest first
	bool exhausted = false;
	int calls = 0;
	const int64_t until = steady_() + kDeadlineMs;
	const auto budget_left = [&] { return calls < kMaxCalls && steady_() < until; };

	while (collected.size() < query.limit) {
		if (end <= horizon) {
			exhausted = true;
			break;
		}
		if (!budget_left())
			break;
		const int64_t start = std::max(end - window, horizon);
		std::vector<LogLine> chunk;
		std::string token;
		bool whole = false;
		for (int pages = 0; pages < kPagesPerWindow && budget_left(); ++pages) {
			std::string body = "{\"logGroupName\":" + json_text(group);
			if (prefix.empty())
				body += ",\"logStreamNames\":[" + json_text(stream) + "]";
			else
				body += ",\"logStreamNamePrefix\":" + json_text(prefix);
			// endTime takes the events at it: one before our exclusive end.
			body += ",\"startTime\":" + std::to_string(start) +
					",\"endTime\":" + std::to_string(end - 1) +
					",\"limit\":" + std::to_string(kFilterPage);
			if (!query.q.empty())
				body += ",\"filterPattern\":" + json_text("\"" + query.q + "\"");
			if (!token.empty())
				body += ",\"nextToken\":" + json_text(token);
			body += "}";
			++calls;
			const AwsReply reply = api_.json_call(
					creds, "logs", std::string(kLogsTarget) + "FilterLogEvents", body);
			if (not_found(reply)) {
				page.lines.clear();
				page.before.clear();
				return;
			}
			JsonValue json;
			if (!reply_json(reply, json, page.error))
				return;
			std::vector<LogLine> got = events_of(json);
			chunk.insert(chunk.end(), std::make_move_iterator(got.begin()),
					std::make_move_iterator(got.end()));
			token = text_of(json, "nextToken");
			if (token.empty()) {
				whole = true;
				break;
			}
		}
		if (!whole) {
			// More in the window than a read takes: its newer end, narrower, while
			// calls are left to read it.
			const bool narrower = end - start > kNarrowestWindowMs;
			if (narrower && budget_left()) {
				window = std::max((end - start) / 8, kNarrowestWindowMs);
				continue;
			}
			// The narrowest window, or the last this read can take. A page with lines
			// stops before it (or, at the narrowest, takes what was read of it); a page
			// with none yet takes what was read of it, its oldest lines (perhaps none),
			// and goes on past it, so its cursor always moves back.
			if (!collected.empty() && (narrower || chunk.empty()))
				break;
		}
		std::stable_sort(chunk.begin(), chunk.end(),
				[](const LogLine &a, const LogLine &b) { return a.at_ms < b.at_ms; });
		collected.insert(collected.begin(), std::make_move_iterator(chunk.begin()),
				std::make_move_iterator(chunk.end()));
		end = start;
		window *= 4;
	}

	if (collected.size() > query.limit) {
		keep_newest(collected, query.limit, page);
		return;
	}
	page.lines = std::move(collected);
	page.before = exhausted ? std::string() : "t:" + std::to_string(end);
}

// ---- a directory ----------------------------------------------------------------------

LogPage DirectoryLogStore::read(const LogQuery &query) {
	LogPage page;
	page.group = dir_;
	const LogSource &s = query.source;
	std::string relative;
	bool every_task = false;
	switch (s.subject) {
		case LogSource::Subject::Server:
			relative = "servers/" + std::to_string(s.id) + "/g" + std::to_string(s.generation) +
					".log";
			break;
		case LogSource::Subject::Machine:
			relative = "machines/" + std::to_string(s.id) + "/hostd.log";
			break;
		case LogSource::Subject::Program:
			every_task = s.task.empty();
			relative = every_task ? "programs/" + s.program + "/"
								  : program::program_log_path(s.program, s.task);
			break;
	}
	page.stream = relative;
	const fs::path root = fs::u8path(dir_);
	char kind = 0;
	std::string value;
	std::optional<int64_t> cursor;
	if (!query.before.empty()) {
		if (!split_cursor(query.before, kind, value) || kind != (every_task ? 't' : 'o') ||
				!(cursor = cursor_number(value))) {
			page.bad_cursor = true;
			return page;
		}
	}

	if (!every_task) {
		const fs::path file = root / fs::u8path(relative);
		std::error_code ec;
		if (!fs::exists(file, ec))
			return page; // nothing written yet
		std::vector<LogLine> newest_first;
		int64_t oldest_start = -1;
		const ScanEnd end =
				scan_back(file, cursor.value_or(-1), [&](std::string_view line, int64_t start) {
					LogLine parsed = parse_stamped_line(line);
					if (!holds(parsed.text, query.q))
						return true;
					newest_first.push_back(std::move(parsed));
					oldest_start = start;
					return newest_first.size() < query.limit;
				});
		if (end.error) {
			page.error = "cannot read " + relative;
			return page;
		}
		page.lines.assign(std::make_move_iterator(newest_first.rbegin()),
				std::make_move_iterator(newest_first.rend()));
		const int64_t older = newest_first.size() >= query.limit ? oldest_start : end.at;
		page.before =
				end.reached_start || older <= 0 ? std::string() : "o:" + std::to_string(older);
		return page;
	}

	// Every task's file of the program, merged by time: each line older than the
	// cursor, the newest `limit` (and one more, to know of the older) of each file
	// at most.
	const fs::path dir = root / fs::u8path(relative);
	std::error_code ec;
	if (!fs::is_directory(dir, ec))
		return page;
	std::vector<LogLine> merged;
	bool all_read = true;
	const int64_t end_ms = cursor.value_or(INT64_MAX);
	for (const fs::directory_entry &e : fs::directory_iterator(dir, ec)) {
		if (!e.is_regular_file(ec) || e.path().extension() != ".log")
			continue;
		std::vector<LogLine> newest_first;
		const ScanEnd end = scan_back(e.path(), -1, [&](std::string_view line, int64_t) {
			LogLine parsed = parse_stamped_line(line);
			// A line with no time has no place among the others'.
			if (parsed.at_ms < 0 || parsed.at_ms >= end_ms || !holds(parsed.text, query.q))
				return true;
			newest_first.push_back(std::move(parsed));
			return newest_first.size() <= query.limit;
		});
		if (end.error) {
			page.error = "cannot read " + relative + e.path().filename().u8string();
			return page;
		}
		all_read = all_read && end.reached_start;
		// In the file's order, so lines of one millisecond keep theirs.
		merged.insert(merged.end(), std::make_move_iterator(newest_first.rbegin()),
				std::make_move_iterator(newest_first.rend()));
	}
	std::stable_sort(merged.begin(), merged.end(),
			[](const LogLine &a, const LogLine &b) { return a.at_ms < b.at_ms; });
	if (merged.size() > query.limit) {
		keep_newest(merged, query.limit, page);
		return page;
	}
	if (!all_read && !merged.empty())
		page.before = "t:" + std::to_string(merged.front().at_ms);
	page.lines = std::move(merged);
	return page;
}

std::unique_ptr<LogStore> make_log_store(
		proc::CommandRunner &runner, const std::string &scratch_dir, std::string &why) {
	std::string error;
	const program::LogStoreSetting setting = program::log_store_from_env(error);
	switch (setting.kind) {
		case program::LogStoreSetting::Kind::None:
			why = error.empty() ? std::string(program::kLogStoreVariable) + " is unset" : error;
			return nullptr;
		case program::LogStoreSetting::Kind::Directory:
			return std::make_unique<DirectoryLogStore>(setting.location);
		case program::LogStoreSetting::Kind::CloudWatch:
			break;
	}
	const InfraConfig infra = infra_config_from_env(scratch_dir);
	if (const std::string bad = AwsApi::check_config(infra.aws); !bad.empty()) {
		why = "CloudWatch Logs is not read here: " + bad;
		return nullptr;
	}
	return std::make_unique<CloudWatchLogStore>(runner, infra.aws, setting.location);
}

} // namespace opennova::novaworld_server::infra
