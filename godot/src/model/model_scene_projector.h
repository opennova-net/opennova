#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "model/model_authoring_manifest.h"
#include "model/model_document.h"

namespace godot {

// Projects a .3di document into an editable node tree shaped by the scene
// naming contract (docs/threedi/scene-naming-contract.md) and fills the
// manifest with everything the nodes cannot spell (materials, LOD words,
// registers). The tree is a transient projection the exporter reads back;
// nothing here is a Godot import of the format. A construct outside the
// scene form (occlusion records, indexed strips, a second matrix, ...) is
// REFUSED by name rather than dropped (ADR 0003): `project` returns null and
// `get_refusals` lists them.
class ModelSceneProjector : public RefCounted {
	GDCLASS(ModelSceneProjector, RefCounted)

	String last_error_;
	PackedStringArray refusals_;

	void refuse(const String &p_what);
	bool check_scope(const opennova::threedi::Threedi3di3 *p_model);

protected:
	static void _bind_methods();

public:
	// The returned root is unowned: the caller adds it to a tree or frees it.
	// `textures_dir` is where the preview materials resolve their textures
	// (empty: the document's own directory).
	Node3D *project(const Ref<ModelDocument> &p_document, const Ref<ModelAuthoringManifest> &p_manifest,
			const String &p_textures_dir);
	String get_last_error() const { return last_error_; }
	PackedStringArray get_refusals() const { return refusals_; }
};

} // namespace godot
