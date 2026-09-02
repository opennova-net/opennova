extends GutTest

# PerfTimeline (engine/util/perf_timeline.gd): span nesting, unbalanced-span
# recovery, the retained ring, and the summary/brief lines. The class has no
# global cost between operations — these tests exercise the whole surface the
# mission-load instrumentation and the future overlay perf pane rely on.


func test_spans_nest_and_measure() -> void:
	var timeline := PerfTimeline.begin("op")
	timeline.span("outer")
	timeline.span("inner")
	timeline.end_span()
	timeline.end_span()
	var line := timeline.finish()
	var spans := timeline.spans()
	assert_eq(spans.size(), 2, "two spans recorded")
	assert_eq(spans[0].depth, 0, "outer is top-level")
	assert_eq(spans[1].depth, 1, "inner nests under outer")
	assert_true(spans[0].end_us >= spans[1].end_us,
		"outer ends at or after inner")
	assert_string_contains(line, "op", "the structured line names the operation")


func test_finish_closes_unbalanced_spans() -> void:
	var timeline := PerfTimeline.begin("op")
	timeline.span("left_open")
	timeline.finish()
	assert_true(timeline.spans()[0].end_us > 0,
		"finish closes spans an early return left open")


func test_end_span_without_open_is_inert() -> void:
	var timeline := PerfTimeline.begin("op")
	timeline.end_span()
	timeline.finish()
	assert_eq(timeline.spans().size(), 0)


func test_brief_lists_top_level_spans_without_label() -> void:
	var timeline := PerfTimeline.begin("op")
	timeline.span("alpha")
	timeline.end_span()
	timeline.span("beta")
	timeline.end_span()
	timeline.finish()
	var brief := timeline.brief(2)
	assert_string_contains(brief, "alpha")
	assert_string_contains(brief, "beta")
	assert_false(brief.contains("op"), "brief omits the label (owners embed it in their own status)")
	assert_string_contains(timeline.summary(2), "op", "summary carries the label")


func test_span_names_and_ms() -> void:
	var timeline := PerfTimeline.begin("op")
	timeline.span("stage")
	timeline.end_span()
	timeline.finish()
	assert_eq(Array(timeline.span_names()), ["stage"])
	assert_true(timeline.span_ms("stage") >= 0.0)
	assert_eq(timeline.span_ms("absent"), 0.0)
