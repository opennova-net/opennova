// The decoded-state metrics over the steady window: the C2S 0x0C uplink and the
// S2C 0x0A frame header and per-entity records (the 0x0A shape report diff_0a.py
// printed, folded in and made failing). A documented subset of the retired
// verifier's Compare-SemanticDirection:
//   exact value sets   uplink shape/ADM/state/move/carrier/priority occupancy;
//                      frame sub-block, flags (flags2's route nibble and modal
//                      step), record classes, local state, health, state word,
//                      carried, weapon/timer/env/objective/mount blocks; entity
//                      class:type coverage; the anchor's own record offset
//   zero / constant    a field RR fills that the run leaves all zero, or a
//                      position RR changes that the run never does
//   field population   per entity class, the fields RR ever fills (diff_0a)
//   dynamic vectors    origin-normalized range, displacement and step p90
//                      (nearest rank) within 1/8x..8x of RR, each sequence past a
//                      lone transient lead and short of a lone tail, entity positions in
//                      world units (a move within the records' compressed steps none),
//                      entity and uplink positions one sequence per carrier frame;
//                      priority scores' per-slot median likewise;
//                      initial position/anchor within 64 units
//   round targets      per shooter kind, whether its S2C 0x0A round events ever name
//                      a target and whom (the recipient's own body, 0xFFFF, another)
// Session-local handles reduce to none/bound (normalization.h).

#include "metric.h"

#include "normalization.h"

#include <net/npwire/ingame_decode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace opennova::parity {

