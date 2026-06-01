// Land Warrior animation parsers (SAF1/KSA/ACA/ANM) + pose sampler.
//
// Byte-faithful to Dflw.exe (imagebase 0x400000). Mirrors threedi_lw.cpp style.
//   LWAnim_LoadSAF1     @ 0x44A0B0   LWAnim_LoadKsa      @ 0x449F50
//   LWAnim_LoadAca      @ 0x44AC50   LWAnim_ParseAcaLine @ 0x44AB30
//   LWAnim_LoadAnm      @ 0x44AA60   LWAnim_ParseAnmLine @ 0x44A6D0
//   LWAnim_PoseSkeleton @ 0x4A0C00 (pose recipe: notes/3di-lw/lw-saf-pose.md)

#include "threedi/threedi_lw_anim.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Byte readers + overflow-checked size math (kept file-static, like threedi_lw.cpp)
// ============================================================================

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Bit-cast little-endian u32 to float (no aliasing UB).
static float rd_f32(const uint8_t *p) {
    uint32_t u = rd_u32(p);
    float r;
    memcpy(&r, &u, sizeof(r));
    return r;
}

static void copy_name(char *dst, size_t dst_size, const uint8_t *src, size_t field) {
    size_t n = field < dst_size - 1 ? field : dst_size - 1;
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) dst[i] = (char)src[i];
    dst[i] = '\0';
}

static int checked_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return -1;
    *out = a * b;
    return 0;
}

static int checked_add_size(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) return -1;
    *out = a + b;
    return 0;
}

// __ftol semantics: truncate toward zero (float->int64), store LOW 16 bits (wrap,
// not saturate). Matches Dflw __ftol + 'mov [esi+k],ax'.
static int16_t saf_ftol(float x) {
    int64_t t = (int64_t)x; // C truncates toward zero
    return (int16_t)(uint16_t)(uint64_t)t;
}

static int read_whole_file(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size ? (size_t)size : 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return -1;
    }
    *out_buf = buf;
    *out_len = (size_t)size;
    return 0;
}

// ============================================================================
// Detect helpers
// ============================================================================

int threedi_lw_anim_detect_saf(const uint8_t *data, size_t len) {
    if (!data || len < THREEDI_LW_SAF1_HEADER_SIZE) return 0;
    return rd_u32(data) == THREEDI_LW_SAF1_MAGIC ? 1 : 0;
}

int threedi_lw_anim_detect_ksa(const uint8_t *data, size_t len) {
    if (!data || len < THREEDI_LW_KSA_HEADER_SIZE) return 0;
    if (data[0] != 'K' || data[1] != 'S' || data[2] != 'A') return 0; // 3-byte magic
    return rd_u32(data + 0x04) == THREEDI_LW_KSA_VERSION ? 1 : 0;
}

// ============================================================================
// SAF1 (LWAnim_LoadSAF1 @ 0x44A0B0)
// ============================================================================

void threedi_lw_saf_init(ThreediLwSaf *out) {
    if (out) memset(out, 0, sizeof(*out));
}

void threedi_lw_saf_free(ThreediLwSaf *out) {
    if (!out) return;
    free(out->frames);
    memset(out, 0, sizeof(*out));
}

