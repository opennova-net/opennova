#include "player/special_key_outcome.h"

using namespace godot;

Ref<SpecialKeyOutcome> SpecialKeyOutcome::make(const opennova::inmatch::SpecialKeyOutcome &p_outcome) {
	Ref<SpecialKeyOutcome> out;
	out.instantiate();
	out->outcome_ = p_outcome;
	return out;
}

void SpecialKeyOutcome::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_taken"), &SpecialKeyOutcome::is_taken);
	ClassDB::bind_method(D_METHOD("is_quit_confirmed"), &SpecialKeyOutcome::is_quit_confirmed);
	ClassDB::bind_method(D_METHOD("is_restart_splash"), &SpecialKeyOutcome::is_restart_splash);
	ClassDB::bind_method(D_METHOD("get_pick"), &SpecialKeyOutcome::get_pick);
	ClassDB::bind_method(D_METHOD("get_page"), &SpecialKeyOutcome::get_page);
	ClassDB::bind_method(D_METHOD("is_page_forward"), &SpecialKeyOutcome::is_page_forward);
	BIND_ENUM_CONSTANT(PAGE_NONE);
	BIND_ENUM_CONSTANT(PAGE_SCOREBOARD);
	BIND_ENUM_CONSTANT(PAGE_HELP);
	BIND_ENUM_CONSTANT(PAGE_BRIEFING);
}
