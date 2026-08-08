#pragma once

// Sim-side model derivations from a parsed .3di (ADR 0028): the runtime
// collision/occlusion model builders and the bound-sphere radius the entity
// init consumes, plus the model predicates the build gates read. Moved from
// the shell adapter's internal header — the bodies are the same structural
// translations, with their [orig] witnesses, now linkable by any engine
// consumer and testable headless.

#include <threedi/threedi_3di3.h>
#include <world/collision.h>
#include <world/occlusion.h>

namespace opennova::simassets {

// Build the runtime collision model from a parsed .3di CDTA block — the exact
// inverse of the parse scaling; sections mirror the COBJ grouping and each
// face's local CNRM index resolves against its object's run at build time
// [orig: the per-COBJ normal-run fixup in the collision builder @ 0x5b3bf0].
bool collision_model_from_3di(const ThreediCollisionModel *col,
                              opennova::world::CollisionModel &out,
                              bool allow_sphere_only = false);

// The model bound-sphere radius from the .3di itself — the entity+0
// boundRadius source [orig: Entity_InitFromModel @ 0x40dc30].
float model_bound_radius_from_3di(const Threedi3di3 &model);

// Build the runtime occlusion model from the parsed OCCL tables
// [orig: load_occlusion_model_data @ 0x5b4a00].
bool occlusion_model_from_3di(const Threedi3di3 &model,
                              opennova::world::OcclusionModel &out);

// LOD carries skinned geometry: the header mesh-type stamp, or any strip with
// a bone table. (The render adapter's NovaObjectData::is_skinned delegates
// here — one implementation per engine fact, ADR 0016.)
bool model_is_skinned(const Threedi3di3 &model, int lod_index);

// The model has a real collision presence: authored volumes, or (skinned
// models only) a face mesh or person spheres. This is the allow_sphere_only
// gate the collision build consumes for organics. (NovaObjectData::
// has_collision delegates here.)
bool model_has_collision(const Threedi3di3 &model);

} // namespace opennova::simassets
