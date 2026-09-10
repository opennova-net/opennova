#pragma once

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <vector>

#include <formats/threedi/threedi_build.h>

#include "model/model_authoring_manifest.h"
#include "model/model_document.h"

namespace godot {

// Reads a node tree shaped by the scene naming contract (the projector's
// output, or a stock-importer scene the author named) together with its
// manifest, and assembles a .3di document through the engine's construction
// API. Nodes carry geometry and placement; the manifest carries every word
// the nodes cannot spell. Anything the tree does not spell the way the
// contract requires is reported by name and the export returns null.
class ModelSceneExporter : public RefCounted {
	GDCLASS(ModelSceneExporter, RefCounted)

	String last_error_;
	PackedStringArray errors_;

	void fail(const String &p_what);

	struct PartRef {
		Node3D *node = nullptr;
		Transform3D global; // relative to the LOD node
		Vector3 rel;        // the pivot relative to the parent part (the root: its global origin)
		int parent = -1;    // part index; the root names itself
	};

	bool export_lod(Node3D *p_lod_node, int p_lod_index, const Ref<ModelAuthoringManifest> &p_manifest,
			opennova::threedi::ThreediBuildModel &r_model, int &r_part_count);
	bool collect_static_parts(Node3D *p_lod_node, std::vector<PartRef> &r_parts);
	bool collect_skinned_parts(Node3D *p_lod_node, Skeleton3D *&r_skeleton, std::vector<PartRef> &r_parts);
	bool export_mesh(MeshInstance3D *p_mesh, int p_part, const Transform3D &p_mesh_global,
			Skeleton3D *p_skeleton, const std::vector<PartRef> &p_parts, int p_lod,
			const Ref<ModelAuthoringManifest> &p_manifest, opennova::threedi::ThreediBuildModel &r_model);
	bool export_collision(Node3D *p_collision, int p_part_count, const std::vector<int> &p_part_parents,
			opennova::threedi::ThreediBuildModel &r_model);
	bool export_user_points(Node3D *p_points, opennova::threedi::ThreediBuildModel &r_model);
	bool export_lights(Node3D *p_lights, opennova::threedi::ThreediBuildModel &r_model);

protected:
	static void _bind_methods();

public:
	// The document the tree describes, or null (see get_errors).
	Ref<ModelDocument> export_scene(Node3D *p_root, const Ref<ModelAuthoringManifest> &p_manifest);
	String get_last_error() const { return last_error_; }
	PackedStringArray get_errors() const { return errors_; }
};

} // namespace godot
