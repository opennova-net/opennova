#include <runtime/devtools/scripted_input.h>

#include <runtime/controls/controls.h>
#include <runtime/world/angle.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace opennova::devtools {
namespace {

std::string step_label(size_t index) { return "step " + std::to_string(index); }

bool position_q16(double value, int32_t &out) {
	if (!std::isfinite(value)) return false;
	const long double scaled = static_cast<long double>(value) * 65536.0L;
	if (scaled < static_cast<long double>(INT32_MIN) || scaled > static_cast<long double>(INT32_MAX))
		return false;
	out = static_cast<int32_t>(std::trunc(scaled));
	return true;
}

} // namespace

bool script_pose_from_mission(double x, double y, double z, double yaw_deg, double pitch_deg,
		ScriptPose &out, std::string &error) {
	const double axes[3] = {x, y, z};
	for (int axis = 0; axis < 3; ++axis) {
		if (!position_q16(axes[axis], out.position_q16[axis])) {
			error = "the setup position is outside the 16.16 range";
			return false;
		}
	}
	if (!std::isfinite(yaw_deg)) {
		error = "the setup yaw is not a finite number";
		return false;
	}
	if (!std::isfinite(pitch_deg) || pitch_deg < -90.0 || pitch_deg > 90.0) {
		error = "the setup pitch must be between -90 and 90 degrees";
		return false;
	}
	out.heading_bam = world::bam_heading_from_mission_yaw_deg(yaw_deg);
	out.pitch_bam = static_cast<int32_t>(std::trunc(pitch_deg * world::kBamPerDegree));
	return true;
}

bool ScriptedInput::load(const std::vector<ScriptStep> &steps, std::string &error) {
	std::vector<Event> events;
	events.reserve(steps.size() + 1);
	bool have_end = false;
	for (size_t i = 0; i < steps.size(); ++i) {
		const ScriptStep &s = steps[i];
		if (s.tick < 0) {
			error = step_label(i) + ": the tick must be >= 0";
			return false;
		}
		Event e;
		e.tick = s.tick;
		e.step = static_cast<uint32_t>(i);
		switch (s.kind) {
			case ScriptStep::Kind::Down:
			case ScriptStep::Kind::Up:
			case ScriptStep::Kind::Press: {
				const controls::ActionDef *def = controls::action_for_code(s.code);
				if (def == nullptr) {
					error = step_label(i) + ": no controls catalog row dispatches action code " +
							std::to_string(s.code);
					return false;
				}
				e.token = def->token;
				e.code = s.code;
				e.op = s.kind == ScriptStep::Kind::Up ? Op::Up : Op::Down;
				events.push_back(e);
				if (s.kind == ScriptStep::Kind::Press) {
					e.op = Op::Up;
					e.tick = s.tick + 1;
					e.press_release = true;
					events.push_back(e);
				}
				break;
			}
			case ScriptStep::Kind::Look:
				if (!std::isfinite(s.dx_px) || !std::isfinite(s.dy_px)) {
					error = step_label(i) + ": look_px must be two finite numbers";
					return false;
				}
				e.op = Op::Look;
				e.dx_px = s.dx_px;
				e.dy_px = s.dy_px;
				events.push_back(e);
				break;
			case ScriptStep::Kind::End:
				if (have_end) {
					error = step_label(i) + ": the script has a second end step";
					return false;
				}
				have_end = true;
				e.op = Op::End;
				events.push_back(e);
				break;
		}
	}
	if (!have_end) {
		error = "the script has no end step";
		return false;
	}
	// File order breaks a tie; a press's release follows its own press.
	std::stable_sort(events.begin(), events.end(),
			[](const Event &a, const Event &b) { return a.tick < b.tick; });
	const auto end = std::find_if(events.begin(), events.end(),
			[](const Event &e) { return e.op == Op::End; });
	if (end + 1 != events.end()) {
		const Event &late = *(end + 1);
		error = step_label(late.step) + (late.press_release ? "'s release" : "") + " at tick " +
				std::to_string(late.tick) + " falls after the end at tick " + std::to_string(end->tick);
		return false;
	}
	reset();
	events_ = std::move(events);
	records_.assign(steps.size(), ScriptStepRecord{});
	return true;
}

bool ScriptedInput::start() {
	if (events_.empty() || state_ != State::Idle) return false;
	state_ = State::Armed;
	return true;
}

void ScriptedInput::cancel() {
	if (state_ == State::Armed || state_ == State::Running) stop(State::Cancelled, "cancelled");
	held_.clear();
	look_dx_ = look_dy_ = 0.0;
}

void ScriptedInput::advance(int64_t logic_tick) {
	if (state_ == State::Armed) {
		start_tick_ = logic_tick;
		last_tick_ = logic_tick;
		state_ = State::Running;
	}
	if (state_ != State::Running) return;
	if (logic_tick < last_tick_) {
		stop(State::Cancelled, "the logic clock ran backwards (a reloaded mission)");
		look_dx_ = look_dy_ = 0.0;
		return;
	}
	last_tick_ = logic_tick;
	const int64_t relative = logic_tick - start_tick_;
	std::vector<const char *> changed;
	while (next_ < events_.size() && events_[next_].tick <= relative) {
		const Event &e = events_[next_];
		if (!apply(e, changed)) break; // waits for the next sample, and so does every later step
		ScriptStepRecord &record = records_[e.step];
		(e.press_release ? record.release_logic_tick : record.applied_logic_tick) = logic_tick;
		++next_;
		if (e.op == Op::End) {
			end_tick_ = logic_tick;
			stop(State::Finished, std::string());
			break;
		}
	}
}

bool ScriptedInput::apply(const Event &e, std::vector<const char *> &changed) {
	const auto flipped = [&changed](const char *token) {
		return std::find(changed.begin(), changed.end(), token) != changed.end();
	};
	switch (e.op) {
		case Op::Down:
		case Op::Up: {
			const auto it = std::find_if(held_.begin(), held_.end(),
					[&e](const Held &h) { return h.token == e.token; });
			const bool held = it != held_.end();
			if (held == (e.op == Op::Down)) return true; // no change
			if (flipped(e.token)) return false;
			if (held) held_.erase(it);
			else held_.push_back({e.token, e.code});
			changed.push_back(e.token);
			return true;
		}
		case Op::Look:
			look_dx_ += e.dx_px;
			look_dy_ += e.dy_px;
			return true;
		case Op::End:
			for (const Held &h : held_)
				if (flipped(h.token)) return false; // a press this sample still gets its sample
			return true;
	}
	return true;
}

void ScriptedInput::stop(State state, std::string reason) {
	state_ = state;
	cancel_reason_ = std::move(reason);
	held_.clear();
}

void ScriptedInput::reset() {
	events_.clear();
	records_.clear();
	held_.clear();
	next_ = 0;
	state_ = State::Idle;
	start_tick_ = end_tick_ = last_tick_ = -1;
	look_dx_ = look_dy_ = 0.0;
	cancel_reason_.clear();
}

bool ScriptedInput::token_held(std::string_view token) const {
	for (const Held &h : held_)
		if (token == h.token) return true;
	return false;
}

void ScriptedInput::take_look(double &r_dx_px, double &r_dy_px) {
	r_dx_px = look_dx_;
	r_dy_px = look_dy_;
	look_dx_ = look_dy_ = 0.0;
}

std::vector<int> ScriptedInput::held_codes() const {
	std::vector<int> out;
	out.reserve(held_.size());
	for (const Held &h : held_) out.push_back(h.code);
	return out;
}

} // namespace opennova::devtools
