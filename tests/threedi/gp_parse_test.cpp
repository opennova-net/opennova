// GP format parsing test - verifies GP parser handles files correctly.
// Uses fixtures/threedi/gp directory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/dirent_compat.h"
#include <sys/stat.h>

#include "threedi/threedi_gp.h"
#include "threedi/threedi_ir.h"
#include "common/test_paths.h"

static int gp_lght_bgr_converts_to_rgb(void) {
    ThreediGpFile gp;
    threedi_gp_init(&gp);
    gp.light_count = 1;
    gp.lights = (ThreediGpLight *)calloc(1, sizeof(ThreediGpLight));
    if (!gp.lights) return 0;

    /* The GP payload stores both light colors as B,G,R, matching LGHT in
       3DI3. Use distinct channels so an accidental byte-for-byte RGB copy
       cannot pass. */
    gp.lights[0].color_start[0] = 11;
    gp.lights[0].color_start[1] = 22;
    gp.lights[0].color_start[2] = 33;
    gp.lights[0].color_end[0] = 44;
    gp.lights[0].color_end[1] = 55;
    gp.lights[0].color_end[2] = 66;

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    const int converted = threedi_ir_from_gp(&gp, &ir) == 0;
    const int correct =
        converted && ir.light_count == 1 &&
        ir.lights[0].color_start[0] == 33.0f / 255.0f &&
        ir.lights[0].color_start[1] == 22.0f / 255.0f &&
        ir.lights[0].color_start[2] == 11.0f / 255.0f &&
        ir.lights[0].color_end[0] == 66.0f / 255.0f &&
        ir.lights[0].color_end[1] == 55.0f / 255.0f &&
        ir.lights[0].color_end[2] == 44.0f / 255.0f;
    if (!correct) {
        fprintf(stderr, "GP LGHT BGR-to-RGB conversion mismatch\n");
    }
    threedi_ir_free(&ir);
    threedi_gp_free(&gp);
    return correct;
}

static int has_extension(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    return strcmp(name + nlen - elen, ext) == 0;
}

static int is_gp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t magic[4];
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    if (n < 3) return 0;
    return memcmp(magic, "GPM", 3) == 0 ||
           memcmp(magic, "GPS", 3) == 0 ||
           memcmp(magic, "GPP", 3) == 0;
}