int threedi_lw_saf_parse(const uint8_t *data, size_t len, ThreediLwSaf *out) {
    if (!data || !out) return -1;
    threedi_lw_saf_init(out);

    if (len < THREEDI_LW_SAF1_HEADER_SIZE) return -1;
    if (rd_u32(data + 0x00) != THREEDI_LW_SAF1_MAGIC) return -1; // @0x44a103
    out->version = rd_u32(data + 0x04);                          // unused by engine
    out->frame_count = rd_u32(data + 0x08);                      // @0x44a115
    // data+0x0C read into header but NEVER consumed (MR's ==52 check is bogus).

    if (out->frame_count == 0) return 0; // valid empty clip

    out->frames = (ThreediLwAnimFrame *)calloc(out->frame_count, sizeof(ThreediLwAnimFrame));
    if (!out->frames) {
        threedi_lw_saf_free(out);
        return -1;
    }

    size_t cur = THREEDI_LW_SAF1_HEADER_SIZE; // sequential cursor @ file off 0x10
    for (uint32_t f = 0; f < out->frame_count; ++f) {
        // a. 52-byte frame header
        if (cur > len || (size_t)THREEDI_LW_SAF1_FRAME_HDR > len - cur) {
            threedi_lw_saf_free(out);
            return -1;
        }
        const uint8_t *fh = data + cur;
        uint32_t part_count = rd_u32(fh + 0x00); // v18[0]

        // b. root tail: 9 i16 in source-float order [5,8,6,9,7,10,3,2,4]
        ThreediLwAnimFrame *rf = &out->frames[f];
        float v[11];
        for (int k = 2; k <= 10; ++k) v[k] = rd_f32(fh + 4 * k);

        rf->root[0] = saf_ftol(v[5] * 85.333336f);    // @0x3C
        rf->root[1] = saf_ftol(v[8] * 85.333336f);    // @0x3E
        rf->root[2] = saf_ftol(v[6] * 85.333336f);    // @0x40
        rf->root[3] = saf_ftol(v[9] * 85.333336f);    // @0x42
        rf->root[4] = saf_ftol(v[7] * 85.333336f);    // @0x44
        if (rf->root[4] > (int16_t)-30) rf->root[4] = (int16_t)-30; // clamp <= -30 (@0x44a1d5)
        rf->root[5] = saf_ftol(v[10] * 85.333336f);   // @0x46
        rf->root[6] = saf_ftol(v[3] * 1365.3334f);    // @0x48  [+]
        rf->root[7] = saf_ftol(v[2] * -1365.3334f);   // @0x4A  [-]
        rf->root[8] = saf_ftol(v[4] * -1365.3334f);   // @0x4C  [-]
        // zero_pad already 0 from calloc (engine zeroes 0x4E..0x57 @0x44a231)

        cur += THREEDI_LW_SAF1_FRAME_HDR;

        // c. part records: read part_count, store at most 15; cursor advances by full count.
        for (uint32_t i = 0; i < part_count; ++i) {
            if (cur > len || (size_t)THREEDI_LW_SAF1_PART_REC > len - cur) {
                threedi_lw_saf_free(out);
                return -1;
            }
            const uint8_t *pr = data + cur;
            if (i < THREEDI_LW_MAX_PARTS) {
                rf->parts[i].b0 = (uint8_t)(pr[0] + 0x80); // +0x80 bias on byte0 ONLY (@0x44a25e)
                rf->parts[i].b1 = pr[1];                   // @0x44a267
                rf->parts[i].b2 = pr[2];                   // @0x44a26a
                rf->parts[i].pad = 0;                      // engine leaves uninit; we zero
            }
            cur += THREEDI_LW_SAF1_PART_REC; // disk byte3 discarded
        }
        // records [part_count..14] left zero by calloc
    }

    return 0;
}

int threedi_lw_saf_read(const char *path, ThreediLwSaf *out) {
    if (!path || !out) return -1;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (read_whole_file(path, &buf, &len) != 0) return -1;
    int rc = threedi_lw_saf_parse(buf, len, out);
    free(buf);
    return rc;
}

// ============================================================================
// KSA (LWAnim_LoadKsa @ 0x449F50) — pre-baked 88-byte runtime frames
// ============================================================================

void threedi_lw_ksa_init(ThreediLwKsa *out) {
    if (out) memset(out, 0, sizeof(*out));
}

void threedi_lw_ksa_free(ThreediLwKsa *out) {
    if (!out) return;
    free(out->slots);
    free(out->frames); // slots[*].frames alias this; do not free separately
    memset(out, 0, sizeof(*out));
}

// Read an already-baked 88-byte runtime frame straight into the struct.
// KSA stores byte0 ALREADY biased; no +0x80 re-applied.
static void decode_runtime_frame(const uint8_t *p, ThreediLwAnimFrame *rf) {
    for (int i = 0; i < THREEDI_LW_MAX_PARTS; ++i) {
        rf->parts[i].b0 = p[i * 4 + 0];
        rf->parts[i].b1 = p[i * 4 + 1];
        rf->parts[i].b2 = p[i * 4 + 2];
        rf->parts[i].pad = p[i * 4 + 3];
    }
    for (int k = 0; k < THREEDI_LW_ROOT_I16_COUNT; ++k)
        rf->root[k] = (int16_t)rd_u16(p + 0x3C + 2 * k);
    memcpy(rf->zero_pad, p + 0x4E, 10);
}

