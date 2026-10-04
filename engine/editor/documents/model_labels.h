#pragma once

// The model's display names (ADR 0046 S17, Models: the S15 treatment for a `.3di`): what each record of
// a model, and each value that names a part, a surface or a level, reads as to a modder. Pure functions
// of the document (the model row as it stands, the immutable parsed base).
//
// The words follow the Blender add-on's names, so a modder reads one name on both sides: a part is
// `PN01` (a rig's `BN01`), two digits and 1-based in a name, 0-based inside
// (docs/threedi/scene-naming-contract.md, Parts); a LOD keeps its 0-based index (`_lod_index`, 0 the
// primary), as the engine and the RE records number them. What a value means is the game's, cited where
// the words are made; what the file does not say (a part's name: the PANM table carries none) is the
// add-on's convention, said as such.

#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/name_source.h>
#include <editor/model/document.h>

namespace opennova::editor {

class ModelDocument;
struct ModelRow;

// A part by its index in LOD 0: `PN01` for part 0 of a rigid model, `BN01` of a skinned one (the
// add-on's names); "None" for the value a field calls none (-1, 255); "No part 6 (LOD 0 has 5 parts)"
// past LOD 0's parts.
std::string model_part_name(const ModelRow &row, int64_t part);
// Whether a part index names a part of LOD 0.
bool model_part_exists(const ModelRow &row, int64_t part);

// What a LOD draws at, by the game's level walk [orig: Model_SelectRlodLevel @ 0x5c3b20: level i draws
// while the scaled projected radius is above its threshold and at or below the level before's; the walk
// stops at the first level whose threshold is 0, and past the last level draws the last (@ 0x5c3b58)]:
// "above 160 px", "19 to 64 px", "below 19 px" (the last LOD, whatever its own threshold), "at any size"
// (a single LOD), "never (LOD 3 draws down to 0 px)".
std::string model_lod_range(const ModelRow &row, size_t lod);
std::string model_lod_range(const std::vector<int32_t> &thresholds, size_t lod);
// Whether the game's walk can reach the LOD by distance.
bool model_lod_drawn(const ModelRow &row, size_t lod);
bool model_lod_drawn(const std::vector<int32_t> &thresholds, size_t lod);
// The LODs' thresholds in order (RMDL's pixel counts).
std::vector<int32_t> model_lod_thresholds(const ModelRow &row);

// What a user point's name makes of it in the game, "" for a name the game looks up nowhere itself
// (an item may still name it: its particle effects, an attached weapon): a sitex a passenger seat, a
// ctrlx the control seat, a drvrx the driver's seat, UseGun the gunner's seat [orig:
// Entity_GetBoneSlotType @ 0x434ED0; EntityDef_LoadModelsAndCallbacks @ 0x439F50, @ 0x43A4F0, @ 0x43A532,
// @ 0x43A570, @ 0x43A5A9], a flare a countermeasure flare's launch point and prim / sec / bullet01 /
// bullet02 a vehicle weapon's muzzle [orig: Entity_InitVehicleAI @ 0x460200], an agun a gunner's
// attachment [orig: Entity_SetupGunnerAttachments @ 0x4681AA], TARGET the origin a vehicle's weapons aim
// and fire from [orig: Entity_InitFromModel @ 0x40dd04; Entity_ComputeWeaponFireOrigin @ 0x43b5d4] and
// LOOK the origin of its fire's line-of-sight check [orig: @ 0x43B749], CAMERA a mounted gun's camera
// [orig: Entity_InitBoneReferences @ 0x4414A9], ground the point a placement stands on the terrain (the
// placement rule, threedi_3di3_ground_anchor).
std::string model_user_point_role(const std::string &name);

// What a record of the model reads as (DocumentType::record_label; the outline's rows, the Inspector's
// heading, Problems' places, the wire's titles): the model by its name; a LOD by its index, what it
// draws at and its parts ("LOD 0: above 160 px, 5 parts"); a part animation by its part and what it does
// ("PN04: spins, HELO_ROTOR"); a material by its first texture and its shader ("tblkhwk1.tga,
// VS_DOT3DIFF"); a texture row by its file and slot; a light by its kind and part; a user point by its
// name and role; a register by its name; a rotation frame by its row and the parts that turn through it;
// a section by its part; a volume by its type; a bullet face by its surface; an occlusion record by its
// type. "" for a record the document does not have.
std::string model_record_label(const Document &document, const NodeAddress &address, const NameSource *names);
// A field's value worded where it names something (DocumentType::value_label): a part index by its name,
// a bullet face's surface by its words.
bool model_value_label(const Document &document, const NodeAddress &address, const FieldUse &field, const Value &value,
                       const NameSource *names, DisplayName &out);

} // namespace opennova::editor
