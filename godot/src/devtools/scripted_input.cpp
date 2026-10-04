#include "devtools/scripted_input.h"

#include "util/string_convert.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <runtime/controls/controls.h>

#include <cmath>
#include <string>
#include <vector>

namespace godot {

namespace {

using Step = opennova::devtools::ScriptStep;

// A JSON number is a double: an integral one is an integer.
bool integral(const Variant &p_value, int64_t &r_out) {
	if (p_value.get_type() == Variant::INT) {
		r_out = p_value;
		return true;
	}
	if (p_value.get_type() != Variant::FLOAT) return false;
	const double value = p_value;
	if (!std::isfinite(value) || value != std::floor(value) || std::fabs(value) > 9.0e15) return false;
	r_out = static_cast<int64_t>(value);
	return true;
}

bool number(const Variant &p_value, double &r_out) {
	if (p_value.get_type() == Variant::INT) {
		r_out = static_cast<double>(static_cast<int64_t>(p_value));
		return true;
	}
	if (p_value.get_type() != Variant::FLOAT) return false;
	r_out = p_value;
	return std::isfinite(r_out);
}

// The step's shape, or the refusal naming what is wrong with it.
String marshal_step(const Dictionary &p_step, Step &r_out) {
	static const char *const kActions[] = {"down", "up", "press", "look_px", "end"};
	if (!p_step.has("tick") || !integral(p_step["tick"], r_out.tick)) {
		return "\"tick\" must be an integer";
	}
	int actions = 0;
	for (const char *key : kActions) {
		if (p_step.has(key)) ++actions;
	}
	if (actions != 1) return "needs exactly one of down / up / press / look_px / end";
	if (p_step.has("look_px")) {
		const Variant look = p_step["look_px"];
		const Array pair = look.get_type() == Variant::ARRAY ? Array(look) : Array();
		if (pair.size() != 2 || !number(pair[0], r_out.dx_px) || !number(pair[1], r_out.dy_px)) {
			return "\"look_px\" must be [dx, dy] numbers";
		}
		r_out.kind = Step::Kind::Look;
		return String();
	}
	if (p_step.has("end")) {
		const Variant end = p_step["end"];
		if (end.get_type() != Variant::BOOL || !bool(end)) return "\"end\" must be true";
		r_out.kind = Step::Kind::End;
		return String();
	}
	const char *key = p_step.has("down") ? "down" : (p_step.has("up") ? "up" : "press");
	int64_t code = 0;
	if (!integral(p_step[key], code) || code < 0 || code > 0xFFFF) {
		return String("\"") + key + "\" must be an action code";
	}
	r_out.code = static_cast<int>(code);
	r_out.kind = p_step.has("down") ? Step::Kind::Down
			: (p_step.has("up") ? Step::Kind::Up : Step::Kind::Press);
	return String();
}

} // namespace

Error ScriptedInput::load_steps(const Array &p_steps) {
	std::vector<Step> steps;
	steps.reserve(static_cast<size_t>(p_steps.size()));
	for (int64_t i = 0; i < p_steps.size(); ++i) {
		const Variant item = p_steps[i];
		Step step;
		const String refusal = item.get_type() == Variant::DICTIONARY
				? marshal_step(Dictionary(item), step)
				: String("not an object");
		if (!refusal.is_empty()) {
			error_ = "step " + String::num_int64(i) + ": " + refusal;
			return ERR_INVALID_PARAMETER;
		}
		steps.push_back(step);
	}
	std::string error;
	if (!driver_.load(steps, error)) {
		error_ = opennova::to_gd(error);
		return ERR_INVALID_PARAMETER;
	}
	error_ = String();
	return OK;
}

bool ScriptedInput::is_token_held(const String &p_token) const {
	const CharString token = p_token.utf8();
	return driver_.token_held(token.get_data());
}

Vector2 ScriptedInput::take_look() {
	double dx = 0.0;
	double dy = 0.0;
	driver_.take_look(dx, dy);
	return Vector2(static_cast<real_t>(dx), static_cast<real_t>(dy));
}

String ScriptedInput::get_state_name() const {
	using State = opennova::devtools::ScriptedInput::State;
	switch (driver_.state()) {
		case State::Idle: return driver_.loaded() ? "loaded" : "idle";
		case State::Armed: return "armed";
		case State::Running: return "running";
		case State::Finished: return "finished";
		case State::Cancelled: return "cancelled";
	}
	return "idle";
}

bool ScriptedInput::is_done() const {
	using State = opennova::devtools::ScriptedInput::State;
	return driver_.state() == State::Finished || driver_.state() == State::Cancelled;
}

String ScriptedInput::get_cancel_reason() const {
	return opennova::to_gd(driver_.cancel_reason());
}

PackedInt64Array ScriptedInput::get_applied_logic_ticks() const {
	PackedInt64Array out;
	for (const opennova::devtools::ScriptStepRecord &record : driver_.records()) {
		out.push_back(record.applied_logic_tick);
	}
	return out;
}

PackedInt64Array ScriptedInput::get_release_logic_ticks() const {
	PackedInt64Array out;
	for (const opennova::devtools::ScriptStepRecord &record : driver_.records()) {
		out.push_back(record.release_logic_tick);
	}
	return out;
}

PackedInt32Array ScriptedInput::get_held_codes() const {
	PackedInt32Array out;
	for (const int code : driver_.held_codes()) out.push_back(code);
	return out;
}

String ScriptedInput::token_for_action_code(int p_code) {
	const opennova::controls::ActionDef *def = opennova::controls::action_for_code(p_code);
	return def != nullptr ? String::utf8(def->token) : String();
}

void ScriptedInput::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_steps", "steps"), &ScriptedInput::load_steps);
	ClassDB::bind_method(D_METHOD("get_error"), &ScriptedInput::get_error);
	ClassDB::bind_method(D_METHOD("start"), &ScriptedInput::start);
	ClassDB::bind_method(D_METHOD("cancel"), &ScriptedInput::cancel);
	ClassDB::bind_method(D_METHOD("advance", "logic_tick"), &ScriptedInput::advance);
	ClassDB::bind_method(D_METHOD("is_token_held", "token"), &ScriptedInput::is_token_held);
	ClassDB::bind_method(D_METHOD("take_look"), &ScriptedInput::take_look);
	ClassDB::bind_method(D_METHOD("get_state"), &ScriptedInput::get_state);
	ClassDB::bind_method(D_METHOD("get_state_name"), &ScriptedInput::get_state_name);
	ClassDB::bind_method(D_METHOD("is_done"), &ScriptedInput::is_done);
	ClassDB::bind_method(D_METHOD("get_start_logic_tick"), &ScriptedInput::get_start_logic_tick);
	ClassDB::bind_method(D_METHOD("get_end_logic_tick"), &ScriptedInput::get_end_logic_tick);
	ClassDB::bind_method(D_METHOD("get_cancel_reason"), &ScriptedInput::get_cancel_reason);
	ClassDB::bind_method(D_METHOD("get_applied_logic_ticks"), &ScriptedInput::get_applied_logic_ticks);
	ClassDB::bind_method(D_METHOD("get_release_logic_ticks"), &ScriptedInput::get_release_logic_ticks);
	ClassDB::bind_method(D_METHOD("get_held_codes"), &ScriptedInput::get_held_codes);
	ClassDB::bind_static_method("ScriptedInput", D_METHOD("token_for_action_code", "code"),
			&ScriptedInput::token_for_action_code);
	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_ARMED);
	BIND_ENUM_CONSTANT(STATE_RUNNING);
	BIND_ENUM_CONSTANT(STATE_FINISHED);
	BIND_ENUM_CONSTANT(STATE_CANCELLED);
}

} // namespace godot
