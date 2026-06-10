// WAC compiler: AST -> faithful bytecode Program.
//
// Emits the original 4-byte instruction encoding (see bytecode.h): EnterEvent /
// AndChain / Jump control flow, accumulator folds for boolean/comparison
// expressions, and command calls with inline operand references. Operands are
// resolved here (V#/G#/M#, builtins, prefixes, literals -> pool refs), the
// rebased analogue of WacScript_ResolveParameter @0x4f2920.
#ifndef OPENNOVA_WAC_COMPILER_H
#define OPENNOVA_WAC_COMPILER_H

#include <string_view>
#include <vector>

#include "wac/ast.h"
#include "wac/program.h"

namespace opennova::world {
class EntityRegistry;
}

namespace opennova::wac {

struct CompileEnv {
    // Optional: lets symbolic group/area names resolve to interned ids. When null,
    // groups/areas must be numeric (sufficient for unit tests).
    opennova::world::EntityRegistry *registry = nullptr;
};

// Compile a parsed statement list into a Program. Diagnostics from both parse and
// compile phases accumulate on the Program.
Program compile(const std::vector<Stmt> &statements, const CompileEnv &env);

// Convenience: lex + parse + compile a single source string.
Program compile_source(std::string_view source, const CompileEnv &env);

// Multi-file program: game.wac -> server.wac -> <mission>.wac concatenated into
// one program (events numbered across all sources). [orig: WacScript_InitAndLoad
// compiles the three files into a single bytecode buffer.] V# are cleared at
// mission start by the caller; G# persist.
Program compile_program(const std::vector<std::string> &sources, const CompileEnv &env);

} // namespace opennova::wac

#endif // OPENNOVA_WAC_COMPILER_H
