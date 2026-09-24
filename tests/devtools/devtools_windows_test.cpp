// The dev tools' inspection windows added after the ADR 0039 core (the Log
// window and the per-domain windows) against a null ImGui backend: each
// window formats its pushed record (or its drained source), its controls
// leave as the debug-control rows they name, and a layout pass draws it.
// The window registry itself is pinned in devtools_test.cpp.
#include "devtools_test_support.h"

#include <base/io/log_ring.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/log_window.h>

#include <cstring>
#include <string>

using namespace devtools_test;
using opennova::devtools::ControlResult;
using opennova::devtools::GameDevTools;
using opennova::devtools::LogWindow;

namespace {

// The Log window drains its ring past the cursor, marks a wrap, carries the
// F3 command verdicts, filters by level and text, and clears its rows while
// keeping the cursor.
void test_log_window_drains_filters_and_marks_gaps() {
	using opennova::io::LogLevel;
	opennova::io::LogRing ring;
	LogWindow window;
	window.set_ring(&ring);
	window.poll();
	CHECK(window.row_count() == 0, "an empty ring drains nothing");

	ring.record(LogLevel::kInfo, "mission loaded");
	ring.record(LogLevel::kWarn, "clamped a fog distance");
	ring.record(LogLevel::kError, "texture missing: foo.pcx");
	window.poll();
	CHECK(window.row_count() == 3, "three entries drain");
	CHECK(std::strcmp(window.row_text(0), "[info] mission loaded") == 0, "oldest first, level-tagged");
	CHECK(std::strcmp(window.row_text(2), "[error] texture missing: foo.pcx") == 0, "the error row");
	window.poll();
	CHECK(window.row_count() == 3, "a second poll with nothing new adds nothing");

	// Overrun the ring's capacity between two polls: the window says how many
	// it missed.
	for (size_t i = 0; i < opennova::io::LogRing::kCapacity + 10; ++i) {
		ring.record(LogLevel::kDebug, "spam");
	}
	window.poll();
	bool gap = false;
	for (int i = 0; i < window.row_count(); ++i) {
		gap = gap || std::strstr(window.row_text(i), "the ring wrapped: 10 message(s) missed") != nullptr;
	}
	CHECK(gap, "a wrapped ring is marked with the missed count");

	ControlResult refused;
	refused.id = "set_entity_health";
	refused.message = "Unauthorized";
	window.add_command_result(refused);
	ControlResult read;
	read.id = "environment_weather_snapshot";
	read.ok = true;
	read.detail = "{\"tod_hhmm\": 1230}";
	window.add_command_result(read);

	window.set_level_mask(1u << LogWindow::kCommand);
	CHECK(window.row_count() == 2, "the cmd level shows only the F3 verdicts");
	CHECK(std::strcmp(window.row_text(0), "[cmd] set_entity_health: failed (Unauthorized)") == 0,
			"a refusal row");
	CHECK(std::strcmp(window.row_text(1),
				  "[cmd] environment_weather_snapshot: ok = {\"tod_hhmm\": 1230}") == 0,
			"a read carries its full payload");

	window.set_level_mask((1u << LogWindow::kLevelCount) - 1u);
	window.set_text_filter("TEXTURE");
	CHECK(window.row_count() == 1, "the text filter is case-insensitive");
	window.set_text_filter("");

	window.clear();
	CHECK(window.total_rows() == 0, "clear drops the rows");
	window.poll();
	CHECK(window.total_rows() == 0, "and keeps the cursor: nothing old comes back");
	ring.record(LogLevel::kInfo, "after the clear");
	window.poll();
	CHECK(window.total_rows() == 1, "new entries still arrive");
}

// The tools route every command verdict to the Log window too, and the
// window draws in a layout pass.
void test_log_window_in_the_tools() {
	NullBackend backend;
	GameDevTools tools;
	opennova::io::LogRing ring;
	tools.set_log_ring(&ring);
	ControlResult ok;
	ok.id = "hide_foliage";
	ok.ok = true;
	tools.report_control_result(ok);
	CHECK(tools.log_window().total_rows() == 1, "the verdict lands in the Log window");
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	tools.log_window().open = true;
	ring.record(opennova::io::LogLevel::kInfo, "hello");
	CHECK(draw_once(tools, 1), "the Log window draws");
	CHECK(tools.log_window().total_rows() == 2, "the draw drained the ring");
}

}  // namespace

int main() {
	test_log_window_drains_filters_and_marks_gaps();
	test_log_window_in_the_tools();
	return report("devtools_windows_test");
}
