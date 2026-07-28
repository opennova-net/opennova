// WAC operand parameter types.
//
// The 28 types (ids 0-27) extracted from the binary's type table (Jointops.exe
// @0x82D064, the typeName base used by WacScript_DumpActionDefsToXML) and
// confirmed against WacScript_ResolveParameter @0x4f2920. Numeric literals are
// scaled to 16.16 fixed-point per the declared slot type (distance/meters/
// seconds/hour/heading). RAW types (Text/Filename/Variable) are passed
// by-reference (pointer), which drives the command call_conv derivation.
#ifndef OPENNOVA_WAC_PARAM_TYPE_H
#define OPENNOVA_WAC_PARAM_TYPE_H

#include <cstdint>

namespace opennova::wac {

enum class ParamType : uint8_t {
    Null = 0,
    Value = 1,
    Number = 2,
    Red = 3,
    Green = 4,
    Blue = 5,
    Distance = 6,
    Heading = 7,
    Seconds = 8,
    Hour = 9,
    Meters = 10,
    Ssn = 11,
    Group = 12,
    Team = 13,
    Area = 14,
    Target = 15,
    WpList = 16,
    Text = 17,
    Filename = 18,
    SoundSet = 19,
    TextToken = 20,
    Face = 21,
    Fx = 22,
    Ammo = 23,
    Anim = 24,
    Cheat = 25,
    IfName = 26,
    Variable = 27,
};

constexpr int kParamTypeCount = 28;


const char *param_type_name(ParamType t);

} // namespace opennova::wac

#endif // OPENNOVA_WAC_PARAM_TYPE_H
