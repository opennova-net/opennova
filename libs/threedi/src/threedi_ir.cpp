// Common Intermediate Representation (IR) utility functions

#include "threedi/threedi_ir.h"
#include "threedi/threedi_3di3.h"
#include "threedi/threedi_gp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void threedi_ir_init(ThreediModelIR *ir) {
    if (!ir) return;
    memset(ir, 0, sizeof(ThreediModelIR));
    ir->collision_lod = -1;
}

static void threedi_ir_lod_free(ThreediIRLod *lod) {
    if (!lod) return;
    free(lod->vertices);
    free(lod->indices);
    free(lod->primitives);
    free(lod->parts);
    free(lod->part_animations);
    memset(lod, 0, sizeof(ThreediIRLod));
}

static void threedi_ir_collision_free(ThreediIRCollision *col) {
    if (!col) return;
    free(col->vertices);
    free(col->normals);
    free(col->planes);
    free(col->volumes);
    free(col->faces);
    free(col->objects);
    free(col->translations);
}

static void threedi_ir_occlusion_free(ThreediIROcclusion *occ) {
    if (!occ) return;
    free(occ->vertices);
    free(occ->faces);
    free(occ->planes);
    free(occ->objects);
}

void threedi_ir_free(ThreediModelIR *ir) {
    if (!ir) return;

    // Free LODs
    if (ir->lods) {
        for (size_t i = 0; i < ir->lod_count; ++i) {
            threedi_ir_lod_free(&ir->lods[i]);
        }
        free(ir->lods);
    }

    // Free materials
    free(ir->materials);

    // Free lights
    free(ir->lights);

    // Free userpoints
    free(ir->userpoints);

    // Free collision
    if (ir->collision) {
        threedi_ir_collision_free(ir->collision);
        free(ir->collision);
    }

    // Free occlusion
    if (ir->occlusion) {
        threedi_ir_occlusion_free(ir->occlusion);
        free(ir->occlusion);
    }

    // Free control registers
    free(ir->control_registers);

    // Free matrices
    free(ir->matrices);

    // Zero out the structure
    memset(ir, 0, sizeof(ThreediModelIR));
}

// ============================================================================
// Unified read function
// ============================================================================

int threedi_ir_read(const char *path, ThreediModelIR *out) {
    if (!path || !out) return -1;

    // Read file header to detect format
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t magic[4];
    size_t nread = fread(magic, 1, 4, f);
    fclose(f);

    if (nread < 4) return -1;

    // Check for Modern format (3DI3)
    if (memcmp(magic, "3DI3", 4) == 0) {
        Threedi3di3 model;
        memset(&model, 0, sizeof(model));

        if (threedi_3di3_read(path, &model) != 0) {
            threedi_3di3_free(&model);
            return -1;
        }

        int result = threedi_ir_from_3di3(&model, out);
        threedi_3di3_free(&model);
        return result;
    }

    // Check for GP format
    ThreediGpMeshType gp_type = threedi_gp_detect(magic, 4);
    if (gp_type != THREEDI_GP_MESH_UNKNOWN) {
        ThreediGpFile gp;
        threedi_gp_init(&gp);

        if (threedi_gp_read(path, &gp) != 0) {
            threedi_gp_free(&gp);
            return -1;
        }

        int result = threedi_ir_from_gp(&gp, out);
        threedi_gp_free(&gp);
        return result;
    }

    return -1; // Unknown format
}
