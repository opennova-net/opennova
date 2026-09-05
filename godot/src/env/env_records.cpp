#include "env/env_records.h"

#include "util/record_bind.h"

using namespace godot;

void EnvDayPhase::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY_IS(EnvDayPhase, night)
	OPENNOVA_RECORD_READ_ONLY(EnvDayPhase, Variant::FLOAT, blend)
}

void EnvSunGlare::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(EnvSunGlare, Variant::INT, glare)
	OPENNOVA_RECORD_READ_ONLY(EnvSunGlare, Variant::INT, fog_whiten)
}