namespace {

MetricValue metric(char dir, const std::string &name, const std::string &key, const char *code) {
	MetricValue value;
	value.direction = dir;
	value.metric = name;
	value.key = key;
	value.code = code;
	return value;
}

void set_metric(std::vector<MetricValue> &out, char dir, const std::string &name, const char *code,
		const std::set<std::string> &a, const std::set<std::string> &b,
		const std::string &key = "") {
	MetricValue value = metric(dir, name, key, code);
	value.distance = jaccard_distance(a, b);
	value.subject = render_set(b);
	value.reference = render_set(a);
	out.push_back(value);
}

// RR fills the field (some sample nonzero) and the run never does.
void fill_metric(std::vector<MetricValue> &out, char dir, const std::string &name, const char *code,
		bool rr_fills, bool other_fills, const std::string &key = "") {
	MetricValue value = metric(dir, name, key, code);
	value.distance = rr_fills && !other_fills ? 1.0 : 0.0;
	value.subject = other_fills ? "filled" : "never filled";
	value.reference = rr_fills ? "filled" : "never filled";
	out.push_back(value);
}

template <typename T> std::string join(const T *values, size_t count) {
	std::string out;
	for (size_t i = 0; i < count; ++i) {
		if (i != 0)
			out += ",";
		out += std::to_string(values[i]);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Dynamic vectors

struct Sample {
	std::string group;
	std::vector<double> axes;
	std::vector<double> quantum; // per axis, a compact record's step (empty: exact values)
};

struct VectorStats {
	size_t samples = 0;
	std::vector<double> baseline;
	double range_p90 = 0.0, displacement_p90 = 0.0, step_p90 = 0.0;
};

double wrapped(double delta, double modulus) {
	if (modulus <= 0.0)
		return delta;
	const double half = modulus / 2.0;
	while (delta > half)
		delta -= modulus;
	while (delta < -half)
		delta += modulus;
	return delta;
}

// A sequence's lone transients at its edges, as the half-open range of `seq` it keeps. Its
// first sample is a lone transient when the next one differs from it and the one after
// repeats the next: the sequence comes to rest on its second sample, and the first is a
// change the window's edge cut (a pose landing, a lone change record), so it sets no
// baseline (state.transient_lead). Its last sample is one when it differs from the one
// before while that one repeats its own predecessor: a change its end cut, the window's
// end or its source's (a joiner's uplinks stop at its death) (state.transient_tail).
template <typename T, typename Same>
std::pair<size_t, size_t> sequence_span(const std::vector<T> &seq, Same same) {
	static_assert(has_normalization("state.transient_lead"));
	static_assert(has_normalization("state.transient_tail"));
	size_t begin = 0, end = seq.size();
	if (seq.size() >= 3 && !same(seq[0], seq[1]) && same(seq[1], seq[2]))
		begin = 1;
	if (end - begin >= 3 && !same(seq[end - 1], seq[end - 2]) && same(seq[end - 2], seq[end - 3]))
		--end;
	return { begin, end };
}

// Two samples' delta on one axis. A compact record carries its offset from the anchor
// rounded to the nearest step of 16 << shift [orig: Network_CompressFixedPoint @0x4C2780 (the
// rounding add of 8 << shift @0x4C2790..0x4C27B3, the 12-bit mantissa @0x4C27C6)], so it
// decodes to within half its step of the position it carried, and a delta within the two
// samples' half steps is none (state.position_quantum).
double axis_delta(const Sample &x, const Sample &y, size_t a, double modulus) {
	static_assert(has_normalization("state.position_quantum"));
	const double delta = wrapped(x.axes[a] - y.axes[a], modulus);
	if (!x.quantum.empty() && !y.quantum.empty() &&
			std::fabs(delta) <= (x.quantum[a] + y.quantum[a]) / 2.0)
		return 0.0;
	return delta;
}

bool same_sample(const Sample &x, const Sample &y, double modulus) {
	if (x.quantum.empty() || y.quantum.empty())
		return x.axes == y.axes;
	for (size_t a = 0; a < x.axes.size(); ++a)
		if (axis_delta(x, y, a, modulus) != 0.0)
			return false;
	return true;
}

// `samples` in capture order.
VectorStats vector_stats(const std::vector<Sample> &samples, double modulus) {
	VectorStats stats;
	stats.samples = samples.size();
	std::map<std::string, std::vector<const Sample *>> groups;
	for (const Sample &sample : samples)
		groups[sample.group].push_back(&sample);
	std::vector<double> ranges, displacements, steps;
	for (const auto &group : groups) {
		const std::pair<size_t, size_t> span = sequence_span(group.second,
				[modulus](const Sample *x, const Sample *y) {
					return same_sample(*x, *y, modulus);
				});
		const std::vector<const Sample *> seq(group.second.begin() + std::ptrdiff_t(span.first),
				group.second.begin() + std::ptrdiff_t(span.second));
		if (group.first == samples.front().group)
			stats.baseline = seq.front()->axes;
		if (seq.size() < 2)
			continue;
		const size_t axes = seq.front()->axes.size();
		std::vector<double> lo(axes, 0.0), hi(axes, 0.0);
		for (size_t i = 0; i < seq.size(); ++i) {
			double displacement = 0.0, step = 0.0;
			for (size_t a = 0; a < axes; ++a) {
				const double from_origin = axis_delta(*seq[i], *seq.front(), a, modulus);
				displacement += std::fabs(from_origin);
				lo[a] = std::min(lo[a], from_origin);
				hi[a] = std::max(hi[a], from_origin);
				if (i > 0)
					step += std::fabs(axis_delta(*seq[i], *seq[i - 1], a, modulus));
			}
			displacements.push_back(displacement);
			if (step > 0.0)
				steps.push_back(step);
		}
		double range = 0.0;
		for (size_t a = 0; a < axes; ++a)
			range += hi[a] - lo[a];
		ranges.push_back(range);
	}
	stats.range_p90 = p90(ranges);
	stats.displacement_p90 = p90(displacements);
	stats.step_p90 = p90(steps);
	return stats;
}

constexpr size_t kDynamicMinSamples = 8;
constexpr double kDynamicRatioFloor = 3.0; // log2(8): the 1/8x..8x band
constexpr double kBaselineFloorFixed16 = 4194304.0; // 64 world units in 16.16

void dynamic_metrics(std::vector<MetricValue> &out, char dir, const std::string &label,
		const std::vector<Sample> &a, const std::vector<Sample> &b, double modulus, bool baseline) {
	static_assert(has_normalization("state.position"));
	if (a.size() < kDynamicMinSamples || b.size() < kDynamicMinSamples)
		return;
	const VectorStats sa = vector_stats(a, modulus), sb = vector_stats(b, modulus);
	if (baseline && sa.baseline.size() == sb.baseline.size()) {
		double worst = 0.0;
		for (size_t i = 0; i < sa.baseline.size(); ++i)
			worst = std::max(worst, std::fabs(wrapped(sb.baseline[i] - sa.baseline[i], modulus)));
		MetricValue value = metric(dir, label + "_baseline", "", "DYNAMIC_BASELINE_MISMATCH");
		value.distance = worst;
		value.floor = kBaselineFloorFixed16;
		value.subject = render_list([&] {
			std::vector<std::string> v;
			for (double x : sb.baseline)
				v.push_back(render_number(x));
			return v;
		}());
		value.reference = render_list([&] {
			std::vector<std::string> v;
			for (double x : sa.baseline)
				v.push_back(render_number(x));
			return v;
		}());
		value.detail = "the first sample's largest axis delta (16.16), a lone transient lead skipped";
		out.push_back(value);
	}
	const struct {
		const char *name;
		double VectorStats::*field;
		const char *code;
	} kStats[] = {
		{ "range_p90", &VectorStats::range_p90, "DYNAMIC_RANGE_MISMATCH" },
		{ "displacement_p90", &VectorStats::displacement_p90, "DYNAMIC_RANGE_MISMATCH" },
		{ "step_p90", &VectorStats::step_p90, "DYNAMIC_STEP_MISMATCH" },
	};
	for (const auto &stat : kStats) {
		const double ra = sa.*(stat.field), rb = sb.*(stat.field);
		if (!(ra > 0.0))
			continue;
		MetricValue value = metric(dir, label + "_" + stat.name, "", stat.code);
		value.distance = rb > 0.0 ? std::fabs(std::log2(rb / ra)) : 64.0;
		value.floor = kDynamicRatioFloor;
		value.subject = render_number(rb);
		value.reference = render_number(ra);
		value.detail = "|log2(subject / RR)|; 3 is the 1/8x..8x band";
		out.push_back(value);
	}
}

// Sequences whose key changes over the samples (a moving or turning entity), each
// sequence's keys past its lone transients (sequence_span). A sequence is one handle's
// records in one frame of reference (`sequence`: the handle and its carrier word).
template <typename T>
size_t changing_handles(const std::vector<const T *> &records,
		const std::function<std::string(const T &)> &sequence,
		const std::function<std::string(const T &)> &key) {
	std::map<std::string, std::vector<std::string>> seen;
	for (const T *record : records)
		seen[sequence(*record)].push_back(key(*record));
	size_t changing = 0;
	for (const auto &entry : seen) {
		const std::pair<size_t, size_t> span = sequence_span(
				entry.second, [](const std::string &x, const std::string &y) { return x == y; });
		const std::set<std::string> keys(entry.second.begin() + std::ptrdiff_t(span.first),
				entry.second.begin() + std::ptrdiff_t(span.second));
		changing += keys.size() > 1 ? 1 : 0;
	}
	return changing;
}

double median(std::vector<double> values) {
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	return values[(values.size() - 1) / 2];
}

// ---------------------------------------------------------------------------
// C2S 0x0C

void priority_score_metric(std::vector<MetricValue> &out, int slot, const std::vector<double> &a,
		const std::vector<double> &b) {
	static_assert(has_normalization("uplink.priority_score"));
	const double ma = median(a);
	if (a.size() < kDynamicMinSamples || !(ma > 0.0))
		return;
	MetricValue value = metric('C', "uplink_priority_score_p50", std::to_string(slot),
			"UPLINK_PRIORITY_MISMATCH");
	value.floor = kDynamicRatioFloor;
	value.reference = render_number(ma);
	if (b.size() < kDynamicMinSamples) {
		value.comparable = false;
		value.subject = std::to_string(b.size()) + " samples";
		value.detail = "fewer than 8 occupied samples";
		out.push_back(value);
		return;
	}
	const double mb = median(b);
	value.distance = mb > 0.0 ? std::fabs(std::log2(mb / ma)) : 64.0;
	value.subject = render_number(mb);
	value.detail = "|log2(subject / RR)| of the slot's median score; 3 is the 1/8x..8x band";
	out.push_back(value);
}

std::vector<const wire::ParityUplinkState *> steady_uplinks(const RunFacts &run) {
	std::vector<const wire::ParityUplinkState *> out;
	for (const wire::ParityUplinkState &state : run.trace.uplinks)
		if (state.decoded && run.in_steady(state.ts_ns))
			out.push_back(&state);
	return out;
}

void compare_uplinks(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("uplink.carrier"));
	static_assert(has_normalization("uplink.priority_handle"));
	const auto a = steady_uplinks(rr), b = steady_uplinks(other);
	if (a.empty() && b.empty())
		return;
	using U = wire::ParityUplinkState;
	auto values = [](const std::vector<const U *> &states,
						  const std::function<std::string(const U &)> &key) {
		std::set<std::string> out;
		for (const U *state : states)
			out.insert(key(*state));
		return out;
	};
	auto both = [&](const char *name, const char *code,
						const std::function<std::string(const U &)> &key) {
		set_metric(out, 'C', name, code, values(a, key), values(b, key));
	};
	both("uplink_shape", "UPLINK_SHAPE_MISMATCH", [](const U &s) {
		return std::to_string(s.length) + "|" + std::to_string(s.type_id) + "|" +
				std::to_string(s.sub_op);
	});
	both("uplink_adm", "UPLINK_LOADOUT_MISMATCH", [](const U &s) { return std::to_string(s.adm); });
	both("uplink_state", "UPLINK_STATE_MISMATCH",
			[](const U &s) { return std::to_string(s.state_flags); });
	both("uplink_move", "UPLINK_MOVE_MISMATCH", [](const U &s) { return std::to_string(s.move); });
	both("uplink_carrier", "UPLINK_CARRIER_MISMATCH",
			[](const U &s) { return std::string(s.carrier == 0xFFFF ? "none" : "bound"); });
	// The carrier word is the body's ground link and the uplink's position is relative
	// to it, so each change of the word between consecutive uplinks changes the
	// position's frame of reference (uplink.carrier).
	auto carrier_changes = [](const std::vector<const U *> &states) {
		size_t changes = 0;
		for (size_t i = 1; i < states.size(); ++i)
			changes += states[i]->carrier != states[i - 1]->carrier ? 1 : 0;
		return double(changes);
	};
	MetricValue frames = metric('C', "uplink_carrier_changes", "", "UPLINK_CARRIER_SEQUENCE");
	count_metric(frames, carrier_changes(a), 0.0, carrier_changes(b), 0.0);
	frames.detail = "changes of the carrier word between consecutive uplinks";
	out.push_back(frames);
	auto occupied = [](const U &s, int slot) {
		return s.priority_handle[slot] != 0 || s.priority_score[slot] != 0;
	};
	both("uplink_priority", "UPLINK_PRIORITY_MISMATCH", [&occupied](const U &s) {
		std::string key;
		for (int slot = 0; slot < 4; ++slot)
			key += (slot ? "," : "") + std::string(occupied(s, slot) ? "occupied" : "empty");
		return key;
	});
	// The scores are continuous distance/view weights (uplink.priority_score): each
	// slot's median within the 1/8x..8x band of RR's.
	for (int slot = 0; slot < 4; ++slot) {
		auto scores = [&occupied, slot](const std::vector<const U *> &states) {
			std::vector<double> out;
			for (const U *s : states)
				if (occupied(*s, slot))
					out.push_back(double(s->priority_score[slot]));
			return out;
		};
		priority_score_metric(out, slot, scores(a), scores(b));
	}

	auto any = [](const std::vector<const U *> &states, const std::function<bool(const U &)> &f) {
		for (const U *state : states)
			if (f(*state))
				return true;
		return false;
	};
	auto fills = [&](const char *name, const char *code, const std::function<bool(const U &)> &f) {
		fill_metric(out, 'C', name, code, any(a, f), any(b, f));
	};
	fills("uplink_position_fill", "UPLINK_ALL_ZERO",
			[](const U &s) { return s.pos_x != 0 || s.pos_y != 0 || s.pos_z != 0; });
	fills("uplink_orientation_fill", "UPLINK_ORIENTATION_ZERO",
			[](const U &s) { return s.heading != 0 || s.pitch != 0; });
	fills("uplink_analog_fill", "UPLINK_ANALOG_ZERO",
			[](const U &s) { return s.analog_x != 0 || s.analog_y != 0 || s.analog_z != 0; });
	fills("uplink_priority_fill", "UPLINK_PRIORITY_EMPTY",
			[](const U &s) { return s.priority_nonzero != 0; });
	// The position is relative to the carrier word's pose (uplink.carrier): a change of
	// the word is a change of frame, not motion.
	const std::function<std::string(const U &)> sequence = [](const U &s) {
		return std::to_string(s.handle) + "@" + std::to_string(s.carrier);
	};
	const std::function<std::string(const U &)> position = [](const U &s) {
		return std::to_string(s.pos_x) + "," + std::to_string(s.pos_y) + "," +
				std::to_string(s.pos_z);
	};
	fill_metric(out, 'C', "uplink_motion", "UPLINK_CONSTANT",
			changing_handles(a, sequence, position) > 0,
			changing_handles(b, sequence, position) > 0);

	auto samples = [](const std::vector<const U *> &states, int which) {
		std::vector<Sample> out;
		for (const U *s : states) {
			Sample sample;
			// The position and heading go out relative to the carrier word's pose: each
			// carrier's uplinks are one sequence, and no step spans two frames of reference
			// (uplink.carrier).
			if (which != 2)
				sample.group = std::to_string(s->carrier);
			if (which == 0)
				sample.axes = { double(s->pos_x), double(s->pos_y), double(s->pos_z) };
			else if (which == 1)
				sample.axes = { double(s->heading), double(s->pitch) };
			else
				sample.axes = { double(s->analog_x), double(s->analog_y), double(s->analog_z) };
			out.push_back(sample);
		}
		return out;
	};
	dynamic_metrics(out, 'C', "uplink_position", samples(a, 0), samples(b, 0), 0.0, true);
	dynamic_metrics(out, 'C', "uplink_orientation", samples(a, 1), samples(b, 1), 65536.0, false);
	dynamic_metrics(out, 'C', "uplink_analog", samples(a, 2), samples(b, 2), 0.0, false);
}

// ---------------------------------------------------------------------------
// S2C 0x0A

// The recipient's own entity: the handle its C2S 0x0C uplinks name (the modal one).
bool own_handle(const RunFacts &run, uint32_t &handle) {
	std::map<uint32_t, size_t> handles;
	for (const wire::ParityUplinkState &state : run.trace.uplinks)
		if (state.decoded && run.in_comparison(state.ts_ns))
			++handles[state.handle];
	if (handles.empty())
		return false;
	handle = std::max_element(handles.begin(), handles.end(), [](const auto &x, const auto &y) {
		return x.second < y.second;
	})->first;
	return true;
}

std::vector<const wire::ParityFrameState *> steady_frames(const RunFacts &run) {
	std::vector<const wire::ParityFrameState *> out;
	for (const wire::ParityFrameState &state : run.trace.frames)
		if (state.decoded && run.in_steady(state.ts_ns))
			out.push_back(&state);
	return out;
}

// The modal step between consecutive frames' flags2 (the per-slot send counter):
// a capture drop makes a step of 2, so only the mode compares.
std::set<std::string> phase_step(const std::vector<const wire::ParityFrameState *> &frames) {
	std::map<uint32_t, size_t> steps;
	for (size_t i = 1; i < frames.size(); ++i)
		++steps[(frames[i]->flags2 - frames[i - 1]->flags2) & 0xFFu];
	if (steps.empty())
		return {};
	const auto mode = std::max_element(steps.begin(), steps.end(),
			[](const auto &x, const auto &y) { return x.second < y.second; });
	return { std::to_string(mode->first) };
}

void compare_frames(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("frame.carried"));
	static_assert(has_normalization("frame.mount_handle"));
	static_assert(has_normalization("frame.timer_seconds"));
	static_assert(has_normalization("frame.server_stats"));
	static_assert(has_normalization("frame.phase_counter"));
	static_assert(has_normalization("frame.env_tod"));
	const auto a = steady_frames(rr), b = steady_frames(other);
	if (a.empty() && b.empty())
		return;
	using F = wire::ParityFrameState;
	auto values = [](const std::vector<const F *> &frames,
						  const std::function<std::string(const F &)> &key) {
		std::set<std::string> out;
		for (const F *frame : frames)
			out.insert(key(*frame));
		return out;
	};
	auto both = [&](const char *name, const char *code,
						const std::function<std::string(const F &)> &key) {
		set_metric(out, 'S', name, code, values(a, key), values(b, key));
	};
	both("frame_subblock", "FRAME_SUBBLOCK_MISMATCH",
			[](const F &f) { return std::to_string(f.sub_block); });
	both("frame_flags1", "FRAME_HEADER_FLAGS_MISMATCH",
			[](const F &f) { return std::to_string(f.flags1); });
	// flags2 counts sends: the client routes on its low nibble (D-NET-75). The
	// nibbles a window holds are the counter's phase slice until it spans one full
	// cycle (16 sends at the modal step of one, the mask's width), and the slice's
	// start is the sends before the window: x5's kick (about six steady frames, the
	// punt ends the session) read RR {7..12} with RR-aa 0.917 from it. So the set
	// compares over a full cycle on both sides; frame_phase_step keeps the count.
	both("frame_flags2", "FRAME_HEADER_FLAGS_MISMATCH",
			[](const F &f) { return std::to_string(f.flags2 & kFrameFlags2RouteMask); });
	constexpr size_t kRouteCycle = size_t(kFrameFlags2RouteMask) + 1;
	if (a.size() < kRouteCycle || b.size() < kRouteCycle) {
		MetricValue &route = out.back();
		route.comparable = false;
		route.reference_short = a.size() < kRouteCycle;
		route.detail = "fewer than 16 steady frames: the route nibbles are a phase slice";
	}
	set_metric(out, 'S', "frame_phase_step", "FRAME_HEADER_FLAGS_MISMATCH", phase_step(a),
			phase_step(b));
	both("frame_record_classes", "FRAME_RECORD_CLASS_MISMATCH", [](const F &f) {
		std::string key;
		if (f.players)
			key += "P";
		if (f.vehicles)
			key += "V";
		if (f.infantry)
			key += "I";
		if (f.none)
			key += "N";
		return key.empty() ? std::string("-") : key;
	});
	both("frame_local_state", "FRAME_LOCAL_STATE_MISMATCH",
			[](const F &f) { return std::to_string(f.state_flag_byte); });
	// A residual between a run's full health and death is a hit's draw: the damage is
	// the round's impact speed times its weight / 875 [orig: Weapon_CalcImpactDamage
	// @0x4EC920 (@0x4ECB2B)] times the zone's multiplier (on a type-3 target the switch
	// @0x4EC9BF: 1.25 for zones 0..4, 1.0 for 5..8 and for 19 and above (the default leg
	// @0x4EC9B6 keeps the fld1 @0x4EC94D), 0.5 for 9..12 and 15..18, 3.0 for
	// 13..14; the def flag 0x200's seat path @0x4EC968..0x4EC994: 6.0 for seat types 2, 3,
	// 6 and 7), then on a type-3 target the shooter's 0.9 or 1.1 (@0x4ECB5A / @0x4ECB66)
	// and the ammo def's clamp (@0x4ECB77..0x4ECB91), so where a round lands and how fast
	// it arrives pick the value. The frame's tail carries the body's Health word as it stands at
	// the send [orig: NetPacket_WritePlayerState @0x4FF6B0 (the read @0x4FFD56, the store
	// @0x4FFD6A)] and the frames go out at the recipient's own cadence [orig:
	// Server_SendEntityStateToPlayer @0x517BA0 (the per-slot counter @0x517BE8)], so a kill
	// from full, or a hit and the kill inside one frame step, shows no residual: a run whose
	// health falls from full straight to 0 between two consecutive frames takes the residual
	// out of both sets (frame.health_hit).
	static_assert(has_normalization("frame.health_hit"));
	auto full_health = [](const std::vector<const F *> &frames) {
		int32_t full = 0;
		for (const F *f : frames)
			full = std::max(full, f->health);
		return full;
	};
	auto health = [](const std::vector<const F *> &frames, int32_t full) {
		std::set<std::string> out;
		for (const F *f : frames)
			out.insert(f->health == 0 || f->health == full ? std::to_string(f->health)
														   : std::string("hit"));
		return out;
	};
	auto straight_death = [](const std::vector<const F *> &frames, int32_t full) {
		for (size_t i = 1; i < frames.size(); ++i)
			if (full > 0 && frames[i - 1]->health == full && frames[i]->health == 0)
				return true;
		return false;
	};
	{
		const int32_t full_a = full_health(a), full_b = full_health(b);
		std::set<std::string> ha = health(a, full_a), hb = health(b, full_b);
		if (straight_death(a, full_a) || straight_death(b, full_b)) {
			ha.erase("hit");
			hb.erase("hit");
		}
		set_metric(out, 'S', "frame_health", "FRAME_HEADER_VALUES_MISMATCH", ha, hb);
	}
	both("frame_armor", "FRAME_HEADER_VALUES_MISMATCH",
			[](const F &f) { return std::to_string(f.armor); });
	both("frame_carried", "FRAME_HEADER_VALUES_MISMATCH",
			[](const F &f) { return std::string(f.carried == 0xFFFF ? "none" : "bound"); });
	// The weapon block rides one frame in four [orig: NetPacket_WritePlayerState @0x4FF6B0
	// (the phase test @0x4FF81B)], and its slot bytes +0x168, +0x170, +0x16C and +0x164
	// (weapon[1..4], written @0x4FF845, @0x4FF876, @0x4FF8A1, @0x4FF8BB) are counters the
	// host steps down in one per-slot pass once a second [orig: Server_TickUpdate @0x51D7E0
	// (the per-slot pass @0x51DFBA..0x51E022)]. A countdown's first sample is its first
	// second or the next (its start against the block's phase), and one the window cuts ends
	// where the window does: each countdown compares over the values both runs' samples
	// cover, a sample that leaves it taking its slot's value out. Two countdowns two accepts
	// start step in that one pass, so their offset is the starts' difference less the passes
	// between the accepts: the team change's 60s [orig: NapiNPServerMsg_0x04D_ChangeTeam
	// @0x518F70 (the stores @0x51906B, @0x519076)] and the re-arm's 30 a second or less later
	// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790 (the store @0x515BA6)] stand 30 or
	// 29 apart. So a block renders as its shape (each countdown slot running or not), each
	// kept slot's value and each two kept slots' offset, and a run's offset one from RR's,
	// where RR's own is not the run's, is RR's: the one pass between two accepts
	// (frame.weapon_countdown).
	static_assert(has_normalization("frame.weapon_countdown"));
	{
		struct Countdown {
			size_t first = 0, last = 0; // its samples, indices into the run's blocks
			uint32_t top = 0, bottom = 0;
		};
		auto blocks = [](const std::vector<const F *> &frames) {
			std::vector<const F *> out;
			for (const F *f : frames)
				if (f->weapon_present)
					out.push_back(f);
			return out;
		};
		// A countdown: consecutive nonzero samples, none above the one before it.
		auto countdowns = [](const std::vector<const F *> &w, size_t slot) {
			std::vector<Countdown> out;
			bool open = false;
			for (size_t i = 0; i < w.size(); ++i) {
				const uint32_t v = w[i]->weapon[slot];
				if (v == 0) {
					open = false;
					continue;
				}
				if (open && v <= out.back().bottom) {
					out.back().last = i;
					out.back().bottom = v;
					continue;
				}
				out.push_back({ i, i, v, v });
				open = true;
			}
			return out;
		};
		const std::vector<const F *> wa = blocks(a), wb = blocks(b);
		std::set<std::pair<const F *, size_t>> dropped; // a block's slot left out
		for (size_t slot = 1; slot <= 4; ++slot) {
			const std::vector<Countdown> ca = countdowns(wa, slot), cb = countdowns(wb, slot);
			if (ca.size() != cb.size())
				continue;
			for (size_t k = 0; k < ca.size(); ++k) {
				const Countdown &x = ca[k], &y = cb[k];
				// The top: one second apart, or a countdown already running when the window
				// opened. The bottom: a countdown the window's end cut.
				uint32_t hi = std::max(x.top, y.top), lo = std::min(x.bottom, y.bottom);
				if (x.top != y.top &&
						(hi - std::min(x.top, y.top) == 1 || x.first == 0 || y.first == 0))
					hi = std::min(x.top, y.top);
				if (x.bottom != y.bottom && (x.last + 1 == wa.size() || y.last + 1 == wb.size()))
					lo = std::max(x.bottom, y.bottom);
				if (lo > hi)
					continue;
				auto drop = [&dropped, slot, lo, hi](
									const std::vector<const F *> &w, const Countdown &c) {
					for (size_t i = c.first; i <= c.last; ++i)
						if (w[i]->weapon[slot] > hi || w[i]->weapon[slot] < lo)
							dropped.insert({ w[i], slot });
				};
				drop(wa, x);
				drop(wb, y);
			}
		}
		struct Rendered {
			std::set<std::string> keys;
			std::set<std::tuple<size_t, size_t, int64_t>> offsets; // slot, slot, value apart
		};
		auto rendered = [&dropped](const std::vector<const F *> &frames) {
			Rendered out;
			for (const F *f : frames) {
				if (!f->weapon_present) {
					out.keys.insert("0");
					continue;
				}
				std::string shape = "1:" + std::to_string(f->weapon[0]);
				for (size_t s = 1; s < 7; ++s)
					shape += "," +
							(s <= 4 && f->weapon[s] != 0 ? std::string("c")
														 : std::to_string(f->weapon[s]));
				out.keys.insert(shape + "," + std::to_string(f->weapon_uniform_team_mask));
				std::vector<size_t> kept;
				for (size_t s = 1; s <= 4; ++s)
					if (f->weapon[s] != 0 && !dropped.count({ f, s })) {
						kept.push_back(s);
						out.keys.insert(
								"w" + std::to_string(s) + ":" + std::to_string(f->weapon[s]));
					}
				for (size_t i = 0; i < kept.size(); ++i)
					for (size_t j = i + 1; j < kept.size(); ++j)
						out.offsets.insert({ kept[i], kept[j],
								int64_t(f->weapon[kept[i]]) - int64_t(f->weapon[kept[j]]) });
			}
			return out;
		};
		auto offset_key = [](size_t s, size_t t, int64_t apart) {
			return "w" + std::to_string(s) + "-w" + std::to_string(t) + ":" + std::to_string(apart);
		};
		Rendered ra = rendered(a), rb = rendered(b);
		for (const auto &[s, t, apart] : ra.offsets)
			ra.keys.insert(offset_key(s, t, apart));
		for (const auto &[s, t, apart] : rb.offsets) {
			int64_t shown = apart;
			if (!ra.offsets.count({ s, t, apart })) {
				if (ra.offsets.count({ s, t, apart - 1 }))
					shown = apart - 1;
				else if (ra.offsets.count({ s, t, apart + 1 }))
					shown = apart + 1;
			}
			rb.keys.insert(offset_key(s, t, shown));
		}
		set_metric(out, 'S', "frame_weapon", "FRAME_WEAPON_PHASE_MISMATCH", ra.keys, rb.keys);
	}
	// breathtime and fallmps; the fps and CPU bytes are machine-load measurements.
	both("frame_timer", "FRAME_TIMER_PHASE_MISMATCH", [](const F &f) {
		return f.timer_present ? "1:" + join(f.timer_state, 2) : std::string("0");
	});
	both("frame_env", "FRAME_ENV_PHASE_MISMATCH", [](const F &f) {
		if (!f.env_present)
			return std::string("0");
		// fog dist/accel, quake, cloud, rain, overcast, param; not the time of day
		const uint32_t weather[7] = { f.env[0], f.env[1], f.env[3], f.env[4], f.env[5], f.env[6],
			f.env[7] };
		return "1:" + join(weather, 7);
	});
	both("frame_objective", "FRAME_OBJECTIVE_PHASE_MISMATCH", [](const F &f) {
		return f.objective_present ? "1:" + join(f.objective, 4) : std::string("0");
	});
	// The mounted-ammo record rides only the frame whose route nibble is 8
	// [orig: NetPacket_WritePlayerState @0x4FF6B0 (the test @0x4FFD94..0x4FFD9A)],
	// one in 16, and reads the body's mount word +0x16C (@0x4FFDA4; 0xFFFF when null,
	// @0x4FFEC7), the word the recipient's own record names as its carrier with its
	// attach bone (else the ground entity, bone 0) [orig: NetPacket_SerializePlayerState
	// @0x4C09C0 (@0x4C0A00..0x4C0A08, @0x4C0A1A)]. Each record is keyed by the own
	// record's carrier state in its frame, and a state compares when both runs carried a
	// record in it or both stayed in it a full cycle: a shorter stay shows its record only
	// when a nibble-8 frame falls inside it (frame.mount_phase).
	static_assert(has_normalization("frame.mount_phase"));
	{
		struct MountFacts {
			std::map<std::string, size_t> dwell; // each state's longest run of frames
			std::map<std::string, std::set<std::string>> carried;
			bool absent = false; // a frame without the record
		};
		auto facts = [](const RunFacts &run, const std::vector<const F *> &frames) {
			MountFacts out;
			uint32_t own = 0;
			const bool known = own_handle(run, own);
			std::map<int, const wire::ParityEntity *> self;
			for (const wire::ParityEntity &e : run.trace.entities)
				if (known && e.cls == 'P' && e.handle == own && run.in_steady(e.ts_ns))
					self[e.frame] = &e;
			std::string last;
			size_t stay = 0;
			for (const F *f : frames) {
				const auto found = self.find(f->frame);
				std::string state = "-";
				if (found != self.end())
					state = found->second->carrier == 0xFFFF
							? std::string("none")
							: "bound:" + std::to_string(found->second->carrier_bone);
				stay = state == last ? stay + 1 : 1;
				last = state;
				out.dwell[state] = std::max(out.dwell[state], stay);
				if (!f->mount_present) {
					out.absent = true;
					continue;
				}
				const bool bound = f->mount_handle != 0xFFFF;
				out.carried[state].insert(std::string(bound ? "bound:" : "none:") +
						(f->mount_bound ? "1:" : "0:") +
						(bound ? std::to_string(f->mount_clip) + ":" +
												std::to_string(f->mount_reserve)
							   : std::string("-:-")));
			}
			return out;
		};
		auto keyed = [](const MountFacts &run, const MountFacts &other) {
			std::set<std::string> out;
			if (run.absent)
				out.insert("0");
			for (const auto &entry : run.carried) {
				// A full route cycle: 16 frames.
				const auto cycled = [&entry](const MountFacts &m) {
					const auto d = m.dwell.find(entry.first);
					return d != m.dwell.end() && d->second > size_t(kFrameFlags2RouteMask);
				};
				if (!other.carried.count(entry.first) && !(cycled(run) && cycled(other)))
					continue;
				for (const std::string &value : entry.second)
					out.insert(entry.first + "/" + value);
			}
			return out;
		};
		const MountFacts fa = facts(rr, a), fb = facts(other, b);
		set_metric(out, 'S', "frame_mount", "FRAME_PASSENGER_PHASE_MISMATCH", keyed(fa, fb),
				keyed(fb, fa));
	}
	if (a.size() < kRouteCycle || b.size() < kRouteCycle) {
		MetricValue &mount = out.back();
		mount.comparable = false;
		mount.reference_short = a.size() < kRouteCycle;
		mount.detail = "fewer than 16 steady frames: the mount record is a phase slice";
	}

	auto any = [](const std::vector<const F *> &frames, const std::function<bool(const F &)> &f) {
		for (const F *frame : frames)
			if (f(*frame))
				return true;
		return false;
	};
	auto anchor = [](const F &f) { return f.anchor_x != 0 || f.anchor_y != 0 || f.anchor_z != 0; };
	fill_metric(out, 'S', "frame_anchor_fill", "FRAME_ANCHOR_ZERO", any(a, anchor), any(b, anchor));
	// The CPU share is a measurement, but a host that never writes one is not
	// (Server_TickUpdate @0x51D7EA..0x51D7FB, NetPacket_WritePlayerState
	// @0x4FFA76..0x4FFA7C).
	auto cpu = [](const F &f) { return f.timer_present && f.timer_state[3] != 0; };
	fill_metric(out, 'S', "frame_server_cpu_fill", "FRAME_TIMER_PHASE_MISMATCH", any(a, cpu),
			any(b, cpu));
	auto samples = [](const std::vector<const F *> &frames) {
		std::vector<Sample> out;
		for (const F *f : frames)
			out.push_back(
					{ "", { double(f->anchor_x), double(f->anchor_y), double(f->anchor_z) } });
		return out;
	};
	dynamic_metrics(out, 'S', "frame_anchor", samples(a), samples(b), 0.0, true);
}

std::vector<const wire::ParityEntity *> steady_entities(const RunFacts &run) {
	std::vector<const wire::ParityEntity *> out;
	for (const wire::ParityEntity &entity : run.trace.entities)
		if (run.in_steady(entity.ts_ns))
			out.push_back(&entity);
	return out;
}

void compare_entities(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("entity.handle"));
	const auto a = steady_entities(rr), b = steady_entities(other);
	if (a.empty() && b.empty())
		return;
	using E = wire::ParityEntity;
	auto types = [](const std::vector<const E *> &entities) {
		std::set<std::string> out;
		for (const E *e : entities)
			out.insert(std::string(1, e->cls) + ":" + std::to_string(e->type_id));
		return out;
	};
	set_metric(out, 'S', "entity_types", "FRAME_ENTITY_TYPES_MISMATCH", types(a), types(b));

