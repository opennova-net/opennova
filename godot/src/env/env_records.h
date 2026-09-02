#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// The hardcoded sunrise / sunset windows at one clock time
// (EnvFile.get_day_phase; engine: formats/env/env_render.cpp): whether the
// time is night and the day blend the render folds by.
class EnvDayPhase : public RefCounted {
	GDCLASS(EnvDayPhase, RefCounted)

public:
	bool is_night() const { return night_; }
	void set_night(bool p_night) { night_ = p_night; }
	float get_blend() const { return blend_; }
	void set_blend(float p_blend) { blend_ = p_blend; }

protected:
	static void _bind_methods();

private:
	bool night_ = false;
	float blend_ = 0.0f;
};

// The sun glare from view-sun alignment and occlusion brightness
// (EnvFile.compute_sun_glare; engine: formats/env/env_celestial.h): the glare
// byte (0..255) and the fog whitening (0..40).
class EnvSunGlare : public RefCounted {
	GDCLASS(EnvSunGlare, RefCounted)

public:
	int get_glare() const { return glare_; }
	void set_glare(int p_glare) { glare_ = p_glare; }
	int get_fog_whiten() const { return fog_whiten_; }
	void set_fog_whiten(int p_value) { fog_whiten_ = p_value; }

protected:
	static void _bind_methods();

private:
	int glare_ = 0;
	int fog_whiten_ = 0;
};

} // namespace godot
