// Internal shared helpers between tdp.cpp (3DP reader/writer + 3DA writer)
// and tdp_read_3da.cpp (3DA reader).  Not part of the public API; do not
// include from outside libs/tdp/src/.
//
// All functions defined here are `inline` so they can be #included into
// multiple translation units without ODR violations or forcing a separate
// .cpp.  Keep this header self-contained.

#ifndef TDP_INTERNAL_H
#define TDP_INTERNAL_H

#include "tdp/tdp.h"
#include "tdp/tdp_material.h"
#include "threedi/threedi_material_class.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// String / parsing helpers
// ---------------------------------------------------------------------------

static inline void tdp_copy_str(char *dst, std::size_t dst_size, const char *src) {
    if (!src || !dst || dst_size == 0) return;
    std::size_t len = std::strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

static inline bool tdp_iequals(const char *a, const char *b) {
    for (;; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b)))
            return false;
        if (*a == '\0') return true;
    }
}

static inline int tdp_to_int(const char *s) {
    if (!s || !*s) return 0;
    return static_cast<int>(std::strtol(s, nullptr, 10));
}

static inline float tdp_to_float(const char *s) {
    if (!s || !*s) return 0.0f;
    return std::strtof(s, nullptr);
}

static inline int tdp_clamp_255(float v) {
    int r = static_cast<int>(v * 255.0f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return r;
}

static inline float tdp_byte_to_unit(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return static_cast<float>(v) / 255.0f;
}

// ---------------------------------------------------------------------------
// Surface-type ↔ ptype mapping
// ---------------------------------------------------------------------------

static inline int tdp_surface_type_to_ptype(uint8_t st) {
    switch (st) {
        case 0x0E: return 0;  // Metal
        case 0x0D: return 1;  // Wood
        case 0x0C: return 2;  // Stone
        case 0x11: return 3;  // Foliage
        case 0x12: return 4;  // Hard Metal
        case 0x10: return 5;  // Cloth
        case 0x0F: return 6;  // Glass
        case 0x07: return 7;  // Water
        case 0x13: return 8;  // Flesh
        case 0x01: return 9;  // Dirt
        default:   return 9;
    }
}

static inline uint8_t tdp_ptype_to_surface_type(int ptype) {
    switch (ptype) {
        case 0: return 0x0E;  // Metal
        case 1: return 0x0D;  // Wood
        case 2: return 0x0C;  // Stone
        case 3: return 0x11;  // Foliage
        case 4: return 0x12;  // Hard Metal
        case 5: return 0x10;  // Cloth
        case 6: return 0x0F;  // Glass
        case 7: return 0x07;  // Water
        case 8: return 0x13;  // Flesh
        case 9: return 0x01;  // Dirt
        default: return 0x01;
    }
}

// ---------------------------------------------------------------------------
// Control register name <-> index resolution
// ---------------------------------------------------------------------------

static inline int32_t tdp_ctrlreg_lookup(const TdpProject *proj, const char *name) {
    if (!proj || !name || !name[0] || !proj->ctrl_regs) return -1;
    for (std::size_t i = 0; i < proj->ctrl_reg_count; ++i) {
        if (std::strcmp(proj->ctrl_regs[i].name, name) == 0)
            return static_cast<int32_t>(i);
    }
    return -1;
}

static inline int32_t tdp_ctrlreg_intern(TdpProject *proj, const char *name) {
    if (!proj || !name || !name[0]) return -1;
    int32_t idx = tdp_ctrlreg_lookup(proj, name);
    if (idx >= 0) return idx;
    std::size_t new_count = proj->ctrl_reg_count + 1;
    auto *expanded = static_cast<TdpControlRegister *>(
        std::realloc(proj->ctrl_regs, new_count * sizeof(TdpControlRegister)));
    if (!expanded) return -1;
    proj->ctrl_regs = expanded;
    std::memset(&proj->ctrl_regs[proj->ctrl_reg_count], 0,
                sizeof(TdpControlRegister));
    tdp_copy_str(proj->ctrl_regs[proj->ctrl_reg_count].name,
                 sizeof(proj->ctrl_regs[proj->ctrl_reg_count].name), name);
    proj->ctrl_reg_count = new_count;
    return static_cast<int32_t>(proj->ctrl_reg_count - 1);
}

static inline const char *tdp_ctrlreg_name(const TdpProject *proj, int32_t idx) {
    if (!proj || !proj->ctrl_regs || idx < 0 ||
        (std::size_t)idx >= proj->ctrl_reg_count)
        return "";
    return proj->ctrl_regs[idx].name;
}

// ---------------------------------------------------------------------------
// Texture slot accessors — model's textures[] array is the source of truth
// ---------------------------------------------------------------------------

static inline const TdpMaterialTexture *
tdp_material_find_static_tex(const TdpMaterial *m, uint8_t slot) {
    for (uint32_t i = 0; i < m->texture_count && i < TDP_MAX_MATERIAL_TEXTURES; ++i) {
        const TdpMaterialTexture *t = &m->textures[i];
        if (t->slot == slot && !(t->flags & 0x01u /*ANIMATED*/))
            return t;
    }
    return nullptr;
}

static inline TdpMaterialTexture *
tdp_material_find_or_alloc_tex(TdpMaterial *m, uint8_t slot, uint8_t frame,
                          bool animated) {
    for (uint32_t i = 0; i < m->texture_count && i < TDP_MAX_MATERIAL_TEXTURES; ++i) {
        TdpMaterialTexture *t = &m->textures[i];
        bool t_anim = (t->flags & 0x01u) != 0;
        if (t->slot == slot && t->frame == frame && t_anim == animated)
            return t;
    }
    if (m->texture_count >= TDP_MAX_MATERIAL_TEXTURES) return nullptr;
    TdpMaterialTexture *t = &m->textures[m->texture_count++];
    std::memset(t, 0, sizeof(*t));
    t->slot = slot;
    t->frame = frame;
    if (animated) t->flags |= 0x01u;
    return t;
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

#define TDP_MAX_TOKENS 30

struct TdpTokens {
    char buf[1024];
    const char *toks[TDP_MAX_TOKENS];
    int count;
};

static inline void tdp_tokenize(TdpTokens *t, const char *line) {
    t->count = 0;
    if (!line || !*line) return;

    std::size_t n = std::strlen(line);
    if (n >= sizeof(t->buf)) n = sizeof(t->buf) - 1;
    for (std::size_t i = 0; i < n; ++i) {
        char c = line[i];
        if (c == '\r') c = '\0';
        t->buf[i] = c;
    }
    std::memset(t->buf + n, 0, sizeof(t->buf) - n);

    bool in_quote = false;
    for (std::size_t i = 0; t->buf[i]; ++i) {
        if (!in_quote && t->buf[i] == ';') { t->buf[i] = '\0'; break; }
        if (!in_quote && t->buf[i] == '/' && t->buf[i + 1] == '/') { t->buf[i] = '\0'; break; }
        if (t->buf[i] == '"') {
            in_quote = !in_quote;
            t->buf[i] = '\0';
        } else if (!in_quote && (t->buf[i] == ' ' || t->buf[i] == '\t' || t->buf[i] == ',')) {
            t->buf[i] = '\0';
        }
    }

    for (std::size_t i = 0; i < sizeof(t->buf) && t->count < TDP_MAX_TOKENS;) {
        while (i < sizeof(t->buf) && t->buf[i] == '\0') ++i;
        if (i >= sizeof(t->buf) || !t->buf[i]) break;
        t->toks[t->count++] = &t->buf[i];
        while (i < sizeof(t->buf) && t->buf[i]) ++i;
    }
}

static inline const char *tdp_tok_at(const TdpTokens *t, int idx) {
    if (idx < 0 || idx >= t->count) return "";
    return t->toks[idx];
}

// Extract quoted string from raw line for name/path fields.
static inline void tdp_extract_quoted(const char *line, char *dst, std::size_t dst_size) {
    const char *q0 = std::strchr(line, '"');
    if (!q0) { dst[0] = '\0'; return; }
    const char *q1 = std::strchr(q0 + 1, '"');
    if (!q1 || q1 < q0 + 1) { dst[0] = '\0'; return; }
    std::size_t len = static_cast<std::size_t>(q1 - q0 - 1);
    if (len >= dst_size) len = dst_size - 1;
    std::memcpy(dst, q0 + 1, len);
    dst[len] = '\0';
}

// ---------------------------------------------------------------------------
// Material array growth
// ---------------------------------------------------------------------------

static inline TdpMaterial *tdp_ensure_material(TdpProject *proj, std::size_t idx) {
    if (idx >= proj->material_count) {
        std::size_t new_count = idx + 1;
        proj->materials = static_cast<TdpMaterial *>(
            std::realloc(proj->materials, new_count * sizeof(TdpMaterial)));
        for (std::size_t i = proj->material_count; i < new_count; ++i)
            std::memset(&proj->materials[i], 0, sizeof(TdpMaterial));
        proj->material_count = new_count;
    }
    return &proj->materials[idx];
}

#endif  // TDP_INTERNAL_H
