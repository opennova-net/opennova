#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <formats/adm/adm.h>

#include <vector>

namespace godot {

// One .adm animation-definition table: the ordered rows binding an anim slot
// key to its ring of clip names. Parsed from disk or bytes, written back in
// the canonical form (retail's hand-edited tables are parse-equal, not
// byte-equal). Rows are added in order; the reset row goes first by
// convention (the rig's skeleton source).
class AnimDefDocument : public RefCounted {
	GDCLASS(AnimDefDocument, RefCounted)

	std::vector<opennova::adm::AdmEntry> entries_;
	String source_path_;
	String last_error_;

protected:
	static void _bind_methods();

public:
	Error load_from_path(const String &p_path);
	Error load_from_bytes(const PackedByteArray &p_bytes);
	Error save_to_path(const String &p_path);
	PackedByteArray to_bytes();

	void clear();
	bool add_row(const String &p_key, const PackedStringArray &p_variants);
	int get_row_count() const { return static_cast<int>(entries_.size()); }
	String get_row_key(int p_row) const;
	PackedStringArray get_row_variants(int p_row) const;
	int find_row(const String &p_key) const;
	String get_source_path() const { return source_path_; }
	String get_last_error() const { return last_error_; }
};

} // namespace godot
