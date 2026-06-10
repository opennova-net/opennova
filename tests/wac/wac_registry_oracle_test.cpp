// WAC registry oracle: asserts our command table matches the binary-extracted
// ISA (165 commands + 28 param types). Ground truth: the schema dumped from
// Jointops.exe's command table @0x82D290 (notes/wac/wac_commands.schema.json).
#include <cstdio>
#include <cstring>

#include "wac/command.h"
#include "wac/param_type.h"

using namespace opennova::wac;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static const CommandDef *byname(const char *n) { return wac_find_command(n); }

int main() {
    // Count is exact (165 from the binary).
    CHECK(wac_command_count() == 165);

    // Param-type table (28 ids) matches WacScript_ResolveParameter ordering.
    CHECK(kParamTypeCount == 28);
    CHECK(std::strcmp(param_type_name(ParamType::Null), "null") == 0);
    CHECK(std::strcmp(param_type_name(ParamType::Ssn), "ssn") == 0);
    CHECK(std::strcmp(param_type_name(ParamType::Group), "group") == 0);
    CHECK(std::strcmp(param_type_name(ParamType::Variable), "variable") == 0);
    CHECK(std::strcmp(param_type_name(ParamType::Seconds), "seconds") == 0);

    // Index == bytecode command id: entry 0 is `elapse`, entry 1 is `never`.
    CHECK(std::strcmp(wac_commands()[0].name, "elapse") == 0);
    CHECK(std::strcmp(wac_commands()[1].name, "never") == 0);

    // Representative rows (name, kind, argc, param types, replication flags).
    const CommandDef *elapse = byname("elapse");
    CHECK(elapse && cmd_is_condition(*elapse) && elapse->argc == 1 &&
          elapse->params[0] == ParamType::Seconds);

    const CommandDef *never = byname("never");
    CHECK(never && cmd_is_condition(*never) && never->argc == 0);

    const CommandDef *set = byname("set");
    CHECK(set && !cmd_is_condition(*set) && set->argc == 2 &&
          set->params[0] == ParamType::Variable && set->params[1] == ParamType::Value);
    // derived call_conv: arg0 is a RAW (variable) lvalue -> conv 6.
    CHECK(set && set->call_conv == 6);

    const CommandDef *kill_ssn = byname("killSSN");
    CHECK(kill_ssn && kill_ssn->argc == 1 && kill_ssn->params[0] == ParamType::Ssn);

    const CommandDef *eq = byname("eq");
    CHECK(eq && eq->argc == 2);

    const CommandDef *fx2tgt = byname("fx2tgt");
    CHECK(fx2tgt && cmd_is_replicated(*fx2tgt)); // flags 0x0a -> &0x18 set

    const CommandDef *ssntowp = byname("SSNtoWP");
    CHECK(ssntowp && ssntowp->argc == 2 && ssntowp->params[0] == ParamType::Ssn &&
          ssntowp->params[1] == ParamType::WpList);

    // Case-insensitive lookup (corpus uses varied case).
    CHECK(wac_command_index("SSNDEAD") == wac_command_index("ssndead"));
    CHECK(wac_command_index("ssndead") >= 0);

    // Spot-check the richer retail set is present.
    CHECK(byname("ammo2ssn") != nullptr);
    CHECK(byname("pisteam") != nullptr);
    CHECK(byname("SSNseesSSN") != nullptr);

    std::printf(failures ? "ORACLE TESTS FAILED (%d)\n" : "registry oracle passed\n", failures);
    return failures ? 1 : 0;
}
