// The logs the admin's pages read (apps/novaworld_web/infra/log_store.h), no database and no
// network:
//
//   - CloudWatch Logs behind a scripted CommandRunner answering as AWS would (each call's
//     curl config file read: argv exactly {curl, -q, --config, <file>}, no credential on it):
//     a server's start and a machine's hostd by the stream their rows name (GetLogEvents
//     backwards from the newest, the bodies' group, stream, limit and token; the token handed
//     back once nothing is older ending the paging; a stretch with no event read past; a
//     stream not there yet read as empty; a row naming none read as nothing, no group or
//     stream named), a program's task by its ECS stream, a program's every task and a text
//     filter through FilterLogEvents' windows walking back (each window's times, the prefix
//     or the stream, the quoted pattern; a millisecond the page's cut falls inside left whole
//     to the older page; a window too dense read narrower, and one denser than the calls
//     reach taken as far as read, the cursor always moving back; the calls bounded), a
//     cursor of the other kind refused, AWS's error in words and no credential in it, and
//     the short cache (a read like one 3 s back calls nothing);
//   - a directory: a file's newest lines backwards from its end by pages, the times read off
//     the programs' stamps (a line with none kept, its time null), a text filter, a file not
//     written yet empty, every task's file of a program merged by time and paged by it, a
//     cursor of the other kind refused;
//   - the stamp: its time to the millisecond, a line without one whole.

#include "infra/log_store.h"

#include "infra_aws_stub.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <base/io/file_io.h>
#include <base/io/json.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace infra = opennova::novaworld_server::infra;
namespace io = opennova::io;
namespace fs = std::filesystem;
using infra_stub::answer;
using opennova::proc::CommandResult;

namespace {

// 2026-10-18T12:00:00Z in milliseconds.
constexpr int64_t kNoonMs = 1792324800LL * 1000;

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

struct Call {
	std::vector<std::string> argv;
	std::string key;
	std::string body;
	io::JsonValue json;
};

// AWS behind curl: each call's config file read, its name and JSON body handed to
// `answer` (the credentials answered by itself).
class ScriptedAws : public opennova::proc::CommandRunner {
public:
	std::function<CommandResult(const Call &)> answer_call;

