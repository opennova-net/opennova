#include "threedi/threedi_ctrl_catalog.h"

#include "io/strutil.h"

#include <cstddef>

namespace {

// Exact descriptor order from the 96 populated retail records.
// [orig: global control-register descriptor table @ 0x83DCE8]
const char *const kCtrlRegisterNames[THREEDI_CTRL_REGISTER_COUNT] = {
    "LOD_FRAC",
    "LOD_FADE_IN",
    "LOD_FADE_OUT",
    "FLICKER",
    "SWING",
    "TALK",
    "DEATH",
    "NVG_FLIP",
    "TEAMSWING",
    "HUD_HEALTH",
    "HUD_MANA",
    "HUD_COMPASS",
    "WPN_TRIGGER",
    "WPN_HAMMER",
    "PARA",
    "PARA_O",
    "DOOR_00",
    "DOOR_01",
    "DOOR_02",
    "DOOR_03",
    "DOOR_04",
    "DOOR_05",
    "DOOR_06",
    "DOOR_07",
    "DOOR_08",
    "DOOR_09",
    "DOOR_10",
    "DOOR_11",
    "DOOR_12",
    "DOOR_13",
    "DOOR_14",
    "DOOR_15",
    "UPL_INTENSITY",
    "LIGHTSWITCH0",
    "LIGHTSWITCH1",
    "LIGHTSWITCH2",
    "LIGHTSWITCH3",
    "PARTICLE_ALPHA",
    "PARTICLE_RGB",
    "HELO_REAR_GEAR",
    "HELO_GEARDOORS",
    "HELO_GEAR",
    "HELO_GEARB",
    "HELO_BAYDOORS",
    "HELO_PCANOPY",
    "HELO_CPCANOPY",
    "HELO_ROTOR",
    "HELO_TAILROTOR",
    "HELO_PILOTYAW",
    "HELO_PILOTPITCH",
    "HELO_CPILOTYAW",
    "HELO_CPILOTPITCH",
    "HELO_GUNYAW",
    "HELO_GUNPITCH",
    "HEAT_GLOW",
    "EWEAP_GUNYAW",
    "EWEAP_GUNPITCH",
    "WEAP_SPIN",
    "TRACER_SCALE",
    "TRACER_WIDTH",
    "VEHICLE_WHEELS",
    "VEHICLE_STEERING",
    "VEHICLE_SPEED",
    "VEHICLE_GUNYAW",
    "VEHICLE_GUNPITCH",
    "OBJECT_DESTROY",
    "OBJECT_DESTROY01",
    "OBJECT_DESTROY02",
    "OBJECT_DESTROY03",
    "OBJECT_DESTROY04",
    "OBJECT_DESTROY05",
    "VEHICLE_SPECIAL1",
    "VEHICLE_SPECIAL2",
    "VEHICLE_TIRE00",
    "VEHICLE_TIRE01",
    "VEHICLE_TIRE02",
    "VEHICLE_TIRE03",
    "VEHICLE_TIRE04",
    "VEHICLE_TIRE05",
    "VEHICLE_TIRE06",
    "VEHICLE_TIRE07",
    "VEHICLE_TIRE08",
    "VEHICLE_TIRE09",
    "VEHICLE_TIRE10",
    "VEHICLE_TIRE11",
    "VEHICLE_TIRE12",
    "VEHICLE_TIRE13",
    "VEHICLE_WHEELS00",
    "VEHICLE_WHEELS01",
    "VEHICLE_WHEELS02",
    "VEHICLE_WHEELS03",
    "LFP_CAMPPERCENT",
    "TEX_TEAM",
    "TEX_CAMO1",
    "TEX_CAMO2",
    "TEX_CAMO3"
};

static_assert(
    sizeof(kCtrlRegisterNames) / sizeof(kCtrlRegisterNames[0]) ==
        THREEDI_CTRL_REGISTER_COUNT,
    "retail CTRL catalog must contain every populated descriptor");

} // namespace

const char *threedi_ctrl_register_name(size_t ordinal)
{
    if (ordinal >= THREEDI_CTRL_REGISTER_COUNT) {
        return NULL;
    }
    return kCtrlRegisterNames[ordinal];
}

int threedi_ctrl_register_ordinal(const char *name)
{
    if (!name || !name[0]) {
        return THREEDI_CTRL_REGISTER_NOT_FOUND;
    }

    // Retail scans from ordinal zero and returns the first case-insensitive
    // match.  Unlike the original return convention, this interface keeps
    // LOD_FRAC (zero) distinguishable from an unknown name.
    // [orig: CtrlName_ToOrdinal @ 0x57B290]
    for (size_t ordinal = 0; ordinal < THREEDI_CTRL_REGISTER_COUNT; ++ordinal) {
        if (opennova::strutil::iequals(name, kCtrlRegisterNames[ordinal])) {
            return static_cast<int>(ordinal);
        }
    }
    return THREEDI_CTRL_REGISTER_NOT_FOUND;
}

uint8_t threedi_ctrl_register_loader_ordinal(const char *name)
{
    const int ordinal = threedi_ctrl_register_ordinal(name);

    // The model loader stores CtrlName_ToOrdinal's zero result unchanged, so
    // an unknown authored CTRL name aliases LOD_FRAC just like a real ordinal
    // zero.  Keep this compatibility behavior out of ordinary lookups.
    // [orig: sub_5B4640 @ 0x5B4640; CtrlName_ToOrdinal @ 0x57B290]
    return ordinal == THREEDI_CTRL_REGISTER_NOT_FOUND
        ? static_cast<uint8_t>(THREEDI_CTRL_LOD_FRAC)
        : static_cast<uint8_t>(ordinal);
}
