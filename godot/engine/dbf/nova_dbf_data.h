#ifndef NOVA_DBF_DATA_H
#define NOVA_DBF_DATA_H

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <dbf/dbf.h>

namespace godot {

class NovaResourceRoot;

// GDExtension wrapper over opennova::dbf (libs/dbf). Parses a mission's co-named
// .DBF dialog bank (magic 'DLG0') and resolves a dialog id (e.g. "dlg001") to the
// LWF sound-set name(s) it plays (e.g. "Z00gR100"). Used by the runtime to turn a
// mission PlayWavList action's dialog id into a set the .lwf bank can play.
class NovaDbfData : public RefCounted {
	GDCLASS(NovaDbfData, RefCounted)

private:
	opennova::dbf::File file_;
	String source_path_;
	String last_error_;
	bool loaded_ = false;

	bool decode(const PackedByteArray &bytes);

protected:
	static void _bind_methods();

public:
	Error open_file(const String &p_path);
	Error open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool load_bytes(const PackedByteArray &p_bytes);

	bool is_loaded() const { return loaded_; }
	String get_source_path() const { return source_path_; }
	String get_last_error() const { return last_error_; }

	int get_dialog_count() const;
	PackedStringArray get_dialog_ids() const;
	bool has_dialog(const String &p_id) const;
	// The ordered def_id_name(s) (LWF set names) a dialog group plays; empty if
	// the id is unknown. v1 callers play the first.
	PackedStringArray resolve_dialog(const String &p_id) const;

	NovaDbfData() = default;
};

} // namespace godot

#endif // NOVA_DBF_DATA_H
