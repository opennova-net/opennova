// WAC compiler: AST -> faithful bytecode Program.
//
// Emits the original 4-byte instruction encoding (see bytecode.h): EnterEvent /
// AndChain / Jump control flow, accumulator folds for boolean/comparison
// expressions, and command calls with inline operand references. Operands are
// resolved here (V#/G#/M#, builtins, prefixes, literals -> pool refs), the
// rebased analogue of WacScript_ResolveParameter @0x4f2920.
#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include <formats/wac/ast.h>
#include <formats/wac/program.h>

namespace opennova::world {
class EntityRegistry;
struct AmmoTable;
}

namespace opennova::particle { class EffectScene; }
namespace opennova::audio { class SoundSetIndex; }

namespace opennova::wac {

struct CompileEnv {
    // Optional: lets symbolic group/area names resolve to interned ids. When null,
    // groups/areas must be numeric (sufficient for unit tests).
    opennova::world::EntityRegistry *registry = nullptr;
    const opennova::audio::SoundSetIndex *sounds = nullptr;
    opennova::particle::EffectScene *effects = nullptr; // mounted catalog, compile-time handles
    const opennova::world::AmmoTable *ammo = nullptr; // literal AMMO bindings at compilation
    std::vector<std::string> source_names; // parallel to compile_program's source texts
    std::function<bool(const std::string &, std::string &)> load_source; // RUN's mounted-file reader
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