	// diff_0a's per-class field population: a field RR ever fills that the run never does.
	auto populated = [](const std::vector<const E *> &entities) {
		std::set<std::string> out;
		for (const E *e : entities) {
			const std::string cls(1, e->cls);
			if (e->pos_x || e->pos_y || e->pos_z)
				out.insert(cls + ".pos");
			if (e->orient_a || e->orient_b)
				out.insert(cls + ".orient");
			if (e->state_a || e->state_b)
				out.insert(cls + ".state");
			if (e->vital)
				out.insert(cls + ".vital");
		}
		return out;
	};
	const std::set<std::string> pa = populated(a), pb = populated(b);
	MetricValue population = metric('S', "entity_field_population", "", "ENTITY_FIELD_EMPTY");
	std::set<std::string> empty;
	for (const std::string &field : pa)
		if (!pb.count(field))
			empty.insert(field);
	population.distance = double(empty.size());
	population.subject = render_set(pb);
	population.reference = render_set(pa);
	population.detail = empty.empty() ? "" : "RR fills, the run leaves empty: " + render_set(empty);
	out.push_back(population);

	// Positions compare in world 16.16: a compact code is sign/exponent/mantissa
	// relative to the recipient's eye anchor (state.position), so code arithmetic is
	// no distance and every code moves with the recipient. A record that names a
	// carrier carries its position and heading local to that carrier instead, never
	// anchor-relative (entity.carrier): each handle's records run one sequence per
	// carrier word, so a carrier-local offset is a position in its carrier's frame and
	// no step or motion key spans two frames.
	static_assert(has_normalization("entity.carrier"));
	for (const char cls : { 'P', 'V', 'I' }) {
		std::vector<const E *> ca, cb;
		for (const E *e : a)
			if (e->cls == cls)
				ca.push_back(e);
		for (const E *e : b)
			if (e->cls == cls)
				cb.push_back(e);
		if (ca.empty() && cb.empty())
			continue;
		const std::string key(1, cls);
		const std::function<std::string(const E &)> sequence = [](const E &e) {
			return std::to_string(e.handle) +
					(e.carrier_local ? "@" + std::to_string(e.carrier) : std::string());
		};
		const std::function<std::string(const E &)> position = [](const E &e) {
			return std::to_string(e.world_x) + "," + std::to_string(e.world_y) + "," +
					std::to_string(e.world_z);
		};
		fill_metric(out, 'S', "entity_motion", "FRAME_ENTITY_CONSTANT",
				changing_handles(ca, sequence, position) > 0,
				changing_handles(cb, sequence, position) > 0, key);
		auto samples = [&sequence](const std::vector<const E *> &entities, bool orientation) {
			std::vector<Sample> out;
			for (const E *e : entities) {
				Sample sample;
				sample.group = sequence(*e);
				if (orientation) {
					sample.axes = { double(e->orient_a), double(e->orient_b) };
				} else {
					// Each axis's step: 16 << ((code & 0x0E) | 1) [orig: Network_CompressFixedPoint
					// @0x4C2780 (the shift @0x4C27A0..0x4C27AC, stored & 0x0E @0x4C27B5)].
					auto step = [](uint32_t code) { return double(16u << ((code & 0x0Eu) | 1u)); };
					sample.axes = { double(e->world_x), double(e->world_y), double(e->world_z) };
					sample.quantum = { step(e->pos_x), step(e->pos_y), step(e->pos_z) };
				}
				out.push_back(sample);
			}
			return out;
		};
		const double modulus = cls == 'V' ? 65536.0 : 256.0;
		dynamic_metrics(out, 'S', std::string("entity_") + cls + "_position", samples(ca, false),
				samples(cb, false), 0.0, false);
		dynamic_metrics(out, 'S', std::string("entity_") + cls + "_orientation", samples(ca, true),
				samples(cb, true), modulus, false);
	}
}

