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

#include <stddef.h>

// ASCII case fold. Avoids strcasecmp (POSIX) / _stricmp (MSVC), neither of which
// is portable across the toolchains this library builds on.
static char ground_to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Compare a fixed-capacity, null-terminated name against a lowercase literal,
// case-insensitively. Stops at the literal's terminator, so it never reads past
// either buffer. Returns 1 on match.
static int name_matches_ignore_case(const char *name, size_t name_cap, const char *lower) {
    for (size_t i = 0; i < name_cap; ++i) {
        const char a = ground_to_lower(name[i]);
        const char b = lower[i];
        if (a != b) {
            return 0;
        }
        if (b == '\0') {
            return 1; // both terminated at the same position
        }
    }
    return 0; // name ran its full capacity without terminating: not a match
}

extern "C" int threedi_3di3_ground_anchor(const Threedi3di3 *model, float out[3]) {
    if (!model || !out) {
        return 0;
    }

    // 1. "ground" userpoint. Userpoints are model-global, so the LOD is irrelevant here.
    for (size_t i = 0; i < model->user_point_count; ++i) {
        const ThreediUserPoint *up = &model->user_points[i];
        if (name_matches_ignore_case(up->name, sizeof(up->name), "ground")) {
            threedi_user_point_position(up, out);
            return 1;
        }
    }

    // 2. Fallback: the model origin — what the original tool grounds when no
    //    userpoint exists (see file comment for the shipped-data evidence).
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    return 1;
}
