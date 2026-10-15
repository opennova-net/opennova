#pragma once

// The husk model swap a destroyed entity's picture takes (world-wac-ai-re
// §24.6; the husk graphic is runtime/world/present_passes.h's
// husk_render_graphic): on an entity drawn by a node of its own, the husk
// model grafts under that node (inheriting its transform, so a settling wreck
// keeps moving with it) and the node's other children hide; a batched static
// has no node, so its instance is carved out of its graphic's batches and the
// husk grafts into the container at the placed transform. Either way the husk
// keeps what belongs to the entity, not its graphic: the static-caster role,
// the mirror's reflect flag, the projection owner, the occlusion masks and,
// for a carved static, the entity's light query and identity. The game's
// DestructionPresenter swaps through it; the OpenNova Editor's mission view
// is its intended second caller.

#include <utility>
#include <vector>

#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

namespace godot {

class MissionObjectPlacer;
class Node;
class Node3D;
class ObjectModel;

// What a graft changed, so it can be undone.
struct HuskGraft {
	enum Kind { INDIVIDUAL, STATIC };
	Kind kind = INDIVIDUAL;
	int bms_id = 0;
	// An individual node's other children and their authored visibility.
	std::vector<std::pair<ObjectID, bool>> children;
	// A carved static's placed transform.
	Transform3D placed_transform;
	String husk_graphic;
	bool casts_static_shadow = false;
	// The grafted husk model.
	ObjectID husk;
};

enum class HuskGraftResult {
	Grafted,
	// The husk graphic built no model: nothing changed.
	NoModel,
	// A batched static the placer holds no instance of: nothing changed.
	NotCarved,
};

// Graft `husk_graphic` under the entity's own node. `bms_id` (0 none) names
// the static terrain shadow the husk takes over.
HuskGraftResult husk_graft_individual(MissionObjectPlacer &p_placer, Node3D &p_node,
		const String &p_husk_graphic, int p_bms_id, HuskGraft &r_graft);
// Carve batched static `bms_id` out of its batches and graft `husk_graphic`
// under `container` at its placed transform.
HuskGraftResult husk_graft_static(MissionObjectPlacer &p_placer, Node3D &p_container, int p_bms_id,
		const String &p_husk_graphic, HuskGraft &r_graft);
// Undo a graft's changes to the intact entity: the static terrain shadow
// replacement cleared, a carved static shown again, the hidden children back
// at their authored visibility. The husk model itself is its owner's to free.
void husk_graft_restore(MissionObjectPlacer *p_placer, const HuskGraft &p_graft);

// Whether any visual under `root` casts the static terrain shadow.
bool node_has_static_shadow_caster(Node *p_root);

} // namespace godot