int threedi_lw_ksa_parse(const uint8_t *data, size_t len, ThreediLwKsa *out) {
    if (!data || !out) return -1;
    threedi_lw_ksa_init(out);

    if (len < THREEDI_LW_KSA_HEADER_SIZE) return -1;
    if (data[0] != 'K' || data[1] != 'S' || data[2] != 'A') return -1; // @0x449f85
    out->version = rd_u32(data + 0x04);
    if (out->version != THREEDI_LW_KSA_VERSION) return -1; // @0x449fa6
    out->blob_size = rd_u32(data + 0x2C);                  // @0x449fb0
    out->slot_count = rd_u32(data + 0x34);                 // @0x449fe2

    // blob_size cross-check (loader trusts it; we assert for safety).
    size_t need = 0;
    if (checked_add_size(THREEDI_LW_KSA_HEADER_SIZE, (size_t)out->blob_size, &need) != 0) return -1;
    if (need > len) return -1;

    if (out->slot_count == 0) return 0;
    if (out->slot_count > THREEDI_LW_KSA_MAX_SLOTS) return -1;

    size_t slot_table_bytes = 0;
    if (checked_mul_size((size_t)out->slot_count, THREEDI_LW_KSA_SLOT_REC, &slot_table_bytes) != 0) return -1;
    size_t frame_data_off = 0;
    if (checked_add_size(THREEDI_LW_KSA_HEADER_SIZE, slot_table_bytes, &frame_data_off) != 0) return -1;
    if (frame_data_off > len) return -1;

    out->slots = (ThreediLwKsaSlot *)calloc(out->slot_count, sizeof(ThreediLwKsaSlot));
    if (!out->slots) {
        threedi_lw_ksa_free(out);
        return -1;
    }

    // First pass: read slot records, sum frames, per-frame bounds-check.
    const uint8_t *slot_table = data + THREEDI_LW_KSA_HEADER_SIZE;
    size_t cursor = frame_data_off;
    uint64_t total = 0;
    for (uint32_t s = 0; s < out->slot_count; ++s) {
        const uint8_t *rec = slot_table + (size_t)s * THREEDI_LW_KSA_SLOT_REC;
        uint32_t fc = rd_u32(rec + 0x00);            // @0x44a00e
        out->slots[s].frame_count = fc;
        out->slots[s].stored_slot = rd_u32(rec + 0x08);
        out->slots[s].loop_frame = rd_u32(rec + 0x0C);
        out->slots[s].frame_base = (uint32_t)total;

        size_t frame_bytes = 0;
        if (checked_mul_size((size_t)fc, THREEDI_LW_RUNTIME_FRAME_SIZE, &frame_bytes) != 0) {
            threedi_lw_ksa_free(out);
            return -1;
        }
        size_t next = 0;
        if (checked_add_size(cursor, frame_bytes, &next) != 0 || next > len) {
            threedi_lw_ksa_free(out);
            return -1;
        }
        cursor = next;
        total += fc;
        if (total > UINT32_MAX) {
            threedi_lw_ksa_free(out);
            return -1;
        }
    }
    out->total_frames = (uint32_t)total;

    if (out->total_frames) {
        out->frames = (ThreediLwAnimFrame *)calloc(out->total_frames, sizeof(ThreediLwAnimFrame));
        if (!out->frames) {
            threedi_lw_ksa_free(out);
            return -1;
        }
        const uint8_t *fp = data + frame_data_off;
        for (uint32_t i = 0; i < out->total_frames; ++i)
            decode_runtime_frame(fp + (size_t)i * THREEDI_LW_RUNTIME_FRAME_SIZE, &out->frames[i]);
        for (uint32_t s = 0; s < out->slot_count; ++s)
            out->slots[s].frames = out->frames + out->slots[s].frame_base;
    }

    return 0;
}

