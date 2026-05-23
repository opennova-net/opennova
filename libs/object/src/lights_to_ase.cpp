// libs/object/src/lights_to_ase.cpp
//
// Port of pyopennova/ase_from_3di3.py:
//   _lights          (lines 650-677) — walks ir.lights[], builds ase_Light
//   _populate_light  (lines 800-818) — folded inline; name/type/pos/color/atten/falloff/row2
//   _light_rgb       (line 841)      — color_start[B,G,R] -> (R/255, G/255, B/255)
//
// IR field mapping vs Python properties on ThreediLight:
//   ir->lights[i].atten_start         = light.attenuation_start
//   ir->lights[i].atten_end           = light.attenuation_end
//   ir->lights[i].subobj_index        = light.part_index
//   ir->lights[i].falloff_byte        = light.falloff (cast to float)
//   (ir->lights[i].flags >> 3) & 1   = light.light_type
//   ir->lights[i].color_start[0..2]  = B,G,R bytes (0 = B, 1 = G, 2 = R)
//   ir->lights[i].rotation[0..2]     = first 3 floats of rotation[4]
//   ir->lods[0].render_objects[i].abs[] = lod0.parts[i].abs_position
//
// _light_rgb: color_start is packed B,G,R,unused.  Since values are bytes
//   (>1.0 when treated as float), color_rgb() divides by 255 and swaps
//   indices: R = color_start[2]/255, G = color_start[1]/255, B = color_start[0]/255.
//
// _lights name: "LP{light_idx+1:02d}" where light_idx = subobj_index if >= 0
//   else i.

#include "object/lights_to_ase.h"

#include "object/coords.h"    // object_render_space

#include "ase/types.h"        // ase_Light

#include "threedi/threedi_3di3.h"  // Threedi3di3, ThreediLight, ThreediLod

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static inline float zero_tiny_parent_offset(float value)
{
    return std::fabs(value) <= 1.0e-7f ? 0.0f : value;
}

extern "C" int object_emit_lights(
    const Threedi3di3* ir,
    ase_Light* out_lights,
    int out_capacity)
{
    if (!ir || !out_lights || out_capacity <= 0)
        return 0;

    const int n = (int)ir->light_count;
    if (n <= 0)
        return 0;

    // Pre-build part abs positions for lod0 (Python lines 651-658).
    // part_abs[i] = render_space(lod0.parts[i].abs_position).
    const ThreediRenderObject* parts = nullptr;
    int part_count = 0;
    if (ir->lod_count > 0) {
        parts = ir->lods[0].render_objects;
        part_count = (int)ir->lods[0].render_object_count;
    }

    int emitted = 0;
    for (int i = 0; i < n && emitted < out_capacity; ++i) {
        const ThreediLight* light = &ir->lights[i];
        ase_Light* al = &out_lights[emitted];

        // light_idx: subobj_index if >= 0, else i (Python line 662).
        int light_idx = ((int)light->subobj_index >= 0) ? (int)light->subobj_index : i;

        // name: "LP{light_idx+1:02d}"
        std::snprintf(al->name, sizeof(al->name), "LP%02d", light_idx + 1);

        // type: (flags >> 3) & 1 (Python property light_type line 315-316)
        al->type = ((int)light->flags >> 3) & 1;

        // pos: render_space(light.offset) + part_abs[part_index] if valid
        // (Python lines 663-665).
        float pos[3];
        object_render_space(light->offset[0], light->offset[1], light->offset[2], pos);

        int part_idx = (int)light->subobj_index;
        if (part_idx >= 0 && part_idx < part_count && parts != nullptr) {
            float part_pos[3];
            object_render_space(
                parts[part_idx].abs[0],
                parts[part_idx].abs[1],
                parts[part_idx].abs[2],
                part_pos);
            pos[0] += zero_tiny_parent_offset(part_pos[0]);
            pos[1] += zero_tiny_parent_offset(part_pos[1]);
            pos[2] += zero_tiny_parent_offset(part_pos[2]);
        }
        al->pos[0] = pos[0];
        al->pos[1] = pos[1];
        al->pos[2] = pos[2];

        // color: _light_rgb -> color_rgb(color_start)
        // color_start is packed [B, G, R, unused] uint8 bytes.
        // color_rgb divides by 255 and returns (R, G, B) (Python lines 83-86).
        al->color[0] = (float)light->color_start[2] / 255.0f; // R
        al->color[1] = (float)light->color_start[1] / 255.0f; // G
        al->color[2] = (float)light->color_start[0] / 255.0f; // B

        // intensity: not in IR (Python _populate_light sets 0.0 from ir path).
        al->intensity = 0.0f;

        // atten_start: light.attenuation_start (= ir->atten_start)
        al->atten_start = light->atten_start;

        // atten_end: light.attenuation_end (= ir->atten_end)
        al->atten_end = light->atten_end;

        // near_atten_start / near_atten_end: not in IR (default 0.0).
        al->near_atten_start = 0.0f;
        al->near_atten_end   = 0.0f;

        // hotspot: not in IR (default 0.0).
        al->hotspot = 0.0f;

        // falloff: light.falloff = float(falloff_byte) (Python line 311-312)
        al->falloff = (float)light->falloff_byte;

        // tm_row2: render_space of rotation[0..2], then negate.
        // Python lines 666-675:
        //   row2 = coords.render_space(light.rotation)  (uses first 3 of float[4])
        //   "tm_row2": (-row2[0], -row2[1], -row2[2])
        float row2[3];
        object_render_space(
            light->rotation[0],
            light->rotation[1],
            light->rotation[2],
            row2);
        al->tm_row2[0] = -row2[0];
        al->tm_row2[1] = -row2[1];
        al->tm_row2[2] = -row2[2];

        ++emitted;
    }

    return emitted;
}
