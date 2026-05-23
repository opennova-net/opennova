// libs/object/src/nlascexp_quirks.cpp
#include "object/nlascexp_options.h"
#include "object/nlascexp_quirks.h"
#include <cstring>

extern "C" void object_nlascexp_options_init_defaults(NlascexpOptions* opts) {
    if (!opts) return;
    opts->include_collisions = 1;
    opts->include_occlusion = 1;
    opts->include_lights = 1;
    opts->scaleby = 1.0f;
    opts->float_precision = 4;
}

extern "C" int object_node_tm_has_negative_handedness(const float tm[4][3]) {
    // cross(row0, row1)
    const float cx = tm[0][1]*tm[1][2] - tm[0][2]*tm[1][1];
    const float cy = tm[0][2]*tm[1][0] - tm[0][0]*tm[1][2];
    const float cz = tm[0][0]*tm[1][1] - tm[0][1]*tm[1][0];
    // dot(row2, cross)
    const float d = tm[2][0]*cx + tm[2][1]*cy + tm[2][2]*cz;
    return (d < 0.0f) ? 1 : 0;
}

extern "C" size_t object_fixup_name(const char* name, char* out, size_t out_size) {
    if (!name || !out || out_size == 0) return 0;
    size_t len = std::strlen(name);
    // Trim trailing whitespace.
    while (len > 0 && (name[len-1] == ' ' || name[len-1] == '\t' || name[len-1] == '\n' || name[len-1] == '\r')) {
        --len;
    }
    size_t copy = (len < out_size - 1) ? len : (out_size - 1);
    std::memcpy(out, name, copy);
    out[copy] = '\0';
    return copy;
}

extern "C" size_t object_decode_ir_string(const char* raw, size_t raw_len, char* out, size_t out_size) {
    if (!raw || !out || out_size == 0) return 0;
    // Find first NUL within raw_len.
    size_t end = 0;
    while (end < raw_len && raw[end] != '\0') ++end;
    size_t copy = (end < out_size - 1) ? end : (out_size - 1);
    std::memcpy(out, raw, copy);
    out[copy] = '\0';
    return copy;
}
