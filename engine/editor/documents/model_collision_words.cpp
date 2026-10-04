// What a model's collision does in the game, in words (model_collision_words.h).

#include <editor/documents/model_collision_words.h>

#include <algorithm>

namespace opennova::editor {

namespace {

// The collidable types the game acts on [orig: Entity_ComputeBoneCollisionForce @ 0x4ae150; each row's
// witness in docs/world/world-wac-ai-re.md section 15.4], with the Super OED manual's names (section
// 1.1.3.4) and the add-on's codes (VOLUME_CODES).
const ModelVolumeType kTypes[] = {
	{1, "CB", "solid",
	 "Solid for everyone: what touches it is pushed out. The one type lines of sight, the ground probe and "
	 "generic rays stop at [orig: @ 0x413298, @ 0x4aebdd].",
	 ModelVolumeFamily::Solid},
	{4, "CL", "ladder",
	 "A ladder: touching it starts the climb, its first plane the face climbed [orig: @ 0x4ae894..0x4aea30].",
	 ModelVolumeFamily::Ladder},
	{5, "CV", "touch, no push", "Marks a contact and pushes nothing [orig: @ 0x4ae874].", ModelVolumeFamily::Zone},
	{6, "CA", "armory zone",
	 "Inside it the armory opens on its key (weapon.mnu) [orig: @ 0x4aea45; Input_HandleActionBinding @ 0x49b848].",
	 ModelVolumeFamily::Zone},
	{7, "VC", "vehicle wall", "Solid for vehicles alone (the vehicle contact mask) [orig: @ 0x4ae558].",
	 ModelVolumeFamily::SolidFor},
	{8, "BB", "blink box",
	 "A building's inside: standing in it accumulates its flags into the frame's blink state, whose bits the "
	 "draw reads as indoors (0x2), the sky off (0x4) and the water off (0x8) [orig: @ 0x4aea68; "
	 "Entity_TestCollisionSections @ 0x4aef90]. The manual's letters (V, S, W keeping the voxels, sky and "
	 "water; L and O) each clear a bit; how they meet those bits is unknown (render-occlusion-re D-OCC-8).",
	 ModelVolumeFamily::Blink},
	{9, "CD", "door", "Touching it opens the door section it belongs to [orig: @ 0x4aeb0f..0x4aeb22].",
	 ModelVolumeFamily::Zone},
	{10, "CT", "change-team box", "Touching it asks to capture or change team [orig: @ 0x4aeb7b, @ 0x4b31e3].",
	 ModelVolumeFamily::Zone},
	{11, "CM", "vehicle loadout zone",
	 "Inside it the vehicle menu opens on its key (vehicle.mnu) [orig: @ 0x4aeb92, @ 0x49b858].",
	 ModelVolumeFamily::Zone},
	{12, "VK", "masked wall", "Solid on the masked contact path (mask 0x10) alone [orig: @ 0x4ae568].",
	 ModelVolumeFamily::SolidFor},
	{13, "CF", "special function",
	 "Standing on it sets off a special function (a FARP), only while grounded on it [orig: @ 0x4aebb3].",
	 ModelVolumeFamily::Zone},
	{16, "DH", "damage, high", "Each touch costs 50 health [orig: @ 0x4aeb39].", ModelVolumeFamily::Damage},
	{17, "DM", "damage, medium", "Each touch costs 6 health [orig: @ 0x4aeb50].", ModelVolumeFamily::Damage},
	{18, "DL", "damage, low", "Each touch costs 1 health [orig: @ 0x4aeb67].", ModelVolumeFamily::Damage},
	{19, "CP", "player wall", "Solid for players alone: AI walks through [orig: @ 0x4ae543].",
	 ModelVolumeFamily::SolidFor},
};

// A type the game lists nothing for acts as solid; the add-on names the ones retail ships.
const ModelVolumeType kUnlisted[] = {
	{0, "CX", "solid", "Solid: a type the game lists nothing for pushes out as CB does [orig: @ 0x4aebdd].",
	 ModelVolumeFamily::Solid},
	{2, "CS", "solid", "Solid: the game lists nothing for this type, so it pushes out as CB does [orig: @ 0x4aebdd].",
	 ModelVolumeFamily::Solid},
	{3, "CC", "solid", "Solid: the game lists nothing for this type, so it pushes out as CB does [orig: @ 0x4aebdd].",
	 ModelVolumeFamily::Solid},
	{14, "LP", "solid", "Solid: the game lists nothing for this type, so it pushes out as CB does [orig: @ 0x4aebdd].",
	 ModelVolumeFamily::Solid},
};

const ModelVolumeType kOther = {-1, "", "solid",
                                "Solid: the game lists nothing for this type, so it pushes out as CB does "
                                "[orig: @ 0x4aebdd].",
                                ModelVolumeFamily::Solid};

} // namespace

const ModelVolumeType &model_volume_type(int64_t type) {
	for (const ModelVolumeType &row : kTypes)
		if (row.type == type) return row;
	for (const ModelVolumeType &row : kUnlisted)
		if (row.type == type) return row;
	return kOther;
}

const char *model_volume_family_words(ModelVolumeFamily family) {
	switch (family) {
	case ModelVolumeFamily::Solid: return "Solid";
	case ModelVolumeFamily::SolidFor: return "Solid for some";
	case ModelVolumeFamily::Ladder: return "Ladder";
	case ModelVolumeFamily::Blink: return "Blink box";
	case ModelVolumeFamily::Zone: return "Zone";
	case ModelVolumeFamily::Damage: return "Damage zone";
	}
	return "Solid";
}

const char *model_occlusion_type_words(int64_t type) {
	switch (type) {
	case 0: return "occluder";
	case 1: return "open";
	case 2: return "window";
	case 3: return "portal";
	case 4: return "OH";
	default: return "unknown";
	}
}

const char *model_occlusion_type_what(int64_t type) {
	switch (type) {
	case 0: return "Hides what lies behind it from the draw [orig: Terrain_TestSectorEntityOcclusion @ 0x5c4610].";
	case 1:
		return "An open slot: it occludes only while its building is marked (bit 31), and marks the building "
		       "open (drawn outside in) [orig: Terrain_TestSectorEntityOcclusion @ 0x5c4610]. What it stands for "
		       "(a door, a window, a destroyed wall) is unknown (render-occlusion-re D-OCC-3).";
	case 2:
		return "A window to the outside: the draw looks through it into the room it belongs to [orig: "
		       "Render_VisibilityPortalTraversal @ 0x5c4ae0].";
	case 3:
		return "A doorway between two rooms: the draw passes through it into the room it leads to [orig: "
		       "Render_VisibilityPortalTraversal @ 0x5c4ae0].";
	case 4: return "No witnessed meaning.";
	default: return "A type the game reads nothing for.";
	}
}

bool model_section_breaks(const threedi::ThreediCollisionObject &section) { return (section.unk0 & 2) != 0; }

bool model_section_is_person(const threedi::Threedi3di3 &model, size_t section) {
	const threedi::ThreediCollisionModel *c = model.collision;
	if (model.header.mesh_type != threedi::THREEDI_MESH_SKINNED || !c || section >= c->object_count) return false;
	const threedi::ThreediCollisionObject &object = c->objects[section];
	if (object.num_faces != 0 || object.num_bounding_volumes != 0) return false;
	// A person's whole-body row holds the face mesh (docs/world/world-wac-ai-re.md section 15.8b,
	// person-model subobjects): a skinned model with no face anywhere is no person's.
	for (size_t o = 0; o < c->object_count; ++o)
		if (c->objects[o].num_faces > 0) return true;
	return false;
}

int32_t model_person_hit_radius_q16(int section, int32_t authored_q16) {
	const int64_t scale = section == 14 ? 65 : 45;
	int64_t radius = 0xCCC + int64_t(authored_q16) * scale / 100;
	if (section == 15 || section == 16) radius = std::min<int64_t>(radius, 0x3000);
	return int32_t(radius);
}

const char *const kModelCollisionWords =
		"What the game tests the model against: its volumes for contact and lines of sight, its bullet "
		"faces for rounds, its sections' spheres and boxes before either.";
const char *const kModelSectionWords =
		"The collision of one part: it moves with the part of its number on LOD 0 [orig: "
		"Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]. Its sphere is the quick test before a line of sight "
		"meets its volumes [orig: Entity_RaycastCollisionModel @ 0x413060]; its box the quick test of contact and "
		"of blink boxes, and where a blast breaks off a section that can break [orig: Entity_ApplyWeaponDamage @ "
		"0x4e6c5e].";
const char *const kModelHitSphereWords =
		"A person's hit sphere on a bone: a round hits the bone when it passes within 45 percent of the radius "
		"plus 1/20 m (the head, BN15: 65 percent; BN16 and BN17 no more than 3/16 m), and the bone sets where "
		"the hit lands [orig: Physics_RaycastAgainstBoneSections @ 0x4e4670]. Only a person is tested so: a "
		"skinned model no person wears (a first-person view's arms) has its bone spheres read by no round.";
const char *const kModelVolumeWords =
		"A convex solid bounded by its planes, its box the quick test. Rounds pass volumes: they hit the "
		"bullet faces.";
const char *const kModelBulletFaceWords =
		"A triangle a round's ray tests from either side (Front only: from its front) [orig: "
		"Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]: a hit plays its surface's ammo effect and stops the "
		"round, but Bullets pass lets every round by with no effect, and the soft surfaces (Water, Glass, Cloth, "
		"Foliage, Flesh) let it go on through at a cost of its energy [orig: Projectile_ProcessDamageOnTarget @ "
		"0x4e823f..0x4e8266].";
const char *const kModelSectionBreaksWords =
		"A blast breaks this section off: its flags carry 2, so a blast whose box holds its box's middle "
		"takes it [orig: Entity_ApplyWeaponDamage @ 0x4e6c5e..0x4e6e6b, the bit @ 0x4e6cd0].";
const char *const kModelOcclusionWords = "A polygon the draw of a building's rooms reads (not a collision).";
const char *const kModelBoundsWords =
		"The collision block's box: its middle and half diagonal make the sphere the game culls the model by "
		"and picks its LOD with [orig: Entity_ComputeBoundingSphere @ 0x5c69a0; Model_SelectRlodLevel @ 0x5c3b20].";
const char *const kModelBoundRadiusWords =
		"The model's radius plus 1/16 m about its origin: the sphere the game's proximity and broad-phase "
		"tests use [orig: Entity_InitFromModel @ 0x40dc30].";
const char *const kModelProbeBoxWords =
		"A vehicle's contact solve probes the ground from these: the box of its solid volumes in the lower "
		"half, and the footprint of those in the bottom eighth (wheels, skids) [orig: "
		"Threedi_BuildCollisionModelFromChunks @ 0x5b4455..0x5b45db].";
const char *const kModelPartSphereWords =
		"A part's bound sphere on the drawn LOD: no reader in the game is witnessed.";

} // namespace opennova::editor
