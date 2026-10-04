// Canonical Joint Operations global control-register catalog.
//
// A model's CTRL records name entries in this global table.  The ordinal is
// the index used by retail's two-dword runtime control-register storage.
// Retail's 96 come first, in its order; OpenNova's own registers follow them
// (ADR 0047 decision 17), so the bus holds both and retail's ordinals never
// move.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace opennova::threedi {

enum {
    // Ordinal zero is LOD_FRAC, so a miss needs a distinct result.
    THREEDI_CTRL_REGISTER_NOT_FOUND = -1,

    // Every populated retail descriptor, in global-bus order. Keep runtime
    // callers on these semantic ordinals instead of model-local CTRL order.
    // [orig: global control-register descriptor table @ 0x83DCE8]
    THREEDI_CTRL_LOD_FRAC = 0,
    THREEDI_CTRL_LOD_FADE_IN = 1,
    THREEDI_CTRL_LOD_FADE_OUT = 2,
    THREEDI_CTRL_FLICKER = 3,
    THREEDI_CTRL_SWING = 4,
    THREEDI_CTRL_TALK = 5,
    THREEDI_CTRL_DEATH = 6,
    THREEDI_CTRL_NVG_FLIP = 7,
    THREEDI_CTRL_TEAMSWING = 8,
    THREEDI_CTRL_HUD_HEALTH = 9,
    THREEDI_CTRL_HUD_MANA = 10,
    THREEDI_CTRL_HUD_COMPASS = 11,
    THREEDI_CTRL_WPN_TRIGGER = 12,
    THREEDI_CTRL_WPN_HAMMER = 13,
    THREEDI_CTRL_PARA = 14,
    THREEDI_CTRL_PARA_O = 15,
    THREEDI_CTRL_DOOR_00 = 16,
    THREEDI_CTRL_DOOR_01 = 17,
    THREEDI_CTRL_DOOR_02 = 18,
    THREEDI_CTRL_DOOR_03 = 19,
    THREEDI_CTRL_DOOR_04 = 20,
    THREEDI_CTRL_DOOR_05 = 21,
    THREEDI_CTRL_DOOR_06 = 22,
    THREEDI_CTRL_DOOR_07 = 23,
    THREEDI_CTRL_DOOR_08 = 24,
    THREEDI_CTRL_DOOR_09 = 25,
    THREEDI_CTRL_DOOR_10 = 26,
    THREEDI_CTRL_DOOR_11 = 27,
    THREEDI_CTRL_DOOR_12 = 28,
    THREEDI_CTRL_DOOR_13 = 29,
    THREEDI_CTRL_DOOR_14 = 30,
    THREEDI_CTRL_DOOR_15 = 31,
    THREEDI_CTRL_UPL_INTENSITY = 32,
    THREEDI_CTRL_LIGHTSWITCH0 = 33,
    THREEDI_CTRL_LIGHTSWITCH1 = 34,
    THREEDI_CTRL_LIGHTSWITCH2 = 35,
    THREEDI_CTRL_LIGHTSWITCH3 = 36,
    THREEDI_CTRL_PARTICLE_ALPHA = 37,
    THREEDI_CTRL_PARTICLE_RGB = 38,
    THREEDI_CTRL_HELO_REAR_GEAR = 39,
    THREEDI_CTRL_HELO_GEARDOORS = 40,
    THREEDI_CTRL_HELO_GEAR = 41,
    THREEDI_CTRL_HELO_GEARB = 42,
    THREEDI_CTRL_HELO_BAYDOORS = 43,
    THREEDI_CTRL_HELO_PCANOPY = 44,
    THREEDI_CTRL_HELO_CPCANOPY = 45,
    THREEDI_CTRL_HELO_ROTOR = 46,
    THREEDI_CTRL_HELO_TAILROTOR = 47,
    THREEDI_CTRL_HELO_PILOTYAW = 48,
    THREEDI_CTRL_HELO_PILOTPITCH = 49,
    THREEDI_CTRL_HELO_CPILOTYAW = 50,
    THREEDI_CTRL_HELO_CPILOTPITCH = 51,
    THREEDI_CTRL_HELO_GUNYAW = 52,
    THREEDI_CTRL_HELO_GUNPITCH = 53,
    THREEDI_CTRL_HEAT_GLOW = 54,
    THREEDI_CTRL_EWEAP_GUNYAW = 55,
    THREEDI_CTRL_EWEAP_GUNPITCH = 56,
    THREEDI_CTRL_WEAP_SPIN = 57,
    THREEDI_CTRL_TRACER_SCALE = 58,
    THREEDI_CTRL_TRACER_WIDTH = 59,
    THREEDI_CTRL_VEHICLE_WHEELS = 60,
    THREEDI_CTRL_VEHICLE_STEERING = 61,
    THREEDI_CTRL_VEHICLE_SPEED = 62,
    THREEDI_CTRL_VEHICLE_GUNYAW = 63,
    THREEDI_CTRL_VEHICLE_GUNPITCH = 64,
    THREEDI_CTRL_OBJECT_DESTROY = 65,
    THREEDI_CTRL_OBJECT_DESTROY01 = 66,
    THREEDI_CTRL_OBJECT_DESTROY02 = 67,
    THREEDI_CTRL_OBJECT_DESTROY03 = 68,
    THREEDI_CTRL_OBJECT_DESTROY04 = 69,
    THREEDI_CTRL_OBJECT_DESTROY05 = 70,
    THREEDI_CTRL_VEHICLE_SPECIAL1 = 71,
    THREEDI_CTRL_VEHICLE_SPECIAL2 = 72,
    THREEDI_CTRL_VEHICLE_TIRE00 = 73,
    THREEDI_CTRL_VEHICLE_TIRE01 = 74,
    THREEDI_CTRL_VEHICLE_TIRE02 = 75,
    THREEDI_CTRL_VEHICLE_TIRE03 = 76,
    THREEDI_CTRL_VEHICLE_TIRE04 = 77,
    THREEDI_CTRL_VEHICLE_TIRE05 = 78,
    THREEDI_CTRL_VEHICLE_TIRE06 = 79,
    THREEDI_CTRL_VEHICLE_TIRE07 = 80,
    THREEDI_CTRL_VEHICLE_TIRE08 = 81,
    THREEDI_CTRL_VEHICLE_TIRE09 = 82,
    THREEDI_CTRL_VEHICLE_TIRE10 = 83,
    THREEDI_CTRL_VEHICLE_TIRE11 = 84,
    THREEDI_CTRL_VEHICLE_TIRE12 = 85,
    THREEDI_CTRL_VEHICLE_TIRE13 = 86,
    THREEDI_CTRL_VEHICLE_WHEELS00 = 87,
    THREEDI_CTRL_VEHICLE_WHEELS01 = 88,
    THREEDI_CTRL_VEHICLE_WHEELS02 = 89,
    THREEDI_CTRL_VEHICLE_WHEELS03 = 90,
    THREEDI_CTRL_LFP_CAMPPERCENT = 91,
    THREEDI_CTRL_TEX_TEAM = 92,
    THREEDI_CTRL_TEX_CAMO1 = 93,
    THREEDI_CTRL_TEX_CAMO2 = 94,
    THREEDI_CTRL_TEX_CAMO3 = 95,
    // Retail's table: every populated descriptor, the next one zero.
    THREEDI_CTRL_RETAIL_REGISTER_COUNT = 96,