int threedi_lw_ksa_read(const char *path, ThreediLwKsa *out) {
    if (!path || !out) return -1;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (read_whole_file(path, &buf, &len) != 0) return -1;
    int rc = threedi_lw_ksa_parse(buf, len, out);
    free(buf);
    return rc;
}

// ============================================================================
// Text helpers (ACA / ANM)
// ============================================================================

// Copy one line (up to \r or \n) into buf (NUL-terminated, truncated to cap).
// Returns the start of the next line (past the \r\n / \n).
static const uint8_t *next_line(const uint8_t *p, const uint8_t *end, char *buf, size_t cap) {
    size_t n = 0;
    while (p < end && *p != '\n' && *p != '\r') {
        if (n + 1 < cap) buf[n++] = (char)*p;
        ++p;
    }
    buf[n] = '\0';
    // skip the line terminator (\r, \n, or \r\n)
    if (p < end && *p == '\r') ++p;
    if (p < end && *p == '\n') ++p;
    return p;
}

static void strip_line_comment(char *line) {
    for (char *c = line; c[0]; ++c) {
        if (c[0] == '/' && c[1] == '/') {
            c[0] = '\0';
            return;
        }
    }
}

static int tokenize(char *line, char *tok[], int max) {
    int n = 0;
    char *s = line;
    while (n < max) {
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '\0') break;
        tok[n++] = s;
        while (*s && *s != ' ' && *s != '\t') ++s;
        if (*s) *s++ = '\0';
    }
    return n;
}

// ============================================================================
// ACA: 'slot <slot_id> <filename> [loop_frame]' (LWAnim_ParseAcaLine @ 0x44AB30)
// ============================================================================

void threedi_lw_aca_init(ThreediLwAca *out) {
    if (out) memset(out, 0, sizeof(*out));
}

void threedi_lw_aca_free(ThreediLwAca *out) {
    if (!out) return;
    free(out->entries);
    memset(out, 0, sizeof(*out));
}

int threedi_lw_aca_parse(const uint8_t *data, size_t len, ThreediLwAca *out) {
    if (!data || !out) return -1;
    threedi_lw_aca_init(out);

    const uint8_t *p = data, *end = data + len;
    size_t cap = 0;
    char buf[512];
    while (p < end) {
        p = next_line(p, end, buf, sizeof(buf));
        strip_line_comment(buf);
        char tmp[512];
        memcpy(tmp, buf, sizeof(tmp));
        char *tok[4];
        int nt = tokenize(tmp, tok, 4);
        if (nt < 3 || strcmp(tok[0], "slot") != 0) continue;

        if (out->entry_count >= cap) {
            size_t ncap = cap ? cap * 2 : 32;
            ThreediLwAcaEntry *ne = (ThreediLwAcaEntry *)realloc(out->entries, ncap * sizeof(*ne));
            if (!ne) {
                threedi_lw_aca_free(out);
                return -1;
            }
            out->entries = ne;
            cap = ncap;
        }
        ThreediLwAcaEntry *e = &out->entries[out->entry_count++];
        memset(e, 0, sizeof(*e));
        e->slot_id = (uint32_t)atoi(tok[1]);
        copy_name(e->saf_name, sizeof(e->saf_name), (const uint8_t *)tok[2], strlen(tok[2]));
        if (nt >= 4) {
            e->loop_frame = atoi(tok[3]);
            e->has_loop = 1;
        } else {
            e->loop_frame = -1;
            e->has_loop = 0;
        }
    }
    return 0;
}

int threedi_lw_aca_read(const char *path, ThreediLwAca *out) {
    if (!path || !out) return -1;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (read_whole_file(path, &buf, &len) != 0) return -1;
    int rc = threedi_lw_aca_parse(buf, len, out);
    free(buf);
    return rc;
}

// ============================================================================
// ANM: '<name> <slot> [velocity] [override]' (LWAnim_ParseAnmLine @ 0x44A6D0)
// ============================================================================

void threedi_lw_anm_init(ThreediLwAnm *out) {
    if (out) memset(out, 0, sizeof(*out));
}

void threedi_lw_anm_free(ThreediLwAnm *out) {
    if (!out) return;
    free(out->entries);
    memset(out, 0, sizeof(*out));
}