// The anchor's own offset: the recipient's own record, decompressed, in 1/16 units.
// Retail anchors every frame at the recipient's Position + CameraOffset
// [orig: Server_SendEntityStateToPlayer @0x517BA0 (@0x517BEF..0x517C13)], so its own record
// encodes -CameraOffset (the standing eye: z -0xD001); positions in world units no longer
// show where the anchor sits, this does. Between two rests the eye moves along one curve
// a tick at a time (a death's fall, a hit's tilt decaying), and the recipient's frames
// sample it at its own send cadence (the per-slot counter @0x517BE8): which points of a
// curve a window holds is that phase. So a rest (the same offset in two or more
// consecutive records) compares as its key, and the records between two rests as one
// transition 'from>to'; a carrier-local record cuts the sequence, and a transition the
// cut or the window's edge leaves open is not compared. A dead body's eye is its death
// clip's: the clip is the killing round's hit section and approach quadrant [orig:
// OrganicClass_HandleEvent @0x407310 (the quadrant @0x407478, Entity_ComputeAnimSlotIndex
// @0x43A690 called @0x40747E, the store @0x407483)] and the eye the clip's capsule extent,
// at most 0xD000 [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 (@0x4B6984..0x4B6997)].
// So a rest none of whose records plays a clip the other run's own record plays is that
// hit's draw: it compares as 'drawn', and two drawn rests make no transition
// (frame.self_offset_rests).
void compare_self_offset(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("state.position"));
	static_assert(has_normalization("frame.self_offset_rests"));
	using E = wire::ParityEntity;
	// The own record's clip: its anim state id (the player record's state_b high bits).
	auto clip = [](const E &e) { return e.state_b >> 8; };
	struct Own {
		std::vector<std::vector<const E *>> segments = std::vector<std::vector<const E *>>(1);
		std::set<uint32_t> clips;
		size_t records = 0;
	};
	auto own_records = [&clip](const RunFacts &run) {
		Own out;
		uint32_t handle = 0;
		if (!own_handle(run, handle))
			return out;
		for (const E &e : run.trace.entities) {
			if (e.cls != 'P' || e.handle != handle || !run.in_steady(e.ts_ns))
				continue;
			if (e.carrier_local) {
				if (!out.segments.back().empty())
					out.segments.emplace_back();
				continue;
			}
			out.segments.back().push_back(&e);
			out.clips.insert(clip(e));
			++out.records;
		}
		return out;
	};
	auto offsets = [&clip](const Own &run, const Own &other) {
		std::set<std::string> out;
		auto sixteenths = [](int32_t v) { return std::to_string(std::lround(double(v) / 4096.0)); };
		auto key = [&sixteenths](const E &e) {
			return sixteenths(e.offset_x) + "," + sixteenths(e.offset_y) + "," +
					sixteenths(e.offset_z);
		};
		auto same = [](const E &x, const E &y) {
			return x.offset_x == y.offset_x && x.offset_y == y.offset_y && x.offset_z == y.offset_z;
		};
		for (const std::vector<const E *> &segment : run.segments) {
			std::vector<std::pair<size_t, size_t>> rests; // first and last record of each
			for (size_t i = 0; i < segment.size();) {
				size_t j = i;
				while (j + 1 < segment.size() && same(*segment[j + 1], *segment[i]))
					++j;
				if (j > i)
					rests.emplace_back(i, j);
				i = j + 1;
			}
			auto drawn = [&](const std::pair<size_t, size_t> &rest) {
				for (size_t i = rest.first; i <= rest.second; ++i)
					if (other.clips.count(clip(*segment[i])))
						return false;
				return true;
			};
			for (size_t r = 0; r < rests.size(); ++r) {
				const bool drawn_at = drawn(rests[r]);
				const std::string at = drawn_at ? "drawn" : key(*segment[rests[r].first]);
				out.insert(at);
				if (r == 0)
					continue;
				const bool drawn_from = drawn(rests[r - 1]);
				if (drawn_from && drawn_at)
					continue;
				const std::string from = drawn_from ? "drawn" : key(*segment[rests[r - 1].first]);
				if (from != at || rests[r].first > rests[r - 1].second + 1)
					out.insert(from + ">" + at);
			}
		}
		return out;
	};
	const Own own_a = own_records(rr), own_b = own_records(other);
	if (own_a.records == 0)
		return;
	MetricValue value = metric('S', "frame_self_offset", "", "FRAME_ANCHOR_OFFSET_MISMATCH");
	const std::set<std::string> a = offsets(own_a, own_b), b = offsets(own_b, own_a);
	value.subject = render_set(b);
	value.reference = render_set(a);
	value.detail = "the recipient's own record against the anchor, 1/16 units: its rests and "
				   "the transitions between them, a rest in a clip only one run plays 'drawn'";
	if (own_b.records == 0) {
		value.comparable = false;
		value.detail = "the run's recipient record is not in its steady frames";
	} else {
		value.distance = jaccard_distance(a, b);
	}
	out.push_back(value);
}

