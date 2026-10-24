#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

namespace godot {

// One named wall-clock span of a LoadTimeline: its nesting depth and its
// start / end ticks (end_us 0 while open).
class LoadTimelineSpan : public RefCounted {
	GDCLASS(LoadTimelineSpan, RefCounted)

public:
	String get_name() const { return name_; }
	void set_name(const String &p_name) { name_ = p_name; }
	int get_depth() const { return depth_; }
	void set_depth(int p_depth) { depth_ = p_depth; }
	void set_start_us(int64_t p_value) { start_us_ = p_value; }
	int64_t get_start_us() const { return start_us_; }
	int64_t get_end_us() const { return end_us_; }
	void set_end_us(int64_t p_value) { end_us_ = p_value; }
	int64_t duration_us() const { return end_us_ - start_us_; }
	// The renderer's synchronous pipeline compiles at the span's edges (an instrument, read
	// under --verbose alone; -1 unread): LoadTimeline::pipeline_compiles.
	void set_start_compiles(int64_t p_value) { start_compiles_ = p_value; }
	void set_end_compiles(int64_t p_value) { end_compiles_ = p_value; }
	int64_t compiles() const {
		return start_compiles_ >= 0 && end_compiles_ >= start_compiles_
				? end_compiles_ - start_compiles_
				: 0;
	}

protected:
	static void _bind_methods();

private:
	String name_;
	int depth_ = 0;
	int64_t start_us_ = 0;
	int64_t end_us_ = 0;
	int64_t start_compiles_ = -1;
	int64_t end_compiles_ = -1;
};

// Named nested wall-clock spans for one operation (a mission load; the former
// perf_timeline.gd, ADR 0043 slice G10). An owner creates a timeline with
// begin(), brackets stages with span()/end_span(), and finish()es it, which
// prints one structured line. The owner keeps the timeline it cares about
// (GameWorld.last_load_timeline()); nothing global runs between operations: a
// timeline only costs while its operation does.
class LoadTimeline : public RefCounted {
	GDCLASS(LoadTimeline, RefCounted)

public:
	static constexpr double US_PER_MS = 1000.0;
	static constexpr double MS_PER_S = 1000.0;

	static Ref<LoadTimeline> begin(const String &p_operation_label);

	String get_label() const { return label_; }
	void set_label(const String &p_label) { label_ = p_label; }

	void span(const String &p_name);
	// Ends the innermost open span (no-op when none is open).
	void end_span();
	// A closed span the owner measured outside its own brackets (a deferred flush's share), at
	// the current nesting depth.
	void add_span(const String &p_name, int64_t p_start_us, int64_t p_end_us);
	// Close any spans an early return left open plus the timeline itself, print
	// the structured line, and return the one-line summary so callers can
	// surface it (status bar, log).
	String finish();
	double total_ms() const;
	// Milliseconds of the first completed span named `name` (0.0 when absent).
	double span_ms(const String &p_name) const;
	PackedStringArray span_names() const;
	// The recorded spans, in order, for tooling.
	TypedArray<LoadTimelineSpan> spans() const;
	// "label: 2.4s -- terrain 1.8s, objects 520ms, parse 40ms".
	String summary(int p_top_n = 4) const;
	// The label-less timing line ("2.4s -- terrain 1.8s, objects 520ms"): total
	// plus the largest top-level spans (descending, up to `top_n`). For
	// embedding in an owner's own status message.
	String brief(int p_top_n = 4) const;
	// Every span in its bracketing order, nested ones too ('>' per depth), each as its
	// start's offset from the timeline's and its length: "objects +0 24ms, runtime +24
	// 447ms, >first_leg +471 3ms". finish() prints it under the summary line (an instrument:
	// it shows the time no top-level span covers).
	String detail() const;
	// The one duration rendering every perf surface shares (the summary line
	// and the load status agree by construction).
	static String format_ms(double p_ms);
	// The renderer's synchronous pipeline compiles so far (canvas, mesh, surface and draw
	// sources; the background specializations left out), read under --verbose alone, else -1:
	// finish() prints the spans that compiled any (an instrument: where a load's first-use
	// pipelines land).
	static int64_t pipeline_compiles();

protected:
	static void _bind_methods();

private:
	String label_;
	Vector<Ref<LoadTimelineSpan>> spans_;
	Vector<int> open_; // stack of indices into spans_
	int64_t start_us_ = 0;
	int64_t end_us_ = 0;
};

} // namespace godot