static int collision_ir_matches_gp(const char *path,
                                   const ThreediGpCollision *src,
                                   const ThreediIRCollision *ir) {
    if (!ir) {
        fprintf(stderr, "Missing collision IR for %s\n", path);
        return 0;
    }
    if (src->object_count < 0 ||
        ir->object_count != (size_t)src->object_count) {
        fprintf(stderr, "Collision object count mismatch for %s\n", path);
        return 0;
    }
    if (ir->vertex_count != (size_t)src->vertex_count ||
        ir->normal_count != (size_t)src->normal_count ||
        ir->face_count != (size_t)src->face_count ||
        ir->translation_count != (size_t)src->translation_count) {
        fprintf(stderr, "Collision mesh/translation count mismatch for %s\n", path);
        return 0;
    }
    if (!threedi_ir_collision_is_runtime_safe(ir)) {
        fprintf(stderr, "Runtime-unsafe collision IR for %s\n", path);
        return 0;
    }
    for (size_t i = 0; i < ir->object_count; ++i) {
        const ThreediGpCollisionObject *s = &src->objects[i];
        const ThreediIRCollisionObject *d = &ir->objects[i];
        if (d->num_vertices != s->vertex_count ||
            d->num_faces != s->face_count ||
            d->num_planes != s->normal_count ||
            d->num_bounding_volumes != s->volume_count ||
            d->parent_subobject_index != s->parent_subobject ||
            d->offset[0] != s->translation[0] ||
            d->offset[1] != s->translation[1] ||
            d->offset[2] != s->translation[2] ||
            d->min[0] != s->bbox_min_x ||
            d->min[1] != s->bbox_min_y ||
            d->min[2] != s->bbox_min_z ||
            d->max[0] != s->bbox_max_x ||
            d->max[1] != s->bbox_max_y ||
            d->max[2] != s->bbox_max_z ||
            d->mid[0] != s->center[0] ||
            d->mid[1] != s->center[1] ||
            d->mid[2] != s->center[2] ||
            d->radius != s->bounding_sphere_radius) {
            fprintf(stderr, "Collision object metadata mismatch for %s\n", path);
            return 0;
        }
    }
    for (size_t i = 0; i < ir->normal_count; ++i) {
        const ThreediGpCollisionNormal *s = &src->normals[i];
        const ThreediIRCollisionNormal *d = &ir->normals[i];
        if (d->normal_q14[0] != s->nx || d->normal_q14[1] != s->ny ||
            d->normal_q14[2] != s->nz || d->dominant_axis != s->dominant_axis) {
            fprintf(stderr, "Collision normal mismatch for %s\n", path);
            return 0;
        }
    }
    for (size_t i = 0; i < ir->face_count; ++i) {
        const ThreediGpCollisionFace *s = &src->faces[i];
        const ThreediIRCollisionFace *d = &ir->faces[i];
        if ((uint16_t)d->vert_index[0] != s->vertex_indices[0] ||
            (uint16_t)d->vert_index[1] != s->vertex_indices[1] ||
            (uint16_t)d->vert_index[2] != s->vertex_indices[2] ||
            d->normal_index != s->normal_index || d->plane_dist_fp16 != s->plane_d ||
            d->min_fp16[0] != s->bbox_min_x || d->min_fp16[1] != s->bbox_min_y ||
            d->min_fp16[2] != s->bbox_min_z || d->max_fp16[0] != s->bbox_max_x ||
            d->max_fp16[1] != s->bbox_max_y || d->max_fp16[2] != s->bbox_max_z ||
            d->material_flags != (uint32_t)s->surface_flags ||
            d->poly_type != s->surface_type) {
            fprintf(stderr, "Collision face mismatch for %s\n", path);
            return 0;
        }
    }
    for (size_t i = 0; i < ir->volume_count; ++i) {
        const int32_t object_index = ir->volumes[i].object_index;
        if (object_index >= 0 && (size_t)object_index >= ir->object_count) {
            fprintf(stderr, "Collision volume object index out of range for %s\n", path);
            return 0;
        }
    }
    for (size_t i = 0; i < ir->translation_count; ++i) {
        if (ir->translations[i].translation[0] != src->translations[i].x ||
            ir->translations[i].translation[1] != src->translations[i].y ||
            ir->translations[i].translation[2] != src->translations[i].z) {
            fprintf(stderr, "Collision translation mismatch for %s\n", path);
            return 0;
        }
    }
    return 1;
}

