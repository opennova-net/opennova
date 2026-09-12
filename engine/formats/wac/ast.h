// WAC abstract syntax tree.
#pragma once

#include <string>
#include <cstdint>
#include <utility>
#include <vector>

namespace opennova::wac {

// A function-call argument: a primary operand (variable, literal, symbolic
// reference, or string). A signed literal keeps its minus in the lexeme.
struct Arg {
    std::string text;     // raw operand lexeme (the resolver parses it)
    bool is_string = false;
};

// A command invocation (condition or action). `name` is looked up in the
// registry; `args` are resolved against the world/var store.
struct Call {
    std::string name;
    std::vector<Arg> args;
    int line = 0;
    uint32_t source_index = 0;
};

// Expression calls retain the retail accumulator operations and stack edges.
// Auto parentheses remain open until a closing parenthesis or statement boundary;
// they are not a conventional precedence tree.
struct Expr {
    enum Kind { Leaf, Sequence, Pop, Store } kind = Leaf;
    std::string op;            // accumulator fold, empty for a plain call
    bool negate = false;      // NOT on a call result or saved stack byte
    bool push = false;        // save the accumulator byte before this call
    Call call;                // Leaf
    std::vector<Expr> kids;    // Sequence
};

struct Stmt;

// [orig: Script_Compile @0x4F31F0: THEN=2, ENTER=11, LEAVE=12]
enum class IfMode { Then, Enter, Leave };

struct ElseIf {
    std::string event_name;
    Expr cond;
    IfMode mode = IfMode::Then;
    std::vector<Stmt> body;
};

struct Stmt {
    enum Kind { Action, If, Block, Declaration, Run, Assignment, Expression } kind = Action;
    Arg assignment_target;

    // Action
    Call call;

    // Declarations and RUN retain their authored name in call.name.
    bool array_declaration = false;

    // If
    std::string event_name;
    Expr cond;
    IfMode mode = IfMode::Then;
    std::vector<Stmt> body;        // THEN (or loop/block body)
    std::vector<ElseIf> elifs;
    std::vector<Stmt> else_body;
    bool has_else = false;

    // Block: body is the first DO alternative; NEXT introduces another.
    std::string block_kind;
    Arg block_argument; // GLOOP's named group
    std::vector<std::vector<Stmt>> next_bodies;
};

} // namespace opennova::wac
