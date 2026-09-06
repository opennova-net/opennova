#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>

namespace godot {

// One .3di document: parsed from disk or assembled by the exporter, written
// back through the parity writer. The format reads and writes itself
// (load_from_path / save_to_path); Godot's resource system never sees it.
// Read-only from GDScript beyond load/save: the editable form of a model is
// the scene the projector builds and the exporter reads.
class ModelDocument : public RefCounted {
	GDCLASS(ModelDocument, RefCounted)

	opennova::threedi::Threedi3di3 parsed_ = {};
	bool parsed_valid_ = false;
	std::unique_ptr<opennova::threedi::ThreediAssembled> assembled_;
	String source_path_;
	String last_error_;

	void _clear();

protected:
	static void _bind_methods();

public:
	~ModelDocument();

	Error load_from_path(const String &p_path);
	Error load_from_bytes(const PackedByteArray &p_bytes);
	Error save_to_path(const String &p_path);
	PackedByteArray to_bytes();

	bool is_loaded() const { return parsed_valid_ || assembled_ != nullptr; }
	String get_model_name() const;
	String get_source_path() const { return source_path_; }
	String get_last_error() const { return last_error_; }
	int get_lod_count() const;
	int get_part_count(int p_lod_index) const;
	int get_material_count() const;
	bool is_skinned() const;
	bool has_occlusion() const;

	// C++ consumers (the projector reads, the exporter hands over what it built).
	const opennova::threedi::Threedi3di3 *model() const;
	void adopt_assembled(std::unique_ptr<opennova::threedi::ThreediAssembled> p_assembled);
};

} // namespace godot
