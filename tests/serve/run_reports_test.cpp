// What a supervised run of opennova-serve leaves its supervisor (apps/serve/run_reports.h), on a
// hand-stepped clock: a minute of 62.5 Hz frames summed up as their p50, p99 and longest frame
// time and the late frames (a frame that starts past its own end's deadline), nothing before the
// minute is out, the window starting over after it; the nearest-rank quantile; the
// --stats-file's JSON line, the --exit-file's one line, the --second-listing-file's JSON line
// in each state and the --build-file's JSON line; and the flags on the command line
// (--status-page on|off, --stats-file, --exit-file, --second-listing-file, --build-file, and
// --version, which needs nothing a run does).

#include "run_reports.h"
#include "server.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
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

// A window's frames summed up as `works` (ms each, in order), its minute out.
std::optional<serve::TickMinute> minute_of(const std::vector<double> &works) {
	serve::TickStats ticks;
	int64_t start = 0;
	for (const double ms : works) {
		ticks.frame(start, start + static_cast<int64_t>(ms * kMs), start + kPeriodNs);
		start += kPeriodNs;
	}
	return ticks.take_if_due(serve::TickStats::kWindowNs);
}

void quantiles() {
	std::vector<double> values;
	for (int i = 100; i >= 1; --i)
		values.push_back(i);
	const auto hundred = minute_of(values);
	CHECK(hundred && near(hundred->p50_ms, 50.0) && near(hundred->p99_ms, 99.0) &&
			near(hundred->max_ms, 100.0));
	const auto three = minute_of({ 7.0, 1.0, 4.0 });
	CHECK(three && near(three->p50_ms, 4.0) && near(three->p99_ms, 7.0));
	// A frame past the binned range reads as the longest frame; one inside it at its bin's
	// lower edge, the file's 0.01 ms.
	const auto slow = minute_of({ 2.0, 1500.0 });
	CHECK(slow && near(slow->p99_ms, 1500.0) && near(slow->max_ms, 1500.0) &&
			near(slow->p50_ms, 2.0));
	serve::TickStats fine;
	fine.frame(0, 2004 * 1000, kPeriodNs);
	const auto binned = fine.take_if_due(serve::TickStats::kWindowNs);
	CHECK(binned && near(binned->p50_ms, 2.0) && near(binned->max_ms, 2.004));
	std::printf("  the nearest-rank quantile, to the bin\n");
}

// An unpaced loop (lock_framerate 0) runs millions of frames a minute: the window counts them,
// keeping none, so its summary is the same as for a few and its memory does not grow with them.
void unpaced_minute() {
	serve::TickStats ticks;
	constexpr int64_t kFrames = 5000000;
	constexpr int64_t kStepNs = serve::TickStats::kWindowNs / kFrames;
	for (int64_t i = 0; i < kFrames; ++i) {
		const int64_t start = i * kStepNs;
		ticks.frame(start, start + (i == kFrames / 2 ? 30 * kMs : 5000), start + kPeriodNs);
	}
	const auto minute = ticks.take_if_due(serve::TickStats::kWindowNs);
	CHECK(minute && minute->frames == static_cast<uint64_t>(kFrames) &&
			near(minute->p50_ms, 0.0) && near(minute->p99_ms, 0.0) && near(minute->max_ms, 30.0) &&
			minute->late == 0);
	std::printf("  an unpaced minute: millions of frames counted, none kept\n");
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
	// The build the server runs: ONE's version (a staging build's run number among it) and its
	// commit's short sha, none for a build that names none.
	OneBuild build;
	build.version = *Version::parse("0.0.22.123");
	build.commit = "a1b2c3d";
	build.staging = true;
	CHECK(serve::build_file_json(build) == "{\"version\":\"0.0.22.123\",\"commit\":\"a1b2c3d\"}\n");
	build.version = *Version::parse("0.0.22");
	build.commit.clear();
	CHECK(serve::build_file_json(build) == "{\"version\":\"0.0.22\",\"commit\":\"\"}\n");
	std::printf("  the --stats-file line, the --exit-file line and the --build-file line\n");
}

