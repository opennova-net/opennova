#include "threedi/threedi_ctrl_catalog.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct ExpectedRegister {
    const char *name;
    int named_ordinal;
};

const ExpectedRegister kExpectedRegisters[] = {
    {"LOD_FRAC", THREEDI_CTRL_LOD_FRAC},
    {"LOD_FADE_IN", THREEDI_CTRL_LOD_FADE_IN},
    {"LOD_FADE_OUT", THREEDI_CTRL_LOD_FADE_OUT},
    {"FLICKER", THREEDI_CTRL_FLICKER},
    {"SWING", THREEDI_CTRL_SWING},
    {"TALK", THREEDI_CTRL_TALK},
    {"DEATH", THREEDI_CTRL_DEATH},
    {"NVG_FLIP", THREEDI_CTRL_NVG_FLIP},
    {"TEAMSWING", THREEDI_CTRL_TEAMSWING},
    {"HUD_HEALTH", THREEDI_CTRL_HUD_HEALTH},
    {"HUD_MANA", THREEDI_CTRL_HUD_MANA},
    {"HUD_COMPASS", THREEDI_CTRL_HUD_COMPASS},
    {"WPN_TRIGGER", THREEDI_CTRL_WPN_TRIGGER},
    {"WPN_HAMMER", THREEDI_CTRL_WPN_HAMMER},
    {"PARA", THREEDI_CTRL_PARA},
    {"PARA_O", THREEDI_CTRL_PARA_O},
    {"DOOR_00", THREEDI_CTRL_DOOR_00},
    {"DOOR_01", THREEDI_CTRL_DOOR_01},
    {"DOOR_02", THREEDI_CTRL_DOOR_02},
    {"DOOR_03", THREEDI_CTRL_DOOR_03},
    {"DOOR_04", THREEDI_CTRL_DOOR_04},
    {"DOOR_05", THREEDI_CTRL_DOOR_05},
    {"DOOR_06", THREEDI_CTRL_DOOR_06},
    {"DOOR_07", THREEDI_CTRL_DOOR_07},
    {"DOOR_08", THREEDI_CTRL_DOOR_08},
    {"DOOR_09", THREEDI_CTRL_DOOR_09},
    {"DOOR_10", THREEDI_CTRL_DOOR_10},
    {"DOOR_11", THREEDI_CTRL_DOOR_11},
    {"DOOR_12", THREEDI_CTRL_DOOR_12},
    {"DOOR_13", THREEDI_CTRL_DOOR_13},
    {"DOOR_14", THREEDI_CTRL_DOOR_14},
    {"DOOR_15", THREEDI_CTRL_DOOR_15},
    {"UPL_INTENSITY", THREEDI_CTRL_UPL_INTENSITY},
    {"LIGHTSWITCH0", THREEDI_CTRL_LIGHTSWITCH0},
    {"LIGHTSWITCH1", THREEDI_CTRL_LIGHTSWITCH1},
    {"LIGHTSWITCH2", THREEDI_CTRL_LIGHTSWITCH2},
    {"LIGHTSWITCH3", THREEDI_CTRL_LIGHTSWITCH3},
    {"PARTICLE_ALPHA", THREEDI_CTRL_PARTICLE_ALPHA},
    {"PARTICLE_RGB", THREEDI_CTRL_PARTICLE_RGB},
    {"HELO_REAR_GEAR", THREEDI_CTRL_HELO_REAR_GEAR},
    {"HELO_GEARDOORS", THREEDI_CTRL_HELO_GEARDOORS},
    {"HELO_GEAR", THREEDI_CTRL_HELO_GEAR},
    {"HELO_GEARB", THREEDI_CTRL_HELO_GEARB},
    {"HELO_BAYDOORS", THREEDI_CTRL_HELO_BAYDOORS},
    {"HELO_PCANOPY", THREEDI_CTRL_HELO_PCANOPY},
    {"HELO_CPCANOPY", THREEDI_CTRL_HELO_CPCANOPY},
    {"HELO_ROTOR", THREEDI_CTRL_HELO_ROTOR},
    {"HELO_TAILROTOR", THREEDI_CTRL_HELO_TAILROTOR},
    {"HELO_PILOTYAW", THREEDI_CTRL_HELO_PILOTYAW},
    {"HELO_PILOTPITCH", THREEDI_CTRL_HELO_PILOTPITCH},
    {"HELO_CPILOTYAW", THREEDI_CTRL_HELO_CPILOTYAW},
    {"HELO_CPILOTPITCH", THREEDI_CTRL_HELO_CPILOTPITCH},
    {"HELO_GUNYAW", THREEDI_CTRL_HELO_GUNYAW},
    {"HELO_GUNPITCH", THREEDI_CTRL_HELO_GUNPITCH},
    {"HEAT_GLOW", THREEDI_CTRL_HEAT_GLOW},
    {"EWEAP_GUNYAW", THREEDI_CTRL_EWEAP_GUNYAW},
    {"EWEAP_GUNPITCH", THREEDI_CTRL_EWEAP_GUNPITCH},
    {"WEAP_SPIN", THREEDI_CTRL_WEAP_SPIN},
    {"TRACER_SCALE", THREEDI_CTRL_TRACER_SCALE},
    {"TRACER_WIDTH", THREEDI_CTRL_TRACER_WIDTH},
    {"VEHICLE_WHEELS", THREEDI_CTRL_VEHICLE_WHEELS},
    {"VEHICLE_STEERING", THREEDI_CTRL_VEHICLE_STEERING},
    {"VEHICLE_SPEED", THREEDI_CTRL_VEHICLE_SPEED},
    {"VEHICLE_GUNYAW", THREEDI_CTRL_VEHICLE_GUNYAW},
    {"VEHICLE_GUNPITCH", THREEDI_CTRL_VEHICLE_GUNPITCH},
    {"OBJECT_DESTROY", THREEDI_CTRL_OBJECT_DESTROY},
    {"OBJECT_DESTROY01", THREEDI_CTRL_OBJECT_DESTROY01},
    {"OBJECT_DESTROY02", THREEDI_CTRL_OBJECT_DESTROY02},
    {"OBJECT_DESTROY03", THREEDI_CTRL_OBJECT_DESTROY03},
    {"OBJECT_DESTROY04", THREEDI_CTRL_OBJECT_DESTROY04},
    {"OBJECT_DESTROY05", THREEDI_CTRL_OBJECT_DESTROY05},
    {"VEHICLE_SPECIAL1", THREEDI_CTRL_VEHICLE_SPECIAL1},
    {"VEHICLE_SPECIAL2", THREEDI_CTRL_VEHICLE_SPECIAL2},
    {"VEHICLE_TIRE00", THREEDI_CTRL_VEHICLE_TIRE00},
    {"VEHICLE_TIRE01", THREEDI_CTRL_VEHICLE_TIRE01},
    {"VEHICLE_TIRE02", THREEDI_CTRL_VEHICLE_TIRE02},
    {"VEHICLE_TIRE03", THREEDI_CTRL_VEHICLE_TIRE03},
    {"VEHICLE_TIRE04", THREEDI_CTRL_VEHICLE_TIRE04},
    {"VEHICLE_TIRE05", THREEDI_CTRL_VEHICLE_TIRE05},
    {"VEHICLE_TIRE06", THREEDI_CTRL_VEHICLE_TIRE06},
    {"VEHICLE_TIRE07", THREEDI_CTRL_VEHICLE_TIRE07},
    {"VEHICLE_TIRE08", THREEDI_CTRL_VEHICLE_TIRE08},
    {"VEHICLE_TIRE09", THREEDI_CTRL_VEHICLE_TIRE09},
    {"VEHICLE_TIRE10", THREEDI_CTRL_VEHICLE_TIRE10},
    {"VEHICLE_TIRE11", THREEDI_CTRL_VEHICLE_TIRE11},
    {"VEHICLE_TIRE12", THREEDI_CTRL_VEHICLE_TIRE12},
    {"VEHICLE_TIRE13", THREEDI_CTRL_VEHICLE_TIRE13},
    {"VEHICLE_WHEELS00", THREEDI_CTRL_VEHICLE_WHEELS00},
    {"VEHICLE_WHEELS01", THREEDI_CTRL_VEHICLE_WHEELS01},
    {"VEHICLE_WHEELS02", THREEDI_CTRL_VEHICLE_WHEELS02},
    {"VEHICLE_WHEELS03", THREEDI_CTRL_VEHICLE_WHEELS03},
    {"LFP_CAMPPERCENT", THREEDI_CTRL_LFP_CAMPPERCENT},
    {"TEX_TEAM", THREEDI_CTRL_TEX_TEAM},
    {"TEX_CAMO1", THREEDI_CTRL_TEX_CAMO1},
    {"TEX_CAMO2", THREEDI_CTRL_TEX_CAMO2},
    {"TEX_CAMO3", THREEDI_CTRL_TEX_CAMO3}
};

