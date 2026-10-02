#pragma once

#include <runtime/inmatch/disconnect_reason.h>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class RtxtStringFile;

// A joiner connection's error record (inmatch::ConnectionErrorRecord) as a
// typed value: the join failure fields and the latched disconnect record that
// retail's reason text is built from. The shell resolves the text through its
// loaded gameerr table (Strings), the expansion's override table first.
class ConnectionError : public RefCounted {
	GDCLASS(ConnectionError, RefCounted)

	opennova::inmatch::ConnectionErrorRecord record_;

protected:
	static void _bind_methods();

public:
	static Ref<ConnectionError> make(const opennova::inmatch::ConnectionErrorRecord &p_record);
	// A literal record (tests and fixtures author one through this).
	static Ref<ConnectionError> from_fields(int p_connect_error, int p_connect_param,
			const String &p_connect_text, int p_disconnect_code, int p_disconnect_param,
			const String &p_disconnect_text);
	const opennova::inmatch::ConnectionErrorRecord &record() const { return record_; }

	// True when the record holds a join failure or a disconnect.
	bool is_set() const;
	int get_connect_error() const { return static_cast<int>(record_.connect_error); }
	int get_connect_param() const { return static_cast<int>(record_.connect_param); }
	int get_disconnect_code() const { return static_cast<int>(record_.disconnect_code); }
	int get_disconnect_param() const { return static_cast<int>(record_.disconnect_param); }
	// The gameerr entry the reason reads ("NCC007"), "" when the record is empty.
	String reason_key() const;
	// Retail's reason text (inmatch::disconnect_reason_string).
	String reason_text(const Ref<RtxtStringFile> &p_override_table,
			const Ref<RtxtStringFile> &p_gameerr) const;
	// OpenNova browser policy, not a retail witness: the server refused this
	// build or install outright (an incompatible game version, an expansion
	// mismatch, or a banned address), so retrying the same server cannot work.
	bool refuses_this_install() const;
};

} // namespace godot
