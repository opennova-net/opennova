#include "hud/feed_row.h"

#include "util/string_convert.h"

namespace godot {

String FeedRow::get_key() const { return opennova::to_gd(value_.key); }
String FeedRow::get_attacker() const { return opennova::to_gd(value_.attacker); }
String FeedRow::get_victim() const { return opennova::to_gd(value_.victim); }
String FeedRow::get_extra() const { return opennova::to_gd(value_.extra); }
String FeedRow::get_wpname_key() const { return opennova::to_gd(value_.wpname_key); }

void FeedRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_event_type"), &FeedRow::get_event_type);
	ClassDB::bind_method(D_METHOD("get_kind"), &FeedRow::get_kind);
	ClassDB::bind_method(D_METHOD("is_camp"), &FeedRow::is_camp);
	ClassDB::bind_method(D_METHOD("is_own"), &FeedRow::is_own);
	ClassDB::bind_method(D_METHOD("get_key"), &FeedRow::get_key);
	ClassDB::bind_method(D_METHOD("get_attacker"), &FeedRow::get_attacker);
	ClassDB::bind_method(D_METHOD("get_victim"), &FeedRow::get_victim);
	ClassDB::bind_method(D_METHOD("get_extra"), &FeedRow::get_extra);
	ClassDB::bind_method(D_METHOD("get_wpname_key"), &FeedRow::get_wpname_key);
	ClassDB::bind_method(D_METHOD("get_color"), &FeedRow::get_color);
}

} // namespace godot