void second_listing_lines() {
	using State = serve::SecondListingState;
	serve::SecondListingReport r;
	// Off: a run with no second listing; listed: nothing more to say.
	CHECK(serve::second_listing_json(r) == "{\"state\":\"off\"}\n");
	r.state = State::Listed;
	CHECK(serve::second_listing_json(r) == "{\"state\":\"listed\"}\n");
	// Trying: the try under way of the run's most, and no reason.
	r.state = State::Trying;
	r.attempt = 1;
	r.attempts = 5;
	r.reason = "left over";
	CHECK(serve::second_listing_json(r) == "{\"state\":\"trying\",\"try\":1,\"tries\":5}\n");
	// Retrying: the next try, when it starts, why and the tag behind it.
	r.state = State::Retrying;
	r.attempt = 2;
	r.next_try_at = 1792000060;
	r.reason = "the gate did not answer";
	r.tag = "NWEC15";
	CHECK(serve::second_listing_json(r) ==
			"{\"state\":\"retrying\",\"try\":2,\"tries\":5,\"next_try_at\":1792000060,"
			"\"reason\":\"the gate did not answer\",\"tag\":\"NWEC15\"}\n");
	// Not listed: why, as one line of escaped text, and the players its service sent.
	r.state = State::NotListed;
	r.reason = "the service did not accept the \"name\"\n";
	r.tag = "NWEC57";
	r.slots = { 2, 5 };
	CHECK(serve::second_listing_json(r) ==
			"{\"state\":\"not_listed\",\"reason\":\"the service did not accept the \\\"name\\\" \","
			"\"tag\":\"NWEC57\",\"slots\":[2,5]}\n");
	r.tag.clear();
	r.slots.clear();
	r.reason = "its gate needs --allow-public";
	CHECK(serve::second_listing_json(r) ==
			"{\"state\":\"not_listed\",\"reason\":\"its gate needs --allow-public\"}\n");
	// The write-on-change test: every member counts.
	serve::SecondListingReport a = r;
	CHECK(a == r);
	a.slots = { 1 };
	CHECK(a != r);
	CHECK(std::string(serve::second_listing_state_text(State::NotListed)) == "not_listed");
	std::printf("  the --second-listing-file line in each state\n");
}

void flags() {
	const std::vector<std::string> base = { "--resource-dir", "/opt/game", "/HOST", "server.host" };
	serve::ServeOptions defaults;
	std::string error;
	CHECK(serve::parse_serve_options(base, defaults, error) == 0);
	CHECK(defaults.status_page && defaults.stats_file.empty() && defaults.exit_file.empty() &&
			defaults.second_listing_file.empty() && defaults.build_file.empty() &&
			!defaults.print_version);

	std::vector<std::string> args = base;
	args.insert(args.end(),
			{ "--status-page", "off", "--stats-file", "tick-stats.json", "--exit-file", "exit.txt",
					"--second-listing-file", "second-listing.json", "--build-file", "build.json" });
	serve::ServeOptions hosted;
	CHECK(serve::parse_serve_options(args, hosted, error) == 0);
	CHECK(!hosted.status_page && hosted.stats_file == "tick-stats.json" &&
			hosted.exit_file == "exit.txt" && hosted.second_listing_file == "second-listing.json" &&
			hosted.build_file == "build.json" && !hosted.print_version);

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
	for (const char *flag : { "--status-page", "--stats-file", "--exit-file",
				 "--second-listing-file", "--build-file" }) {
		args = base;
		args.push_back(flag);
		serve::ServeOptions refused;
		CHECK(serve::parse_serve_options(args, refused, error) == 1);
		CHECK(error == std::string(flag) + " needs a value");
	}
	// --version needs no /HOST and no --resource-dir; what else it is given still parses (the
	// game its line names), and a bad value of it is still refused.
	serve::ServeOptions version;
	CHECK(serve::parse_serve_options({ "--version" }, version, error) == 0 &&
			version.print_version && version.resource_dir.empty() && version.host_file.empty());
	serve::ServeOptions version_of_game;
	CHECK(serve::parse_serve_options(
				  { "--resource-dir", "/opt/game", "/game", "dfx2", "--version" }, version_of_game,
				  error) == 0 &&
			version_of_game.print_version && version_of_game.resource_dir == "/opt/game" &&
			version_of_game.game == "dfx2");
	serve::ServeOptions version_refused;
	CHECK(serve::parse_serve_options({ "--version", "--lan-port", "x" }, version_refused, error) ==
			1);
	std::printf("  the command line: --status-page on|off, --stats-file, --exit-file, "
				"--second-listing-file, --build-file, --version\n");
}

} // namespace

int main() {
	tick_minute();
	late_frames();
	quantiles();
	unpaced_minute();
	files();
	second_listing_lines();
	flags();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("OK: a supervised run's reports\n");
	return 0;
}
