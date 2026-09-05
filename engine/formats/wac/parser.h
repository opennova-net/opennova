// WAC parser: tokens -> AST. Recursive-descent statements + precedence-climbing
// boolean/comparison expressions (faithful to Script_Compile's keyword/operator
// set and "left to right" ordering). Error-tolerant: records diagnostics and
// resyncs rather than aborting, so the shipped corpus (with typos) parses.
#pragma once

#include <string_view>
#include <vector>

#include <formats/wac/ast.h>
#include <formats/wac/program.h>

namespace opennova::wac {

struct ParseResult {
    std::vector<Stmt> statements;
    std::vector<Diagnostic> diagnostics;
};

ParseResult parse(std::string_view source);

} // namespace opennova::wac
