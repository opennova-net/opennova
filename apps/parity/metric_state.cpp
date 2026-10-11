// The decoded-state metrics over the steady window: the C2S 0x0C uplink and the
// S2C 0x0A frame header and per-entity records (the 0x0A shape report diff_0a.py
// printed, folded in and made failing). A documented subset of the retired
// verifier's Compare-SemanticDirection:
//   exact value sets   uplink shape/ADM/state/move/carrier/priority; frame
//                      sub-block, flags, record classes, local state, health,
//                      state word, carried, weapon/timer/env/objective/mount
//                      blocks; entity class:type coverage
//   zero / constant    a field RR fills that the run leaves all zero, or a
//                      position RR changes that the run never does
//   field population   per entity class, the fields RR ever fills (diff_0a)
//   dynamic vectors    origin-normalized range, displacement and step p90 within
//                      1/8x..8x of RR; initial position/anchor within 64 units
// Session-local handles reduce to none/bound (normalization.h).

#include "metric.h"

#include "normalization.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
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

double p90(std::vector<double> values) {
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	return values[size_t(std::floor(double(values.size() - 1) * 0.9))];
}

// `samples` in capture order.
VectorStats vector_stats(const std::vector<Sample> &samples, double modulus) {
	VectorStats stats;
	stats.samples = samples.size();
	if (!samples.empty())
		stats.baseline = samples.front().axes;
	std::map<std::string, std::vector<const Sample *>> groups;
	for (const Sample &sample : samples)
		groups[sample.group].push_back(&sample);
	std::vector<double> ranges, displacements, steps;
	for (const auto &group : groups) {
		const std::vector<const Sample *> &seq = group.second;
		if (seq.size() < 2)
			continue;
		const size_t axes = seq.front()->axes.size();
		std::vector<double> lo(axes, 0.0), hi(axes, 0.0);
		for (size_t i = 0; i < seq.size(); ++i) {
			double displacement = 0.0, step = 0.0;
			for (size_t a = 0; a < axes; ++a) {
				const double from_origin = wrapped(seq[i]->axes[a] - seq.front()->axes[a], modulus);
				displacement += std::fabs(from_origin);
				lo[a] = std::min(lo[a], from_origin);
				hi[a] = std::max(hi[a], from_origin);
				if (i > 0)
					step += std::fabs(wrapped(seq[i]->axes[a] - seq[i - 1]->axes[a], modulus));
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
		value.detail = "the first sample's largest axis delta (16.16)";
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

// Handles whose key changes over the samples (a moving or turning entity).
template <typename T>
size_t changing_handles(const std::vector<const T *> &records,
		const std::function<uint32_t(const T &)> &handle,
		const std::function<std::string(const T &)> &key) {
	std::map<uint32_t, std::set<std::string>> seen;
	for (const T *record : records)
		seen[handle(*record)].insert(key(*record));
	size_t changing = 0;
	for (const auto &entry : seen)
		changing += entry.second.size() > 1 ? 1 : 0;
	return changing;
}

// ---------------------------------------------------------------------------
// C2S 0x0C

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
	both("uplink_priority", "UPLINK_PRIORITY_MISMATCH", [](const U &s) {
		std::string key;
		for (int slot = 0; slot < 4; ++slot) {
			const bool occupied = s.priority_handle[slot] != 0 || s.priority_score[slot] != 0;
			key += (slot ? "," : "") + std::string(occupied ? "occupied:" : "empty:") +
					std::to_string(s.priority_score[slot]);
		}
		return key;
	});

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
	const std::function<uint32_t(const U &)> handle = [](const U &s) { return s.handle; };
	const std::function<std::string(const U &)> position = [](const U &s) {
		return std::to_string(s.pos_x) + "," + std::to_string(s.pos_y) + "," +
				std::to_string(s.pos_z);
	};
	fill_metric(out, 'C', "uplink_motion", "UPLINK_CONSTANT",
			changing_handles(a, handle, position) > 0, changing_handles(b, handle, position) > 0);

	auto samples = [](const std::vector<const U *> &states, int which) {
		std::vector<Sample> out;
		for (const U *s : states) {
			Sample sample;
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

std::vector<const wire::ParityFrameState *> steady_frames(const RunFacts &run) {
	std::vector<const wire::ParityFrameState *> out;
	for (const wire::ParityFrameState &state : run.trace.frames)
		if (state.decoded && run.in_steady(state.ts_ns))
			out.push_back(&state);
	return out;
}

void compare_frames(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("frame.carried"));
	static_assert(has_normalization("frame.mount_handle"));
	static_assert(has_normalization("frame.timer_seconds"));
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
	both("frame_flags2", "FRAME_HEADER_FLAGS_MISMATCH",
			[](const F &f) { return std::to_string(f.flags2); });
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
	both("frame_health", "FRAME_HEADER_VALUES_MISMATCH",
			[](const F &f) { return std::to_string(f.health); });
	both("frame_state_word", "FRAME_HEADER_VALUES_MISMATCH",
			[](const F &f) { return std::to_string(f.state_word); });
	both("frame_carried", "FRAME_HEADER_VALUES_MISMATCH",
			[](const F &f) { return std::string(f.carried == 0xFFFF ? "none" : "bound"); });
	both("frame_weapon", "FRAME_WEAPON_PHASE_MISMATCH", [](const F &f) {
		return f.weapon_present
				? "1:" + join(f.weapon, 7) + "," + std::to_string(f.weapon_uniform_team_mask)
				: std::string("0");
	});
	both("frame_timer", "FRAME_TIMER_PHASE_MISMATCH", [](const F &f) {
		return f.timer_present ? "1:" + join(f.timer_state, 4) : std::string("0");
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
	both("frame_mount", "FRAME_PASSENGER_PHASE_MISMATCH", [](const F &f) {
		if (!f.mount_present)
			return std::string("0");
		const bool bound = f.mount_handle != 0xFFFF;
		return std::string(bound ? "bound:" : "none:") + (f.mount_bound ? "1:" : "0:") +
				(bound ? std::to_string(f.mount_clip) + ":" + std::to_string(f.mount_reserve)
					   : std::string("-:-"));
	});

	auto any = [](const std::vector<const F *> &frames, const std::function<bool(const F &)> &f) {
		for (const F *frame : frames)
			if (f(*frame))
				return true;
		return false;
	};
	auto anchor = [](const F &f) { return f.anchor_x != 0 || f.anchor_y != 0 || f.anchor_z != 0; };
	fill_metric(out, 'S', "frame_anchor_fill", "FRAME_ANCHOR_ZERO", any(a, anchor), any(b, anchor));
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
		const std::function<uint32_t(const E &)> handle = [](const E &e) { return e.handle; };
		const std::function<std::string(const E &)> position = [](const E &e) {
			return std::to_string(e.pos_x) + "," + std::to_string(e.pos_y) + "," +
					std::to_string(e.pos_z);
		};
		fill_metric(out, 'S', "entity_motion", "FRAME_ENTITY_CONSTANT",
				changing_handles(ca, handle, position) > 0,
				changing_handles(cb, handle, position) > 0, key);
		auto samples = [](const std::vector<const E *> &entities, bool orientation) {
			std::vector<Sample> out;
			for (const E *e : entities) {
				Sample sample;
				sample.group = std::to_string(e->handle);
				if (orientation)
					sample.axes = { double(e->orient_a), double(e->orient_b) };
				else
					sample.axes = { double(e->pos_x), double(e->pos_y), double(e->pos_z) };
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

} // namespace

void compare_state(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	compare_uplinks(rr, other, out);
	compare_frames(rr, other, out);
	compare_entities(rr, other, out);
}

} // namespace opennova::parity
