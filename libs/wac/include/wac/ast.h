// WAC abstract syntax tree.
#ifndef OPENNOVA_WAC_AST_H
#define OPENNOVA_WAC_AST_H

#include <string>
#include <utility>
#include <vector>

namespace opennova::wac {

// A function-call argument: a primary operand (variable, literal, symbolic
// reference, or string), with an optional unary minus folded in.
struct Arg {
    std::string text;     // raw operand lexeme (the resolver parses it)
    bool is_string = false;
    bool negate = false;  // unary minus on a numeric literal
};

// A command invocation (condition or action). `name` is looked up in the
// registry; `args` are resolved against the world/var store.
struct Call {
    std::string name;
    std::vector<Arg> args;
    int line = 0;
};

// A boolean / condition expression: leaves are calls; combinators are the WAC
// boolean operators. Comparison operators between bare operands are lowered to
// the corresponding registry call (== -> eq, < -> lt, ...) by the parser.
struct Expr {
    enum Kind { Leaf, Not, And, Or, Xor } kind = Leaf;
    Call call;                 // Leaf
    std::vector<Expr> kids;    // Not (1 child), And/Or/Xor (2+)
};

struct Stmt;

struct ElseIf {
    Expr cond;
    std::vector<Stmt> body;
};

struct Stmt {
    enum Kind { Action, If, Block } kind = Action;

    // Action
    Call call;

    // If
    Expr cond;
    std::vector<Stmt> body;        // THEN (or loop/block body)
    std::vector<ElseIf> elifs;
    std::vector<Stmt> else_body;
    bool has_else = false;

    // Block (loop / decl placeholder — body executed inline in this milestone)
    std::string block_kind;        // "doseq" / "dornd" / "ploop" / "gloop"
};

} // namespace opennova::wac

#endif // OPENNOVA_WAC_AST_H