	CommandResult run(const std::vector<std::string> &argv, int) override {
		Call call;
		call.argv = argv;
		std::string config;
		if (argv.size() == 4) {
			std::ifstream in(fs::u8path(argv[3]), std::ios::binary);
			config.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		call.key = infra_stub::key_of(config);
		call.body = infra_stub::config_line(config, "data-raw");
		std::string error;
		io::json_parse(call.body, call.json, error);
		std::lock_guard<std::mutex> lock(mu_);
		calls.push_back(call);
		if (call.key == "credentials")
			return answer(200, infra_stub::kCredentials);
		return answer_call ? answer_call(call) : infra_stub::curl_failed(7, "no answer");
	}

	size_t count(const std::string &key) {
		std::lock_guard<std::mutex> lock(mu_);
		return static_cast<size_t>(std::count_if(
				calls.begin(), calls.end(), [&](const Call &c) { return c.key == key; }));
	}

	std::vector<Call> calls;

private:
	std::mutex mu_;
};

opennova::novaworld_server::infra::AwsApiConfig aws_config(const std::string &scratch) {
	opennova::novaworld_server::infra::AwsApiConfig c;
	c.region = "us-east-1";
	c.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
	c.scratch_dir = scratch;
	return c;
}

std::string events_json(const std::vector<std::pair<int64_t, std::string>> &events) {
	std::string out = "[";
	for (size_t i = 0; i < events.size(); ++i) {
		if (i != 0)
			out += ",";
		out += "{\"timestamp\":" + std::to_string(events[i].first) + ",\"message\":\"" +
				events[i].second + "\\n\",\"ingestionTime\":1}";
	}
	return out + "]";
}

int64_t number(const io::JsonValue &json, const char *key) {
	const io::JsonValue *v = json.get(key);
	return v != nullptr && v->is_number() ? static_cast<int64_t>(v->number) : -1;
}

std::string text(const io::JsonValue &json, const char *key) {
	return json.get_string(key, "");
}

// A FilterLogEvents page's cursor ("t:<ms>") as its millisecond; -1 for none.
int64_t cursor_ms(const std::string &before) {
	return before.rfind("t:", 0) == 0 ? std::stoll(before.substr(2)) : -1;
}

int test_cloudwatch_stream() {
	test_temp::TempDir scratch("log_store_stream");
	ScriptedAws aws;
	int64_t steady = 0;
	infra::CloudWatchLogStore store(
			aws, aws_config(scratch.root()), "/opennova/prod", [] { return kNoonMs; },
			[&steady] { return steady; });
	aws.answer_call = [](const Call &c) {
		if (c.key != "logs GetLogEvents")
			return infra_stub::curl_failed(7, "unexpected " + c.key);
		const std::string token = text(c.json, "nextToken");
		if (token.empty())
			return answer(200,
					"{\"events\":" +
							events_json({ { kNoonMs - 2000, "serving on UDP 32768" },
									{ kNoonMs - 1000, "a player joined" } }) +
							",\"nextForwardToken\":\"f/2\",\"nextBackwardToken\":\"b/1/s\"}");
		if (token == "b/1/s") // a stretch with nothing in it
			return answer(200, "{\"events\":[],\"nextBackwardToken\":\"b/0/s\"}");
		if (token == "b/0/s")
			return answer(200,
					"{\"events\":" + events_json({ { kNoonMs - 9000, "booting" } }) +
							",\"nextBackwardToken\":\"b/0/s\"}");
		return answer(400, "{\"__type\":\"InvalidParameterException\",\"message\":\"bad token\"}");
	};

	infra::LogQuery q;
	q.source.subject = infra::LogSource::Subject::Server;
	q.source.id = 12;
	q.source.generation = 3;
	q.source.stream = "12/g3";
	q.limit = 200;
	const infra::LogPage first = store.read(q);
	TEST_EXPECT(first.error.empty() && !first.bad_cursor);
	TEST_EXPECT(first.group == "/opennova/prod/servers" && first.stream == "12/g3");
	TEST_EXPECT(first.lines.size() == 2 && first.lines[0].text == "serving on UDP 32768" &&
			first.lines[0].at_ms == kNoonMs - 2000 && first.lines[1].text == "a player joined");
	TEST_EXPECT(first.before == "g:b/1/s");
	// The call: the stream's newest, its limit; the credentials in the file, never argv.
	const Call &call = aws.calls.back();
	TEST_EXPECT(call.argv.size() == 4 && call.argv[1] == "-q" && call.argv[2] == "--config");
	for (const std::string &arg : call.argv)
		TEST_EXPECT(!contains(arg, infra_stub::kSecret) && !contains(arg, infra_stub::kKeyId));
	TEST_EXPECT(text(call.json, "logGroupName") == "/opennova/prod/servers" &&
			text(call.json, "logStreamName") == "12/g3" && number(call.json, "limit") == 200 &&
			!call.json.get("nextToken") && contains(call.body, "\"startFromHead\":false"));

	// Older: an empty stretch read past, then the oldest, its token standing still.
	q.before = first.before;
	const infra::LogPage older = store.read(q);
	TEST_EXPECT(
			older.lines.size() == 1 && older.lines[0].text == "booting" && older.before.empty());
	TEST_EXPECT(aws.count("logs GetLogEvents") == 3);

	// The cache: the newest again within 3 s calls nothing; past it, AWS again.
	q.before.clear();
	steady += 2000;
	TEST_EXPECT(store.read(q).lines.size() == 2 && aws.count("logs GetLogEvents") == 3);
	steady += 1500;
	TEST_EXPECT(store.read(q).lines.size() == 2 && aws.count("logs GetLogEvents") == 4);

	// A machine's hostd, a program's task.
	q.source.subject = infra::LogSource::Subject::Machine;
	q.source.id = 4;
	q.source.stream = "hostd/hostd/8e1f";
	TEST_EXPECT(store.read(q).group == "/opennova/prod/machines" &&
			text(aws.calls.back().json, "logStreamName") == "hostd/hostd/8e1f");
	q.source.subject = infra::LogSource::Subject::Program;
	q.source.program = "web";
	q.source.task = "0123456789abcdef0123456789abcdef";
	q.source.stream.clear();
	const infra::LogPage task = store.read(q);
	TEST_EXPECT(task.group == "/opennova/prod/service" &&
			task.stream == "web/web/0123456789abcdef0123456789abcdef" &&
			text(aws.calls.back().json, "logStreamName") == task.stream);

	// No stream named yet: nothing called, and no group or stream named either.
	const size_t before_calls = aws.calls.size();
	q.source.subject = infra::LogSource::Subject::Server;
	q.source.stream.clear();
	const infra::LogPage none = store.read(q);
	TEST_EXPECT(none.lines.empty() && none.before.empty() && none.error.empty() &&
			aws.calls.size() == before_calls);
	TEST_EXPECT(none.group.empty() && none.stream.empty());

	// A cursor of the other kind, a token that is none: refused before AWS.
	q.source.stream = "12/g3";
	q.before = "t:123";
	TEST_EXPECT(store.read(q).bad_cursor && aws.calls.size() == before_calls);
	q.before = "g:b/1/s\"}";
	TEST_EXPECT(store.read(q).bad_cursor && aws.calls.size() == before_calls);

	// A stream not there yet: empty; AWS's error in words, no credential.
	aws.answer_call = [](const Call &) {
		return answer(400,
				"{\"__type\":\"ResourceNotFoundException\",\"message\":\"The "
				"specified log stream does not exist.\"}");
	};
	q.before.clear();
	q.source.stream = "13/g1";
	const infra::LogPage missing = store.read(q);
	TEST_EXPECT(missing.error.empty() && missing.lines.empty() && missing.before.empty());
	aws.answer_call = [](const Call &) {
		return answer(400,
				"{\"__type\":\"AccessDeniedException\",\"Message\":\"User: "
				"arn:aws:sts::1:assumed-role/opennova-prod-web is not authorized to "
				"perform: logs:GetLogEvents\"}");
	};
	q.source.stream = "14/g1";
	const infra::LogPage denied = store.read(q);
	TEST_EXPECT(contains(denied.error, "logs answered 400: AccessDeniedException") &&
			contains(denied.error, "logs:GetLogEvents") &&
			!contains(denied.error, infra_stub::kSecret));
	std::printf("  CloudWatch, a known stream: newest first by GetLogEvents, older by its "
				"token, the cache, a missing stream empty, AWS's words\n");
	return 0;
}

// A log group's events, as FilterLogEvents would find them over a window.
struct Events {
	std::vector<std::pair<int64_t, std::string>> all; // by time

