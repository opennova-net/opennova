#include "world/load_timeline.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void LoadTimelineSpan::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_name"), &LoadTimelineSpan::get_name);
	ClassDB::bind_method(D_METHOD("set_name", "name"), &LoadTimelineSpan::set_name);
	ClassDB::bind_method(D_METHOD("get_depth"), &LoadTimelineSpan::get_depth);
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &LoadTimelineSpan::set_depth);
	ClassDB::bind_method(D_METHOD("get_start_us"), &LoadTimelineSpan::get_start_us);
	ClassDB::bind_method(D_METHOD("set_start_us", "value"), &LoadTimelineSpan::set_start_us);
	ClassDB::bind_method(D_METHOD("get_end_us"), &LoadTimelineSpan::get_end_us);
	ClassDB::bind_method(D_METHOD("set_end_us", "value"), &LoadTimelineSpan::set_end_us);
	ClassDB::bind_method(D_METHOD("duration_us"), &LoadTimelineSpan::duration_us);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "depth"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "start_us"), "set_start_us", "get_start_us");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "end_us"), "set_end_us", "get_end_us");
}

Ref<LoadTimeline> LoadTimeline::begin(const String &p_operation_label) {
	Ref<LoadTimeline> timeline;
	timeline.instantiate();
	timeline->label_ = p_operation_label;
	timeline->start_us_ = Time::get_singleton()->get_ticks_usec();
	return timeline;
}

void LoadTimeline::span(const String &p_name) {
	open_.push_back(spans_.size());
	Ref<LoadTimelineSpan> entry;
	entry.instantiate();
	entry->set_name(p_name);
	entry->set_depth(open_.size() - 1);
	entry->set_start_us(Time::get_singleton()->get_ticks_usec());
	spans_.push_back(entry);
}

void LoadTimeline::end_span() {
	if (open_.is_empty()) {
		return;
	}
	const int idx = open_[open_.size() - 1];
	open_.remove_at(open_.size() - 1);
	spans_[idx]->set_end_us(Time::get_singleton()->get_ticks_usec());
}

String LoadTimeline::finish() {
	while (!open_.is_empty()) {
		end_span();
	}
	end_us_ = Time::get_singleton()->get_ticks_usec();
	const String line = summary();
	UtilityFunctions::print_verbose(String("LoadTimeline: ") + line);
	return line;
}

double LoadTimeline::total_ms() const {
	const int64_t end = end_us_ > 0 ? end_us_ : Time::get_singleton()->get_ticks_usec();
	return static_cast<double>(end - start_us_) / US_PER_MS;
}

double LoadTimeline::span_ms(const String &p_name) const {
	for (const Ref<LoadTimelineSpan> &s : spans_) {
		if (s->get_name() == p_name && s->get_end_us() > 0) {
			return static_cast<double>(s->duration_us()) / US_PER_MS;
		}
	}
	return 0.0;
}

PackedStringArray LoadTimeline::span_names() const {
	PackedStringArray names;
	for (const Ref<LoadTimelineSpan> &s : spans_) {
		names.append(s->get_name());
	}
	return names;
}

TypedArray<LoadTimelineSpan> LoadTimeline::spans() const {
	TypedArray<LoadTimelineSpan> out;
	for (const Ref<LoadTimelineSpan> &s : spans_) {
		out.push_back(s);
	}
	return out;
}

String LoadTimeline::summary(int p_top_n) const {
	return label_ + String(": ") + brief(p_top_n);
}

String LoadTimeline::brief(int p_top_n) const {
	Vector<Ref<LoadTimelineSpan>> tops;
	for (const Ref<LoadTimelineSpan> &s : spans_) {
		if (s->get_depth() == 0 && s->get_end_us() > 0) {
			tops.push_back(s);
		}
	}
	// Descending by duration (the former sort_custom ordering).
	for (int i = 1; i < tops.size(); ++i) {
		Ref<LoadTimelineSpan> key = tops[i];
		int j = i - 1;
		while (j >= 0 && tops[j]->duration_us() < key->duration_us()) {
			tops.write[j + 1] = tops[j];
			--j;
		}
		tops.write[j + 1] = key;
	}
	PackedStringArray parts;
	const int count = MIN(p_top_n, tops.size());
	for (int i = 0; i < count; ++i) {
		const Ref<LoadTimelineSpan> &s = tops[i];
		parts.append(s->get_name() + String(" ") +
				format_ms(static_cast<double>(s->duration_us()) / US_PER_MS));
	}
	const String head = format_ms(total_ms());
	if (parts.is_empty()) {
		return head;
	}
	return head + String(" ") + String::chr(0x2014) + String(" ") + String(", ").join(parts);
}

String LoadTimeline::format_ms(double p_ms) {
	if (p_ms >= MS_PER_S) {
		return vformat("%.1fs", p_ms / MS_PER_S);
	}
	return vformat("%dms", static_cast<int>(Math::round(p_ms)));
}

void LoadTimeline::_bind_methods() {
	ClassDB::bind_static_method("LoadTimeline", D_METHOD("begin", "operation_label"),
			&LoadTimeline::begin);
	ClassDB::bind_static_method("LoadTimeline", D_METHOD("format_ms", "ms"),
			&LoadTimeline::format_ms);
	ClassDB::bind_method(D_METHOD("get_label"), &LoadTimeline::get_label);
	ClassDB::bind_method(D_METHOD("set_label", "label"), &LoadTimeline::set_label);
	ClassDB::bind_method(D_METHOD("span", "name"), &LoadTimeline::span);
	ClassDB::bind_method(D_METHOD("end_span"), &LoadTimeline::end_span);
	ClassDB::bind_method(D_METHOD("finish"), &LoadTimeline::finish);
	ClassDB::bind_method(D_METHOD("total_ms"), &LoadTimeline::total_ms);
	ClassDB::bind_method(D_METHOD("span_ms", "name"), &LoadTimeline::span_ms);
	ClassDB::bind_method(D_METHOD("span_names"), &LoadTimeline::span_names);
	ClassDB::bind_method(D_METHOD("spans"), &LoadTimeline::spans);
	ClassDB::bind_method(D_METHOD("summary", "top_n"), &LoadTimeline::summary, DEFVAL(4));
	ClassDB::bind_method(D_METHOD("brief", "top_n"), &LoadTimeline::brief, DEFVAL(4));
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "label"), "set_label", "get_label");
}
