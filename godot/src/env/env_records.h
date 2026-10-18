#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <formats/env/env_weather.h> // DayPhase

namespace godot {

// The hardcoded sunrise / sunset windows at one clock time
// (EnvFile.get_day_phase; engine: formats/env/env_render.cpp): a value
// wrapper over env::DayPhase — whether the time is night and the day blend
// the render folds by.
class EnvDayPhase : public RefCounted {
	GDCLASS(EnvDayPhase, RefCounted)

	opennova::env::DayPhase value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::env::DayPhase &p_value) { value_ = p_value; }

	bool is_night() const { return value_.is_night; }
	float get_blend() const { return value_.blend; }
};

} // namespace godot
