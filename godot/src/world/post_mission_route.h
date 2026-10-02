#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "network/connection_error.h"

namespace godot {

// The post-mission router's verdict for one exit (engine: inmatch::route_mission_exit), as
// the shell reads it back from the world (GameWorld::post_mission_route): whether the
// NovaWorld session survives for the NovaWorld menu, and the error text the menu shows first
// -- a gameerr.bin generic error by key, or the in-match connection's disconnect reason: its
// error record, from which the shell builds retail's reason text, plus a diagnostic
// (both empty while that connection was healthy).
class PostMissionRoute : public RefCounted {
	GDCLASS(PostMissionRoute, RefCounted)

public:
	bool get_keep_session() const { return keep_session_; }
	void set_keep_session(bool p_value) { keep_session_ = p_value; }
	bool get_error() const { return error_; }
	void set_error(bool p_value) { error_ = p_value; }
	String get_error_key() const { return error_key_; }
	void set_error_key(const String &p_value) { error_key_ = p_value; }
	String get_error_text() const { return error_text_; }
	void set_error_text(const String &p_value) { error_text_ = p_value; }
	Ref<ConnectionError> get_connection_error() const { return connection_error_; }
	void set_connection_error(const Ref<ConnectionError> &p_value) { connection_error_ = p_value; }

protected:
	static void _bind_methods();

private:
	bool keep_session_ = false;
	bool error_ = false;
	String error_key_;
	String error_text_;
	Ref<ConnectionError> connection_error_;
};

} // namespace godot