    // OpenNova's registers past retail's 96 (ADR 0047 decision 17; a ratified
    // divergence, docs/threedi/3di-gp-format-re.md D-3DI-7): names a model WE
    // author may carry. Retail's loader resolves an unknown name to slot 0,
    // which nothing writes, so the same model in the retail executable holds
    // every such track at its start; author the full state there.
    // The magazine's spent share, 16.16: (capacity - rounds in the clip) /
    // capacity, 0 full and 0x10000 empty.
    THREEDI_CTRL_WPN_SPENT = 96,
    // Round k from the top of the magazine: 0x10000 once it is spent (the clip
    // holds fewer than k rounds), else 0. A stepped register for hiding one
    // round with a scale or translation track.
    THREEDI_CTRL_WPN_ROUND_1 = 97,
    THREEDI_CTRL_WPN_ROUND_2 = 98,
    THREEDI_CTRL_WPN_ROUND_3 = 99,
    THREEDI_CTRL_WPN_ROUND_4 = 100,
    THREEDI_CTRL_WPN_ROUND_5 = 101,
    THREEDI_CTRL_WPN_ROUND_6 = 102,
    THREEDI_CTRL_WPN_ROUND_7 = 103,
    THREEDI_CTRL_WPN_ROUND_8 = 104,
    THREEDI_CTRL_WPN_ROUND_COUNT = 8,

    // The bus: retail's registers and OpenNova's.
    THREEDI_CTRL_REGISTER_COUNT = 105
};

static_assert(THREEDI_CTRL_WPN_ROUND_8 - THREEDI_CTRL_WPN_ROUND_1 + 1 == THREEDI_CTRL_WPN_ROUND_COUNT,
              "the round registers are consecutive");
static_assert(THREEDI_CTRL_WPN_SPENT == THREEDI_CTRL_RETAIL_REGISTER_COUNT,
              "OpenNova's registers start right past retail's table");

// Return the canonical spelling for ordinal, or NULL when ordinal is outside
// [0, THREEDI_CTRL_REGISTER_COUNT).  The returned string has static lifetime.
const char *threedi_ctrl_register_name(size_t ordinal);

// True for an ordinal of retail's own table (below
// THREEDI_CTRL_RETAIL_REGISTER_COUNT); OpenNova's registers follow it.
bool threedi_ctrl_register_is_retail(size_t ordinal);

// Resolve a canonical name using retail's ASCII case-insensitive comparison.
// Return THREEDI_CTRL_REGISTER_NOT_FOUND for NULL, empty, or unknown names.
int threedi_ctrl_register_ordinal(const char *name);

// Resolve a model-authored CTRL name using the retail loader convention:
// unknown and empty inline names alias global ordinal zero (LOD_FRAC). An
// OpenNova register resolves to its own slot here, where retail's loader
// reads zero (D-3DI-7).
// NULL also returns zero as an OpenNova API safety extension; retail's loader
// always passes a non-NULL inline name buffer. This compatibility behavior is
// deliberately separate from the unambiguous lookup above.
uint8_t threedi_ctrl_register_loader_ordinal(const char *name);

} // namespace opennova::threedi
