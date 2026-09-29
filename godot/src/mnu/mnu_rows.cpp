#include "mnu/mnu_rows.h"

#include "util/string_convert.h"

namespace godot {

String MnuSoundRow::get_state() const { return opennova::to_gd(value_.state); }
String MnuSoundRow::get_trigger() const { return opennova::to_gd(value_.trigger); }
String MnuSoundRow::get_file() const { return opennova::to_gd(value_.file); }

void MnuSoundRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_state"), &MnuSoundRow::get_state);
	ClassDB::bind_method(D_METHOD("get_trigger"), &MnuSoundRow::get_trigger);
	ClassDB::bind_method(D_METHOD("get_file"), &MnuSoundRow::get_file);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "state", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_state");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "trigger", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_trigger");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_file");
}

Ref<MnuActionRow> MnuActionRow::make(const String &p_type, const String &p_target,
		const String &p_state, bool p_toggle, const String &p_file) {
	Ref<MnuActionRow> row;
	row.instantiate();
	row->value_.type = opennova::to_std(p_type);
	row->value_.target = opennova::to_std(p_target);
	row->value_.state = opennova::to_std(p_state);
	row->value_.toggle = p_toggle;
	row->value_.file = opennova::to_std(p_file);
	return row;
}

String MnuActionRow::get_type() const { return opennova::to_gd(value_.type); }
String MnuActionRow::get_target() const { return opennova::to_gd(value_.target); }
String MnuActionRow::get_state() const { return opennova::to_gd(value_.state); }
String MnuActionRow::get_file() const { return opennova::to_gd(value_.file); }
String MnuActionRow::get_field() const { return opennova::to_gd(value_.field); }
String MnuActionRow::get_field_attr() const { return opennova::to_gd(value_.field_attr); }
String MnuActionRow::get_test() const { return opennova::to_gd(value_.test); }

void MnuActionRow::_bind_methods() {
	ClassDB::bind_static_method("MnuActionRow",
			D_METHOD("make", "type", "target", "state", "toggle", "file"), &MnuActionRow::make);
	ClassDB::bind_method(D_METHOD("get_type"), &MnuActionRow::get_type);
	ClassDB::bind_method(D_METHOD("get_target"), &MnuActionRow::get_target);
	ClassDB::bind_method(D_METHOD("get_state"), &MnuActionRow::get_state);
	ClassDB::bind_method(D_METHOD("get_file"), &MnuActionRow::get_file);
	ClassDB::bind_method(D_METHOD("get_field"), &MnuActionRow::get_field);
	ClassDB::bind_method(D_METHOD("get_field_attr"), &MnuActionRow::get_field_attr);
	ClassDB::bind_method(D_METHOD("get_test"), &MnuActionRow::get_test);
	ClassDB::bind_method(D_METHOD("get_target_form"), &MnuActionRow::get_target_form);
	ClassDB::bind_method(D_METHOD("is_toggle"), &MnuActionRow::is_toggle);
	ClassDB::bind_method(D_METHOD("is_external_browser"), &MnuActionRow::is_external_browser);
	const auto prop = [](const char *p_name, Variant::Type p_type, const char *p_getter) {
		ADD_PROPERTY(PropertyInfo(p_type, p_name, PROPERTY_HINT_NONE, "",
							 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
				"", p_getter);
	};
	prop("type", Variant::STRING, "get_type");
	prop("target", Variant::STRING, "get_target");
	prop("state", Variant::STRING, "get_state");
	prop("file", Variant::STRING, "get_file");
	prop("field", Variant::STRING, "get_field");
	prop("field_attr", Variant::STRING, "get_field_attr");
	prop("test", Variant::STRING, "get_test");
	prop("target_form", Variant::INT, "get_target_form");
	prop("toggle", Variant::BOOL, "is_toggle");
	prop("external_browser", Variant::BOOL, "is_external_browser");
}

} // namespace godot
