// WAC parser: tokens -> AST. Recursive-descent statements + precedence-climbing
// boolean/comparison expressions (faithful to Script_Compile's keyword/operator
// set and "left to right" ordering). Error-tolerant: records diagnostics and
// resyncs rather than aborting, so the shipped corpus (with typos) parses.
#ifndef OPENNOVA_WAC_PARSER_H
#define OPENNOVA_WAC_PARSER_H

#include <string_view>
#include <vector>

#include "wac/ast.h"
#include "wac/program.h"

namespace opennova::wac {

struct ParseResult {
    std::vector<Stmt> statements;
    std::vector<Diagnostic> diagnostics;
};

ParseResult parse(std::string_view source);

} // namespace opennova::wac

#endif // OPENNOVA_WAC_PARSER_H