// ---------------------------------------------------------------------------
// S2C 0x0A round events

// Each handle's entity records in time: the class a round's shooter resolves to.
using ClassTimeline = std::map<uint32_t, std::vector<std::pair<uint64_t, char>>>;

ClassTimeline class_timeline(const RunFacts &run) {
	ClassTimeline out;
	for (const wire::ParityEntity &e : run.trace.entities)
		out[e.handle].emplace_back(e.ts_ns, e.cls);
	for (auto &entry : out)
		std::stable_sort(entry.second.begin(), entry.second.end(),
				[](const auto &x, const auto &y) { return x.first < y.first; });
	return out;
}

// The class of the shooter's latest record at or before the round (its own frame's
// first), else of its first later one; 'U' when the trace never carries it.
char shooter_class(const ClassTimeline &timeline, const wire::ParityRoundEvent &round) {
	const auto found = timeline.find(round.shooter);
	if (found == timeline.end())
		return 'U';
	const auto &records = found->second;
	const auto after = std::upper_bound(records.begin(), records.end(), round.ts_ns,
			[](uint64_t ts, const std::pair<uint64_t, char> &record) { return ts < record.first; });
	return after == records.begin() ? records.front().second : std::prev(after)->second;
}

// A round's kind: its shooter's class and its fire leg, the flag byte's adm-indexed bit
// ("adm"), else its alt-fire bit ("alt"), else "plain" (ingame_decode.h's round flags).
std::string round_kind(char cls, uint32_t flags) {
	const char *leg = (flags & kRoundEventFlagAdmIndexed) ? "adm"
			: (flags & kRoundEventFlagAltFire)			  ? "alt"
														  : "plain";
	return std::string(1, cls) + "." + leg;
}