static int test_gp_parse(const char *path) {
    ThreediGpFile gp;
    threedi_gp_init(&gp);

    if (threedi_gp_read(path, &gp) != 0) {
        fprintf(stderr, "Failed to parse %s\n", path);
        return 0;
    }

    // Basic sanity checks
    if (gp.header.mesh_type == THREEDI_GP_MESH_UNKNOWN) {
        fprintf(stderr, "Unknown mesh type for %s\n", path);
        threedi_gp_free(&gp);
        return 0;
    }

    if (gp.header.num_lods <= 0 || gp.header.num_lods > 4) {
        fprintf(stderr, "Invalid LOD count %d for %s\n", gp.header.num_lods, path);
        threedi_gp_free(&gp);
        return 0;
    }

    if (gp.rvert_count == 0) {
        fprintf(stderr, "No vertices for %s\n", path);
        threedi_gp_free(&gp);
        return 0;
    }

    // Verify collision object sizes if present
    if (gp.collision && gp.collision->object_count > 0) {
        for (int32_t i = 0; i < gp.collision->object_count; ++i) {
            ThreediGpCollisionObject *obj = &gp.collision->objects[i];
            // Each collision object should have reasonable counts
            if (obj->vertex_count < 0 || obj->face_count < 0 || obj->volume_count < 0) {
                fprintf(stderr, "Invalid collision object counts in %s\n", path);
                threedi_gp_free(&gp);
                return 0;
            }
        }
    }

    // Verify occlusion object sizes if present
    if (gp.occlusion && gp.occlusion->object_count > 0) {
        for (size_t i = 0; i < gp.occlusion->object_count; ++i) {
            ThreediGpOcclusionObject *obj = &gp.occlusion->objects[i];
            // Each occlusion object should have reasonable counts
            if (obj->num_vertices < 0 || obj->num_planes < 0 || obj->num_faces < 0) {
                fprintf(stderr, "Invalid occlusion object counts in %s\n", path);
                threedi_gp_free(&gp);
                return 0;
            }
        }
    }

    // Test IR conversion
    ThreediModelIR ir;
    threedi_ir_init(&ir);
    if (threedi_ir_from_gp(&gp, &ir) != 0) {
        fprintf(stderr, "Failed to convert %s to IR\n", path);
        threedi_gp_free(&gp);
        return 0;
    }

    // Verify IR has expected data
    if (ir.lod_count == 0) {
        fprintf(stderr, "No LODs in IR for %s\n", path);
        threedi_ir_free(&ir);
        threedi_gp_free(&gp);
        return 0;
    }
    if (gp.collision && !collision_ir_matches_gp(path, gp.collision, ir.collision)) {
        threedi_ir_free(&ir);
        threedi_gp_free(&gp);
        return 0;
    }

    threedi_ir_free(&ir);
    threedi_gp_free(&gp);
    return 1;
}

int main(void) {
    if (!gp_lght_bgr_converts_to_rgb()) {
        return EXIT_FAILURE;
    }

    const char *repo_root = test_paths_repo_root(__FILE__);
    char fixtures_dir[4096];
    char **files = NULL;
    size_t file_count = 0, file_cap = 0;
    size_t i;

    snprintf(fixtures_dir, sizeof(fixtures_dir), "%s/fixtures/threedi/gp", repo_root);

    DIR *d = opendir(fixtures_dir);
    if (!d) {
        fprintf(stderr, "No GP fixtures found at %s\n", fixtures_dir);
        return EXIT_FAILURE;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_extension(ent->d_name, ".3di")) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", fixtures_dir, ent->d_name);

        // Only test GP format files
        if (!is_gp_file(path)) continue;

        if (file_count >= file_cap) {
            file_cap = file_cap ? file_cap * 2 : 64;
            files = (char **)realloc(files, file_cap * sizeof(char *));
        }
        files[file_count] = (char *)malloc(strlen(path) + 1);
        strcpy(files[file_count], path);
        file_count++;
    }
    closedir(d);

    if (file_count == 0) {
        fprintf(stderr, "No GP format .3di files found in %s\n", fixtures_dir);
        free(files);
        return EXIT_FAILURE;
    }

    printf("Testing %zu GP format files...\n", file_count);

    size_t passed = 0, failed = 0;
    for (i = 0; i < file_count; ++i) {
        const char *name = strrchr(files[i], '/');
        name = name ? name + 1 : files[i];
        printf("  %s... ", name);
        fflush(stdout);

        if (test_gp_parse(files[i])) {
            printf("OK\n");
            passed++;
        } else {
            printf("FAILED\n");
            failed++;
        }
    }

    for (i = 0; i < file_count; ++i) free(files[i]);
    free(files);

    if (failed > 0) {
        printf("GP parse test: %zu passed, %zu failed\n", passed, failed);
        return EXIT_FAILURE;
    }

    printf("All %zu GP parse tests passed.\n", passed);
    return EXIT_SUCCESS;
}
