#pragma once

// What a model's collision does in the game, in words (ADR 0046 S17, Models: the collision shown): a
// volume's type, and what each kind of collision record and bound is to the game, each from the RE
// records (docs/world/world-wac-ai-re.md section 15; docs/render/render-occlusion-re.md; the 3DI record),
// "no witnessed meaning" where the game's use is not known. Pure tables: the outline's titles, the
// Inspector, the picture's legend and its hover read them.

#include <cstddef>
#include <cstdint>
#include <string>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

// How a volume's type acts, what the picture colours it by: solid for everyone, solid for some (a
// vehicle, a player, the masked path), a ladder, a blink box, a zone that sets something on contact, a
// zone that costs health.
enum class ModelVolumeFamily : uint8_t { Solid, SolidFor, Ladder, Blink, Zone, Damage };

struct ModelVolumeType {
	int64_t type = 0;
	const char *code = "";  // the add-on's name code (CB, CL, BB...)
	const char *words = ""; // in a few words: "solid", "ladder", "blink box"
	const char *what = "";  // what the game does with it, a sentence or two
	ModelVolumeFamily family = ModelVolumeFamily::Solid;
};

// A volume's type as the game acts on it [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150, the type
// dispatch; docs/world/world-wac-ai-re.md section 15.4]: a type the game lists nothing for (0, 2, 3, 14,
// any other) is solid.
const ModelVolumeType &model_volume_type(int64_t type);
// "Solid", "Solid for some", "Ladder", "Blink box", "Zone", "Damage zone": a family's legend words.
const char *model_volume_family_words(ModelVolumeFamily family);

// What an occlusion record's type does in the draw [orig: Render_VisibilityPortalTraversal @ 0x5c4ae0;
// Terrain_TestSectorEntityOcclusion @ 0x5c4610]: its words and a sentence.
const char *model_occlusion_type_words(int64_t type);
const char *model_occlusion_type_what(int64_t type);

// Whether a section is a person's hit sphere: a bone section (no faces, no volumes) of a skinned model
// whose whole-body row holds the face mesh, as every person model's does (docs/world/world-wac-ai-re.md
// section 15.8b, person-model subobjects); a skinned model with no face at all (a first-person view's
// arms) is no person's, and the game tests its bone spheres against no round.
bool model_section_is_person(const threedi::Threedi3di3 &model, size_t section);
// Whether a blast breaks the section off: its flags word carries 2 [orig: Entity_ApplyWeaponDamage @
// 0x4e6c5e..0x4e6e6b, the bit @ 0x4e6cd0; runtime/world/collision.h CollisionSection::flags].
bool model_section_breaks(const threedi::ThreediCollisionObject &section);

// The sentences the Inspector and the picture's hover give each kind of collision record and bound.
extern const char *const kModelCollisionWords;    // the collision row
extern const char *const kModelSectionWords;      // a section of a rigid model
extern const char *const kModelHitSphereWords;    // a section of a person (a skinned model)
extern const char *const kModelVolumeWords;       // any volume, before its type's words
extern const char *const kModelBulletFaceWords;   // a bullet face
extern const char *const kModelSectionBreaksWords; // a section a blast breaks off
extern const char *const kModelOcclusionWords;    // any occlusion record, before its type's words
extern const char *const kModelBoundsWords;       // the collision block's box and the projection sphere
extern const char *const kModelBoundRadiusWords;  // the model's bound radius
extern const char *const kModelProbeBoxWords;     // a vehicle's two probe boxes
extern const char *const kModelPartSphereWords;   // a part's bound sphere

} // namespace opennova::editor
