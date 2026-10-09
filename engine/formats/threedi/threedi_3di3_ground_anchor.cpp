// Placement ground-anchor resolution for 3DI3 models.
//
// When a placed object's stored position marks where it meets the terrain, the
// renderer must offset the model so its "ground" reference point lands there
// rather than the model origin. This computes that reference point:
//   1. The first userpoint named "ground" (case-insensitive — shipped assets use
//      lowercase "ground" while authoring/spec language says "Ground").
//   2. Otherwise the model ORIGIN. Shipped JO missions place userpoint-less
//      models (Jungle Tree #7, the M939 trucks, Beach Hut #1, Power Generator
//      Housing, ...) with origin − terrain_height == 0 exactly, while models
//      WITH a ground userpoint sit at −anchor — i.e. the original tool grounds
//      the userpoint when present and the origin otherwise. An earlier fallback
//      here (part-0 bounding-sphere center) buried every userpoint-less model
//      by its center height.
//
// Returned in model space (threedi_user_point_position's axis order) so callers
// apply their own coordinate convention exactly once (the placement consumers
// feed the result through their single negate-x model-space map).

#include <formats/threedi/threedi_3di3.h>

namespace opennova::threedi {

int threedi_3di3_ground_anchor(const Threedi3di3 *model, float out[3]) {
    if (!model || !out) {
        return 0;
    }

    // 1. "ground" userpoint. Userpoints are model-global, so the LOD is irrelevant here.
    const int ground = threedi_3di3_find_user_point(model, "ground");
    if (ground >= 0) {
        threedi_user_point_position(&model->user_points[ground], out);
        return 1;
    }

    // 2. Fallback: the model origin — what the original tool grounds when no
    //    userpoint exists (see file comment for the shipped-data evidence).
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    return 1;
}

} // namespace opennova::threedi
