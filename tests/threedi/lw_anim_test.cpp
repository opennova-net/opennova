// Land Warrior animation parser + pose-sampler test.
//
// Golden values derived from Dflw.exe RE and the checked-in dflw badguy fixtures
// (chr_file=player01, anim_def=enemy00). Validates the SAF/KSA/ACA/ANM parsers
// byte-exactly and the pose sampler's identity + 90-degree-Rx invariants.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "threedi/threedi_lw.h"
#include "threedi/threedi_lw_anim.h"
#include "common/test_paths.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL: ");                                       \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(b);
        return NULL;
    }
    *len = (size_t)sz;
    return b;
}

static int close_f(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_detect(const char *root) {
    char ksa_p[4096], saf_p[4096];
    snprintf(ksa_p, sizeof(ksa_p), "%s/fixtures/threedi/lw/PLAYER01.KSA", root);
    snprintf(saf_p, sizeof(saf_p), "%s/fixtures/threedi/lw/3CROU01A.SAF", root);
    size_t kl = 0, sl = 0;
    uint8_t *k = read_file(ksa_p, &kl);
    uint8_t *s = read_file(saf_p, &sl);
    CHECK(k && s, "detect: fixtures unreadable");
    if (k && s) {
        CHECK(threedi_lw_anim_detect_ksa(k, kl) == 1, "detect_ksa(KSA) != 1");
        CHECK(threedi_lw_anim_detect_ksa(s, sl) == 0, "detect_ksa(SAF) != 0");
        CHECK(threedi_lw_anim_detect_saf(s, sl) == 1, "detect_saf(SAF) != 1");
        CHECK(threedi_lw_anim_detect_saf(k, kl) == 0, "detect_saf(KSA) != 0");
    }
    free(k);
    free(s);
}

static void test_ksa(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/PLAYER01.KSA", root);
    ThreediLwKsa ksa;
    if (threedi_lw_ksa_read(path, &ksa) != 0) {
        fprintf(stderr, "  FAIL: cannot parse PLAYER01.KSA\n");
        ++g_failures;
        return;
    }
    CHECK(ksa.version == 1, "KSA version %u != 1", ksa.version);
    CHECK(ksa.slot_count == 255, "KSA slot_count %u != 255", ksa.slot_count);
    CHECK(ksa.blob_size == 1412676u, "KSA blob_size %u != 1412676", ksa.blob_size);
    CHECK(ksa.total_frames == 15972u, "KSA total_frames %u != 15972", ksa.total_frames);

    uint32_t recomputed = 0;
    for (uint32_t s = 0; s < ksa.slot_count; ++s) recomputed += ksa.slots[s].frame_count;
    CHECK(recomputed == ksa.total_frames, "KSA recomputed frames %u != total %u", recomputed,
          ksa.total_frames);

    // A non-empty slot's frames alias the flat array at its frame_base.
    int checked_alias = 0;
    for (uint32_t s = 0; s < ksa.slot_count; ++s) {
        if (ksa.slots[s].frame_count > 0) {
            CHECK(ksa.slots[s].frames == ksa.frames + ksa.slots[s].frame_base,
                  "KSA slot %u frames pointer not aliased to frame_base", s);
            checked_alias = 1;
            break;
        }
    }
    CHECK(checked_alias, "KSA had no non-empty slot to spot-check");
    threedi_lw_ksa_free(&ksa);
    threedi_lw_ksa_free(&ksa); // double-free must be a no-op
}

static void test_saf(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/3CROU01A.SAF", root);
    size_t len = 0;
    uint8_t *raw = read_file(path, &len);
    if (!raw) {
        fprintf(stderr, "  FAIL: cannot read 3CROU01A.SAF\n");
        ++g_failures;
        return;
    }
    ThreediLwSaf saf;
    if (threedi_lw_saf_parse(raw, len, &saf) != 0) {
        fprintf(stderr, "  FAIL: cannot parse 3CROU01A.SAF\n");
        ++g_failures;
        free(raw);
        return;
    }
    CHECK(saf.frame_count == 39, "SAF frame_count %u != 39", saf.frame_count);

    // +0x80 bias: frame 0 part 0 byte0 is at file offset 0x10 + 0x34 (frame hdr) = 0x44.
    if (saf.frame_count > 0 && len > 0x44) {
        uint8_t disk_b0 = raw[0x44];
        CHECK(saf.frames[0].parts[0].b0 == (uint8_t)(disk_b0 + 0x80),
              "SAF +0x80 bias: parts[0].b0=%u != (disk %u + 0x80)", saf.frames[0].parts[0].b0,
              disk_b0);
    }
    // -30 clamp on root[4] for every frame.
    for (uint32_t f = 0; f < saf.frame_count; ++f) {
        CHECK(saf.frames[f].root[4] <= (int16_t)-30, "SAF frame %u root[4]=%d > -30", f,
              (int)saf.frames[f].root[4]);
    }
    threedi_lw_saf_free(&saf);
    free(raw);
}

static void test_aca(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/PLAYER01.ACA", root);
    ThreediLwAca aca;
    if (threedi_lw_aca_read(path, &aca) != 0) {
        fprintf(stderr, "  FAIL: cannot parse PLAYER01.ACA\n");
        ++g_failures;
        return;
    }
    CHECK(aca.entry_count == 53, "ACA entry_count %u != 53", aca.entry_count);
    if (aca.entry_count > 0) {
        // First slot entry's saf is the doc's 3crou01a.
        CHECK(strstr(aca.entries[0].saf_name, "3crou01a") != NULL ||
                  strstr(aca.entries[0].saf_name, "3CROU01A") != NULL,
              "ACA first saf_name '%s' unexpected", aca.entries[0].saf_name);
    }
    threedi_lw_aca_free(&aca);
}

static void test_anm(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/ENEMY00.ANM", root);
    ThreediLwAnm anm;
    if (threedi_lw_anm_read(path, &anm) != 0) {
        fprintf(stderr, "  FAIL: cannot parse ENEMY00.ANM\n");
        ++g_failures;
        return;
    }
    CHECK(anm.entry_count == 124, "ANM entry_count %u != 124", anm.entry_count);
    if (anm.entry_count > 0) {
        CHECK(anm.entries[0].name[0] != '\0', "ANM first entry has empty name");
    }
    threedi_lw_anm_free(&anm);
}

static void test_sampler(void) {
    // Two-bone synthetic skeleton: bone0 root at origin, bone1 child offset +100x.
    ThreediLwSubObject so[2];
    memset(so, 0, sizeof(so));
    so[0].parent = -1;
    so[0].pos[0] = 0; so[0].pos[1] = 0; so[0].pos[2] = 0;
    so[1].parent = 0;
    so[1].pos[0] = 100; so[1].pos[1] = 0; so[1].pos[2] = 0;

    // Identity frame: all angle bytes zero.
    ThreediLwAnimFrame frame;
    memset(&frame, 0, sizeof(frame));

    ThreediLwAnimPose pose;
    CHECK(threedi_lw_anim_sample(&frame, so, 2, 1.0f, &pose) == 0, "sampler returned error");
    CHECK(pose.bone_count == 2, "sampler bone_count %u != 2", pose.bone_count);
    if (pose.bone_count == 2) {
        // Both bones identity rotation.
        for (int b = 0; b < 2; ++b) {
            CHECK(close_f(pose.bones[b].local[0][0], 1) && close_f(pose.bones[b].local[1][1], 1) &&
                      close_f(pose.bones[b].local[2][2], 1),
                  "bone %d identity rotation expected", b);
        }
        // Bone1 rest offset passes through as local translation.
        CHECK(close_f(pose.bones[1].local[0][3], 100) && close_f(pose.bones[1].local[1][3], 0) &&
                  close_f(pose.bones[1].local[2][3], 0),
              "bone1 local translation %.2f,%.2f,%.2f != 100,0,0", pose.bones[1].local[0][3],
              pose.bones[1].local[1][3], pose.bones[1].local[2][3]);
    }
    threedi_lw_anim_pose_free(&pose);

    // Bone1 b2 = 64 => +90deg Rx (64 * 2pi/256 = pi/2).
    frame.parts[1].b2 = 64;
    CHECK(threedi_lw_anim_sample(&frame, so, 2, 1.0f, &pose) == 0, "sampler(Rx90) error");
    if (pose.bone_count == 2) {
        // Rx(90) = [[1,0,0],[0,0,-1],[0,1,0]]
        CHECK(close_f(pose.bones[1].local[1][1], 0) && close_f(pose.bones[1].local[1][2], -1) &&
                  close_f(pose.bones[1].local[2][1], 1) && close_f(pose.bones[1].local[2][2], 0),
              "bone1 Rx(90) rows wrong: [%.3f %.3f / %.3f %.3f]", pose.bones[1].local[1][1],
              pose.bones[1].local[1][2], pose.bones[1].local[2][1], pose.bones[1].local[2][2]);
    }
    threedi_lw_anim_pose_free(&pose);
    threedi_lw_anim_pose_free(&pose); // double-free no-op
}

int main(void) {
    const char *root = test_paths_repo_root(__FILE__);
    printf("LW animation tests (fixtures/threedi/lw)\n");
    test_detect(root);
    test_ksa(root);
    test_saf(root);
    test_aca(root);
    test_anm(root);
    test_sampler();
    if (g_failures) {
        printf("LW anim test: %d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    printf("All LW animation checks passed.\n");
    return EXIT_SUCCESS;
}