int threedi_lw_anm_parse(const uint8_t *data, size_t len, ThreediLwAnm *out) {
    if (!data || !out) return -1;
    threedi_lw_anm_init(out);

    const uint8_t *p = data, *end = data + len;
    size_t cap = 0;
    char buf[512];
    while (p < end) {
        p = next_line(p, end, buf, sizeof(buf));
        strip_line_comment(buf);
        char tmp[512];
        memcpy(tmp, buf, sizeof(tmp));
        char *tok[4];
        int nt = tokenize(tmp, tok, 4);
        if (nt < 2) continue;

        if (out->entry_count >= cap) {
            size_t ncap = cap ? cap * 2 : 64;
            ThreediLwAnmEntry *ne = (ThreediLwAnmEntry *)realloc(out->entries, ncap * sizeof(*ne));
            if (!ne) {
                threedi_lw_anm_free(out);
                return -1;
            }
            out->entries = ne;
            cap = ncap;
        }
        ThreediLwAnmEntry *e = &out->entries[out->entry_count++];
        memset(e, 0, sizeof(*e));
        copy_name(e->name, sizeof(e->name), (const uint8_t *)tok[0], strlen(tok[0]));
        e->slot = (uint32_t)atoi(tok[1]);
        e->override_val = -1;
        if (nt >= 3) {
            e->velocity = (float)atof(tok[2]);
            e->has_velocity = 1;
        }
        if (nt >= 4) {
            e->override_val = atoi(tok[3]);
            e->has_override = 1;
        }
    }
    return 0;
}

int threedi_lw_anm_read(const char *path, ThreediLwAnm *out) {
    if (!path || !out) return -1;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (read_whole_file(path, &buf, &len) != 0) return -1;
    int rc = threedi_lw_anm_parse(buf, len, out);
    free(buf);
    return rc;
}

// ============================================================================
// Skeleton rest data (reads a model's LOD sub-objects for the sampler)
// ============================================================================

int threedi_lw_anim_read_skeleton(const char *path, uint32_t lod,
                                  ThreediLwSubObject **out_subs,
                                  uint32_t *out_count, uint32_t *out_flags) {
    if (!path || !out_subs || !out_count || !out_flags) return -1;
    *out_subs = NULL;
    *out_count = 0;
    *out_flags = 0;

    ThreediLwFile f;
    threedi_lw_init(&f);
    if (threedi_lw_read(path, &f) != 0) {
        threedi_lw_free(&f);
        return -1;
    }
    int rc = -1;
    if (lod < f.lod_count && f.lods) {
        const ThreediLwLod *l = &f.lods[lod];
        *out_flags = l->flags;
        uint32_t n = l->subobject_count;
        if (n > 0 && l->subobjects) {
            ThreediLwSubObject *arr = (ThreediLwSubObject *)malloc((size_t)n * sizeof(*arr));
            if (arr) {
                memcpy(arr, l->subobjects, (size_t)n * sizeof(*arr));
                *out_subs = arr;
                *out_count = n;
                rc = 0;
            }
        } else {
            rc = 0; // valid model with no sub-objects in this LOD
        }
    }
    threedi_lw_free(&f);
    return rc;
}

void threedi_lw_anim_free_skeleton(ThreediLwSubObject *subs) { free(subs); }

// ============================================================================
// Pose sampler (LWAnim_PoseSkeleton @ 0x4A0C00, gameplay modifiers SKIPPED)
// ============================================================================

// Angle byte -> radians: byte << 24 over a 2^32 circle => byte * (2*PI/256).
#define LW_TWO_PI 6.28318530717958648f
static float lw_bam(uint8_t b) { return (float)b * (LW_TWO_PI / 256.0f); }

// out(3x3) = a * b, row-major.
static void mat3_mul(const float a[3][3], const float b[3][3], float out[3][3]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}

// out = R * p + t.
static void world_point(const float R[3][3], const float t[3], const float p[3], float out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = R[r][0] * p[0] + R[r][1] * p[1] + R[r][2] * p[2] + t[r];
}

void threedi_lw_anim_pose_init(ThreediLwAnimPose *out) {
    if (out) memset(out, 0, sizeof(*out));
}

