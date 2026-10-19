// What a supervised run of opennova-serve leaves its supervisor (apps/serve/run_reports.h), on a
// hand-stepped clock: a minute of 62.5 Hz frames summed up as their p50, p99 and longest frame
// time and the late frames (a frame that starts past its own end's deadline), nothing before the
// minute is out, the window starting over after it; the nearest-rank quantile; the
// --stats-file's JSON line and the --exit-file's one line; and the three flags on the command
// line (--status-page on|off, --stats-file, --exit-file).

#include "run_reports.h"
#include "server.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

bool near(double a, double b) {
	return std::fabs(a - b) < 1e-9;
}

constexpr int64_t kMs = 1000 * 1000;
constexpr int64_t kPeriodNs = 16 * kMs; // the 62.5 Hz quantum

void tick_minute() {
	serve::TickStats ticks;
	CHECK(!ticks.take_if_due(0));
	// A minute of frames on time, each 2 ms of work; frame 1000 takes 40 ms, so
	// the one after it starts past its own end's deadline (the loop a whole
	// quantum behind) and the loop is back on time after that.
	int64_t start = 0;
	int64_t deadline = kPeriodNs;
	int frames = 0;
	while (start < serve::TickStats::kWindowNs) {
		const int64_t work = frames == 1000 ? 40 * kMs : 2 * kMs;
		ticks.frame(start, start + work, deadline);
		++frames;
		// The next frame starts once this one's work is done and its sleep is over.
		start = std::max(start + work, deadline);
		deadline += kPeriodNs;
	}
	CHECK(!ticks.take_if_due(serve::TickStats::kWindowNs - 1));
	const auto minute = ticks.take_if_due(start);
	CHECK(minute.has_value());
	if (minute) {
		CHECK(minute->frames == static_cast<uint64_t>(frames));
		CHECK(near(minute->p50_ms, 2.0));
		CHECK(near(minute->p99_ms, 2.0));
		CHECK(near(minute->max_ms, 40.0));
		CHECK(minute->late == 1);
	}
	// The window started over: nothing until a frame and a minute after it.
	CHECK(!ticks.take_if_due(start + serve::TickStats::kWindowNs));
	ticks.frame(start, start + 5 * kMs, start + kPeriodNs);
	CHECK(!ticks.take_if_due(start + serve::TickStats::kWindowNs - 1));
	const auto next = ticks.take_if_due(start + serve::TickStats::kWindowNs);
	CHECK(next && next->frames == 1 && near(next->p99_ms, 5.0) && next->late == 0);
	std::printf(
			"  a minute of frames: p50, p99, the longest, the late ones; the window starts over\n");
}

void late_frames() {
	// A machine that gives the server no CPU: every frame starts a whole quantum behind.
	serve::TickStats ticks;
	for (int i = 0; i < 10; ++i) {
		const int64_t start = (i + 1) * kPeriodNs + kMs;
		ticks.frame(start, start + 20 * kMs, (i + 1) * kPeriodNs);
	}
	const auto minute = ticks.take_if_due(serve::TickStats::kWindowNs + 2 * kPeriodNs);
	CHECK(minute && minute->late == 10 && near(minute->p50_ms, 20.0));
	// A frame that starts exactly at its deadline is on time; a clock that ran
	// back counts no negative work.
	serve::TickStats edge;
	edge.frame(100, 50, 100);
	const auto one = edge.take_if_due(100 + serve::TickStats::kWindowNs);
	CHECK(one && one->late == 0 && near(one->max_ms, 0.0));
	std::printf("  late frames: a start past its own end's deadline\n");
}

void quantiles() {
	std::vector<double> none;
	CHECK(serve::quantile(none, 0.5) == 0.0);
	std::vector<double> values;
	for (int i = 100; i >= 1; --i)
		values.push_back(i);
	CHECK(near(serve::quantile(values, 0.50), 50.0));
	CHECK(near(serve::quantile(values, 0.99), 99.0));
	CHECK(near(serve::quantile(values, 1.0), 100.0));
	std::vector<double> three = { 7.0, 1.0, 4.0 };
	CHECK(near(serve::quantile(three, 0.5), 4.0));
	CHECK(near(serve::quantile(three, 0.99), 7.0));
	std::printf("  the nearest-rank quantile\n");
}

void files() {
	serve::TickMinute m;
	m.p50_ms = 1.234;
	m.p99_ms = 16.5;
	m.max_ms = 31.999;
	m.late = 3;
	m.frames = 3750;
	CHECK(serve::tick_minute_json(m, 1792000000) ==
			"{\"at\":1792000000,\"p50_ms\":1.23,\"p99_ms\":16.50,\"max_ms\":32.00,\"late\":3,"
			"\"frames\":3750}\n");
	CHECK(serve::exit_file_text("the session ended: map\r\nB did\tnot boot") ==
			"the session ended: map  B did not boot\n");
	CHECK(serve::exit_file_text("") == "\n");
	std::printf("  the --stats-file line and the --exit-file line\n");
}

void flags() {
	const std::vector<std::string> base = { "--resource-dir", "/opt/game", "/HOST", "server.host" };
	serve::ServeOptions defaults;
	std::string error;
	CHECK(serve::parse_serve_options(base, defaults, error) == 0);
	CHECK(defaults.status_page && defaults.stats_file.empty() && defaults.exit_file.empty());

	std::vector<std::string> args = base;
	args.insert(args.end(),
			{ "--status-page", "off", "--stats-file", "tick-stats.json", "--exit-file",
					"exit.txt" });
	serve::ServeOptions hosted;
	CHECK(serve::parse_serve_options(args, hosted, error) == 0);
	CHECK(!hosted.status_page && hosted.stats_file == "tick-stats.json" &&
			hosted.exit_file == "exit.txt");

	args = base;
	args.insert(args.end(), { "--status-page", "on" });
	serve::ServeOptions on;
	CHECK(serve::parse_serve_options(args, on, error) == 0 && on.status_page);

	for (const char *bad : { "maybe", "OFF" }) {
		args = base;
		args.insert(args.end(), { "--status-page", bad });
		serve::ServeOptions refused;
		CHECK(serve::parse_serve_options(args, refused, error) == 1);
		CHECK(error == "--status-page takes on or off");
	}
	for (const char *flag : { "--status-page", "--stats-file", "--exit-file" }) {
		args = base;
		args.push_back(flag);
		serve::ServeOptions refused;
		CHECK(serve::parse_serve_options(args, refused, error) == 1);
		CHECK(error == std::string(flag) + " needs a value");
	}
	std::printf("  the command line: --status-page on|off, --stats-file, --exit-file\n");
}

} // namespace

int main() {
	tick_minute();
	late_frames();
	quantiles();
	files();
	flags();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("OK: a supervised run's reports\n");
	return 0;
}
