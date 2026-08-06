// Unit tests for threedi_3di3_ground_anchor: the placement anchor resolution
// used by the mission editor / runtime (ground userpoint, else the model
// ORIGIN — shipped missions place userpoint-less models with origin exactly on
// the terrain; an earlier bounding-center fallback buried them by half a
// model). Synthetic models only — no fixtures, no allocation/free (the helper
// is read-only).

#include <stdio.h>
#include <string.h>

#include "threedi/threedi_3di3.h"

static int failures = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    }
}

static int approx(float a, float b) {
    float d = a - b;
    if (d < 0.0f) {
        d = -d;
    }
    return d < 1e-5f;
}

int main(void) {
    // 1. A userpoint named "ground" (case-insensitive) takes priority over any part.
    {
        Threedi3di3 model;
        memset(&model, 0, sizeof(model));

        ThreediUserPoint ups[2];
        memset(ups, 0, sizeof(ups));
        strcpy(ups[0].name, "muzzle");
        ups[0].x = 9 << 16;
        ups[0].y = 9 << 16;
        ups[0].z = 9 << 16;
        strcpy(ups[1].name, "GrOuNd"); // mixed case must still match
        // Authored 16.16 source triple; the anchor reports the decoded model
        // space (x->z, y->-x, z->y).
        ups[1].x = 3 << 16;
        ups[1].y = -(1 << 16);
        ups[1].z = 2 << 16;
        model.user_points = ups;
        model.user_point_count = 2;

        // A part with a different center, to prove the userpoint is preferred.
        ThreediRenderObject part;
        memset(&part, 0, sizeof(part));
        part.bounding_center[0] = -5.0f;
        part.bounding_center[1] = -5.0f;
        part.bounding_center[2] = -5.0f;
        ThreediLod lod;
        memset(&lod, 0, sizeof(lod));
        lod.render_objects = &part;
        lod.render_object_count = 1;
        model.lods = &lod;
        model.lod_count = 1;

        float out[3] = {0.0f, 0.0f, 0.0f};
        check(threedi_3di3_ground_anchor(&model, out) == 1, "ground userpoint should be found");
        check(approx(out[0], 1.0f) && approx(out[1], 2.0f) && approx(out[2], 3.0f),
              "ground userpoint position decoded into model space");
    }

    // 2. No ground userpoint -> the model ORIGIN, never a part center: shipped
    //    missions place such models with origin exactly on the terrain, so any
    //    geometric fallback would mis-ground them (the old part-0 bounding
    //    center buried models by their center height).
    {
        Threedi3di3 model;
        memset(&model, 0, sizeof(model));

        ThreediUserPoint up;
        memset(&up, 0, sizeof(up));
        strcpy(up.name, "exhaust");
        up.x = 7 << 16;
        model.user_points = &up;
        model.user_point_count = 1;

        ThreediRenderObject parts[2];
        memset(parts, 0, sizeof(parts));
        parts[0].bounding_center[0] = 4.0f; // present, must be IGNORED
        parts[0].bounding_center[1] = 5.0f;
        parts[0].bounding_center[2] = 6.0f;
        parts[1].bounding_center[0] = 99.0f;
        ThreediLod lod;
        memset(&lod, 0, sizeof(lod));
        lod.render_objects = parts;
        lod.render_object_count = 2;
        model.lods = &lod;
        model.lod_count = 1;

        float out[3] = {9.0f, 9.0f, 9.0f};
        check(threedi_3di3_ground_anchor(&model, out) == 1, "userpoint-less model still anchors");
        check(approx(out[0], 0.0f) && approx(out[1], 0.0f) && approx(out[2], 0.0f),
              "the fallback anchor is the model origin, not part geometry");
    }

    // 3. Empty model -> origin anchor too (an anchor always exists for a valid model).
    {
        Threedi3di3 model;
        memset(&model, 0, sizeof(model));
        float out[3] = {42.0f, 42.0f, 42.0f};
        check(threedi_3di3_ground_anchor(&model, out) == 1, "empty model anchors at its origin");
        check(approx(out[0], 0.0f) && approx(out[1], 0.0f) && approx(out[2], 0.0f),
              "empty model anchor is the origin");
    }

    // 4. A name that merely starts with "ground" must NOT match — proven by the
    //    anchor landing on the origin fallback, not the userpoint's position.
    {
        Threedi3di3 model;
        memset(&model, 0, sizeof(model));
        ThreediUserPoint up;
        memset(&up, 0, sizeof(up));
        strcpy(up.name, "groundzero");
        up.x = 1 << 16;
        model.user_points = &up;
        model.user_point_count = 1;
        float out[3] = {9.0f, 9.0f, 9.0f};
        check(threedi_3di3_ground_anchor(&model, out) == 1, "'groundzero' model still anchors");
        check(approx(out[2], 0.0f), "'groundzero' must not match 'ground' (origin fallback used)");
    }

    if (failures == 0) {
        printf("ground_anchor: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
