#include "wac/command.h"

#include <cctype>

#include <io/strutil.h>

namespace opennova::wac {

namespace {

using opennova::strutil::iequals;

const char *const kParamTypeNames[kParamTypeCount] = {
    "null", "value", "number", "red", "green", "blue", "distance", "heading",
    "seconds", "hour", "meters", "ssn", "group", "team", "area", "target",
    "wplist", "text", "filename", "soundset", "texttoken", "face", "fx", "ammo",
    "anim", "cheat", "ifname", "variable",
};

} // namespace

const char *param_type_name(ParamType t) {
    int i = static_cast<int>(t);
    if (i < 0 || i >= kParamTypeCount) return "?";
    return kParamTypeNames[i];
}

const CommandDef *wac_find_command(std::string_view name) {
    const CommandDef *cmds = wac_commands();
    int n = wac_command_count();
    for (int i = 0; i < n; ++i) {
        if (iequals(cmds[i].name, name)) return &cmds[i];
    }
    return nullptr;
}

int wac_command_index(std::string_view name) {
    const CommandDef *cmds = wac_commands();
    int n = wac_command_count();
    for (int i = 0; i < n; ++i) {
        if (iequals(cmds[i].name, name)) return i;
    }
    return -1;
}

} // namespace opennova::wac
