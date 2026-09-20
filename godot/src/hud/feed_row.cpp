#include "hud/feed_row.h"

#include "util/string_convert.h"

namespace godot {

String FeedRow::get_key() const { return opennova::to_gd(value_.key); }
String FeedRow::get_extra() const { return opennova::to_gd(value_.extra); }
String FeedRow::get_wpname_key() const { return opennova::to_gd(value_.wpname_key); }

String FeedRow::format_line(const String &tmpl, const String &unknown,
        const String &bonus, const String &wpname) const {
	return opennova::to_gd(opennova::hud::feed_format_row(value_, tmpl.utf8().get_data(),
			unknown.utf8().get_data(), bonus.utf8().get_data(), wpname.utf8().get_data()));
}

void FeedRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_announcement"), &FeedRow::is_announcement);
	ClassDB::bind_method(D_METHOD("format_line", "template", "unknown", "bonus", "wpname"),
			&FeedRow::format_line);
	ClassDB::bind_method(D_METHOD("get_kind"), &FeedRow::get_kind);
	ClassDB::bind_method(D_METHOD("is_camp"), &FeedRow::is_camp);
	ClassDB::bind_method(D_METHOD("get_key"), &FeedRow::get_key);
	ClassDB::bind_method(D_METHOD("get_extra"), &FeedRow::get_extra);
	ClassDB::bind_method(D_METHOD("get_wpname_key"), &FeedRow::get_wpname_key);
	ClassDB::bind_method(D_METHOD("get_color"), &FeedRow::get_color);
}

} // namespace godot
