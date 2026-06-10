// Unit tests for threedi_ir_ground_anchor: the placement anchor resolution used
// by the mission editor / runtime (ground userpoint, else part-0 bounding center).
// Synthetic IR only — no fixtures, no allocation/free (the helper is read-only).

#include <stdio.h>
#include <string.h>

#include "threedi/threedi_ir.h"

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
        ThreediModelIR ir;
        memset(&ir, 0, sizeof(ir));

        ThreediIRUserPoint ups[2];
        memset(ups, 0, sizeof(ups));
        strcpy(ups[0].name, "muzzle");
        ups[0].position[0] = 9.0f;
        ups[0].position[1] = 9.0f;
        ups[0].position[2] = 9.0f;
        strcpy(ups[1].name, "GrOuNd"); // mixed case must still match
        ups[1].position[0] = 1.0f;
        ups[1].position[1] = 2.0f;
        ups[1].position[2] = 3.0f;
        ir.userpoints = ups;
        ir.userpoint_count = 2;

        // A part with a different center, to prove the userpoint is preferred.
        ThreediIRPart part;
        memset(&part, 0, sizeof(part));
        part.bounding_center[0] = -5.0f;
        part.bounding_center[1] = -5.0f;
        part.bounding_center[2] = -5.0f;
        ThreediIRLod lod;
        memset(&lod, 0, sizeof(lod));
        lod.parts = &part;
        lod.part_count = 1;
        ir.lods = &lod;
        ir.lod_count = 1;

        float out[3] = {0.0f, 0.0f, 0.0f};
        check(threedi_ir_ground_anchor(&ir, 0, out) == 1, "ground userpoint should be found");
        check(approx(out[0], 1.0f) && approx(out[1], 2.0f) && approx(out[2], 3.0f),
              "ground userpoint position returned verbatim (IR order)");
    }

    // 2. No ground userpoint -> bounding center of part 0 (not part 1).
    {
        ThreediModelIR ir;
        memset(&ir, 0, sizeof(ir));

        ThreediIRUserPoint up;
        memset(&up, 0, sizeof(up));
        strcpy(up.name, "exhaust");
        up.position[0] = 7.0f;
        ir.userpoints = &up;
        ir.userpoint_count = 1;

        ThreediIRPart parts[2];
        memset(parts, 0, sizeof(parts));
        parts[0].bounding_center[0] = 4.0f;
        parts[0].bounding_center[1] = 5.0f;
        parts[0].bounding_center[2] = 6.0f;
        parts[1].bounding_center[0] = 99.0f; // must use part 0
        ThreediIRLod lod;
        memset(&lod, 0, sizeof(lod));
        lod.parts = parts;
        lod.part_count = 2;
        ir.lods = &lod;
        ir.lod_count = 1;

        float out[3] = {0.0f, 0.0f, 0.0f};
        check(threedi_ir_ground_anchor(&ir, 0, out) == 1, "fallback to part 0 center");
        check(approx(out[0], 4.0f) && approx(out[1], 5.0f) && approx(out[2], 6.0f),
              "part 0 bounding center returned verbatim");
    }

    // 3. Empty model -> no anchor; out left untouched.
    {
        ThreediModelIR ir;
        memset(&ir, 0, sizeof(ir));
        float out[3] = {42.0f, 42.0f, 42.0f};
        check(threedi_ir_ground_anchor(&ir, 0, out) == 0, "empty model has no anchor");
        check(approx(out[0], 42.0f) && approx(out[1], 42.0f) && approx(out[2], 42.0f),
              "out left untouched when no anchor found");
    }

    // 4. Out-of-range LOD and no userpoint -> no fallback.
    {
        ThreediModelIR ir;
        memset(&ir, 0, sizeof(ir));
        ThreediIRPart part;
        memset(&part, 0, sizeof(part));
        ThreediIRLod lod;
        memset(&lod, 0, sizeof(lod));
        lod.parts = &part;
        lod.part_count = 1;
        ir.lods = &lod;
        ir.lod_count = 1;
        float out[3] = {0.0f, 0.0f, 0.0f};
        check(threedi_ir_ground_anchor(&ir, 5, out) == 0, "out-of-range lod yields no fallback");
    }

    // 5. A name that merely starts with "ground" must NOT match.
    {
        ThreediModelIR ir;
        memset(&ir, 0, sizeof(ir));
        ThreediIRUserPoint up;
        memset(&up, 0, sizeof(up));
        strcpy(up.name, "groundzero");
        up.position[0] = 1.0f;
        ir.userpoints = &up;
        ir.userpoint_count = 1;
        float out[3] = {0.0f, 0.0f, 0.0f};
        check(threedi_ir_ground_anchor(&ir, 0, out) == 0, "'groundzero' must not match 'ground'");
    }

    if (failures == 0) {
        printf("ground_anchor: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
