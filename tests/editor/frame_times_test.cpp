// The Shell's frames as the perf query reads them (session/frame_times.h): a phase added only inside a frame
// (twice in one frame adds up), the ring holding the last kFrames frames and wrapping, the percentiles by
// nearest rank, the slow and stall counts (held and all told), the worst frame's phases, and the summary on
// the wire; and the perf query over a session, which answers every section with no frame yet run.
#include <editor/session/editor_queries.h>
#include <editor/session/frame_times.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>

#include <cstdio>
#include <string>

#include "common/test_expect.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using editor_test::NoProcess;
using opennova::io::JsonValue;

namespace {

void frame(FrameTimes &times, int64_t wall, int64_t layout = 0) {
	times.begin_frame();
	times.add(FramePhase::Wall, wall);
	times.add(FramePhase::Layout, layout);
	times.end_frame();
}

int test_phases_and_ring() {
	FrameTimes times;
	// Nothing outside a frame.
	times.add(FramePhase::Wall, 1000);
	times.end_frame();
	TEST_EXPECT(times.frames() == 0 && times.total_frames() == 0 && times.percentile(FramePhase::Wall, 0.5) == 0);
	// A phase added twice in one frame adds up; a negative time counts as none.
	times.begin_frame();
	TEST_EXPECT(times.in_frame());
	times.add(FramePhase::Compose, 300);
	times.add(FramePhase::Compose, 200);
	times.add(FramePhase::Devices, -5);
	times.add(FramePhase::Wall, 900);
	times.end_frame();
	TEST_EXPECT(!times.in_frame() && times.frames() == 1);
	TEST_EXPECT(times.worst()[size_t(FramePhase::Compose)] == 500 && times.worst()[size_t(FramePhase::Devices)] == 0);
	// 1..100 ms walls: the median the 50th, p95 the 95th, the most the 100th.
	FrameTimes ranks;
	for (int i = 1; i <= 100; ++i) frame(ranks, int64_t(i) * 1000, i);
	TEST_EXPECT(ranks.percentile(FramePhase::Wall, 0.5) == 50000);
	TEST_EXPECT(ranks.percentile(FramePhase::Wall, 0.95) == 95000);
	TEST_EXPECT(ranks.percentile(FramePhase::Wall, 1.0) == 100000);
	TEST_EXPECT(ranks.worst()[size_t(FramePhase::Layout)] == 100);
	// Over 50 ms: 51..100 (50 frames); over 100 ms: none.
	const JsonValue json = ranks.json();
	TEST_EXPECT(json.get_int("slow", -1) == 50 && json.get_int("stall", -1) == 0 && json.get_int("slow_total", -1) == 50);
	// The ring keeps the last kFrames: the slow frames scroll out of it, never out of the totals.
	for (size_t i = 0; i < FrameTimes::kFrames; ++i) frame(ranks, 1000);
	TEST_EXPECT(ranks.frames() == FrameTimes::kFrames && ranks.total_frames() == 100 + FrameTimes::kFrames);
	TEST_EXPECT(ranks.percentile(FramePhase::Wall, 1.0) == 1000);
	const JsonValue after = ranks.json();
	TEST_EXPECT(after.get_int("slow", -1) == 0 && after.get_int("slow_total", -1) == 50);
	return 0;
}

int test_summary_json() {
	FrameTimes times;
	frame(times, 120000, 7000);
	frame(times, 2000, 1000);
	const JsonValue json = times.json();
	TEST_EXPECT(json.get_int("frames", -1) == 2 && json.get_int("stall_total", -1) == 1);
	const JsonValue *phases = json.get("phases");
	TEST_EXPECT(phases && phases->is_object());
	for (size_t p = 0; p < kFramePhaseCount; ++p) {
		const JsonValue *row = phases->get(frame_phase_token(FramePhase(p)));
		TEST_EXPECT(row && row->get("mean") && row->get("p50") && row->get("p95") && row->get("max"));
	}
	TEST_EXPECT(phases->get("wall")->get_int("max", -1) == 120000 && phases->get("layout")->get_int("mean", -1) == 4000);
	const JsonValue *worst = json.get("worst");
	TEST_EXPECT(worst && worst->get_int("wall", -1) == 120000 && worst->get_int("layout", -1) == 7000);
	return 0;
}

int test_perf_query() {
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	std::string error;
	JsonValue answer = session.query("perf", JsonValue(), error);
	TEST_EXPECT(error.empty() && answer.is_object());
	for (const char *section : {"frames", "problems", "validation", "graph", "originals", "textures", "status"})
		TEST_EXPECT(answer.get(section) && answer.get(section)->is_object());
	TEST_EXPECT(answer.get("frames")->get_int("total_frames", -1) == 0);
	// The Shell's frame: the poll adds its parts to it.
	session.frame_times().begin_frame();
	session.poll();
	session.frame_times().add(FramePhase::Wall, 1);
	session.frame_times().end_frame();
	answer = session.query("perf", JsonValue(), error);
	TEST_EXPECT(error.empty() && answer.get("frames")->get_int("total_frames", -1) == 1);
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_phases_and_ring();
	failures += test_summary_json();
	failures += test_perf_query();
	if (failures == 0) std::printf("editor_frame_times: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