struct RoundFacts {
	std::map<std::string, size_t> rounds; // per kind: its steady rounds
	std::map<std::string, size_t> targeted; // per kind: those carrying a target
	std::map<std::string, std::set<std::string>> relations; // per kind: whom they name
};

RoundFacts round_facts(const RunFacts &run) {
	RoundFacts out;
	const ClassTimeline timeline = class_timeline(run);
	uint32_t self = 0;
	const bool has_self = own_handle(run, self);
	for (const wire::ParityRoundEvent &round : run.trace.rounds) {
		if (!run.in_steady(round.ts_ns))
			continue;
		const std::string kind = round_kind(shooter_class(timeline, round), round.flags);
		++out.rounds[kind];
		if (!(round.flags & kRoundEventHasTargetHandle))
			continue;
		++out.targeted[kind];
		const char *relation = "other";
		if (round.target == 0xFFFF)
			relation = "unpooled";
		else if (has_self && round.target == self)
			relation = "self";
		out.relations[kind].insert(relation);
	}
	return out;
}

// The round events' fire target. Retail's tag-2 writer sets 0x40 when the shooter's AI slot
// word (+0x68 -> +0xC) names a target and then writes that target's pool handle, 0xFFFF for
// a pointer in no pool [orig: NetPacket_SerializeRoundEvent @0x504820 (the reads @0x50484E /
// @0x504855, the pool walk @0x50485C..0x5048A7, 0xFFFF @0x504882..0x504886, the flag
// @0x5048BF..0x5048C1, the word @0x50494D..0x504966)]. The word is read live at the send, so
// a shooter's rounds before it acquires a target go without one: how many carry it is the
// acquisition's phase and is not compared. Whether a kind of shooter's rounds ever carry a
// target, and whom they name, is its AI's (round.target):
//   round_target <kind>           RR's rounds of the kind carry a target and the run's never
//                                 do (ROUND_TARGET_MISSING), or the run's do and RR's never
//                                 (ROUND_TARGET_UNEXPECTED); a side with no rounds of the
//                                 kind in its steady window is short;
//   round_target_relation <kind>  whom the targets name: self (the recipient's own body, the
//                                 handle its uplinks name), unpooled (0xFFFF) or other; a
//                                 side with no target of the kind leaves it to round_target.
void compare_rounds(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("round.target"));
	const RoundFacts a = round_facts(rr), b = round_facts(other);
	std::set<std::string> kinds;
	for (const auto &entry : a.rounds)
		kinds.insert(entry.first);
	for (const auto &entry : b.rounds)
		kinds.insert(entry.first);
	auto count = [](const std::map<std::string, size_t> &counts, const std::string &kind) {
		const auto found = counts.find(kind);
		return found == counts.end() ? size_t(0) : found->second;
	};
	auto relations = [](const RoundFacts &facts, const std::string &kind) {
		const auto found = facts.relations.find(kind);
		return found == facts.relations.end() ? std::set<std::string>() : found->second;
	};
	auto fill = [](size_t rounds, size_t targeted) {
		return (targeted ? std::string("filled (") : std::string("never filled (")) +
				std::to_string(targeted) + " of " + std::to_string(rounds) + ")";
	};
	for (const std::string &kind : kinds) {
		const size_t na = count(a.rounds, kind), nb = count(b.rounds, kind);
		const size_t ta = count(a.targeted, kind), tb = count(b.targeted, kind);
		MetricValue target = metric('S', "round_target", kind,
				tb == 0 ? "ROUND_TARGET_MISSING" : "ROUND_TARGET_UNEXPECTED");
		target.distance = (ta != 0) != (tb != 0) ? 1.0 : 0.0;
		target.subject = fill(nb, tb);
		target.reference = fill(na, ta);
		if (na == 0 || nb == 0) {
			target.comparable = false;
			target.reference_short = na == 0;
			target.detail = std::string(na == 0 ? "RR's" : "the run's") +
					" steady window holds no round of this kind";
		}
		out.push_back(target);

		MetricValue relation =
				metric('S', "round_target_relation", kind, "ROUND_TARGET_RELATION_MISMATCH");
		const std::set<std::string> ra = relations(a, kind), rb = relations(b, kind);
		relation.subject = render_set(rb);
		relation.reference = render_set(ra);
		if (ta == 0 || tb == 0) {
			relation.comparable = false;
			relation.reference_short = true;
			relation.detail = "a side's rounds of this kind name no target: round_target judges it";
		} else {
			relation.distance = jaccard_distance(ra, rb);
		}
		out.push_back(relation);
	}
}

} // namespace

void compare_state(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	compare_uplinks(rr, other, out);
	compare_frames(rr, other, out);
	compare_entities(rr, other, out);
	compare_self_offset(rr, other, out);
	compare_rounds(rr, other, out);
}

} // namespace opennova::parity
