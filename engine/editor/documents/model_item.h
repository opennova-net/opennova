#pragma once

#include <cstdint>
#include <string>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

// What item a model is drawn as, read from the model alone (ADR 0046 DI-12, "Place a model no item draws
// yet"; docs/world/itemdef-re.md, "The item a model makes"): the row a new items.def item takes for a model no
// item of the project draws. The original's items.def keeps the kind of thing an item is in its TYPE, and the
// game acts on parts of the model only for some TYPEs, so the model's parts say the TYPE:
//
// - Its seats or its driver's place (a `sitex`, `ctrlx` or `drvrx` user point): a vehicle. The game binds those
//   points as the vehicle's seats and its control seat [orig: EntityDef_LoadModelsAndCallbacks @ 0x439F50, the
//   seat walk @ 0x43A47B..0x43A5CD], which a vehicle's class drives (its physics and its control class: cveh,
//   chel, cbot), none of which the model says. Retail: the 36 models with a ctrlx or drvrx point are all
//   vehicles, as are 31 of the 32 with a sitex.
// - A `UseGun` point: a mounted weapon, the gun its gunner turns [orig: @ 0x43A582, useGunBone]: an ewep object
//   with its weapon, which the model does not say. Retail: all 24 such models.
// - A skinned model: a person [orig: EntityDef_LoadModelsAndCallbacks @ 0x439F50, its type 3 branch], who needs his
//   animation table (anim_def) and his classes. Retail: 30 of the 32 skinned models are persons (the other two,
//   the tanks, have a ctrlx).
// - Its occlusion (OOBJ) or a blink box (a BB volume, type 8): a building (TYPE 5). The game builds its portals
//   from the buildings' occlusion at the mission's start [orig: Terrain_InitBuildingPortals @ 0x5c7480], draws a
//   building through the building batch alone [orig: Entity_BuildProximityLists_Pool2 `def->type ==
//   ItemType_Building` @ 0x4b946e], lets only buildings block a line of sight [orig:
//   Physics_RaycastFindCollisionEntity @ 0x539a70] and tests the blink boxes of buildings alone [orig:
//   Entity_TestCollisionSections @ 0x4aef90]: a model with those parts does what they do only as a building.
//   Retail: 134 of the 141 such models that are no land-for-points station are buildings.
// - Any other: a decoration (TYPE 2, which `foliage` names too: the game reads both as 2 [orig:
//   ItemDef_ParseProperty @ 0x49eb00]), drawn where its record stands it. Retail: 396 of the 459 models with
//   none of those parts are decorations or foliage (26 are buildings; 35 objects, each for a class of its own:
//   the S&D targets, the flags, the thrown weapons).
//
// Either one is made with no class (no ai_function or move_function): the game keeps it where its record stands
// it (the null row of the class tables [orig: EntityDef_LookupPhysicsCallback @ 0x4a9240]), as retail's 359 of
// 480 decorations and 146 of 163 buildings are, and with no hp, which the game makes immune to damage [orig:
// Entity_InitFromModel @ 0x40DC95 / @ 0x40DC9F, an hp-0 definition's armor words set to -1].
enum class ModelItemKind : uint8_t { Decoration, Building, Person, Vehicle, MountedWeapon };

// "decoration", "building", "person", "vehicle", "mounted_weapon".
const char *model_item_kind_token(ModelItemKind kind);

struct ModelItemFacts {
	ModelItemKind kind = ModelItemKind::Decoration;
	// The items.def TYPE the item takes (def::DefItemType): decoration (2) or building (5) for the kinds the
	// editor makes; the TYPE the kind would be for the others.
	int type = 2;
	// Whether the editor makes the item from the model alone: a decoration or a building. A person, a vehicle or
	// a mounted weapon needs what the model does not say (its animation table, its classes, its physics, its
	// weapon).
	bool makes = true;
	// What of the model decided it, in words ("its occlusion, 8 objects, and 2 blink boxes").
	std::string because;
	// How many doors its parts' registers turn (DOOR_00 and on, as the door system drives them [orig:
	// BoneCallback_BuildBoneTransforms @ 0x4E3070]): they open only for an item of the door class with its doors
	// counted, which the item made has not (0: none).
	int doors = 0;
};

ModelItemFacts model_item_facts(const threedi::Threedi3di3 &model);

// The kind and why in words, for a status line: "a building (its occlusion, 8 objects)".
std::string model_item_words(const ModelItemFacts &facts);

} // namespace opennova::editor