	CommandResult filter(const Call &c) const {
		const int64_t start = number(c.json, "startTime");
		const int64_t end = number(c.json, "endTime"); // inclusive
		const int64_t limit = number(c.json, "limit");
		const std::string token = text(c.json, "nextToken");
		std::string pattern = text(c.json, "filterPattern");
		if (pattern.size() >= 2)
			pattern = pattern.substr(1, pattern.size() - 2);
		std::vector<std::pair<int64_t, std::string>> in;
		for (const auto &e : all)
			if (e.first >= start && e.first <= end &&
					(pattern.empty() || contains(e.second, pattern)))
				in.push_back(e);
		const size_t from = token.empty() ? 0 : std::stoul(token);
		std::vector<std::pair<int64_t, std::string>> page;
		for (size_t i = from; i < in.size() && page.size() < static_cast<size_t>(limit); ++i)
			page.push_back(in[i]);
		const size_t next = from + page.size();
		std::string body = "{\"events\":" + events_json(page) + ",\"searchedLogStreams\":[]";
		if (next < in.size())
			body += ",\"nextToken\":\"" + std::to_string(next) + "\"";
		return answer(200, body + "}");
	}
};

int test_cloudwatch_filtered() {
	test_temp::TempDir scratch("log_store_filter");
	ScriptedAws aws;
	// Each case below a minute past the last: none answered from the cache.
	int64_t steady = 0;
	infra::CloudWatchLogStore store(
			aws, aws_config(scratch.root()), "/opennova/prod", [] { return kNoonMs; },
			[&steady] { return steady; });
	// Web's tasks: a line a minute over the last 5 hours, the newest a minute ago, and a
	// second line at line 200's millisecond, where the first page's cut falls.
	Events web;
	for (int k = 300; k >= 1; --k)
		web.all.push_back({ kNoonMs - 60'000LL * k, "line " + std::to_string(300 - k) });
	const int64_t at_200 = web.all[200].first;
	web.all.insert(web.all.begin() + 201, { at_200, "twin of line 200" });
	aws.answer_call = [&web](const Call &c) { return web.filter(c); };

	infra::LogQuery q;
	q.source.subject = infra::LogSource::Subject::Program;
	q.source.program = "web";
	q.limit = 100;
	const infra::LogPage first = store.read(q);
	TEST_EXPECT(first.error.empty() && first.group == "/opennova/prod/service" &&
			first.stream == "web/web/");
	// Its cut falls inside line 200's millisecond: both go to the older page.
	TEST_EXPECT(first.lines.size() == 99 && first.lines.back().text == "line 299" &&
			first.lines.front().text == "line 201");
	TEST_EXPECT(first.before == "t:" + std::to_string(at_200 + 1));
	// Every call names the prefix, the window walking back from now, 15 min, then 4 times
	// as long.
	TEST_EXPECT(text(aws.calls[1].json, "logStreamNamePrefix") == "web/web/" &&
			!aws.calls[1].json.get("logStreamNames") && !aws.calls[1].json.get("filterPattern"));
	TEST_EXPECT(number(aws.calls[1].json, "endTime") == kNoonMs &&
			number(aws.calls[1].json, "startTime") == kNoonMs + 1 - 15 * 60'000);
	TEST_EXPECT(number(aws.calls[2].json, "endTime") == kNoonMs - 15 * 60'000 &&
			number(aws.calls[2].json, "startTime") == kNoonMs + 1 - 75 * 60'000);

	// Older pages to the first line: every line once, in order, the twins on one page.
	std::vector<std::vector<infra::LogLine>> pages = { first.lines };
	q.before = first.before;
	while (!q.before.empty() && pages.size() < 10) {
		const infra::LogPage page = store.read(q);
		TEST_EXPECT(page.error.empty());
		pages.push_back(page.lines);
		q.before = page.before;
	}
	TEST_EXPECT(pages.size() == 4 && q.before.empty());
	std::vector<std::string> all;
	for (auto it = pages.rbegin(); it != pages.rend(); ++it)
		for (const infra::LogLine &l : *it)
			all.push_back(l.text);
	TEST_EXPECT(all.size() == 301 && all.front() == "line 0" && all.back() == "line 299");
	TEST_EXPECT(all[200] == "line 200" && all[201] == "twin of line 200");
	TEST_EXPECT(pages[1].back().text == "twin of line 200");

	// A text filter over a task's stream: the quoted pattern.
	q.before.clear();
	q.source.task = "0123456789abcdef0123456789abcdef";
	q.q = "line 29";
	const infra::LogPage found = store.read(q);
	TEST_EXPECT(aws.calls.back().json.get("logStreamNames") != nullptr &&
			text(aws.calls.back().json, "filterPattern") == "\"line 29\"");
	TEST_EXPECT(found.lines.size() == 11 && found.lines.back().text == "line 299" &&
			found.lines.front().text == "line 29" && found.before.empty());

	// A dense log: more in the first window than a read takes, read narrower.
	Events dense;
	for (int k = 25000; k >= 1; --k)
		dense.all.push_back({ kNoonMs - 10LL * k, "busy " + std::to_string(25000 - k) });
	aws.answer_call = [&dense](const Call &c) { return dense.filter(c); };
	steady += 60'000;
	q.source.task.clear();
	q.q.clear();
	q.limit = 50;
	const infra::LogPage busy = store.read(q);
	TEST_EXPECT(busy.lines.size() == 50 && busy.lines.back().text == "busy 24999");

	// Denser than the calls can narrow to: 16 lines a millisecond over the last 2 s, so every
	// window the read reaches (down to 1.757 s) holds more than its two full pages. The page
	// holds what was read of the last one (its oldest lines) and its cursor moves back, page
	// after page; never no line at the request's own cursor.
	Events denser;
	for (int64_t ms = 1999; ms >= 0; --ms)
		for (int i = 0; i < 16; ++i)
			denser.all.push_back({ kNoonMs - ms, "dense " + std::to_string(1999 - ms) });
	aws.answer_call = [&denser](const Call &c) { return denser.filter(c); };
	steady += 60'000;
	size_t calls_before = aws.count("logs FilterLogEvents");
	const infra::LogPage full = store.read(q);
	TEST_EXPECT(full.error.empty() && !full.lines.empty() && full.lines.size() <= 50);
	TEST_EXPECT(aws.count("logs FilterLogEvents") - calls_before ==
			static_cast<size_t>(infra::CloudWatchLogStore::kMaxCalls));
	const int64_t full_cursor = cursor_ms(full.before);
	TEST_EXPECT(full_cursor > kNoonMs - 2000 && full_cursor <= kNoonMs &&
			full.lines.front().at_ms >= full_cursor);
	q.before = full.before;
	const infra::LogPage fuller = store.read(q);
	TEST_EXPECT(fuller.error.empty() && !fuller.lines.empty() && cursor_ms(fuller.before) > 0 &&
			cursor_ms(fuller.before) < full_cursor && fuller.lines.back().at_ms < full_cursor);
	// Pages that found nothing yet with more to search (a filter over a busy log): no line,
	// the cursor back past the narrowest window read.
	aws.answer_call = [](const Call &) {
		return answer(200, "{\"events\":[],\"nextToken\":\"more\"}");
	};
	q.before = "t:" + std::to_string(kNoonMs - 60'000);
	q.q = "rare";
	const infra::LogPage searching = store.read(q);
	TEST_EXPECT(searching.error.empty() && searching.lines.empty() &&
			cursor_ms(searching.before) > 0 && cursor_ms(searching.before) < kNoonMs - 60'000);
	q.before.clear();
	q.q.clear();

	// Nothing in any window: read back to the groups' horizon, and nothing older.
	aws.answer_call = [](const Call &) { return answer(200, "{\"events\":[]}"); };
	steady += 60'000;
	calls_before = aws.count("logs FilterLogEvents");
	const infra::LogPage quiet = store.read(q);
	TEST_EXPECT(quiet.lines.empty() && quiet.before.empty() && quiet.error.empty());
	TEST_EXPECT(aws.count("logs FilterLogEvents") - calls_before <=
			static_cast<size_t>(infra::CloudWatchLogStore::kMaxCalls));
	// A window that never ends: the calls stop at their bound, and the cursor goes on.
	aws.answer_call = [](const Call &) {
		return answer(200,
				"{\"events\":" + events_json({ { kNoonMs - 5, "again" } }) +
						",\"nextToken\":\"more\"}");
	};
	steady += 60'000;
	calls_before = aws.count("logs FilterLogEvents");
	const infra::LogPage endless = store.read(q);
	TEST_EXPECT(endless.error.empty() && !endless.lines.empty() && cursor_ms(endless.before) > 0 &&
			cursor_ms(endless.before) < kNoonMs + 1);
	TEST_EXPECT(aws.count("logs FilterLogEvents") - calls_before ==
			static_cast<size_t>(infra::CloudWatchLogStore::kMaxCalls));
	q.before = "g:b/1/s";
	TEST_EXPECT(store.read(q).bad_cursor);
	std::printf("  CloudWatch, a program's every task and a filter: windows walking back, a "
				"millisecond kept whole, a dense window narrowed, a denser one's cursor moving "
				"back, the calls bounded\n");
	return 0;
}

bool put_lines(const std::string &path, const std::vector<std::string> &lines) {
	std::error_code ec;
	fs::create_directories(fs::u8path(path).parent_path(), ec);
	std::string text;
	for (const std::string &l : lines)
		text += l + "\n";
	std::string error;
	return io::write_file_atomic(path, text, error);
}

std::string stamp(int64_t second, int ms = 0) {
	char buf[40];
	std::snprintf(buf, sizeof(buf), "2026-10-18T12:%02d:%02d.%03dZ ", static_cast<int>(second / 60),
			static_cast<int>(second % 60), ms);
	return buf;
}

int test_directory() {
	test_temp::TempDir dir("log_store_dir");
	std::vector<std::string> lines;
	lines.push_back("a line hostd wrote with no time");
	for (int i = 0; i < 450; ++i)
		lines.push_back(
				stamp(i) + (i % 50 == 0 ? "ERROR " : "") + "server line " + std::to_string(i));
	TEST_EXPECT(put_lines(dir.root() + "/servers/12/g3.log", lines));
	infra::DirectoryLogStore store(dir.root());
	infra::LogQuery q;
	q.source.subject = infra::LogSource::Subject::Server;
	q.source.id = 12;
	q.source.generation = 3;
	q.limit = 200;
	const infra::LogPage first = store.read(q);
	TEST_EXPECT(first.error.empty() && first.group == dir.root() &&
			first.stream == "servers/12/g3.log");
	TEST_EXPECT(first.lines.size() == 200 && first.lines.back().text == "server line 449" &&
			first.lines.front().text == "ERROR server line 250");
	TEST_EXPECT(
			first.lines.back().at_ms == kNoonMs + 449 * 1000 && first.before.rfind("o:", 0) == 0);
	q.before = first.before;
	const infra::LogPage second = store.read(q);
	TEST_EXPECT(second.lines.size() == 200 && second.lines.back().text == "server line 249");
	q.before = second.before;
	const infra::LogPage third = store.read(q);
	TEST_EXPECT(third.lines.size() == 51 && third.before.empty());
	TEST_EXPECT(third.lines.front().at_ms == -1 &&
			third.lines.front().text == "a line hostd wrote with no time");

	// The filter, a file not written yet, a cursor of the other kind.
	q.before.clear();
	q.q = "ERROR";
	const infra::LogPage errors = store.read(q);
	TEST_EXPECT(errors.lines.size() == 9 && errors.lines.front().text == "ERROR server line 0" &&
			errors.before.empty());
	q.q.clear();
	q.source.generation = 4;
	const infra::LogPage unwritten = store.read(q);
	TEST_EXPECT(unwritten.lines.empty() && unwritten.before.empty() && unwritten.error.empty());
	q.before = "t:5";
	TEST_EXPECT(store.read(q).bad_cursor);

	// A machine's hostd.
	TEST_EXPECT(put_lines(dir.root() + "/machines/4/hostd.log", { "agent started" }));
	q.before.clear();
	q.source.subject = infra::LogSource::Subject::Machine;
	q.source.id = 4;
	const infra::LogPage machine = store.read(q);
	TEST_EXPECT(machine.stream == "machines/4/hostd.log" && machine.lines.size() == 1 &&
			machine.lines[0].text == "agent started");

	// A program's tasks merged by time: a's even seconds, b's odd and a second line at
	// its 7 s.
	std::vector<std::string> a;
	std::vector<std::string> b;
	for (int s = 0; s < 20; ++s)
		(s % 2 == 0 ? a : b).push_back(stamp(s) + "line " + std::to_string(s));
	b.insert(b.begin() + 4, stamp(7) + "line 7 twin");
	TEST_EXPECT(put_lines(dir.root() + "/programs/web/aaaa.log", a));
	TEST_EXPECT(put_lines(dir.root() + "/programs/web/bbbb.log", b));
	TEST_EXPECT(put_lines(dir.root() + "/programs/lobby/cccc.log", { stamp(30) + "not web's" }));
	q.source.subject = infra::LogSource::Subject::Program;
	q.source.program = "web";
	q.limit = 8;
	const infra::LogPage newest = store.read(q);
	TEST_EXPECT(newest.stream == "programs/web/" && newest.lines.size() == 8);
	TEST_EXPECT(newest.lines.back().text == "line 19" && newest.lines.front().text == "line 12");
	q.before = newest.before;
	const infra::LogPage middle = store.read(q);
	TEST_EXPECT(middle.lines.size() == 8 && middle.lines.front().text == "line 5" &&
			middle.lines[2].text == "line 7" && middle.lines[3].text == "line 7 twin" &&
			middle.lines.back().text == "line 11");
	q.before = middle.before;
	const infra::LogPage oldest = store.read(q);
	TEST_EXPECT(oldest.lines.size() == 5 && oldest.lines.front().text == "line 0" &&
			oldest.lines.back().text == "line 4" && oldest.before.empty());
	q.before.clear();
	q.source.task = "bbbb";
	const infra::LogPage one_task = store.read(q);
	TEST_EXPECT(one_task.stream == "programs/web/bbbb.log" && one_task.lines.size() == 8 &&
			one_task.lines.back().text == "line 19");
	q.source.task.clear();
	q.before = "o:12";
	TEST_EXPECT(store.read(q).bad_cursor);
	std::printf("  a directory: a file's pages back from its end, the filter, every task's "
				"file merged by time\n");
	return 0;
}

int test_stamp() {
	const infra::LogLine l = infra::parse_stamped_line("2026-10-18T12:00:01.250Z [boot] ready.");
	TEST_EXPECT(l.at_ms == kNoonMs + 1250 && l.text == "[boot] ready.");
	const infra::LogLine bare = infra::parse_stamped_line("2026-10-18 12:00:01 nope");
	TEST_EXPECT(bare.at_ms == -1 && bare.text == "2026-10-18 12:00:01 nope");
	TEST_EXPECT(infra::parse_stamped_line("2026-13-18T12:00:01.250Z x").at_ms == -1);
	TEST_EXPECT(infra::parse_stamped_line("1970-01-01T00:00:00.000Z ").at_ms == 0);
	std::printf("  the stamp: its time to the millisecond\n");
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "cloudwatch_stream", test_cloudwatch_stream },
		{ "cloudwatch_filtered", test_cloudwatch_filtered },
		{ "directory", test_directory },
		{ "stamp", test_stamp },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the logs the admin's pages read\n");
	return 0;
}
