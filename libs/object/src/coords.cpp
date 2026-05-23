#include "object/coords.h"

extern "C" void object_render_space(float x, float y, float z, float out[3]) {
    out[0] = -x;
    out[1] = -z;
    out[2] =  y;
}

extern "C" void object_bone_space(float x, float y, float z, float out[3]) {
    out[0] =  x;
    out[1] = -z;
    out[2] =  y;
}