bool expect_equal(const char *label, int got, int expected)
{
    if (got == expected) {
        return true;
    }
    std::fprintf(stderr, "%s: got %d, expected %d\n", label, got, expected);
    return false;
}

} // namespace

int main()
{
    bool ok = true;
    uint64_t descriptor_fingerprint = UINT64_C(14695981039346656037);
    const std::size_t expected_count =
        sizeof(kExpectedRegisters) / sizeof(kExpectedRegisters[0]);

    ok &= expect_equal("catalog count",
                       THREEDI_CTRL_REGISTER_COUNT,
                       static_cast<int>(expected_count));

    for (std::size_t ordinal = 0; ordinal < expected_count; ++ordinal) {
        const ExpectedRegister &expected = kExpectedRegisters[ordinal];
        const char *actual = threedi_ctrl_register_name(ordinal);
        if (!actual || std::strcmp(actual, expected.name) != 0) {
            std::fprintf(stderr,
                         "ordinal %zu: got %s, expected %s\n",
                         ordinal,
                         actual ? actual : "<null>",
                         expected.name);
            ok = false;
            continue;
        }
        ok &= expect_equal(expected.name,
                           expected.named_ordinal,
                           static_cast<int>(ordinal));
        ok &= expect_equal(actual,
                           threedi_ctrl_register_ordinal(actual),
                           static_cast<int>(ordinal));
        for (const unsigned char *p =
                     reinterpret_cast<const unsigned char *>(actual);
             *p != 0;
             ++p) {
            descriptor_fingerprint =
                    (descriptor_fingerprint ^ *p) *
                    UINT64_C(1099511628211);
        }
        // Include each descriptor's terminator so concatenation boundaries are
        // pinned independently of the hand-written expected table.
        descriptor_fingerprint =
                descriptor_fingerprint * UINT64_C(1099511628211);
    }

    // Frozen FNV-1a fingerprint of the 96 terminated names read directly from
    // Jointops.exe descriptors 0x83DCE8..0x83E8E7. The next descriptor is zero.
    if (descriptor_fingerprint != UINT64_C(0x96293394734257F7)) {
        std::fprintf(stderr,
                     "retail descriptor fingerprint: got 0x%016llx\n",
                     static_cast<unsigned long long>(descriptor_fingerprint));
        ok = false;
    }

    ok &= threedi_ctrl_register_name(THREEDI_CTRL_REGISTER_COUNT) == NULL;
    ok &= threedi_ctrl_register_name(999) == NULL;
    ok &= expect_equal("LOD_FRAC is valid zero",
                       threedi_ctrl_register_ordinal("lod_frac"),
                       THREEDI_CTRL_LOD_FRAC);
    ok &= expect_equal("mixed-case lookup",
                       threedi_ctrl_register_ordinal("eWeAp_GuNpItCh"),
                       THREEDI_CTRL_EWEAP_GUNPITCH);
    ok &= expect_equal("null lookup",
                       threedi_ctrl_register_ordinal(NULL),
                       THREEDI_CTRL_REGISTER_NOT_FOUND);
    ok &= expect_equal("empty lookup",
                       threedi_ctrl_register_ordinal(""),
                       THREEDI_CTRL_REGISTER_NOT_FOUND);
    ok &= expect_equal("unknown lookup",
                       threedi_ctrl_register_ordinal("NOT_A_RETAIL_REGISTER"),
                       THREEDI_CTRL_REGISTER_NOT_FOUND);
    ok &= expect_equal("loader known",
                       threedi_ctrl_register_loader_ordinal("Vehicle_Special2"),
                       THREEDI_CTRL_VEHICLE_SPECIAL2);
    ok &= expect_equal("loader unknown aliases zero",
                       threedi_ctrl_register_loader_ordinal("NOT_A_RETAIL_REGISTER"),
                       THREEDI_CTRL_LOD_FRAC);
    ok &= expect_equal("loader null aliases zero",
                       threedi_ctrl_register_loader_ordinal(NULL),
                       THREEDI_CTRL_LOD_FRAC);
    ok &= expect_equal("loader empty aliases zero",
                       threedi_ctrl_register_loader_ordinal(""),
                       THREEDI_CTRL_LOD_FRAC);

    return ok ? 0 : 1;
}
