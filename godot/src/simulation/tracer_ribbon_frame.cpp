#include "simulation/tracer_ribbon_frame.h"

using namespace godot;

void TracerRibbonStrip::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_positions"), &TracerRibbonStrip::get_positions);
	ClassDB::bind_method(D_METHOD("set_positions", "positions"),
			&TracerRibbonStrip::set_positions);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "positions"), "set_positions",
			"get_positions");
	ClassDB::bind_method(D_METHOD("get_colors"), &TracerRibbonStrip::get_colors);
	ClassDB::bind_method(D_METHOD("set_colors", "colors"), &TracerRibbonStrip::set_colors);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_COLOR_ARRAY, "colors"), "set_colors",
			"get_colors");
}

void TracerRibbonFrame::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_additive"), &TracerRibbonFrame::get_additive);
	ClassDB::bind_method(D_METHOD("set_additive", "strip"), &TracerRibbonFrame::set_additive);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "additive", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT, "TracerRibbonStrip"),
			"set_additive", "get_additive");
	ClassDB::bind_method(D_METHOD("get_alpha"), &TracerRibbonFrame::get_alpha);
	ClassDB::bind_method(D_METHOD("set_alpha", "strip"), &TracerRibbonFrame::set_alpha);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "alpha", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT, "TracerRibbonStrip"),
			"set_alpha", "get_alpha");
	ClassDB::bind_method(D_METHOD("get_channels"), &TracerRibbonFrame::get_channels);
	ClassDB::bind_method(D_METHOD("set_channels", "channels"),
			&TracerRibbonFrame::set_channels);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "channels"), "set_channels", "get_channels");
}