void threedi_lw_anim_pose_free(ThreediLwAnimPose *out) {
    if (!out) return;
    free(out->bones);
    memset(out, 0, sizeof(*out));
}

int threedi_lw_anim_sample(const ThreediLwAnimFrame *frame,
                           const ThreediLwSubObject *subobjects,
                           uint32_t subobject_cnt,
                           float pos_scale,
                           ThreediLwAnimPose *out) {
    if (!frame || !subobjects || !out) return -1;
    if (subobject_cnt == 0 || subobject_cnt > THREEDI_LW_MAX_PARTS) return -1;
    threedi_lw_anim_pose_init(out);

    float wR[THREEDI_LW_MAX_PARTS][3][3]; // world rotation per bone
    float wt[THREEDI_LW_MAX_PARTS][3];    // world position per bone

    for (uint32_t i = 0; i < subobject_cnt; ++i) {
        const ThreediLwSubObject *so = &subobjects[i];

        // World rotation: ABSOLUTE, R = Rz(b0) * Ry(b1) * Rx(b2).
        float ax = lw_bam(frame->parts[i].b2); // Rx (innermost)
        float ay = lw_bam(frame->parts[i].b1); // Ry
        float az = lw_bam(frame->parts[i].b0); // Rz (outermost)
        float cx = cosf(ax), sx = sinf(ax);
        float cy = cosf(ay), sy = sinf(ay);
        float cz = cosf(az), sz = sinf(az);
        float Rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
        float Ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
        float Rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
        float Rzy[3][3];
        mat3_mul(Rz, Ry, Rzy);
        mat3_mul(Rzy, Rx, wR[i]);

        // World position: bone 0 (root) at origin; children chain through the
        // parent rest offset (subobj.pos = loader's rel = pos - parent.pos).
        int has_parent = (i != 0 && so->parent >= 0 && (uint32_t)so->parent < subobject_cnt);
        if (!has_parent) {
            wt[i][0] = wt[i][1] = wt[i][2] = 0.0f;
        } else {
            // Rest offset = this sub-object's pos minus its parent's pos (the loader's
            // rel @ +0x30 = pos - parent.pos; ThreediLwSubObject stores the raw abs pos @+0x3C).
            uint32_t pi = (uint32_t)so->parent;
            float o[3] = {(float)(so->pos[0] - subobjects[pi].pos[0]) * pos_scale,
                          (float)(so->pos[1] - subobjects[pi].pos[1]) * pos_scale,
                          (float)(so->pos[2] - subobjects[pi].pos[2]) * pos_scale};
            world_point(wR[pi], wt[pi], o, wt[i]);
        }
    }

    out->bones = (ThreediLwBonePose *)calloc(subobject_cnt, sizeof(ThreediLwBonePose));
    if (!out->bones) return -1;
    out->bone_count = subobject_cnt;

    // local = parent.world^-1 * bone.world  (root: local == world).
    for (uint32_t i = 0; i < subobject_cnt; ++i) {
        const ThreediLwSubObject *so = &subobjects[i];
        float LR[3][3], lt[3];
        int has_parent = (i != 0 && so->parent >= 0 && (uint32_t)so->parent < subobject_cnt);
        if (!has_parent) {
            memcpy(LR, wR[i], sizeof(LR));
            lt[0] = wt[i][0];
            lt[1] = wt[i][1];
            lt[2] = wt[i][2];
        } else {
            uint32_t p = (uint32_t)so->parent;
            // parent.world^-1: rotation transpose (orthonormal), t' = -R^T * t.
            float PRt[3][3];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) PRt[r][c] = wR[p][c][r];
            float zero[3] = {0, 0, 0};
            float ptw[3];
            world_point(PRt, zero, wt[p], ptw); // R^T * parent_t
            float invPt[3] = {-ptw[0], -ptw[1], -ptw[2]};
            mat3_mul(PRt, wR[i], LR);
            world_point(PRt, invPt, wt[i], lt);
        }
        for (int r = 0; r < 3; ++r) {
            out->bones[i].local[r][0] = LR[r][0];
            out->bones[i].local[r][1] = LR[r][1];
            out->bones[i].local[r][2] = LR[r][2];
            out->bones[i].local[r][3] = lt[r];
        }
    }
    return 0;
}
